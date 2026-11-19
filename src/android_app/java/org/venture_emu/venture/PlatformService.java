// SPDX-License-Identifier: GPL-3.0-or-later
package org.venture_emu.venture;

import android.app.Service;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;

/**
 * The Meta Platform broker for the games Venture installed
 * (docs/design/native-headsets.md, 5.3). A game's Platform loader binds this
 * service (PlatformBroker in the game's dex) and sends the requests the PC
 * runtime sends over its pipe; the core answers them with the game's account
 * (CredentialBroker), so the account's Meta session never leaves Venture.
 * Only callers whose package Venture installed and signed with the user's
 * game key are served.
 */
public final class PlatformService extends Service {
    static final String DESCRIPTOR = "org.venture_emu.platform.Broker";
    static final int CALL = IBinder.FIRST_CALL_TRANSACTION;
    // ErrorCode::identity_mismatch (hle/ovrplatform/provider.h).
    private static final String REFUSED = "{\"error\":-20005}";

    private final Binder binder = new Binder() {
        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code != CALL) return super.onTransact(code, data, reply, flags);
            data.enforceInterface(DESCRIPTOR);
            String request = data.readString();
            String answer = handle(Binder.getCallingUid(), request);
            reply.writeNoException();
            reply.writeString(answer);
            return true;
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        Native.ensure(this);
    }

    @Override
    public IBinder onBind(Intent intent) {
        return binder;
    }

    private String handle(int uid, String request) {
        if (request == null) return REFUSED;
        PackageManager packages = getPackageManager();
        String[] names = packages.getPackagesForUid(uid);
        if (names == null) return REFUSED;
        long identity = Binder.clearCallingIdentity();
        try {
            for (String name : names) {
                if (!GameKey.signs(packages, name)) continue;
                String answer = Native.platformRequest(name, request);
                if (answer != null) return answer;
            }
        } finally {
            Binder.restoreCallingIdentity(identity);
        }
        return REFUSED;
    }
}
