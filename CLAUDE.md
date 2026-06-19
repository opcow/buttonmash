# CLAUDE.md

Guidance for working in this repo.

## What this is

**ButtonMash** is a single-button USB device built on a **SparkFun Pro Micro
(ATmega32U4, 5V/16MHz, board `sparkfun_promicro16`)**. Pressing the button emits
one configurable keystroke. It is a **keyboard-only** USB device (no COM port);
configuration and firmware-update triggering happen over a vendor **RawHID**
interface.

Two parts:
- **Firmware** — PlatformIO / Arduino, in [src/main.cpp](src/main.cpp).
- **MashConfig** — a .NET 8 WinForms host app in [MashConfig/](MashConfig/) that
  auto-discovers the device over HID and reconfigures the key.

## USB identity

- VID `0x1B4F`, PID `0xB175`, product string `ButtonMash`.
- Manufacturer is forced to `"SparkFun"` by the core for VID `0x1B4F`
  (USBCore.cpp) — not overridable via config.
- PID `0xB175` was chosen deliberately: it is unclaimed by any installed Windows
  INF. A matching INF (e.g. `0x9207` = Arduino LilyPad) overrides the descriptor
  name; changing the PID also forces Windows to re-enumerate instead of showing a
  cached name. **Don't change the PID without re-checking `C:\Windows\INF\*.inf`
  for VID_1B4F collisions.**

## Firmware ([src/main.cpp](src/main.cpp), [platformio.ini](platformio.ini))

- Uses **NicoHood HID-Project** (`nicohood/HID-Project@^2.8.4`). Three HID
  interfaces: `BootKeyboard` (keyboard keys + modifiers), `Consumer` (media
  keys), and `RawHID` (bidirectional host config). 3 IN endpoints, within the
  32U4 budget.
- `-DCDC_DISABLED` is what actually removes the COM port (just not using `Serial`
  is **not** enough on the 32U4 core). This also disables the 1200bps-touch
  auto-reset used for flashing — see Reflashing below.
- `#include <Arduino.h>` **must be first** — in a `.cpp` it isn't auto-included,
  so `USBCON` would be undefined and HID-Project fails to compile.
- **A press emits a configurable action** (`keyType, mode, rapidDelay, keys[6]`):
  - `keyType`: `0` = keyboard, `1` = consumer (media).
  - `keys[6]`: for **keyboard**, a **chord** — up to `NUM_KEYS = 6` HID usages
    held simultaneously (`0` = empty slot; usage `0x00` is never a real key).
    Keyboard usages: Space `0x2C`, F1–F12 `0x3A`–`0x45`, F13–F24 `0x68`–`0x73`,
    numpad `0x54`–`0x63`, etc. **Modifiers are just usages in the list**: `0xE0`–
    `0xE7` (L/R Ctrl/Shift/Alt/GUI) — `BootKeyboard.press()` folds them into the
    report's modifier byte, so `{0xE0,0xE1,0x1A}` sends LeftCtrl+LeftShift+W with
    no special-casing. This covers **multi-key combos**, **left/right modifier
    distinction** (LeftCtrl `0xE0` vs RightCtrl `0xE4`), and a **bare modifier as
    the key itself** (e.g. `{0xE3}` = just the Windows key). For **consumer**
    (media), there's a single **16-bit** usage (Vol Up `0xE9`, Play/Pause `0xCD`)
    stored in `keys[0]`=low / `keys[1]`=high; the firmware branches on `keyType`.
  - `mode`: `0` = hold (down on press, up on release → OS auto-repeats), `1` =
    one-shot (down then up `oneShotTapMs ≈ 10ms` later via a non-blocking
    `pendingRelease`, so a held button fires once), `2` = rapid fire (while held,
    the loop oscillates press/release — `rapidDownMs 20` / `rapidDelay` ms gap,
    configurable 10–250ms, default 40ms ≈ 16Hz).
- EEPROM layout: `magic@0 (0xA9), keyType@1, mode@2, rapidDelay@3, keys[6]@4-9`.
  Magic bumped `0xA8`→`0xA9` when the single `code`+`modifiers` pair became a
  `keys[6]` chord, so stale configs reset to default (Space / hold / no mods /
  40ms). Bump again on any encoding change.
- Debounce is non-blocking, time-based (`debounceDelay = 50ms`).

### RawHID protocol (host → device, byte 0 = command)

| Cmd | Payload | Reply |
|-----|---------|-------|
| `I` | — | `"ButtonMash"` |
| `?` | — | `['K', keyType, mode, rapidDelay, k0..k5]` (10 bytes) |
| `K` | `keyType, mode, rapidDelay, k0..k5` | same 10-byte echo |
| `B` | — | (none — device resets) |

RawHID receives host data via **SET_REPORT on the control pipe** (no interrupt
OUT endpoint). Report size = `USB_EP_SIZE` = 64 bytes. Windows `WriteFile`
auto-routes to SET_REPORT, so hidapi `hid_write` works.

`enterBootloader()` writes Caterina magic `0x7777` to `0x0800`, then watchdog
resets. The bootloader enumerates as PID `0x9205` and stays ~8s.

## MashConfig ([MashConfig/](MashConfig/))

**Cross-platform C++ (Windows/Linux/macOS)** — Dear ImGui front end (GLFW +
OpenGL3) over a **hidapi** transport, built with CMake. (Was a Windows-only .NET
WinForms app; rewritten for portability.) Dependencies are pulled via CMake
`FetchContent` (GLFW, Dear ImGui, hidapi, stb) — no system installs needed to
build; `find_package(OpenGL)` uses the system GL.

- [src/device.cpp](MashConfig/src/device.cpp) — hidapi transport. Discovery:
  `hid_enumerate(VID,PID)`, filter on `usage_page == 0xFFC0` (the RawHID
  collection, vs. the keyboard one), then confirm with the `I` handshake.
  **hidapi report-id gotcha:** on **write** the buffer leads with a report-id
  byte (`0`), so `cmd`/`arg` go at `[1]`/`[2]`. On **read** hidapi returns
  unnumbered-report data starting at **index 0** — so `buf[0]` is already the
  reply byte; **do NOT strip a leading byte** (the old HidSharp host did).
- [src/keys.cpp](MashConfig/src/keys.cpp) — `keys::Type` (keyboard/consumer), the
  `keys::Chord` alias (`std::array<uint8_t,6>`), the **curated** preset table
  (non-typeable / uncommon keys + media; **letters/digits dropped** — those come
  from press-to-capture), `Describe(type, chord)` (modifiers named first via
  `ModifierName`, then keys, joined with `+`), `IsModifierUsage`/`ModifierName`,
  the `ImGuiKey → usage-ID` map, and `HeldModifierUsages()` (side-specific
  `0xE0`–`0xE7` from `ImGui::IsKeyDown`). **Must stay in sync with the firmware's
  usage IDs / key types.**
- [src/device.cpp](MashConfig/src/device.cpp) — `ButtonMashConfig` struct
  (`keyType, mode, rapidDelay, std::array<uint8_t,kMaxKeys> keys`) +
  `GetConfig`/`SetConfig` over the 10-byte `?`/`K` protocol.
- [src/main.cpp](MashConfig/src/main.cpp) — GLFW/OpenGL3/ImGui loop. Single
  borderless full-window panel; 1.5s rescan timer; curated preset combo (sets the
  main key, keeping modifiers), **Discord-style press-to-capture** (arm, press
  the combo, release to commit), a **Hold/One-shot** mode selector, a **Fire
  delay** slider (10–250ms, enabled only in Rapid fire mode), Apply/Rescan, and
  "Update firmware" (bootloader) with a confirm modal; an in-app Dark/Light
  toggle. Capture accumulates the **union of every usage held during the
  gesture** (`AllHeldUsages()` = side-specific `HeldModifierUsages()` + any
  non-modifier keys) and commits the whole chord on full release, so it records a
  bare key, a `Ctrl+Shift+W` combo, **or a modifier-only combo** like
  `Ctrl+Shift` (no separate modifier checkboxes — capture handles modifiers
  directly). `Chord*` helpers (`ChordAdd`/`ChordHas`/`ChordSetMainKey`/
  `ChordSingleUsage`) edit `pending.keys`. Window icon loaded from
  `Assets/icon-64.png` via stb_image + `glfwSetWindowIcon`.

Build & run (per OS, via CMake presets):

```sh
cmake --preset windows && cmake --build build/windows --config Release   # Windows
cmake --preset linux   && cmake --build build/linux                      # Linux
cmake --preset macos   && cmake --build build/macos                      # macOS
```

- **Windows generator** is `Visual Studio 18 2026` (VS Community 2026, MSVC
  14.51). The exe is built `WIN32` (no console) with [app.rc](MashConfig/app.rc)
  embedding `app.ico`.
- **Linux** needs build deps `xorg-dev libgl1-mesa-dev libudev-dev`, and a udev
  rule for non-root HID access:
  [packaging/99-buttonmash.rules](packaging/99-buttonmash.rules).
- **macOS** builds a `.app` bundle (`MACOSX_BUNDLE` +
  [packaging/Info.plist.in](MashConfig/packaging/Info.plist.in)); OpenGL 3.2 core
  is deprecated-but-functional. GLFW window icons are ignored on macOS.
- CI: [.github/workflows/build.yml](.github/workflows/build.yml) builds all three
  on a runner matrix.

### Icon

The icon set's source of truth is
[MashConfig/Assets/icon-256.png](MashConfig/Assets/icon-256.png) (a dark disc +
white glyph, antialiased against transparency; itself derived from the master
[icon.png](icon.png)). Generated assets are the smaller
[MashConfig/Assets/](MashConfig/Assets/) PNGs (16/24/32/48/64/128, copied next to
the binary at build time; the runtime window icon loads `icon-64.png`) and
[MashConfig/app.ico](MashConfig/app.ico) (multi-resolution 16…256, PNG-compressed
entries, embedded into the exe via [app.rc](MashConfig/app.rc)).

Regenerate with [MashConfig/tools/gen-icons.ps1](MashConfig/tools/gen-icons.ps1)
(`pwsh -File MashConfig/tools/gen-icons.ps1`; System.Drawing — ImageMagick is not
installed). **The downscale must be premultiplied-alpha**: the master's
transparent pixels carry white RGB, so a naive straight-alpha resize bleeds white
into the dark disc edge and leaves an ugly light fringe at small sizes; the script
premultiplies RGB by alpha before the bicubic resize and un-premultiplies after.

## Reflashing

Because there's no CDC port, `pio run -t upload` alone **cannot** reset the board
into the bootloader. Run `pio run -t upload` first so avrdude is waiting, then
trigger the bootloader within ~8s:

1. **Software:** run `pio run -t upload`, then click "Update firmware (enter
   bootloader)" in MashConfig (sends `B`).
2. **Hardware (recovery):** run `pio run -t upload`, then double-tap a momentary
   switch wired across `RST`↔`GND` (~750ms window). Always works because the
   bootloader is separate from the sketch.

## Environment notes

- `pio` is not on PATH. Use `$env:USERPROFILE\.platformio\penv\Scripts\pio.exe`.
- Windows caches device names by VID/PID. Real descriptor name is in
  `DEVPKEY_Device_BusReportedDeviceDesc`; `DeviceDesc` may be the stale cache.
  Clear ghosts with `pnputil` remove + rescan (hidden devices).
