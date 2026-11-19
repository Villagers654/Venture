// SPDX-License-Identifier: GPL-3.0-or-later
package org.venture_emu.venture;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.pm.PackageManager;
import android.util.SparseArray;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Android's runtime permissions, asked at the moment Venture needs one: the
 * system's own dialog, after Venture's reason when Android says to show one
 * (shouldShowRequestPermissionRationale). Called from the core's worker
 * threads; each call blocks until the user answered.
 */
final class Permissions {
    private Permissions() {}

    // Request codes of their own, above the small counters SDL uses for its
    // permission and file dialog requests.
    private static final AtomicInteger next = new AtomicInteger(0x5650);
    private static final SparseArray<Pending> pending = new SparseArray<>();
    private static final long ANSWER_MINUTES = 10;

    private static final class Pending {
        final CountDownLatch done = new CountDownLatch(1);
        boolean granted;
    }

    static boolean granted(String permission) {
        Context context = Device.context();
        return context != null && context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
    }

    /**
     * Whether `permission` is granted, asking for it first when it is not. The
     * reason (a string resource) is shown before the system dialog when
     * Android says the user needs one.
     */
    static boolean request(final String permission, final int reason) {
        if (granted(permission)) return true;
        Context context = Device.context();
        if (!(context instanceof Activity)) return false;
        final Activity activity = (Activity) context;
        final int code = next.incrementAndGet();
        final Pending wait = new Pending();
        synchronized (pending) {
            pending.put(code, wait);
        }
        // Anonymous classes: the APK's Java is compiled against android.jar,
        // which has no LambdaMetafactory.
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (!activity.shouldShowRequestPermissionRationale(permission)) {
                    activity.requestPermissions(new String[] {permission}, code);
                    return;
                }
                new AlertDialog.Builder(activity)
                        .setMessage(reason)
                        .setPositiveButton(android.R.string.ok, new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface dialog, int which) {
                                activity.requestPermissions(new String[] {permission}, code);
                            }
                        })
                        .setNegativeButton(android.R.string.cancel, new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface dialog, int which) {
                                deliver(code, false);
                            }
                        })
                        .setOnCancelListener(new DialogInterface.OnCancelListener() {
                            @Override
                            public void onCancel(DialogInterface dialog) {
                                deliver(code, false);
                            }
                        })
                        .show();
            }
        });
        try {
            wait.done.await(ANSWER_MINUTES, TimeUnit.MINUTES);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } finally {
            synchronized (pending) {
                pending.remove(code);
            }
        }
        return wait.granted || granted(permission);
    }

    private static void deliver(int code, boolean granted) {
        Pending wait;
        synchronized (pending) {
            wait = pending.get(code);
        }
        if (wait == null) return;
        wait.granted = granted;
        wait.done.countDown();
    }

    /** The activity's permission result; false when the request was not one of these. */
    static boolean onResult(int code, int[] results) {
        synchronized (pending) {
            if (pending.get(code) == null) return false;
        }
        deliver(code, results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED);
        return true;
    }
}
