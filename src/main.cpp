#include <Arduino.h> // defines USBCON etc.; required before HID-Project in a .cpp
#include <HID-Project.h>
#include <EEPROM.h>
#include <avr/wdt.h>

// Keyboard-only build: this device exposes a BootKeyboard (the actual key it
// types), a Consumer interface (media keys), and a RawHID interface used by the
// MashConfig host app to reconfigure it. There is no CDC serial port
// (-DCDC_DISABLED in platformio.ini).

const int buttonPin = 2;

// debounce window (ms) — ignore state changes faster than this
const unsigned long debounceDelay = 50;

// In one-shot mode, how long the key is held down before auto-release.
const unsigned long oneShotTapMs = 10;

// Rapid-fire timing: while the button is held, the key is tapped repeatedly —
// held down for rapidDownMs, then released for rapidDelay ms. Period ≈ 16Hz default.
const unsigned long rapidDownMs = 20;

// --- Press configuration ----------------------------------------------------
// What a button press emits. keyType selects the HID interface: keyboard keys
// vs. consumer/media keys.
//
// For keyboard, the press is a *chord* — a list of up to NUM_KEYS HID usages
// held simultaneously (0 = empty slot; usage 0x00 is never a real key). Modifier
// usages 0xE0-0xE7 (L/R Ctrl/Shift/Alt/GUI) are just ordinary list members:
// BootKeyboard folds them into the report's modifier byte, so e.g. {0xE0, 0xE1,
// 0x1A} sends LeftCtrl+LeftShift+W with no special-casing. This is also how a
// bare modifier (send just RightCtrl) and multi-key combos are expressed.
//
// For consumer (media), there's a single 16-bit usage (e.g. MEDIA_VOLUME_UP
// 0xE9, MEDIA_PLAY_PAUSE 0xCD); it lives in keys[0]=lo, keys[1]=hi.
enum KeyType : uint8_t
{
  KEY_TYPE_KEYBOARD = 0,
  KEY_TYPE_CONSUMER = 1,
};

const uint8_t NUM_KEYS = 6; // max simultaneous usages (HID boot report limit)

enum PressMode : uint8_t
{
  MODE_HOLD = 0,     // key down on press, up on release (OS auto-repeats)
  MODE_ONE_SHOT = 1, // key down then up ~10ms later; held button fires once
  MODE_RAPID = 2,    // while held, tap the key repeatedly at a fixed rate
};

uint8_t keyType = KEY_TYPE_KEYBOARD;
uint8_t keys[NUM_KEYS] = {KEY_SPACE, 0, 0, 0, 0, 0}; // default: Space (0x2C)
uint8_t mode = MODE_HOLD;
uint8_t rapidDelay = 40; // gap between rapid-fire taps (ms); configurable via RawHID

// EEPROM layout: a magic byte marks that a config has been saved. Bumped to 0xA9
// when the single code+modifiers pair became a keys[NUM_KEYS] chord; stale
// configs reset to default. Bump again if the encoding changes.
const int eepromMagicAddr = 0;      // [0]      magic
const int eepromTypeAddr = 1;       // [1]      keyType
const int eepromModeAddr = 2;       // [2]      mode
const int eepromRapidDelayAddr = 3; // [3]      rapidDelay (ms)
const int eepromKeysAddr = 4;       // [4..9]   keys[NUM_KEYS]
const uint8_t eepromMagic = 0xA9;

bool keyIsPressed = false;          // true while the key is currently held down
bool pendingRelease = false;        // one-shot: a release is scheduled
unsigned long pressedAt = 0;        // millis() when the one-shot press happened
unsigned long rapidPhaseAt = 0;     // rapid-fire: when the current tap phase began

int previousButtonState = HIGH;     // last debounced state
int lastReading = HIGH;             // last raw reading
unsigned long lastDebounceTime = 0; // time of last raw change

// Receive buffer for RawHID output reports from the host (one endpoint packet).
uint8_t rawhidBuffer[RAWHID_RX_SIZE];

// persist the active config so it survives a power cycle (update() only writes
// when a byte actually changes, sparing EEPROM wear)
void saveConfig()
{
  EEPROM.update(eepromMagicAddr, eepromMagic);
  EEPROM.update(eepromTypeAddr, keyType);
  EEPROM.update(eepromModeAddr, mode);
  EEPROM.update(eepromRapidDelayAddr, rapidDelay);
  for (uint8_t i = 0; i < NUM_KEYS; ++i)
  {
    EEPROM.update(eepromKeysAddr + i, keys[i]);
  }
}

// load the saved config, falling back to the default on blank/unconfigured EEPROM
void loadConfig()
{
  if (EEPROM.read(eepromMagicAddr) == eepromMagic)
  {
    keyType = EEPROM.read(eepromTypeAddr);
    mode = EEPROM.read(eepromModeAddr);
    rapidDelay = EEPROM.read(eepromRapidDelayAddr);
    for (uint8_t i = 0; i < NUM_KEYS; ++i)
    {
      keys[i] = EEPROM.read(eepromKeysAddr + i);
    }
  }
  else
  {
    keyType = KEY_TYPE_KEYBOARD;
    keys[0] = KEY_SPACE;
    for (uint8_t i = 1; i < NUM_KEYS; ++i)
    {
      keys[i] = 0;
    }
    mode = MODE_HOLD;
    rapidDelay = 40;
  }
}

// press the currently-configured chord (every non-empty usage in keys[]).
// Modifier usages 0xE0-0xE7 fold into the report's modifier byte automatically.
void pressActive()
{
  if (keyType == KEY_TYPE_CONSUMER)
  {
    Consumer.press(ConsumerKeycode(keys[0] | (uint16_t(keys[1]) << 8)));
  }
  else
  {
    for (uint8_t i = 0; i < NUM_KEYS; ++i)
    {
      if (keys[i])
      {
        BootKeyboard.press(KeyboardKeycode(keys[i]));
      }
    }
  }
  keyIsPressed = true;
}

// release whatever is currently held (the chord, or the consumer key), in reverse
void releaseActive()
{
  if (keyType == KEY_TYPE_CONSUMER)
  {
    Consumer.release(ConsumerKeycode(keys[0] | (uint16_t(keys[1]) << 8)));
  }
  else
  {
    for (uint8_t i = NUM_KEYS; i > 0; --i)
    {
      if (keys[i - 1])
      {
        BootKeyboard.release(KeyboardKeycode(keys[i - 1]));
      }
    }
  }
  keyIsPressed = false;
}

// swap the active config; if the button is held while switching, release the old
// chord first so it doesn't stick down on the host
void setConfig(uint8_t newType, uint8_t newMode, uint8_t newRapidDelay,
               const uint8_t *newKeys)
{
  if (keyIsPressed)
  {
    releaseActive();
  }
  pendingRelease = false;
  keyType = newType;
  mode = newMode;
  rapidDelay = newRapidDelay;
  for (uint8_t i = 0; i < NUM_KEYS; ++i)
  {
    keys[i] = newKeys[i];
  }
  saveConfig();
}

// reboot into the Caterina bootloader so new firmware can be flashed without a
// CDC port (writes the magic boot key, then forces a watchdog reset)
void enterBootloader()
{
  cli();
  *(volatile uint16_t *)0x0800 = 0x7777; // Caterina MAGIC_KEY at MAGIC_KEY_POS
  wdt_enable(WDTO_120MS);
  for (;;)
  {
  }
}

// send a fixed-size RawHID input report back to the host
void sendReport(const uint8_t *bytes, uint8_t n)
{
  uint8_t tx[RAWHID_TX_SIZE];
  memset(tx, 0, sizeof(tx));
  for (uint8_t i = 0; i < n && i < sizeof(tx); ++i)
  {
    tx[i] = bytes[i];
  }
  RawHID.write(tx, sizeof(tx));
}

// reply ['K', keyType, mode, rapidDelay, k0..k5] — the full config
void sendConfig()
{
  uint8_t msg[4 + NUM_KEYS] = {'K', keyType, mode, rapidDelay};
  for (uint8_t i = 0; i < NUM_KEYS; ++i)
  {
    msg[4 + i] = keys[i];
  }
  sendReport(msg, sizeof(msg));
}

void sendIdentity()
{
  uint8_t tx[RAWHID_TX_SIZE];
  memset(tx, 0, sizeof(tx));
  // "ButtonMash" identity string for host auto-discovery
  strcpy_P((char *)tx, PSTR("ButtonMash"));
  RawHID.write(tx, sizeof(tx));
}

// host protocol over RawHID (host sends a report; byte 0 is the command):
//   'I'                                       — identify: reply "ButtonMash"
//   '?'                                       — reply the current config (see sendConfig)
//   'K' keyType mode rapidDelay k0..k5        — set the config, save, reply config
//   'B'                                       — reboot into the bootloader
void handleRawHid()
{
  if (RawHID.available() <= 0)
  {
    return;
  }

  uint8_t buf[4 + NUM_KEYS]; // 'K' keyType mode rapidDelay + keys[NUM_KEYS]
  memset(buf, 0, sizeof(buf));
  uint8_t n = 0;
  while (RawHID.available() > 0)
  {
    uint8_t b = RawHID.read();
    if (n < sizeof(buf))
    {
      buf[n] = b;
    }
    ++n;
  }

  switch (buf[0])
  {
  case 'I':
    sendIdentity();
    break;
  case '?':
    sendConfig();
    break;
  case 'K':
    // payload: keyType, mode, rapidDelay, keys[NUM_KEYS]
    setConfig(buf[1], buf[2], buf[3], &buf[4]);
    sendConfig();
    break;
  case 'B':
    enterBootloader();
    break;
  default:
    break;
  }
}

void setup()
{
  pinMode(buttonPin, INPUT_PULLUP);
  loadConfig();
  BootKeyboard.begin();
  Consumer.begin();
  RawHID.begin(rawhidBuffer, sizeof(rawhidBuffer));
}

void loop()
{
  // listen for host commands that reconfigure the key or trigger reflashing
  handleRawHid();

  // one-shot: release the key a short time after it was pressed
  if (pendingRelease && (millis() - pressedAt) >= oneShotTapMs)
  {
    releaseActive();
    pendingRelease = false;
  }

  // rapid-fire: while the button is held, oscillate press/release at a fixed rate
  if (mode == MODE_RAPID && previousButtonState == LOW)
  {
    unsigned long now = millis();
    if (keyIsPressed && (now - rapidPhaseAt) >= rapidDownMs)
    {
      releaseActive();
      rapidPhaseAt = now;
    }
    else if (!keyIsPressed && (now - rapidPhaseAt) >= rapidDelay)
    {
      pressActive();
      rapidPhaseAt = now;
    }
  }

  // read the raw state of the button
  int reading = digitalRead(buttonPin);

  // reset the debounce timer whenever the raw reading changes
  if (reading != lastReading)
  {
    lastDebounceTime = millis();
  }
  lastReading = reading;

  // accept the reading only after it has been stable past the window
  if ((millis() - lastDebounceTime) >= debounceDelay &&
      reading != previousButtonState)
  {
    previousButtonState = reading;

    if (reading == LOW)
    {
      // button down: emit the configured key
      pressActive();
      if (mode == MODE_ONE_SHOT)
      {
        // schedule a quick auto-release; the button-up edge is then ignored
        pressedAt = millis();
        pendingRelease = true;
      }
      else if (mode == MODE_RAPID)
      {
        // start the rapid-fire oscillator from this first tap
        rapidPhaseAt = millis();
      }
    }
    else
    {
      // button up: hold and rapid release on release; one-shot already auto-released
      if ((mode == MODE_HOLD || mode == MODE_RAPID) && keyIsPressed)
      {
        releaseActive();
      }
    }
  }
}
