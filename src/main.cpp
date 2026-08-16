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
constexpr uint32_t COMPLETE_HOLD_MS = 480;

#ifdef ESP32_FOCUS_SELF_TEST
constexpr uint32_t TEST_FOCUS_MS = 2000;
constexpr uint32_t TEST_SHORT_BREAK_MS = 1000;
constexpr uint32_t TEST_LONG_BREAK_MS = 1500;
constexpr uint32_t TEST_COMPLETE_HOLD_MS = 420;
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
constexpr int16_t DIGIT_Y = 82;
constexpr int16_t DIGIT_W = 38;
constexpr int16_t DIGIT_H = 54;
constexpr int16_t DIGIT_X[4] = {104, 136, 184, 216};

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

char visibleDigits[4] = {'2', '5', '0', '0'};
char fromDigits[4] = {'2', '5', '0', '0'};
char toDigits[4] = {'2', '5', '0', '0'};
bool digitAnimating[4] = {false, false, false, false};
uint32_t digitAnimationAt = 0;
int32_t lastTimeSecond = -1;

float displayedProgress = 0.0f;
uint32_t lastRingFrame = 0;
int16_t lastKnobX = -1;
int16_t lastKnobY = -1;
int16_t lastArcAngle = -1;
uint16_t lastRingColor = 0;
uint32_t lastReadyFrame = 0;
uint32_t lastDigitFrame = 0;
uint32_t lastPulseFrame = 0;
bool startPulseActive = false;
uint32_t startPulseAt = 0;

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

uint32_t completeHoldMs() {
#ifdef ESP32_FOCUS_SELF_TEST
  return TEST_COMPLETE_HOLD_MS;
#else
  return COMPLETE_HOLD_MS;
#endif
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

const char* stateName() {
  switch (timerState) {
    case TimerState::Ready: return "READY";
    case TimerState::Running: return session == Session::Focus ? "DEEP WORK" : "RECOVER";
    case TimerState::Paused: return "PAUSED";
    case TimerState::Complete: return "COMPLETE";
  }
  return "READY";
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
}

void drawHeader() {
  const uint16_t secondary = timerState == TimerState::Paused ? mixColor(COL_BG, COL_MUTED, 125) : COL_MUTED;
  drawLeaf(13, 13, baseAccent());
  display.setTextDatum(TL_DATUM);
  display.setTextFont(2);
  display.setTextColor(COL_CREAM, COL_BG);
  display.drawString(headerName(), 31, 7);
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
  if (timerState == TimerState::Paused) {
    return mixColor(COL_TRACK, baseAccent(), breathe(now, 2300, 55, 145));
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
  const bool animatedState = timerState == TimerState::Paused || timerState == TimerState::Complete;
  if (!force && !animatedState && arcAngle == lastArcAngle && knobX == lastKnobX && knobY == lastKnobY && accent == lastRingColor) return;

  // Clear only the ring band, then restore the thin track and live progress.
  display.drawSmoothArc(RING_X, RING_Y, RING_R + 4, RING_IR - 5, 0, 360, COL_BG, COL_BG, false);
  display.drawSmoothArc(RING_X, RING_Y, RING_R, RING_IR, 0, 360, COL_TRACK, COL_BG, false);
  if (displayedProgress >= 0.999f) {
    display.drawSmoothArc(RING_X, RING_Y, RING_R, RING_IR, 0, 360, accent, COL_BG, false);
  } else if (displayedProgress > 0.0001f) {
    display.drawSmoothArc(RING_X, RING_Y, RING_R, RING_IR, 180, arcAngle, accent, COL_BG, true);
  }

  if (timerState == TimerState::Complete) {
    const uint32_t age = now - completedAt;
    const uint16_t sweepStart = (180 + (age * 540UL) / completeHoldMs()) % 360;
    const uint16_t sweepEnd = (sweepStart + 52) % 360;
    display.drawSmoothArc(RING_X, RING_Y, RING_R, RING_IR, sweepStart, sweepEnd, COL_CREAM, COL_BG, true);
    ringPoint(float(sweepEnd), knobX, knobY);
  }
  display.fillSmoothCircle(knobX, knobY, 4, COL_PEACH, COL_BG);

  lastArcAngle = arcAngle;
  lastKnobX = knobX;
  lastKnobY = knobY;
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
  digitSprite.drawString(text, DIGIT_W / 2, DIGIT_H / 2 + yOffset, 7);
  digitSprite.pushSprite(DIGIT_X[index] - DIGIT_W / 2, DIGIT_Y);
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
  display.drawString(":", RING_X, DIGIT_Y + DIGIT_H / 2, 7);
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

void drawSessionStatus() {
  const uint16_t accent = baseAccent();
  const int16_t labelWidth = display.textWidth(sessionName(), 2);
  drawLeaf(RING_X - labelWidth / 2 - 17, 150, accent);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(accent, COL_BG);
  display.drawString(sessionName(), RING_X + 5, 153, 2);
  const uint16_t stateColor = timerState == TimerState::Paused ? COL_CREAM : COL_MUTED;
  display.setTextColor(stateColor, COL_BG);
  display.drawString(stateName(), RING_X, 173, 1);
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

void drawPrimaryButton(uint32_t now) {
  const bool ready = timerState == TimerState::Ready;
  const uint8_t breathLevel = ready ? breathe(now, 2000, 55, 230) : 155;
  const uint16_t edge = mixColor(COL_PRIMARY, baseAccent(), breathLevel);
  display.fillRoundRect(BTN_PRIMARY.x, BTN_PRIMARY.y + 2, BTN_PRIMARY.w, BTN_PRIMARY.h, 13, COL_BG);
  display.fillRoundRect(BTN_PRIMARY.x, BTN_PRIMARY.y, BTN_PRIMARY.w, BTN_PRIMARY.h, 13, COL_PRIMARY);
  display.drawRoundRect(BTN_PRIMARY.x, BTN_PRIMARY.y, BTN_PRIMARY.w, BTN_PRIMARY.h, 13, edge);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(COL_CREAM, COL_PRIMARY);
  display.drawString(primaryLabel(), BTN_PRIMARY.x + BTN_PRIMARY.w / 2 - (ready ? 5 : 0),
                     BTN_PRIMARY.y + BTN_PRIMARY.h / 2 + 1, 2);
  if (ready) {
    const uint16_t dot = mixColor(COL_PRIMARY, COL_PEACH, breathLevel);
    display.fillSmoothCircle(BTN_PRIMARY.x + 88, BTN_PRIMARY.y + BTN_PRIMARY.h / 2, breathLevel > 150 ? 4 : 3, dot, COL_PRIMARY);
  }
}

void drawButtons(uint32_t now) {
  drawSecondaryButton(BTN_RESET, "RESET");
  drawPrimaryButton(now);
  drawSecondaryButton(BTN_NEXT, "NEXT");
}

void drawScreen(uint32_t now) {
  drawBackdrop();
  drawHeader();
  displayedProgress = logicalProgress(now);
  lastArcAngle = lastKnobX = lastKnobY = -1;
  lastRingColor = 0;
  drawProgressRing(now, true);
  drawDigitsInstant(now);
  drawSessionStatus();
  drawButtons(now);
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
  if (timerState == TimerState::Running) {
    accumulatedMs += now - runStartedAt;
    timerState = TimerState::Paused;
  } else if (timerState == TimerState::Ready || timerState == TimerState::Paused) {
    runStartedAt = now;
    timerState = TimerState::Running;
  } else return;

  if (previous == TimerState::Ready) {
    startPulseActive = true;
    startPulseAt = now;
  }
  logState();
  drawScreen(now);
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
  timerState = autoStart ? TimerState::Running : TimerState::Ready;
  runStartedAt = now;
  startPulseActive = false;
  logState();
  drawScreen(now);
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
  updateDigitTargets(now);
  updateDigitFade(now);
  updateStartPulse(now);

  if (now - lastRingFrame >= 28) drawProgressRing(now);
  if (timerState == TimerState::Ready && now - lastReadyFrame >= 50) {
    lastReadyFrame = now;
    drawPrimaryButton(now);
  }
}

void pollTouch(uint32_t now) {
  static uint32_t lastPoll = 0;
  static uint8_t stableSamples = 0;
  static bool held = false;
  static int16_t lastX = 0, lastY = 0;
  if (now - lastPoll < 18) return;
  lastPoll = now;

  int16_t x = 0, y = 0;
  const bool pressed = readTouch(x, y);
  if (pressed) {
    if (stableSamples == 0 || (abs(x - lastX) < 25 && abs(y - lastY) < 25)) {
      if (stableSamples < 10) ++stableSamples;
    } else stableSamples = 1;
    lastX = x;
    lastY = y;
    if (stableSamples >= 3) held = true;
    return;
  }

  if (held) {
    if (inside(BTN_PRIMARY, lastX, lastY)) startOrPause(now);
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
  digitalWrite(BACKLIGHT_PIN, HIGH);
  initTouch();
  display.init();
  display.setRotation(1);

  digitSprite.setColorDepth(16);
  if (!digitSprite.createSprite(DIGIT_W, DIGIT_H)) Serial.println("[fatal] digit sprite allocation failed");

  display.fillScreen(COL_BG);
  drawLeaf(149, 92, COL_GREEN, 2);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(COL_MUTED, COL_BG);
  display.drawString("ESP32 FOCUS", RING_X, 130, 2);
  delay(380);

  drawScreen(millis());
  logState();
  Serial.println("[ready] botanical UI + touch controls active");
#ifdef ESP32_FOCUS_SELF_TEST
  Serial.println("[self-test] accelerated cycle started");
  startOrPause(millis());
#endif
}

void loop() {
  const uint32_t now = millis();
  pollTouch(now);

  if (timerState == TimerState::Running && elapsedMs(now) >= sessionDurationMs()) {
    accumulatedMs = sessionDurationMs();
    timerState = TimerState::Complete;
    completedAt = now;
    logState();
    drawScreen(now);
  } else if (timerState == TimerState::Complete && now - completedAt >= completeHoldMs()) {
    advanceSession(now, true);
  }

  updateAnimations(now);
  if (now - lastHealthLog >= 60000UL) {
    lastHealthLog = now;
    Serial.printf("[health] uptime=%lus heap=%u state=%s\n", now / 1000UL,
                  ESP.getFreeHeap(), timerState == TimerState::Running ? "running" : "idle");
  }
  delay(2);
}
