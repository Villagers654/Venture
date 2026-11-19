// SPDX-License-Identifier: GPL-3.0-or-later
package org.venture_emu.game;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import android.os.SystemClock;

/**
 * The game's side of the Venture app's Platform broker (PlatformService):
 * one Binder call per request from the Platform loader, which calls call()
 * from its own threads. The binding starts at application start and comes
 * back by itself if the Venture app's process restarts.
 */
final class PlatformBroker {
    private PlatformBroker() {}

    private static final String VENTURE = "org.venture_emu.venture";
    private static final String SERVICE = "org.venture_emu.venture.PlatformService";
    private static final String DESCRIPTOR = "org.venture_emu.platform.Broker";
    private static final int CALL = IBinder.FIRST_CALL_TRANSACTION;
    private static final long WAIT_MS = 10000;
    // ErrorCode::unavailable (hle/ovrplatform/provider.h).
    private static final String UNAVAILABLE = "{\"error\":-20003}";

    private static final Object lock = new Object();
    private static Context context;
    private static IBinder binder;
    private static boolean binding;

    private static final ServiceConnection connection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder service) {
            synchronized (lock) {
                binder = service;
                lock.notifyAll();
            }
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            synchronized (lock) {
                binder = null;
            }
        }

        @Override
        public void onBindingDied(ComponentName name) {
            synchronized (lock) {
                binder = null;
                binding = false;
                try {
                    context.unbindService(this);
                } catch (IllegalArgumentException ignored) {
                }
                lock.notifyAll();
            }
        }

        @Override
        public void onNullBinding(ComponentName name) {
            synchronized (lock) {
                binding = false;
                lock.notifyAll();
            }
        }
    };

    static void start(Context c) {
        synchronized (lock) {
            context = c.getApplicationContext();
            bind();
        }
    }

    // Requires lock.
    private static void bind() {
        if (binding || context == null) return;
        Intent intent = new Intent();
        intent.setClassName(VENTURE, SERVICE);
        try {
            binding = context.bindService(intent, connection, Context.BIND_AUTO_CREATE);
        } catch (SecurityException e) {
            binding = false;
        }
    }

    /** Called by the Platform loader: a request's JSON, the broker's JSON reply. */
    static String call(String request) {
        IBinder service;
        synchronized (lock) {
            if (binder == null) bind();
            long deadline = SystemClock.uptimeMillis() + WAIT_MS;
            while (binder == null && binding) {
                long left = deadline - SystemClock.uptimeMillis();
                if (left <= 0) break;
                try {
                    lock.wait(left);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
            service = binder;
        }
        if (service == null) return UNAVAILABLE;
        Parcel data = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(DESCRIPTOR);
            data.writeString(request);
            if (!service.transact(CALL, data, reply, 0)) return UNAVAILABLE;
            reply.readException();
            String answer = reply.readString();
            return answer == null ? UNAVAILABLE : answer;
        } catch (RemoteException | RuntimeException e) {
            return UNAVAILABLE;
        } finally {
            data.recycle();
            reply.recycle();
        }
    }
}
