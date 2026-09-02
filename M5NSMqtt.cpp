#include "M5NSMqtt.h"
#include "externs.h"
#include <WiFi.h>
#include <PubSubClient.h>

static WiFiClient espMqttClient;
static PubSubClient mqttClient(espMqttClient);
static unsigned long lastMqttReconnect = 0;
static bool discoveryPublished = false;

static String getDeviceId() {
  if (strlen(cfg.deviceName) > 0) {
    String dev = String(cfg.deviceName);
    dev.trim();
    dev.replace(' ', '_');
    dev.replace('/', '_');
    return dev;
  }
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[16];
  sprintf(buf, "m5ns_%02x%02x", mac[4], mac[5]);
  return String(buf);
}

static String getBaseTopic() {
  String pfx = String(cfg.mqtt_topic_prefix);
  if (pfx.length() == 0) pfx = "m5ns";
  return pfx + "/" + getDeviceId();
}

static void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[128];
  unsigned int len = (length < sizeof(msg) - 1) ? length : (sizeof(msg) - 1);
  memcpy(msg, payload, len);
  msg[len] = '\0';
  String strMsg = String(msg);
  strMsg.trim();

  Serial.printf("[MQTT] Message arrived on [%s]: %s\r\n", topic, strMsg.c_str());
  String base = getBaseTopic();

  if (String(topic) == base + "/screen/set") {
    if (strMsg.equalsIgnoreCase("ON") || strMsg == "1" || strMsg.equalsIgnoreCase("TRUE")) {
      setScreenPower(true);
    } else if (strMsg.equalsIgnoreCase("OFF") || strMsg == "0" || strMsg.equalsIgnoreCase("FALSE")) {
      setScreenPower(false);
    } else if (strMsg.equalsIgnoreCase("TOGGLE")) {
      toggleScreenPower();
    }
    mqttPublishState();
  }
  else if (String(topic) == base + "/brightness/set") {
    int val = strMsg.toInt();
    if (val >= 0 && val <= 100) {
      setBrightness((uint8_t)val);
      mqttPublishState();
    }
  }
  else if (String(topic) == base + "/page/set") {
    if (strMsg.equalsIgnoreCase("NEXT")) {
      cyclePage();
    } else {
      int p = strMsg.toInt();
      if (p >= 0 && p <= maxPage) {
        setPage(p);
      }
    }
    mqttPublishState();
  }
  else if (String(topic) == base + "/snooze/set") {
    if (strMsg.equalsIgnoreCase("OFF") || strMsg == "0") {
      setSnooze(0);
    } else if (strMsg.equalsIgnoreCase("PRESS") || strMsg.equalsIgnoreCase("CYCLE") || strMsg.equalsIgnoreCase("TOGGLE")) {
      triggerSnooze();
    } else {
      int mins = strMsg.toInt();
      if (mins > 0) {
        setSnooze(mins);
      } else {
        triggerSnooze();
      }
    }
    mqttPublishState();
  }
  else if (String(topic) == base + "/refresh/set") {
    int val = strMsg.toInt();
    if (val > 0) {
      refreshIntervalSec = (val <= 60) ? (val * 60) : val;
      resetNextRead();
      mqttPublishState();
    }
  }
}

void mqttInit() {
  if (!cfg.mqtt_enabled || strlen(cfg.mqtt_server) == 0) {
    return;
  }
  int port = (cfg.mqtt_port > 0) ? cfg.mqtt_port : 1883;
  mqttClient.setServer(cfg.mqtt_server, port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1536);
  discoveryPublished = false;
  Serial.printf("[MQTT] Initialized target: %s:%d\r\n", cfg.mqtt_server, port);
}

bool isMqttConnected() {
  return cfg.mqtt_enabled && mqttClient.connected();
}

void mqttPublishState() {
  if (!cfg.mqtt_enabled || !mqttClient.connected()) {
    return;
  }

  String base = getBaseTopic();
  String stateTopic = base + "/state";

  DynamicJsonDocument doc(1024);
  doc["sgv"] = cfg.show_mgdl ? (int)round(ns.sensSgvMgDl) : (float)ns.sensSgv;
  doc["sgv_display"] = cfg.show_mgdl ? String((int)round(ns.sensSgvMgDl)) : String(ns.sensSgv, 1);
  doc["delta"] = cfg.show_mgdl ? ns.delta_mgdl : ns.delta_scaled;
  doc["delta_display"] = String(ns.delta_display);
  doc["direction"] = String(ns.sensDir);
  doc["iob"] = ns.iob;
  doc["cob"] = ns.cob;
  doc["basal"] = ns.basal_current;
  doc["screen"] = screenOn ? "ON" : "OFF";
  doc["brightness"] = screenOn ? lcdBrightness : savedBrightness;
  doc["page"] = dispPage;

  struct tm timeinfo;
  int snoozeRemaining = 0;
  bool timeOK = getLocalTime(&timeinfo);
  if (timeOK && snoozeUntil > 0) {
    snoozeRemaining = difftime(snoozeUntil, mktime(&timeinfo));
    if (snoozeRemaining < 0) snoozeRemaining = 0;
  }
  doc["snooze_active"] = (snoozeRemaining > 0);
  doc["snooze_remaining"] = (snoozeRemaining + 59) / 60;
  doc["snooze_multiplier"] = snoozeMult;
  doc["refresh_interval"] = refreshIntervalSec;

  String pwrSource;
  bool isCharging = false;
  int batPct = 0;
  float batV = 0.0f;
  getPowerTelemetry(pwrSource, isCharging, batPct, batV);

  doc["battery_percentage"] = batPct;
  doc["battery_voltage"] = serialized(String(batV, 2));
  doc["power_source"] = pwrSource;
  doc["is_charging"] = isCharging;
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["free_heap"] = ESP.getFreeHeap();

  String payload;
  serializeJson(doc, payload);
  mqttClient.publish(stateTopic.c_str(), payload.c_str(), false);

  // Also publish individual values for simple non-JSON MQTT consumers
  mqttClient.publish((base + "/sgv").c_str(), doc["sgv_display"].as<const char*>(), false);
  mqttClient.publish((base + "/screen").c_str(), screenOn ? "ON" : "OFF", false);
  mqttClient.publish((base + "/brightness").c_str(), String(doc["brightness"].as<int>()).c_str(), false);
  mqttClient.publish((base + "/page").c_str(), String(dispPage).c_str(), false);
  mqttClient.publish((base + "/battery").c_str(), String(batPct).c_str(), false);
}

static void publishHADiscoveryEntity(const String &component, const String &objectId, const String &configJson) {
  String devId = getDeviceId();
  String discTopic = "homeassistant/" + component + "/" + devId + "/" + objectId + "/config";
  mqttClient.publish(discTopic.c_str(), configJson.c_str(), true);
}

void mqttPublishDiscovery() {
  if (!cfg.mqtt_enabled || !cfg.mqtt_ha_discovery || !mqttClient.connected()) {
    return;
  }

  String devId = getDeviceId();
  String base = getBaseTopic();
  String statusTopic = base + "/status";
  String stateTopic = base + "/state";

  String devObj = ",\"dev\":{"
                  "\"ids\":[\"" + devId + "\"],"
                  "\"name\":\"M5 Nightscout " + devId + "\","
                  "\"mdl\":\"M5Stack Core2\","
                  "\"mf\":\"M5Stack\","
                  "\"sw\":\"" + M5NSversion + "\"}";

  // 1. Glucose SGV
  String uom = cfg.show_mgdl ? "mg/dL" : "mmol/L";
  String sgvCfg = "{\"name\":\"Glucose\","
                  "\"stat_t\":\"" + stateTopic + "\","
                  "\"val_tpl\":\"{{ value_json.sgv }}\","
                  "\"unit_of_meas\":\"" + uom + "\","
                  "\"icon\":\"mdi:water-percent\","
                  "\"uniq_id\":\"" + devId + "_sgv\","
                  "\"avty_t\":\"" + statusTopic + "\"" + devObj + "}";
  publishHADiscoveryEntity("sensor", "sgv", sgvCfg);

  // 2. Delta
  String deltaCfg = "{\"name\":\"Delta\","
                    "\"stat_t\":\"" + stateTopic + "\","
                    "\"val_tpl\":\"{{ value_json.delta }}\","
                    "\"unit_of_meas\":\"" + uom + "\","
                    "\"icon\":\"mdi:chart-line\","
                    "\"uniq_id\":\"" + devId + "_delta\","
                    "\"avty_t\":\"" + statusTopic + "\"" + devObj + "}";
  publishHADiscoveryEntity("sensor", "delta", deltaCfg);

  // 3. Direction
  String dirCfg = "{\"name\":\"Direction\","
                  "\"stat_t\":\"" + stateTopic + "\","
                  "\"val_tpl\":\"{{ value_json.direction }}\","
                  "\"icon\":\"mdi:arrow-decision-auto\","
                  "\"uniq_id\":\"" + devId + "_direction\","
                  "\"avty_t\":\"" + statusTopic + "\"" + devObj + "}";
  publishHADiscoveryEntity("sensor", "direction", dirCfg);

  // 4. Battery Sensor
  String batCfg = "{\"name\":\"Battery\","
                  "\"dev_cla\":\"battery\","
                  "\"stat_t\":\"" + stateTopic + "\","
                  "\"val_tpl\":\"{{ value_json.battery_percentage }}\","
                  "\"unit_of_meas\":\"%\","
                  "\"uniq_id\":\"" + devId + "_battery\","
                  "\"avty_t\":\"" + statusTopic + "\"" + devObj + "}";
  publishHADiscoveryEntity("sensor", "battery", batCfg);

  // 5. Battery Voltage
  String voltCfg = "{\"name\":\"Battery Voltage\","
                   "\"dev_cla\":\"voltage\","
                   "\"stat_t\":\"" + stateTopic + "\","
                   "\"val_tpl\":\"{{ value_json.battery_voltage }}\","
                   "\"unit_of_meas\":\"V\","
                   "\"uniq_id\":\"" + devId + "_voltage\","
                   "\"avty_t\":\"" + statusTopic + "\"" + devObj + "}";
  publishHADiscoveryEntity("sensor", "voltage", voltCfg);

  // 6. Power Source
  String pwrCfg = "{\"name\":\"Power Source\","
                  "\"stat_t\":\"" + stateTopic + "\","
                  "\"val_tpl\":\"{{ value_json.power_source }}\","
                  "\"icon\":\"mdi:power-plug\","
                  "\"uniq_id\":\"" + devId + "_power_source\","
                  "\"avty_t\":\"" + statusTopic + "\"" + devObj + "}";
  publishHADiscoveryEntity("sensor", "power_source", pwrCfg);

  // 7. Battery Charging Binary Sensor
  String chgCfg = "{\"name\":\"Battery Charging\","
                  "\"dev_cla\":\"battery_charging\","
                  "\"stat_t\":\"" + stateTopic + "\","
                  "\"val_tpl\":\"{{ 'ON' if value_json.is_charging else 'OFF' }}\","
                  "\"uniq_id\":\"" + devId + "_charging\","
                  "\"avty_t\":\"" + statusTopic + "\"" + devObj + "}";
  publishHADiscoveryEntity("binary_sensor", "charging", chgCfg);

  // 8. Snooze Active Binary Sensor
  String snzCfg = "{\"name\":\"Snooze Active\","
                  "\"stat_t\":\"" + stateTopic + "\","
                  "\"val_tpl\":\"{{ 'ON' if value_json.snooze_active else 'OFF' }}\","
                  "\"icon\":\"mdi:alarm-snooze\","
                  "\"uniq_id\":\"" + devId + "_snooze_active\","
                  "\"avty_t\":\"" + statusTopic + "\"" + devObj + "}";
  publishHADiscoveryEntity("binary_sensor", "snooze_active", snzCfg);

  // 9. Screen Switch
  String scrCfg = "{\"name\":\"Screen Power\","
                  "\"stat_t\":\"" + stateTopic + "\","
                  "\"val_tpl\":\"{{ value_json.screen }}\","
                  "\"cmd_t\":\"" + base + "/screen/set\","
                  "\"pl_on\":\"ON\","
                  "\"pl_off\":\"OFF\","
                  "\"icon\":\"mdi:monitor\","
                  "\"uniq_id\":\"" + devId + "_screen\","
                  "\"avty_t\":\"" + statusTopic + "\"" + devObj + "}";
  publishHADiscoveryEntity("switch", "screen", scrCfg);

  // 10. Brightness Number
  String brCfg = "{\"name\":\"Screen Brightness\","
                 "\"stat_t\":\"" + stateTopic + "\","
                 "\"val_tpl\":\"{{ value_json.brightness }}\","
                 "\"cmd_t\":\"" + base + "/brightness/set\","
                 "\"min\":0,\"max\":100,\"step\":1,"
                 "\"icon\":\"mdi:brightness-6\","
                 "\"uniq_id\":\"" + devId + "_brightness\","
                 "\"avty_t\":\"" + statusTopic + "\"" + devObj + "}";
  publishHADiscoveryEntity("number", "brightness", brCfg);

  // 11. Snooze Button
  String btnCfg = "{\"name\":\"Snooze Alarm\","
                  "\"cmd_t\":\"" + base + "/snooze/set\","
                  "\"pl_prs\":\"PRESS\","
                  "\"icon\":\"mdi:alarm-snooze\","
                  "\"uniq_id\":\"" + devId + "_btn_snooze\","
                  "\"avty_t\":\"" + statusTopic + "\"" + devObj + "}";
  publishHADiscoveryEntity("button", "snooze", btnCfg);

  // 12. Snooze Remaining Sensor
  String snzRemCfg = "{\"name\":\"Snooze Remaining\","
                     "\"stat_t\":\"" + stateTopic + "\","
                     "\"unit_of_meas\":\"min\","
                     "\"val_tpl\":\"{{ value_json.snooze_remaining }}\","
                     "\"icon\":\"mdi:timer-sand\","
                     "\"uniq_id\":\"" + devId + "_snooze_remaining\","
                     "\"avty_t\":\"" + statusTopic + "\"" + devObj + "}";
  publishHADiscoveryEntity("sensor", "snooze_remaining", snzRemCfg);

  // 13. Snooze Duration Number Control
  String snzNumCfg = "{\"name\":\"Snooze Duration\","
                     "\"stat_t\":\"" + stateTopic + "\","
                     "\"val_tpl\":\"{{ value_json.snooze_remaining }}\","
                     "\"cmd_t\":\"" + base + "/snooze/set\","
                     "\"min\":0,\"max\":240,\"step\":15,"
                     "\"unit_of_meas\":\"min\","
                     "\"icon\":\"mdi:alarm-snooze\","
                     "\"uniq_id\":\"" + devId + "_snooze_duration\","
                     "\"avty_t\":\"" + statusTopic + "\"" + devObj + "}";
  publishHADiscoveryEntity("number", "snooze_duration", snzNumCfg);

  discoveryPublished = true;
  Serial.println("[MQTT] Home Assistant Discovery published.");
}

void mqttLoop() {
  if (!cfg.mqtt_enabled || strlen(cfg.mqtt_server) == 0) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMqttReconnect > 15000 || lastMqttReconnect == 0) {
      lastMqttReconnect = now;
      String devId = getDeviceId();
      String base = getBaseTopic();
      String statusTopic = base + "/status";

      Serial.printf("[MQTT] Connecting to %s:%d as %s...\r\n", cfg.mqtt_server, (cfg.mqtt_port > 0 ? cfg.mqtt_port : 1883), devId.c_str());

      bool connected = false;
      if (strlen(cfg.mqtt_user) > 0) {
        connected = mqttClient.connect(devId.c_str(), cfg.mqtt_user, cfg.mqtt_pass, statusTopic.c_str(), 1, true, "offline");
      } else {
        connected = mqttClient.connect(devId.c_str(), statusTopic.c_str(), 1, true, "offline");
      }

      if (connected) {
        Serial.println("[MQTT] Connected successfully!");
        mqttClient.publish(statusTopic.c_str(), "online", true);

        // Subscribe to command topics
        mqttClient.subscribe((base + "/screen/set").c_str());
        mqttClient.subscribe((base + "/brightness/set").c_str());
        mqttClient.subscribe((base + "/page/set").c_str());
        mqttClient.subscribe((base + "/snooze/set").c_str());
        mqttClient.subscribe((base + "/refresh/set").c_str());

        if (cfg.mqtt_ha_discovery) {
          mqttPublishDiscovery();
        }
        mqttPublishState();
      } else {
        Serial.printf("[MQTT] Connection failed, state: %d\r\n", mqttClient.state());
      }
    }
  } else {
    mqttClient.loop();
  }
}
