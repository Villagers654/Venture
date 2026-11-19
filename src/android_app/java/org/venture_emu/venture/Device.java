// SPDX-License-Identifier: GPL-3.0-or-later
package org.venture_emu.venture;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;

import org.libsdl.app.SDLActivity;

/** The device's own facilities the C++ core uses (common/platform/android/device.cpp). */
final class Device {
    private Device() {}

    static Context context() {
        Context activity = SDLActivity.getContext();
        return activity != null ? activity : Native.appContext;
    }

    static void start(Intent intent) {
        Context context = context();
        if (!(context instanceof Activity)) intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        context.startActivity(intent);
    }

    /** Opens a link in the device's browser; false when nothing can show it. */
    static boolean openUrl(String url) {
        try {
            start(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
            return true;
        } catch (Exception e) {
            return false;
        }
    }
}
