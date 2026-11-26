package com.rea.nfsug2;

import android.view.Surface;

/** JNI surface of libnfsug2.so. All methods are thread-safe on the native side. */
public final class NativeBridge {
    static {
        System.loadLibrary("nfsug2");
    }

    private NativeBridge() {}

    // Logical pad buttons (XInput bit layout).
    public static final int BTN_DPAD_UP = 0x0001;
    public static final int BTN_DPAD_DOWN = 0x0002;
    public static final int BTN_DPAD_LEFT = 0x0004;
    public static final int BTN_DPAD_RIGHT = 0x0008;
    public static final int BTN_START = 0x0010;
    public static final int BTN_BACK = 0x0020;
    public static final int BTN_LTHUMB = 0x0040;
    public static final int BTN_RTHUMB = 0x0080;
    public static final int BTN_LB = 0x0100;
    public static final int BTN_RB = 0x0200;
    public static final int BTN_A = 0x1000;
    public static final int BTN_B = 0x2000;
    public static final int BTN_X = 0x4000;
    public static final int BTN_Y = 0x8000;
    /** Overlay-only menu button: sent to the game as the Enter key. */
    public static final int BTN_OK = 0x10000;

    /** Input sources; the native side merges them so touch and controller can coexist. */
    public static final int SOURCE_TOUCH = 0;
    public static final int SOURCE_CONTROLLER = 1;

    /** Boots the guest once per process. Returns false if the executable cannot be loaded. */
    public static native boolean start(String gameDir, String userDir);

    public static native void surfaceChanged(Surface surface, int width, int height);

    public static native void surfaceDestroyed();

    public static native void setPaused(boolean paused);

    /** Axes are in [-1, 1] (sticks, +y = down) and [0, 1] (triggers). */
    public static native void setPadState(int source, int buttons,
                                          float lx, float ly, float rx, float ry,
                                          float lt, float rt);

    /** Physical keyboard: DirectInput scan code (DIK_*). */
    public static native void keyEvent(int dikScanCode, boolean down);

    /** One-line human readable boot/run status for the HUD. */
    public static native String status();

    /** True until the guest process calls ExitProcess or faults fatally. */
    public static native boolean isRunning();
}
