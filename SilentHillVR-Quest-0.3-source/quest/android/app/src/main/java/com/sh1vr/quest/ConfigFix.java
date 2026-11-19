package com.sh1vr.quest;

import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/** 0.2 and the first 0.3 builds shipped a default config.cfg with pre-tuning
 *  gun aim (vr_aim_up = -26 ...) that overrode the tuned per-weapon defaults:
 *  every gun shot off target. A config.cfg still carrying that value is
 *  replaced by the current default (the old one is kept as config.cfg.bak).
 *  Saves live in gamedata/save and are never touched. */
final class ConfigFix {
    private static final String TAG = "SilentHillVR";

    /** Only the old bundled config had this line (written by the game as is). */
    private static final String OLD_MARK = "vr_aim_up = -26.000";

    interface Source { InputStream open() throws IOException; }

    private ConfigFix() {}

    /** Returns true when config.cfg was replaced. */
    static boolean run(File root, Source newDefault) {
        File cfg = new File(root, "config.cfg");
        if (!cfg.isFile() || !hasOldMark(cfg)) return false;
        File bak = new File(root, "config.cfg.bak");
        File tmp = new File(root, "config.cfg.tmp");
        try {
            try (InputStream in = newDefault.open(); OutputStream out = new FileOutputStream(tmp)) {
                byte[] buf = new byte[1 << 16];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            }
            if (bak.exists() && !bak.delete()) throw new IOException("cannot replace " + bak);
            if (!cfg.renameTo(bak)) throw new IOException("cannot keep the old config as " + bak);
            if (!tmp.renameTo(cfg)) {
                bak.renameTo(cfg);   // put the old one back rather than leave none
                throw new IOException("cannot write " + cfg);
            }
            Log.i(TAG, "old default config replaced (aim fix); previous one kept as " + bak);
            return true;
        } catch (IOException e) {
            Log.e(TAG, "config fix failed; config.cfg left as it was", e);
            tmp.delete();
            return false;
        }
    }

    private static boolean hasOldMark(File cfg) {
        if (cfg.length() > (1 << 20)) return false;
        try (InputStream in = new FileInputStream(cfg)) {
            byte[] b = new byte[(int) cfg.length()];
            int got = 0, n;
            while (got < b.length && (n = in.read(b, got, b.length - got)) > 0) got += n;
            for (String line : new String(b, 0, got, StandardCharsets.UTF_8).split("\n"))
                if (line.trim().equals(OLD_MARK)) return true;
        } catch (IOException e) {
            Log.w(TAG, "cannot read " + cfg, e);
        }
        return false;
    }
}
