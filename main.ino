#include <WiFi.h>
#include <time.h>
#include <sys/time.h>
#include "config.h"
#include "SharedData.h"
#include "NetworkTasks.h"
#include "Display.h"

static bool     wasPlaying       = false;

void setup() {
  Serial.begin(115200);
  delay(500);

  InitDisplay();
  InitSensors();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 20) { 
    delay(500); 
    Serial.print("."); 
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org");
    Serial.println("[WIFI] Connected!");
    delay(800);
  } else {
    Serial.println("[WIFI] Offline mode.");
    struct timeval tv = { .tv_sec = 1748000000 };
    settimeofday(&tv, NULL);
    delay(1000);
  }

  StartNetworkTasks();
  lastAutoRotateMs = millis();
  fullRedrawNeeded = true;
}

void loop() {
  handleButton();

  // Check if we need to launch or update BMO
  if (xSemaphoreTake(lfmMutex, pdMS_TO_TICKS(10))) {
    if (lfmData.valid) {
      if (lfmData.nowPlaying && !wasPlaying) {
        activateBmo((BmoMood)lfmData.bmoMood, lfmData.track, lfmData.artist);
        wasPlaying = true;
      } else if (!lfmData.nowPlaying && wasPlaying) {
        dismissBmo();
        wasPlaying = false;
      } else if (lfmData.nowPlaying && isBmoActive() && lfmData.needsRedraw) {
        // Only reinitialise BMO if the track genuinely changed
        if (bmoTrackChanged(lfmData.track, lfmData.artist)) {
          activateBmo((BmoMood)lfmData.bmoMood, lfmData.track, lfmData.artist);
        }
        lfmData.needsRedraw = false;  // consume the flag either way
      }
    }
    xSemaphoreGive(lfmMutex);
  }

  if (isBmoActive()) {
    UpdateBmoAnimation();
    delay(5);
    return;
  }

  if (autoRotate && (millis() - lastAutoRotateMs > AUTO_ROTATE_MS)) {
    lastAutoRotateMs = millis();
    currentPage = (currentPage + 1) % 4;
    resetPageState();
  }

  struct tm t = {};
  bool timeValid = getLocalTime(&t);

  switch (currentPage) {
    case 0:
      if (timeValid) drawClockPage(t);
      else drawSyncingScreen();
      break;

    case 1:
      drawSensorPage();
      break;

    case 2:
      if (xSemaphoreTake(lfmMutex, pdMS_TO_TICKS(10))) {
        drawLastFmPage(&lfmData);
        xSemaphoreGive(lfmMutex);
      }
      break;

    case 3:
      if (xSemaphoreTake(weatherMutex, pdMS_TO_TICKS(10))) {
        drawWeatherPage(&wxData);
        xSemaphoreGive(weatherMutex);
      }
      break;
  }

  delay(10);
}