#ifndef _M5NS_TREND_HISTORY_H_
#define _M5NS_TREND_HISTORY_H_

#include <Arduino.h>
#include <time.h>

#define TREND_HISTORY_CAPACITY 1500 // 25 hours at 1-minute intervals (or 5 days at 5-min intervals)

struct TrendPoint {
  uint16_t sgvMgDl;
  uint32_t timestamp;
};


struct ClinicalMetrics {
  float meanMgDl;
  float meanMmol;
  float sdMgDl;
  float cvPercent;
  float gmiPercent;
  int tirVeryLow;   // < 54 mg/dL (< 3.0 mmol/L)
  int tirLow;       // 54-69 mg/dL (3.0-3.8 mmol/L)
  int tirTarget;    // 70-180 mg/dL (3.9-10.0 mmol/L)
  int tirHigh;      // 181-250 mg/dL (10.1-13.9 mmol/L)
  int tirVeryHigh;  // > 250 mg/dL (> 13.9 mmol/L)
  int count;
};

void trendHistoryInit();
void trendHistoryAdd(uint16_t sgvMgDl, time_t t);
int trendHistoryGetCount();
int trendHistoryGetPoints(TrendPoint *outBuffer, int maxCount, int hours);
ClinicalMetrics trendHistoryGetMetrics(int hours);

int trendHistoryGetZoomHours();
void trendHistoryCycleZoom();

void drawExtendedTrendPage();
void drawClinicalStatsPage();

#endif // _M5NS_TREND_HISTORY_H_
