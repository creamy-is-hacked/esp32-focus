#include <Arduino.h>
#include <TFT_eSPI.h>
#include <esp_system.h>
#include <math.h>

// Known-good hardware configuration recovered from the local ESP32 notes.
constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;
constexpr int BACKLIGHT_PIN = 21;
constexpr int TOUCH_CS = 33;
constexpr int TOUCH_IRQ = 36;
constexpr int TOUCH_DIN = 32;
constexpr int TOUCH_MISO = 39;
constexpr int TOUCH_CLK = 25;

// Timer configuration (minutes).
constexpr uint8_t FOCUS_MINUTES = 25;
constexpr uint8_t SHORT_BREAK_MINUTES = 5;
constexpr uint8_t LONG_BREAK_MINUTES = 15;
constexpr uint8_t FOCUSES_PER_CYCLE = 4;
constexpr uint32_t STARTUP_SPLASH_MS = 2400;
constexpr uint32_t SESSION_SPLASH_MS = 1050;
constexpr uint32_t SPLASH_FADE_OUT_MS = 260;
constexpr uint32_t SPLASH_FADE_IN_MS = 520;
constexpr uint32_t COMPLETION_FRAME_MS = 42;
constexpr uint32_t FOCUS_THOUGHT_FRAME_MS = 30;
constexpr uint32_t FOCUS_THOUGHT_TRANSITION_MS = 1050;
constexpr int16_t FOCUS_THOUGHT_CHAR_W = 6;
constexpr int16_t FOCUS_THOUGHT_X = 103;
constexpr int16_t FOCUS_THOUGHT_Y = 63;
constexpr int16_t FOCUS_THOUGHT_W = 114;
constexpr int16_t FOCUS_THOUGHT_H = 14;

#ifdef ESP32_FOCUS_SELF_TEST
constexpr uint32_t TEST_FOCUS_MS = 2000;
constexpr uint32_t TEST_SHORT_BREAK_MS = 1000;
constexpr uint32_t TEST_LONG_BREAK_MS = 1500;
#endif

// Reference-image palette converted to RGB565.
constexpr uint16_t COL_BG = 0x08A2;       // #0F1412
constexpr uint16_t COL_SURFACE = 0x1903;  // #1B231F
constexpr uint16_t COL_PRIMARY = 0x29C6;  // #2D3A33
constexpr uint16_t COL_GREEN = 0xA633;    // #A7C69F
constexpr uint16_t COL_PEACH = 0xFD91;    // #FFB08A
constexpr uint16_t COL_CREAM = 0xE75A;    // #E6EAD2
constexpr uint16_t COL_MUTED = 0x7C6F;    // #7E8D7F
constexpr uint16_t COL_TRACK = 0x31E7;    // subdued ring track
constexpr uint16_t COL_DEEP = 0x1103;     // background depth

constexpr int16_t RING_X = 160;
constexpr int16_t RING_Y = 109;
constexpr int16_t RING_R = 77;
constexpr int16_t RING_IR = 72;
// Font 6 is 27 px wide by 48 px high. These four small redraw regions stay
// comfortably inside the ring's 144 px inner diameter, so a digit update can
// never erase the progress track.
constexpr int16_t DIGIT_Y = 82;
constexpr int16_t DIGIT_W = 28;
constexpr int16_t DIGIT_H = 54;
constexpr int16_t DIGIT_X[4] = {112, 140, 181, 209};

enum class Session : uint8_t { Focus, ShortBreak, LongBreak };
enum class TimerState : uint8_t { Ready, Running, Paused, Complete };

struct Button { int16_t x, y, w, h; };
constexpr Button BTN_RESET{15, 202, 76, 28};
constexpr Button BTN_PRIMARY{105, 197, 110, 38};
constexpr Button BTN_NEXT{229, 202, 76, 28};

TFT_eSPI display;
TFT_eSprite digitSprite(&display);

Session session = Session::Focus;
TimerState timerState = TimerState::Ready;
uint8_t cycleFocuses = 0;
uint32_t accumulatedMs = 0;
uint32_t runStartedAt = 0;
uint32_t completedAt = 0;
uint32_t lastHealthLog = 0;
uint32_t lastCompletionFrame = 0;
uint8_t backlightLevel = 255;

char visibleDigits[4] = {'2', '5', '0', '0'};
char fromDigits[4] = {'2', '5', '0', '0'};
char toDigits[4] = {'2', '5', '0', '0'};
bool digitAnimating[4] = {false, false, false, false};
uint32_t digitAnimationAt = 0;
int32_t lastTimeSecond = -1;

float displayedProgress = 0.0f;
float lastDrawnProgress = 0.0f;
uint32_t lastRingFrame = 0;
int16_t lastKnobX = -1;
int16_t lastKnobY = -1;
int16_t lastArcAngle = -1;
uint16_t lastRingColor = 0;
uint32_t lastDigitFrame = 0;
uint32_t lastPulseFrame = 0;
uint32_t lastBotanicalFrame = 0;
int32_t lastRuntimeSecond = -1;
int16_t fireflyX[4] = {-1, -1, -1, -1};
int16_t fireflyY[4] = {-1, -1, -1, -1};
int16_t backgroundFireflyX[4] = {-1, -1, -1, -1};
int16_t backgroundFireflyY[4] = {-1, -1, -1, -1};
uint8_t lastLaurelCount = 0;
bool startPulseActive = false;
uint32_t startPulseAt = 0;
int8_t lastFocusThought = -1;
int8_t nextFocusThought = -1;
uint32_t lastFocusThoughtFrame = 0;
int32_t lastFocusThoughtCycle = -1;

constexpr const char* FOCUS_THOUGHTS[] = {
  "RUMINATING", "COMBOBULATING", "COGITATING", "MARINATING", "CONSIDERING",
  "PROCESSING", "SYNTHESIZING", "BRAINSTORMING", "IDEATING", "REFLECTING",
  "ANALYZING", "EXPLORING", "CONNECTING", "DISCOVERING", "IMAGINING",
  "CREATING", "FOCUSING", "THINKING", "REASONING", "CONCENTRATING",
  "CALCULATING", "VISUALIZING", "PLANNING", "DEEPENING", "UNRAVELING",
  "FORMULATING", "INVENTING", "BREWING", "SHAPING", "FLOWING"
};
constexpr uint8_t FOCUS_THOUGHT_COUNT = sizeof(FOCUS_THOUGHTS) / sizeof(FOCUS_THOUGHTS[0]);

uint8_t pickFocusThought(int8_t avoid) {
  uint8_t thought = uint8_t(esp_random() % FOCUS_THOUGHT_COUNT);
  if (avoid >= 0 && thought == uint8_t(avoid)) {
    thought = uint8_t((thought + 1 + esp_random() % (FOCUS_THOUGHT_COUNT - 1)) %
                      FOCUS_THOUGHT_COUNT);
  }
  return thought;
}

uint16_t mixColor(uint16_t a, uint16_t b, uint8_t amount) {
  const uint16_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  const uint16_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  const uint16_t r = ar + ((int16_t(br) - ar) * amount) / 255;
  const uint16_t g = ag + ((int16_t(bg) - ag) * amount) / 255;
  const uint16_t bl = ab + ((int16_t(bb) - ab) * amount) / 255;
  return (r << 11) | (g << 5) | bl;
}

uint8_t breathe(uint32_t now, uint32_t period, uint8_t low = 45, uint8_t high = 220) {
  const float phase = float(now % period) / float(period);
  const float wave = 0.5f - 0.5f * cosf(phase * 2.0f * PI);
  return low + uint8_t(wave * (high - low));
}

uint32_t sessionDurationMs() {
#ifdef ESP32_FOCUS_SELF_TEST
  switch (session) {
    case Session::Focus: return TEST_FOCUS_MS;
    case Session::ShortBreak: return TEST_SHORT_BREAK_MS;
    case Session::LongBreak: return TEST_LONG_BREAK_MS;
  }
#else
  switch (session) {
    case Session::Focus: return uint32_t(FOCUS_MINUTES) * 60000UL;
    case Session::ShortBreak: return uint32_t(SHORT_BREAK_MINUTES) * 60000UL;
    case Session::LongBreak: return uint32_t(LONG_BREAK_MINUTES) * 60000UL;
  }
#endif
  return 0;
}

uint16_t baseAccent() {
  if (session == Session::ShortBreak) return COL_PEACH;
  if (session == Session::LongBreak) return COL_CREAM;
  return COL_GREEN;
}

const char* sessionName() {
  switch (session) {
    case Session::Focus: return "FOCUS";
    case Session::ShortBreak: return "SHORT BREAK";
    case Session::LongBreak: return "LONG BREAK";
  }
  return "FOCUS";
}

const char* headerName() {
  return session == Session::Focus ? "FOCUS" : "BREAK";
}

const char* primaryLabel() {
  if (timerState == TimerState::Running) return "PAUSE";
  if (timerState == TimerState::Paused) return "RESUME";
  return "START";
}

uint32_t elapsedMs(uint32_t now) {
  if (timerState == TimerState::Running) return accumulatedMs + (now - runStartedAt);
  if (timerState == TimerState::Complete) return sessionDurationMs();
  return accumulatedMs;
}

float logicalProgress(uint32_t now) {
  const uint32_t duration = sessionDurationMs();
  if (!duration) return 0.0f;
  const uint32_t elapsed = min(elapsedMs(now), duration);
  return float(elapsed) / float(duration);
}

void touchWriteByte(uint8_t command) {
  for (uint8_t i = 0; i < 8; ++i) {
    digitalWrite(TOUCH_CLK, LOW);
    digitalWrite(TOUCH_DIN, (command & 0x80) ? HIGH : LOW);
    command <<= 1;
    delayMicroseconds(3);
    digitalWrite(TOUCH_CLK, HIGH);
    delayMicroseconds(3);
  }
}

uint16_t touchReadAdc(uint8_t command) {
  digitalWrite(TOUCH_CS, LOW);
  delayMicroseconds(1);
  touchWriteByte(command);
  digitalWrite(TOUCH_CLK, LOW);
  delayMicroseconds(5);
  uint16_t value = 0;
  for (uint8_t i = 0; i < 13; ++i) {
    digitalWrite(TOUCH_CLK, LOW);
    delayMicroseconds(3);
    value = (value << 1) | digitalRead(TOUCH_MISO);
    digitalWrite(TOUCH_CLK, HIGH);
    delayMicroseconds(3);
  }
  digitalWrite(TOUCH_CS, HIGH);
  return value;
}

bool readTouch(int16_t& x, int16_t& y) {
  touchReadAdc(0xD0);
  const uint16_t rawX = touchReadAdc(0xD0);
  const uint16_t rawY = touchReadAdc(0x90);
  if (rawY <= 100 || rawY >= 4080 || rawX <= 100 || rawX >= 4000) return false;
  x = constrain(map(constrain(rawY, uint16_t(200), uint16_t(3900)), 200, 3900, 0, SCREEN_W), 0, SCREEN_W - 1);
  y = constrain(map(constrain(rawX, uint16_t(200), uint16_t(3900)), 200, 3900, 0, SCREEN_H), 0, SCREEN_H - 1);
  return true;
}

bool inside(const Button& button, int16_t x, int16_t y) {
  return x >= button.x && x < button.x + button.w && y >= button.y && y < button.y + button.h;
}

void drawLeaf(int16_t x, int16_t y, uint16_t color, uint8_t scale = 1) {
  display.drawLine(x, y + 7 * scale, x + 8 * scale, y - 1 * scale, color);
  display.fillTriangle(x + 3 * scale, y + 2 * scale,
                       x + 12 * scale, y - 3 * scale,
                       x + 9 * scale, y + 7 * scale, color);
  display.drawLine(x + 5 * scale, y + 4 * scale, x + 10 * scale, y, COL_BG);
}

void drawBotanicalSprig(int16_t x, int16_t y, int8_t direction, uint16_t color) {
  int16_t stemX = x;
  int16_t stemY = y;
  for (uint8_t i = 0; i < 4; ++i) {
    const int16_t nextX = stemX + direction * (5 + i);
    const int16_t nextY = stemY - 11;
    display.drawLine(stemX, stemY, nextX, nextY, color);
    const int16_t leafX = nextX + direction * 2;
    const int16_t leafY = nextY + 2;
    display.fillTriangle(leafX, leafY,
                         leafX + direction * 8, leafY - 5,
                         leafX + direction * 7, leafY + 4, color);
    if (i > 0) {
      display.fillTriangle(nextX, nextY + 2,
                           nextX - direction * 7, nextY - 3,
                           nextX - direction * 6, nextY + 6, color);
    }
    stemX = nextX;
    stemY = nextY;
  }
}

uint8_t completionFlash(uint32_t now) {
  // A sharp opening flare followed by a slower heartbeat keeps the completion
  // state impossible to miss without making the display look like a warning
  // beacon. This repeats for as long as the user leaves the phase untouched.
  const uint32_t beat = now % 1700UL;
  if (beat < 240UL) return uint8_t(245UL - (beat * 165UL) / 240UL);
  if (beat < 420UL) return uint8_t(80UL + ((beat - 240UL) * 70UL) / 180UL);
  return breathe(now + 510UL, 1700UL, 45, 155);
}

void drawCompletionPetal(int16_t centerX, int16_t centerY,
                         int16_t tipX, int16_t tipY, uint16_t color) {
  const int16_t dx = tipX - centerX;
  const int16_t dy = tipY - centerY;
  const int16_t midX = centerX + dx / 2;
  const int16_t midY = centerY + dy / 2;
  const int16_t perpendicularX = -dy / 5;
  const int16_t perpendicularY = dx / 5;
  display.fillTriangle(tipX, tipY,
                       midX + perpendicularX, midY + perpendicularY,
                       midX - perpendicularX, midY - perpendicularY, color);
}

void drawCompletionBloom(uint32_t now) {
  constexpr int16_t BLOOM_X = 108;
  constexpr int16_t BLOOM_Y = 78;
  constexpr int16_t BLOOM_W = 104;
  constexpr int16_t BLOOM_H = 52;
  const uint16_t accent = baseAccent();
  const uint8_t flash = completionFlash(now);

  // Only this small opaque patch is refreshed. The card, text, and side
  // foliage remain untouched, avoiding a large SPI transfer every frame.
  display.fillRect(BLOOM_X, BLOOM_Y, BLOOM_W, BLOOM_H, COL_SURFACE);

  // A rotating eight-petal bloom and its flare create the persistent visual
  // signal while retaining the same quiet botanical language as the timer.
  const float rotation = float(now % 3600UL) / 3600.0f * 2.0f * PI;
  const int16_t bloomRadius = 28 + int16_t(flash / 13);
  const uint16_t shimmer = mixColor(COL_SURFACE, COL_PEACH, flash);
  for (uint8_t i = 0; i < 8; ++i) {
    const float angle = rotation + float(i) * PI / 4.0f;
    const int16_t tipX = 160 + int16_t(roundf(cosf(angle) * bloomRadius));
    const int16_t tipY = 105 + int16_t(roundf(sinf(angle) * bloomRadius * 0.48f));
    drawCompletionPetal(160, 105, tipX, tipY,
                        i % 2 == 0 ? shimmer : mixColor(COL_SURFACE, accent, flash));
  }
  display.fillCircle(160, 105, 4 + flash / 100, shimmer);
  const uint8_t rayAlpha = 55 + flash / 2;
  const int16_t rayLength = 43 + int16_t(flash / 8);
  for (uint8_t i = 0; i < 8; ++i) {
    const float angle = float(i) * PI / 4.0f;
    display.drawLine(160 + int16_t(roundf(cosf(angle) * 35.0f)),
                     105 + int16_t(roundf(sinf(angle) * 18.0f)),
                     160 + int16_t(roundf(cosf(angle) * rayLength)),
                     105 + int16_t(roundf(sinf(angle) * rayLength * 0.48f)),
                     mixColor(COL_SURFACE, COL_CREAM, rayAlpha));
  }
}

void drawCompletionAlert(uint32_t now, bool force = false) {
  if (!force && now - lastCompletionFrame < COMPLETION_FRAME_MS) return;
  lastCompletionFrame = now;

  constexpr int16_t CARD_X = 38;
  constexpr int16_t CARD_Y = 49;
  constexpr int16_t CARD_W = 244;
  constexpr int16_t CARD_H = 136;
  if (force) {
    const uint16_t accent = baseAccent();
    const uint8_t flash = completionFlash(now);
    const uint16_t edge = mixColor(COL_SURFACE, accent, 145 + flash / 3);
    const uint16_t leaf = mixColor(COL_SURFACE, accent, 125 + flash / 2);

    // This large card is composed once when completion begins. Redrawing it
    // on every animation tick was the source of the visible refresh churn.
    display.fillRoundRect(CARD_X, CARD_Y, CARD_W, CARD_H, 18, COL_SURFACE);
    display.drawRoundRect(CARD_X, CARD_Y, CARD_W, CARD_H, 18, edge);
    display.drawRoundRect(CARD_X + 3, CARD_Y + 3, CARD_W - 6, CARD_H - 6, 15,
                          mixColor(COL_SURFACE, COL_PEACH, flash / 2));

    drawBotanicalSprig(54, 171, 1, leaf);
    drawBotanicalSprig(266, 171, -1, leaf);
    display.fillCircle(57, 137, 2, mixColor(COL_SURFACE, COL_PEACH, 170));
    display.fillCircle(263, 137, 2, mixColor(COL_SURFACE, COL_PEACH, 170));

    display.setTextDatum(MC_DATUM);
    display.setTextColor(COL_PEACH, COL_SURFACE);
    display.drawString("PHASE COMPLETE", RING_X, 71, 1);
    display.setTextColor(COL_CREAM, COL_SURFACE);
    display.drawString(session == Session::Focus ? "FOCUS COMPLETE" : "BREAK COMPLETE",
                       RING_X, 137, 2);
    display.setTextColor(mixColor(COL_SURFACE, COL_CREAM, 185), COL_SURFACE);
    display.drawString("TAP ANYWHERE TO BEGIN NEXT", RING_X, 158, 1);
  }

  drawCompletionBloom(now);
}

void drawAmbientFireflies(uint32_t now) {
  constexpr int16_t X[7] = {49, 270, 289, 22, 70, 250, 303};
  constexpr int16_t Y[7] = {106, 91, 133, 126, 66, 57, 111};
  for (uint8_t i = 0; i < 7; ++i) {
    const uint8_t glow = breathe(now + i * 730UL, 2200 + i * 410UL, 42, 210);
    display.drawPixel(X[i], Y[i], mixColor(COL_BG, COL_PEACH, glow));
  }
}

void setBacklight(uint8_t level) {
  backlightLevel = level;
  analogWrite(BACKLIGHT_PIN, level);
}

void fadeBacklightTo(uint8_t target, uint32_t duration) {
  const uint8_t start = backlightLevel;
  const uint32_t startedAt = millis();
  while (true) {
    const uint32_t age = millis() - startedAt;
    const float linear = duration == 0 ? 1.0f : min(1.0f, float(age) / float(duration));
    const float eased = linear * linear * (3.0f - 2.0f * linear);
    const int16_t delta = int16_t(target) - int16_t(start);
    setBacklight(uint8_t(constrain(int16_t(start) + int16_t(roundf(delta * eased)), 0, 255)));
    if (linear >= 1.0f) break;
    delay(16);
  }
}

void beginSplashTransition() {
  fadeBacklightTo(0, SPLASH_FADE_OUT_MS);
  display.fillScreen(COL_BG);
}

void drawSplashArc(int16_t centerX, int16_t centerY, int16_t radiusX, int16_t radiusY,
                  float startAngle, float endAngle, uint16_t color) {
  bool hasPrevious = false;
  int16_t previousX = 0;
  int16_t previousY = 0;
  for (float angle = startAngle; angle <= endAngle; angle += 5.0f) {
    const float radians = angle * DEG_TO_RAD;
    const int16_t x = centerX + int16_t(roundf(cosf(radians) * radiusX));
    const int16_t y = centerY + int16_t(roundf(sinf(radians) * radiusY));
    if (hasPrevious) display.drawLine(previousX, previousY, x, y, color);
    previousX = x;
    previousY = y;
    hasPrevious = true;
  }
}

void drawSplashStars(uint32_t now, uint16_t accent) {
  constexpr int16_t X[12] = {22, 47, 78, 112, 137, 184, 215, 248, 281, 303, 67, 257};
  constexpr int16_t Y[12] = {32, 72, 24, 54, 30, 28, 57, 22, 68, 42, 109, 104};
  for (uint8_t i = 0; i < 12; ++i) {
    const uint8_t glow = breathe(now + i * 173UL, 1900UL + i * 83UL, 46, 190);
    const uint16_t color = mixColor(COL_BG, i % 4 == 0 ? accent : COL_PEACH, glow);
    display.drawPixel(X[i], Y[i], color);
    if (glow > 150 && i % 3 == 0) display.drawPixel(X[i] + 1, Y[i], color);
  }
}

void drawSeedEmblem(int16_t centerX, int16_t centerY, uint16_t accent) {
  const uint16_t vein = mixColor(COL_BG, COL_PRIMARY, 150);
  display.drawLine(centerX, centerY + 15, centerX, centerY - 14, vein);
  display.fillTriangle(centerX, centerY - 1,
                       centerX + 15, centerY - 15,
                       centerX + 10, centerY + 5, accent);
  display.fillTriangle(centerX - 1, centerY + 1,
                       centerX - 12, centerY - 11,
                       centerX - 8, centerY + 7, mixColor(COL_BG, accent, 190));
  display.drawLine(centerX + 2, centerY - 2, centerX + 10, centerY - 10, vein);
  display.fillCircle(centerX, centerY + 16, 2, COL_PEACH);
}

const char* transitionTitle();
const char* transitionCopy();
const char* transitionDuration();

void drawStartupArtwork() {
  display.fillScreen(COL_BG);

  // The startup screen is a small moonlit observatory: concentric orbital
  // paths frame a glowing seed, while the low horizon keeps the composition
  // grounded instead of reading like a generic loading card.
  constexpr int16_t ORBIT_X = 160;
  constexpr int16_t ORBIT_Y = 78;
  display.fillCircle(ORBIT_X, ORBIT_Y, 42, mixColor(COL_BG, COL_PEACH, 18));
  display.fillCircle(ORBIT_X, ORBIT_Y, 31, mixColor(COL_BG, COL_PEACH, 28));
  display.fillCircle(ORBIT_X, ORBIT_Y, 21, mixColor(COL_BG, COL_PEACH, 44));
  display.drawEllipse(ORBIT_X, ORBIT_Y, 52, 25, mixColor(COL_BG, COL_GREEN, 70));
  display.drawEllipse(ORBIT_X, ORBIT_Y, 73, 36, mixColor(COL_BG, COL_PRIMARY, 210));
  drawSplashArc(ORBIT_X, ORBIT_Y, 94, 47, 198.0f, 344.0f, mixColor(COL_BG, COL_GREEN, 68));
  drawSplashArc(ORBIT_X, ORBIT_Y, 94, 47, 18.0f, 132.0f, mixColor(COL_BG, COL_PEACH, 50));
  drawSeedEmblem(ORBIT_X, ORBIT_Y - 2, COL_GREEN);

  // A layered horizon and quiet corner stems make startup feel like a place,
  // not merely a preloader.
  display.fillTriangle(0, 190, 0, 240, 120, 240, COL_DEEP);
  display.fillTriangle(0, 215, 82, 170, 183, 240, COL_SURFACE);
  display.fillTriangle(91, 240, 220, 181, 320, 229, COL_PRIMARY);
  display.fillTriangle(206, 240, 320, 174, 320, 240, COL_DEEP);
  drawBotanicalSprig(8, 206, 1, mixColor(COL_BG, COL_PRIMARY, 220));
  drawBotanicalSprig(312, 206, -1, mixColor(COL_BG, COL_PRIMARY, 220));
  display.drawLine(74, 199, 96, 169, mixColor(COL_BG, COL_GREEN, 62));
  display.drawLine(246, 199, 224, 169, mixColor(COL_BG, COL_GREEN, 62));
  drawSplashStars(millis(), COL_GREEN);

  display.setTextDatum(MC_DATUM);
  display.setTextColor(COL_CREAM, COL_BG);
  display.drawString("ESP32 FOCUS", RING_X, 132, 2);
  display.setTextColor(COL_MUTED, COL_BG);
  display.drawString("MOONLIT OBSERVATORY", RING_X, 149, 1);
}

void drawSessionArtwork(uint16_t accent) {
  display.fillScreen(COL_BG);
  const uint16_t quiet = mixColor(COL_BG, accent, 48);
  const uint16_t deepAccent = mixColor(COL_BG, accent, 92);

  // Each phase receives its own small visual metaphor while sharing the same
  // night-garden language as startup.
  if (session == Session::Focus) {
    display.fillCircle(RING_X, 83, 37, mixColor(COL_BG, accent, 18));
    display.drawEllipse(RING_X, 83, 47, 22, quiet);
    display.drawEllipse(RING_X, 83, 70, 32, mixColor(COL_BG, accent, 62));
    drawSplashArc(RING_X, 83, 91, 42, 205.0f, 330.0f, deepAccent);
    drawSplashArc(RING_X, 83, 91, 42, 26.0f, 115.0f, mixColor(COL_BG, COL_PEACH, 45));
    drawSeedEmblem(RING_X, 78, accent);
    display.fillCircle(75, 67, 2, COL_PEACH);
    display.fillCircle(246, 54, 2, accent);
    display.drawLine(44, 182, 76, 145, quiet);
    display.drawLine(276, 182, 244, 145, quiet);
  } else if (session == Session::ShortBreak) {
    display.fillCircle(160, 87, 39, mixColor(COL_BG, COL_PEACH, 16));
    display.drawEllipse(160, 87, 61, 28, mixColor(COL_BG, COL_PEACH, 55));
    display.drawEllipse(160, 87, 90, 41, quiet);
    drawSplashArc(160, 87, 105, 46, 195.0f, 304.0f, mixColor(COL_BG, COL_PEACH, 66));
    display.drawLine(117, 78, 151, 78, accent);
    display.drawLine(127, 88, 168, 88, mixColor(COL_BG, accent, 205));
    display.drawLine(140, 98, 181, 98, mixColor(COL_BG, COL_PEACH, 170));
    drawLeaf(194, 91, accent);
    drawLeaf(117, 108, mixColor(COL_BG, accent, 185));
    display.fillCircle(70, 59, 2, COL_PEACH);
    display.fillCircle(261, 114, 2, accent);
  } else {
    // Long break: a crescent moon and slow constellation, with foliage kept
    // low so the screen feels spacious and restorative.
    display.fillCircle(160, 80, 27, mixColor(COL_BG, COL_CREAM, 82));
    display.fillCircle(172, 71, 27, COL_BG);
    display.drawEllipse(160, 80, 50, 27, quiet);
    display.drawEllipse(160, 80, 84, 41, mixColor(COL_BG, accent, 58));
    display.fillCircle(114, 60, 2, COL_PEACH);
    display.fillCircle(221, 52, 2, accent);
    display.fillCircle(249, 98, 1, COL_PEACH);
    drawSplashArc(160, 80, 105, 47, 210.0f, 318.0f, deepAccent);
    drawBotanicalSprig(17, 190, 1, mixColor(COL_BG, accent, 180));
    drawBotanicalSprig(303, 190, -1, mixColor(COL_BG, accent, 180));
  }

  display.setTextDatum(MC_DATUM);
  display.fillRoundRect(130, 126, 60, 20, 10, COL_SURFACE);
  display.drawRoundRect(130, 126, 60, 20, 10, mixColor(COL_SURFACE, accent, 105));
  display.setTextColor(COL_PEACH, COL_SURFACE);
  display.drawString(transitionDuration(), RING_X, 136, 1);
  display.setTextColor(COL_CREAM, COL_BG);
  display.drawString(transitionTitle(), RING_X, 166, 4);
  display.setTextColor(COL_MUTED, COL_BG);
  display.drawString(transitionCopy(), RING_X, 188, 1);
}

const char* transitionTitle() {
  if (session == Session::ShortBreak) return "BREATHE";
  if (session == Session::LongBreak) return "RESTORE";
  return "FOCUS";
}

const char* transitionCopy() {
  if (session == Session::ShortBreak) return "A MOMENT TO RESET";
  if (session == Session::LongBreak) return "MAKE SPACE FOR CLARITY";
  return "DEEP WORK BEGINS";
}

const char* transitionDuration() {
  if (session == Session::ShortBreak) return "5 MIN";
  if (session == Session::LongBreak) return "15 MIN";
  return "25 MIN";
}

void drawProgressFlourish(int16_t x, int16_t y, int16_t width, float progress, uint16_t accent) {
  progress = constrain(progress, 0.0f, 1.0f);
  display.fillRoundRect(x, y, width, 6, 3, COL_SURFACE);
  const int16_t fillWidth = max(int16_t(2), int16_t(roundf(width * progress)));
  display.fillRoundRect(x, y, fillWidth, 6, 3, accent);
  if (progress > 0.01f && progress < 0.99f) {
    display.fillSmoothCircle(x + fillWidth - 2, y + 3, 3, COL_PEACH, accent);
  }
}

void showStartupSplash() {
  beginSplashTransition();
  drawStartupArtwork();
  display.setTextDatum(MC_DATUM);
  display.setTextColor(mixColor(COL_BG, COL_GREEN, 190), COL_BG);
  display.drawString("TUNING THE SIGNAL", RING_X, 164, 1);
  fadeBacklightTo(255, SPLASH_FADE_IN_MS);

  constexpr int16_t BAR_X = 90;
  constexpr int16_t BAR_Y = 176;
  constexpr int16_t BAR_W = 140;
  const uint32_t startedAt = millis();
  uint8_t copyStage = 0;
  while (true) {
    const uint32_t age = millis() - startedAt;
    const float linear = fminf(1.0f, float(age) / float(STARTUP_SPLASH_MS));
    const float eased = linear * linear * (3.0f - 2.0f * linear);
    drawProgressFlourish(BAR_X, BAR_Y, BAR_W, eased, COL_GREEN);
    drawSplashStars(millis(), COL_GREEN);

    const uint8_t nextStage = linear >= 0.72f ? 2 : linear >= 0.36f ? 1 : 0;
    if (nextStage != copyStage) {
      copyStage = nextStage;
      display.fillRect(70, 157, 180, 14, COL_BG);
      display.setTextDatum(MC_DATUM);
      display.setTextColor(copyStage == 2 ? COL_PEACH : COL_MUTED, COL_BG);
      display.drawString(copyStage == 1 ? "CALIBRATING TOUCH" : "READY TO FOCUS", RING_X, 164, 1);
    }
    if (linear >= 1.0f) break;
    delay(32);
  }
  fadeBacklightTo(0, SPLASH_FADE_OUT_MS);
}

void showSessionSplash() {
  const uint16_t accent = baseAccent();
  beginSplashTransition();
  drawSessionArtwork(accent);

  display.setTextDatum(MC_DATUM);
  display.setTextColor(mixColor(COL_BG, accent, 185), COL_BG);
  display.drawString("OPENING THE NEXT INTERVAL", RING_X, 205, 1);
  fadeBacklightTo(255, SPLASH_FADE_IN_MS);

  constexpr int16_t BAR_X = 120;
  constexpr int16_t BAR_Y = 218;
  constexpr int16_t BAR_W = 80;
  const uint32_t startedAt = millis();
  uint8_t copyStage = 0;
  while (true) {
    const uint32_t age = millis() - startedAt;
    const float linear = fminf(1.0f, float(age) / float(SESSION_SPLASH_MS));
    const float eased = linear * linear * (3.0f - 2.0f * linear);
    drawProgressFlourish(BAR_X, BAR_Y, BAR_W, eased, accent);
    drawSplashStars(millis(), accent);
    const uint8_t nextStage = linear >= 0.68f ? 2 : linear >= 0.30f ? 1 : 0;
    if (nextStage != copyStage) {
      copyStage = nextStage;
      display.fillRect(78, 198, 164, 14, COL_BG);
      display.setTextDatum(MC_DATUM);
      display.setTextColor(copyStage == 2 ? COL_PEACH : COL_MUTED, COL_BG);
      display.drawString(copyStage == 1 ? "SETTLING INTO RHYTHM" : "LET THE MOMENT LAND",
                         RING_X, 205, 1);
    }
    if (linear >= 1.0f) break;
    delay(32);
  }
  fadeBacklightTo(0, SPLASH_FADE_OUT_MS);
}

void drawBackdrop() {
  display.fillScreen(COL_BG);

  // Low-contrast landscape layers and peach sun from the design reference.
  display.fillCircle(32, 60, 17, mixColor(COL_BG, COL_PEACH, 92));
  display.fillTriangle(0, 95, 0, 196, 112, 196, COL_DEEP);
  display.fillTriangle(0, 131, 76, 91, 160, 196, COL_SURFACE);
  display.fillTriangle(44, 196, 174, 121, 254, 196, COL_PRIMARY);
  display.fillTriangle(158, 196, 265, 117, 320, 165, COL_SURFACE);
  display.fillTriangle(213, 196, 320, 118, 320, 196, COL_DEEP);
  display.fillRect(0, 179, SCREEN_W, 61, COL_DEEP);
  display.fillTriangle(0, 178, 84, 150, 154, 213, COL_SURFACE);
  display.fillTriangle(103, 212, 235, 159, 320, 210, COL_PRIMARY);

  const uint16_t contour = mixColor(COL_BG, COL_MUTED, 46);
  display.drawEllipse(35, 59, 46, 29, contour);
  display.drawEllipse(35, 59, 58, 37, contour);
  display.drawEllipse(286, 156, 43, 26, contour);
  display.drawEllipse(286, 156, 56, 35, contour);
  display.drawPixel(62, 176, mixColor(COL_BG, COL_PEACH, 150));
  display.drawPixel(274, 174, mixColor(COL_BG, COL_PEACH, 180));
  display.fillCircle(302, 91, 1, mixColor(COL_BG, COL_PEACH, 190));

  // A quiet botanical corner detail.
  const uint16_t leafDark = mixColor(COL_BG, COL_PRIMARY, 150);
  display.drawLine(284, 0, 318, 42, leafDark);
  display.fillTriangle(294, 13, 305, 8, 302, 22, leafDark);
  display.fillTriangle(306, 27, 318, 24, 312, 38, leafDark);

  // Keep the working face clean and opaque for stable partial redraws.
  display.fillCircle(RING_X, RING_Y, RING_R + 4, COL_BG);

  // Layered foreground foliage frames the timer without entering its redraw
  // region or competing with the controls.
  const uint16_t stem = mixColor(COL_BG, COL_PRIMARY, 205);
  drawBotanicalSprig(5, 190, 1, stem);
  drawBotanicalSprig(315, 190, -1, stem);
  display.drawLine(69, 153, 47, 126, mixColor(COL_BG, COL_GREEN, 62));
  display.drawLine(251, 153, 273, 126, mixColor(COL_BG, COL_GREEN, 62));
  display.fillTriangle(48, 127, 39, 121, 45, 135, mixColor(COL_BG, COL_PRIMARY, 175));
  display.fillTriangle(272, 127, 281, 121, 275, 135, mixColor(COL_BG, COL_PRIMARY, 175));
}

void laurelPoint(bool rightSide, float position, int16_t& x, int16_t& y) {
  const float angle = (rightSide ? 50.0f : 130.0f) + (rightSide ? -100.0f : 100.0f) * position;
  const float radians = angle * DEG_TO_RAD;
  constexpr float LAUREL_R = 84.0f;
  x = RING_X + int16_t(roundf(cosf(radians) * LAUREL_R));
  y = RING_Y + int16_t(roundf(sinf(radians) * LAUREL_R));
}

void drawLaurelGlow(int16_t x, int16_t y, int8_t direction) {
  const uint16_t sageHalo = mixColor(COL_BG, COL_GREEN, 54);
  const uint16_t peachHalo = mixColor(COL_BG, COL_PEACH, 74);
  const int16_t glowX = x + direction * 8;
  display.fillCircle(glowX, y - 3, 2, sageHalo);
  display.drawPixel(glowX + direction * 3, y - 3, peachHalo);
  display.drawPixel(glowX + direction, y - 7, sageHalo);
  display.drawPixel(glowX + direction, y + 3, sageHalo);
}

void drawLaurelCluster(int16_t x, int16_t y, int8_t direction,
                       uint16_t color, bool active) {
  if (active) drawLaurelGlow(x, y, direction);

  const uint16_t vein = mixColor(COL_BG, color, active ? 126 : 78);
  const uint16_t bud = mixColor(COL_BG, COL_PEACH, active ? 190 : 48);

  // Main leaf and its fine center vein.
  display.drawLine(x, y + 3, x + direction * 5, y - 3, color);
  display.fillTriangle(x + direction, y + 1,
                       x + direction * 8, y - 5,
                       x + direction * 6, y + 3, color);
  display.drawLine(x + direction * 2, y,
                   x + direction * 6, y - 3, vein);

  // Smaller companion leaflet and twig make every milestone a botanical
  // cluster rather than a single geometric leaf.
  display.drawLine(x, y + 1, x + direction * 4, y + 5, color);
  display.fillTriangle(x + direction * 2, y + 2,
                       x + direction * 7, y + 2,
                       x + direction * 5, y + 7, color);
  display.drawLine(x + direction * 3, y + 3,
                   x + direction * 6, y + 3, vein);
  display.fillCircle(x + direction * 9, y - 4, 1, bud);
}

void drawLaurelFrame() {
  constexpr uint8_t LAUREL_PAIRS = 12;
  const uint16_t stemColor = mixColor(COL_BG, COL_PRIMARY, 175);
  for (uint8_t side = 0; side < 2; ++side) {
    int16_t lastX, lastY;
    laurelPoint(side == 1, 0.0f, lastX, lastY);
    for (uint8_t step = 1; step <= 18; ++step) {
      int16_t x, y;
      laurelPoint(side == 1, float(step) / 18.0f, x, y);
      display.drawLine(lastX, lastY, x, y, stemColor);
      lastX = x;
      lastY = y;
    }
  }

  // The complete dim wreath is the botanical equivalent of the empty ring;
  // elapsed progress brightens these pairs from the bottom upward.
  for (uint8_t i = 0; i < LAUREL_PAIRS; ++i) {
    const float position = float(i) / float(LAUREL_PAIRS - 1);
    int16_t x, y;
    laurelPoint(false, position, x, y);
    drawLaurelCluster(x, y, -1, stemColor, false);
    laurelPoint(true, position, x, y);
    drawLaurelCluster(x, y, 1, stemColor, false);
  }
}

void drawLaurelGrowth(uint32_t now, bool force = false) {
  constexpr uint8_t LAUREL_PAIRS = 12;
  const float progress = logicalProgress(now);
  const uint8_t count = min<uint8_t>(LAUREL_PAIRS, uint8_t(progress * (LAUREL_PAIRS + 0.001f)));
  if (force) lastLaurelCount = 0;
  if (count <= lastLaurelCount) return;
  const uint16_t leafColor = mixColor(COL_BG, baseAccent(), 205);
  for (uint8_t i = lastLaurelCount; i < count; ++i) {
    const float position = float(i) / float(LAUREL_PAIRS - 1);
    int16_t x, y;
    laurelPoint(false, position, x, y);
    drawLaurelCluster(x, y, -1, leafColor, true);
    laurelPoint(true, position, x, y);
    drawLaurelCluster(x, y, 1, leafColor, true);
  }
  lastLaurelCount = count;
}

void drawRuntime(uint32_t now) {
  const uint32_t totalSeconds = now / 1000UL;
  if (int32_t(totalSeconds) == lastRuntimeSecond) return;

  const uint32_t hours = totalSeconds / 3600UL;
  const uint32_t minutes = (totalSeconds / 60UL) % 60UL;
  const uint32_t seconds = totalSeconds % 60UL;
  char text[28];
  snprintf(text, sizeof(text), "Runtime : %02lu:%02lu:%02lu",
           static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes),
           static_cast<unsigned long>(seconds));

  // This is a dedicated opaque header cell, so the uptime can refresh once a
  // second without disturbing the title, cycle dots, or the timer artwork.
  display.fillRect(96, 6, 150, 13, COL_BG);
  display.setTextDatum(TL_DATUM);
  display.setTextFont(1);
  display.setTextColor(mixColor(COL_BG, COL_GREEN, 190), COL_BG);
  display.drawString(text, 96, 8);
  lastRuntimeSecond = int32_t(totalSeconds);
}

void drawHeader(uint32_t now) {
  const uint16_t secondary = timerState == TimerState::Paused ? mixColor(COL_BG, COL_MUTED, 125) : COL_MUTED;
  drawLeaf(13, 13, baseAccent());
  display.setTextDatum(TL_DATUM);
  display.setTextFont(2);
  display.setTextColor(COL_CREAM, COL_BG);
  display.drawString(headerName(), 31, 7);
  drawRuntime(now);
  display.setTextFont(1);
  display.setTextColor(mixColor(COL_BG, COL_PEACH, 210), COL_BG);
  display.drawString("P O M O D O R O", 31, 22);

  for (uint8_t i = 0; i < FOCUSES_PER_CYCLE; ++i) {
    const int16_t x = 260 + i * 14;
    if (i < cycleFocuses) display.fillCircle(x, 15, 3, COL_GREEN);
    else if (i == cycleFocuses && cycleFocuses < FOCUSES_PER_CYCLE) display.fillCircle(x, 15, 3, COL_PEACH);
    else display.fillCircle(x, 15, 3, mixColor(COL_BG, secondary, 135));
  }
  display.drawFastHLine(12, 34, 296, mixColor(COL_BG, COL_PRIMARY, 170));
}

uint16_t ringColor(uint32_t now) {
  (void)now;
  if (timerState == TimerState::Paused) {
    return mixColor(COL_TRACK, baseAccent(), 110);
  }
  if (timerState == TimerState::Complete) return COL_GREEN;
  return baseAccent();
}

void ringPoint(float angle, int16_t& x, int16_t& y) {
  const float radians = angle * DEG_TO_RAD;
  const float radius = (RING_R + RING_IR) * 0.5f;
  x = RING_X - int16_t(roundf(sinf(radians) * radius));
  y = RING_Y + int16_t(roundf(cosf(radians) * radius));
}

void updateRingBotanicals(uint32_t now, bool force = false) {
  if (!force && now - lastBotanicalFrame < 55) return;
  lastBotanicalFrame = now;
  drawAmbientFireflies(now);

  // Four slow wanderers live in solid-color scenery pockets outside the ring.
  // Restoring only their two-pixel footprints keeps the background stable.
  constexpr int16_t BASE_X[4] = {68, 18, 267, 292};
  constexpr int16_t BASE_Y[4] = {105, 148, 73, 102};
  constexpr int8_t AMP_X[4] = {4, 4, 6, 6};
  constexpr int8_t AMP_Y[4] = {2, 4, 5, 4};
  constexpr uint16_t RESTORE[4] = {COL_SURFACE, COL_DEEP, COL_BG, COL_BG};
  for (uint8_t i = 0; i < 4; ++i) {
    if (backgroundFireflyX[i] >= 0) {
      display.fillCircle(backgroundFireflyX[i], backgroundFireflyY[i], 2, RESTORE[i]);
    }
  }

  const float wanderPhase = float(now % 16000UL) / 16000.0f * 2.0f * PI;
  for (uint8_t i = 0; i < 4; ++i) {
    backgroundFireflyX[i] = BASE_X[i] + int16_t(roundf(
      sinf(wanderPhase + i * 1.71f) * AMP_X[i]));
    backgroundFireflyY[i] = BASE_Y[i] + int16_t(roundf(
      cosf(wanderPhase * (0.72f + i * 0.07f) + i * 1.19f) * AMP_Y[i]));
    const uint8_t glow = breathe(now + i * 810UL, 2600 + i * 530UL, 70, 220);
    const uint16_t halo = mixColor(RESTORE[i], COL_PEACH, glow / 3);
    const uint16_t core = mixColor(RESTORE[i], COL_PEACH, glow);
    display.fillCircle(backgroundFireflyX[i], backgroundFireflyY[i], 2, halo);
    display.drawPixel(backgroundFireflyX[i], backgroundFireflyY[i], core);
  }

  for (uint8_t i = 0; i < 4; ++i) {
    if (fireflyX[i] >= 0) display.fillSmoothCircle(fireflyX[i], fireflyY[i], 2, COL_BG, COL_BG);
  }

  const float phase = float(now % 8200UL) / 8200.0f * 2.0f * PI;
  constexpr float ORBIT_R[4] = {68.0f, 67.0f, 69.0f, 68.0f};
  const float angles[4] = {
    180.0f + 82.0f * sinf(phase),
    180.0f + 76.0f * sinf(phase + 1.45f),
    180.0f + 70.0f * sinf(phase + 2.80f),
    180.0f + 64.0f * sinf(phase + 4.10f)
  };
  for (uint8_t i = 0; i < 4; ++i) {
    const float radians = angles[i] * DEG_TO_RAD;
    fireflyX[i] = RING_X - int16_t(roundf(sinf(radians) * ORBIT_R[i]));
    fireflyY[i] = RING_Y + int16_t(roundf(cosf(radians) * ORBIT_R[i]));
    const uint8_t glow = breathe(now + i * 620UL, 1750 + i * 460UL, 72, 220);
    const uint16_t color = mixColor(COL_BG, COL_PEACH, glow);
    display.fillSmoothCircle(fireflyX[i], fireflyY[i], glow > 160 ? 2 : 1, color, COL_BG);
  }
}

void drawRingSpan(float startAngle, float endAngle, uint16_t color, bool rounded = true) {
  int16_t start = int16_t(roundf(startAngle));
  int16_t end = int16_t(roundf(endAngle));
  if (end <= start) return;
  while (start < 0) {
    start += 360;
    end += 360;
  }
  while (start >= 360) {
    start -= 360;
    end -= 360;
  }
  if (end <= 360) {
    display.drawSmoothArc(RING_X, RING_Y, RING_R, RING_IR, start, end, color, COL_BG, rounded);
  } else {
    display.drawSmoothArc(RING_X, RING_Y, RING_R, RING_IR, start, 360, color, COL_BG, rounded);
    display.drawSmoothArc(RING_X, RING_Y, RING_R, RING_IR, 0, end - 360, color, COL_BG, rounded);
  }
}

void drawProgressRing(uint32_t now, bool force = false) {
  const float target = logicalProgress(now);
  const uint32_t dt = lastRingFrame ? now - lastRingFrame : 40;
  lastRingFrame = now;
  if (force) displayedProgress = target;
  else {
    const float blend = fminf(1.0f, float(dt) / 190.0f);
    displayedProgress += (target - displayedProgress) * blend;
  }
  if (displayedProgress < 0.0001f) displayedProgress = 0.0f;
  if (displayedProgress > 0.9998f) displayedProgress = 1.0f;

  const float endAngleF = 180.0f + displayedProgress * 360.0f;
  const int16_t arcAngle = int16_t(roundf(endAngleF)) % 360;
  int16_t knobX, knobY;
  ringPoint(fmodf(endAngleF, 360.0f), knobX, knobY);
  const uint16_t accent = ringColor(now);
  if (!force && (displayedProgress <= lastDrawnProgress ||
      (arcAngle == lastArcAngle && knobX == lastKnobX && knobY == lastKnobY))) return;

  if (force) {
    // A full band draw is only allowed while composing a brand-new screen.
    display.drawSmoothArc(RING_X, RING_Y, RING_R + 4, RING_IR - 5, 0, 360, COL_BG, COL_BG, false);
    display.drawSmoothArc(RING_X, RING_Y, RING_R, RING_IR, 0, 360, COL_TRACK, COL_BG, false);
    if (displayedProgress >= 0.999f) {
      display.drawSmoothArc(RING_X, RING_Y, RING_R, RING_IR, 0, 360, accent, COL_BG, false);
    } else if (displayedProgress > 0.0f) {
      drawRingSpan(180.0f, endAngleF, accent);
    }
  } else {
    // Remove only the previous endpoint, restore the few covered arc degrees,
    // then paint only the newly elapsed span. The rest of the ring is untouched.
    if (lastDrawnProgress > 0.0f && lastKnobX >= 0) {
      display.fillSmoothCircle(lastKnobX, lastKnobY, 6, COL_BG, COL_BG);
      const float oldEnd = 180.0f + lastDrawnProgress * 360.0f;
      drawRingSpan(oldEnd - 7.0f, oldEnd, accent);
      drawRingSpan(oldEnd, oldEnd + 7.0f, COL_TRACK, false);
      drawRingSpan(oldEnd, endAngleF, accent);
    } else {
      drawRingSpan(180.0f, endAngleF, accent);
    }
  }

  if (displayedProgress > 0.0f) {
    const uint16_t halo = mixColor(accent, COL_PEACH, 140);
    display.fillSmoothCircle(knobX, knobY, 5, halo, COL_BG);
    display.fillSmoothCircle(knobX, knobY, 3, COL_PEACH, halo);
    lastKnobX = knobX;
    lastKnobY = knobY;
  } else {
    lastKnobX = lastKnobY = -1;
  }

  lastArcAngle = arcAngle;
  lastDrawnProgress = displayedProgress;
  lastRingColor = accent;
}

void formatTimeDigits(uint32_t now, char out[4], int32_t& totalSeconds) {
  const uint32_t duration = sessionDurationMs();
  const uint32_t remaining = duration - min(elapsedMs(now), duration);
  totalSeconds = int32_t((remaining + 999) / 1000);
  char text[6];
  snprintf(text, sizeof(text), "%02ld%02ld", long(totalSeconds / 60), long(totalSeconds % 60));
  memcpy(out, text, 4);
}

void renderDigit(uint8_t index, char value, uint16_t color, int8_t yOffset = 0) {
  char text[2] = {value, '\0'};
  digitSprite.fillSprite(COL_BG);
  digitSprite.setTextDatum(MC_DATUM);
  digitSprite.setTextColor(color, COL_BG);
  digitSprite.drawString(text, DIGIT_W / 2, DIGIT_H / 2 + yOffset, 6);

  const int16_t left = DIGIT_X[index] - DIGIT_W / 2;
  // Clear only the inset digit cell, then treat the sprite background as
  // transparent. This avoids pushing an opaque tile over the progress ring.
  display.fillRect(left, DIGIT_Y, DIGIT_W, DIGIT_H, COL_BG);
  digitSprite.pushSprite(left, DIGIT_Y, COL_BG);
}

void drawDigitsInstant(uint32_t now) {
  int32_t seconds;
  char digits[4];
  formatTimeDigits(now, digits, seconds);
  lastTimeSecond = seconds;
  for (uint8_t i = 0; i < 4; ++i) {
    visibleDigits[i] = fromDigits[i] = toDigits[i] = digits[i];
    digitAnimating[i] = false;
    renderDigit(i, digits[i], COL_CREAM);
  }
  display.setTextDatum(MC_DATUM);
  display.setTextColor(COL_CREAM, COL_BG);
  display.drawString(":", RING_X, DIGIT_Y + DIGIT_H / 2, 6);
}

void updateDigitTargets(uint32_t now) {
  int32_t seconds;
  char digits[4];
  formatTimeDigits(now, digits, seconds);
  if (seconds == lastTimeSecond) return;
  lastTimeSecond = seconds;
  digitAnimationAt = now;
  for (uint8_t i = 0; i < 4; ++i) {
    if (digits[i] == visibleDigits[i]) continue;
    fromDigits[i] = visibleDigits[i];
    toDigits[i] = digits[i];
    digitAnimating[i] = true;
  }
}

void updateDigitFade(uint32_t now) {
  constexpr uint32_t FADE_OUT_MS = 60;
  constexpr uint32_t FADE_TOTAL_MS = 145;
  const uint32_t age = now - digitAnimationAt;
  bool anyAnimating = false;
  for (bool active : digitAnimating) anyAnimating |= active;
  if (!anyAnimating) return;
  if (age < FADE_TOTAL_MS && now - lastDigitFrame < 24) return;
  lastDigitFrame = now;
  for (uint8_t i = 0; i < 4; ++i) {
    if (!digitAnimating[i]) continue;
    if (age < FADE_OUT_MS) {
      const uint8_t level = 255 - uint8_t((age * 255UL) / FADE_OUT_MS);
      renderDigit(i, fromDigits[i], mixColor(COL_BG, COL_CREAM, level), int8_t(age / 25));
    } else if (age < FADE_TOTAL_MS) {
      const uint8_t level = uint8_t(((age - FADE_OUT_MS) * 255UL) / (FADE_TOTAL_MS - FADE_OUT_MS));
      renderDigit(i, toDigits[i], mixColor(COL_BG, COL_CREAM, level), int8_t(3 - (age - FADE_OUT_MS) / 30));
    } else {
      visibleDigits[i] = toDigits[i];
      digitAnimating[i] = false;
      renderDigit(i, visibleDigits[i], COL_CREAM);
    }
  }
}

void drawFocusThought(uint32_t now, bool force = false) {
  const bool visible = session == Session::Focus && timerState == TimerState::Running;
  if (!visible) {
    if (lastFocusThought != -1 || force) {
      display.fillRect(FOCUS_THOUGHT_X, FOCUS_THOUGHT_Y,
                       FOCUS_THOUGHT_W, FOCUS_THOUGHT_H, COL_BG);
      lastFocusThought = -1;
      nextFocusThought = -1;
      lastFocusThoughtFrame = 0;
      lastFocusThoughtCycle = -1;
    }
    return;
  }

  const uint32_t thoughtAge = elapsedMs(now);
  constexpr uint32_t cycleMs = FOCUS_THOUGHT_TRANSITION_MS * 2;
  const int32_t thoughtCycle = int32_t(thoughtAge / cycleMs);
  if (thoughtCycle != lastFocusThoughtCycle || lastFocusThought < 0 || nextFocusThought < 0) {
    if (lastFocusThought < 0) {
      lastFocusThought = int8_t(pickFocusThought(-1));
      nextFocusThought = int8_t(pickFocusThought(lastFocusThought));
    } else {
      lastFocusThought = nextFocusThought;
      nextFocusThought = int8_t(pickFocusThought(lastFocusThought));
    }
    lastFocusThoughtCycle = thoughtCycle;
  }
  const uint8_t thought = uint8_t(lastFocusThought);
  if (!force && now - lastFocusThoughtFrame < FOCUS_THOUGHT_FRAME_MS) return;

  // This small cell sits inside the ring above the countdown. Repainting only
  // this 114x14 region keeps each animated letter independent from the timer
  // digits and scenery.
  display.fillRect(FOCUS_THOUGHT_X, FOCUS_THOUGHT_Y,
                   FOCUS_THOUGHT_W, FOCUS_THOUGHT_H, COL_BG);
  display.setTextDatum(MC_DATUM);
  display.setTextFont(1);

  const char* word = FOCUS_THOUGHTS[thought];
  uint8_t length = 0;
  while (word[length] != '\0') ++length;
  const int16_t firstX = RING_X - int16_t((length - 1) * FOCUS_THOUGHT_CHAR_W / 2);
  const uint32_t phase = thoughtAge % cycleMs;
  const uint32_t fadeOutStart = FOCUS_THOUGHT_TRANSITION_MS;
  const bool fadingIn = phase < FOCUS_THOUGHT_TRANSITION_MS;
  const bool fadingOut = phase >= fadeOutStart;
  const uint32_t transitionAge = fadingIn ? phase : phase - fadeOutStart;
  const uint32_t transitionWidth = FOCUS_THOUGHT_TRANSITION_MS;
  for (uint8_t i = 0; i < length; ++i) {
    uint8_t alpha = 185;
    if (fadingIn || fadingOut) {
      const int32_t letterProgress = int32_t(
        (transitionAge * uint32_t(length + 1) * 256UL) / transitionWidth) -
        int32_t(i * 256U);
      if (fadingIn) {
        if (letterProgress <= 0) alpha = 0;
        else if (letterProgress < 256) {
          alpha = uint8_t((letterProgress * 185L) / 256L);
        }
      } else {
        if (letterProgress >= 256) alpha = 0;
        else if (letterProgress > 0) {
          alpha = uint8_t(185 - (letterProgress * 185L) / 256L);
        }
      }
    }
    if (alpha == 0) continue;
    char letter[2] = {word[i], '\0'};
    display.setTextColor(mixColor(COL_BG, COL_GREEN, alpha), COL_BG);
    display.drawString(letter, firstX + i * FOCUS_THOUGHT_CHAR_W,
                       FOCUS_THOUGHT_Y + 7, 1);
  }
  lastFocusThought = int8_t(thought);
  lastFocusThoughtFrame = now;
}

void drawSessionStatus() {
  const uint16_t accent = baseAccent();
  const char* compactSession = session == Session::Focus ? "FOCUS" : "BREAK";
  const char* compactState = "READY";
  if (timerState == TimerState::Running) compactState = session == Session::Focus ? "IN FLOW" : "REST";
  else if (timerState == TimerState::Paused) compactState = "PAUSED";
  else if (timerState == TimerState::Complete) compactState = "DONE";

  display.fillSmoothCircle(RING_X, 146, 2, accent, COL_BG);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(accent, COL_BG);
  display.drawString(compactSession, RING_X, 155, 2);
  const uint16_t stateColor = timerState == TimerState::Paused ? COL_CREAM : COL_MUTED;
  display.setTextColor(stateColor, COL_BG);
  display.drawString(compactState, RING_X, 170, 1);
}

void drawSecondaryButton(const Button& button, const char* label) {
  const uint16_t edge = timerState == TimerState::Paused ? mixColor(COL_BG, COL_PRIMARY, 120) : COL_PRIMARY;
  const uint16_t text = timerState == TimerState::Paused ? mixColor(COL_BG, COL_MUTED, 145) : COL_MUTED;
  display.fillRoundRect(button.x, button.y, button.w, button.h, 10, COL_SURFACE);
  display.drawRoundRect(button.x, button.y, button.w, button.h, 10, edge);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(text, COL_SURFACE);
  display.drawString(label, button.x + button.w / 2, button.y + button.h / 2 + 1, 2);
}

void drawPrimaryButton() {
  const bool ready = timerState == TimerState::Ready;
  // Keep the button static between state changes. Repainting the whole control
  // every 50 ms caused visible tearing/flicker on the SPI display.
  const uint16_t edge = mixColor(COL_PRIMARY, baseAccent(), ready ? 205 : 155);
  display.fillRoundRect(BTN_PRIMARY.x, BTN_PRIMARY.y + 2, BTN_PRIMARY.w, BTN_PRIMARY.h, 13, COL_BG);
  display.fillRoundRect(BTN_PRIMARY.x, BTN_PRIMARY.y, BTN_PRIMARY.w, BTN_PRIMARY.h, 13, COL_PRIMARY);
  display.drawRoundRect(BTN_PRIMARY.x, BTN_PRIMARY.y, BTN_PRIMARY.w, BTN_PRIMARY.h, 13, edge);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(COL_CREAM, COL_PRIMARY);
  display.drawString(primaryLabel(), BTN_PRIMARY.x + BTN_PRIMARY.w / 2 - (ready ? 5 : 0),
                     BTN_PRIMARY.y + BTN_PRIMARY.h / 2 + 1, 2);
  if (ready) {
    const uint16_t dot = mixColor(COL_PRIMARY, COL_PEACH, 205);
    display.fillSmoothCircle(BTN_PRIMARY.x + 88, BTN_PRIMARY.y + BTN_PRIMARY.h / 2, 4, dot, COL_PRIMARY);
  }
}

void drawButtons() {
  drawSecondaryButton(BTN_RESET, "RESET");
  drawPrimaryButton();
  drawSecondaryButton(BTN_NEXT, "NEXT");
}

void drawScreen(uint32_t now, bool stagedReveal = false, bool fadeIn = false) {
  (void)stagedReveal;
  drawBackdrop();
  drawLaurelFrame();
  drawHeader(now);
  displayedProgress = logicalProgress(now);
  lastArcAngle = lastKnobX = lastKnobY = -1;
  lastRingColor = 0;
  lastDrawnProgress = 0.0f;
  for (uint8_t i = 0; i < 4; ++i) {
    fireflyX[i] = -1;
    fireflyY[i] = -1;
    backgroundFireflyX[i] = -1;
    backgroundFireflyY[i] = -1;
  }
  lastLaurelCount = 0;
  drawProgressRing(now, true);
  drawDigitsInstant(now);
  drawSessionStatus();
  drawLaurelGrowth(now, true);
  drawButtons();
  updateRingBotanicals(now, true);
  drawFocusThought(now, true);
  if (timerState == TimerState::Complete) drawCompletionAlert(now, true);
  if (fadeIn) fadeBacklightTo(255, SPLASH_FADE_IN_MS);
}

void logState() {
  Serial.printf("[state] %s | %s | cycle %u/%u\n", sessionName(),
                timerState == TimerState::Running ? "running" :
                timerState == TimerState::Paused ? "paused" :
                timerState == TimerState::Complete ? "complete" : "ready",
                cycleFocuses, FOCUSES_PER_CYCLE);
}

void startOrPause(uint32_t now) {
  const TimerState previous = timerState;
  bool stagedReveal = false;
  if (timerState == TimerState::Running) {
    accumulatedMs += now - runStartedAt;
    timerState = TimerState::Paused;
  } else if (timerState == TimerState::Ready) {
    showSessionSplash();
    now = millis();
    runStartedAt = now;
    timerState = TimerState::Running;
    stagedReveal = true;
  } else if (timerState == TimerState::Paused) {
    runStartedAt = now;
    timerState = TimerState::Running;
  } else return;

  if (previous == TimerState::Ready) {
    startPulseActive = true;
  }
  logState();
  drawScreen(now, stagedReveal, previous == TimerState::Ready);
  if (previous == TimerState::Ready) {
    now = millis();
    runStartedAt = now;
    startPulseAt = now;
  }
}

void resetSession(uint32_t now) {
  accumulatedMs = 0;
  timerState = TimerState::Ready;
  startPulseActive = false;
  logState();
  drawScreen(now);
}

void advanceSession(uint32_t now, bool autoStart) {
  if (session == Session::Focus) {
    if (cycleFocuses < FOCUSES_PER_CYCLE) ++cycleFocuses;
    session = cycleFocuses >= FOCUSES_PER_CYCLE ? Session::LongBreak : Session::ShortBreak;
  } else {
    if (session == Session::LongBreak) cycleFocuses = 0;
    session = Session::Focus;
  }
  accumulatedMs = 0;
  startPulseActive = false;
  showSessionSplash();
  now = millis();
  timerState = autoStart ? TimerState::Running : TimerState::Ready;
  runStartedAt = now;
  logState();
  drawScreen(now, true, true);
  if (autoStart) runStartedAt = millis();
}

void skipSession(uint32_t now) {
  Serial.println("[control] next session");
  advanceSession(now, false);
}

void updateStartPulse(uint32_t now) {
  if (!startPulseActive) return;
  constexpr uint32_t PULSE_MS = 260;
  const uint32_t age = now - startPulseAt;
  if (age < PULSE_MS && now - lastPulseFrame < 24) return;
  lastPulseFrame = now;
  display.fillRect(108, 139, 104, 4, COL_BG);
  if (age >= PULSE_MS) {
    startPulseActive = false;
    return;
  }
  const float progress = float(age) / float(PULSE_MS);
  const int16_t width = 18 + int16_t(progress * 82.0f);
  const uint8_t alpha = uint8_t((1.0f - progress) * 230.0f);
  display.drawFastHLine(RING_X - width / 2, 140, width, mixColor(COL_BG, COL_PEACH, alpha));
}

void updateAnimations(uint32_t now) {
  if (timerState == TimerState::Complete) {
    drawCompletionAlert(now);
    return;
  }

  updateDigitTargets(now);
  updateDigitFade(now);
  updateStartPulse(now);

  if (now - lastRingFrame >= 28) drawProgressRing(now);
  drawLaurelGrowth(now);
  updateRingBotanicals(now);
  drawFocusThought(now);
}

void pollTouch(uint32_t now) {
  static uint32_t lastPoll = 0;
  static uint8_t stableSamples = 0;
  static bool held = false;
  static uint32_t touchStartedAt = 0;
  static int16_t lastX = 0, lastY = 0;
  if (now - lastPoll < 18) return;
  lastPoll = now;

  int16_t x = 0, y = 0;
  const bool pressed = readTouch(x, y);
  if (pressed) {
    if (stableSamples == 0) touchStartedAt = now;
    if (stableSamples == 0 || (abs(x - lastX) < 25 && abs(y - lastY) < 25)) {
      if (stableSamples < 10) ++stableSamples;
    } else stableSamples = 1;
    lastX = x;
    lastY = y;
    if (stableSamples >= 3) held = true;
    return;
  }

  if (held) {
    // Completion is a modal state: every screen tap acknowledges the alert
    // and starts the next phase. A press that began before completion cannot
    // dismiss it accidentally; the user must make a fresh tap.
    if (timerState == TimerState::Complete && touchStartedAt >= completedAt) {
      Serial.println("[control] completion acknowledged");
      advanceSession(now, true);
    } else if (inside(BTN_PRIMARY, lastX, lastY)) startOrPause(now);
    else if (inside(BTN_RESET, lastX, lastY)) resetSession(now);
    else if (inside(BTN_NEXT, lastX, lastY)) skipSession(now);
  }
  stableSamples = 0;
  held = false;
}

void initTouch() {
  pinMode(TOUCH_CS, OUTPUT);
  pinMode(TOUCH_CLK, OUTPUT);
  pinMode(TOUCH_DIN, OUTPUT);
  pinMode(TOUCH_MISO, INPUT);
  pinMode(TOUCH_IRQ, INPUT);
  digitalWrite(TOUCH_CS, HIGH);
  digitalWrite(TOUCH_CLK, HIGH);
}

void setup() {
  Serial.begin(115200);
  delay(80);
  Serial.println("\n[esp32-focus] boot");
  Serial.printf("[boot] reset_reason=%d heap=%u\n", int(esp_reset_reason()), ESP.getFreeHeap());
  Serial.println("[hardware] ESP32 + ILI9341 320x240 + XPT2046");

  pinMode(BACKLIGHT_PIN, OUTPUT);
  setBacklight(0);
  initTouch();
  display.init();
  display.setRotation(1);

  digitSprite.setColorDepth(16);
  if (!digitSprite.createSprite(DIGIT_W, DIGIT_H)) Serial.println("[fatal] digit sprite allocation failed");

  showStartupSplash();

  drawScreen(millis(), true, true);
  logState();
  Serial.println("[ready] botanical UI + touch controls active");
#ifdef ESP32_FOCUS_SELF_TEST
  Serial.println("[self-test] accelerated cycle started");
  startOrPause(millis());
#endif
}

void loop() {
  uint32_t now = millis();
  pollTouch(now);
  // Touch actions may display a transition splash. Refresh the timestamp so
  // the new session never inherits time spent on that splash.
  now = millis();

  if (timerState == TimerState::Running && elapsedMs(now) >= sessionDurationMs()) {
    accumulatedMs = sessionDurationMs();
    timerState = TimerState::Complete;
    completedAt = now;
    logState();
    drawScreen(now);
  }

  updateAnimations(now);
  drawRuntime(now);
  if (now - lastHealthLog >= 60000UL) {
    lastHealthLog = now;
    Serial.printf("[health] uptime=%lus heap=%u state=%s\n", now / 1000UL,
                  ESP.getFreeHeap(), timerState == TimerState::Running ? "running" : "idle");
  }
  delay(2);
}
