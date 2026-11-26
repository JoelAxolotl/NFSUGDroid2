package com.rea.nfsug2;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

/**
 * Translates Android gamepad / joystick / keyboard events into the logical pad
 * state consumed by the native DirectInput layer.
 */
final class ControllerInput {
    interface Listener {
        /** Called whenever a physical controller produced input. */
        void onControllerActivity();
    }

    private final Listener listener;
    private int buttons;
    private float lx, ly, rx, ry, lt, rt;
    private float hatX, hatY;

    ControllerInput(Listener listener) {
        this.listener = listener;
    }

    static boolean isGamepadSource(int source) {
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                || (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
    }

    private static int buttonForKey(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: return NativeBridge.BTN_A;
            case KeyEvent.KEYCODE_BUTTON_B: return NativeBridge.BTN_B;
            case KeyEvent.KEYCODE_BUTTON_X: return NativeBridge.BTN_X;
            case KeyEvent.KEYCODE_BUTTON_Y: return NativeBridge.BTN_Y;
            case KeyEvent.KEYCODE_BUTTON_L1: return NativeBridge.BTN_LB;
            case KeyEvent.KEYCODE_BUTTON_R1: return NativeBridge.BTN_RB;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return NativeBridge.BTN_LTHUMB;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return NativeBridge.BTN_RTHUMB;
            case KeyEvent.KEYCODE_BUTTON_START: return NativeBridge.BTN_START;
            case KeyEvent.KEYCODE_BUTTON_SELECT:
            case KeyEvent.KEYCODE_BACK: return NativeBridge.BTN_BACK;
            case KeyEvent.KEYCODE_DPAD_UP: return NativeBridge.BTN_DPAD_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return NativeBridge.BTN_DPAD_DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return NativeBridge.BTN_DPAD_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return NativeBridge.BTN_DPAD_RIGHT;
            default: return 0;
        }
    }

    /** Returns true if the event was consumed. */
    boolean onKey(KeyEvent ev) {
        boolean down = ev.getAction() == KeyEvent.ACTION_DOWN;
        if (ev.getAction() != KeyEvent.ACTION_DOWN && ev.getAction() != KeyEvent.ACTION_UP) {
            return false;
        }
        int src = ev.getSource();
        if (isGamepadSource(src)) {
            int bit = buttonForKey(ev.getKeyCode());
            // Analog triggers on some pads also report L2/R2 key events.
            if (ev.getKeyCode() == KeyEvent.KEYCODE_BUTTON_L2) { lt = down ? 1f : 0f; push(); return true; }
            if (ev.getKeyCode() == KeyEvent.KEYCODE_BUTTON_R2) { rt = down ? 1f : 0f; push(); return true; }
            if (bit != 0) {
                buttons = down ? (buttons | bit) : (buttons & ~bit);
                push();
                return true;
            }
        }
        if ((src & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD) {
            int dik = KeyMap.toDik(ev.getKeyCode());
            if (dik != 0) {
                if (ev.getRepeatCount() == 0) NativeBridge.keyEvent(dik, down);
                return true;
            }
        }
        return false;
    }

    private static float axis(MotionEvent ev, InputDevice dev, int axis) {
        float v = ev.getAxisValue(axis);
        InputDevice.MotionRange r = dev != null ? dev.getMotionRange(axis, ev.getSource()) : null;
        float flat = r != null ? Math.max(r.getFlat(), 0.08f) : 0.08f;
        if (Math.abs(v) < flat) return 0f;
        // Rescale so the stick reaches full range just outside the dead zone.
        float s = (Math.abs(v) - flat) / (1f - flat);
        return Math.copySign(Math.min(s, 1f), v);
    }

    boolean onGenericMotion(MotionEvent ev) {
        if (!isGamepadSource(ev.getSource()) || ev.getAction() != MotionEvent.ACTION_MOVE) {
            return false;
        }
        InputDevice dev = ev.getDevice();
        lx = axis(ev, dev, MotionEvent.AXIS_X);
        ly = axis(ev, dev, MotionEvent.AXIS_Y);
        rx = axis(ev, dev, MotionEvent.AXIS_Z);
        ry = axis(ev, dev, MotionEvent.AXIS_RZ);
        lt = Math.max(ev.getAxisValue(MotionEvent.AXIS_LTRIGGER), ev.getAxisValue(MotionEvent.AXIS_BRAKE));
        rt = Math.max(ev.getAxisValue(MotionEvent.AXIS_RTRIGGER), ev.getAxisValue(MotionEvent.AXIS_GAS));

        // D-pads that report as a hat axis instead of key events.
        float nhx = ev.getAxisValue(MotionEvent.AXIS_HAT_X);
        float nhy = ev.getAxisValue(MotionEvent.AXIS_HAT_Y);
        if (nhx != hatX || nhy != hatY) {
            hatX = nhx;
            hatY = nhy;
            buttons &= ~(NativeBridge.BTN_DPAD_LEFT | NativeBridge.BTN_DPAD_RIGHT
                    | NativeBridge.BTN_DPAD_UP | NativeBridge.BTN_DPAD_DOWN);
            if (hatX < -0.5f) buttons |= NativeBridge.BTN_DPAD_LEFT;
            if (hatX > 0.5f) buttons |= NativeBridge.BTN_DPAD_RIGHT;
            if (hatY < -0.5f) buttons |= NativeBridge.BTN_DPAD_UP;
            if (hatY > 0.5f) buttons |= NativeBridge.BTN_DPAD_DOWN;
        }
        push();
        return true;
    }

    private void push() {
        NativeBridge.setPadState(NativeBridge.SOURCE_CONTROLLER, buttons, lx, ly, rx, ry, lt, rt);
        listener.onControllerActivity();
    }
}
