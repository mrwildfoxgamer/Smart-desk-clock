#include "NetworkTasks.h"
#include "SharedData.h"
#include "config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <string.h>
#include <ctype.h>

static void safeCopy(char* dst, size_t dstSize, const char* src) {
  if (!dst || dstSize == 0) return;
  if (!src) src = "";
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = '\0';
}


// ── Globals Declared in SharedData.h ──────────────────────────
// ── Helper Functions ──────────────────────────────────────────
String urlEncode(const char* msg) {
  const char* hex = "0123456789ABCDEF"; String out = "";
  while (*msg != '\0') {
    if (isalnum(*msg) || *msg=='-' || *msg=='_' || *msg=='.' || *msg=='~') {
      out += *msg;
    } else {
      out += '%'; out += hex[(uint8_t)*msg >> 4]; out += hex[(uint8_t)*msg & 0xf];
    }
    msg++;
  }
  return out;
}

int wbToWmo(int wb) {
  if (wb==800) return 0; if (wb==801||wb==802) return 2; if (wb==803||wb==804) return 3;
  if (wb>=700&&wb<800) return 45; if (wb>=300&&wb<400) return 51;
  if (wb>=500&&wb<600) return 61; if (wb>=600&&wb<700) return 71;
  if (wb>=200&&wb<300) return 95; return 0;
}

BmoMood getMoodFromTrack(const char* track, const char* artist, const char* album) {
  char buf[128];
  snprintf(buf, sizeof(buf), "%s %s %s", track, artist, album);
  for (int i = 0; buf[i]; i++) buf[i] = tolower((unsigned char)buf[i]);

  const char* hypeWords[] = {"hype","party","rave","rage","banger","fire","lit","beast","pump","energy","epic","power","heavy","metal","punk","edm","trap","riot","electric",NULL};
  for (int i = 0; hypeWords[i];  i++) if (strstr(buf, hypeWords[i]))  return BMO_SURPRISED;

  const char* happyWords[] = {"happy","joy","sunshine","smile","dance","summer","fun","love","wonderful","amazing","celebrate","bliss","cheer","pop","disco",NULL};
  for (int i = 0; happyWords[i]; i++) if (strstr(buf, happyWords[i])) return BMO_HAPPY;

  const char* sadWords[] = {"sad","cry","tears","broken","hurt","pain","lost","miss","alone","dark","goodbye","sorry","depression","grief","sorrow","falling apart","dying","emo",NULL};
  for (int i = 0; sadWords[i];   i++) if (strstr(buf, sadWords[i]))   return BMO_SAD;

  const char* chillWords[] = {"chill","relax","sleep","lo-fi","lofi","ambient","drift","mellow","soft","peaceful","calm","night","dreaming","acoustic","indie folk","breeze",NULL};
  for (int i = 0; chillWords[i]; i++) if (strstr(buf, chillWords[i])) return BMO_CHILL;

  const char* darkWords[] = {"dark","shadow","death","blood","evil","black","hate","curse","ghost","void","demon","hell","doom","gloom","nightmare","horror","abyss","gothic",NULL};
  for (int i = 0; darkWords[i];  i++) if (strstr(buf, darkWords[i]))  return BMO_DARK;

  return BMO_NEUTRAL;
}

BmoMood fetchTrackMood(const char* track, const char* artist, const char* album) {
  BmoMood localFallback = getMoodFromTrack(track, artist, album);
  String url = "http://ws.audioscrobbler.com/2.0/?method=track.gettoptags&artist=" + urlEncode(artist) + "&track=" + urlEncode(track) + "&api_key=" LFM_API_KEY "&format=json&autocorrect=1";

  HTTPClient http; http.begin(url); http.setTimeout(4000);
  if (http.GET() != 200) { http.end(); return localFallback; }
  String payload = http.getString(); http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return localFallback;
  JsonArray tags = doc["toptags"]["tag"];
  if (tags.isNull() || tags.size() == 0) return localFallback;

  char tagString[256] = ""; int count = 0;
  for (JsonObject tag : tags) {
    if (count >= 5) break;
    const char* tagName = tag["name"];
    if (tagName) { strcat(tagString, tagName); strcat(tagString, " "); }
    count++;
  }
  for (int i = 0; tagString[i]; i++) tagString[i] = tolower((unsigned char)tagString[i]);

  if (strstr(tagString, "dark") || strstr(tagString, "death") || strstr(tagString, "doom")) return BMO_DARK;
  if (strstr(tagString, "sad") || strstr(tagString, "emo") || strstr(tagString, "melancholy")) return BMO_SAD;
  if (strstr(tagString, "metal") || strstr(tagString, "party") || strstr(tagString, "edm")) return BMO_SURPRISED;
  if (strstr(tagString, "chill") || strstr(tagString, "lo-fi") || strstr(tagString, "ambient")) return BMO_CHILL;
  if (strstr(tagString, "happy") || strstr(tagString, "pop") || strstr(tagString, "indie pop")) return BMO_HAPPY;

  return localFallback;
}

// ── Background Tasks ──────────────────────────────────────────
void WeatherTask(void *pvParameters) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http; http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      http.begin("http://api.weatherbit.io/v2.0/current?lat=" WX_LAT "&lon=" WX_LON "&key=" WX_API_KEY);
      http.setTimeout(8000);
      if (http.GET() == 200) {
        DynamicJsonDocument doc(6144);
        if (!deserializeJson(doc, http.getString())) {
          JsonObject data = doc["data"][0];
          if (xSemaphoreTake(weatherMutex, portMAX_DELAY)) {
            wxData.temp       = data["temp"] | 0.0f;
            wxData.feelsLike  = data["app_temp"] | wxData.temp;
            wxData.humidity   = data["rh"] | 0;
            wxData.windSpeed  = (data["wind_spd"] | 0.0f) * 3.6f;
            wxData.windDir    = data["wind_dir"] | 0;
            wxData.precip     = data["precip"] | 0.0f;
            wxData.pressure   = data["pres"] | 0;
            wxData.uvIndex    = data["uv"] | 0.0f;
            wxData.wmoCode    = wbToWmo(data["weather"]["code"] | 800);
            safeCopy(wxData.description, sizeof(wxData.description), data["weather"]["description"] | "Unknown");
            wxData.valid      = true;
            wxData.needsRedraw = true;
            xSemaphoreGive(weatherMutex);
          }
        }
      }
      http.end();
    }
    vTaskDelay(WX_REFRESH_MS / portTICK_PERIOD_MS);
  }
}

void LastFmTask(void *pvParameters) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http; http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      http.begin("http://ws.audioscrobbler.com/2.0/?method=user.getrecenttracks&user=" LFM_USERNAME "&api_key=" LFM_API_KEY "&format=json&limit=1");
      http.setTimeout(8000);
      if (http.GET() == 200) {
        DynamicJsonDocument doc(8192);
        if (!deserializeJson(doc, http.getString())) {
          JsonObject attr = doc["recenttracks"]["track"][0]["@attr"];
          bool isNowPlaying = (!attr.isNull() && attr["nowplaying"] == "true");

          const char* trackName  = doc["recenttracks"]["track"][0]["name"] | "Unknown";
          const char* artistName = doc["recenttracks"]["track"][0]["artist"]["#text"] | "Unknown";
          const char* albumName  = doc["recenttracks"]["track"][0]["album"]["#text"] | "";
          int newMood = isNowPlaying ? fetchTrackMood(trackName, artistName, albumName) : BMO_NEUTRAL;

          if (xSemaphoreTake(lfmMutex, portMAX_DELAY)) {
            JsonObject am = doc["recenttracks"]["@attr"];
            lfmData.userPlaycount = (!am.isNull() && am.containsKey("total")) ? am["total"].as<int>() : 0;
            JsonObject track = doc["recenttracks"]["track"][0];
            if (!track.isNull()) {
              safeCopy(lfmData.track,  sizeof(lfmData.track),  trackName);
              safeCopy(lfmData.artist, sizeof(lfmData.artist), artistName);
              safeCopy(lfmData.album,  sizeof(lfmData.album),  albumName);
              lfmData.nowPlaying = isNowPlaying;
              lfmData.loved      = (strcmp(track["loved"] | "0", "1") == 0);
              safeCopy(lfmData.date, sizeof(lfmData.date), track["date"]["#text"] | "");
              lfmData.bmoMood    = newMood;
              lfmData.valid      = true;
              lfmData.needsRedraw = true;
            }
            xSemaphoreGive(lfmMutex);
          }
        }
      }
      http.end();
    }
    vTaskDelay(LFM_REFRESH_MS / portTICK_PERIOD_MS);
  }
}

void StartNetworkTasks() {
  weatherMutex = xSemaphoreCreateMutex();
  lfmMutex     = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(WeatherTask, "WeatherTask", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(LastFmTask,  "LastFmTask",  8192, NULL, 1, NULL, 0);
}