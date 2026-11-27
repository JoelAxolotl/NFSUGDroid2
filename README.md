# NFSUG2 for Android

Runs the Windows PC release of *Need for Speed Underground 2* (`SPEED2.EXE`, x86-32, Direct3D 9)
natively on arm64 Android phones. The game's code is **statically recompiled to ARM64 once, on your
PC**, and runs on a C++ runtime that implements the Windows and DirectX APIs the game uses
(Win32, Direct3D 9 → OpenGL ES 3.2, DirectInput 8, DirectSound → AAudio). The app layer is in Java.

Reference device: Poco X6 Pro 5G (Dimensity 8300, Mali-G615, Android 16), tested over wireless ADB.

## Contents

- [Bring your own game](#bring-your-own-game)
- [Requirements](#requirements)
- [Build and install](#build-and-install)
- [Playing](#playing)
- [How it works](#how-it-works)
- [Performance](#performance)
- [Development](#development)
- [Troubleshooting](#troubleshooting)
- [Status and roadmap](#status-and-roadmap)

## Bring your own game

This project contains **no game code or data** and must never be distributed with any.
It provides the runtime and a static recompiler. Each player builds a personal APK from their own
official copy of the game:

1. `tools/recomp/recomp.py` reads **your** `SPEED2.EXE` and writes C++ to
   `android/app/src/main/cpp/generated/`.
2. The Android build compiles that C++ into the APK as native ARM64 code.
3. At boot, the runtime checks that the `SPEED2.EXE` on the phone is the exact file the APK was
   built from (size + FNV-1a 64). On a mismatch it does not run the native code; it falls back to
   the (slow) interpreter.

The generated code and the APK are derived from your copy of the game. **Keep them to
yourself: never commit, upload or share them.** `.gitignore` excludes `generated/`, build outputs
and `*.apk`.

## Requirements

| What | Version / notes |
|---|---|
| Game | NFS Underground 2 PC, v1.2 `SPEED2.EXE` (4,800,512 bytes) with its full data folders |
| Phone | arm64 Android 10+ with OpenGL ES 3.2 (developed on Android 16 / Mali-G615); ~4 GB free storage |
| PC | Windows, PowerShell 7, Python 3.11+ (the scripts create `.venv` with `pefile` and `capstone`) |
| Android tooling | Android SDK with platform-tools (`adb`), NDK 29.0.14206865, CMake 3.22.1 |
| Java | JDK 21 (Gradle 8.9 wrapper, AGP 8.7.3 are fetched by the build) |

Enable **Developer options → Wireless debugging** (or USB debugging) on the phone and pair it
with `adb pair` once.

## Build and install

One command recompiles your executable, builds the APK, installs it and copies your game data:

```powershell
tools/build_apk.ps1 -GameDir "D:\Games\NFS Underground 2" -Install -PushData
```

| Switch | Effect |
|---|---|
| `-GameDir` | Your installed game folder (must contain `SPEED2.EXE`) |
| `-Install` | `adb install -r` the APK (keeps existing app data and saves) |
| `-PushData` | Copies the game data to `/sdcard/Android/data/com.rea.nfsug2/files/game/` |
| `-Profile <file>` | Extra function entry points recorded on the device (`files/user/aot_profile.txt`) |

Recompiling takes about a minute and the APK build a few more. Re-run with only `-Install` after
code changes; the game data only needs pushing once.

> **Never uninstall the app.** Uninstalling deletes `/sdcard/Android/data/com.rea.nfsug2`, including
> the pushed game data and your saves. Installing over it (`adb install -r`) is safe.

On the phone, open **NFSUG2**, check that the launcher finds `SPEED2.EXE`, and tap **Start game**.

## Playing

### Controls

The touch overlay and physical controllers drive the game through its own default keyboard
bindings (read from the DirectInput action maps the game registers):

| On-screen / controller | Game action | Key sent |
|---|---|---|
| GAS / right trigger | Throttle | Up + W |
| BRAKE / left trigger | Brake / reverse | Down + S |
| STEER stick / left stick (X) / D-pad | Steer, menu navigation | Left / Right (D-pad also Up / Down) |
| E-BRK / X | Handbrake | Space |
| NOS / A | Nitrous (A also accepts in menus) | Left Alt (+ Enter) |
| GEAR ▶ / RB, ◀ GEAR / LB | Shift up / down | Left Shift / Left Ctrl |
| CAM / Y | Change camera | C |
| Back / Select | Look back | B |
| OK | Accept | Enter |
| BACK, ❚❚ / B, Start | Back, pause | Esc |

The overlay hides itself while a physical controller is in use (toggle on the launcher screen).
Physical keyboards are passed through as-is.

### Files on the phone

| Path (under `/sdcard/Android/data/com.rea.nfsug2/files/`) | Contents |
|---|---|
| `game/` | Your game data (`SPEED2.EXE`, `GLOBAL`, `CARS`, `TRACKS`, …, plus the 0-byte `foobar` disc marker) |
| `user/log.txt`, `user/log.prev.txt` | Runtime log of the current and previous run |
| `user/registry.txt` | The game's registry settings (graphics options, …) |
| `user/profile/` | Save games |
| `user/aot_profile.txt` | Functions seen at run time, for the recompiler's `-Profile` |

## How it works

```
Java   LauncherActivity → GameActivity (":game" process)
       SurfaceView + GamepadOverlayView (touch) + ControllerInput (pads/keyboards) → NativeBridge (JNI)

C++    runtime/process     boot, PEB/TEB, guest threads (16 MB host stacks), fault handler
       runtime/aot         recompiled-function table, EXE identity check, profiling counters
       runtime/vfs         Windows paths → game/ and user/ folders
       core/memory         guest 4 GiB address space in one host reservation (host = base + guest)
       core/guest_heap     HeapAlloc / GlobalAlloc / LocalAlloc
       loader/pe_loader    maps SPEED2.EXE at 0x400000, binds imports to HLE thunks
       cpu/                x86 semantics shared by the interpreter and the recompiled code:
                           ops.h (integer, lazy flags), x87_ops.h (FPU), mmx.cpp, interp.cpp
       cpu/aot_rt.h        runtime support for generated code (calls, fused flags, helpers)
       generated/          YOUR recompiled game code (not part of the project)
       hle/                kernel32, user32, advapi32 (registry), winmm, ws2_32, …;
                           COM objects for DirectInput 8 and DirectDraw
       gfx/                Direct3D 9 → GLES 3.2: device and resources (d3d9.cpp), draws (d3d9_draw.cpp),
                           shader bytecode → GLSL (shader_translator), fixed-function emulation (ffp),
                           texture formats incl. DXT decode (texconv), EGL (gl_context)
       audio/dsound        DirectSound buffers mixed into an AAudio stream
       platform/           input state (pads → key events), window
```

- **Guest ↔ host calls.** Imported functions and COM methods are 4-byte thunk slots at
  `0x7F000000` that dispatch to C++ (`hle::dispatch`). Host → guest callbacks (window procedure,
  timers, DirectInput enumeration) use `hle::call_guest`.
- **COM objects** live in guest memory as `[vtable][id]`; the id indexes a lock-free table of host
  objects.

### Static recompiler (`tools/recomp/recomp.py`)

- **Discovery.** Starts from the entry point, every code pointer in the relocation table (which
  survives in an unnamed section at RVA `0x4F5000`) and the optional device profile. It follows call
  targets to a fixed point and resolves switch jump tables through relocated table entries. Result:
  ~18.6k functions, 1.18M instructions.
- **Emission.** One C++ function per guest function: basic blocks are `goto` labels, direct calls are
  direct C++ calls, and guest registers live in locals. Semantics come from the interpreter's own
  helpers, so both engines give bit-identical results. 98% of instructions are native; the rest
  (mostly MMX/SSE/3DNow!) are single-stepped by the interpreter in place.
- **Flag optimisation.** A per-function flag liveness pass. Flag producers whose flags are never
  read compute only their result (85%). A `cmp`/`test`/`add`/`sub`/`inc`/`dec` followed by
  `jcc`/`setcc`/`cmovcc` becomes a direct C++ comparison (100k branches).
- **x87** instructions call an always-inline core with constant operands, so each folds to straight
  code on host doubles.
- Output is written atomically (temp folder, then swapped in).

### Graphics

- **Coordinates.** Frames are rendered top-down, and Present flips while blitting to the window.
  The vertex epilogue applies D3D's Y flip, half-pixel offset and depth range.
- **Shaders.** vs/ps 1.x–2.0 bytecode is translated to GLSL ES 3.20. Fixed-function stages
  become generated shaders keyed by state.
- **Textures.** DXT1/3/5 are decoded on the CPU, because Mali GPUs have no S3TC support.
- **Dynamic geometry** (`*UP` draws) streams through an unsynchronised mapped ring buffer.
- **State blocks** are compiled to `memcpy` ranges.

## Performance

Measured on the reference phone, free-roam while driving at speed: **48–60 FPS** (vsync-capped at 60).
Menus run at 60 FPS. The log reports, every 5 s:

```
presented N frames, FPS ...
  gfx/5s: draw / present / texture upload time, ring-buffer traffic
  cpu/5s: instructions still run by the interpreter (should be ~0.01%)
```

| Optimisation | Effect |
|---|---|
| Static recompilation instead of interpretation | 7 → 21 FPS free-roam |
| Inline x87 core | removed 31% of the main thread; 21 → ~55 FPS |
| Flag liveness and branch fusion | removed lazy-flag materialisation (6.7%) |
| Mapped, unsynchronised vertex ring | removed 1.6 ms stalls per `*UP` draw |
| Lock-free COM lookup, sampled stats timing | ~9% mutex/clock overhead (to be re-measured) |

### Profiling on the device

```powershell
adb shell simpleperf record --app com.rea.nfsug2 -e cpu-clock -f 2000 --duration 15 -o /data/local/tmp/perf.data
adb pull /data/local/tmp/perf.data
python $NDK/simpleperf/binary_cache_builder.py -i perf.data -lib android/app/build/intermediates/merged_native_libs/debug
$NDK/simpleperf/bin/windows/x86_64/simpleperf.exe report -i perf.data --symfs binary_cache --sort comm,symbol
```

Add `--call-graph dwarf,8192` (short durations) to see callers. Generated functions appear as
`F_<guest address>`.

## Development

```powershell
tools/run_device.ps1             # build, install (keeps data), autostart, print the log tail
tools/run_device.ps1 -Trace      # also log the first 32 calls of every Win32/COM function
tools/run_device.ps1 -NoBuild    # reinstall and launch the last build
```

`run_device.ps1` builds whatever is in `generated/`; run `build_apk.ps1` (or `recomp.py`) after
changing the recompiler. When several ADB transports list the same phone, set `$env:ANDROID_SERIAL`.

Other tools:

| Tool | Purpose |
|---|---|
| `tools/pe_survey.py`, `tools/isa_census.py` | Import survey and instruction census of `SPEED2.EXE` (output in `analysis/`) |
| `tools/tests/fused_flags_test.cpp` | Checks the recompiler's fused conditions and flag-free forms against the interpreter (49M cases). Build with NDK clang `--target=aarch64-linux-android29 -std=c++20 -O2 -static-libstdc++ -I android/app/src/main/cpp`, push and run with adb. |

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Very low FPS; log says `AOT: ... does not match` | The phone's `SPEED2.EXE` differs from the one the APK was built from. Rebuild with `build_apk.ps1` from the same file you pushed. |
| Launcher says `SPEED2.EXE` not found | Push the data again (`-PushData`), or check the folder name `files/game/`. |
| Game stops at the disc check | The 0-byte `foobar` marker file is missing from `files/game/`. |
| `adb` lists the phone twice | Set `$env:ANDROID_SERIAL` to one of the entries. |
| Wireless ADB stops connecting | Re-enable Wireless debugging (its port changes), then `adb mdns services` / `adb connect`. |
| Status line at the bottom shows `Memory fault @… (EIP …)` or `CPU fault at …` | The game's code crashed; the details (registers, stack) are in `files/user/log.txt`. Please report the line. |
| Anything else | Read `files/user/log.txt` (and `log.prev.txt` after a crash). |

## Status and roadmap

Working: boot to gameplay, career and free-roam, saves, menus, rendering, audio, touch and
controller input, static recompilation with identity check.

Open:

- **Max graphics settings** ended in a "Memory fault" (guest crash) after applying. The suspected
  cause is fixed but not yet verified on the device. The game serialises its non-reentrant audio
  library by suspending the audio thread (`SuspendThread`, called from `0x5BE1E0`). The runtime
  ignored that call, so both threads raced on the library's context pointer (`0x8B7BB0`, fault at
  `0073927D`). `SuspendThread` now stops the target at its next safe point (guest function entry,
  interpreter step, or return from host code). Texture memory at max detail still needs measuring.
- **Audio decoder thread** still interprets its MMX code. It runs on its own core, but
  native MMX emission would cut its CPU use.
- **GPU driver overhead** (~17% at ~800 draws/frame) could be cut by caching GL state.
- **DXT textures** are decoded to RGBA8 (4–8× their size); transcoding to ASTC would save memory.
- **Controller mode:** native DirectInput gamepad (analog steering) is disabled (`kExposeGamepad`)
  until it matches the game's controller configuration. Rumble is not implemented yet.
- **Robustness:** SEH `RaiseException`, hiding the mouse cursor.
