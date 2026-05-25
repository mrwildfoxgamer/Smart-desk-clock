#ifndef CONFIG_H
#define CONFIG_H

// ── Wi-Fi & Time ──────────────────────────────────────────────
#define WIFI_SSID           "bla"
#define WIFI_PASSWORD       "12345678"
#define GMT_OFFSET_SEC      19800
#define DAYLIGHT_OFFSET_SEC 0

// ── API Keys & Endpoints ──────────────────────────────────────
#define LFM_API_KEY         "840ae36f4924ea3f06fffabdc29b9765"
#define LFM_USERNAME        "Mr_Wild_Fox"

#define WX_API_KEY          "b71c929961d0409b9b0e3c6197f87595"
#define WX_LAT              "9.9312"
#define WX_LON              "76.2673"

// ── Hardware Pins ─────────────────────────────────────────────
#define TFT_CS              5
#define TFT_DC              1
#define TFT_RST             0
#define TFT_MOSI            7
#define TFT_SCLK            10
#define BTN_PIN             4

// ── Screen & Arc Geometry ─────────────────────────────────────
#define SW                  320
#define SH                  240
#define ARC_CX              160
#define ARC_CY              312
#define ARC_R               195
#define ARC_A_START         210.0f
#define ARC_A_END           330.0f
#define DAY_START           6.0f
#define DAY_END             18.0f

// ── Timings & Intervals (in milliseconds) ─────────────────────
#define DEBOUNCE_MS         50
#define DOUBLE_PRESS_MS     400
#define AUTO_ROTATE_MS      (10UL * 60UL * 1000UL)
#define WX_REFRESH_MS       (50UL * 60UL * 1000UL)
#define LFM_REFRESH_MS      (15UL * 1000UL)
#define BMO_FRAME_MS        40 // ~25 fps animation

// ── BMO Mood Enum ─────────────────────────────────────────────
enum BmoMood {
  BMO_NEUTRAL = 0,
  BMO_HAPPY,
  BMO_SURPRISED,
  BMO_CHILL,
  BMO_SAD,
  BMO_DARK
};

#endif // CONFIG_H