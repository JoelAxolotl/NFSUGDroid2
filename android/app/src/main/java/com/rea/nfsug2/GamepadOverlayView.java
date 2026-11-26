package com.rea.nfsug2;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.util.SparseIntArray;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

/**
 * Multi-touch on-screen gamepad drawn over the game surface.
 *
 * Left half: floating analog stick (steering / menu navigation).
 * Right side: GAS (RT), BRAKE (LT), NOS (A), E-BRAKE (X), CAM (Y), BACK (B).
 * Top: shift down / up (LB / RB) and pause (START).
 *
 * Fingers may slide between buttons (e.g. gas to brake) without lifting.
 */
public class GamepadOverlayView extends View {
    private static final int OWNER_NONE = -1;
    private static final int OWNER_STICK = -2;

    /** One circular button. Positions and radius are fractions of the view height/width. */
    private static final class Btn {
        final String label;
        final int bit;        // NativeBridge.BTN_* or 0
        final int trigger;    // 0 none, 1 = LT, 2 = RT
        final float fx, fy, fr;
        float cx, cy, r;
        int pressCount;

        Btn(String label, int bit, int trigger, float fx, float fy, float fr) {
            this.label = label;
            this.bit = bit;
            this.trigger = trigger;
            this.fx = fx;
            this.fy = fy;
            this.fr = fr;
        }
    }

    private final Btn[] btns = {
            // Driving
            new Btn("GAS", 0, 2, 0.905f, 0.72f, 0.125f),
            new Btn("BRAKE", 0, 1, 0.745f, 0.80f, 0.105f),
            new Btn("NOS", NativeBridge.BTN_A, 0, 0.935f, 0.42f, 0.075f),
            new Btn("E-BRK", NativeBridge.BTN_X, 0, 0.815f, 0.52f, 0.075f),
            new Btn("◀ GEAR", NativeBridge.BTN_LB, 0, 0.065f, 0.13f, 0.06f),
            new Btn("GEAR ▶", NativeBridge.BTN_RB, 0, 0.185f, 0.13f, 0.06f),
            new Btn("CAM", NativeBridge.BTN_Y, 0, 0.815f, 0.13f, 0.06f),
            // Menus: OK = Enter, BACK = Esc, D-pad = arrow keys
            new Btn("OK", NativeBridge.BTN_OK, 0, 0.695f, 0.13f, 0.065f),
            new Btn("BACK", NativeBridge.BTN_B, 0, 0.935f, 0.13f, 0.06f),
            new Btn("▲", NativeBridge.BTN_DPAD_UP, 0, 0.115f, 0.36f, 0.055f),
            new Btn("▼", NativeBridge.BTN_DPAD_DOWN, 0, 0.115f, 0.58f, 0.055f),
            new Btn("◀", NativeBridge.BTN_DPAD_LEFT, 0, 0.055f, 0.47f, 0.055f),
            new Btn("▶", NativeBridge.BTN_DPAD_RIGHT, 0, 0.175f, 0.47f, 0.055f),
            new Btn("❚❚", NativeBridge.BTN_START, 0, 0.50f, 0.09f, 0.055f),
    };

    private final SparseIntArray owner = new SparseIntArray();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);

    private int stickPointer = -1;
    private float stickOx, stickOy, stickX, stickY, stickR;
    private int lastButtons;
    private float lastLx, lastLy, lastLt, lastRt;

    public GamepadOverlayView(Context ctx) {
        super(ctx);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(4f);
        text.setColor(Color.WHITE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        for (Btn b : btns) {
            b.cx = b.fx * w;
            b.cy = b.fy * h;
            b.r = b.fr * h;
        }
        stickR = 0.15f * h;
        text.setTextSize(0.035f * h);
    }

    private Btn hit(float x, float y) {
        Btn best = null;
        float bestD = Float.MAX_VALUE;
        for (Btn b : btns) {
            float dx = x - b.cx, dy = y - b.cy;
            float d = dx * dx + dy * dy;
            float rr = b.r * 1.2f;
            if (d <= rr * rr && d < bestD) {
                best = b;
                bestD = d;
            }
        }
        return best;
    }

    private int indexOf(Btn b) {
        for (int i = 0; i < btns.length; i++) if (btns[i] == b) return i;
        return OWNER_NONE;
    }

    private void release(int pointerId) {
        int o = owner.get(pointerId, OWNER_NONE);
        if (o >= 0) btns[o].pressCount--;
        if (o == OWNER_STICK) {
            stickPointer = -1;
            stickX = stickY = 0f;
        }
        owner.delete(pointerId);
    }

    private void pressButton(Btn b) {
        if (b.pressCount++ == 0) performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
    }

    private void press(int pointerId, float x, float y) {
        Btn b = hit(x, y);
        if (b != null) {
            pressButton(b);
            owner.put(pointerId, indexOf(b));
        } else if (x < getWidth() * 0.5f && y > getHeight() * 0.25f && stickPointer < 0) {
            stickPointer = pointerId;
            stickOx = x;
            stickOy = y;
            stickX = stickY = 0f;
            owner.put(pointerId, OWNER_STICK);
        } else {
            owner.put(pointerId, OWNER_NONE);
        }
    }

    private void move(int pointerId, float x, float y) {
        int o = owner.get(pointerId, OWNER_NONE);
        if (o == OWNER_STICK) {
            float dx = (x - stickOx) / stickR, dy = (y - stickOy) / stickR;
            float len = (float) Math.sqrt(dx * dx + dy * dy);
            if (len > 1f) { dx /= len; dy /= len; }
            stickX = dx;
            stickY = dy;
        } else if (o >= 0) {
            Btn now = hit(x, y);
            if (now != btns[o]) {
                btns[o].pressCount--;
                if (now != null) {
                    pressButton(now);
                    owner.put(pointerId, indexOf(now));
                } else {
                    owner.put(pointerId, OWNER_NONE);
                }
            }
        } else if (o == OWNER_NONE) {
            Btn now = hit(x, y);
            if (now != null) {
                pressButton(now);
                owner.put(pointerId, indexOf(now));
            }
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent ev) {
        int action = ev.getActionMasked();
        int idx = ev.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                press(ev.getPointerId(idx), ev.getX(idx), ev.getY(idx));
                break;
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < ev.getPointerCount(); i++) {
                    move(ev.getPointerId(i), ev.getX(i), ev.getY(i));
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                release(ev.getPointerId(idx));
                break;
            case MotionEvent.ACTION_CANCEL:
                for (int i = 0; i < ev.getPointerCount(); i++) release(ev.getPointerId(i));
                break;
            default:
                return true;
        }
        publish();
        invalidate();
        return true;
    }

    /** Clears all touches, e.g. when the overlay is hidden mid-press. */
    public void reset() {
        owner.clear();
        for (Btn b : btns) b.pressCount = 0;
        stickPointer = -1;
        stickX = stickY = 0f;
        publish();
        invalidate();
    }

    private void publish() {
        int buttons = 0;
        float lt = 0f, rt = 0f;
        for (Btn b : btns) {
            if (b.pressCount <= 0) continue;
            buttons |= b.bit;
            if (b.trigger == 1) lt = 1f;
            if (b.trigger == 2) rt = 1f;
        }
        if (buttons != lastButtons || stickX != lastLx || stickY != lastLy || lt != lastLt || rt != lastRt) {
            lastButtons = buttons;
            lastLx = stickX;
            lastLy = stickY;
            lastLt = lt;
            lastRt = rt;
            NativeBridge.setPadState(NativeBridge.SOURCE_TOUCH, buttons, stickX, stickY, 0f, 0f, lt, rt);
        }
    }

    @Override
    protected void onDraw(Canvas c) {
        for (Btn b : btns) {
            boolean on = b.pressCount > 0;
            fill.setColor(on ? Color.argb(150, 255, 170, 40) : Color.argb(70, 255, 255, 255));
            stroke.setColor(Color.argb(on ? 230 : 140, 255, 255, 255));
            c.drawCircle(b.cx, b.cy, b.r, fill);
            c.drawCircle(b.cx, b.cy, b.r, stroke);
            c.drawText(b.label, b.cx, b.cy - (text.descent() + text.ascent()) / 2f, text);
        }
        if (stickPointer >= 0) {
            fill.setColor(Color.argb(50, 255, 255, 255));
            stroke.setColor(Color.argb(140, 255, 255, 255));
            c.drawCircle(stickOx, stickOy, stickR, fill);
            c.drawCircle(stickOx, stickOy, stickR, stroke);
            fill.setColor(Color.argb(170, 255, 170, 40));
            c.drawCircle(stickOx + stickX * stickR, stickOy + stickY * stickR, stickR * 0.42f, fill);
        } else {
            // Hint where the stick lives.
            stroke.setColor(Color.argb(60, 255, 255, 255));
            c.drawCircle(getWidth() * 0.27f, getHeight() * 0.74f, stickR, stroke);
            c.drawText("STEER", getWidth() * 0.27f, getHeight() * 0.74f, text);
        }
    }
}
