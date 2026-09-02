#include <M5GFX.h>
#if defined ( SDL_h_ )

#define M5 display
static M5GFX display;

#include "../../../Free_Fonts.h"
#include "../../../iot_iconset_16x16.c"

// Simulator state
static int currentPage = 0; // Default to Page 0
static int activeSubtype = 0; // 0: Fast Acting, 1: Long Acting
static float fastDose = 1.0f;
static float longDose = 18.0f;

void drawIcon(int16_t x, int16_t y, const uint8_t *bitmap, uint16_t color) {
  display.drawBitmap(x, y, 16, 16, bitmap, color);
}

static void drawSimpleBox(int x, int y, int w, int h, const char* label, uint16_t bg, uint16_t border, uint16_t textCol, bool isActive = false) {
  display.fillRoundRect(x, y, w, h, 5, bg);
  display.drawRoundRect(x, y, w, h, 5, border);
  if (isActive) {
    display.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 4, border);
  }
  display.setTextColor(textCol, bg);
  display.setTextDatum(MC_DATUM);
  display.drawString(label, x + w / 2, y + h / 2);
}

void drawBottomBar() {
  display.fillRect(0, 218, 320, 22, BLACK);
  drawIcon(45, 220, (const uint8_t*)sun_icon16x16, TFT_LIGHTGREY);
  drawIcon(150, 220, (const uint8_t*)clock_icon16x16, TFT_LIGHTGREY);
  drawIcon(256, 220, (const uint8_t*)door_icon16x16, TFT_LIGHTGREY);
}

void drawArrow(int x, int y, int asize, int aangle, int pwidth, int plength, uint16_t color) {
  float dx = (asize - 10) * cos((aangle - 90) * M_PI / 180.0) + x;
  float dy = (asize - 10) * sin((aangle - 90) * M_PI / 180.0) + y;
  float x1 = 0;         float y1 = plength;
  float x2 = pwidth / 2.0;  float y2 = pwidth / 2.0;
  float x3 = -pwidth / 2.0; float y3 = pwidth / 2.0;
  float angle = (aangle * M_PI / 180.0) - (135.0 * M_PI / 180.0);
  float xx1 = x1 * cos(angle) - y1 * sin(angle) + dx;
  float yy1 = y1 * cos(angle) + x1 * sin(angle) + dy;
  float xx2 = x2 * cos(angle) - y2 * sin(angle) + dx;
  float yy2 = y2 * cos(angle) + x2 * sin(angle) + dy;
  float xx3 = x3 * cos(angle) - y3 * sin(angle) + dx;
  float yy3 = y3 * cos(angle) + x3 * sin(angle) + dy;
  display.fillTriangle(xx1, yy1, xx3, yy3, xx2, yy2, color);
  for (int d = -2; d <= 2; d++) {
    display.drawLine(x + d, y, xx1 + d, yy1, color);
    display.drawLine(x, y + d, xx1, yy1 + d, color);
  }
}

void drawMiniGraph() {
  display.drawLine(231, 113, 319, 113, TFT_LIGHTGREY);
  display.drawLine(231, 203, 319, 203, TFT_LIGHTGREY);
  display.drawLine(231, 200 - (4 - 3) * 10 + 3, 319, 200 - (4 - 3) * 10 + 3, TFT_LIGHTGREY);
  display.drawLine(231, 200 - (9 - 3) * 10 + 3, 319, 200 - (9 - 3) * 10 + 3, TFT_LIGHTGREY);
  
  float last10[10] = { 6.8, 7.0, 7.1, 7.3, 7.4, 7.6, 7.7, 7.8, 8.0, 8.0 };
  for (int i = 9; i >= 0; i--) {
    float glk = last10[9 - i];
    uint16_t sgvColor = TFT_GREEN;
    display.fillCircle(234 + i * 9, 203 - (glk - 3.0) * 10.0, 3, sgvColor);
  }
}

// -------------------------------------------------------------
// PAGE 0: MAIN DASHBOARD (100% FAITHFUL TO REAL FIRMWARE)
// -------------------------------------------------------------
void drawPage0_Dashboard() {
  display.fillScreen(BLACK);

  // 1. Header & Date (03:00A 09/02)
  display.setFreeFont(FSSB12);
  display.setTextSize(1);
  display.setTextDatum(TL_DATUM);
  display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display.drawString("03:00A 09/02", 0, 0);

  // 2. User Name (David)
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.drawString("David", 0, 24);

  // 3. Big Delta (+7)
  display.setFreeFont(FSSB24);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.drawString("+7", 0, 58);

  // 4. White Minutes Badge (1 min)
  display.fillRoundRect(200, 0, 120, 90, 15, TFT_WHITE);
  display.setFreeFont(FSSB24);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(TFT_BLACK, TFT_WHITE);
  display.drawString("1", 260, 32);

  display.setFreeFont(FSSB12);
  display.drawString("min", 260, 70);

  // 5. Giant SGV (144)
  display.setFreeFont(FSSB24);
  display.setTextSize(2);
  display.setTextDatum(TL_DATUM);
  display.setTextColor(TFT_GREEN, TFT_BLACK);
  display.drawString("144", 0, 120);

  // 6. Direction Arrow (↗ FortyFiveUp)
  drawArrow(170, 160, 10, 45 + 85, 40, 40, TFT_GREEN);

  // 7. Mini 3-line trend graph on the right
  drawMiniGraph();

  // 8. Bottom bezel icons
  drawBottomBar();
}

// -------------------------------------------------------------
// PAGE 5: RAPID TREATMENTS (100% IDENTICAL TO M5NSTreatments.cpp)
// -------------------------------------------------------------
void drawPage5_Treatments() {
  display.fillScreen(BLACK);
  
  // Header
  display.drawLine(0, 22, 319, 22, TFT_DARKGREY);
  display.setFreeFont(FSSB9);
  display.setTextSize(1);
  display.setTextDatum(ML_DATUM);
  display.setTextColor(TFT_CYAN, BLACK);
  display.drawString("LOG INSULIN", 8, 11);

  display.setTextDatum(MC_DATUM);
  display.setTextColor(TFT_GREEN, BLACK);
  display.drawString("144 FortyFiveUp", 160, 11);

  display.setTextDatum(MR_DATUM);
  display.setTextColor(TFT_LIGHTGREY, BLACK);
  display.drawString("03:00 AM", 312, 11);

  // Tabs (y: 28..58)
  bool isFast = (activeSubtype == 0);
  uint16_t fastBg = isFast ? 0x0217 : 0x0841;
  uint16_t fastBorder = isFast ? TFT_CYAN : 0x2104;
  uint16_t fastText = isFast ? TFT_WHITE : TFT_LIGHTGREY;
  drawSimpleBox(10, 28, 145, 30, "Fast Acting", fastBg, fastBorder, fastText, isFast);

  uint16_t longBg = !isFast ? 0x4010 : 0x0841;
  uint16_t longBorder = !isFast ? TFT_MAGENTA : 0x2104;
  uint16_t longText = !isFast ? TFT_WHITE : TFT_LIGHTGREY;
  drawSimpleBox(165, 28, 145, 30, "Long Acting", longBg, longBorder, longText, !isFast);

  // Stepper Card (y: 66..136)
  display.fillRoundRect(10, 66, 300, 70, 8, 0x0841);
  display.drawRoundRect(10, 66, 300, 70, 8, 0x2104);

  // Stepper Buttons
  if (isFast) {
    drawSimpleBox(15, 72, 44, 58, "-1.0", 0x18C3, 0x39E7, TFT_WHITE);
    drawSimpleBox(63, 72, 44, 58, "-0.5", 0x18C3, 0x39E7, TFT_WHITE);
    drawSimpleBox(213, 72, 44, 58, "+0.5", 0x18C3, 0x39E7, TFT_WHITE);
    drawSimpleBox(261, 72, 44, 58, "+1.0", 0x18C3, 0x39E7, TFT_WHITE);
  } else {
    drawSimpleBox(15, 72, 44, 58, "-5", 0x18C3, 0x39E7, TFT_WHITE);
    drawSimpleBox(63, 72, 44, 58, "-1", 0x18C3, 0x39E7, TFT_WHITE);
    drawSimpleBox(213, 72, 44, 58, "+1", 0x18C3, 0x39E7, TFT_WHITE);
    drawSimpleBox(261, 72, 44, 58, "+5", 0x18C3, 0x39E7, TFT_WHITE);
  }

  // Large Center Dose
  display.setFreeFont(FSSB18);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(isFast ? TFT_CYAN : TFT_MAGENTA, 0x0841);
  char doseStr[16];
  if (isFast) {
    snprintf(doseStr, sizeof(doseStr), "%.1f U", fastDose);
  } else {
    snprintf(doseStr, sizeof(doseStr), "%.0f U", longDose);
  }
  display.drawString(doseStr, 160, isFast ? 86 : 101);

  if (isFast) {
    display.setFreeFont(FSSB9);
    drawSimpleBox(128, 108, 64, 22, "~1.0U", 0x0200, 0x0400, TFT_GREEN, false);
  }

  // Single Action Button (y: 146..204)
  char actionLabel[48];
  if (isFast) {
    snprintf(actionLabel, sizeof(actionLabel), "LOG %.1f U FAST ACTING", fastDose);
  } else {
    snprintf(actionLabel, sizeof(actionLabel), "LOG %.0f U LONG ACTING", longDose);
  }

  uint16_t actBg = isFast ? 0x0217 : 0x4010;
  uint16_t actBorder = isFast ? TFT_CYAN : TFT_MAGENTA;
  display.fillRoundRect(10, 146, 300, 58, 8, actBg);
  display.drawRoundRect(10, 146, 300, 58, 8, actBorder);
  display.drawRoundRect(11, 147, 298, 56, 7, actBorder);
  display.setTextColor(TFT_WHITE, actBg);
  display.setFreeFont(FSSB9);
  display.setTextDatum(MC_DATUM);
  display.drawString(actionLabel, 160, 175);

  drawBottomBar();
}

void renderCurrentPage() {
  switch (currentPage) {
    case 0: drawPage0_Dashboard(); break;
    case 5: drawPage5_Treatments(); break;
    default: drawPage0_Dashboard(); break;
  }
}

void setup(void) {
  display.init();
  display.setRotation(1);
  renderCurrentPage();
}

void loop(void) {
  int32_t x, y;
  if (display.getTouch(&x, &y)) {
    // Bottom Bezel Touch (y >= 215)
    if (y >= 215) {
      if (x <= 106) {
        currentPage = (currentPage == 0) ? 5 : 0;
        renderCurrentPage();
      } else if (x >= 107 && x <= 213) {
        currentPage = 0;
        renderCurrentPage();
      } else if (x >= 214) {
        currentPage = (currentPage == 0) ? 5 : 0;
        renderCurrentPage();
      }
      SDL_Delay(180);
      return;
    }

    if (currentPage == 5) {
      // Tabs (y: 28..58)
      if (y >= 28 && y <= 58) {
        if (x >= 10 && x <= 155 && activeSubtype != 0) {
          activeSubtype = 0;
          renderCurrentPage();
        } else if (x >= 165 && x <= 310 && activeSubtype != 1) {
          activeSubtype = 1;
          renderCurrentPage();
        }
      }
      // Steppers (y: 66..136)
      else if (y >= 66 && y <= 136) {
        if (x >= 15 && x <= 59) {
          if (activeSubtype == 0) { fastDose = std::max(0.0f, fastDose - 1.0f); }
          else { longDose = std::max(0.0f, longDose - 5.0f); }
          renderCurrentPage();
        } else if (x >= 63 && x <= 107) {
          if (activeSubtype == 0) { fastDose = std::max(0.0f, fastDose - 0.5f); }
          else { longDose = std::max(0.0f, longDose - 1.0f); }
          renderCurrentPage();
        } else if (x >= 213 && x <= 257) {
          if (activeSubtype == 0) { fastDose = std::min(50.0f, fastDose + 0.5f); }
          else { longDose = std::min(100.0f, longDose + 1.0f); }
          renderCurrentPage();
        } else if (x >= 261 && x <= 305) {
          if (activeSubtype == 0) { fastDose = std::min(50.0f, fastDose + 1.0f); }
          else { longDose = std::min(100.0f, longDose + 5.0f); }
          renderCurrentPage();
        }
      }
      // Action Button (y: 146..204)
      else if (y >= 146 && y <= 204 && x >= 10 && x <= 310) {
        display.fillRoundRect(10, 146, 300, 58, 8, 0x03E0);
        display.drawRoundRect(10, 146, 300, 58, 8, TFT_GREEN);
        display.setTextColor(TFT_WHITE, 0x03E0);
        display.setFreeFont(FSSB9);
        display.setTextDatum(MC_DATUM);
        display.drawString("TREATMENT LOGGED OK", 160, 175);
        SDL_Delay(500);
        renderCurrentPage();
      }
    } else {
      if (y < 215) {
        currentPage = (currentPage == 0) ? 5 : 0;
        renderCurrentPage();
      }
    }
    SDL_Delay(120);
  }
  SDL_Delay(16);
}

int user_func(bool* running) {
  setup();
  do {
    loop();
  } while (*running);
  return 0;
}

int main(int, char**) {
  return lgfx::Panel_sdl::main(user_func, 16);
}

#endif
