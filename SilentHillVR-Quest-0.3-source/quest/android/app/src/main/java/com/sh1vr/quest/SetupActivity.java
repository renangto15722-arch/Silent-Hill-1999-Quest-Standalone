package com.sh1vr.quest;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.util.Log;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/** 2D panel shown instead of the game when it cannot start yet: no all-files
 *  access, files waiting in Download, or no disc. Runs in its own process
 *  (":setup"), because GameActivity kills its process when it closes.
 *  Starts the game by itself once everything is in place. */
public class SetupActivity extends Activity {
    private static final String TAG = "SilentHillVR";

    private TextView title, body, status;
    private ProgressBar bar;
    private Button primary, quit;
    private boolean busy;
    private final Handler ui = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        buildUi();
        if (!Environment.isExternalStorageManager()) {
            showPermission();
            if (savedInstanceState == null) openAllFilesSettings();
        }
    }

    /** Look in Download once, as soon as file access is there. After that only
     *  the buttons do: the Quest resumes a panel over and over (several times a
     *  second while it is shown), and every resume used to rescan Download. */
    private boolean autoCheck = true;

    @Override
    protected void onResume() {
        super.onResume();
        if (busy) return;
        if (!Environment.isExternalStorageManager()) {
            showPermission();
            return;
        }
        if (autoCheck) {
            autoCheck = false;
            startImport();
        }
    }

    // ---- screens

    private void showPermission() {
        title.setText("Allow file access");
        body.setText("Silent Hill VR keeps the disc, saves and settings in\n"
            + GameActivity.DATA_ROOT + "\nso it needs \"All files access\".\n\n"
            + "Turn the switch on in the panel that opened, then come back here.\n\n"
            + "If no panel opened: Quest Settings > Apps > Silent Hill VR > Permissions.");
        hideProgress();
        primary.setText("Open settings");
        primary.setOnClickListener(v -> openAllFilesSettings());
        primary.setEnabled(true);
    }

    private void showNoDisc(int failed, List<String> problems) {
        title.setText("No Silent Hill disc found");
        body.setText("1. Open the Quest browser and download your own Silent Hill disc image: "
            + "the .bin file (and its .cue), or a .zip / .7z / .rar that has them.\n"
            + "2. Come back here and press Check again. The game moves it from the "
            + "Download folder into place by itself.\n\n"
            + "HD texture / HD maps packs (.zip / .7z / .rar) downloaded the same way are installed too.\n\n"
            + "You can also copy the files yourself to\n"
            + GameActivity.DATA_ROOT + "/gamedata");
        hideProgress();
        status.setText(problemText(failed, problems));
        primary.setText("Check again");
        primary.setOnClickListener(v -> startImport());
        primary.setEnabled(true);
    }

    // ---- work

    private void startImport() {
        busy = true;
        title.setText("Silent Hill VR");
        body.setText("Looking in the Download folder...");
        status.setText("");
        bar.setProgress(0);
        bar.setVisibility(android.view.View.VISIBLE);
        primary.setEnabled(false);

        final File root = new File(GameActivity.DATA_ROOT);
        new Thread(() -> {
            int failed = 0;
            List<String> problems = new ArrayList<>();
            boolean disc = false;
            try {
                DiscImport.Result r = DiscImport.importAll(root, (what, pct) -> ui.post(() -> {
                    body.setText(what);
                    bar.setProgress(pct);
                }));
                failed = r.failed;
                problems.addAll(r.problems);
                disc = DiscImport.hasDisc(new File(root, "gamedata"));
            } catch (Exception e) {
                Log.e(TAG, "setup failed", e);
                failed++;
            }
            final int f = failed;
            final boolean ok = disc;
            ui.post(() -> {
                busy = false;
                if (ok && f == 0 && problems.isEmpty()) startGame();
                else if (ok) {
                    // The disc is there; say what could not be used, then let them go on.
                    title.setText("Almost ready");
                    body.setText("The game can start. Some files in Download could not be used:");
                    status.setText(problemText(f, problems));
                    hideProgress();
                    primary.setText("Start the game");
                    primary.setOnClickListener(v -> startGame());
                    primary.setEnabled(true);
                } else showNoDisc(f, problems);
            });
        }, "SetupImport").start();
    }

    private static String problemText(int failed, List<String> problems) {
        StringBuilder sb = new StringBuilder();
        for (String p : problems) sb.append("- ").append(p).append('\n');
        if (failed > 0)
            sb.append(failed).append(" file(s) could not be installed (see SilentHillVR.log). ")
              .append("They are still in Download.");
        return sb.toString().trim();
    }

    private void startGame() {
        Intent i = new Intent(this, GameActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(i);
        finishAndRemoveTask();
    }

    private void openAllFilesSettings() {
        Intent[] tries = {
            new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                       Uri.parse("package:" + getPackageName())),
            new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION),
            new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                       Uri.parse("package:" + getPackageName())),
        };
        for (Intent i : tries) {
            try {
                startActivity(i);
                return;
            } catch (Exception e) {
                Log.w(TAG, "settings panel not available: " + i.getAction());
            }
        }
        status.setText("Could not open the settings panel. Use Quest Settings > Apps.");
    }

    // ---- layout (code only: no resources to keep in sync)

    private void buildUi() {
        int pad = dp(32);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(pad, pad, pad, pad);
        col.setBackgroundColor(Color.rgb(16, 16, 16));
        col.setGravity(Gravity.CENTER_HORIZONTAL);

        title = text(26, true);
        body = text(18, false);
        status = text(16, false);
        status.setTextColor(Color.rgb(230, 160, 90));

        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(100);

        primary = new Button(this);
        quit = new Button(this);
        quit.setText("Quit");
        quit.setOnClickListener(v -> finishAndRemoveTask());

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER);
        buttons.addView(primary);
        buttons.addView(quit);

        col.addView(title);
        col.addView(body, lp(dp(16)));
        col.addView(bar, lp(dp(16)));
        col.addView(status, lp(dp(8)));
        col.addView(buttons, lp(dp(24)));
        hideProgress();
        setContentView(col);
    }

    private TextView text(int sp, boolean bold) {
        TextView t = new TextView(this);
        t.setTextSize(sp);
        t.setTextColor(Color.rgb(235, 235, 235));
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private LinearLayout.LayoutParams lp(int top) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        p.topMargin = top;
        return p;
    }

    private void hideProgress() { bar.setVisibility(android.view.View.GONE); }

    private int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }
}
