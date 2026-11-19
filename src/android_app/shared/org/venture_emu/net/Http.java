// SPDX-License-Identifier: GPL-3.0-or-later
package org.venture_emu.net;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

import javax.net.ssl.SSLException;

/**
 * HTTPS through the device's own network stack (system TLS, certificate store
 * and proxy) for the C++ core (common/platform/android/http_transfer.cpp), in
 * the Venture app and in the games it installs (their Platform loader).
 * Redirects are not followed: the core checks every hop against its domain
 * rules, as on desktop.
 */
final class Http {
    private Http() {}

    // Failure kinds the core maps to its messages (http::check).
    static final int OK = 0, TIMEOUT = 1, TLS = 2, NETWORK = 3;

    /** One transfer. The core reads status, headers and body through it. */
    static final class Transfer {
        HttpURLConnection connection;
        InputStream body;
        int status;
        String[] headers = new String[0];
        int failure = OK;
        String message = "";
    }

    static Transfer open(String url, String method, String[] headers, byte[] body, int connectMs, int readMs) {
        Transfer t = new Transfer();
        try {
            URL u = new URL(url);
            String scheme = u.getProtocol();
            boolean loopback = "http".equals(scheme) && "127.0.0.1".equals(u.getHost());
            if (!"https".equals(scheme) && !loopback) throw new IOException("Only HTTPS is allowed.");
            HttpURLConnection c = (HttpURLConnection) (loopback ? u.openConnection(java.net.Proxy.NO_PROXY)
                                                                : u.openConnection());
            t.connection = c;
            c.setInstanceFollowRedirects(false);
            c.setUseCaches(false);
            c.setConnectTimeout(connectMs);
            c.setReadTimeout(readMs);
            c.setRequestMethod(method);
            c.setRequestProperty("Accept-Encoding", "identity");
            for (int i = 0; i + 1 < headers.length; i += 2) c.setRequestProperty(headers[i], headers[i + 1]);
            if (body != null) {
                c.setDoOutput(true);
                c.setFixedLengthStreamingMode(body.length);
                OutputStream out = c.getOutputStream();
                out.write(body);
                out.close();
            }
            t.status = c.getResponseCode();
            List<String> flat = new ArrayList<>();
            for (Map.Entry<String, List<String>> e : c.getHeaderFields().entrySet()) {
                if (e.getKey() == null) continue;  // the status line
                List<String> values = e.getValue();
                // The last value wins, as the core's header parser keeps it.
                flat.add(e.getKey());
                flat.add(values.isEmpty() ? "" : values.get(values.size() - 1));
            }
            t.headers = flat.toArray(new String[0]);
            InputStream in;
            try {
                in = c.getInputStream();
            } catch (IOException error) {
                in = c.getErrorStream();
            }
            t.body = in;
        } catch (SocketTimeoutException e) {
            fail(t, TIMEOUT, e);
        } catch (SSLException e) {
            fail(t, TLS, e);
        } catch (Exception e) {
            fail(t, NETWORK, e);
        }
        return t;
    }

    /** Bytes read into buffer, -1 at the end, -2 on failure (kind in t.failure). */
    static int read(Transfer t, byte[] buffer) {
        if (t.body == null) return -1;
        try {
            return t.body.read(buffer);
        } catch (SocketTimeoutException e) {
            fail(t, TIMEOUT, e);
        } catch (SSLException e) {
            fail(t, TLS, e);
        } catch (Exception e) {
            fail(t, NETWORK, e);
        }
        return -2;
    }

    static void close(Transfer t) {
        try {
            if (t.body != null) t.body.close();
        } catch (IOException ignored) {
        }
        if (t.connection != null) t.connection.disconnect();
    }

    private static void fail(Transfer t, int kind, Exception e) {
        t.failure = kind;
        String text = e.getClass().getSimpleName();
        if (e.getMessage() != null) text += ": " + e.getMessage();
        t.message = text;
    }
}
