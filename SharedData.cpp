#include "SharedData.h"

WeatherData wxData = {0};
LastFmData lfmData = {0};
SemaphoreHandle_t weatherMutex = NULL;
SemaphoreHandle_t lfmMutex = NULL;
