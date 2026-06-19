# ButtonMash

A SparkFun Pro Micro (ATmega32U4) firmware that turns a single button into a USB
keystroke, plus **MashConfig**, a Windows app to reconfigure the key.

## Device

The device is a **keyboard-only** USB device (no COM port). It exposes:

- a **BootKeyboard** — keyboard keys (with modifiers) the button sends,
- a **Consumer** interface — media keys (volume, play/pause, …), and
- a **RawHID** vendor interface — used by MashConfig to read/change the config and
  to trigger firmware updates.

USB identity: VID `0x1B4F`, PID `0xB175`, product string `ButtonMash`.

A button press emits a configurable action: either a **keyboard chord** — up to
6 HID usages held together, which covers a single key, multi-key combos
(`Ctrl+Shift+W`), left/right modifier distinction (`RightCtrl+Del`), and a bare
modifier on its own (just the Windows key) — or a single **media** key. Plus a
**mode**:

- **Hold** — key held while the button is held (the OS auto-repeats).
- **One-shot** — a single ~10 ms tap; no repeat even if the button is held.
- **Rapid fire** — while held, the key is auto-tapped at a fixed rate (~16 Hz).

### Host protocol (RawHID)

The host sends a report; byte 0 is the command:

| Command | Payload | Reply |
| --------- | --------- | ------- |
| `I` | — | `"ButtonMash"` |
| `?` | — | `['K', keyType, mode, rapidDelay, k0..k5]` (10 bytes) |
| `K` | `keyType, mode, rapidDelay, k0..k5` | same 10-byte echo |
| `B` | — | (none — device resets) |

`keyType` = 0 keyboard / 1 consumer; `mode` = 0 hold / 1 one-shot / 2 rapid fire;
`k0..k5` is the chord (HID usages, `0` = empty slot). Modifiers are just usages
`0xE0`–`0xE7` (L/R Ctrl/Shift/Alt/Win) in the list. For a media key, the single
16-bit consumer usage lives in `k0` (low) / `k1` (high).

## MashConfig (host app)

Source: [github.com/opcow/mashconfig](https://github.com/opcow/mashconfig)

`MashConfig/` is a cross-platform (Windows/Linux/macOS) C++ app built with **Dear
ImGui** (GLFW + OpenGL3) over a **hidapi** transport. It auto-discovers the device
over HID (VID/PID + RawHID usage page `0xFFC0`, confirmed with the `I` handshake)
and shows the current config. You set a new one by choosing a **preset** (the
dropdown lists non-typeable / uncommon keys — numpad, Pause, Scroll Lock, F1–F24,
bare modifiers, media, …) or by **press-to-capture** (Discord-style: arm it,
press your key combo, then release to set it). Capture records exactly what you
held, side-specific left/right modifiers included — a single key, a full
`Ctrl+Shift+W` combo, or a modifier-only combo like `Ctrl+Shift`. Then pick
**Hold**, **One-shot**, or **Rapid fire**. It has an in-app Dark/Light theme
toggle.

### Building

You need **CMake ≥ 3.20** and a C++17 compiler. All other dependencies (GLFW,
Dear ImGui, hidapi, stb) are fetched automatically by CMake — nothing to install.

```sh
# Windows (Visual Studio 2026 / MSVC)
cmake --preset windows && cmake --build build/windows --config Release

# Linux (needs: xorg-dev libgl1-mesa-dev libudev-dev)
cmake --preset linux && cmake --build build/linux

# macOS (produces MashConfig.app)
cmake --preset macos && cmake --build build/macos
```

The binary lands in `MashConfig/build/<os>/` (Windows: `Release/MashConfig.exe`)
with an `Assets/` folder copied alongside for the window icon.

On **Linux**, install the udev rule once so MashConfig can reach the device
without root:

```sh
sudo cp packaging/99-buttonmash.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Prebuilt binaries for all three platforms are produced by CI
(`.github/workflows/build.yml`).

## Reflashing the firmware

Because the device has **no CDC serial port**, PlatformIO's normal 1200bps-touch
auto-reset cannot put it into the bootloader. Start the upload command first so
avrdude is already waiting, then trigger the bootloader within the ~8 second window:

1. **Software (normal):** run the upload command first, then immediately click
   **"Update firmware (enter bootloader)"** in MashConfig (sends the `B` command):

   ```bash
   pio run -t upload
   ```

2. **Hardware (recovery fallback):** run `pio run -t upload`, then double-tap a
   momentary switch wired between the **`RST`** and **`GND`** pads (short them
   twice within ~750 ms). The Caterina bootloader stays active ~8 s, exposing its
   own port (PID `0x9205`) for avrdude.

The hardware switch is the safety net if firmware ever becomes unresponsive — the
bootloader is separate from the sketch, so a double-tap reset always recovers.
# buttonmash
