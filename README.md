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

## Status (2026-11-27)

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
