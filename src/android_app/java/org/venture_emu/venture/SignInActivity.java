// SPDX-License-Identifier: GPL-3.0-or-later
package org.venture_emu.venture;

import android.app.Activity;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.webkit.CookieManager;
import android.webkit.PermissionRequest;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

/**
 * The Meta sign-in window. Pages stay on Meta's domains and embedded frames
 * (security checks) on plain HTTPS, by the same rules the desktop windows use
 * (meta::auth_navigation_allowed, meta::auth_frame_allowed in the core). The
 * oculus:// address Meta ends with is handed to the launcher, never loaded.
 * Each sign-in starts without cookies, so one account never signs in as another.
 */
public class SignInActivity extends Activity {
    static final String EXTRA_URL = "url";

    private WebView view;
    private boolean callbackSent, loadFailed;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        SignIn.current = this;
        String url = getIntent().getStringExtra(EXTRA_URL);
        if (url == null || !Native.navigationAllowed(url)) {
            Native.signInEvent(SignIn.ERROR, "Could not open the Meta sign-in window.");
            finish();
            return;
        }
        CookieManager cookies = CookieManager.getInstance();
        cookies.removeAllCookies(null);
        cookies.flush();
        view = new WebView(this);
        WebSettings settings = view.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        settings.setAllowFileAccess(false);
        settings.setAllowContentAccess(false);
        settings.setSupportMultipleWindows(false);
        settings.setGeolocationEnabled(false);
        view.setWebChromeClient(new WebChromeClient() {
            @Override
            public void onPermissionRequest(PermissionRequest request) {
                request.deny();
            }
        });
        view.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView v, WebResourceRequest request) {
                String uri = request.getUrl().toString();
                if (Native.isCallback(uri)) {
                    if (!callbackSent) {
                        callbackSent = true;
                        Native.signInEvent(SignIn.CALLBACK, uri);
                    }
                    finish();
                    return true;
                }
                boolean allowed = request.isForMainFrame() ? Native.navigationAllowed(uri) : Native.frameAllowed(uri);
                return !allowed;
            }

            @Override
            public void onPageStarted(WebView v, String uri, Bitmap favicon) {
                loadFailed = false;
                if (uri != null && !uri.equals("about:blank") && !Native.navigationAllowed(uri)) v.stopLoading();
            }

            @Override
            public void onReceivedError(WebView v, WebResourceRequest request, WebResourceError error) {
                if (request.isForMainFrame()) loadFailed = true;
            }

            @Override
            public void onPageFinished(WebView v, String uri) {
                if (!loadFailed) Native.signInEvent(SignIn.READY, "");
            }
        });
        setContentView(view);
        view.loadUrl(url);
    }

    @Override
    protected void onDestroy() {
        if (SignIn.current == this) SignIn.current = null;
        if (view != null) {
            view.stopLoading();
            view.destroy();
            view = null;
        }
        Native.signInEvent(SignIn.CLOSED, "");
        super.onDestroy();
    }
}
