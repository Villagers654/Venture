// SPDX-License-Identifier: GPL-3.0-or-later
package org.venture_emu.venture;

import android.content.Context;
import android.os.Build;
import android.os.LocaleList;

import java.io.File;

/**
 * The calls between the app's Java side and the shared C++ core. The core
 * keeps global references to the helper classes it calls (Http, Keys, SignIn,
 * Device), resolved here on a thread that has the app's class loader.
 */
public final class Native {
    private Native() {}

    static Context appContext;
    private static boolean loaded;

    /**
     * Loads the core and initializes it when the process did not start with
     * the launcher activity (a game binding PlatformService).
     */
    static synchronized void ensure(Context context) {
        if (appContext != null) return;
        if (!loaded) {
            System.loadLibrary("venture");
            loaded = true;
        }
        init(context);
    }

    static synchronized void init(Context context) {
        appContext = context.getApplicationContext();
        File data = new File(appContext.getFilesDir(), "venture");
        data.mkdirs();
        // The interface languages in the user's order ("fr-FR,en-US"), for "auto".
        String locales = LocaleList.getDefault().toLanguageTags();
        nativeInit(appContext, data.getAbsolutePath(), appContext.getCacheDir().getAbsolutePath(),
                appContext.getApplicationInfo().sourceDir, Build.VERSION.SDK_INT, locales);
    }

    /** Sets the data folder and caches the helper classes; once per process. */
    private static native void nativeInit(Context context, String dataDir, String cacheDir, String apk, int sdk,
                                          String locales);

    // Meta sign-in (SignInActivity and the oculus:// callback).
    static native void signInEvent(int kind, String value);
    static native boolean navigationAllowed(String url);
    static native boolean frameAllowed(String url);
    static native boolean isCallback(String url);
    /** An oculus:// address from the device's browser, for a waiting manual sign-in. */
    static native void signInLink(String url);

    /**
     * One Platform broker request from a game Venture installed (PlatformService):
     * the JSON reply, or null when Venture did not install that package.
     */
    static native String platformRequest(String packageName, String request);
}
