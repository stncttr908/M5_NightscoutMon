#include "M5NSTrendHistory.h"
#include "M5NSDevice.h"
#include "externs.h"
#include "Free_Fonts.h"
#include "iot_iconset_16x16.c"
#include <math.h>

static TrendPoint s_history[TREND_HISTORY_CAPACITY];
static int s_count = 0;

static const int s_zoomLevels[] = {3, 6, 12, 24};
static int s_zoomIndex = 1; // Default: 6 Hours

void trendHistoryInit() {
  s_count = 0;
  memset(s_history, 0, sizeof(s_history));
}

void trendHistoryAdd(uint16_t sgvMgDl, time_t t) {
  if (sgvMgDl == 0 || t == 0) return;
  uint32_t ts = (uint32_t)t;

  if (s_count == 0) {
    s_history[0].sgvMgDl = sgvMgDl;
    s_history[0].timestamp = ts;
    s_count = 1;
    return;
  }

  // Fast path: incoming point is newer than latest point in sorted history
  if (ts > s_history[s_count - 1].timestamp) {
    if (s_count < TREND_HISTORY_CAPACITY) {
      s_history[s_count].sgvMgDl = sgvMgDl;
      s_history[s_count].timestamp = ts;
      s_count++;
    } else {
      memmove(&s_history[0], &s_history[1], (TREND_HISTORY_CAPACITY - 1) * sizeof(TrendPoint));
      s_history[TREND_HISTORY_CAPACITY - 1].sgvMgDl = sgvMgDl;
      s_history[TREND_HISTORY_CAPACITY - 1].timestamp = ts;
    }
    return;
  }

  // Incoming point updates latest point
  if (ts == s_history[s_count - 1].timestamp) {
    s_history[s_count - 1].sgvMgDl = sgvMgDl;
    return;
  }

  // Incoming point is older (historical backfill) - binary search
  int low = 0, high = s_count - 1;
  while (low <= high) {
    int mid = low + (high - low) / 2;
    if (s_history[mid].timestamp == ts) {
      s_history[mid].sgvMgDl = sgvMgDl;
      return;
    }
    if (s_history[mid].timestamp < ts) {
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }

  int pos = low;
  if (s_count < TREND_HISTORY_CAPACITY) {
    memmove(&s_history[pos + 1], &s_history[pos], (s_count - pos) * sizeof(TrendPoint));
    s_history[pos].sgvMgDl = sgvMgDl;
    s_history[pos].timestamp = ts;
    s_count++;
  } else if (pos > 0) {
    memmove(&s_history[0], &s_history[1], (pos - 1) * sizeof(TrendPoint));
    s_history[pos - 1].sgvMgDl = sgvMgDl;
    s_history[pos - 1].timestamp = ts;
  }
}

int trendHistoryGetCount() {
  return s_count;
}

int trendHistoryGetPoints(TrendPoint *outBuffer, int maxCount, int hours) {
  if (s_count == 0 || maxCount <= 0 || outBuffer == NULL) return 0;

  uint32_t now = s_history[s_count - 1].timestamp;
  uint32_t cutoff = (now > (uint32_t)(hours * 3600UL)) ? (now - (uint32_t)(hours * 3600UL)) : 0;

  int collected = 0;
  for (int i = 0; i < s_count && collected < maxCount; i++) {
    if (s_history[i].timestamp >= cutoff) {
      outBuffer[collected++] = s_history[i];
    }
  }
  return collected;
}

ClinicalMetrics trendHistoryGetMetrics(int hours) {
  ClinicalMetrics m = {0};
  if (s_count == 0) {
    if (ns.sensSgvMgDl > 0) {
      uint16_t v = (uint16_t)round(ns.sensSgvMgDl);
      m.count = 1;
      m.meanMgDl = v;
      m.meanMmol = v / 18.018f;
      m.sdMgDl = 0.0f;
      m.cvPercent = 0.0f;
      m.gmiPercent = 3.31f + (0.02392f * m.meanMgDl);
      if (v < 54) m.tirVeryLow = 1;
      else if (v < 70) m.tirLow = 1;
      else if (v <= 180) m.tirTarget = 1;
      else if (v <= 250) m.tirHigh = 1;
      else m.tirVeryHigh = 1;
    }
    return m;
  }

  uint32_t now = s_history[s_count - 1].timestamp;
  uint32_t cutoff = (now > (uint32_t)(hours * 3600UL)) ? (now - (uint32_t)(hours * 3600UL)) : 0;

  double sum = 0.0;
  for (int i = 0; i < s_count; i++) {
    if (s_history[i].timestamp < cutoff) continue;
    uint16_t v = s_history[i].sgvMgDl;
    sum += v;
    m.count++;
    if (v < 54) m.tirVeryLow++;
    else if (v < 70) m.tirLow++;
    else if (v <= 180) m.tirTarget++;
    else if (v <= 250) m.tirHigh++;
    else m.tirVeryHigh++;
  }

  if (m.count == 0) return m;

  m.meanMgDl = (float)(sum / m.count);
  m.meanMmol = m.meanMgDl / 18.018f;

  double varSum = 0.0;
  for (int i = 0; i < s_count; i++) {
    if (s_history[i].timestamp < cutoff) continue;
    double diff = s_history[i].sgvMgDl - m.meanMgDl;
    varSum += (diff * diff);
  }
  m.sdMgDl = (float)sqrt(varSum / m.count);

  if (m.meanMgDl > 0.1f) {
    m.cvPercent = (m.sdMgDl / m.meanMgDl) * 100.0f;
  }

  m.gmiPercent = 3.31f + (0.02392f * m.meanMgDl);

  return m;
}

int trendHistoryGetZoomHours() {
  return s_zoomLevels[s_zoomIndex];
}

void trendHistoryCycleZoom() {
  s_zoomIndex = (s_zoomIndex + 1) % 4;
}

static uint16_t getSgvColor(uint16_t sgvMgDl) {
  if (sgvMgDl < 70) return TFT_RED;
  if (sgvMgDl <= 180) return TFT_GREEN;
  if (sgvMgDl <= 250) return TFT_YELLOW;
  return TFT_RED;
}

// ==============================================================================
// Screen 3: Extended Trend Graph Page (Full Screen Multi-Hour Zoom)
// ==============================================================================
void drawExtendedTrendPage() {
  M5.Lcd.fillScreen(BLACK);

  int zoomHours = trendHistoryGetZoomHours();

  // Top header bar (y: 0 - 24)
  M5.Lcd.setTextDatum(TL_DATUM);
  M5.Lcd.setFreeFont(FSSB9);
  M5.Lcd.setTextSize(1);

  // Latest SGV + Direction
  char sgvStr[16];
  if (cfg.show_mgdl) {
    snprintf(sgvStr, sizeof(sgvStr), "%d", (int)round(ns.sensSgvMgDl));
  } else {
    snprintf(sgvStr, sizeof(sgvStr), "%.1f", ns.sensSgv);
  }
  uint16_t curColor = getSgvColor((uint16_t)round(ns.sensSgvMgDl));
  M5.Lcd.setTextColor(curColor, BLACK);
  M5.Lcd.drawString(sgvStr, 4, 4);
  int sgvWidth = M5.Lcd.textWidth(sgvStr);

  // Delta + Arrow
  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextColor(TFT_WHITE, BLACK);
  char deltaBuf[16];
  if (strlen(ns.delta_display) > 0) {
    strlcpy(deltaBuf, ns.delta_display, sizeof(deltaBuf));
  } else if (cfg.show_mgdl) {
    snprintf(deltaBuf, sizeof(deltaBuf), "%+d", ns.delta_mgdl);
  } else {
    snprintf(deltaBuf, sizeof(deltaBuf), "%+.1f", ns.delta_scaled);
  }
  M5.Lcd.drawString(deltaBuf, 8 + sgvWidth, 4);

  // Zoom indicators in center (x: 100 to 220)
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setFreeFont(NULL);
  M5.Lcd.setTextSize(1);
  const int zoomXPositions[4] = {112, 142, 174, 208};
  for (int z = 0; z < 4; z++) {
    int zH = s_zoomLevels[z];
    char zStr[8];
    snprintf(zStr, sizeof(zStr), "%dH", zH);
    int pillX = zoomXPositions[z];
    if (z == s_zoomIndex) {
      M5.Lcd.fillRect(pillX - 12, 3, 24, 16, TFT_BLUE);
      M5.Lcd.setTextColor(TFT_WHITE, TFT_BLUE);
    } else {
      M5.Lcd.setTextColor(TFT_DARKGREY, BLACK);
    }
    M5.Lcd.drawString(zStr, pillX, 11);
  }

  // TIR percentage in header (right side, x=314)
  ClinicalMetrics m = trendHistoryGetMetrics(zoomHours);
  int totalPts = (m.count > 0) ? m.count : 1;
  int tirPct = (int)round(((float)m.tirTarget / totalPts) * 100.0f);
  char tirStr[20];
  snprintf(tirStr, sizeof(tirStr), "TIR:%d%%", tirPct);
  M5.Lcd.setTextDatum(TR_DATUM);
  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextColor(TFT_GREEN, BLACK);
  M5.Lcd.drawString(tirStr, 314, 4);

  // Divider line
  M5.Lcd.drawLine(0, 22, 319, 22, TFT_DARKGREY);

  // Graph Area coordinates (x: 32 to 312, y: 28 to 198)
  const int graphX = 32;
  const int graphW = 280;
  const int graphTopY = 28;
  const int graphBotY = 196;
  const int graphH = graphBotY - graphTopY;

  const float minMgDl = 40.0f;
  const float maxMgDl = 260.0f;

  auto valToY = [&](float mgdl) -> int {
    if (mgdl < minMgDl) mgdl = minMgDl;
    if (mgdl > maxMgDl) mgdl = maxMgDl;
    return graphBotY - (int)(((mgdl - minMgDl) / (maxMgDl - minMgDl)) * graphH);
  };

  int y180 = valToY(180.0f);
  int y70  = valToY(70.0f);
  int y120 = valToY(120.0f);

  // Target range dashed lines
  for (int x = graphX; x < graphX + graphW; x += 4) {
    M5.Lcd.drawPixel(x, y180, TFT_YELLOW);
    M5.Lcd.drawPixel(x, y70, TFT_RED);
  }

  // Y-axis labels
  M5.Lcd.setTextDatum(MR_DATUM);
  M5.Lcd.setFreeFont(NULL); // Fast default font for tiny labels
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(TFT_YELLOW, BLACK);
  M5.Lcd.drawString(cfg.show_mgdl ? "180" : "10.0", graphX - 3, y180);
  M5.Lcd.setTextColor(TFT_RED, BLACK);
  M5.Lcd.drawString(cfg.show_mgdl ? "70" : "3.9", graphX - 3, y70);
  M5.Lcd.setTextColor(TFT_DARKGREY, BLACK);
  M5.Lcd.drawString(cfg.show_mgdl ? "120" : "6.7", graphX - 3, y120);

  // Time grid marks on X axis
  M5.Lcd.setTextDatum(TL_DATUM);
  char tLeft[8], tMid[8];
  snprintf(tLeft, sizeof(tLeft), "-%dh", zoomHours);
  snprintf(tMid, sizeof(tMid), "-%dh", zoomHours / 2);
  M5.Lcd.setTextColor(TFT_DARKGREY, BLACK);
  M5.Lcd.drawString(tLeft, graphX, graphBotY + 2);
  M5.Lcd.setTextDatum(TC_DATUM);
  M5.Lcd.drawString(tMid, graphX + graphW / 2, graphBotY + 2);
  M5.Lcd.setTextDatum(TR_DATUM);
  M5.Lcd.setTextColor(TFT_CYAN, BLACK);
  M5.Lcd.drawString("Now", graphX + graphW, graphBotY + 2);

  // Plot trend points with strictly sorted order & time scaling
  if (s_count > 0) {
    uint32_t latestTime = s_history[s_count - 1].timestamp;
    uint32_t windowStartTime = (latestTime > (uint32_t)(zoomHours * 3600UL)) ? (latestTime - (uint32_t)(zoomHours * 3600UL)) : 0;

    int prevX = -1, prevY = -1;
    int lastDrawnX = -999;

    for (int i = 0; i < s_count; i++) {
      if (s_history[i].timestamp < windowStartTime) continue;

      float fraction = (float)(s_history[i].timestamp - windowStartTime) / (float)(zoomHours * 3600UL);
      if (fraction < 0.0f) fraction = 0.0f;
      if (fraction > 1.0f) fraction = 1.0f;

      int curX = graphX + (int)(fraction * graphW);
      int curY = valToY(s_history[i].sgvMgDl);
      uint16_t ptColor = getSgvColor(s_history[i].sgvMgDl);

      if (prevX >= 0) {
        M5.Lcd.drawLine(prevX, prevY, curX, curY, TFT_CYAN);
      }
      if (curX - lastDrawnX >= 3 || i == s_count - 1) {
        M5.Lcd.fillCircle(curX, curY, (i == s_count - 1) ? 3 : 2, ptColor);
        lastDrawnX = curX;
      }
      prevX = curX;
      prevY = curY;
    }
  } else {
    int curX = graphX + graphW;
    int curY = valToY(ns.sensSgvMgDl);
    M5.Lcd.fillCircle(curX, curY, 4, curColor);
  }

  // Bottom Touch Bar (y: 216 - 240)
  M5.Lcd.fillRect(0, 216, 320, 24, BLACK);
  M5.Lcd.drawLine(0, 216, 319, 216, TFT_DARKGREY);

  // Left button: ZOOM label
  M5.Lcd.setFreeFont(FSSB9);
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setTextColor(TFT_CYAN, BLACK);
  char zoomLabel[16];
  snprintf(zoomLabel, sizeof(zoomLabel), "ZOOM %dH", zoomHours);
  M5.Lcd.drawString(zoomLabel, 50, 228);

  // Middle button: SNOOZE
  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextColor(TFT_LIGHTGREY, BLACK);
  M5.Lcd.drawString("SNOOZE", 160, 228);

  // Right button: PAGE / EXIT (Door icon)
  drawIcon(256, 220, (uint8_t*)door_icon16x16, TFT_LIGHTGREY);
}

// ==============================================================================
// Screen 4: Clinical AGP & Time-in-Range Statistics Page
// ==============================================================================
void drawClinicalStatsPage() {
  M5.Lcd.fillScreen(BLACK);

  ClinicalMetrics m = trendHistoryGetMetrics(24);

  // Top header bar (y: 0 - 24)
  M5.Lcd.setTextDatum(TL_DATUM);
  M5.Lcd.setFreeFont(FSSB9);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(TFT_WHITE, BLACK);
  M5.Lcd.drawString("GLUCOSE PROFILE (24H)", 6, 4);

  char cntStr[16];
  snprintf(cntStr, sizeof(cntStr), "%d pts", m.count);
  M5.Lcd.setTextDatum(TR_DATUM);
  M5.Lcd.setFreeFont(FSS9);
  M5.Lcd.setTextColor(TFT_GREEN, BLACK);
  M5.Lcd.drawString(cntStr, 314, 4);

  M5.Lcd.drawLine(0, 22, 319, 22, TFT_DARKGREY);

  int total = (m.count > 0) ? m.count : 1;
  int pVHigh  = (int)round(((float)m.tirVeryHigh / total) * 100.0f);
  int pHigh   = (int)round(((float)m.tirHigh / total) * 100.0f);
  int pTarget = (int)round(((float)m.tirTarget / total) * 100.0f);
  int pLow    = (int)round(((float)m.tirLow / total) * 100.0f);
  int pVLow   = 100 - (pVHigh + pHigh + pTarget + pLow);
  if (pVLow < 0) pVLow = 0;

  // Left Box: 5-Tier TIR Breakdown (x: 4 to 156, y: 26 to 176)
  M5.Lcd.drawRoundRect(4, 26, 152, 150, 4, TFT_DARKGREY);
  M5.Lcd.setFreeFont(NULL);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextDatum(TL_DATUM);
  M5.Lcd.setTextColor(TFT_LIGHTGREY, BLACK);
  M5.Lcd.drawString("TIME IN RANGES", 10, 32);

  int rowY = 48;
  auto drawTirRow = [&](const char *label, int pct, uint16_t col) {
    M5.Lcd.setTextColor(col, BLACK);
    M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.drawString(label, 10, rowY);
    char buf[10];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    M5.Lcd.setTextDatum(TR_DATUM);
    M5.Lcd.drawString(buf, 150, rowY);
    rowY += 24;
  };

  drawTirRow(cfg.show_mgdl ? "V.High (>250)" : "V.High (>13.9)", pVHigh, TFT_RED);
  drawTirRow(cfg.show_mgdl ? "High (181-250)" : "High (10.1-13.9)", pHigh, TFT_YELLOW);
  drawTirRow(cfg.show_mgdl ? "Target (70-180)" : "Target (3.9-10.0)", pTarget, TFT_GREEN);
  drawTirRow(cfg.show_mgdl ? "Low (54-69)" : "Low (3.0-3.8)", pLow, TFT_MAGENTA);
  drawTirRow(cfg.show_mgdl ? "V.Low (<54)" : "V.Low (<3.0)", pVLow, TFT_RED);

  // Right Box: Key Summary Metrics (x: 164 to 316, y: 26 to 176)
  M5.Lcd.drawRoundRect(164, 26, 152, 150, 4, TFT_DARKGREY);
  M5.Lcd.setTextColor(TFT_LIGHTGREY, BLACK);
  M5.Lcd.drawString("SUMMARY METRICS", 170, 32);

  int rY = 50;
  auto drawMetricRow = [&](const char *label, const char *val, uint16_t valCol) {
    M5.Lcd.setTextColor(TFT_LIGHTGREY, BLACK);
    M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.drawString(label, 170, rY);
    M5.Lcd.setTextColor(valCol, BLACK);
    M5.Lcd.setTextDatum(TR_DATUM);
    M5.Lcd.drawString(val, 310, rY);
    rY += 28;
  };

  char meanBuf[16], sdBuf[16], cvBuf[16], gmiBuf[16];
  if (cfg.show_mgdl) {
    snprintf(meanBuf, sizeof(meanBuf), "%d mg/dL", (int)round(m.meanMgDl));
    snprintf(sdBuf, sizeof(sdBuf), "%.1f", m.sdMgDl);
  } else {
    snprintf(meanBuf, sizeof(meanBuf), "%.1f mmol/L", m.meanMmol);
    snprintf(sdBuf, sizeof(sdBuf), "%.1f", m.sdMgDl / 18.018f);
  }
  snprintf(cvBuf, sizeof(cvBuf), "%.1f%%", m.cvPercent);
  snprintf(gmiBuf, sizeof(gmiBuf), "%.1f%%", m.gmiPercent);

  drawMetricRow("Mean SGV:", meanBuf, TFT_WHITE);
  drawMetricRow("GMI / eA1C:", gmiBuf, TFT_CYAN);
  drawMetricRow("Std Dev (SD):", sdBuf, TFT_WHITE);
  drawMetricRow("CV (Variability):", cvBuf, (m.cvPercent < 36.0f) ? TFT_GREEN : TFT_YELLOW);

  // Clinical Goals Footer line (y: 184 - 210)
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setFreeFont(NULL);
  M5.Lcd.setTextColor(TFT_DARKGREY, BLACK);
  M5.Lcd.drawString("Targets: TIR > 70%  |  CV < 36%  |  Low < 4%", 160, 195);

  // Bottom Touch Bar (y: 216 - 240)
  M5.Lcd.fillRect(0, 216, 320, 24, BLACK);
  M5.Lcd.drawLine(0, 216, 319, 216, TFT_DARKGREY);

  // Left button: Brightness
  drawIcon(45, 220, (uint8_t*)sun_icon16x16, TFT_LIGHTGREY);

  // Middle button: Snooze
  drawIcon(150, 220, (uint8_t*)clock_icon16x16, TFT_LIGHTGREY);

  // Right button: Page / Exit (Door icon)
  drawIcon(256, 220, (uint8_t*)door_icon16x16, TFT_LIGHTGREY);
}
