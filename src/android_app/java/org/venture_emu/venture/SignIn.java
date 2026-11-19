// SPDX-License-Identifier: GPL-3.0-or-later
package org.venture_emu.venture;

import android.content.Intent;

/**
 * Meta sign-in for the C++ launcher (launcher/platform/android/auth_browser.cpp):
 * a WebView activity kept on Meta's pages that hands back Meta's oculus://
 * address, as the desktop sign-in windows do. The manual sign-in link opens in
 * the device's browser instead; Venture receives the same oculus:// address
 * there through its intent filter.
 */
final class SignIn {
    private SignIn() {}

    // AuthBrowser::Kind
    static final int READY = 0, CALLBACK = 1, CLOSED = 2, ERROR = 3;

    static volatile SignInActivity current;

    /** Opens the sign-in window at Meta's sign-in page; false when it cannot open. */
    static boolean open(String url) {
        try {
            Intent intent = new Intent(Device.context(), SignInActivity.class);
            intent.putExtra(SignInActivity.EXTRA_URL, url);
            Device.start(intent);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    static void close() {
        final SignInActivity activity = current;
        // No lambdas: javac compiles against android.jar, which has no LambdaMetafactory.
        if (activity != null)
            activity.runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    activity.finish();
                }
            });
    }

    /** An oculus:// address the browser handed to Venture. */
    static void deliverIntent(Intent intent) {
        if (intent == null || !Intent.ACTION_VIEW.equals(intent.getAction()) || intent.getData() == null) return;
        String url = intent.getDataString();
        if (url != null && Native.isCallback(url)) Native.signInLink(url);
    }
}
