#include "Display.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_AHTX0.h>
#include <Wire.h>
#include <math.h>

// ── Hardware Instances ────────────────────────────────────────
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
Adafruit_AHTX0 aht;
bool ahtConnected = false;

// ── Page & Global State ───────────────────────────────────────
int currentPage = 0;
bool autoRotate = true;
bool fullRedrawNeeded = true;
uint32_t lastAutoRotateMs = 0; 

// ── Button State ──────────────────────────────────────────────
static uint32_t btnPressTime    = 0;
static uint32_t btnReleaseTime  = 0;
static bool     lastBtnState    = HIGH;
static int      pressCount      = 0;

static void safeCopy(char* dst, size_t dstSize, const char* src) {
  if (!dst || dstSize == 0) return;
  if (!src) src = "";
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = '\0';
}

// ── BMO State ─────────────────────────────────────────────────
// bmoFace  – redrawn every frame (face body + animated features)
// bmoBgCache – pre-rendered static face body, copied into bmoFace each frame
//              so we never re-draw the body geometry per-frame
GFXcanvas16* bmoFace    = nullptr;
GFXcanvas16* bmoBgCache = nullptr;   // pre-baked static face body

static bool      bmoActive          = false;
static BmoMood   bmoMood            = BMO_NEUTRAL;
static uint32_t  bmoAnimFrame       = 0;
static uint32_t  bmoLastDraw        = 0;
static char      currentBmoTrack[48]  = "";
static char      currentBmoArtist[40] = "";
static bool      bmoNeedsFullRedraw   = true;

// Returns true (and updates cache) only when track+artist differ from last shown
bool bmoTrackChanged(const char* track, const char* artist) {
  if (strcmp(track, currentBmoTrack) != 0 || strcmp(artist, currentBmoArtist) != 0)
    return true;
  return false;
}

// Canvas / geometry constants
#define BMO_CV_W        240
#define BMO_CV_H        100
#define BMO_CV_X        (160 - (BMO_CV_W / 2))
#define BMO_CV_Y        (120 - (BMO_CV_H / 2))
#define CV_CX           (BMO_CV_W / 2)
#define CV_CY           (BMO_CV_H / 2)
#define BMO_EYE_OFFSET_X 45
#define BMO_EYE_Y        35
#define BMO_MOUTH_Y      60

// ── Static face-body geometry (drawn once into bmoBgCache) ────
// Body outline
#define BMO_BODY_X      10
#define BMO_BODY_Y       5
#define BMO_BODY_W     (BMO_CV_W - 20)
#define BMO_BODY_H     (BMO_CV_H - 10)
#define BMO_BODY_R      12

// Screen bezel (the dark rounded rect inside BMO's face)
#define BMO_SCR_X       30
#define BMO_SCR_Y       10
#define BMO_SCR_W      (BMO_CV_W - 60)
#define BMO_SCR_H      (BMO_CV_H - 22)
#define BMO_SCR_R        8

// ── Star Coordinates for Clock ────────────────────────────────
static const uint16_t SX[] = {12,30,52,78,102,122,150,172,198,220,242,268,288,308,18,42,68,94,118,144,168,194,218,246,274,300,25,60,100,140,180,220,262,298};
static const uint8_t  SY[] = {18,8,30,14,38,6,24,40,12,30,20,9,34,16,48,28,52,18,44,32,20,45,25,38,14,48,55,22,50,12,40,30,55,20};
#define NUM_STARS 34

// ── Caches (for flicker-free redraws) ─────────────────────────
static int   p_clockMin = -1, p_clockSec = -1;
static int   prevBx = -1, prevBy = -1;
static float s_prevTempC    = -999.0f;
static float s_prevHumidity = -999.0f;

// ─────────────────────────────────────────────────────────────
// SENSOR & DISPLAY INIT
// ─────────────────────────────────────────────────────────────
void InitSensors() {
  Wire.begin(8, 9, 100000);
  if (aht.begin(&Wire)) {
    ahtConnected = true;
    Serial.println("[HW] AHT21 found!");
  } else {
    ahtConnected = false;
  }
}

void InitDisplay() {
  pinMode(BTN_PIN, INPUT_PULLUP);
  tft.init(240, 320);
  tft.setRotation(1);
  tft.invertDisplay(false); // Fixes the inverted colors issue!
  tft.fillScreen(C_BLACK);
  tft.setTextColor(C_ACCENT);
  tft.setTextSize(2);
  tft.setCursor(52, 105);
  tft.print("Booting System...");
  lastAutoRotateMs = millis();
}

void resetPageState() {
  fullRedrawNeeded  = true;
  prevBx = prevBy   = -1;
  p_clockMin = p_clockSec = -1;
  s_prevTempC = s_prevHumidity = -999.0f;
}

// ─────────────────────────────────────────────────────────────
// BUTTON HANDLER
// ─────────────────────────────────────────────────────────────
void handleButton() {
  bool state = digitalRead(BTN_PIN);
  uint32_t now = millis();

  if (state == LOW && lastBtnState == HIGH && (now - btnReleaseTime > DEBOUNCE_MS)) {
    btnPressTime = now;
  }
  if (state == HIGH && lastBtnState == LOW && (now - btnPressTime > DEBOUNCE_MS)) {
    btnReleaseTime = now;
    pressCount++;
  }
  lastBtnState = state;

  if (pressCount > 0 && (now - btnReleaseTime > DOUBLE_PRESS_MS)) {
    if (bmoActive) {
      dismissBmo();
    } else if (pressCount == 1) {
      currentPage = (currentPage + 1) % 4;
      resetPageState();
    } else if (pressCount >= 2) {
      autoRotate = !autoRotate;
      lastAutoRotateMs = now;
      fullRedrawNeeded = true;
    }
    pressCount = 0;
  }
}

// ─────────────────────────────────────────────────────────────
// UI HELPERS
// ─────────────────────────────────────────────────────────────
float toRad(float d) { return d * 3.14159265f / 180.0f; }

static void drawDots() {
  int dy = SH-8, sp = 16;
  int x0 = SW/2 - (3 * sp) / 2;
  for (int i = 0; i < 4; i++) {
    int x = x0 + i*sp;
    if (i == currentPage) tft.fillCircle(x, dy, 4, C_ACCENT);
    else {
      tft.fillCircle(x, dy, 3, C_BLACK);
      tft.drawCircle(x, dy, 3, 0x4208);
    }
  }
  int playX = x0 + 4 * sp - 4;
  if (autoRotate) tft.fillTriangle(playX, dy-4, playX, dy+4, playX+6, dy, C_ACCENT);
  else tft.fillRect(playX, dy-5, 8, 11, C_BLACK);
}

void truncate(char* dst, const char* src, int maxLen) {
  strncpy(dst, src, maxLen - 1);
  dst[maxLen - 1] = '\0';
  if ((int)strlen(src) >= maxLen) {
    dst[maxLen - 4] = '.'; dst[maxLen - 3] = '.'; dst[maxLen - 2] = '.'; dst[maxLen - 1] = '\0';
  }
}

void drawSyncingScreen() {
  tft.fillScreen(C_BLACK);
  tft.setTextColor(C_ACCENT);
  tft.setTextSize(2);
  tft.setCursor(52, 100);
  tft.print("Syncing Time...");
}

// ─────────────────────────────────────────────────────────────
// BMO ANIMATION OVERLAY
// ─────────────────────────────────────────────────────────────
bool isBmoActive() { return bmoActive; }

// ── Pre-bake the static face body into bmoBgCache ─────────────
static void bmoBuildBodyCache() {
  if (!bmoBgCache) return;
  bmoBgCache->fillScreen(C_BMO_FACE);

  // Outer body rounded rect
  bmoBgCache->fillRoundRect(BMO_BODY_X, BMO_BODY_Y, BMO_BODY_W, BMO_BODY_H, BMO_BODY_R, C_BMO_MGREEN);
  bmoBgCache->fillRoundRect(BMO_BODY_X + 2, BMO_BODY_Y + 2, BMO_BODY_W - 4, BMO_BODY_H - 4, BMO_BODY_R - 1, C_BMO_FACE);

  // Screen bezel (dark surround)
  bmoBgCache->fillRoundRect(BMO_SCR_X - 3, BMO_SCR_Y - 3, BMO_SCR_W + 6, BMO_SCR_H + 6, BMO_SCR_R + 2, C_BMO_OUTLINE);
  // Screen interior (face colour — eyes/mouth drawn on top each frame)
  bmoBgCache->fillRoundRect(BMO_SCR_X, BMO_SCR_Y, BMO_SCR_W, BMO_SCR_H, BMO_SCR_R, C_BMO_FACE);

  // Blush dots
  bmoBgCache->fillCircle(CV_CX - BMO_EYE_OFFSET_X - 12, BMO_EYE_Y + 10, 4, C_BMO_BLUSH);
  bmoBgCache->fillCircle(CV_CX + BMO_EYE_OFFSET_X + 12, BMO_EYE_Y + 10, 4, C_BMO_BLUSH);

  // Teeth hint (small white line below mouth area — drawn once)
  bmoBgCache->fillRoundRect(CV_CX - 8, BMO_MOUTH_Y + 8, 16, 4, 2, C_BMO_TOOTH);
}

void activateBmo(BmoMood mood, const char* track, const char* artist) {
  if (!bmoActive) {
    bmoFace    = new GFXcanvas16(BMO_CV_W, BMO_CV_H);
    bmoBgCache = new GFXcanvas16(BMO_CV_W, BMO_CV_H);
    bmoBuildBodyCache();
    bmoActive = true;
  } else if (mood != bmoMood) {
    // Mood changed: rebuild body cache (blush/teeth colours unchanged but be safe)
    bmoBuildBodyCache();
  }
  bmoMood = mood;
  safeCopy(currentBmoTrack,  sizeof(currentBmoTrack),  track);
  safeCopy(currentBmoArtist, sizeof(currentBmoArtist), artist);
  bmoNeedsFullRedraw = true;
  bmoAnimFrame       = 0;
}

void dismissBmo() {
  if (bmoActive) {
    bmoActive = false;
    delete bmoFace;    bmoFace    = nullptr;
    delete bmoBgCache; bmoBgCache = nullptr;
    resetPageState();
  }
}

void drawCanvasCurve(int cx, int cy, int width, int height, bool smile, uint16_t color) {
  for (float tt = 0; tt <= 1.0f; tt += 0.02f) {
    float x = (1-tt)*(1-tt)*(cx - width) + 2*(1-tt)*tt*cx + tt*tt*(cx + width);
    float y = (1-tt)*(1-tt)*cy + 2*(1-tt)*tt*(cy + (smile ? height : -height)) + tt*tt*cy;
    bmoFace->fillCircle((int)x, (int)y, 1, color);
  }
}

void bmoDrawEyes(int offsetX, int offsetY) {
  bmoFace->fillCircle(CV_CX - BMO_EYE_OFFSET_X + offsetX, BMO_EYE_Y + offsetY, 6, C_BMO_INK);
  bmoFace->fillCircle(CV_CX + BMO_EYE_OFFSET_X + offsetX, BMO_EYE_Y + offsetY, 6, C_BMO_INK);
}
void bmoDrawClosedEyes(int offsetX, int offsetY) {
  drawCanvasCurve(CV_CX - BMO_EYE_OFFSET_X + offsetX, BMO_EYE_Y + offsetY + 2, 8, -6, false, C_BMO_INK);
  drawCanvasCurve(CV_CX + BMO_EYE_OFFSET_X + offsetX, BMO_EYE_Y + offsetY + 2, 8, -6, false, C_BMO_INK);
}

void UpdateBmoAnimation() {
  if (!bmoActive || !bmoFace || !bmoBgCache) return;
  uint32_t now = millis();
  if (now - bmoLastDraw < BMO_FRAME_MS) return;
  bmoLastDraw = now;

  // ── One-shot: paint the static full-screen background ─────────
  // Track/artist text and the teal fill never change; only draw them
  // when activateBmo() triggers bmoNeedsFullRedraw.
  if (bmoNeedsFullRedraw) {
    bmoNeedsFullRedraw = false;

    // Paint only the regions OUTSIDE the canvas — never the full screen.
    // Canvas occupies x:BMO_CV_X..BMO_CV_X+BMO_CV_W, y:BMO_CV_Y..BMO_CV_Y+BMO_CV_H
    // Top strip
    tft.fillRect(0, 0, SW, BMO_CV_Y, C_BMO_FACE);
    // Bottom strip
    tft.fillRect(0, BMO_CV_Y + BMO_CV_H, SW, SH - (BMO_CV_Y + BMO_CV_H), C_BMO_FACE);
    // Left strip (canvas height only)
    tft.fillRect(0, BMO_CV_Y, BMO_CV_X, BMO_CV_H, C_BMO_FACE);
    // Right strip (canvas height only)
    tft.fillRect(BMO_CV_X + BMO_CV_W, BMO_CV_Y, SW - (BMO_CV_X + BMO_CV_W), BMO_CV_H, C_BMO_FACE);

    // Track name (bottom area, below the canvas)
    tft.setTextSize(1);
    tft.setTextColor(C_BMO_OUTLINE, C_BMO_FACE);
    char trackLine[48]; truncate(trackLine, currentBmoTrack, 40);
    int16_t x1, y1; uint16_t tw, th;
    tft.getTextBounds(trackLine, 0, 0, &x1, &y1, &tw, &th);
    // Clear + draw track
    tft.fillRect(0, SH - 26, SW, 12, C_BMO_FACE);
    tft.setCursor((SW - (int)tw) / 2, SH - 24);
    tft.print(trackLine);

    // Artist name
    char artLine[40]; truncate(artLine, currentBmoArtist, 36);
    tft.getTextBounds(artLine, 0, 0, &x1, &y1, &tw, &th);
    tft.fillRect(0, SH - 14, SW, 12, C_BMO_FACE);
    tft.setCursor((SW - (int)tw) / 2, SH - 13);
    tft.print(artLine);

    // Decorative border dots
    for (int i = 0; i < 5; i++) {
      tft.fillCircle(20 + i * 14, BMO_CV_Y - 10, 3, C_BMO_MGREEN);
      tft.fillCircle(20 + i * 14, BMO_CV_Y + BMO_CV_H + 10, 3, C_BMO_MGREEN);
    }
  }

  // ── Per-frame: copy pre-baked body into face canvas ────────────
  // memcpy is fast: BMO_CV_W*BMO_CV_H*2 = 48 000 bytes (~one SPI burst)
  memcpy(bmoFace->getBuffer(),
         bmoBgCache->getBuffer(),
         (size_t)BMO_CV_W * BMO_CV_H * sizeof(uint16_t));

  // ── Compute animation parameters for this frame ────────────────
  bmoAnimFrame++;
  float tf = (float)bmoAnimFrame;
  int   bobOffset = 0, lookX = 0;
  bool  isBlinking = false, isSinging = false;
  bool  isSquinting = false;   // chill half-closed eyes
  bool  isWide = false;        // surprised wide eyes

  switch (bmoMood) {
    case BMO_NEUTRAL:
      bobOffset  = (int)(sinf(tf * 0.12f) * 2.0f);
      lookX      = (int)(sinf(tf * 0.05f) * 4.0f);
      isBlinking = (bmoAnimFrame % 65 < 3);
      break;

    case BMO_HAPPY:
      bobOffset  = (int)(sinf(tf * 0.40f) * 5.0f);
      lookX      = (int)(sinf(tf * 0.12f) * 6.0f);
      isBlinking = (bmoAnimFrame % 48 < 3);
      isSinging  = (bmoAnimFrame % 12 < 6);
      break;

    case BMO_SURPRISED:
      // Fast nervous jitter
      bobOffset  = (bmoAnimFrame % 6 < 3) ? 2 : -2;
      lookX      = (int)(sinf(tf * 0.5f) * 3.0f);
      isWide     = true;
      isBlinking = (bmoAnimFrame % 80 < 2);   // rare blink
      isSinging  = (bmoAnimFrame % 16 < 4);   // stuttery open mouth
      break;

    case BMO_CHILL:
      bobOffset   = (int)(sinf(tf * 0.04f) * 3.0f);
      lookX       = (int)(sinf(tf * 0.025f) * 2.0f);
      isSquinting = true;   // always half-closed
      break;

    case BMO_SAD:
      // Slow heavy bob, downward look drift
      bobOffset  = (int)(sinf(tf * 0.07f) * 2.5f) + 2;
      lookX      = (int)(sinf(tf * 0.04f) * 2.0f);
      isBlinking = (bmoAnimFrame % 90 < 3);
      break;

    case BMO_DARK:
      // Slow pulse, slight sway, rare blink
      bobOffset  = (int)(sinf(tf * 0.06f) * 1.5f);
      lookX      = (int)(sinf(tf * 0.03f) * 3.0f);
      isBlinking = (bmoAnimFrame % 120 < 2);
      break;

    default:
      bobOffset  = (int)(sinf(tf * 0.12f) * 2.0f);
      break;
  }

  // ── Draw eyes ─────────────────────────────────────────────────
  if (isSquinting) {
    // Chill: half-closed — draw a filled rect over the top half of each eye circle
    bmoFace->fillCircle(CV_CX - BMO_EYE_OFFSET_X + lookX, BMO_EYE_Y + bobOffset, 6, C_BMO_INK);
    bmoFace->fillCircle(CV_CX + BMO_EYE_OFFSET_X + lookX, BMO_EYE_Y + bobOffset, 6, C_BMO_INK);
    // Cover upper half to create squint
    bmoFace->fillRect(CV_CX - BMO_EYE_OFFSET_X + lookX - 7,
                      BMO_EYE_Y + bobOffset - 8, 14, 8, C_BMO_FACE);
    bmoFace->fillRect(CV_CX + BMO_EYE_OFFSET_X + lookX - 7,
                      BMO_EYE_Y + bobOffset - 8, 14, 8, C_BMO_FACE);
  } else if (isBlinking) {
    bmoDrawClosedEyes(lookX, bobOffset);
  } else if (isWide) {
    // Surprised: larger pupils
    bmoFace->fillCircle(CV_CX - BMO_EYE_OFFSET_X + lookX, BMO_EYE_Y + bobOffset, 8, C_BMO_INK);
    bmoFace->fillCircle(CV_CX + BMO_EYE_OFFSET_X + lookX, BMO_EYE_Y + bobOffset, 8, C_BMO_INK);
    // Highlight spec
    bmoFace->fillCircle(CV_CX - BMO_EYE_OFFSET_X + lookX + 3, BMO_EYE_Y + bobOffset - 3, 2, C_BMO_FACE);
    bmoFace->fillCircle(CV_CX + BMO_EYE_OFFSET_X + lookX + 3, BMO_EYE_Y + bobOffset - 3, 2, C_BMO_FACE);
  } else {
    bmoDrawEyes(lookX, bobOffset);
    // Eye highlight
    bmoFace->fillCircle(CV_CX - BMO_EYE_OFFSET_X + lookX + 2, BMO_EYE_Y + bobOffset - 2, 1, C_BMO_MLITE);
    bmoFace->fillCircle(CV_CX + BMO_EYE_OFFSET_X + lookX + 2, BMO_EYE_Y + bobOffset - 2, 1, C_BMO_MLITE);
  }

  // ── Draw mouth ────────────────────────────────────────────────
  if (isSinging) {
    // Open round mouth
    bmoFace->fillRoundRect(CV_CX - 6 + lookX, BMO_MOUTH_Y + bobOffset - 2, 12, 7, 3, C_BMO_INK);
  } else {
    switch (bmoMood) {
      case BMO_HAPPY:
        drawCanvasCurve(CV_CX + lookX, BMO_MOUTH_Y + bobOffset, 12, 7, true, C_BMO_INK);
        break;
      case BMO_SAD:
        // Frown: upside-down smile, shifted down
        drawCanvasCurve(CV_CX + lookX, BMO_MOUTH_Y + bobOffset + 6, 10, 6, false, C_BMO_INK);
        break;
      case BMO_DARK:
        // Thin flat line
        bmoFace->drawLine(CV_CX - 8 + lookX, BMO_MOUTH_Y + bobOffset + 2,
                          CV_CX + 8 + lookX, BMO_MOUTH_Y + bobOffset + 2, C_BMO_INK);
        break;
      case BMO_SURPRISED:
        // Small 'O'
        bmoFace->drawCircle(CV_CX + lookX, BMO_MOUTH_Y + bobOffset + 2, 5, C_BMO_INK);
        break;
      case BMO_CHILL:
        // Gentle slight smile
        drawCanvasCurve(CV_CX + lookX, BMO_MOUTH_Y + bobOffset, 10, 3, true, C_BMO_INK);
        break;
      default:
        // Neutral straight line
        bmoFace->drawLine(CV_CX - 6 + lookX, BMO_MOUTH_Y + bobOffset,
                          CV_CX + 6 + lookX, BMO_MOUTH_Y + bobOffset, C_BMO_INK);
        break;
    }
  }

  // ── Blit only the face canvas region to screen ─────────────────
  // This is the ONLY tft write per frame: 240×100 px = 48 KB over SPI.
  // The surrounding teal area and text are never touched after the first draw.
  tft.drawRGBBitmap(BMO_CV_X, BMO_CV_Y, bmoFace->getBuffer(), BMO_CV_W, BMO_CV_H);
}

// ─────────────────────────────────────────────────────────────
// PAGE 0: CLOCK & ARC
// ─────────────────────────────────────────────────────────────
void bodyPos(float angleDeg, int &x, int &y) {
  float r = toRad(angleDeg);
  x = ARC_CX + (int)(ARC_R * cosf(r));
  y = ARC_CY + (int)(ARC_R * sinf(r));
}

float timeToAngle(float h) {
  bool isNight = (h < DAY_START || h >= DAY_END);
  if (!isNight) {
    return ARC_A_START + ((h - DAY_START) / (DAY_END - DAY_START)) * (ARC_A_END - ARC_A_START);
  } else {
    float hh = h < DAY_START ? h + 24.0f : h;
    return ARC_A_START + ((hh - DAY_END) / (DAY_START + 24.0f - DAY_END)) * (ARC_A_END - ARC_A_START);
  }
}

void drawClockPage(struct tm &t) {
  bool night = (t.tm_hour < 6 || t.tm_hour >= 18);
  
  if (fullRedrawNeeded) {
    tft.fillScreen(C_BLACK);
    if (night) {
      for (int i = 0; i < NUM_STARS; i++) {
        uint16_t col = (i%3==0)?C_STAR_A:(i%3==1)?C_STAR_B:C_STAR_C;
        tft.drawPixel(SX[i], SY[i], col);
        if (i % 5 == 0) { tft.drawPixel(SX[i]+1, SY[i], col); tft.drawPixel(SX[i], SY[i]+1, col); }
      }
    }
    
    uint16_t arcCol = night ? C_ARC_NIGHT : C_ARC_DAY;
    for (float a = ARC_A_START; a <= ARC_A_END; a += 0.7f) {
      int x, y; bodyPos(a, x, y);
      if (x >= 0 && x < SW && y >= 0 && y < SH) tft.fillCircle(x, y, 4, arcCol);
    }

    tft.drawFastHLine(70, 214, SW-140, 0x2104);
    tft.setTextSize(1); tft.setTextColor(0x4208, C_BLACK);
    tft.setCursor(8, 222); tft.print("06:00");
    tft.setCursor(SW-36, 222); tft.print("18:00");
    drawDots();
    
    const char* WD[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
    const char* MN[] = {"","JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
    char date[24]; sprintf(date, "%s  %02d %s %d", WD[t.tm_wday], t.tm_mday, MN[t.tm_mon+1], t.tm_year+1900);
    int16_t x1, y1; uint16_t tw, th;
    tft.getTextBounds(date, 0, 0, &x1, &y1, &tw, &th);
    tft.setTextColor(C_LABEL, C_BLACK);
    tft.setCursor((SW-(int)tw)/2, 204);
    tft.print(date);
    
    fullRedrawNeeded = false;
    p_clockMin = p_clockSec = -1;
  }

  // Track where the HH:MM block ends so seconds can follow without overlap
  static int s_timeTextEndX = 0;

  if (t.tm_min != p_clockMin) {
    p_clockMin = t.tm_min;
    char buf[6]; sprintf(buf, "%02d:%02d", t.tm_hour, t.tm_min);
    int16_t x1, y1; uint16_t tw, th;
    tft.setTextSize(5); tft.getTextBounds(buf, 0, 0, &x1, &y1, &tw, &th);
    int tx = (SW-(int)tw)/2;
    // Clear wide enough area to cover old HH:MM + old :SS + AM/PM
    tft.fillRect(0, 18, SW, 46, C_BLACK);
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(tx, 20); tft.print(buf);
    s_timeTextEndX = tx + (int)tw;
    // AM/PM sits above the seconds, to the right of HH:MM
    tft.setTextSize(2); tft.setTextColor(C_ACCENT, C_BLACK);
    tft.setCursor(s_timeTextEndX + 4, 20); tft.print(t.tm_hour < 12 ? "AM" : "PM");
    // Force seconds redraw since we cleared the area
    p_clockSec = -1;
  }

  if (t.tm_sec != p_clockSec) {
    p_clockSec = t.tm_sec;
    char sec[4]; sprintf(sec, ":%02d", t.tm_sec);
    // Clear only the seconds area (below AM/PM)
    tft.fillRect(s_timeTextEndX, 38, 50, 22, C_BLACK);
    tft.setTextSize(2); tft.setTextColor(C_LABEL, C_BLACK);
    tft.setCursor(s_timeTextEndX + 4, 40); tft.print(sec);

    float fh  = t.tm_hour + t.tm_min/60.0f + t.tm_sec/3600.0f;
    int bx, by; bodyPos(timeToAngle(fh), bx, by);
    
    if (bx != prevBx || by != prevBy) {
      if (prevBx >= 0) { 
        int r = night ? 22 : 38;
        tft.fillRect(prevBx-r, prevBy-r, r*2, r*2, C_BLACK);
        uint16_t arcCol = night ? C_ARC_NIGHT : C_ARC_DAY;
        for (float a = ARC_A_START; a <= ARC_A_END; a += 0.7f) {
          int ax, ay; bodyPos(a, ax, ay);
          if (ax >= prevBx-r && ax <= prevBx+r && ay >= prevBy-r && ay <= prevBy+r)
            tft.fillCircle(ax, ay, 4, arcCol);
        }
      }
      if (!night) {
        tft.fillCircle(bx, by, 14, 0xFA00);
        tft.fillCircle(bx, by, 11, C_SUN_MID);
        tft.fillCircle(bx, by, 7, C_SUN_CORE);
      } else { 
        tft.fillCircle(bx, by, 18, 0x1A3F);
        tft.fillCircle(bx, by, 11, C_MOON_BODY);
        tft.fillCircle(bx+5, by-2, 9, C_BLACK); 
      }
      prevBx = bx; prevBy = by;
    }
  }
}

// ─────────────────────────────────────────────────────────────
// PAGE 1: SENSOR PAGE
// ─────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────
// PAGE 1: SENSOR PAGE
// ─────────────────────────────────────────────────────────────
// ── Helper: Calculate Heat Index (Celsius) ────────────────────
float calculateHeatIndex(float tempC, float humidity) {
  // Convert Celsius to Fahrenheit for the standard formula
  float tF = (tempC * 9.0f / 5.0f) + 32.0f;
  
  // Simple formula (works best for lower temperatures)
  float hiF = 0.5f * (tF + 61.0f + ((tF - 68.0f) * 1.2f) + (humidity * 0.094f));

  // If the simple Heat Index is 80°F or higher, use the full Rothfusz equation
  if (hiF > 79.0f) {
    hiF = -42.379f + 2.04901523f * tF + 10.14333127f * humidity 
          - 0.22475541f * tF * humidity - 0.00683783f * tF * tF 
          - 0.05481717f * humidity * humidity + 0.00122874f * tF * tF * humidity 
          + 0.00085282f * tF * humidity * humidity - 0.00000199f * tF * tF * humidity * humidity;
  }
  
  // Convert back to Celsius
  return (hiF - 32.0f) * 5.0f / 9.0f;
}
// ─────────────────────────────────────────────────────────────
void drawSensorPage() {
  // Static state to remember things between frames
  static uint32_t lastSensorRead = 0;
  static sensors_event_t he, te; 
  static bool isOnline = false;
  static int lastDrawnOnline = -1; // -1 means it hasn't been drawn yet

  bool dataChanged = false;

  // 1. Poll sensor every 2 seconds, or immediately if we have no baseline
  if (millis() - lastSensorRead >= 2000 || s_prevTempC == -999.0f) {
    sensors_event_t temp_he, temp_te;
    
    // getEvent() actually returns true if the sensor responds successfully over I2C
    if (aht.getEvent(&temp_he, &temp_te)) {
      isOnline = true;
      
      // SAFETY CHECK: Only process if data is sane
      if (temp_te.temperature >= -20.0f && temp_te.temperature <= 80.0f) {
        he = temp_he;
        te = temp_te;
        
        dataChanged = (fabsf(te.temperature - s_prevTempC) >= 0.1f ||
                       fabsf(he.relative_humidity - s_prevHumidity) >= 0.5f);
                       
        if (dataChanged) {
          s_prevTempC    = te.temperature;
          s_prevHumidity = he.relative_humidity;
        }
      }
    } else {
      // Sensor failed to respond
      isOnline = false;
    }
    lastSensorRead = millis();
  }

  // If absolutely nothing needs updating (no data change, no dot color change, no swipe), exit early!
  if (!dataChanged && !fullRedrawNeeded && isOnline == lastDrawnOnline) return;

  // We need this flag because a full page swipe requires us to redraw the text too
  bool needsTextRedraw = dataChanged;

  // 2. Full Background Redraw (Only triggers when swiping pages)
  if (fullRedrawNeeded) {
    tft.fillScreen(C_BLACK);
    tft.fillRect(0, 0, SW, 28, 0x0842);
    tft.setTextSize(1); tft.setTextColor(C_LABEL, 0x0842);
    tft.setCursor(8, 10);  tft.print("INDOOR SENSOR");
    tft.drawFastHLine(8, 112, SW-16, 0x2104);
    tft.drawFastVLine(SW/2, 116, 100, 0x2104);
    tft.setTextSize(1); tft.setTextColor(C_LABEL, C_BLACK);
    tft.setCursor(20, 122); tft.print("DEW POINT");
    tft.setCursor(SW/2 + 20, 122); tft.print("ABS HUMIDITY");
    drawDots();
    
    fullRedrawNeeded = false;
    lastDrawnOnline = -1; // Force the status dot to redraw on this new background

    // If we have cached sensor data, force the text to draw over the new background
    if (s_prevTempC != -999.0f) {
      needsTextRedraw = true;
    }
  }

  // 3. INDEPENDENT STATUS DOT REDRAW
  if (isOnline != lastDrawnOnline) {
    uint16_t dotColor = isOnline ? 0x07E0 : 0xF800; // Green for online, Red for offline
    
    // Clear the exact spot first (matches the 0x0842 header background color)
    tft.fillCircle(SW - 16, 14, 6, 0x0842);
    // Draw the actual status dot
    tft.fillCircle(SW - 16, 14, 4, dotColor);
    
    lastDrawnOnline = isOnline;
  }

  // 4. Data Text Redraw (Skips if data hasn't changed to prevent flickering)
  if (!needsTextRedraw) return;

  // ── Temperature: value + comfort label ───────────────────────
  tft.fillRect(14, 36, 200, 42, C_BLACK);
  char tBuf[8]; dtostrf(te.temperature, 4, 1, tBuf);
  
  tft.setTextSize(5); tft.setTextColor(C_WHITE, C_BLACK);
  tft.setCursor(14, 36); tft.print(tBuf);
  tft.setTextSize(2); tft.print("\xF7""C");   

  // Comfort label logic
  const char* tempLabel;
  uint16_t    tempLabelColor;
  if      (te.temperature < 16.0f) { tempLabel = "COLD";        tempLabelColor = 0x03FF; } 
  else if (te.temperature < 26.0f) { tempLabel = "COMFORTABLE"; tempLabelColor = 0x07E0; } 
  else if (te.temperature < 29.0f) { tempLabel = "WARM";        tempLabelColor = 0xFD20; } 
  else                             { tempLabel = "HOT";         tempLabelColor = 0xF800; } 

  // Heat Index UI
  float heatIndex = calculateHeatIndex(te.temperature, he.relative_humidity);
  char hiBuf[8];
  dtostrf(heatIndex, 4, 1, hiBuf);

  tft.fillRect(14, 82, 200, 14, C_BLACK);
  tft.setTextSize(1); 
  tft.setTextColor(tempLabelColor, C_BLACK);
  tft.setCursor(14, 82); 
  tft.print("FEELS "); 
  tft.print(hiBuf); 
  tft.print("\xF7""C ("); 
  tft.print(tempLabel); 
  tft.print(")");

  // ── Humidity: value + comfort label ──────────────────────────
  tft.fillRect(SW/2 + 10, 36, SW/2 - 24, 72, C_BLACK);
  char hBuf[8]; snprintf(hBuf, sizeof(hBuf), "%d%%", (int)he.relative_humidity);
  tft.setTextSize(4); tft.setTextColor(0x07E0, C_BLACK);
  tft.setCursor(SW/2 + 10, 42); tft.print(hBuf);

  const char* humLabel;
  uint16_t    humLabelColor;
  if      (he.relative_humidity < 30.0f) { humLabel = "DRY";       humLabelColor = 0xde47; }
  else if (he.relative_humidity < 60.0f) { humLabel = "IDEAL";     humLabelColor = 0x0795; }
  else if (he.relative_humidity < 75.0f) { humLabel = "HUMID";     humLabelColor = 0x0334; }
  else                                   { humLabel = "VERY HUMID";humLabelColor = 0x001f; }
  tft.setTextSize(1); tft.setTextColor(humLabelColor, C_BLACK);
  tft.setCursor(SW/2 + 10, 82); tft.print(humLabel);

  // ── Dew Point ─────────────────────────────────────────────────
  float gamma   = (17.62f * te.temperature) / (243.12f + te.temperature) + logf(he.relative_humidity / 100.0f);
  float dewPoint= (243.12f * gamma) / (17.62f - gamma);
  char dpBuf[8]; dtostrf(dewPoint, 4, 1, dpBuf);
  tft.fillRect(20, 134, SW/2 - 28, 28, C_BLACK);
  tft.setTextSize(3); tft.setTextColor(0x965e, C_BLACK);
  tft.setCursor(20, 134); tft.print(dpBuf); tft.setTextSize(1); tft.print("\xF7""C");

  // ── Absolute Humidity ─────────────────────────────────────────
  float absHum = (6.112f * expf((17.67f * te.temperature)/(te.temperature + 243.5f)) * he.relative_humidity * 2.1674f) / (273.15f + te.temperature);
  char ahBuf[10]; dtostrf(absHum, 4, 1, ahBuf); strcat(ahBuf, "g/m3");
  tft.fillRect(SW/2 + 20, 134, SW/2 - 28, 28, C_BLACK);
  tft.setTextSize(2); tft.setTextColor(0x07fe, C_BLACK);
  tft.setCursor(SW/2 + 20, 134); tft.print(ahBuf);
}

// ─────────────────────────────────────────────────────────────
// PAGE 2: LAST.FM
// ─────────────────────────────────────────────────────────────
void drawVinyl(int cx, int cy, int r, uint16_t col) {
  tft.fillCircle(cx, cy, r, 0x2104);
  tft.drawCircle(cx, cy, r-2, 0x3186);
  tft.drawCircle(cx, cy, r-9, 0x3186);
  tft.fillCircle(cx, cy, r-16, col);
  tft.fillCircle(cx, cy, 3, C_BLACK);
}

void drawPlayingBars(int x, int y, uint16_t col) {
  uint32_t t = millis() / 200;
  int h1 = 4 + (t % 3) * 3; int h2 = 4 + ((t + 1) % 3) * 3; int h3 = 4 + ((t + 2) % 3) * 3;
  tft.fillRect(x, y + (10 - h1), 3, h1, col);
  tft.fillRect(x+5, y + (10 - h2), 3, h2, col);
  tft.fillRect(x+10, y + (10 - h3), 3, h3, col);
}

void drawLastFmPage(LastFmData* data) {
  if (!data->needsRedraw && !fullRedrawNeeded) {
    if (data->nowPlaying) {
      tft.fillRect(90, 100, 20, 12, C_BLACK);
      drawPlayingBars(90, 100, C_LFM_RED);
    }
    return;
  }

  tft.fillScreen(C_BLACK);
  tft.fillRect(0, 0, SW, 28, 0x2000);
  tft.setTextSize(1); tft.setTextColor(C_LFM_RED, 0x2000);
  tft.setCursor(8, 10);  tft.print("LAST.FM");

  if (data->valid) {
    drawVinyl(44, 69, 35, data->nowPlaying ? C_LFM_RED : 0x4208);
    tft.setTextSize(2); tft.setTextColor(C_WHITE, C_BLACK);
    char track[20]; strncpy(track, data->track, 19); track[19] = '\0';
    tft.setCursor(90, 42); tft.print(track);
    tft.setTextSize(1); tft.setTextColor(C_LFM_PINK, C_BLACK);
    tft.setCursor(90, 76); tft.print("by ");
    tft.setTextColor(C_LFM_RED, C_BLACK); tft.print(data->artist);
    
    tft.drawFastHLine(8, 110, SW - 16, 0x2104);
    tft.setTextColor(C_LABEL, C_BLACK); tft.setCursor(10, 118); tft.print("YOUR PLAYS");
    tft.setTextSize(2); tft.setTextColor(C_LFM_PINK, C_BLACK);
    tft.setCursor(10, 130); tft.print(data->userPlaycount); tft.print(" scrobbles");
  } else {
    tft.setTextColor(C_LABEL); tft.setTextSize(2);
    tft.setCursor(30, 100); tft.print("Fetching...");
  }
  
  drawDots();
  fullRedrawNeeded  = false;
  data->needsRedraw = false;
}

// Helper: compass direction from degrees
static const char* windDirStr(int deg) {
  const char* dirs[] = {"N","NE","E","SE","S","SW","W","NW"};
  return dirs[((deg + 22) % 360) / 45];
}

void drawWeatherPage(WeatherData* data) {
  if (!data->needsRedraw && !fullRedrawNeeded) return;

  tft.fillScreen(C_BLACK);
  tft.fillRect(0, 0, SW, 28, 0x0842);
  tft.setTextSize(1); tft.setTextColor(C_LABEL, 0x0842);
  tft.setCursor(8, 10);  tft.print("WEATHER");

  if (data->valid) {
    // ── Big temperature with °C ───────────────────────────────
    char tBuf[10]; dtostrf(data->temp, 4, 1, tBuf);
    tft.setTextSize(5); tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(14, 36); tft.print(tBuf);
    tft.setTextSize(2); tft.print("\xF7""C");   // degree-C

    // Feels-like on the right side of header area
    tft.setTextSize(1); tft.setTextColor(C_LABEL, C_BLACK);
    tft.setCursor(200, 38);  tft.print("Feels");
    tft.setCursor(200, 48);  tft.print("like");
    char flBuf[8]; dtostrf(data->feelsLike, 3, 1, flBuf);
    tft.setTextSize(2); tft.setTextColor(0xC618, C_BLACK);
    tft.setCursor(196, 58);  tft.print(flBuf);

    // Condition description
    tft.setTextSize(1); tft.setTextColor(C_ACCENT, C_BLACK);
    tft.setCursor(14, 82); tft.print(data->description);

    // ── Divider ───────────────────────────────────────────────
    tft.drawFastHLine(8, 96, SW-16, 0x2104);

    // ── Row 1: Humidity | Wind ────────────────────────────────
    // Humidity
    tft.setTextSize(1); tft.setTextColor(C_LABEL, C_BLACK);
    tft.setCursor(10, 102); tft.print("HUMIDITY");
    char hBuf[8]; snprintf(hBuf, sizeof(hBuf), "%d%%", data->humidity);
    tft.setTextSize(2); tft.setTextColor(0x07FF, C_BLACK);
    tft.setCursor(10, 112); tft.print(hBuf);

    // Wind speed + direction
    tft.setTextSize(1); tft.setTextColor(C_LABEL, C_BLACK);
    tft.setCursor(100, 102); tft.print("WIND");
    char wBuf[16]; snprintf(wBuf, sizeof(wBuf), "%.0f km/h %s", data->windSpeed, windDirStr(data->windDir));
    tft.setTextSize(2); tft.setTextColor(0xFDA0, C_BLACK);
    tft.setCursor(100, 112); tft.print(wBuf);

    // ── Divider ───────────────────────────────────────────────
    tft.drawFastHLine(8, 132, SW-16, 0x2104);

    // ── Row 2: UV Index | Pressure ────────────────────────────
    tft.setTextSize(1); tft.setTextColor(C_LABEL, C_BLACK);
    tft.setCursor(10, 138); tft.print("UV INDEX");
    // UV label
    const char* uvLabel;
    uint16_t    uvCol;
    if      (data->uvIndex < 3.0f)  { uvLabel = "LOW";      uvCol = 0x07E0; }
    else if (data->uvIndex < 6.0f)  { uvLabel = "MODERATE"; uvCol = 0xFFE0; }
    else if (data->uvIndex < 8.0f)  { uvLabel = "HIGH";     uvCol = 0xFD20; }
    else if (data->uvIndex < 11.0f) { uvLabel = "VERY HIGH";uvCol = 0xF800; }
    else                            { uvLabel = "EXTREME";  uvCol = 0xF81F; }
    char uvBuf[6]; dtostrf(data->uvIndex, 3, 1, uvBuf);
    tft.setTextSize(2); tft.setTextColor(uvCol, C_BLACK);
    tft.setCursor(10, 148); tft.print(uvBuf); tft.print(" "); tft.print(uvLabel);

    tft.setTextSize(1); tft.setTextColor(C_LABEL, C_BLACK);
    tft.setCursor(200, 138); tft.print("PRES.");
    char pBuf[8]; snprintf(pBuf, sizeof(pBuf), "%d", data->pressure);
    tft.setTextSize(2); tft.setTextColor(0xC618, C_BLACK);
    tft.setCursor(200, 148); tft.print(pBuf);
    tft.setTextSize(1); tft.setTextColor(C_LABEL, C_BLACK); tft.print("hPa");

    // ── Divider ───────────────────────────────────────────────
    tft.drawFastHLine(8, 168, SW-16, 0x2104);

    // ── Row 3: Rain chance | Rain amount ─────────────────────
    tft.setTextSize(1); tft.setTextColor(C_LABEL, C_BLACK);
    tft.setCursor(10, 174); tft.print("RAIN CHANCE");
    char rpBuf[8]; snprintf(rpBuf, sizeof(rpBuf), "%d%%", data->precipProb);
    tft.setTextSize(2); tft.setTextColor(0x039F, C_BLACK);   // rain blue
    tft.setCursor(10, 184); tft.print(rpBuf);

    tft.setTextSize(1); tft.setTextColor(C_LABEL, C_BLACK);
    tft.setCursor(130, 174); tft.print("RAIN NOW");
    char prBuf[10]; dtostrf(data->precip, 4, 1, prBuf); strcat(prBuf, " mm");
    tft.setTextSize(2); tft.setTextColor(0x039F, C_BLACK);
    tft.setCursor(130, 184); tft.print(prBuf);

  } else {
    tft.setTextColor(C_LABEL); tft.setTextSize(2);
    tft.setCursor(30, 100); tft.print("Fetching...");
  }

  drawDots();
  fullRedrawNeeded  = false;
  data->needsRedraw = false;
}