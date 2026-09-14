/*  M5Stack Nightscout monitor - LibreLinkUp ("LibreView") follower data source
    Fetches the latest glucose readings directly from a LibreLinkUp follower account,
    for use when no Nightscout site is available. Protocol adapted from the LibreLinkUp
    client used by ktomy/nightscout-clock (BGSourceLibreLinkUp).
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <strings.h>
#include <mbedtls/sha256.h>
#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>
#include "M5NSLibre.h"
#include "M5NSDexcom.h" // reuses directionToArrowAngle()
#include "externs.h"

// Real, redirect-matchable regions occupy indices 0..LIBRE_NUM_REGIONS-1. Index LIBRE_AUTO
// is the universal entry point: login starts there and follows the region redirect, so the
// user never has to guess. LibreLinkUp never reports "auto" as a redirect region.
#define LIBRE_NUM_REGIONS 12
#define LIBRE_AUTO 12
const char* const libreRegionNames[13] = {
  "AE", "AP", "AU", "CA", "DE", "EU", "EU2", "FR", "JP", "US", "LA", "RU", "AUTO"
};
static const char* LIBRE_HOSTS[13] = {
  "api-ae.libreview.io", "api-ap.libreview.io", "api-au.libreview.io", "api-ca.libreview.io",
  "api-de.libreview.io", "api-eu.libreview.io", "api-eu2.libreview.io", "api-fr.libreview.io",
  "api-jp.libreview.io", "api-us.libreview.io", "api-la.libreview.io", "api.libreview.ru",
  "api.libreview.io"
};

#define LIBRE_LINK_UP_VERSION "4.16.0"
#define LIBRE_LINK_UP_PRODUCT "llu.ios"
#define LIBRE_USER_AGENT "Mozilla/5.0 (iPhone; CPU OS 17_4.1 like Mac OS X) AppleWebKit/536.26 (KHTML, like Gecko) Version/17.4.1 Mobile/10A5355d Safari/8536.25"

// Cached across polls so we only re-authenticate when the token actually expires.
static String libreToken = "";
static time_t libreTokenExpires = 0;
static char libreAccountIdHash[65] = "";
static char librePatientId[64] = "";
// Set once LibreLinkUp reports invalid credentials, to stop hammering the account with
// repeated failed logins. Cleared by libreResetSession().
static bool libreCredentialError = false;

void libreResetSession() {
  libreToken = "";
  libreTokenExpires = 0;
  libreAccountIdHash[0] = 0;
  librePatientId[0] = 0;
  libreCredentialError = false;
}

static void setLibreHeaders(HTTPClient &http, bool authRequired) {
  http.addHeader("User-Agent", LIBRE_USER_AGENT);
  http.addHeader("Content-Type", "application/json;charset=UTF-8");
  http.addHeader("Accept", "application/json");
  http.addHeader("cache-control", "no-cache");
  http.addHeader("version", LIBRE_LINK_UP_VERSION);
  http.addHeader("product", LIBRE_LINK_UP_PRODUCT);
  if (authRequired) {
    http.addHeader("Authorization", "Bearer " + libreToken);
    http.addHeader("account-id", libreAccountIdHash);
  }
}

static void sha256Hex(const char *in, char *outHex65) {
  unsigned char hash[32];
  mbedtls_sha256((const unsigned char*)in, strlen(in), hash, 0);
  for (int i = 0; i < 32; i++)
    sprintf(outHex65 + i * 2, "%02x", hash[i]);
  outHex65[64] = 0;
}

// Days since 1970-01-01 for a UTC civil date (Howard Hinnant's days_from_civil algorithm).
// Used instead of timegm()/mktime() so the conversion doesn't depend on core-specific
// timezone handling - the device's own clock keeps local time via NTP elsewhere.
static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

// LibreLinkUp timestamps look like "6/21/2026 3:45:12 PM", always UTC.
static time_t parseLibreTimestamp(const char *ts) {
  int month, day, year, hour, minute, second;
  char ampm[3] = {0};
  if (sscanf(ts, "%d/%d/%d %d:%d:%d %2s", &month, &day, &year, &hour, &minute, &second, ampm) != 7)
    return 0;
  if (hour == 12) hour = 0;
  if (strcasecmp(ampm, "PM") == 0) hour += 12;
  int64_t days = daysFromCivil(year, month, day);
  return (time_t)(days * 86400LL + hour * 3600 + minute * 60 + second);
}

// Logs in, following one region redirect and one terms-of-use acceptance if LibreLinkUp
// asks for either. On success, caches libreToken/libreTokenExpires/libreAccountIdHash.
// *redirectRegion is set to the correct region index if the server reports one (caller
// must retry against that region); left at -1 otherwise.
static int loginLibre(WiFiClientSecure &client, int serverIdx, const char *email, const char *pass, int touDepth, int *redirectRegion) {
  *redirectRegion = -1;

  HTTPClient http;
  String url = String("https://") + LIBRE_HOSTS[serverIdx] + "/llu/auth/login";
  http.begin(client, url);
  setLibreHeaders(http, false);

  JSONdoc.clear();
  JSONdoc["email"] = email;
  JSONdoc["password"] = pass;
  String body;
  serializeJson(JSONdoc, body);

  int code = http.POST(body);
  String resp = http.getString();
  http.end();
  Serial.printf("[Libre] login code=%d\r\n", code);

  if (code != HTTP_CODE_OK) {
    addErrorLog(code > 0 ? code : 1201);
    return code > 0 ? code : 1201;
  }

  JSONdoc.clear();
  DeserializationError jerr = deserializeJson(JSONdoc, resp);
  if (jerr) {
    Serial.printf("[Libre] login parse error: %s\r\n", jerr.c_str());
    addErrorLog(1001);
    return 1001;
  }

  if (JSONdoc["data"]["redirect"] | false) {
    const char* region = JSONdoc["data"]["region"] | "";
    for (int i = 0; i < LIBRE_NUM_REGIONS; i++) {
      if (strcasecmp(region, libreRegionNames[i]) == 0) {
        *redirectRegion = i;
        break;
      }
    }
    return 0; // caller retries login against the correct region
  }

  const char* stepType = JSONdoc["data"]["step"]["type"] | "";
  if (strcmp(stepType, "tou") == 0) {
    const char* touToken = JSONdoc["data"]["authTicket"]["token"] | "";
    if (touDepth >= 1 || strlen(touToken) == 0) {
      addErrorLog(1203); // account must accept new LibreLinkUp terms in the mobile app first
      return 1203;
    }
    HTTPClient http2;
    String touUrl = String("https://") + LIBRE_HOSTS[serverIdx] + "/llu/auth/continue/tou";
    http2.begin(client, touUrl);
    setLibreHeaders(http2, false);
    http2.addHeader("Authorization", String("Bearer ") + touToken);
    http2.POST("{}");
    http2.end();
    return loginLibre(client, serverIdx, email, pass, touDepth + 1, redirectRegion);
  }

  // Checked only after the redirect and terms-of-use steps: those arrive with a nonzero
  // status too (tou = 4), and must not be mistaken for bad credentials.
  int status = JSONdoc["status"] | 0;
  if (status != 0) {
    libreCredentialError = true;
    addErrorLog(1202);
    return 1202;
  }

  const char* token = JSONdoc["data"]["authTicket"]["token"] | "";
  unsigned long long expires = JSONdoc["data"]["authTicket"]["expires"] | 0ULL;
  const char* accountId = JSONdoc["data"]["user"]["id"] | "";
  if (strlen(token) == 0 || strlen(accountId) == 0) {
    addErrorLog(1201);
    return 1201;
  }

  libreToken = token;
  libreTokenExpires = (time_t)expires;
  sha256Hex(accountId, libreAccountIdHash);
  return 0;
}

// Returns -401 (caller should re-authenticate and retry) on an expired token.
static int fetchPatientId(WiFiClientSecure &client, int serverIdx) {
  HTTPClient http;
  String url = String("https://") + LIBRE_HOSTS[serverIdx] + "/llu/connections";
  http.begin(client, url);
  setLibreHeaders(http, true);
  int code = http.GET();
  String resp = http.getString();
  http.end();
  Serial.printf("[Libre] connections code=%d\r\n", code);

  if (code == HTTP_CODE_UNAUTHORIZED)
    return -401;
  if (code != HTTP_CODE_OK) {
    addErrorLog(code > 0 ? code : 1201);
    return code > 0 ? code : 1201;
  }

  JSONdoc.clear();
  DeserializationError jerr = deserializeJson(JSONdoc, resp);
  if (jerr) {
    Serial.printf("[Libre] connections parse error: %s\r\n", jerr.c_str());
    addErrorLog(1001);
    return 1001;
  }
  JsonArray arr = JSONdoc["data"].as<JsonArray>();
  if (arr.size() == 0) {
    // Login worked but this follower account isn't linked to any sensor/connection.
    addErrorLog(1204);
    return 1204;
  }
  // A follower account may be linked to several people; arr[0] is not necessarily the one
  // whose sensor is currently reporting. Prefer the connection that has a live
  // glucoseMeasurement (most recent wins), so we don't follow an idle sensor and get an
  // endless "No current reading" (1004).
  const char* pid = "";
  time_t bestTs = 0;
  for (int i = 0; i < (int)arr.size(); i++) {
    JsonObject gm = arr[i]["glucoseMeasurement"];
    if (gm.isNull())
      continue; // no live reading on this connection
    time_t ts = parseLibreTimestamp(gm["FactoryTimestamp"] | "");
    if (pid[0] == 0 || ts > bestTs) {
      pid = arr[i]["patientId"] | "";
      bestTs = ts;
    }
  }
  if (pid[0] == 0)
    pid = arr[0]["patientId"] | ""; // none reporting - fall back (graph will report 1004)
  if (strlen(pid) == 0) {
    addErrorLog(1204);
    return 1204;
  }
  strlcpy(librePatientId, pid, sizeof(librePatientId));
  return 0;
}

int readLibre(tConfig *cfg, struct NSinfo *ns) {
  if (WiFiMultiple.run() != WL_CONNECTED) {
    ESP.restart();
  }

  if (libreCredentialError) {
    Serial.println("[Libre] Skipping fetch, invalid credentials backoff active");
    return 1202;
  }

  int serverIdx = cfg->libre_server;
  if (serverIdx < 0 || serverIdx > LIBRE_AUTO)
    serverIdx = LIBRE_AUTO; // default: log in via the universal entry point and auto-detect the region

  WiFiClientSecure client;
  client.setInsecure();

  int err = 0;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (libreToken.length() == 0 || time(NULL) >= libreTokenExpires) {
      int redirectRegion = -1;
      err = loginLibre(client, serverIdx, cfg->libre_user, cfg->libre_pass, 0, &redirectRegion);
      if (err != 0)
        return err;
      if (redirectRegion >= 0 && redirectRegion != serverIdx) {
        // Universal entry point told us the account's real region - use it for this
        // session's connections/graph calls. Not persisted, so login always restarts at the
        // universal entry point and re-detects (handles a later region change on its own).
        Serial.printf("[Libre] Region detected: %s\r\n", libreRegionNames[redirectRegion]);
        serverIdx = redirectRegion;
        err = loginLibre(client, serverIdx, cfg->libre_user, cfg->libre_pass, 0, &redirectRegion);
        if (err != 0)
          return err;
      }
      librePatientId[0] = 0; // token changed, re-resolve the followed patient
    }

    if (librePatientId[0] == 0) {
      err = fetchPatientId(client, serverIdx);
      if (err == -401) {
        libreToken = "";
        continue;
      }
      if (err != 0)
        return err;
    }

    HTTPClient http;
    String url = String("https://") + LIBRE_HOSTS[serverIdx] + "/llu/connections/" + librePatientId + "/graph";
    // HTTP/1.0 forbids chunked responses. Required here because the body is parsed straight
    // off http.getStream(), which - unlike getString() - does not decode chunked framing;
    // with HTTP/1.1 the chunk-size lines corrupt the JSON (errors 1001/1004 every poll).
    http.useHTTP10(true);
    http.begin(client, url);
    setLibreHeaders(http, true);
    int httpCode = http.GET();
    Serial.printf("[Libre] graph code=%d\r\n", httpCode);

    if (httpCode == HTTP_CODE_UNAUTHORIZED) {
      http.end();
      Serial.println("[Libre] Token expired, re-authenticating and retrying");
      libreToken = "";
      continue;
    }
    if (httpCode != HTTP_CODE_OK) {
      http.end();
      addErrorLog(httpCode > 0 ? httpCode : 1201);
      return httpCode > 0 ? httpCode : 1201;
    }

    // The graph response is ~30-50 KB, too big for the shared 16 KB JSONdoc - filter it
    // down to just the fields used here while streaming, into a local document.
    StaticJsonDocument<256> filter;
    JsonObject filterData = filter.createNestedObject("data");
    filterData["connection"]["glucoseMeasurement"] = true;
    JsonArray filterGraphData = filterData.createNestedArray("graphData");
    JsonObject filterGraphItem = filterGraphData.createNestedObject();
    filterGraphItem["ValueInMgPerDl"] = true;
    filterGraphItem["FactoryTimestamp"] = true;

    DynamicJsonDocument doc(16384);
    DeserializationError jerr = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();

    if (jerr) {
      Serial.printf("[Libre] graph parse error: %s\r\n", jerr.c_str());
      addErrorLog(1001);
      return 1001;
    }

    JsonObject gm = doc["data"]["connection"]["glucoseMeasurement"];
    if (gm.isNull()) {
      // The followed connection has no live reading. The active sensor may have moved to
      // another patient on this follower account - re-resolve once before reporting 1004.
      if (attempt == 0) {
        librePatientId[0] = 0;
        continue;
      }
      addErrorLog(1004);
      return 1004;
    }

    strlcpy(ns->sensDev, "LibreLinkUp", 64);
    ns->is_xDrip = false;

    float valueMgDl = gm["ValueInMgPerDl"] | 0;
    const char* ts = gm["FactoryTimestamp"] | "";
    ns->sensTime = parseLibreTimestamp(ts);
    ns->rawtime = (uint64_t)ns->sensTime * 1000ULL;
    localtime_r(&ns->sensTime, &ns->sensTm);

    int trendArrow = gm["TrendArrow"] | 0;
    static const char* trendNames[6] = {
      "NONE", "SingleDown", "FortyFiveDown", "Flat", "FortyFiveUp", "SingleUp"
    };
    strlcpy(ns->sensDir, (trendArrow >= 1 && trendArrow <= 5) ? trendNames[trendArrow] : "NONE", 32);
    ns->arrowAngle = directionToArrowAngle(ns->sensDir);

    ns->sensSgvMgDl = valueMgDl;
    ns->sensSgv = valueMgDl / 18.0;

    JsonArray graphArr = doc["data"]["graphData"].as<JsonArray>();
    int n = graphArr.size();
    int prevIdx = n - 1;
    if (prevIdx >= 0) {
      const char* lastTs = graphArr[prevIdx]["FactoryTimestamp"] | "";
      if (parseLibreTimestamp(lastTs) == ns->sensTime && prevIdx > 0)
        prevIdx--; // the newest graph point duplicates the current reading
    }

    ns->sgvHistoryCount = 0;
    ns->sgvHistory[0].sgv = valueMgDl / 18.0;
    ns->sgvHistory[0].time = ns->sensTime;
    ns->sgvHistoryCount = 1;

    for (int gi = prevIdx; gi >= 0 && ns->sgvHistoryCount < MAX_SGV_HISTORY; gi--) {
      float val = ((float)(int)(graphArr[gi]["ValueInMgPerDl"] | 0)) / 18.0;
      time_t ep = parseLibreTimestamp(graphArr[gi]["FactoryTimestamp"] | "");
      if (val > 0 && ep > 0) {
        ns->sgvHistory[ns->sgvHistoryCount].sgv = val;
        ns->sgvHistory[ns->sgvHistoryCount].time = ep;
        ns->sgvHistoryCount++;
      }
    }
    populateLast10FromHistory(ns);

    if (prevIdx >= 0) {
      float prevMgDl = graphArr[prevIdx]["ValueInMgPerDl"] | 0;
      time_t prevEpoch = parseLibreTimestamp(graphArr[prevIdx]["FactoryTimestamp"] | "");
      ns->delta_mgdl = (int)(valueMgDl - prevMgDl);
      ns->delta_absolute = ns->delta_mgdl;
      ns->delta_scaled = ns->delta_mgdl / 18.0;
      ns->delta_elapsedMins = prevEpoch > 0 ? (ns->sensTime - prevEpoch) / 60.0 : 0;
      ns->delta_interpolated = false;
      if (cfg->show_mgdl)
        sprintf(ns->delta_display, "%+d", ns->delta_mgdl);
      else
        sprintf(ns->delta_display, "%+.1f", ns->delta_scaled);
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
