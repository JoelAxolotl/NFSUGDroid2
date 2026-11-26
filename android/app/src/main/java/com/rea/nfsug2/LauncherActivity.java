package com.rea.nfsug2;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;

/**
 * Entry screen: verifies the game data is present and starts the game process.
 * Launch with {@code --ez autostart true} to skip straight into the game (used by
 * the adb test scripts).
 */
public class LauncherActivity extends Activity {
    static final String PREFS = "nfsug2";
    static final String PREF_OVERLAY = "show_touch_overlay";

    private GamePaths paths;
    private TextView status;
    private Button start;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        paths = new GamePaths(this);
        paths.ensureDirs();

        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);

        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(48, 48, 48, 48);
        col.setBackgroundColor(Color.rgb(12, 14, 20));

        TextView title = new TextView(this);
        title.setText("Need for Speed Underground 2");
        title.setTextColor(Color.WHITE);
        title.setTextSize(26);
        col.addView(title);

        TextView sub = new TextView(this);
        sub.setText("x86 → ARM64 translation layer  v" + BuildConfigInfo.versionName(this));
        sub.setTextColor(Color.rgb(140, 150, 170));
        sub.setTextSize(14);
        col.addView(sub);

        status = new TextView(this);
        status.setTextColor(Color.rgb(220, 220, 220));
        status.setTextSize(14);
        status.setPadding(0, 32, 0, 32);
        col.addView(status);

        CheckBox overlay = new CheckBox(this);
        overlay.setText("Show on-screen gamepad (auto-hides while a controller is in use)");
        overlay.setTextColor(Color.WHITE);
        overlay.setChecked(prefs.getBoolean(PREF_OVERLAY, true));
        overlay.setOnCheckedChangeListener((b, on) ->
                prefs.edit().putBoolean(PREF_OVERLAY, on).apply());
        col.addView(overlay);

        start = new Button(this);
        start.setText("Start game");
        start.setOnClickListener(v -> launchGame());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.gravity = Gravity.START;
        lp.topMargin = 24;
        col.addView(start, lp);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(col);
        setContentView(scroll);

        if (getIntent().getBooleanExtra("autostart", false) && refresh()) {
            launchGame();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    private boolean refresh() {
        File exe = paths.findExecutable();
        boolean ok = exe != null;
        StringBuilder sb = new StringBuilder();
        sb.append("Game folder:\n  ").append(paths.game.getAbsolutePath()).append("\n\n");
        if (ok) {
            sb.append("Found ").append(exe.getName()).append(" (")
              .append(exe.length()).append(" bytes)\n");
        } else {
            sb.append("SPEED2.EXE not found.\n\nCopy your installed game folder there, e.g.\n")
              .append("  adb push \"NFS Underground 2/.\" ")
              .append(paths.game.getAbsolutePath()).append("\n");
        }
        sb.append("\nUser data (saves, settings, logs):\n  ").append(paths.user.getAbsolutePath());
        status.setText(sb.toString());
        start.setEnabled(ok);
        return ok;
    }

    private void launchGame() {
        startActivity(new Intent(this, GameActivity.class));
    }
}
