# DS5Dongle project context

Single source of truth for AI coding agents working in this repository. Claude Code loads it via the `@AGENTS.md`
import in `CLAUDE.md`; other agent tools read this file directly. Keep edits here, not in a per-tool copy.

## Purpose and stack

- This repository builds `ds5-bridge`, Raspberry Pi Pico firmware that bridges a Sony DualSense or DualSense Edge over Bluetooth Classic to a host as a wired USB controller (gamepad HID + UAC1 audio + optional boot keyboard).
- The default target is Pico 2 W (`pico2_w`, RP2350). Optional targets are Pico W (`-DPICO_W_BUILD=ON`, audio processing disabled, 200 MHz) and Waveshare RP2350B-Plus-W (`-DWAVESHARE_RP2350B_PLUS_W_BUILD=ON`). The two board options are mutually exclusive.
- The codebase is C++20 plus one C11 translation unit, built with the Raspberry Pi Pico SDK, TinyUSB, BTstack/CYW43, Opus, and Cockos WDL Resampler.
- `lib/WDL` and `lib/opus` are Git submodules. Do not modify submodule contents unless a task explicitly requires it.

## Current sources of truth

- Use `CMakeLists.txt`, `.github/workflows/build-firmware.yml`, and the English `README.md` as the current build/behavior references.
- Canonical CI pins are Pico SDK `2.3.0`, ARM GNU toolchain `15.2.rel1`, and TinyUSB commit `2d56dc533e45e4e91b15e93fdab5e22e964f328d`.
- Generated output belongs under ignored `build/` directories; the firmware artifact is `ds5-bridge.uf2`.

## Build

The repo carries a Pico SDK checkout at `.deps/pico-sdk` (2.3.0, gitignored), which the Makefile uses by default:

```sh
make build-uf2   # Release, pico2_w, builds in build/standard, then copies a
                 # date-stamped ds5-bridge-DDMMYY-HHMM.uf2 into uf2Build/
```

Override with `make build-uf2 PICO_SDK_PATH=... PICO_TOOLCHAIN_PATH=...`. Direct CMake, for non-default targets:

```sh
git submodule update --init --recursive   # submodules may be uninitialized
cmake -S . -B build/standard -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build/standard --target ds5-bridge
```

Compile gates: `-DENABLE_SERIAL=ON -DENABLE_VERBOSE=ON -DWAKE_DEBUG=ON` (debug build), `-DPICO_W_BUILD=ON`,
`-DWAVESHARE_RP2350B_PLUS_W_BUILD=ON`, `-DENABLE_BATT_LED=OFF`, `-DENABLE_DEBUG=ON` (adds `src/debug.cpp` probes),
`-DDISABLE_SPEAKER_PROC=ON`.

`.github/workflows/build.yml` runs the standard + debug + Pico W + Waveshare + no-battery-LED matrix on every branch push.

## Verification

There is no checked-in unit/integration test suite. For firmware changes, build at least the directly affected
configuration plus the standard build. For build-system, descriptor, or compile-gate changes, mirror CI's full matrix
when practical. Hardware-sensitive audio, pairing, USB enumeration, wake, lightbar, and BOOTSEL behavior still require
device validation — a green build proves nothing about them.

Runtime inspection over USB HID, no reflash needed:

```sh
python tools/config_tool.py get                      # dump live Config_body
python tools/config_tool.py set lightbar_mode=2 --no-save
python tools/config_tool.py fields                   # packed layout table
python tools/reboot_bootsel.py                       # reboot into BOOTSEL
```

`tools/wireshark_dualsense_setstate.lua` post-dissects USB output report `0x02`.

## Repository map

- `src/main.cpp`: startup, the core-0 scheduler, Bluetooth input forwarding, TinyUSB HID/audio callbacks, and USB-to-controller output state handling.
- `src/bt.cpp`: Bluetooth inquiry, pairing/authentication, L2CAP HID channels, feature-report cache, persistent cleared-controller blacklist, controller lifecycle, BT writes, and the battery/lightbar state machine.
- `src/audio.cpp`: four-channel USB OUT processing, 3 kHz haptics, speaker Opus encoding, controller-mic Opus decoding, inter-core queues, and the core-1 loop.
- `src/usb_descriptors.cpp`: dynamic DualSense/DualSense Edge USB device, UAC1 audio, gamepad HID, optional keyboard HID, BOS, and Microsoft OS 2.0 descriptors.
- `src/usb.cpp`: UAC1 mute/volume control requests and translation into controller `SetStateData`.
- `src/config.*` and `src/cmd.*`: packed persistent configuration and vendor feature reports `0xF6`-`0xF9`.
- `src/wake.*` and `src/ps_shortcut.*`: S3 remote-wake state machine using F15, and PS-button Win+G / Win+Tab keyboard shortcuts.
- `src/light_shortcut.*`: D-pad Left + L1 + Triangle combo (200 ms hold) that cycles `lightbar_mode`, confirms with mode+1 rumble pulses, and defers the flash save by 10 s.
- `src/dse.*`: DualSense Edge unlock, paced profile snapshot prefetch, and post-save refresh machinery.
- `src/button_functions.*`: BOOTSEL gesture FSM (single pair/switch, double reboot, triple bootloader, long clear pairings).
- `src/battery_led.*` and `src/status_gpio.*`: low-battery Pico-LED indication and configurable connection GPIO output.
- `src/utils.h`: controller report layouts, `SetStateData`, and DualSense CRC helpers.
- `src/ram_mem.c` plus `cmake/relocate_to_ram.cmake`: RAM-resident memory operations and external-object hot-path relocation.
- `tools/config_tool.py`: host-side HID configuration utility; its field table mirrors `Config_body` byte-for-byte.
- `.github/workflows/build-firmware.yml`: reusable standard/debug/board build matrix and release packaging.

## Runtime data flow

1. Core 0 initializes TinyUSB, CYW43/BTstack, persisted config, Bluetooth, and audio, then runs one flat scheduler loop (bottom of `main.cpp`): `cyw43_arch_poll` → `tud_task` → `wake_task` → `audio_loop` → `interrupt_loop` → battery LED → battery lightbar → BOOTSEL button → BT inquiry LED → `dse_task` → `light_shortcut_task`.
2. A controller input report arrives on the Bluetooth HID interrupt L2CAP channel. `on_bt_data()` strips the Bluetooth framing, handles mic packets and local features, then exposes a 63-byte wired report through TinyUSB HID.
3. Host HID output report `0x02` is converted to a Bluetooth `0x31` report. Configured trigger reduction, speaker gain, mic selection, and volume locking are applied before it is queued to L2CAP.
4. USB audio OUT is 48 kHz, four-channel PCM: channels 0/1 feed speaker encoding and channels 2/3 are resampled to 3 kHz haptics. Core 1 resamples/Opus-encodes speaker audio while also Opus-decoding controller microphone frames.
5. Controller mic PCM returns over USB audio IN as two channels (mono duplicated) in host-clocked 1 ms slices.
6. Feature reports are proxied and cached over the Bluetooth HID control channel. DualSense Edge profile reads are gated until its unlock and paced prefetch complete.

## Performance and concurrency invariants

- **Watchdog.** Non-serial builds arm a 1 s hardware watchdog fed once per scheduler iteration. Nothing in the core-0 loop may block for ~1 s. This is why flash writes are deferred rather than done inline at the moment of a button press or shortcut.
- **RAM residency.** Full audio at the stock 150 MHz clock depends on keeping the steady-state hot path in RAM. Opus, selected WDL methods, BTstack/CYW43/TinyUSB functions, queue operations, CRC lookup data, and custom `memcpy`/`memset`/`memmove` are deliberately relocated to `.time_critical.*`.
- **Relocation fails silently.** `CMakeLists.txt` drives `objcopy` against *pristine* SDK/submodule objects, matching them by stable path suffix and mangled symbol name. A dependency upgrade can change either; `objcopy` skips absent sections, so the link still succeeds while the code quietly reverts to executing from flash. After any dependency bump, inspect the relocation/map output and re-validate real audio. A successful link is not evidence.
- **Inter-core.** Core 0 and core 1 communicate through Pico queues. Preserve nonblocking queue behavior and bounded/static buffers on audio paths.
- **Flash safety.** Flash erase/program and BOOTSEL sensing must use `flash_safe_execute()`. Core 1 registers with `flash_safe_execute_core_init()`, and `PICO_FLASH_ASSUME_CORE1_SAFE=0` must remain in effect so XIP is never accessed while flash/QSPI is unavailable. A save parks core 1 for ~50 ms, which is an audible audio blackout.
- **Shared report.** The real-time HID mode (`polling_rate_mode == 2`) protects `interrupt_in_data` with `report_cs`; do not introduce an unguarded second writer.
- **Hot paths.** Avoid unconditional formatting/logging in RAM/audio paths; verbose logs are normally compiled behind `ENABLE_VERBOSE`.

## Lightbar and battery state

- All outgoing controller state packets funnel through `update_state()` in `bt.cpp`, which calls `apply_lightbar()` on every packet. That is the single enforcement point for lightbar policy — do not emit state packets around it with a raw `bt_write()`.
- `lightbar_mode`: `0` battery tiers, `1` host-controlled (leaves the host's bytes untouched), `2` custom RGB.
- A critical-battery pulse overrides *every* mode, host-controlled included, and strips fade-animation requests so a host app cannot latch an animation over the warning. It is armed only while discharging, with a hysteresis band that holds through the first non-red level so a bouncing level nibble cannot flap it.
- `battery_lightbar_critical()` is the single source for "critically low"; `battery_led.cpp` reads it so the Pico onboard LED blinks on exactly the same condition as the lightbar pulse. Keep them reading one source.
- `battery_lightbar_tick()` only fills gaps: while the host streams output reports those packets already carry the pulse frame, so the tick paces frames and issues the one-shot repaint after the warning clears.

## Configuration protocol

- `Config_body` is packed and persisted in the final flash sector. Schema version is currently `6`.
- HID feature reports: `0xF6` updates/saves/reconnects/reboots-to-BOOTSEL (functions `0x01`-`0x04`), `0xF7` reads config, `0xF8` reads firmware version, and `0xF9` reads RSSI plus live audio-gating flags.
- If fields are added, removed, reordered, or resized, update `Config_body` and `tools/config_tool.py:FIELDS` together and bump `CONFIG_VERSION` (`src/config.cpp`, mirrored in the Python tool) for a breaking on-flash layout change.
- Keep configuration validation ranges aligned between firmware and the Python tool.

## USB identity and descriptor invariants

- USB VID is Sony `0x054C`; PID is `0x0CE6` for DualSense mode and `0x0DF2` for Edge mode.
- The composite device contains UAC1 audio, gamepad HID, and—when wake or the PS shortcut is enabled—a boot-keyboard HID interface. CDC appears only in serial builds.
- `ENABLE_WAKE_HID` is intentionally compiled into every current firmware build. `enable_wake` and `ps_shortcut_enabled` control descriptor exposure and behavior at runtime.
- USB configuration lengths, interface counts, endpoint offsets, HID report lengths, and runtime descriptor truncation are manually coordinated in `usb_descriptors.cpp`. Update all related constants, offsets, and static assertions together.
- Preserve the packed `SetStateData` byte layout and DualSense CRC seeds/trailers. Protocol bytes should only change with packet evidence or an explicit compatibility requirement.
- The gamepad identity can be forced to DS5, forced to Edge, or auto-selected from the connected controller. Keep descriptors, product strings, feature reports, and PID selection consistent.
- DSE profile requests are deliberately paced to avoid overflowing the L2CAP control channel.
- Changes that affect USB identity, descriptors, serial number, polling interval, wake, or keyboard exposure generally require the config tool's USB reconnect command so the host re-enumerates.

## Known repository drift to account for

- `tools/build-macos.sh`, `tools/build-windows.ps1`, and `boards/build_waveshare_rp2350b_plus_w.sh` still pin or mention Pico SDK `2.2.0` / TinyUSB `0.20.0`; they are behind the current CMake/CI/README pins.
- Those helper scripts still describe a separate `ENABLE_WAKE_HID` build even though current CMake always compiles wake/keyboard support and gates it at runtime.
- `README.CN.md` still describes the old overclock and separate wake branch; use the English README and code for current behavior.
- DSE profile-write forwarding in `main.cpp` is currently commented out even though refresh machinery exists in `dse.cpp`; verify actual support before changing or documenting profile writes.
- `uf2Build/` is not listed in `.gitignore`; do not commit the stamped artifacts `make build-uf2` leaves there.
