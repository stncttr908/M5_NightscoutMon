#include "M5NSTreatments.h"
#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "externs.h"
#include "Free_Fonts.h"

extern const unsigned char sun_icon16x16[];
extern const unsigned char clock_icon16x16[];
extern const unsigned char door_icon16x16[];

enum InsulinType {
  INSULIN_FAST = 0,
  INSULIN_LONG = 1
};

static InsulinType s_insulinType = INSULIN_FAST;
static float s_fastDose = 1.0f;
static float s_longDose = 18.0f;
static TreatmentModalState s_modalState = TREATMENT_MODAL_NONE;
static uint32_t s_modalTimeout = 0;
static String s_statusMessage = "";
static uint32_t s_statusMsgExpiry = 0;
static bool s_lastPostSuccess = false;

// Calculate smart suggested correction dose based on current glucose
static float getSuggestedCorrection() {
  float bg = (ns.sensSgvMgDl > 20.0f) ? ns.sensSgvMgDl : (ns.sensSgv * 18.018f);
  if (bg < 120.0f) return 0.5f;
  float diff = bg - 100.0f;
  float isf = 45.0f; // Standard correction sensitivity ~45 mg/dL per unit
  float raw = diff / isf;
  // Round to nearest 0.5U for simplicity
  float rounded = roundf(raw * 2.0f) / 2.0f;
  if (rounded < 0.5f) rounded = 0.5f;
  if (rounded > 15.0f) rounded = 15.0f;
  return rounded;
}

void initTreatments() {
  s_insulinType = INSULIN_FAST;
  s_fastDose = 1.0f;
  s_longDose = 18.0f;
  s_modalState = TREATMENT_MODAL_NONE;
  s_statusMessage = "";
}

bool isTreatmentsModalActive() {
  return (s_modalState != TREATMENT_MODAL_NONE);
}

bool postNightscoutTreatment(const char* eventType, float insulin, float carbs, const char* notes, String &outMsg) {
  if (WiFi.status() != WL_CONNECTED) {
    outMsg = "WiFi disconnected";
    return false;
  }

  if (strlen(cfg.url) == 0) {
    outMsg = "No Nightscout URL configured";
    return false;
  }

  char NSurl[192];
  if (strncmp(cfg.url, "http", 4) != 0) {
    strcpy(NSurl, "https://");
  } else {
    strcpy(NSurl, "");
  }
  strcat(NSurl, cfg.url);
  strcat(NSurl, "/api/v1/treatments");

  if (strlen(cfg.token) > 0) {
    if (strchr(NSurl, '?')) {
      strcat(NSurl, "&token=");
    } else {
      strcat(NSurl, "?token=");
    }
    strcat(NSurl, cfg.token);
  }

  time_t now;
  time(&now);
  struct tm *utc = gmtime(&now);
  char timeBuf[32];
  if (utc) {
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%SZ", utc);
  } else {
    strcpy(timeBuf, "2026-09-02T05:30:00Z");
  }

  DynamicJsonDocument doc(512);
  doc["enteredBy"] = cfg.deviceName[0] ? cfg.deviceName : "M5Stack Core2";
  doc["eventType"] = eventType;
  doc["created_at"] = timeBuf;
  if (insulin > 0.001f) {
    doc["insulin"] = serialized(String(insulin, 1));
  }
  if (carbs > 0.001f) {
    doc["carbs"] = (int)round(carbs);
  }
  if (notes && strlen(notes) > 0) {
    doc["notes"] = notes;
  }

  String payload;
  serializeJson(doc, payload);
  Serial.printf("[TREATMENT] POST %s -> %s\r\n", NSurl, payload.c_str());

  HTTPClient http;
  WiFiClientSecure sclient;
  sclient.setInsecure();

  bool beginOk = false;
  if (strncmp(NSurl, "https", 5) == 0) {
    beginOk = http.begin(sclient, NSurl);
  } else {
    beginOk = http.begin(NSurl);
  }

  if (!beginOk) {
    outMsg = "HTTP connect failed";
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  if (strlen(cfg.token) > 0) {
    http.addHeader("api-secret", cfg.token);
  }

  int httpCode = http.POST(payload);
  http.end();

  if (httpCode >= 200 && httpCode < 300) {
    outMsg = "Logged OK (HTTP " + String(httpCode) + ")";
    return true;
  } else {
    outMsg = "Error HTTP " + String(httpCode);
    return false;
  }
}

static void drawSimpleBox(int x, int y, int w, int h, const char* label, uint16_t bg, uint16_t border, uint16_t textCol, bool isActive = false) {
  M5.Lcd.fillRoundRect(x, y, w, h, 5, bg);
  M5.Lcd.drawRoundRect(x, y, w, h, 5, border);
  if (isActive) {
    M5.Lcd.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 4, border);
  }
  M5.Lcd.setTextColor(textCol, bg);
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.drawString(label, x + w / 2, y + h / 2);
}

void drawRapidTreatmentsPage() {
  M5.Lcd.fillScreen(BLACK);

  // 1. Header (y: 0..22)
  M5.Lcd.drawLine(0, 22, 319, 22, TFT_DARKGREY);
  M5.Lcd.setFreeFont(FSSB9);
  M5.Lcd.setTextDatum(ML_DATUM);
  M5.Lcd.setTextColor(TFT_CYAN, BLACK);
  M5.Lcd.drawString("LOG INSULIN", 8, 11);

  // Current SGV in header (center)
  M5.Lcd.setTextDatum(MC_DATUM);
  char sgvBuf[32];
  if (cfg.show_mgdl) {
    snprintf(sgvBuf, sizeof(sgvBuf), "%.0f %s", ns.sensSgvMgDl, ns.sensDir);
  } else {
    snprintf(sgvBuf, sizeof(sgvBuf), "%.1f %s", ns.sensSgv, ns.sensDir);
  }
  uint16_t sgvColor = TFT_GREEN;
  if (ns.sensSgvMgDl < cfg.red_low || ns.sensSgvMgDl > cfg.red_high) sgvColor = TFT_RED;
  else if (ns.sensSgvMgDl < cfg.yellow_low || ns.sensSgvMgDl > cfg.yellow_high) sgvColor = TFT_YELLOW;
  M5.Lcd.setTextColor(sgvColor, BLACK);
  M5.Lcd.drawString(sgvBuf, 160, 11);

  // Time in header (right)
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char timeStr[16];
    if (cfg.time_format == 1) {
      strftime(timeStr, sizeof(timeStr), "%I:%M%p", &timeinfo);
    } else {
      strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
    }
    M5.Lcd.setTextDatum(MR_DATUM);
    M5.Lcd.setTextColor(TFT_LIGHTGREY, BLACK);
    M5.Lcd.drawString(timeStr, 312, 11);
  }

  // 2. Top Mode Toggle Tabs (y: 28..58, h: 30px)
  M5.Lcd.setFreeFont(FSSB9);
  bool isFast = (s_insulinType == INSULIN_FAST);

  // Fast Acting Tab (x: 10..155, w: 145)
  uint16_t fastBg = isFast ? 0x0217 : 0x0841;
  uint16_t fastBorder = isFast ? TFT_CYAN : 0x2104;
  uint16_t fastText = isFast ? TFT_WHITE : TFT_LIGHTGREY;
  drawSimpleBox(10, 28, 145, 30, "Fast Acting", fastBg, fastBorder, fastText, isFast);

  // Long Acting Tab (x: 165..310, w: 145)
  uint16_t longBg = !isFast ? 0x4010 : 0x0841;
  uint16_t longBorder = !isFast ? TFT_MAGENTA : 0x2104;
  uint16_t longText = !isFast ? TFT_WHITE : TFT_LIGHTGREY;
  drawSimpleBox(165, 28, 145, 30, "Long Acting", longBg, longBorder, longText, !isFast);

  // 3. Large Stepper Card with Live Center Dose (y: 66..136, h: 70px)
  M5.Lcd.fillRoundRect(10, 66, 300, 70, 8, 0x0841);
  M5.Lcd.drawRoundRect(10, 66, 300, 70, 8, 0x2104);

  // Left Step Buttons (x: 15..59 and x: 63..107)
  M5.Lcd.setFreeFont(FSSB9);
  if (isFast) {
    drawSimpleBox(15, 72, 44, 58, "-1.0", 0x18C3, 0x39E7, TFT_WHITE);
    drawSimpleBox(63, 72, 44, 58, "-0.5", 0x18C3, 0x39E7, TFT_WHITE);
  } else {
    drawSimpleBox(15, 72, 44, 58, "-5", 0x18C3, 0x39E7, TFT_WHITE);
    drawSimpleBox(63, 72, 44, 58, "-1", 0x18C3, 0x39E7, TFT_WHITE);
  }

  // Large Center Dose Readout (x: 108..212, center: 160)
  M5.Lcd.setFreeFont(FSSB18);
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setTextColor(isFast ? TFT_CYAN : TFT_MAGENTA, 0x0841);
  char doseStr[16];
  if (isFast) {
    snprintf(doseStr, sizeof(doseStr), "%.1f U", s_fastDose);
  } else {
    snprintf(doseStr, sizeof(doseStr), "%.0f U", s_longDose);
  }
  M5.Lcd.drawString(doseStr, 160, isFast ? 86 : 101);

  // If Fast Acting, show Smart Suggestion Chip directly below the dose readout
  if (isFast) {
    float sug = getSuggestedCorrection();
    char sugLabel[16];
    snprintf(sugLabel, sizeof(sugLabel), "~%.1fU", sug);
    bool isSugActive = (fabs(s_fastDose - sug) < 0.05f);
    M5.Lcd.setFreeFont(FSSB9);
    drawSimpleBox(128, 108, 64, 22, sugLabel, isSugActive ? 0x03E0 : 0x0200, isSugActive ? TFT_GREEN : 0x0400, TFT_GREEN, isSugActive);
  }

  // Right Step Buttons (x: 213..257 and x: 261..305)
  M5.Lcd.setFreeFont(FSSB9);
  if (isFast) {
    drawSimpleBox(213, 72, 44, 58, "+0.5", 0x18C3, 0x39E7, TFT_WHITE);
    drawSimpleBox(261, 72, 44, 58, "+1.0", 0x18C3, 0x39E7, TFT_WHITE);
  } else {
    drawSimpleBox(213, 72, 44, 58, "+1", 0x18C3, 0x39E7, TFT_WHITE);
    drawSimpleBox(261, 72, 44, 58, "+5", 0x18C3, 0x39E7, TFT_WHITE);
  }

  // 4. Single Full-Width Action Button (y: 146..204, h: 58px)
  char actionLabel[48];
  if (s_statusMessage.length() > 0 && millis() < s_statusMsgExpiry) {
    strlcpy(actionLabel, s_statusMessage.c_str(), sizeof(actionLabel));
  } else {
    if (isFast) {
      snprintf(actionLabel, sizeof(actionLabel), "LOG %.1f U FAST ACTING", s_fastDose);
    } else {
      snprintf(actionLabel, sizeof(actionLabel), "LOG %.0f U LONG ACTING", s_longDose);
    }
  }

  uint16_t actBg = (s_statusMessage.length() > 0 && millis() < s_statusMsgExpiry) ? (s_lastPostSuccess ? 0x03E0 : 0x7800) : (isFast ? 0x0217 : 0x4010);
  uint16_t actBorder = (s_statusMessage.length() > 0 && millis() < s_statusMsgExpiry) ? (s_lastPostSuccess ? TFT_GREEN : TFT_RED) : (isFast ? TFT_CYAN : TFT_MAGENTA);
  M5.Lcd.fillRoundRect(10, 146, 300, 58, 8, actBg);
  M5.Lcd.drawRoundRect(10, 146, 300, 58, 8, actBorder);
  M5.Lcd.drawRoundRect(11, 147, 298, 56, 7, actBorder);

  M5.Lcd.setFreeFont(FSSB12);
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setTextColor(TFT_WHITE, actBg);
  M5.Lcd.drawString(actionLabel, 160, 175);

  // 5. Standard Bottom Navigation Bar (y: 216..240)
  M5.Lcd.fillRect(0, 216, 320, 24, BLACK);
  M5.Lcd.drawLine(0, 216, 319, 216, TFT_DARKGREY);

  // Left button: Brightness
  drawIcon(45, 220, (uint8_t*)sun_icon16x16, TFT_LIGHTGREY);

  // Middle button: Snooze
  drawIcon(150, 220, (uint8_t*)clock_icon16x16, TFT_LIGHTGREY);

  // Right button: Page / Exit (Door icon) in standard Light Grey
  drawIcon(256, 220, (uint8_t*)door_icon16x16, TFT_LIGHTGREY);

  // 6. Safety Confirmation Modal Overlay (if active)
  if (s_modalState == TREATMENT_MODAL_CONFIRM) {
    M5.Lcd.fillRoundRect(16, 26, 288, 172, 8, 0x0841);
    M5.Lcd.drawRoundRect(16, 26, 288, 172, 8, isFast ? TFT_CYAN : TFT_MAGENTA);
    M5.Lcd.drawRoundRect(17, 27, 286, 170, 7, isFast ? TFT_CYAN : TFT_MAGENTA);

    M5.Lcd.setFreeFont(FSSB9);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextColor(isFast ? TFT_CYAN : TFT_MAGENTA, 0x0841);
    M5.Lcd.drawString("CONFIRM INSULIN LOG", 160, 44);

    M5.Lcd.setFreeFont(FSSB18);
    M5.Lcd.setTextColor(TFT_WHITE, 0x0841);
    char modalDoseBuf[48];
    if (isFast) {
      snprintf(modalDoseBuf, sizeof(modalDoseBuf), "%.1f U Fast Acting", s_fastDose);
    } else {
      snprintf(modalDoseBuf, sizeof(modalDoseBuf), "%.0f U Long Acting", s_longDose);
    }
    M5.Lcd.drawString(modalDoseBuf, 160, 78);

    M5.Lcd.setFreeFont(FSS9);
    M5.Lcd.setTextColor(TFT_LIGHTGREY, 0x0841);
    M5.Lcd.drawString("Send dose to Nightscout?", 160, 108);

    // Confirm and Cancel buttons
    M5.Lcd.setFreeFont(FSSB12);
    drawSimpleBox(28, 130, 124, 52, "✓ CONFIRM", 0x03E0, TFT_GREEN, TFT_WHITE);
    drawSimpleBox(168, 130, 124, 52, "✗ CANCEL", 0x7800, TFT_RED, TFT_WHITE);
  } else if (s_modalState == TREATMENT_MODAL_POSTING) {
    M5.Lcd.fillRoundRect(20, 75, 280, 70, 8, 0x0841);
    M5.Lcd.drawRoundRect(20, 75, 280, 70, 8, TFT_YELLOW);
    M5.Lcd.setFreeFont(FSSB12);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextColor(TFT_YELLOW, 0x0841);
    M5.Lcd.drawString("Posting to Nightscout...", 160, 110);
  }
}

void handleTreatmentsTouch(int16_t x, int16_t y) {
  // If modal is active
  if (s_modalState == TREATMENT_MODAL_CONFIRM) {
    // Confirm button: x: 28..152, y: 130..182
    if (x >= 28 && x <= 152 && y >= 125 && y <= 186) {
      s_modalState = TREATMENT_MODAL_POSTING;
      drawRapidTreatmentsPage();

      String resultMsg;
      bool ok = false;
      if (s_insulinType == INSULIN_FAST) {
        ok = postNightscoutTreatment("Correction Bolus", s_fastDose, 0.0f, "M5 Core2 Fast Bolus", resultMsg);
      } else {
        ok = postNightscoutTreatment("Basal / Long-Acting", s_longDose, 0.0f, "M5 Core2 Long Basal", resultMsg);
      }

      s_lastPostSuccess = ok;
      s_statusMessage = ok ? ("✓ " + resultMsg) : ("✗ " + resultMsg);
      s_statusMsgExpiry = millis() + 3000;
      s_modalState = TREATMENT_MODAL_NONE;
      drawRapidTreatmentsPage();
      return;
    }

    // Cancel button: x: 168..292, y: 130..182, or tap outside
    if ((x >= 168 && x <= 292 && y >= 125 && y <= 186) || y < 26 || y > 198) {
      s_modalState = TREATMENT_MODAL_NONE;
      drawRapidTreatmentsPage();
      return;
    }
    return;
  }

  // 1. Mode Toggle: y: 26..60
  if (y >= 26 && y <= 60) {
    if (x >= 10 && x <= 155) {
      s_insulinType = INSULIN_FAST;
      drawRapidTreatmentsPage();
      return;
    }
    if (x >= 165 && x <= 310) {
      s_insulinType = INSULIN_LONG;
      drawRapidTreatmentsPage();
      return;
    }
  }

  // 2. Stepper & Center Suggestion: y: 66..138
  if (y >= 66 && y <= 138) {
    // Check if tapped Suggestion chip (center lower)
    if (s_insulinType == INSULIN_FAST && x >= 115 && x <= 205 && y >= 104 && y <= 136) {
      s_fastDose = getSuggestedCorrection();
      drawRapidTreatmentsPage();
      return;
    }

    if (s_insulinType == INSULIN_FAST) {
      if (x >= 12 && x <= 60) {
        s_fastDose -= 1.0f;
        if (s_fastDose < 0.5f) s_fastDose = 0.5f;
        s_fastDose = roundf(s_fastDose * 2.0f) / 2.0f;
        drawRapidTreatmentsPage();
        return;
      }
      if (x >= 61 && x <= 108) {
        s_fastDose -= 0.5f;
        if (s_fastDose < 0.5f) s_fastDose = 0.5f;
        s_fastDose = roundf(s_fastDose * 2.0f) / 2.0f;
        drawRapidTreatmentsPage();
        return;
      }
      if (x >= 212 && x <= 259) {
        s_fastDose += 0.5f;
        if (s_fastDose > 50.0f) s_fastDose = 50.0f;
        s_fastDose = roundf(s_fastDose * 2.0f) / 2.0f;
        drawRapidTreatmentsPage();
        return;
      }
      if (x >= 260 && x <= 308) {
        s_fastDose += 1.0f;
        if (s_fastDose > 50.0f) s_fastDose = 50.0f;
        s_fastDose = roundf(s_fastDose * 2.0f) / 2.0f;
        drawRapidTreatmentsPage();
        return;
      }
    } else {
      if (x >= 12 && x <= 60) {
        s_longDose -= 5.0f;
        if (s_longDose < 1.0f) s_longDose = 1.0f;
        drawRapidTreatmentsPage();
        return;
      }
      if (x >= 61 && x <= 108) {
        s_longDose -= 1.0f;
        if (s_longDose < 1.0f) s_longDose = 1.0f;
        drawRapidTreatmentsPage();
        return;
      }
      if (x >= 212 && x <= 259) {
        s_longDose += 1.0f;
        if (s_longDose > 100.0f) s_longDose = 100.0f;
        drawRapidTreatmentsPage();
        return;
      }
      if (x >= 260 && x <= 308) {
        s_longDose += 5.0f;
        if (s_longDose > 100.0f) s_longDose = 100.0f;
        drawRapidTreatmentsPage();
        return;
      }
    }
  }

  // 3. Full-Width Action Button: y: 142..198
  if (y >= 142 && y <= 198) {
    s_modalState = TREATMENT_MODAL_CONFIRM;
    s_modalTimeout = millis() + 10000;
    drawRapidTreatmentsPage();
    return;
  }

  // 4. Standard Bottom Navigation Bar: y >= 200
  if (y >= 200) {
    if (x < 106) {
      cycleBrightness();
    } else if (x < 213) {
      triggerSnooze();
    } else {
      cyclePage();
    }
  }
}
