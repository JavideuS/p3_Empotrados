// Práctica 3 — Vending machine controller
#include <Arduino.h>
#include <avr/wdt.h>
#include <LiquidCrystal.h>
#include <DHT.h>
#include <Thread.h>
#include <ThreadController.h>

// ---- Pin map (README.md §1) -----------------------------------------------
constexpr uint8_t PIN_BUTTON = 2;  // external button: INT0, active LOW, INPUT_PULLUP
constexpr uint8_t PIN_LED1 = 4;
constexpr uint8_t PIN_LED2 = 5;  // PWM
constexpr uint8_t PIN_DHT = 6;
constexpr uint8_t PIN_TRIG = 7;
constexpr uint8_t PIN_ECHO = 8;
constexpr uint8_t PIN_JOY_X = A0;
constexpr uint8_t PIN_JOY_Y = A1;
constexpr uint8_t PIN_JOY_SW = A2;  // active LOW, INPUT_PULLUP
constexpr uint8_t PIN_SEED = A5;    // left floating: analog noise seeds random()

//                RS  E   D4  D5 D6 D7
LiquidCrystal lcd(12, 11, 10, 9, 3, 13);

// ---- Custom LCD characters (8 slots, 0..7) ----------------------------------
// Slot 0 is avoided: char 0 would terminate C strings.
// Slots 1..4: progress bar cells with 1..4 of 5 pixel columns filled.
// A full cell is 0xFF (solid block, built into the HD44780 ROM).
constexpr uint8_t CH_BAR1 = 1;     // ..CH_BAR1 + 3
constexpr uint8_t CH_E_ACUTE = 5;  // é
constexpr uint8_t CH_FULL = 0xFF;

uint8_t glyphBar[4][8];  // filled in setup()
uint8_t glyphEAcute[8] = {
  0b00010, 0b00100, 0b01110, 0b10001, 0b11111, 0b10000, 0b01110, 0b00000,
};

#ifdef SIMULATION
DHT dht(PIN_DHT, DHT22);  // Wokwi only has a DHT22
#else
DHT dht(PIN_DHT, DHT11);
#endif

// ---- Watchdog -------------------------------------------------------------
// If loop() stops running (hang, stuck library call), the MCU resets itself
// and starts again from BOOT. The slowest normal iteration is a DHT read
// (~25 ms) or a sonar timeout (<= ~75 ms), far below 2 s.
constexpr uint8_t WDT_TIMEOUT = WDTO_2S;

// ---- Timing ---------------------------------------------------------------
constexpr uint8_t BOOT_BLINKS = 3;
constexpr uint16_t BOOT_PERIOD_MS = 1000;  // one blink = 500 ms on + 500 ms off
constexpr uint16_t MONITOR_MS = 5000;      // temp + humidity screen
constexpr uint16_t PREP_MIN_MS = 4000;     // preparation time: random in [4, 8] s
constexpr uint16_t PREP_MAX_MS = 8000;
constexpr uint16_t RETIRE_MS = 3000;  // "RETIRE BEBIDA" screen

// ---- External button ------------------------------------------------------
// The ISR only timestamps edges; the decision is made in loop() (handleButton).
// A 5 s hold passes through the 2–3 s window, so cancel is decided on RELEASE,
// while the long press fires as soon as 5 s are reached (while still held).
constexpr uint16_t BTN_DEBOUNCE_MS = 30;
constexpr uint16_t CANCEL_MIN_MS = 2000;  // release in [2, 3] s → cancel
constexpr uint16_t CANCEL_MAX_MS = 3000;
constexpr uint16_t ADMIN_HOLD_MS = 5000;  // held >= 5 s → toggle admin

volatile bool btnDown = false;           // debounced level
volatile uint32_t btnLastEdge = 0;       // millis() of the last accepted edge
volatile uint32_t btnPressStart = 0;     // millis() when the current press began
volatile uint32_t btnPressDuration = 0;  // length of the last completed press
volatile bool btnReleased = false;       // a press completed (consumed by loop)

// ---- Preparation ----------------------------------------------------------
constexpr uint8_t BAR_CELLS = 12;  // progress bar width (cols 0..11), then " 45%"
uint16_t prepDuration = 0;         // ms, chosen on entering PREPARING

// ---- Distance sensor (HC-SR04) ---------------------------------------------
constexpr uint16_t SONAR_PERIOD_MS = 100;     // HC-SR04 needs >= 60 ms between pings
constexpr uint32_t SONAR_TIMEOUT_US = 25000;  // ~4 m max range, bounds pulseIn() blocking
constexpr int16_t PRESENCE_CM = 100;          // customer if < 1 m
constexpr uint8_t PRESENCE_READINGS = 2;      // consecutive near readings to confirm
constexpr int16_t DIST_NONE = -1;             // no echo (nothing in range)

int16_t distanceCm = DIST_NONE;  // last reading, cached
bool customerPresent = false;    // debounced presence flag

// ---- Temperature / humidity (DHT11) ----------------------------------------
// The DHT11 can't be sampled faster than ~1 Hz, and each read blocks ~25 ms
// with interrupts disabled, so read it rarely and cache the result.
constexpr uint16_t DHT_PERIOD_MS = 2000;

float temperature = NAN;  // °C, NAN until the first valid reading
float humidity = NAN;     // %
bool envUpdated = false;  // set when a new reading arrives (for redraws)

// ---- Joystick -------------------------------------------------------------
// Axis readings go 0..1023 with ~512 at rest. An event fires once when the
// stick leaves the dead zone, and re-arms only after it returns near the
// centre (hysteresis), so one push = one event.
// If the directions come out reversed on the real module, flip these.
#ifdef SIMULATION
constexpr bool JOY_UP_IS_HIGH = true;    // Wokwi: pushing up → VERT 1023
constexpr bool JOY_LEFT_IS_HIGH = true;  // Wokwi: pushing left → HORZ 1023
#else
constexpr bool JOY_UP_IS_HIGH = false;    // kit module: pushing up → VERT 0
constexpr bool JOY_LEFT_IS_HIGH = false;  // kit module: pushing left → HORZ 0
#endif
constexpr int JOY_LOW = 300;  // beyond these → direction
constexpr int JOY_HIGH = 723;
constexpr int JOY_REARM_LOW = 400;  // back inside these → re-armed
constexpr int JOY_REARM_HIGH = 623;
constexpr uint8_t JOY_SW_DEBOUNCE_MS = 30;

enum class JoyEvent : uint8_t
{
  NONE,
  UP,
  DOWN,
  LEFT,
  RIGHT,
  PRESS,
};

// ---- Products -------------------------------------------------------------
// Array order = display order. Names are at most 11 chars so a menu line
// fits in 16: cursor (1) + name (11) + price (4, "1.00").
// Prices in cents: exact ±5 steps for admin, no float rounding.
struct Product
{
  const char* name;
  uint16_t priceCents;
};

Product products[] = {
  { "Cafe Solo", 100 }, { "C. Cortado", 110 }, { "Cafe Doble", 125 }, { "C. Premium", 150 }, { "Chocolate", 200 },
};
constexpr uint8_t PRODUCT_COUNT = sizeof(products) / sizeof(products[0]);

constexpr uint16_t PRICE_STEP_CENTS = 5;  // admin edits in 0.05 steps
constexpr uint16_t PRICE_MIN_CENTS = 5;
constexpr uint16_t PRICE_MAX_CENTS = 995;  // keeps the price at 4 chars ("9.95")

// ---- Scrolling lists ------------------------------------------------------
// A 2-line window over `count` items with a '>' cursor. `format` writes the
// 15 chars that follow the cursor for item i. Used by the product menu, the
// admin menu and the admin price list.
struct ListView
{
  uint8_t count;
  void (*format)(uint8_t i, char* out);  // out has room for 16 chars + '\0'
  uint8_t selected;
  uint8_t top;  // item shown on the first LCD line
};

void formatProduct(uint8_t i, char* out)
{
  uint16_t c = products[i].priceCents;
  snprintf(out, 16, "%-11s%u.%02u", products[i].name, c / 100, c % 100);
}

// Spec order: a) temperatura, b) distancia, c) contador, d) precios.
const char* const ADMIN_ITEMS[] = {
  "Ver temperatura",
  "Ver distancia",
  "Ver contador",
  "Modif. precios",
};
constexpr uint8_t ADMIN_ITEM_COUNT = sizeof(ADMIN_ITEMS) / sizeof(ADMIN_ITEMS[0]);

void formatAdminItem(uint8_t i, char* out)
{
  snprintf(out, 16, "%-15s", ADMIN_ITEMS[i]);
}

ListView productMenu = { PRODUCT_COUNT, formatProduct, 0, 0 };
ListView adminMenu = { ADMIN_ITEM_COUNT, formatAdminItem, 0, 0 };
ListView priceList = { PRODUCT_COUNT, formatProduct, 0, 0 };

uint16_t editCents = 0;  // price being edited (committed only on press)

// ---- FSM ------------------------------------------------------------------
enum class State : uint8_t
{
  BOOT,       // only entered from setup(): power-on or reset
  WAITING,    // "ESPERANDO CLIENTE"
  MONITOR,    // temp + humidity for 5 s
  MENU,       // product list, joystick navigation
  PREPARING,  // random 4–8 s, LED2 + LCD bar show progress
  RETIRE,     // "RETIRE BEBIDA" for 3 s
  CANCEL,     // button released after 2–3 s: reset, then straight to WAITING

  // Admin (hold >= 5 s from anywhere to enter, again to leave). LED1+LED2 on.
  ADMIN_MENU,        // 4 options, joystick up/down + press
  ADMIN_TEMP,        // live temp + humidity      (LEFT → ADMIN_MENU)
  ADMIN_DIST,        // live distance             (LEFT → ADMIN_MENU)
  ADMIN_COUNTER,     // seconds since power-on    (LEFT → ADMIN_MENU)
  ADMIN_PRICES,      // product list              (LEFT → ADMIN_MENU)
  ADMIN_PRICE_EDIT,  // ±0.05, press = save, LEFT = discard (→ ADMIN_PRICES)
};

bool isAdmin(State s)
{
  switch (s)
  {
    case State::ADMIN_MENU:
    case State::ADMIN_TEMP:
    case State::ADMIN_DIST:
    case State::ADMIN_COUNTER:
    case State::ADMIN_PRICES:
    case State::ADMIN_PRICE_EDIT:
      return true;
    default:
      return false;
  }
}

State state;
uint32_t stateStart;  // millis() when the current state was entered

// ---- Threads --------------------------------------------------------------
ThreadController threads;
Thread sonarThread;
Thread dhtThread;

int16_t readDistanceCm()
{
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  uint32_t us = pulseIn(PIN_ECHO, HIGH, SONAR_TIMEOUT_US);
  if (us == 0)
    return DIST_NONE;  // timeout: nothing in range
  return us / 58;      // round trip at ~343 m/s → 58 µs per cm
}

void sonarRun()
{
  static uint8_t nearCount = 0;

  distanceCm = readDistanceCm();
  bool near = distanceCm != DIST_NONE && distanceCm < PRESENCE_CM;

  // Require several consecutive near readings so a single spurious echo
  // doesn't wake the machine. Leaving is reported on the first far reading.
  if (near)
  {
    if (nearCount < PRESENCE_READINGS)
      nearCount++;
  }
  else
  {
    nearCount = 0;
  }
  customerPresent = nearCount >= PRESENCE_READINGS;
}

void dhtRun()
{
  float t = dht.readTemperature();
  float h = dht.readHumidity();  // reuses the same sample, no second bus read

  // Keep the last good value if a read fails (checksum/timeout → NAN).
  if (!isnan(t) && t != temperature)
  {
    temperature = t;
    envUpdated = true;
  }
  if (!isnan(h) && h != humidity)
  {
    humidity = h;
    envUpdated = true;
  }
}

// ---- Input ----------------------------------------------------------------
// External button ISR (CHANGE on INT0). Accepts an edge only if the level
// really changed and the previous accepted edge is older than the debounce
// window. No delay(), no Serial, nothing slow: just timestamps.
void isrButton()
{
  uint32_t now = millis();  // doesn't advance inside an ISR, but reading it is fine
  bool down = digitalRead(PIN_BUTTON) == LOW;

  if (down == btnDown)
    return;  // bounce pair: level back where it was
  if (now - btnLastEdge < BTN_DEBOUNCE_MS)
    return;  // too soon after the last edge: bounce

  btnLastEdge = now;
  btnDown = down;
  if (down)
  {
    btnPressStart = now;
  }
  else
  {
    btnPressDuration = now - btnPressStart;
    btnReleased = true;
  }
}

bool cancelAllowed();  // defined with the FSM below
void onCancel();
void onLongPress();

// Called every loop(): turns ISR timestamps into cancel / long-press actions.
void handleButton()
{
  noInterrupts();
  // If the final edge of a bounce burst was swallowed by the debounce window,
  // the ISR's level is stale. Re-running the ISR logic resyncs it.
  if (btnDown != (digitalRead(PIN_BUTTON) == LOW))
    isrButton();

  // Copy shared state atomically (32-bit reads aren't atomic on AVR).
  bool down = btnDown;
  uint32_t start = btnPressStart;
  bool released = btnReleased;
  uint32_t duration = btnPressDuration;
  btnReleased = false;
  interrupts();

  static bool longFired = false;  // this press already triggered the long action
  uint32_t held = down ? millis() - start : 0;

  // Feedback: LED1 is lit while releasing now would cancel. LED1 is otherwise
  // unused in the service states, so it's free to act as the indicator.
  // `hintOn` tracks our own use of LED1 so we only ever switch off what we
  // switched on (BOOT blinks it and ADMIN keeps it on: don't fight them).
  static bool hintOn = false;
  bool hint = cancelAllowed() && held >= CANCEL_MIN_MS && held <= CANCEL_MAX_MS;
  if (hint != hintOn)
  {
    hintOn = hint;
    digitalWrite(PIN_LED1, hint ? HIGH : LOW);
  }

  if (down && !longFired && held >= ADMIN_HOLD_MS)
  {
    longFired = true;
    onLongPress();
  }

  if (released)
  {
    // A press that already fired the long action never also cancels.
    if (!longFired && duration >= CANCEL_MIN_MS && duration <= CANCEL_MAX_MS)
      onCancel();
    longFired = false;
  }
}

// One axis → one of two directions, with hysteresis. `armed` is per-axis state.
JoyEvent axisEvent(int raw, bool& armed, bool highIsFirst, JoyEvent first, JoyEvent second)
{
  if (!armed)
  {
    if (raw > JOY_REARM_LOW && raw < JOY_REARM_HIGH)
      armed = true;
    return JoyEvent::NONE;
  }
  if (raw > JOY_HIGH)
  {
    armed = false;
    return highIsFirst ? first : second;
  }
  if (raw < JOY_LOW)
  {
    armed = false;
    return highIsFirst ? second : first;
  }
  return JoyEvent::NONE;
}

// Polled every loop(). Returns at most one event per call.
JoyEvent readJoystick()
{
  // Button: event on a debounced HIGH → LOW transition.
  static bool swStable = HIGH;
  static bool swLastRaw = HIGH;
  static uint32_t swLastChange = 0;

  bool raw = digitalRead(PIN_JOY_SW);
  uint32_t now = millis();
  if (raw != swLastRaw)
  {
    swLastRaw = raw;
    swLastChange = now;
  }
  else if (raw != swStable && now - swLastChange >= JOY_SW_DEBOUNCE_MS)
  {
    swStable = raw;
    if (swStable == LOW)
      return JoyEvent::PRESS;
  }

  static bool yArmed = true;
  static bool xArmed = true;
  JoyEvent e = axisEvent(analogRead(PIN_JOY_Y), yArmed, JOY_UP_IS_HIGH, JoyEvent::UP, JoyEvent::DOWN);
  if (e != JoyEvent::NONE)
    return e;
  return axisEvent(analogRead(PIN_JOY_X), xArmed, JOY_LEFT_IS_HIGH, JoyEvent::LEFT, JoyEvent::RIGHT);
}

// ---- Screens --------------------------------------------------------------
void printValue2(float v)
{
  if (isnan(v))
  {
    lcd.print(F("--"));
    return;
  }
  char buf[8];
  snprintf(buf, sizeof buf, "%2d", (int)lround(v));
  lcd.print(buf);
}

// Seconds left in a countdown, rounded up (5000 ms left → 5, 1 ms left → 1).
uint8_t secondsLeft(uint32_t elapsed, uint32_t total)
{
  return elapsed >= total ? 0 : (total - elapsed + 999) / 1000;
}

void drawMonitor()
{
  // 0xDF is the degree sign in the HD44780 character ROM.
  lcd.setCursor(0, 0);
  lcd.print(F("Temp: "));
  printValue2(temperature);
  lcd.print((char)0xDF);
  lcd.print(F("C"));

  lcd.setCursor(0, 1);
  lcd.print(F("Hum:  "));
  printValue2(humidity);
  lcd.print(F(" %"));
}

void drawCountdown(uint8_t s)
{
  lcd.setCursor(14, 0);  // top-right corner: "5s"
  lcd.print(s);
  lcd.print('s');
}

// Prints `text` on `row`, padded with spaces to the full 16 columns, so the
// previous content is overwritten without an lcd.clear() (no flicker).
void printRow(uint8_t row, const char* text)
{
  char line[17];
  snprintf(line, sizeof line, "%-16s", text);
  lcd.setCursor(0, row);
  lcd.print(line);
}

// Draws the 2-line window of a list. Every line is exactly 16 chars.
void drawList(const ListView& list)
{
  for (uint8_t row = 0; row < 2; row++)
  {
    uint8_t i = list.top + row;
    char line[17] = "";
    if (i < list.count)
    {
      line[0] = i == list.selected ? '>' : ' ';
      list.format(i, line + 1);
    }
    printRow(row, line);
  }
}

void listReset(ListView& list)
{
  list.selected = 0;
  list.top = 0;
}

// Moves the selection by ±1 (clamped) and scrolls the window to keep it visible.
void listMove(ListView& list, int8_t delta)
{
  int8_t next = (int8_t)list.selected + delta;
  if (next < 0 || next >= list.count)
    return;
  list.selected = next;

  if (list.selected < list.top)
    list.top = list.selected;
  else if (list.selected > list.top + 1)
    list.top = list.selected - 1;

  drawList(list);
}

// ---- Admin screens --------------------------------------------------------
void drawAdminDist()
{
  char buf[17];
  if (distanceCm == DIST_NONE)
    snprintf(buf, sizeof buf, "Fuera de rango");
  else
    snprintf(buf, sizeof buf, "%d cm", distanceCm);
  printRow(1, buf);
}

void drawAdminCounter(uint32_t seconds)
{
  char buf[17];
  snprintf(buf, sizeof buf, "%lu s", (unsigned long)seconds);
  printRow(1, buf);
}

// Line 1: product with its current (saved) price. Line 2: the new price.
void drawPriceEdit()
{
  char buf[17];
  formatProduct(priceList.selected, buf);
  printRow(0, buf);
  snprintf(buf, sizeof buf, "Nuevo:     %u.%02u", editCents / 100, editCents % 100);
  printRow(1, buf);
}

// Line 2 during preparation: 12-cell bar (60 pixel columns) + " 45%".
void drawProgress(uint8_t pct)
{
  uint8_t filled = (uint16_t)pct * BAR_CELLS * 5 / 100;  // pixel columns lit

  lcd.setCursor(0, 1);
  for (uint8_t i = 0; i < BAR_CELLS; i++)
  {
    uint8_t cols = filled > i * 5 ? filled - i * 5 : 0;
    if (cols >= 5)
      lcd.write(CH_FULL);
    else if (cols == 0)
      lcd.write(' ');
    else
      lcd.write((uint8_t)(CH_BAR1 + cols - 1));
  }

  char buf[6];
  snprintf(buf, sizeof buf, "%3u%%", pct);
  lcd.print(buf);
}

// ---- FSM implementation ---------------------------------------------------
void enterState(State next)
{
  state = next;
  stateStart = millis();
  lcd.clear();

  switch (state)
  {
    case State::BOOT:
      lcd.print(F("CARGANDO ..."));
      break;

    case State::WAITING:
      lcd.print(F("ESPERANDO"));
      lcd.setCursor(0, 1);
      lcd.print(F("CLIENTE"));
      break;

    case State::MONITOR:
      envUpdated = false;
      drawMonitor();
      drawCountdown(secondsLeft(0, MONITOR_MS));
      break;

    case State::MENU:
      listReset(productMenu);
      drawList(productMenu);
      break;

    case State::PREPARING: {
      // random(min, max) excludes max, hence +1. Re-roll on a repeat so two
      // consecutive preparations never take the same time.
      uint16_t last = prepDuration;
      do
        prepDuration = random(PREP_MIN_MS, PREP_MAX_MS + 1);
      while (prepDuration == last);

      lcd.print(F("Preparando Caf"));
      lcd.write(CH_E_ACUTE);
      drawProgress(0);
      analogWrite(PIN_LED2, 0);
      break;
    }

    case State::RETIRE:
      lcd.print(F("RETIRE BEBIDA"));
      drawCountdown(secondsLeft(0, RETIRE_MS));
      break;

    case State::CANCEL:
      // Reset everything a service-b state may have left behind. Menu
      // position and preparation time are re-initialised on their own entry.
      analogWrite(PIN_LED2, 0);
      break;

    // Admin: list positions are NOT reset here, so going back with LEFT
    // returns to the item you came from. They're reset when entering admin
    // (adminMenu) or when opening the price list from the admin menu.
    case State::ADMIN_MENU:
      drawList(adminMenu);
      break;

    case State::ADMIN_TEMP:
      envUpdated = false;
      drawMonitor();
      break;

    case State::ADMIN_DIST:
      lcd.print(F("Distancia:"));
      drawAdminDist();
      break;

    case State::ADMIN_COUNTER:
      lcd.print(F("Contador:"));
      drawAdminCounter(millis() / 1000);
      break;

    case State::ADMIN_PRICES:
      drawList(priceList);
      break;

    case State::ADMIN_PRICE_EDIT:
      editCents = products[priceList.selected].priceCents;
      drawPriceEdit();
      break;
  }
}

// Cancel only applies to the service interaction (spec 3.2 apartado b):
// MONITOR, MENU, PREPARING and RETIRE. Ignored in BOOT and WAITING.
bool cancelAllowed()
{
  switch (state)
  {
    case State::MONITOR:
    case State::MENU:
    case State::PREPARING:
    case State::RETIRE:
      return true;
    default:
      return false;
  }
}

void onCancel()
{
  if (cancelAllowed())
    enterState(State::CANCEL);
}

// Hold >= 5 s: toggles admin. Works from ANY state (spec: "en cualquier
// instante"), including BOOT and a preparation in progress, which is dropped.
void onLongPress()
{
  if (isAdmin(state))
  {
    digitalWrite(PIN_LED1, LOW);
    digitalWrite(PIN_LED2, LOW);
    enterState(State::WAITING);  // back to the start of Service
  }
  else
  {
    // digitalWrite also stops any PWM running on LED2 (e.g. mid-preparation).
    digitalWrite(PIN_LED1, HIGH);
    digitalWrite(PIN_LED2, HIGH);
    listReset(adminMenu);
    enterState(State::ADMIN_MENU);
  }
}

void updateBoot(uint32_t elapsed)
{
  if (elapsed >= (uint32_t)BOOT_BLINKS * BOOT_PERIOD_MS)
  {
    digitalWrite(PIN_LED1, LOW);
    enterState(State::WAITING);
    return;
  }
  // First half of each period on, second half off. Derived from elapsed time
  // (no toggle counter), so a late loop() iteration can't desync the blinks.
  bool on = (elapsed % BOOT_PERIOD_MS) < BOOT_PERIOD_MS / 2;
  digitalWrite(PIN_LED1, on ? HIGH : LOW);
}

void updateWaiting()
{
  if (customerPresent)
    enterState(State::MONITOR);
}

void updateMonitor(uint32_t elapsed)
{
  // A reading may land during the 5 s window: redraw in place (no clear).
  if (envUpdated)
  {
    envUpdated = false;
    drawMonitor();
  }

  // Only touch the LCD when the displayed second actually changes.
  static uint8_t lastShown = 0;
  uint8_t s = secondsLeft(elapsed, MONITOR_MS);
  if (s != lastShown && s > 0)
  {
    lastShown = s;
    drawCountdown(s);
  }

  if (elapsed >= MONITOR_MS)
    enterState(State::MENU);
}

void updateMenu(JoyEvent ev)
{
  switch (ev)
  {
    case JoyEvent::UP:
      listMove(productMenu, -1);
      break;
    case JoyEvent::DOWN:
      listMove(productMenu, +1);
      break;
    case JoyEvent::PRESS:
      enterState(State::PREPARING);
      break;
    default:
      break;
  }
}

// ---- Admin updates --------------------------------------------------------
void updateAdminMenu(JoyEvent ev)
{
  switch (ev)
  {
    case JoyEvent::UP:
      listMove(adminMenu, -1);
      break;
    case JoyEvent::DOWN:
      listMove(adminMenu, +1);
      break;
    case JoyEvent::PRESS:
      switch (adminMenu.selected)
      {
        case 0:
          enterState(State::ADMIN_TEMP);
          break;
        case 1:
          enterState(State::ADMIN_DIST);
          break;
        case 2:
          enterState(State::ADMIN_COUNTER);
          break;
        case 3:
          listReset(priceList);
          enterState(State::ADMIN_PRICES);
          break;
      }
      break;
    default:
      break;  // LEFT does nothing here: this is the top level
  }
}

void updateAdminTemp(JoyEvent ev)
{
  if (ev == JoyEvent::LEFT)
  {
    enterState(State::ADMIN_MENU);
    return;
  }
  if (envUpdated)  // new DHT reading (every 2 s, the sensor's limit)
  {
    envUpdated = false;
    drawMonitor();
  }
}

void updateAdminDist(JoyEvent ev)
{
  if (ev == JoyEvent::LEFT)
  {
    enterState(State::ADMIN_MENU);
    return;
  }
  static int16_t lastShown = DIST_NONE;
  if (distanceCm != lastShown)  // sonar updates every 100 ms
  {
    lastShown = distanceCm;
    drawAdminDist();
  }
}

void updateAdminCounter(JoyEvent ev)
{
  if (ev == JoyEvent::LEFT)
  {
    enterState(State::ADMIN_MENU);
    return;
  }
  static uint32_t lastShown = 0;
  uint32_t s = millis() / 1000;
  if (s != lastShown)
  {
    lastShown = s;
    drawAdminCounter(s);
  }
}

void updateAdminPrices(JoyEvent ev)
{
  switch (ev)
  {
    case JoyEvent::UP:
      listMove(priceList, -1);
      break;
    case JoyEvent::DOWN:
      listMove(priceList, +1);
      break;
    case JoyEvent::PRESS:
      enterState(State::ADMIN_PRICE_EDIT);
      break;
    case JoyEvent::LEFT:
      enterState(State::ADMIN_MENU);
      break;
    default:
      break;
  }
}

void updateAdminPriceEdit(JoyEvent ev)
{
  switch (ev)
  {
    case JoyEvent::UP:
      if (editCents + PRICE_STEP_CENTS <= PRICE_MAX_CENTS)
      {
        editCents += PRICE_STEP_CENTS;
        drawPriceEdit();
      }
      break;
    case JoyEvent::DOWN:
      if (editCents >= PRICE_MIN_CENTS + PRICE_STEP_CENTS)
      {
        editCents -= PRICE_STEP_CENTS;
        drawPriceEdit();
      }
      break;
    case JoyEvent::PRESS:  // save
      products[priceList.selected].priceCents = editCents;
      enterState(State::ADMIN_PRICES);
      break;
    case JoyEvent::LEFT:  // discard
      enterState(State::ADMIN_PRICES);
      break;
    default:
      break;
  }
}

void updatePreparing(uint32_t elapsed)
{
  if (elapsed >= prepDuration)
  {
    analogWrite(PIN_LED2, 0);
    enterState(State::RETIRE);
    return;
  }

  // LED2 duty cycle = progress: 0 % → 0, 100 % → 255.
  analogWrite(PIN_LED2, (uint32_t)255 * elapsed / prepDuration);

  // Redraw only when the shown percentage changes.
  static uint8_t lastPct = 0;
  uint8_t pct = (uint32_t)100 * elapsed / prepDuration;
  if (pct != lastPct)
  {
    lastPct = pct;
    drawProgress(pct);
  }
}

void updateRetire(uint32_t elapsed)
{
  static uint8_t lastShown = 0;
  uint8_t s = secondsLeft(elapsed, RETIRE_MS);
  if (s != lastShown && s > 0)
  {
    lastShown = s;
    drawCountdown(s);
  }

  if (elapsed >= RETIRE_MS)
    enterState(State::WAITING);
}

void updateCancel()
{
  // Pass-through: the reset happened on entry; return to the start of Service
  // immediately (spec: "regresará de inmediato al inicio del modo Servicio").
  enterState(State::WAITING);
}

void setup()
{
  // A watchdog reset leaves the WDT enabled (with the shortest timeout) and
  // WDRF set: clear both first, or setup() itself could be reset in a loop.
  MCUSR = 0;
  wdt_disable();

  pinMode(PIN_LED1, OUTPUT);
  pinMode(PIN_LED2, OUTPUT);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_JOY_SW, INPUT_PULLUP);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  digitalWrite(PIN_LED1, LOW);
  digitalWrite(PIN_LED2, LOW);
  digitalWrite(PIN_TRIG, LOW);

  dht.begin();

  sonarThread.onRun(sonarRun);
  sonarThread.setInterval(SONAR_PERIOD_MS);
  threads.add(&sonarThread);

  // First run happens one period after boot (~2 s), which also respects the
  // DHT11's ~1 s power-up settle time. Values are ready before BOOT ends (3 s).
  dhtThread.onRun(dhtRun);
  dhtThread.setInterval(DHT_PERIOD_MS);
  threads.add(&dhtThread);

  randomSeed(analogRead(PIN_SEED));

  lcd.begin(16, 2);

  // Bar glyph k (1..4 columns lit, left-aligned): every row = top k of 5 bits.
  for (uint8_t k = 1; k <= 4; k++)
  {
    uint8_t row = (0x1F << (5 - k)) & 0x1F;
    for (uint8_t r = 0; r < 8; r++)
      glyphBar[k - 1][r] = row;
    lcd.createChar(CH_BAR1 + k - 1, glyphBar[k - 1]);
  }
  lcd.createChar(CH_E_ACUTE, glyphEAcute);

  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), isrButton, CHANGE);

  enterState(State::BOOT);

  // Enabled last: setup() has slow library calls (lcd.begin) we don't want
  // to budget for.
  wdt_enable(WDT_TIMEOUT);
}

void loop()
{
  wdt_reset();  // "still alive": must happen at least once every 2 s

  threads.run();

  handleButton();  // may change state (cancel / admin) before the update below

  JoyEvent ev = readJoystick();  // polled every loop so no edge is missed
  uint32_t elapsed = millis() - stateStart;

  switch (state)
  {
    case State::BOOT:
      updateBoot(elapsed);
      break;
    case State::WAITING:
      updateWaiting();
      break;
    case State::MONITOR:
      updateMonitor(elapsed);
      break;
    case State::MENU:
      updateMenu(ev);
      break;
    case State::PREPARING:
      updatePreparing(elapsed);
      break;
    case State::RETIRE:
      updateRetire(elapsed);
      break;
    case State::CANCEL:
      updateCancel();
      break;
    case State::ADMIN_MENU:
      updateAdminMenu(ev);
      break;
    case State::ADMIN_TEMP:
      updateAdminTemp(ev);
      break;
    case State::ADMIN_DIST:
      updateAdminDist(ev);
      break;
    case State::ADMIN_COUNTER:
      updateAdminCounter(ev);
      break;
    case State::ADMIN_PRICES:
      updateAdminPrices(ev);
      break;
    case State::ADMIN_PRICE_EDIT:
      updateAdminPriceEdit(ev);
      break;
  }
}
