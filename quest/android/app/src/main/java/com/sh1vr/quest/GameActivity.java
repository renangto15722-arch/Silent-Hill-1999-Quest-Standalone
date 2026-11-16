package com.sh1vr.quest;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;

/** The immersive game: SDL's activity running libmain.so (SDL_main), which
 *  starts OpenXR itself. Data dir: getExternalFilesDir(null). */
public class GameActivity extends SDLActivity {
    private static final String TAG = "SilentHillVR";

    @Override
    protected String[] getLibraries() {
        // The last entry is the one SDL calls SDL_main in. libopenxr_loader.so
        // is dlopen()ed by the game (vr_openxr.c).
        return new String[] { "SDL2", "openal", "main" };
    }

    /** Must match main_pc.c (SH_QUEST data root). */
    static final String DATA_ROOT = "/sdcard/SilentHillVR";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        if (Environment.isExternalStorageManager()) {
            new File(DATA_ROOT, "gamedata").mkdirs();
            Log.i(TAG, "data dir: " + DATA_ROOT);
        } else {
            // No all-files access yet: ask once (a 2D system panel). The game
            // falls back to the app's own files dir until it is granted.
            Log.w(TAG, "no all-files access; data falls back to " + getExternalFilesDir(null));
            try {
                Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                      Uri.parse("package:" + getPackageName()));
                startActivity(i);
            } catch (Exception e) {
                Log.w(TAG, "cannot open the all-files access panel", e);
            }
            File dir = getExternalFilesDir(null);
            if (dir != null) new File(dir, "gamedata").mkdirs();
        }
        super.onCreate(savedInstanceState);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // A relaunch must start a fresh process: the game's globals are not
        // re-initialisable.
        android.os.Process.killProcess(android.os.Process.myPid());
    }
}
