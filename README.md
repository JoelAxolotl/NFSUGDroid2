# NFSUG2 for Android — x86 → ARM64 translation layer

Runs the Windows release of *Need for Speed Underground 2* (SPEED2.EXE, x86-32, D3D9)
on arm64 Android. Test device: Poco X6 Pro 5G (Dimensity 8300, Mali-G615, Android 16), over wireless ADB.

## Layout

| Path | Contents |
|---|---|
| `android/` | Gradle project: Java UI/input (`app/src/main/java`) and the C++ runtime (`app/src/main/cpp`) |
| `tools/run_device.ps1` | build → install (keeps data) → launch → print native log |
| `tools/pe_survey.py`, `tools/isa_census.py` | PE/import survey and instruction census of SPEED2.EXE |
| `analysis/` | generated survey output |

## Distribution model: bring your own game

This project ships **no game code or data**. It provides the translation layer
(runtime, Win32/DirectX on Android, input, audio) and a **static recompiler**.
Each user builds their own APK from their own official install:

```powershell
tools/build_apk.ps1 -GameDir "D:\Games\NFS Underground 2" -Install -PushData
```

`tools/recomp/recomp.py` reads the user's `SPEED2.EXE` **once, on their PC**, and
emits C++ into `android/app/src/main/cpp/generated/`, which clang compiles to
native ARM64 inside the APK. That generated code is derived from the user's copy
of the game: it must never be committed or published, and neither may the
resulting APK. The build records the exact EXE it was made from (size +
FNV-1a 64); at boot the runtime verifies the `SPEED2.EXE` on the device and only
enables the native code on a match, otherwise it falls back to the interpreter.

## Static recompilation

- Discovery: entry point, every code pointer in the relocation table (recovered from
  the unnamed section at RVA 0x4F5000), call targets to a fixed point, switch tables
  via relocated jump-table data, plus `user/aot_profile.txt` (call targets observed on
  device) - ~18.6k functions, 1.18M instructions.
- Emission: one C++ function per guest function; basic blocks are `goto` labels, direct
  calls are direct C++ calls, guest registers live in host locals, and instruction
  semantics come from the same helpers as the interpreter (`cpu/ops.h`, `cpu/x87_ops.h`).
  96% of instructions are emitted natively; the rest are single-stepped by the
  interpreter in place, and unresolvable control flow returns to the dispatcher.
- Nothing is translated at runtime; the interpreter only covers what the static pass
  could not prove (the FPS log reports `cpu/5s: N interpreted instructions`).

## Build and run

```powershell
tools/run_device.ps1            # build, install, autostart, tail log
tools/run_device.ps1 -Trace     # also log the first 32 calls of every Win32/COM function
```

Requirements: Android SDK with NDK 29.0.14206865 and CMake 3.22.1, JDK 21 (Gradle 8.9 wrapper, AGP 8.7.3).

Game data goes to app-specific storage (no permissions needed):
```
adb push SPEED2.EXE foobar GLOBAL FRONTEND LANGUAGES CARS SOUND TRACKS CREDITS NIS SDATA SUBTITLES MOVIES memcard \
    /sdcard/Android/data/com.rea.nfsug2/files/game/
```
**Never uninstall the app** — that deletes the pushed game files. `adb install -r` keeps them.
User data (log, `registry.txt`, saves under `profile/`) lives in `.../files/user/`.

## Architecture

```
Java: LauncherActivity → GameActivity (":game" process)
      SurfaceView + GamepadOverlayView (touch) + ControllerInput (gamepad/keyboard) → NativeBridge (JNI)
C++:  runtime/process   boot, PEB/TEB, guest threads, fault handler
      core/memory       guest 4 GiB address space in one host reservation (host = base + guest addr)
      loader/pe_loader  maps SPEED2.EXE at its fixed base 0x400000, binds the IAT to HLE thunks
      cpu/              x86-32 interpreter: integer ISA + lazy flags, x87 (double precision), MMX
      runtime/aot       hook for ahead-of-time translated functions (table empty for now)
      hle/              Win32 (kernel32, user32, gdi32, advapi32 registry, winmm, ws2_32 …) and COM
                        (DirectInput8, DirectDraw); hle/thunks = guest-visible entry points
      gfx/              D3D9 → GLES 3.2 (EGL context, device, resources, Clear/Present)
      audio/dsound      DirectSound buffers mixed into an AAudio stream
```

Guest → host calls go through 4-byte thunk slots at `0x7F000000`; host → guest callbacks
(WndProc, winmm timers, DirectInput enumeration) use `hle::call_guest`.

### Input

Touch overlay and physical controllers produce one XInput-style pad state. DirectInput exposes it
as an Xbox-360-style joystick (X/Y left stick, Z triggers, Rx/Ry right stick, 10 buttons, POV) and
also mirrors it onto keyboard keys (arrows, Enter, Esc, Space, Alt, …) for menus and default controls.
Physical keyboards map to DIK scan codes.

## Status (2026-11-27, update)

- Game code runs as statically recompiled native ARM64 (verified against the user's EXE at boot);
  the interpreter executes ~0.01% of instructions. Menus 45-60 FPS.
- Rendering: D3D9 -> GLES 3.2 (shader translator, fixed-function emulation, DXT decode). Audio via AAudio.
- Controls (keyboard mode, matching the game's own DirectInput action maps): GAS = Up+W, BRAKE = Down+S,
  stick/D-pad = arrows, E-BRK = Space, NOS = Left Alt (+Enter in menus), GEAR = Left Shift / Left Ctrl,
  CAM = C, BACK/pause = Esc, OK = Enter. The DirectInput gamepad is hidden (`kExposeGamepad`) until it is
  matched to the game's controller configuration.

### Performance notes (2026-11-27, free-roam)

Profiled on device with `simpleperf record --app com.rea.nfsug2` (symbols via the NDK's
`binary_cache_builder.py -lib android/app/build/intermediates/merged_native_libs/debug`).

| Change | Effect |
|---|---|
| x87 core is an `always_inline` header (`cpu/x87_ops.h` `x87_exec`); generated code passes constant op/modrm so each FPU instruction folds to its own case | removed 31% of main-thread time; driving 21 → ~55 FPS |
| Flag liveness + producer/branch fusion in `recomp.py` (`analyze_flags`): 85% of flag producers emit no flags, 100k `jcc` become direct compares (`aotrt::fc_*`) | removed lazy-flag materialization (6.7%) |
| `put_flags`/`fcomi` skip materialization when all flags are overwritten | |
| State blocks compiled to memcpy ranges | removed `copy_state` (2%) |
| Lock-free COM id lookup, sampled draw timing | targets ~9% mutex/clock overhead (pending measurement) |

`tools/tests/fused_flags_test.cpp` checks the fused conditions against the interpreter's
lazy flags (49M cases); build it with the NDK clang for `aarch64-linux-android29` and run via adb.

Next: the audio-decoder thread (0x72xxxx–0x73xxxx) still interprets its MMX code; Mali driver
time (~17%) from ~800 draws/frame could be cut with GL state caching.

## Status (2026-11-27, first boot)

- Boots to the game's main loop on the device: CRT init, registry, CD check (`foobar` marker),
  worker threads, D3D9 device (640x480), DirectInput, DirectSound → AAudio. Main loop runs at
  60 FPS (vsync) issuing ~80–160 draw calls per frame. **Nothing is drawn yet** (black frame).
- Interpreter speed ≈ 50 M guest instructions/s on one core (front-end only so far).

## Roadmap

1. **Rendering** — vertex/index upload, vertex declarations/FVF → VAOs, D3D9 shader bytecode
   (vs_1_1–2_0, ps_1_x–2_0) → GLSL ES 3.20 translator, fixed-function pipeline emulation,
   render/sampler state mapping, texture upload with DXT1/3/5 decode (Mali has no S3TC; consider
   ASTC transcode), Y-flip/half-pixel offset, state blocks.
2. **AOT translation (performance)** — host tool that discovers functions (entry point,
   91k relocation targets from the stripped reloc table at RVA 0x4F5000, vtables, interpreter miss
   log `user/aot_misses.txt`) and emits C++ per guest function into `cpp/generated/`, registered
   with `aot::register_table`. The interpreter remains the fallback.
3. **Robustness** — SEH dispatch for `RaiseException`, IDirectInputDevice8::GetObjectInfo (wheel
   wrapper spam), x86 memory ordering between guest threads, rumble → vibrator, graphics registry
   defaults (`g_*` values), widescreen/resolution options, pause/resume (WM_ACTIVATEAPP).
4. **SSE/3DNow!** — only if code paths reach them despite the x87-only CPUID.
