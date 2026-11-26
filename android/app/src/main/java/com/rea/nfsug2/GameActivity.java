package com.rea.nfsug2;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

/**
 * Hosts the game. Runs in its own process (":game") because the guest owns
 * process-wide native state and may call ExitProcess.
 */
public class GameActivity extends Activity implements SurfaceHolder.Callback, ControllerInput.Listener {
    private final Handler ui = new Handler(Looper.getMainLooper());
    private GamepadOverlayView overlay;
    private TextView hud;
    private ControllerInput controller;
    private boolean overlayEnabled;
    private boolean exiting;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getAttributes().layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;

        GamePaths paths = new GamePaths(this);
        paths.ensureDirs();
        overlayEnabled = getSharedPreferences(LauncherActivity.PREFS, MODE_PRIVATE)
                .getBoolean(LauncherActivity.PREF_OVERLAY, true);

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        SurfaceView surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        root.addView(surface);

        overlay = new GamepadOverlayView(this);
        overlay.setVisibility(overlayEnabled ? View.VISIBLE : View.GONE);
        root.addView(overlay);

        hud = new TextView(this);
        hud.setTextColor(Color.argb(200, 180, 255, 180));
        hud.setTextSize(11);
        hud.setPadding(24, 8, 24, 8);
        FrameLayout.LayoutParams hlp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT);
        hlp.gravity = android.view.Gravity.BOTTOM | android.view.Gravity.CENTER_HORIZONTAL;
        root.addView(hud, hlp);
        setContentView(root);

        controller = new ControllerInput(this);

        if (!NativeBridge.start(paths.game.getAbsolutePath(), paths.user.getAbsolutePath())) {
            hud.setText("Failed to start: " + NativeBridge.status());
        }
        ui.post(statusPoll);
    }

    private final Runnable statusPoll = new Runnable() {
        @Override
        public void run() {
            hud.setText(NativeBridge.status());
            if (!NativeBridge.isRunning()) {
                // The guest exited; give the user a moment to read the final status.
                // The process is killed in onDestroy, after the activity has left the
                // task, so Android does not restart it.
                exiting = true;
                ui.postDelayed(GameActivity.this::finishAndRemoveTask, 4000);
                return;
            }
            ui.postDelayed(this, 250);
        }
    };

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemUi();
    }

    private void hideSystemUi() {
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) {
            c.hide(WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (exiting) Process.killProcess(Process.myPid());
    }

    @Override
    protected void onPause() {
        super.onPause();
        NativeBridge.setPaused(true);
    }

    @Override
    protected void onResume() {
        super.onResume();
        NativeBridge.setPaused(false);
    }

    // --- SurfaceHolder.Callback ---
    @Override
    public void surfaceCreated(SurfaceHolder holder) {}

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        NativeBridge.surfaceChanged(holder.getSurface(), width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        NativeBridge.surfaceDestroyed();
    }

    // --- Physical controllers / keyboards ---
    @Override
    public void onControllerActivity() {
        if (overlay.getVisibility() == View.VISIBLE) {
            overlay.reset();
            overlay.setVisibility(View.GONE);
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent ev) {
        if (controller.onKey(ev)) return true;
        return super.dispatchKeyEvent(ev);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent ev) {
        if (controller.onGenericMotion(ev)) return true;
        return super.dispatchGenericMotionEvent(ev);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent ev) {
        // Any touch brings the overlay back after controller use.
        if (overlayEnabled && overlay.getVisibility() != View.VISIBLE
                && ev.getActionMasked() == MotionEvent.ACTION_DOWN) {
            overlay.setVisibility(View.VISIBLE);
        }
        return super.dispatchTouchEvent(ev);
    }
}
