// SPDX-License-Identifier: GPL-3.0-or-later
package org.venture_emu.game;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;

import java.io.File;

/**
 * Part of every game Venture installs (merged into the game's dex; the
 * adapted manifest declares it, not exported, initialized before any of the
 * game's own code). Loads Venture's libovrplatformloader.so at application
 * start, hands it the context and starts binding the Venture app's Platform
 * broker, so the game's own load of the library finds it ready.
 */
public final class PlatformInit extends ContentProvider {
    @Override
    public boolean onCreate() {
        Context context = getContext();
        if (context == null) return false;
        try {
            System.loadLibrary("ovrplatformloader");
        } catch (UnsatisfiedLinkError e) {
            return false;  // a game without the Platform SDK keeps running
        }
        File external = context.getExternalFilesDir(null);
        File obb = context.getObbDir();
        nativeAttach(context.getApplicationContext(), context.getFilesDir().getAbsolutePath(),
                context.getCacheDir().getAbsolutePath(), external == null ? null : external.getAbsolutePath(),
                obb == null ? null : obb.getAbsolutePath(), Build.VERSION.SDK_INT, PlatformBroker.class);
        PlatformBroker.start(context);
        return true;
    }

    private static native void nativeAttach(Context context, String filesDir, String cacheDir, String externalFilesDir,
                                            String obbDir, int sdk, Class<?> broker);

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        return null;
    }

    @Override
    public String getType(Uri uri) {
        return null;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        return 0;
    }
}
