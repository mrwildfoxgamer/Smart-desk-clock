#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include <time.h>
#include "SharedData.h"
#include "config.h"

// ── Colour Palette ────────────────────────────────────────────
#define C_BLACK      0x0000
#define C_WHITE      0xFFFF
#define C_SUN_CORE   0xFFE0
#define C_SUN_MID    0xFD20
#define C_SUN_RAY    0xFC00
#define C_MOON_BODY  0xDEFB
#define C_MOON_SHADE 0x738E
#define C_ARC_DAY    0x2124
#define C_ARC_NIGHT  0x1A3F
#define C_LABEL      0x5AEB
#define C_ACCENT     0xFD20
#define C_STAR_A     0xFFFF
#define C_STAR_B     0xBDF7
#define C_STAR_C     0x7BEF
#define C_LFM_RED    0xF000
#define C_LFM_PINK   0xF8B2

// ── BMO Colour Palette ────────────────────────────────────────
#define C_BMO_FACE   0xCF98   
#define C_BMO_EYE    0x0841   
#define C_BMO_OUTLINE 0x10A2  
#define C_BMO_MGREEN 0x2D05   
#define C_BMO_MLITE  0x4DC8   
#define C_BMO_TOOTH  0xEF7D   
#define C_BMO_INK    0x0000   
#define C_BMO_BLUSH  0xFBD7   

// ── Page State ────────────────────────────────────────────────
extern int currentPage;
extern bool autoRotate;
extern bool fullRedrawNeeded;
extern uint32_t lastAutoRotateMs;

// ── Hardware Init ─────────────────────────────────────────────
void InitDisplay();
void InitSensors();

// ── UI Control & Rendering ────────────────────────────────────
void handleButton();
void resetPageState();
void drawSyncingScreen();

// Page Drawers
void drawClockPage(struct tm &t);
void drawSensorPage();
void drawLastFmPage(LastFmData* data);
void drawWeatherPage(WeatherData* data);

// ── BMO Overlay Logic ─────────────────────────────────────────
bool isBmoActive();
bool bmoTrackChanged(const char* track, const char* artist);
void activateBmo(BmoMood mood, const char* track, const char* artist);
void dismissBmo();
void UpdateBmoAnimation();

#endif // DISPLAY_H