package com.rea.nfsug2;

import android.content.Context;

import java.io.File;

/**
 * Owns the on-device layout of game and user data.
 *
 * <pre>
 *   /sdcard/Android/data/com.rea.nfsug2/files/game/   the user's NFSU2 install (SPEED2.EXE, GLOBAL, TRACKS, ...)
 *   /sdcard/Android/data/com.rea.nfsug2/files/user/   saves, settings registry, logs
 * </pre>
 *
 * The game folder is app-specific external storage, so it needs no storage
 * permission and can be filled with {@code adb push}.
 */
public final class GamePaths {
    public final File root;
    public final File game;
    public final File user;

    public GamePaths(Context ctx) {
        File ext = ctx.getExternalFilesDir(null);
        root = ext != null ? ext : ctx.getFilesDir();
        game = new File(root, "game");
        user = new File(root, "user");
    }

    public void ensureDirs() {
        //noinspection ResultOfMethodCallIgnored
        game.mkdirs();
        //noinspection ResultOfMethodCallIgnored
        user.mkdirs();
    }

    /** Finds SPEED2.EXE regardless of the case the files were copied with. */
    public File findExecutable() {
        File[] files = game.listFiles();
        if (files == null) return null;
        for (File f : files) {
            if (f.isFile() && f.getName().equalsIgnoreCase("SPEED2.EXE")) return f;
        }
        return null;
    }
}
