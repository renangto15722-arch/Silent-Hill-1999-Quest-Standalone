package com.sh1vr.quest;

import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/** The immersive game: SDL's activity running libmain.so (SDL_main), which
 *  starts OpenXR itself. Data dir: /sdcard/SilentHillVR (all-files access;
 *  SetupActivity gets it, imports downloads and checks for the disc first). */
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

    /** APK assets folder holding the data the game needs besides the disc
     *  (config, VR hands, the port's gamedata files). */
    private static final String ASSET_ROOT = "shdata";
    private static final String MARKER = ".installed_version";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        File root = new File(DATA_ROOT);
        // Not ready to play (no all-files access, downloads waiting to be
        // imported, or no disc): the 2D setup panel handles it and starts the
        // game again when done. SDLActivity still needs its onCreate; finishing
        // here goes straight to onDestroy, before SDL_main ever runs.
        if (!Environment.isExternalStorageManager() || DiscImport.needsSetup(root)) {
            Log.i(TAG, "not ready to play; opening setup");
            startActivity(new Intent(this, SetupActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            super.onCreate(savedInstanceState);
            finish();
            return;
        }
        Log.i(TAG, "data dir: " + DATA_ROOT);
        // Before the game reads config.cfg: swap out the old default one
        // (wrong gun aim) for the current default. Saves are not in it.
        ConfigFix.run(root, () -> getAssets().open(ASSET_ROOT + "/config.cfg"));
        installData(root);
        super.onCreate(savedInstanceState);
    }

    /** Unpacks the bundled data into the data dir: on the first run and after
     *  every app update. config.cfg is only written when missing (it holds the
     *  player's settings); saves, the disc and texture packs are never touched. */
    private void installData(File root) {
        new File(root, "gamedata/save").mkdirs();
        new File(root, "gamedata/texturemods").mkdirs();

        String version = "?";
        try {
            PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
            version = pi.getLongVersionCode() + "-" + pi.lastUpdateTime;
        } catch (Exception e) {
            Log.w(TAG, "no package info", e);
        }
        File marker = new File(root, MARKER);
        if (version.equals(readText(marker))) return;

        try {
            copyTree(getAssets(), ASSET_ROOT, root);
            writeText(marker, version);
            Log.i(TAG, "bundled data installed into " + root + " (" + version + ")");
        } catch (IOException e) {
            Log.e(TAG, "installing the bundled data failed", e);
        }
    }

    private static void copyTree(AssetManager am, String asset, File dst) throws IOException {
        String[] kids = am.list(asset);
        if (kids != null && kids.length > 0) {
            dst.mkdirs();
            for (String k : kids) copyTree(am, asset + "/" + k, new File(dst, k));
            return;
        }
        // a file (asset dirs are never empty in an APK)
        if (dst.getName().equals("config.cfg") && dst.exists()) return;
        File tmp = new File(dst.getPath() + ".tmp");
        try (InputStream in = am.open(asset); OutputStream out = new FileOutputStream(tmp)) {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
        if (dst.exists()) dst.delete();
        if (!tmp.renameTo(dst)) throw new IOException("cannot write " + dst);
    }

    private static String readText(File f) {
        try (InputStream in = new java.io.FileInputStream(f)) {
            byte[] b = new byte[256];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n, StandardCharsets.UTF_8).trim() : "";
        } catch (IOException e) {
            return "";
        }
    }

    private static void writeText(File f, String s) {
        try (OutputStream out = new FileOutputStream(f)) {
            out.write(s.getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            Log.w(TAG, "cannot write " + f, e);
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // A relaunch must start a fresh process: the game's globals are not
        // re-initialisable.
        android.os.Process.killProcess(android.os.Process.myPid());
    }
}
