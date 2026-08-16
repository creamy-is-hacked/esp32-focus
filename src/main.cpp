#include <Arduino.h>
#include <TFT_eSPI.h>
#include <esp_system.h>

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
constexpr uint32_t COMPLETE_HOLD_MS = 1400;

#ifdef ESP32_FOCUS_SELF_TEST
constexpr uint32_t TEST_FOCUS_MS = 2000;
constexpr uint32_t TEST_SHORT_BREAK_MS = 1000;
constexpr uint32_t TEST_LONG_BREAK_MS = 1500;
constexpr uint32_t TEST_COMPLETE_HOLD_MS = 300;
#endif

// Palette: restrained near-black with one state-specific accent.
constexpr uint16_t COL_BG = 0x0861;       // #0b0d10
constexpr uint16_t COL_PANEL = 0x10C3;    // #12161b
constexpr uint16_t COL_PANEL_HI = 0x18E5; // #1a1d25
constexpr uint16_t COL_TEXT = 0xF7BE;     // #f4f7f4
constexpr uint16_t COL_MUTED = 0x7BCF;    // #7c7a7c
constexpr uint16_t COL_DIM = 0x31A8;      // #343534
constexpr uint16_t COL_FOCUS = 0x7D5F;    // cool periwinkle
constexpr uint16_t COL_SHORT = 0x6674;    // soft mint
constexpr uint16_t COL_LONG = 0xED4B;     // warm amber
constexpr uint16_t COL_PAUSED = 0xDEAB;   // muted gold
constexpr uint16_t COL_COMPLETE = 0xDFFF; // pale ice

enum class Session : uint8_t { Focus, ShortBreak, LongBreak };
enum class TimerState : uint8_t { Ready, Running, Paused, Complete };

struct Button {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

constexpr Button BTN_RESET{18, 188, 74, 38};
constexpr Button BTN_PRIMARY{101, 181, 118, 52};
constexpr Button BTN_NEXT{228, 188, 74, 38};

TFT_eSPI display;
TFT_eSprite timeSprite(&display);

Session session = Session::Focus;
TimerState timerState = TimerState::Ready;
uint8_t cycleFocuses = 0;
uint32_t accumulatedMs = 0;
uint32_t runStartedAt = 0;
uint32_t completedAt = 0;
uint32_t lastHealthLog = 0;
int32_t lastShownSecond = -1;

uint32_t sessionDurationMs() {
#ifdef ESP32_FOCUS_SELF_TEST
  switch (session) {
    case Session::Focus: return TEST_FOCUS_MS;
    case Session::ShortBreak: return TEST_SHORT_BREAK_MS;
    case Session::LongBreak: return TEST_LONG_BREAK_MS;
  }
#else
  switch (session) {
    case Session::Focus: return uint32_t(FOCUS_MINUTES) * 60UL * 1000UL;
    case Session::ShortBreak: return uint32_t(SHORT_BREAK_MINUTES) * 60UL * 1000UL;
    case Session::LongBreak: return uint32_t(LONG_BREAK_MINUTES) * 60UL * 1000UL;
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

uint16_t sessionAccent() {
  if (timerState == TimerState::Paused) return COL_PAUSED;
  if (timerState == TimerState::Complete) return COL_COMPLETE;
  switch (session) {
    case Session::Focus: return COL_FOCUS;
    case Session::ShortBreak: return COL_SHORT;
    case Session::LongBreak: return COL_LONG;
  }
  return COL_FOCUS;
}

const char* sessionName() {
  switch (session) {
    case Session::Focus: return "FOCUS";
    case Session::ShortBreak: return "SHORT BREAK";
    case Session::LongBreak: return "LONG BREAK";
  }
  return "FOCUS";
}

const char* stateName() {
  switch (timerState) {
    case TimerState::Ready: return "Ready when you are";
    case TimerState::Running:
      return session == Session::Focus ? "Make this minute count" : "Breathe. Reset. Return.";
    case TimerState::Paused: return "Paused";
    case TimerState::Complete: return "Session complete";
  }
  return "Ready";
}

const char* primaryLabel() {
  return timerState == TimerState::Running ? "PAUSE" :
         timerState == TimerState::Paused ? "RESUME" : "START";
}

uint32_t elapsedMs(uint32_t now) {
  if (timerState == TimerState::Running) return accumulatedMs + (now - runStartedAt);
  if (timerState == TimerState::Complete) return sessionDurationMs();
  return accumulatedMs;
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
  touchReadAdc(0xD0); // settle the XPT2046 ADC
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

void centeredText(const char* text, int16_t y, uint8_t font, uint16_t color) {
  display.setTextDatum(TC_DATUM);
  display.setTextFont(font);
  display.setTextColor(color, COL_BG);
  display.drawString(text, SCREEN_W / 2, y);
}

void drawCycleDots(uint16_t accent) {
  constexpr int16_t startX = 249;
  for (uint8_t i = 0; i < FOCUSES_PER_CYCLE; ++i) {
    const int16_t x = startX + i * 15;
    if (i < cycleFocuses) {
      display.fillCircle(x, 18, 3, accent);
    } else {
      display.drawCircle(x, 18, 3, COL_MUTED);
    }
  }
}

void drawButton(const Button& button, const char* label, bool primary, uint16_t accent) {
  const uint16_t fill = primary ? accent : COL_PANEL;
  const uint16_t text = primary ? COL_BG : COL_MUTED;
  if (primary) display.fillRoundRect(button.x, button.y + 3, button.w, button.h, 13, COL_PANEL_HI);
  display.fillRoundRect(button.x, button.y, button.w, button.h, primary ? 13 : 10, fill);
  if (!primary) display.drawRoundRect(button.x, button.y, button.w, button.h, 10, COL_PANEL_HI);
  display.setTextDatum(MC_DATUM);
  display.setTextFont(2);
  display.setTextColor(text, fill);
  display.drawString(label, button.x + button.w / 2, button.y + button.h / 2 + 1);
}

void drawProgress(uint32_t elapsed, uint16_t accent) {
  constexpr int16_t x = 22;
  constexpr int16_t y = 157;
  constexpr int16_t width = 276;
  constexpr int16_t height = 6;
  display.fillRoundRect(x, y, width, height, 3, COL_PANEL_HI);
  const uint32_t duration = sessionDurationMs();
  const int16_t filled = duration ? int16_t((uint64_t(width) * min(elapsed, duration)) / duration) : 0;
  if (filled >= height) {
    display.fillRoundRect(x, y, filled, height, 3, accent);
    display.fillCircle(x + filled - 3, y + 3, 3, accent);
  } else if (filled > 0) {
    display.fillCircle(x + 3, y + 3, 3, accent);
  }
}

void drawTime(uint32_t now, bool force = false) {
  const uint32_t duration = sessionDurationMs();
  const uint32_t elapsed = min(elapsedMs(now), duration);
  const uint32_t remaining = duration - elapsed;
  const int32_t totalSeconds = int32_t((remaining + 999) / 1000);
  if (!force && totalSeconds == lastShownSecond) return;
  lastShownSecond = totalSeconds;

  char timeText[8];
  snprintf(timeText, sizeof(timeText), "%02ld:%02ld", long(totalSeconds / 60), long(totalSeconds % 60));
  timeSprite.fillSprite(COL_BG);
  timeSprite.setTextDatum(MC_DATUM);
  timeSprite.setTextFont(8);
  timeSprite.setTextColor(COL_TEXT, COL_BG);
  timeSprite.drawString(timeText, 138, 43);
  timeSprite.pushSprite(22, 52);
  drawProgress(elapsed, sessionAccent());
}

void drawScreen(uint32_t now) {
  const uint16_t accent = sessionAccent();
  display.fillScreen(COL_BG);

  display.fillRoundRect(13, 10, 5, 17, 2, accent);
  display.setTextDatum(TL_DATUM);
  display.setTextFont(2);
  display.setTextColor(COL_TEXT, COL_BG);
  display.drawString(sessionName(), 27, 11);
  drawCycleDots(accent);
  display.drawFastHLine(20, 37, 280, COL_PANEL_HI);

  lastShownSecond = -1;
  drawTime(now, true);
  display.fillRect(0, 130, SCREEN_W, 18, COL_BG);
  centeredText(stateName(), 132, 2, timerState == TimerState::Paused ? accent : COL_MUTED);

  drawButton(BTN_RESET, "RESET", false, accent);
  drawButton(BTN_PRIMARY, primaryLabel(), true, accent);
  drawButton(BTN_NEXT, "NEXT", false, accent);
}

void logState() {
  Serial.printf("[state] %s | %s | cycle %u/%u\n", sessionName(),
                timerState == TimerState::Running ? "running" :
                timerState == TimerState::Paused ? "paused" :
                timerState == TimerState::Complete ? "complete" : "ready",
                cycleFocuses, FOCUSES_PER_CYCLE);
}

void startOrPause(uint32_t now) {
  if (timerState == TimerState::Running) {
    accumulatedMs += now - runStartedAt;
    timerState = TimerState::Paused;
  } else if (timerState == TimerState::Ready || timerState == TimerState::Paused) {
    runStartedAt = now;
    timerState = TimerState::Running;
  } else {
    return;
  }
  logState();
  drawScreen(now);
}

void resetSession(uint32_t now) {
  accumulatedMs = 0;
  timerState = TimerState::Ready;
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
  logState();
  drawScreen(now);
}

void skipSession(uint32_t now) {
  Serial.println("[control] next session");
  advanceSession(now, false);
}

void pollTouch(uint32_t now) {
  static uint32_t lastPoll = 0;
  static uint8_t stableSamples = 0;
  static bool held = false;
  static int16_t lastX = 0;
  static int16_t lastY = 0;
  if (now - lastPoll < 18) return;
  lastPoll = now;

  int16_t x = 0;
  int16_t y = 0;
  const bool pressed = readTouch(x, y);
  if (pressed) {
    if (stableSamples == 0 || (abs(x - lastX) < 25 && abs(y - lastY) < 25)) {
      stableSamples = min<uint8_t>(stableSamples + 1, 10);
    } else {
      stableSamples = 1;
    }
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
  display.fillScreen(COL_BG);
  timeSprite.setColorDepth(16);
  if (!timeSprite.createSprite(276, 86)) {
    Serial.println("[fatal] timer sprite allocation failed");
  }

  display.fillRoundRect(143, 79, 34, 34, 10, COL_FOCUS);
  display.fillCircle(160, 96, 5, COL_BG);
  centeredText("ESP32 FOCUS", 125, 2, COL_MUTED);
  delay(420);

  drawScreen(millis());
  logState();
  Serial.println("[ready] touch controls active");
#ifdef ESP32_FOCUS_SELF_TEST
  timerState = TimerState::Running;
  accumulatedMs = 0;
  runStartedAt = millis();
  Serial.println("[self-test] accelerated cycle started");
  drawScreen(runStartedAt);
#endif
}

void loop() {
  const uint32_t now = millis();
  pollTouch(now);

  if (timerState == TimerState::Running) {
    if (elapsedMs(now) >= sessionDurationMs()) {
      accumulatedMs = sessionDurationMs();
      timerState = TimerState::Complete;
      completedAt = now;
      logState();
      drawScreen(now);
    } else {
      drawTime(now);
    }
  } else if (timerState == TimerState::Complete && now - completedAt >= completeHoldMs()) {
    advanceSession(now, true);
  }

  if (now - lastHealthLog >= 60000UL) {
    lastHealthLog = now;
    Serial.printf("[health] uptime=%lus heap=%u state=%s\n", now / 1000UL,
                  ESP.getFreeHeap(), timerState == TimerState::Running ? "running" : "idle");
  }
  delay(2);
}
