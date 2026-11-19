package com.sh1vr.quest;

import android.os.Environment;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** Picks up what the player downloaded with the Quest browser (/sdcard/Download)
 *  and moves it where the game reads it, so no PC or adb is needed:
 *   - Silent Hill .bin (+ its .cue), loose or inside a .zip  -> gamedata/
 *   - texture pack .zip (texupload-* entries)                 -> gamedata/texturemods/ (kept zipped)
 *   - HD maps / menus .zip (TIM, 1ST, ITEM ... folders)       -> extracted into gamedata/load/
 *  Same sorting rules as the PC installer (InstallSH-Quest.bat). Unrelated
 *  files are left alone; nothing that already exists in the destination is
 *  overwritten. */
final class DiscImport {
    private static final String TAG = "SilentHillVR";

    /** Must match LOADFOLDERS in InstallSH-Quest.bat. */
    private static final String[] LOAD_FOLDERS =
        { "TIM", "1ST", "ITEM", "BG", "CHARA", "ANIM", "MISC", "SND", "VIN", "XA", "TEST" };

    /** Boot executables of the Silent Hill discs (USA, PAL, NTSC-J). Any other
     *  PSX disc is only taken when its file name says "silent hill". */
    private static final String[] SH_SERIALS =
        { "SLUS_007.07", "SLES_015.14", "SLPM_861.92" };

    enum Kind { BIN, CUE, ZIP_DISC, ZIP_TEXTURES, ZIP_LOAD }

    static final class Job {
        final Kind kind;
        final File src;
        Job(Kind kind, File src) { this.kind = kind; this.src = src; }
        @Override public String toString() { return kind + " " + src.getName(); }
    }

    interface Progress { void step(String what, int percent); }

    private DiscImport() {}

    static File downloads() {
        return Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
    }

    /** True when the game cannot start straight away: something in Download
     *  is waiting to be imported, or there is no disc in gamedata/ at all. */
    static boolean needsSetup(File root) {
        try {
            return !plan(root).isEmpty() || !hasDisc(new File(root, "gamedata"));
        } catch (Exception e) {
            Log.w(TAG, "import check failed; starting the game anyway", e);
            return false;
        }
    }

    /** Same rule as the native disc pick: any .bin with a PSX boot executable. */
    static boolean hasDisc(File gamedata) {
        File[] files = gamedata.listFiles();
        if (files == null) return false;
        for (File f : files) {
            if (f.isFile() && lower(f.getName()).endsWith(".bin") && bootName(fileSource(f)) != null)
                return true;
        }
        return false;
    }

    /** What would be imported now (read-only: looks, never moves). */
    static List<Job> plan(File root) {
        List<Job> jobs = new ArrayList<>();
        File[] files = downloads().listFiles();
        if (files == null) return jobs;

        File gamedata = new File(root, "gamedata");
        Set<String> discBins = new HashSet<>();   // lower-case names of SH .bin files
        File[] have = gamedata.listFiles();
        if (have != null) {
            for (File f : have) {
                String n = lower(f.getName());
                if (f.isFile() && n.endsWith(".bin")) discBins.add(n);
            }
        }

        List<File> cues = new ArrayList<>();
        for (File f : files) {
            if (!f.isFile()) continue;
            String n = lower(f.getName());
            if (n.endsWith(".bin")) {
                if (isSilentHill(f.getName(), fileSource(f))) {
                    discBins.add(n);
                    if (!new File(gamedata, f.getName()).exists()) jobs.add(new Job(Kind.BIN, f));
                }
            } else if (n.endsWith(".cue")) {
                cues.add(f);
            } else if (n.endsWith(".zip")) {
                Job j = classifyZip(f, root);
                if (j != null) jobs.add(j);
            }
        }
        for (File cue : cues) {
            String bin = cueBinName(cue);
            if (bin != null && discBins.contains(lower(bin))
                    && !new File(gamedata, cue.getName()).exists())
                jobs.add(new Job(Kind.CUE, cue));
        }
        return jobs;
    }

    private static Job classifyZip(File zip, File root) {
        try (ZipFile z = new ZipFile(zip)) {
            boolean textures = false, load = false;
            ZipEntry disc = null;
            Enumeration<? extends ZipEntry> en = z.entries();
            while (en.hasMoreElements()) {
                ZipEntry e = en.nextElement();
                String path = e.getName().replace('\\', '/');
                String base = path.substring(path.lastIndexOf('/') + 1);
                if (lower(base).startsWith("texupload-")) textures = true;
                if (loadFolderStart(path) >= 0) load = true;
                if (disc == null && !e.isDirectory() && lower(base).endsWith(".bin")) {
                    final ZipEntry entry = e;
                    if (isSilentHill(base, (off, buf) -> readZipAt(z, entry, off, buf))) disc = e;
                }
            }
            File gamedata = new File(root, "gamedata");
            if (disc != null) {
                String name = disc.getName().replace('\\', '/');
                name = name.substring(name.lastIndexOf('/') + 1);
                return new File(gamedata, name).exists() ? null : new Job(Kind.ZIP_DISC, zip);
            }
            if (textures) {
                return new File(gamedata, "texturemods/" + zip.getName()).exists()
                     ? null : new Job(Kind.ZIP_TEXTURES, zip);
            }
            if (load) return new Job(Kind.ZIP_LOAD, zip);
        } catch (IOException e) {
            Log.w(TAG, "not a readable zip: " + zip, e);
        }
        return null;
    }

    /** Runs the jobs from plan(). A failed job is logged and skipped; its
     *  source stays in Download. Returns how many jobs failed. */
    static int run(File root, List<Job> jobs, Progress progress) {
        File gamedata = new File(root, "gamedata");
        new File(gamedata, "texturemods").mkdirs();
        new File(gamedata, "load").mkdirs();
        int failed = 0;
        for (int i = 0; i < jobs.size(); i++) {
            Job j = jobs.get(i);
            int base = i * 100 / jobs.size();
            int span = 100 / jobs.size();
            Progress sub = (what, pct) -> progress.step(what, base + pct * span / 100);
            try {
                switch (j.kind) {
                    case BIN:
                    case CUE:
                        sub.step("Moving " + j.src.getName(), 0);
                        move(j.src, new File(gamedata, j.src.getName()));
                        break;
                    case ZIP_TEXTURES:
                        sub.step("Moving texture pack " + j.src.getName(), 0);
                        move(j.src, new File(gamedata, "texturemods/" + j.src.getName()));
                        break;
                    case ZIP_DISC:
                        extractDisc(j.src, gamedata, sub);
                        break;
                    case ZIP_LOAD:
                        extractLoad(j.src, new File(gamedata, "load"), sub);
                        break;
                }
                Log.i(TAG, "imported " + j);
            } catch (IOException e) {
                failed++;
                Log.e(TAG, "import failed: " + j, e);
            }
        }
        progress.step("Done", 100);
        return failed;
    }

    // ---- moving / extracting

    /** Rename (same storage: instant), else copy + delete. */
    private static void move(File src, File dst) throws IOException {
        if (dst.exists()) throw new IOException("already exists: " + dst);
        dst.getParentFile().mkdirs();
        if (src.renameTo(dst)) return;
        File tmp = new File(dst.getPath() + ".tmp");
        try (InputStream in = new FileInputStream(src); OutputStream out = new FileOutputStream(tmp)) {
            copy(in, out, null, 0);
        }
        if (!tmp.renameTo(dst)) throw new IOException("cannot write " + dst);
        if (!src.delete()) Log.w(TAG, "copied but could not delete " + src);
    }

    /** The disc's .bin and .cue go to gamedata/; then the zip is deleted. */
    private static void extractDisc(File zip, File gamedata, Progress p) throws IOException {
        try (ZipFile z = new ZipFile(zip)) {
            long total = 0;
            List<ZipEntry> want = new ArrayList<>();
            Enumeration<? extends ZipEntry> en = z.entries();
            while (en.hasMoreElements()) {
                ZipEntry e = en.nextElement();
                String n = lower(e.getName());
                if (!e.isDirectory() && (n.endsWith(".bin") || n.endsWith(".cue"))) {
                    want.add(e);
                    total += Math.max(0, e.getSize());
                }
            }
            long[] done = { 0 };
            for (ZipEntry e : want) {
                String name = e.getName().replace('\\', '/');
                name = name.substring(name.lastIndexOf('/') + 1);
                File dst = new File(gamedata, name);
                if (dst.exists()) continue;
                p.step("Unpacking " + name, pct(done[0], total));
                extractEntry(z, e, dst, done, total, p, "Unpacking " + name);
            }
        }
        if (!zip.delete()) Log.w(TAG, "unpacked but could not delete " + zip);
    }

    /** Every TIM/1ST/ITEM... folder in the zip lands in gamedata/load/ (the
     *  path above that folder is dropped); then the zip is deleted. */
    private static void extractLoad(File zip, File load, Progress p) throws IOException {
        String loadPath = load.getCanonicalPath() + File.separator;
        try (ZipFile z = new ZipFile(zip)) {
            long total = 0;
            List<ZipEntry> want = new ArrayList<>();
            Enumeration<? extends ZipEntry> en = z.entries();
            while (en.hasMoreElements()) {
                ZipEntry e = en.nextElement();
                if (!e.isDirectory() && loadFolderStart(e.getName().replace('\\', '/')) >= 0) {
                    want.add(e);
                    total += Math.max(0, e.getSize());
                }
            }
            long[] done = { 0 };
            String label = "Installing HD pack " + zip.getName();
            for (ZipEntry e : want) {
                String path = e.getName().replace('\\', '/');
                File dst = new File(load, path.substring(loadFolderStart(path)));
                if (!dst.getCanonicalPath().startsWith(loadPath))
                    throw new IOException("bad zip path: " + path);
                if (dst.exists()) { done[0] += Math.max(0, e.getSize()); continue; }
                dst.getParentFile().mkdirs();
                extractEntry(z, e, dst, done, total, p, label);
            }
        }
        if (!zip.delete()) Log.w(TAG, "unpacked but could not delete " + zip);
    }

    private static void extractEntry(ZipFile z, ZipEntry e, File dst, long[] done, long total,
                                     Progress p, String label) throws IOException {
        File tmp = new File(dst.getPath() + ".tmp");
        try (InputStream in = z.getInputStream(e); OutputStream out = new FileOutputStream(tmp)) {
            copy(in, out, (n) -> {
                done[0] += n;
                p.step(label, pct(done[0], total));
            }, 4 << 20);
        }
        if (!tmp.renameTo(dst)) throw new IOException("cannot write " + dst);
    }

    private interface Counter { void add(long n); }

    /** Copies in → out; reports to counter about every reportEvery bytes. */
    private static void copy(InputStream in, OutputStream out, Counter counter, long reportEvery)
            throws IOException {
        byte[] buf = new byte[1 << 16];
        long pending = 0;
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
            pending += n;
            if (counter != null && pending >= reportEvery) { counter.add(pending); pending = 0; }
        }
        if (counter != null && pending > 0) counter.add(pending);
    }

    private static int pct(long done, long total) {
        return total <= 0 ? 0 : (int) Math.min(100, done * 100 / total);
    }

    /** Index in path where its first TIM/1ST/ITEM... folder starts, or -1.
     *  The folder must be a directory in the path (not the file name). */
    private static int loadFolderStart(String path) {
        String[] parts = path.split("/");
        int at = 0;
        for (int i = 0; i < parts.length - 1; i++) {
            for (String f : LOAD_FOLDERS) if (parts[i].equalsIgnoreCase(f)) return at;
            at += parts[i].length() + 1;
        }
        return -1;
    }

    // ---- disc identification (port of Pc_DetectRegionFromBin, main_pc.c)

    private interface ByteSource { int readAt(long off, byte[] buf) throws IOException; }

    private static ByteSource fileSource(File f) {
        return (off, buf) -> {
            try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
                r.seek(off);
                int got = 0, n;
                while (got < buf.length && (n = r.read(buf, got, buf.length - got)) > 0) got += n;
                return got;
            }
        };
    }

    private static int readZipAt(ZipFile z, ZipEntry e, long off, byte[] buf) throws IOException {
        try (InputStream in = z.getInputStream(e)) {
            long left = off;
            while (left > 0) {
                long s = in.skip(left);
                if (s <= 0) { if (in.read() < 0) return 0; s = 1; }
                left -= s;
            }
            int got = 0, n;
            while (got < buf.length && (n = in.read(buf, got, buf.length - got)) > 0) got += n;
            return got;
        }
    }

    private static boolean isSilentHill(String fileName, ByteSource src) {
        String boot = bootName(src);
        if (boot == null) return false;
        for (String s : SH_SERIALS) if (boot.startsWith(s)) return true;
        return lower(fileName).contains("silent hill");
    }

    /** Name of the SLUS/SLES/SLPM/SLPS/SIPS boot file in the ISO9660 root of a
     *  raw (2352-byte sector) PSX image, or null when it is not one. */
    private static String bootName(ByteSource src) {
        try {
            byte[] sec = new byte[2048];
            if (src.readAt(16L * 2352 + 24, sec) != 2048) return null;
            long rlba = (sec[158] & 0xFF) | (sec[159] & 0xFF) << 8 | (sec[160] & 0xFF) << 16
                      | (long) (sec[161] & 0xFF) << 24;
            if (src.readAt(rlba * 2352 + 24, sec) != 2048) return null;
            for (int o = 0; o + 33 < 2048; ) {
                int len = sec[o] & 0xFF, nl = sec[o + 32] & 0xFF;
                if (len == 0) break;
                if (o + 33 + nl <= 2048 && nl >= 4) {
                    String name = new String(sec, o + 33, nl, StandardCharsets.US_ASCII);
                    if (name.startsWith("SLUS") || name.startsWith("SLES") || name.startsWith("SLPM")
                            || name.startsWith("SLPS") || name.startsWith("SIPS"))
                        return name;
                }
                o += len;
            }
        } catch (IOException e) {
            Log.w(TAG, "cannot read disc header", e);
        }
        return null;
    }

    private static final Pattern CUE_FILE = Pattern.compile("FILE\\s+\"([^\"]+)\"", Pattern.CASE_INSENSITIVE);

    /** The .bin a .cue points at (first FILE line), or null. */
    private static String cueBinName(File cue) {
        if (cue.length() > 65536) return null;
        try (InputStream in = new FileInputStream(cue)) {
            byte[] b = new byte[(int) cue.length()];
            int got = 0, n;
            while (got < b.length && (n = in.read(b, got, b.length - got)) > 0) got += n;
            Matcher m = CUE_FILE.matcher(new String(b, 0, got, StandardCharsets.ISO_8859_1));
            if (!m.find()) return null;
            String name = m.group(1).replace('\\', '/');
            return name.substring(name.lastIndexOf('/') + 1);
        } catch (IOException e) {
            return null;
        }
    }

    private static String lower(String s) { return s.toLowerCase(Locale.ROOT); }
}
