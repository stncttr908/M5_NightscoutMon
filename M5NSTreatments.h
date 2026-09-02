#ifndef _M5NS_TREATMENTS_H_
#define _M5NS_TREATMENTS_H_

#include <Arduino.h>

enum TreatmentModalState {
  TREATMENT_MODAL_NONE = 0,
  TREATMENT_MODAL_CONFIRM,
  TREATMENT_MODAL_POSTING,
  TREATMENT_MODAL_SUCCESS,
  TREATMENT_MODAL_ERROR
};

void initTreatments();
void drawRapidTreatmentsPage();
void handleTreatmentsTouch(int16_t x, int16_t y);
bool isTreatmentsModalActive();
bool postNightscoutTreatment(const char* eventType, float insulin, float carbs, const char* notes, String &outMsg);

#endif // _M5NS_TREATMENTS_H_
