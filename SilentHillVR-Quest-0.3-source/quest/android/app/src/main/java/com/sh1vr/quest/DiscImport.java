package com.sh1vr.quest;

import android.os.Environment;
import android.util.Log;

import com.github.junrar.Archive;
import com.github.junrar.exception.RarException;
import com.github.junrar.rarfile.FileHeader;

import org.apache.commons.compress.archivers.sevenz.SevenZArchiveEntry;
import org.apache.commons.compress.archivers.sevenz.SevenZFile;

import java.io.Closeable;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
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
 *   - Silent Hill .bin (+ its .cue), loose or inside a .zip/.7z/.rar  -> gamedata/
 *   - texture pack (texupload-* files): .zip moved as is              -> gamedata/texturemods/
 *                                       .7z/.rar unpacked into        -> gamedata/texturemods/<name>/
 *   - HD maps / menus (TIM, 1ST, ITEM ... folders), any archive       -> unpacked into gamedata/load/
 *   - an archive that only holds other archives (a .rar inside a .7z) -> unpacked into Download, then
 *     those are sorted the same way
 *  Same sorting rules as the PC installer (InstallSH-Quest.bat). Unrelated
 *  files are left alone; nothing that already exists in the destination is
 *  overwritten. RAR5 cannot be read (junrar only knows RAR up to v4). */
final class DiscImport {
    private static final String TAG = "SilentHillVR";

    /** Must match LOADFOLDERS in InstallSH-Quest.bat. */
    private static final String[] LOAD_FOLDERS =
        { "TIM", "1ST", "ITEM", "BG", "CHARA", "ANIM", "MISC", "SND", "VIN", "XA", "TEST" };

    /** Boot executables of the Silent Hill discs (USA, PAL, NTSC-J). Any other
     *  PSX disc is only taken when its file name says "silent hill". */
    private static final String[] SH_SERIALS =
        { "SLUS_007.07", "SLES_015.14", "SLPM_861.92" };

    /** Names that look like Silent Hill: an archive with one of these names
     *  that cannot be used is reported on the setup panel. */
    private static final Pattern SH_NAME =
        Pattern.compile("silent.?hill|sl[ue]s.?0(0707|1514)|slpm.?86192|texupload|hd.?maps?",
                        Pattern.CASE_INSENSITIVE);

    enum Kind { BIN, CUE, ARC_DISC, ARC_TEXTURES, ARC_LOAD, ARC_NESTED }

    static final class Job {
        final Kind kind;
        final File src;
        Job(Kind kind, File src) { this.kind = kind; this.src = src; }
        @Override public String toString() { return kind + " " + src.getName(); }
    }

    static final class Plan {
        final List<Job> jobs = new ArrayList<>();
        /** Archives in Download that look like Silent Hill but cannot be used. */
        final List<String> problems = new ArrayList<>();
    }

    static final class Result {
        int failed;
        final List<String> problems = new ArrayList<>();
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
            return !plan(root).jobs.isEmpty() || !hasDisc(new File(root, "gamedata"));
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
            if (f.isFile() && lower(f.getName()).endsWith(".bin") && bootName(f) != null)
                return true;
        }
        return false;
    }

    /** Imports everything usable from Download. Archives that held other
     *  archives are unpacked first, then sorted in a following round. */
    static Result importAll(File root, Progress progress) {
        Result r = new Result();
        for (int round = 0; round < 4; round++) {
            Plan p = plan(root);
            Log.i(TAG, "setup round " + round + ": " + p.jobs + " problems " + p.problems);
            r.problems.clear();
            r.problems.addAll(p.problems);
            if (p.jobs.isEmpty()) break;
            r.failed += run(root, p.jobs, progress);
        }
        progress.step("Done", 100);
        return r;
    }

    // ---- planning (read-only: looks, never moves)

    static Plan plan(File root) {
        Plan plan = new Plan();
        File[] files = downloads().listFiles();
        if (files == null) return plan;

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
            if (n.endsWith(".tmp")) continue;
            if (n.endsWith(".bin")) {
                if (isSilentHill(f.getName(), bootName(f))) {
                    discBins.add(n);
                    if (!new File(gamedata, f.getName()).exists()) plan.jobs.add(new Job(Kind.BIN, f));
                }
            } else if (n.endsWith(".cue")) {
                cues.add(f);
            } else {
                classifyArchive(f, root, plan);
            }
        }
        for (File cue : cues) {
            String bin = cueBinName(cue);
            if (bin != null && discBins.contains(lower(bin))
                    && !new File(gamedata, cue.getName()).exists())
                plan.jobs.add(new Job(Kind.CUE, cue));
        }
        return plan;
    }

    private static void classifyArchive(File f, File root, Plan plan) {
        boolean shName = SH_NAME.matcher(f.getName()).find();
        Arc a;
        try {
            a = openArc(f);
        } catch (Unsupported e) {
            if (shName) plan.problems.add(f.getName() + ": " + e.getMessage());
            return;
        } catch (IOException e) {
            Log.w(TAG, "cannot read archive " + f, e);
            if (shName) plan.problems.add(f.getName() + ": cannot be read (damaged or password protected?)");
            return;
        }
        if (a == null) return;   // not an archive
        try (Arc arc = a) {
            boolean textures = false, load = false, nested = false;
            Ent disc = null;
            for (Ent e : arc.entries()) {
                if (e.dir) continue;
                String base = lower(e.base());
                if (base.startsWith("texupload-")) textures = true;
                if (loadFolderStart(e.path) >= 0) load = true;
                // Only unpack an archive of archives that is about Silent Hill:
                // the outer one is deleted afterwards.
                if (isArchiveName(base) && (shName || SH_NAME.matcher(e.base()).find())) nested = true;
                if (disc == null && base.endsWith(".bin")) {
                    String boot = null;
                    try (InputStream in = arc.open(e)) {
                        boot = bootName(in);
                    } catch (IOException ex) {
                        Log.w(TAG, "cannot read " + e.path + " in " + f, ex);
                    }
                    if (isSilentHill(e.base(), boot)) disc = e;
                }
            }
            File gamedata = new File(root, "gamedata");
            if (disc != null) {
                if (!new File(gamedata, disc.base()).exists()) plan.jobs.add(new Job(Kind.ARC_DISC, f));
            } else if (textures) {
                if (!textureDest(root, f, arc.isZip()).exists()) plan.jobs.add(new Job(Kind.ARC_TEXTURES, f));
            } else if (load) {
                plan.jobs.add(new Job(Kind.ARC_LOAD, f));
            } else if (nested) {
                plan.jobs.add(new Job(Kind.ARC_NESTED, f));
            } else if (shName) {
                plan.problems.add(f.getName() + ": no Silent Hill disc or HD pack inside");
            }
        } catch (IOException e) {
            Log.w(TAG, "cannot read archive " + f, e);
            if (shName) plan.problems.add(f.getName() + ": cannot be read");
        }
    }

    private static File textureDest(File root, File arc, boolean zip) {
        File dir = new File(root, "gamedata/texturemods");
        return zip ? new File(dir, arc.getName()) : new File(dir, stripExt(arc.getName()));
    }

    // ---- running

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
                    case ARC_DISC:
                        unpack(j.src, gamedata, "Unpacking " + j.src.getName(), sub, e -> {
                            String b = lower(e.base());
                            return b.endsWith(".bin") || b.endsWith(".cue") ? e.base() : null;
                        });
                        break;
                    case ARC_TEXTURES:
                        if (isZipFile(j.src)) {
                            sub.step("Moving texture pack " + j.src.getName(), 0);
                            move(j.src, textureDest(root, j.src, true));
                        } else {
                            File dst = textureDest(root, j.src, false);
                            unpack(j.src, dst, "Installing texture pack " + j.src.getName(), sub, e -> e.path);
                        }
                        break;
                    case ARC_LOAD:
                        unpack(j.src, new File(gamedata, "load"), "Installing HD pack " + j.src.getName(), sub,
                               e -> {
                                   int at = loadFolderStart(e.path);
                                   return at >= 0 ? e.path.substring(at) : null;
                               });
                        break;
                    case ARC_NESTED:
                        unpack(j.src, downloads(), "Unpacking " + j.src.getName(), sub,
                               e -> isArchiveName(lower(e.base())) && !new File(downloads(), e.base()).exists()
                                    ? e.base() : null);
                        break;
                }
                Log.i(TAG, "imported " + j);
            } catch (IOException e) {
                failed++;
                Log.e(TAG, "import failed: " + j, e);
            }
        }
        return failed;
    }

    /** Where an entry goes, relative to the destination dir; null = skip it. */
    private interface Pick { String to(Ent e); }

    /** Unpacks the picked entries of an archive into dstDir (in archive order:
     *  one pass, also fast for solid 7z/rar), then deletes the archive. */
    private static void unpack(File arcFile, File dstDir, String label, Progress p, Pick pick)
            throws IOException {
        dstDir.mkdirs();
        String dstPath = dstDir.getCanonicalPath() + File.separator;
        try (Arc arc = openArc(arcFile)) {
            if (arc == null) throw new IOException("not an archive: " + arcFile);
            long total = 0;
            for (Ent e : arc.entries()) if (!e.dir && pick.to(e) != null) total += Math.max(0, e.size);
            final long all = total;
            final long[] done = { 0 };
            p.step(label, 0);
            arc.each(e -> {
                if (e.dir) return null;
                String rel = pick.to(e);
                if (rel == null) return null;
                File dst = new File(dstDir, rel);
                if (!dst.getCanonicalPath().startsWith(dstPath))
                    throw new IOException("bad path in archive: " + e.path);
                if (dst.exists()) return null;
                dst.getParentFile().mkdirs();
                return new CountingFileOut(dst, n -> {
                    done[0] += n;
                    p.step(label, pct(done[0], all));
                });
            });
        }
        if (!arcFile.delete()) Log.w(TAG, "unpacked but could not delete " + arcFile);
    }

    private interface Counter { void add(long n); }

    /** Writes to dst.tmp, renames to dst on close; reports progress every 4 MB. */
    private static final class CountingFileOut extends OutputStream {
        private final File dst, tmp;
        private final FileOutputStream out;
        private final Counter counter;
        private long pending;

        CountingFileOut(File dst, Counter counter) throws IOException {
            this.dst = dst;
            this.tmp = new File(dst.getPath() + ".tmp");
            this.out = new FileOutputStream(tmp);
            this.counter = counter;
        }
        @Override public void write(int b) throws IOException { out.write(b); count(1); }
        @Override public void write(byte[] b, int off, int len) throws IOException { out.write(b, off, len); count(len); }
        private void count(long n) {
            pending += n;
            if (pending >= (4 << 20)) { counter.add(pending); pending = 0; }
        }
        @Override public void close() throws IOException {
            out.close();
            if (pending > 0) { counter.add(pending); pending = 0; }
            if (!tmp.renameTo(dst)) throw new IOException("cannot write " + dst);
        }
    }

    /** Rename (same storage: instant), else copy + delete. */
    private static void move(File src, File dst) throws IOException {
        if (dst.exists()) throw new IOException("already exists: " + dst);
        dst.getParentFile().mkdirs();
        if (src.renameTo(dst)) return;
        File tmp = new File(dst.getPath() + ".tmp");
        try (InputStream in = new FileInputStream(src); OutputStream out = new FileOutputStream(tmp)) {
            copy(in, out);
        }
        if (!tmp.renameTo(dst)) throw new IOException("cannot write " + dst);
        if (!src.delete()) Log.w(TAG, "copied but could not delete " + src);
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[1 << 16];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
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

    // ---- archives: one interface over .zip / .7z / .rar

    private static final class Ent {
        final String path;   // '/' separated, as stored
        final long size;
        final boolean dir;
        final int index;     // position in the archive
        Ent(String path, long size, boolean dir, int index) {
            this.path = path.replace('\\', '/');
            this.size = size;
            this.dir = dir;
            this.index = index;
        }
        String base() { return path.substring(path.lastIndexOf('/') + 1); }
    }

    /** For each entry in archive order: the stream to write it to, or null to skip. */
    private interface Sink { OutputStream open(Ent e) throws IOException; }

    private interface Arc extends Closeable {
        List<Ent> entries();
        /** One entry's data (random access: may decode what comes before it). */
        InputStream open(Ent e) throws IOException;
        /** Sequential pass over every entry. */
        void each(Sink sink) throws IOException;
        boolean isZip();
    }

    private static final class Unsupported extends IOException {
        Unsupported(String m) { super(m); }
    }

    private static boolean isArchiveName(String lowerName) {
        return lowerName.endsWith(".zip") || lowerName.endsWith(".7z") || lowerName.endsWith(".rar");
    }

    private static byte[] magic(File f) {
        byte[] b = new byte[8];
        try (InputStream in = new FileInputStream(f)) {
            int got = 0, n;
            while (got < b.length && (n = in.read(b, got, b.length - got)) > 0) got += n;
            return got == b.length ? b : null;
        } catch (IOException e) {
            return null;
        }
    }

    private static boolean isZipFile(File f) {
        byte[] m = magic(f);
        return m != null && m[0] == 'P' && m[1] == 'K' && m[2] == 3 && m[3] == 4;
    }

    /** Opens f by its signature (not its extension). null = not an archive. */
    private static Arc openArc(File f) throws IOException {
        byte[] m = magic(f);
        if (m == null) return null;
        if (m[0] == 'P' && m[1] == 'K' && m[2] == 3 && m[3] == 4) return new ZipArc(f);
        if ((m[0] & 0xFF) == 0x37 && (m[1] & 0xFF) == 0x7A && (m[2] & 0xFF) == 0xBC
                && (m[3] & 0xFF) == 0xAF && (m[4] & 0xFF) == 0x27 && (m[5] & 0xFF) == 0x1C)
            return new SevenZArc(f);
        if (m[0] == 'R' && m[1] == 'a' && m[2] == 'r' && m[3] == '!' && m[4] == 0x1A && m[5] == 0x07) {
            if (m[6] == 0x01)
                throw new Unsupported("RAR5 archives cannot be read here. "
                    + "Unpack it on a phone or PC, or download a .zip / .7z instead");
            return new RarArc(f);
        }
        return null;
    }

    private static final class ZipArc implements Arc {
        private final ZipFile z;
        private final List<ZipEntry> zes = new ArrayList<>();
        private final List<Ent> ents = new ArrayList<>();

        ZipArc(File f) throws IOException {
            z = new ZipFile(f);
            Enumeration<? extends ZipEntry> en = z.entries();
            while (en.hasMoreElements()) {
                ZipEntry e = en.nextElement();
                ents.add(new Ent(e.getName(), e.getSize(), e.isDirectory(), zes.size()));
                zes.add(e);
            }
        }
        @Override public List<Ent> entries() { return ents; }
        @Override public InputStream open(Ent e) throws IOException { return z.getInputStream(zes.get(e.index)); }
        @Override public void each(Sink sink) throws IOException {
            for (Ent e : ents) {
                OutputStream out = sink.open(e);
                if (out == null) continue;
                try (InputStream in = open(e); OutputStream o = out) { copy(in, o); }
            }
        }
        @Override public boolean isZip() { return true; }
        @Override public void close() throws IOException { z.close(); }
    }

    private static final class SevenZArc implements Arc {
        private final File file;
        private SevenZFile sz;
        private final List<SevenZArchiveEntry> zes = new ArrayList<>();
        private final List<Ent> ents = new ArrayList<>();

        SevenZArc(File f) throws IOException {
            file = f;
            sz = SevenZFile.builder().setFile(f).get();
            for (SevenZArchiveEntry e : sz.getEntries()) {
                ents.add(new Ent(e.getName(), e.getSize(), e.isDirectory(), zes.size()));
                zes.add(e);
            }
        }
        @Override public List<Ent> entries() { return ents; }
        @Override public InputStream open(Ent e) throws IOException { return sz.getInputStream(zes.get(e.index)); }
        @Override public void each(Sink sink) throws IOException {
            // Fresh reader, walked with getNextEntry: decodes each solid block once.
            try (SevenZFile seq = SevenZFile.builder().setFile(file).get()) {
                SevenZArchiveEntry ze;
                int i = 0;
                byte[] buf = new byte[1 << 16];
                while ((ze = seq.getNextEntry()) != null) {
                    Ent e = ents.get(i++);
                    if (ze.isDirectory() || !ze.hasStream()) continue;
                    OutputStream out = sink.open(e);
                    if (out == null) continue;   // not read: getNextEntry skips it
                    try (OutputStream o = out) {
                        int n;
                        while ((n = seq.read(buf)) > 0) o.write(buf, 0, n);
                    }
                }
            }
        }
        @Override public boolean isZip() { return false; }
        @Override public void close() throws IOException { sz.close(); }
    }

    private static final class RarArc implements Arc {
        private final File file;
        private final Archive rar;
        private final List<FileHeader> hs;
        private final List<Ent> ents = new ArrayList<>();

        RarArc(File f) throws IOException {
            file = f;
            try {
                rar = new Archive(f);
            } catch (RarException e) {
                throw new IOException("RAR: " + e.getMessage(), e);
            }
            boolean encrypted;
            try {
                encrypted = rar.isEncrypted();
            } catch (RarException e) {
                encrypted = true;
            }
            if (encrypted) {
                closeQuietly(rar);
                throw new Unsupported("password protected");
            }
            hs = rar.getFileHeaders();
            for (int i = 0; i < hs.size(); i++) {
                FileHeader h = hs.get(i);
                ents.add(new Ent(h.getFileName(), h.getFullUnpackSize(), h.isDirectory(), i));
            }
        }
        @Override public List<Ent> entries() { return ents; }

        @Override public InputStream open(Ent e) throws IOException {
            if (!rar.getMainHeader().isSolid()) return rarStream(rar, hs.get(e.index));
            // Solid: every file before it has to be decoded first, on a fresh reader.
            final Archive a;
            try {
                a = new Archive(file);
            } catch (RarException ex) {
                throw new IOException("RAR: " + ex.getMessage(), ex);
            }
            List<FileHeader> fresh = a.getFileHeaders();
            OutputStream nul = new OutputStream() { @Override public void write(int b) {} @Override public void write(byte[] b, int o, int l) {} };
            try {
                for (int i = 0; i < e.index; i++)
                    if (!fresh.get(i).isDirectory()) a.extractFile(fresh.get(i), nul);
            } catch (RarException ex) {
                closeQuietly(a);
                throw new IOException("RAR: " + ex.getMessage(), ex);
            }
            return new FilterInputStream(rarStream(a, fresh.get(e.index))) {
                @Override public void close() throws IOException { super.close(); closeQuietly(a); }
            };
        }

        @Override public void each(Sink sink) throws IOException {
            boolean solid = rar.getMainHeader().isSolid();
            OutputStream nul = new OutputStream() { @Override public void write(int b) {} @Override public void write(byte[] b, int o, int l) {} };
            try {
                for (Ent e : ents) {
                    FileHeader h = hs.get(e.index);
                    if (h.isDirectory()) continue;
                    OutputStream out = sink.open(e);
                    if (out == null) {
                        if (solid) rar.extractFile(h, nul);   // keeps the solid stream in step
                        continue;
                    }
                    try (OutputStream o = out) { rar.extractFile(h, o); }
                }
            } catch (RarException ex) {
                throw new IOException("RAR: " + ex.getMessage(), ex);
            }
        }
        @Override public boolean isZip() { return false; }
        @Override public void close() throws IOException { rar.close(); }

        private static InputStream rarStream(Archive a, FileHeader h) throws IOException {
            return a.getInputStream(h);
        }
    }

    private static void closeQuietly(Closeable c) {
        try { c.close(); } catch (IOException ignored) {}
    }

    // ---- disc identification (port of Pc_DetectRegionFromBin, main_pc.c)

    private static boolean isSilentHill(String fileName, String boot) {
        if (boot == null) return false;
        for (String s : SH_SERIALS) if (boot.startsWith(s)) return true;
        return lower(fileName).contains("silent hill");
    }

    private static String bootName(File f) {
        try (InputStream in = new FileInputStream(f)) {
            return bootName(in);
        } catch (IOException e) {
            return null;
        }
    }

    /** Name of the SLUS/SLES/SLPM/SLPS/SIPS boot file in the ISO9660 root of a
     *  raw (2352-byte sector) PSX image, or null when it is not one. Reads the
     *  stream forward only (the root directory always comes after the PVD), so
     *  it also works on a stream out of an archive. */
    private static String bootName(InputStream in) throws IOException {
        byte[] sec = new byte[2048];
        long pos = 16L * 2352 + 24;
        if (!skipFully(in, pos) || !readFully(in, sec)) return null;
        pos += 2048;
        long rlba = (sec[158] & 0xFF) | (sec[159] & 0xFF) << 8 | (sec[160] & 0xFF) << 16
                  | (long) (sec[161] & 0xFF) << 24;
        long at = rlba * 2352 + 24;
        if (at < pos || !skipFully(in, at - pos) || !readFully(in, sec)) return null;
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
        return null;
    }

    private static boolean skipFully(InputStream in, long n) throws IOException {
        byte[] junk = null;
        while (n > 0) {
            long s = in.skip(n);
            if (s <= 0) {
                if (junk == null) junk = new byte[1 << 16];
                int r = in.read(junk, 0, (int) Math.min(junk.length, n));
                if (r < 0) return false;
                s = r;
            }
            n -= s;
        }
        return true;
    }

    private static boolean readFully(InputStream in, byte[] b) throws IOException {
        int got = 0, n;
        while (got < b.length && (n = in.read(b, got, b.length - got)) > 0) got += n;
        return got == b.length;
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

    private static String stripExt(String name) {
        int dot = name.lastIndexOf('.');
        return dot > 0 ? name.substring(0, dot) : name;
    }

    private static String lower(String s) { return s.toLowerCase(Locale.ROOT); }
}
