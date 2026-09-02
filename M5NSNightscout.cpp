/*  M5Stack Nightscout monitor - Nightscout REST API data source
    Fetches entries (SGV readings) and properties (IOB, COB, delta, loop, basal)
    from a Nightscout backend.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>
#include "M5NSNightscout.h"
#include "M5NSDexcom.h" // directionToArrowAngle
#include "externs.h"

extern const unsigned char wifi1_icon16x16[];
extern const unsigned char wifi2_icon16x16[];
extern int icon_xpos[3];
extern int icon_ypos[3];
extern int rcnt;
#include "RootCA.h"

// Cleans raw JSON string of control characters and problematic Unicode sequences
static void sanitizeJsonString(String &str) {
  for(int i = 0; i < str.length(); i++) {
    if(str.charAt(i) < 32) {
      str.setCharAt(i, 32);
    }
  }
  str.replace("\\u0000", " ");
  str.replace("\\u000b", " ");
  str.replace("\\u0032", " ");
}

// Normalizes and builds base Nightscout URL
static void buildBaseUrl(char *dest, size_t destSize, const char *url) {
  if(strncmp(url, "http", 4) != 0) {
    snprintf(dest, destSize, "https://%s", url);
  } else {
    snprintf(dest, destSize, "%s", url);
  }
  size_t len = strlen(dest);
  if(len > 0 && dest[len - 1] == '/') {
    dest[len - 1] = '\0';
  }
}

int readNightscout(tConfig *cfg, struct NSinfo *ns) {
  HTTPClient http;
  WiFiClientSecure sclient;
  sclient.setCACert(rootCACertificate);

  char NSurl[256];
  int err = 0;
  char tmpstr[64];
  bool is_https_Heroku = false;

  if(WiFiMultiple.run() != WL_CONNECTED) {
    // WiFi not connected
    ESP.restart();
    return -1;
  }

  // --- 1. Query Entries ---
  buildBaseUrl(NSurl, sizeof(NSurl), cfg->url);
  is_https_Heroku = (strstr(NSurl, "https://") != NULL) && (strstr(NSurl, "herokuapp.com") != NULL);
  Serial.print("is_https_Heroku "); Serial.println(is_https_Heroku);

  if(cfg->sgv_only) {
    strlcat(NSurl, "/api/v1/entries.json?find[type][$eq]=sgv&count=10", sizeof(NSurl));
  } else {
    strlcat(NSurl, "/api/v1/entries.json?count=10", sizeof(NSurl));
  }
  if(cfg->token[0] != '\0') {
    strlcat(NSurl, "&token=", sizeof(NSurl));
    strlcat(NSurl, cfg->token, sizeof(NSurl));
  }

  M5.Lcd.fillRect(icon_xpos[0], icon_ypos[0], 16, 16, BLACK);
  drawIcon(icon_xpos[0], icon_ypos[0], (uint8_t*)wifi2_icon16x16, TFT_BLUE);

  Serial.print("JSON query NSurl = '"); Serial.print(NSurl); Serial.println("'");
  if(is_https_Heroku) {
    if(http.begin(sclient, NSurl)) {
      Serial.println("http.begin HTTPS Heroku OK");
    } else {
      Serial.println("http.begin HTTPS Heroku FAILED");
    }
  } else {
    if(http.begin(NSurl)) {
      Serial.println("http.begin OK");
    } else {
      Serial.println("http.begin FAILED");
    }
  }

  const char * headerKeys[] = {"date", "server", "location"};
  http.collectHeaders(headerKeys, 3);

  unsigned long timeInGET = millis();
  int httpCode = http.GET();
  timeInGET = millis() - timeInGET;

  if(httpCode > 0) {
    Serial.printf("[HTTP] GET... code: %d after %lu ms\r\n", httpCode, timeInGET);
    if(httpCode == HTTP_CODE_OK) {
      String json = http.getString();
      Serial.printf("GET() response JSON length = %d\r\n", json.length());
      sanitizeJsonString(json);

      int ndx = 0;
      int sr = json.indexOf("\"date\":", ndx);
      while(sr != -1) {
        ndx = sr + 1;
        if(sr + 20 < json.length()) {
          if(json.charAt(sr + 20) == '.') {
            json.remove(sr + 20, 1);
            while(sr + 20 < json.length() && json.charAt(sr + 20) >= '0' && json.charAt(sr + 20) <= '9') {
              json.remove(sr + 20, 1);
            }
          }
        }
        sr = json.indexOf("\"date\":", ndx);
      }

      Serial.print("Free Heap = "); Serial.println(ESP.getFreeHeap());
      DeserializationError JSONerr = deserializeJson(JSONdoc, json);
      if(JSONerr) {
        Serial.printf("JSON DeserializationError %s\r\n", JSONerr.c_str());
      } else {
        Serial.println("JSON deserialized OK");
      }
      JsonArray arr = JSONdoc.as<JsonArray>();
      Serial.print("JSON array size = "); Serial.println(arr.size());
      if(JSONerr || arr.size() == 0) {
        err = JSONerr ? 1001 : 1002;
        addErrorLog(err);
      } else {
        JsonObject obj;
        int sgvindex = 0;
        do {
          obj = JSONdoc[sgvindex].as<JsonObject>();
          sgvindex++;
        } while ((!obj.containsKey("sgv")) && (sgvindex < (arr.size() - 1)));
        sgvindex--;
        if(sgvindex < 0 || sgvindex > (arr.size() - 1))
          sgvindex = 0;

        strlcpy(ns->sensDev, JSONdoc[sgvindex]["device"] | "N/A", 64);
        ns->is_xDrip = obj.containsKey("xDrip_raw");
        ns->rawtime = JSONdoc[sgvindex]["date"].as<long long>();
        ns->sensTime = ns->rawtime / 1000;
        strlcpy(ns->sensDir, JSONdoc[sgvindex]["direction"] | "N/A", 32);
        if(strcmp(ns->sensDir, "N/A") == 0) {
          strlcpy(ns->sensDir, JSONdoc[sgvindex]["trend"] | "N/A", 32);
        }
        ns->sensSgv = JSONdoc[sgvindex]["sgv"];
        for(int i = 0; i <= 9; i++) {
          ns->last10sgv[i] = JSONdoc[i]["sgv"];
          ns->last10sgv[i] /= 18.0;
        }
        ns->sensSgvMgDl = ns->sensSgv;
        ns->sensSgv /= 18.0;

        localtime_r(&ns->sensTime, &ns->sensTm);
        ns->arrowAngle = directionToArrowAngle(ns->sensDir);

        Serial.print("sensDev = "); Serial.println(ns->sensDev);
        Serial.print("sensTime = "); Serial.print(ns->sensTime);
        sprintf(tmpstr, " (JSON %lld)", (long long) ns->rawtime);
        Serial.print(tmpstr);
        sprintf(tmpstr, " = %s\r", ctime(&ns->sensTime));
        Serial.print(tmpstr);
        Serial.print("sensSgv = "); Serial.println(ns->sensSgv);
        Serial.print("sensDir = "); Serial.println(ns->sensDir);
        Serial.print("Sensor time: "); Serial.print(ns->sensTm.tm_hour);
        Serial.print(":"); Serial.print(ns->sensTm.tm_min);
        Serial.print(":"); Serial.print(ns->sensTm.tm_sec);
        Serial.print(" DST "); Serial.println(ns->sensTm.tm_isdst);
      }
    } else {
      addErrorLog(httpCode);
      err = httpCode;
      if(err == 301 || err == 302) {
        if(http.header("location").length() > 0) {
          strncpy(cfg->url, http.header("location").c_str(), 127);
          Serial.printf("HTTP error %d, redirecting to \"%s\"\r\n", err, cfg->url);
          rcnt = 4;
        }
      }
    }
  } else {
    addErrorLog(httpCode);
    err = httpCode;
  }
  http.end();

  if(err != 0) {
    Serial.printf("Returning with error %d after %lu ms :-(\r\n", err, timeInGET);
    Serial.println(http.errorToString(err));
    M5.Lcd.fillRect(icon_xpos[0], icon_ypos[0], 16, 16, BLACK);
    return err;
  }

  // --- 2. Query Properties ---
  buildBaseUrl(NSurl, sizeof(NSurl), cfg->url);
  switch(cfg->info_line) {
    case 2:
      strlcat(NSurl, "/api/v2/properties/iob,cob,delta,loop,basal", sizeof(NSurl));
      break;
    case 3:
      strlcat(NSurl, "/api/v2/properties/iob,cob,delta,openaps,basal", sizeof(NSurl));
      break;
    default:
      strlcat(NSurl, "/api/v2/properties/iob,cob,delta,basal", sizeof(NSurl));
  }

  if(cfg->token[0] != '\0') {
    strlcat(NSurl, "?token=", sizeof(NSurl));
    strlcat(NSurl, cfg->token, sizeof(NSurl));
  }

  M5.Lcd.fillRect(icon_xpos[0], icon_ypos[0], 16, 16, BLACK);
  drawIcon(icon_xpos[0], icon_ypos[0], (uint8_t*)wifi1_icon16x16, TFT_BLUE);

  Serial.print("Properties query NSurl = '"); Serial.print(NSurl); Serial.println("'");
  if(is_https_Heroku) {
    if(http.begin(sclient, NSurl)) {
      Serial.println("http.begin propertires HTTPS Heroku OK");
    } else {
      Serial.println("http.begin propertires HTTPS Heroku FAILED");
    }
  } else {
    if(http.begin(NSurl)) {
      Serial.println("http.begin propertires OK");
    } else {
      Serial.println("http.begin propertires FAILED");
    }
  }

  timeInGET = millis();
  httpCode = http.GET();
  timeInGET = millis() - timeInGET;

  if(httpCode > 0) {
    Serial.printf("[HTTP] GET properties... code: %d after %lu ms\r\n", httpCode, timeInGET);
    if(httpCode == HTTP_CODE_OK) {
      String propjson = http.getString();
      Serial.printf("GET() properties response JSON length = %d\r\n", propjson.length());
      sanitizeJsonString(propjson);

      DeserializationError propJSONerr = deserializeJson(JSONdoc, propjson);
      if(propJSONerr) {
        err = 1003;
        Serial.println("JSON parsing failed");
        addErrorLog(err);
      } else {
        Serial.println("Deserialized the second JSON and OK");
        JsonObject iob = JSONdoc["iob"];
        ns->iob = iob["iob"];
        strncpy(ns->iob_display, iob["display"] | "N/A", 16);
        strncpy(ns->iob_displayLine, iob["displayLine"] | "IOB: N/A", 16);

        JsonObject cob = JSONdoc["cob"];
        ns->cob = cob["cob"];
        strncpy(ns->cob_display, cob["display"] | "N/A", 16);
        strncpy(ns->cob_displayLine, cob["displayLine"] | "COB: N/A", 16);

        JsonObject delta = JSONdoc["delta"];
        ns->delta_absolute = delta["absolute"];
        ns->delta_elapsedMins = delta["elapsedMins"];
        ns->delta_interpolated = delta["interpolated"];
        ns->delta_mean5MinsAgo = delta["mean5MinsAgo"];
        ns->delta_mgdl = delta["mgdl"];
        ns->delta_scaled = ns->delta_mgdl / 18.0;
        if(cfg->show_mgdl) {
          snprintf(ns->delta_display, sizeof(ns->delta_display), "%+d", ns->delta_mgdl);
        } else {
          snprintf(ns->delta_display, sizeof(ns->delta_display), "%+.1f", ns->delta_scaled);
        }

        JsonObject loop_obj;
        JsonObject loop_display;
        if(cfg->info_line == 3) {
          loop_obj = JSONdoc["openaps"];
          loop_display = loop_obj["status"];
        } else {
          loop_obj = JSONdoc["loop"];
          loop_display = loop_obj["display"];
        }
        strncpy(tmpstr, loop_display["symbol"] | "?", 4);
        ns->loop_display_symbol = tmpstr[0];
        strncpy(ns->loop_display_code, loop_display["code"] | "N/A", 16);
        strncpy(ns->loop_display_label, loop_display["label"] | "N/A", 16);

        JsonObject basal = JSONdoc["basal"];
        strncpy(ns->basal_display, basal["display"] | "N/A", 16);

        JsonObject basal_current_doc = JSONdoc["basal"]["current"];
        ns->basal_current = basal_current_doc["basal"];
        ns->basal_tempbasal = basal_current_doc["tempbasal"];
        ns->basal_combobolusbasal = basal_current_doc["combobolusbasal"];
        ns->basal_totalbasal = basal_current_doc["totalbasal"];
      }
    } else {
      addErrorLog(httpCode);
      err = httpCode;
    }
  } else {
    Serial.printf("Returning with error %d after %lu ms :-(\r\n", httpCode, timeInGET);
    Serial.println(http.errorToString(httpCode));
    addErrorLog(httpCode);
    err = httpCode;
  }
  http.end();

  M5.Lcd.fillRect(icon_xpos[0], icon_ypos[0], 16, 16, BLACK);
  return err;
}
