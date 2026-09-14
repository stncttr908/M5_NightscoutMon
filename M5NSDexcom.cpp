/*  M5Stack Nightscout monitor - Dexcom Share ("Follow") data source
    Fetches the latest glucose readings directly from a Dexcom Share account,
    for use when no Nightscout site is available. Adapted from the Dexcom
    Share protocol used by ktomy/nightscout-clock (BGSourceDexcom).
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>
#include "M5NSDexcom.h"
#include "externs.h"

static const char* DEX_SERVERS[3] = {
  "https://share1.dexcom.com",     // US
  "https://shareous1.dexcom.com",  // outside US
  "https://share.dexcom.jp"        // Japan
};
static const char* DEX_APPID_STD    = "d89443d2-327c-4a6f-89e5-496bbb0317db";
static const char* DEX_APPID_JAPAN  = "d8665ade-9673-4e27-9ff6-92db4ce13d13";

static const char* DEX_AUTH_PATH     = "/ShareWebServices/Services/General/AuthenticatePublisherAccount";
static const char* DEX_LOGIN_PATH    = "/ShareWebServices/Services/General/LoginPublisherAccountById";
static const char* DEX_READINGS_PATH = "/ShareWebServices/Services/Publisher/ReadPublisherLatestGlucoseValues";

// Cached across polls so we only re-authenticate when the session actually expires.
static char dexAccountId[40] = "";
static char dexSessionId[40] = "";
// Set once Dexcom reports invalid credentials, to stop hammering the account with
// repeated failed logins (which can lock it). Cleared by dexcomResetSession().
static bool dexCredentialError = false;

void dexcomResetSession() {
  dexAccountId[0] = 0;
  dexSessionId[0] = 0;
  dexCredentialError = false;
}

// Same direction-string -> arrow angle mapping used by readNightscout(); Dexcom's
// "Trend" values ("Flat", "SingleUp", ...) match the CamelCase branch already handled here.
int directionToArrowAngle(const char *sensDir) {
  int arrowAngle = 180;
  if (strcmp(sensDir, "DoubleDown") == 0 || strcmp(sensDir, "DOUBLE_DOWN") == 0)
    arrowAngle = 90;
  else if (strcmp(sensDir, "SingleDown") == 0 || strcmp(sensDir, "SINGLE_DOWN") == 0)
    arrowAngle = 75;
  else if (strcmp(sensDir, "FortyFiveDown") == 0 || strcmp(sensDir, "FORTY_FIVE_DOWN") == 0)
    arrowAngle = 45;
  else if (strcmp(sensDir, "Flat") == 0 || strcmp(sensDir, "FLAT") == 0)
    arrowAngle = 0;
  else if (strcmp(sensDir, "FortyFiveUp") == 0 || strcmp(sensDir, "FORTY_FIVE_UP") == 0)
    arrowAngle = -45;
  else if (strcmp(sensDir, "SingleUp") == 0 || strcmp(sensDir, "SINGLE_UP") == 0)
    arrowAngle = -75;
  else if (strcmp(sensDir, "DoubleUp") == 0 || strcmp(sensDir, "DOUBLE_UP") == 0)
    arrowAngle = -90;
  else if (strcmp(sensDir, "NONE") == 0)
    arrowAngle = 180;
  else if (strcmp(sensDir, "NOT COMPUTABLE") == 0)
    arrowAngle = 180;
  return arrowAngle;
}

// Dexcom Share auth endpoints return a JSON string like "\"abcd1234-...\"" - strip the quotes.
static void stripQuotes(const String &in, char *out, size_t outLen) {
  String s = in;
  s.trim();
  if (s.length() >= 2 && s.charAt(0) == '"' && s.charAt(s.length() - 1) == '"') {
    s = s.substring(1, s.length() - 1);
  }
  strlcpy(out, s.c_str(), outLen);
}

static int dexPost(WiFiClientSecure &client, const char *baseUrl, const char *path, const String &body, String &response) {
  HTTPClient http;
  String url = String(baseUrl) + path;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "Dexcom Share/3.0.2.11 CFNetwork/711.2.23 Darwin/14.0.0");
  int code = http.POST(body);
  response = http.getString();
  http.end();
  return code;
}

static bool isBadCredentials(const String &resp) {
  return resp.indexOf("AccountPasswordInvalid") >= 0 ||
         resp.indexOf("SSO_AuthenticateAccountNotFound") >= 0 ||
         resp.indexOf("AccountNotFound") >= 0;
}

static int dexAuthenticate(WiFiClientSecure &client, const char *baseUrl, const char *appId, const char *user, const char *pass) {
  JSONdoc.clear();
  JSONdoc["accountName"] = user;
  JSONdoc["password"] = pass;
  JSONdoc["applicationId"] = appId;
  String body;
  serializeJson(JSONdoc, body);

  String resp;
  int code = dexPost(client, baseUrl, DEX_AUTH_PATH, body, resp);
  Serial.printf("[Dexcom] AuthenticatePublisherAccount code=%d\r\n", code);
  if (code != HTTP_CODE_OK) {
    if (isBadCredentials(resp)) {
      dexCredentialError = true;
      addErrorLog(1102);
      return 1102;
    }
    addErrorLog(code > 0 ? code : 1101);
    return code > 0 ? code : 1101;
  }
  stripQuotes(resp, dexAccountId, sizeof(dexAccountId));
  if (strlen(dexAccountId) == 0 || strcmp(dexAccountId, "00000000-0000-0000-0000-000000000000") == 0) {
    dexCredentialError = true;
    addErrorLog(1102);
    return 1102;
  }

  JSONdoc.clear();
  JSONdoc["accountId"] = dexAccountId;
  JSONdoc["password"] = pass;
  JSONdoc["applicationId"] = appId;
  body = "";
  serializeJson(JSONdoc, body);

  code = dexPost(client, baseUrl, DEX_LOGIN_PATH, body, resp);
  Serial.printf("[Dexcom] LoginPublisherAccountById code=%d\r\n", code);
  if (code != HTTP_CODE_OK) {
    if (isBadCredentials(resp)) {
      dexCredentialError = true;
      addErrorLog(1102);
      return 1102;
    }
    addErrorLog(code > 0 ? code : 1101);
    return code > 0 ? code : 1101;
  }
  stripQuotes(resp, dexSessionId, sizeof(dexSessionId));
  if (strlen(dexSessionId) == 0 || strcmp(dexSessionId, "00000000-0000-0000-0000-000000000000") == 0) {
    dexCredentialError = true;
    addErrorLog(1102);
    return 1102;
  }
  return 0;
}

int readDexcom(tConfig *cfg, struct NSinfo *ns) {
  if (WiFiMultiple.run() != WL_CONNECTED) {
    ESP.restart();
  }

  if (dexCredentialError) {
    Serial.println("[Dexcom] Skipping fetch, invalid credentials backoff active");
    return 1102;
  }

  int serverIdx = cfg->dexcom_server;
  if (serverIdx < 0 || serverIdx > 2)
    serverIdx = 0;
  const char* baseUrl = DEX_SERVERS[serverIdx];
  const char* appId = (serverIdx == 2) ? DEX_APPID_JAPAN : DEX_APPID_STD;

  WiFiClientSecure client;
  client.setInsecure();

  int err = 0;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (dexSessionId[0] == 0) {
      err = dexAuthenticate(client, baseUrl, appId, cfg->dexcom_user, cfg->dexcom_pass);
      if (err != 0)
        return err;
    }

    String url = String(baseUrl) + DEX_READINGS_PATH + "?sessionId=" + dexSessionId + "&minutes=1440&maxCount=" + String(MAX_SGV_HISTORY);
    HTTPClient http;
    http.begin(client, url);
    http.addHeader("Accept", "application/json");
    http.addHeader("User-Agent", "Dexcom Share/3.0.2.11 CFNetwork/711.2.23 Darwin/14.0.0");
    int httpCode = http.GET();
    String json = http.getString();
    http.end();

    Serial.printf("[Dexcom] ReadPublisherLatestGlucoseValues code=%d\r\n", httpCode);

    if (httpCode != HTTP_CODE_OK) {
      if ((httpCode == 500 || httpCode == 401) && json.indexOf("Session") >= 0) {
        Serial.println("[Dexcom] Session expired, re-authenticating and retrying");
        dexSessionId[0] = 0;
        continue;
      }
      if (isBadCredentials(json)) {
        dexCredentialError = true;
        addErrorLog(1102);
        return 1102;
      }
      addErrorLog(httpCode > 0 ? httpCode : 1101);
      return httpCode > 0 ? httpCode : 1101;
    }

    JSONdoc.clear();
    DeserializationError jerr = deserializeJson(JSONdoc, json);
    JsonArray arr = JSONdoc.as<JsonArray>();
    if (jerr || arr.size() == 0) {
      err = jerr ? 1001 : 1004;
      addErrorLog(err);
      return err;
    }

    strlcpy(ns->sensDev, "Dexcom Share", 64);
    ns->is_xDrip = false;

    const char* st0 = arr[0]["ST"] | "";
    const char* p0 = strchr(st0, '(');
    ns->rawtime = p0 ? strtoull(p0 + 1, NULL, 10) : 0; // ms since 1970, e.g. "Date(1703182152000)"
    ns->sensTime = ns->rawtime / 1000;
    localtime_r(&ns->sensTime, &ns->sensTm);

    JsonVariant trendVar = arr[0]["Trend"];
    if (trendVar.is<const char*>()) {
      strlcpy(ns->sensDir, trendVar.as<const char*>(), 32);
    } else {
      static const char* trendNames[10] = {
        "NONE", "DoubleUp", "SingleUp", "FortyFiveUp", "Flat",
        "FortyFiveDown", "SingleDown", "DoubleDown", "NOT COMPUTABLE", "NOT COMPUTABLE"
      };
      int t = trendVar.as<int>();
      strlcpy(ns->sensDir, (t >= 0 && t <= 9) ? trendNames[t] : "NONE", 32);
    }
    ns->arrowAngle = directionToArrowAngle(ns->sensDir);

    ns->sensSgvMgDl = arr[0]["Value"];
    ns->sensSgv = ns->sensSgvMgDl / 18.0;

    ns->sgvHistoryCount = 0;
    int nEntries = arr.size();
    if (nEntries > MAX_SGV_HISTORY) nEntries = MAX_SGV_HISTORY;
    for (int i = 0; i < nEntries; i++) {
      const char* st = arr[i]["ST"] | "";
      const char* p = strchr(st, '(');
      uint64_t rt = p ? strtoull(p + 1, NULL, 10) : 0;
      ns->sgvHistory[ns->sgvHistoryCount].sgv = ((float)(int)arr[i]["Value"]) / 18.0;
      ns->sgvHistory[ns->sgvHistoryCount].time = (time_t)(rt / 1000);
      ns->sgvHistoryCount++;
    }
    populateLast10FromHistory(ns);

    if (arr.size() >= 2) {
      int v0 = arr[0]["Value"];
      int v1 = arr[1]["Value"];
      const char* st1 = arr[1]["ST"] | "";
      const char* p1 = strchr(st1, '(');
      uint64_t rawtime1 = p1 ? strtoull(p1 + 1, NULL, 10) : ns->rawtime;
      ns->delta_mgdl = v0 - v1;
      ns->delta_absolute = ns->delta_mgdl;
      ns->delta_scaled = ns->delta_mgdl / 18.0;
      ns->delta_elapsedMins = (ns->rawtime - rawtime1) / 60000.0;
      ns->delta_interpolated = false;
      if (cfg->show_mgdl) {
        sprintf(ns->delta_display, "%+d", ns->delta_mgdl);
      } else {
        sprintf(ns->delta_display, "%+.1f", ns->delta_scaled);
      }
    } else {
      strcpy(ns->delta_display, "N/A");
    }

    ns->iob = 0;
    strlcpy(ns->iob_display, "N/A", 16);
    strlcpy(ns->iob_displayLine, "IOB: N/A", 16);
    ns->cob = 0;
    strlcpy(ns->cob_display, "N/A", 16);
    strlcpy(ns->cob_displayLine, "COB: N/A", 16);
    ns->loop_display_symbol = '?';
    strlcpy(ns->loop_display_code, "N/A", 16);
    strlcpy(ns->loop_display_label, "N/A", 16);
    strlcpy(ns->basal_display, "N/A", 16);

    return 0;
  }

  return err;
}
