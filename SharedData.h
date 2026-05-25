#ifndef SHARED_DATA_H
#define SHARED_DATA_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ── Shared Data Structures ──────────────────────────
struct WeatherData {
  float temp;
  float feelsLike;
  int humidity;
  float windSpeed;
  int windDir;
  float precip;
  int pressure;
  float uvIndex;
  int wmoCode;
  int precipProb;
  char description[24];

  bool valid;
  bool needsRedraw;
};

struct LastFmData {
  char track[48];
  char artist[40];
  char album[40];
  char date[20];
  int userPlaycount;
  bool nowPlaying;
  bool loved;

  int bmoMood;
  bool valid;
  bool needsRedraw;
};

// ── Global Instances & Mutexes ──────────────────────
extern WeatherData wxData;
extern LastFmData lfmData;

extern SemaphoreHandle_t weatherMutex;
extern SemaphoreHandle_t lfmMutex;

#endif