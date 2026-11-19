// SPDX-License-Identifier: GPL-3.0-or-later
package org.venture_emu.venture;

import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.pm.SigningInfo;

import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;

import java.math.BigInteger;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.util.Calendar;

import javax.security.auth.x500.X500Principal;
import java.security.cert.Certificate;
import java.util.Arrays;

/**
 * The user's game key: the signing key Venture signs the games it adapts
 * with, kept by the Android Keystore under ALIAS. Its certificate identifies
 * the games Venture installed (the Platform broker serves only those).
 */
final class GameKey {
    private GameKey() {}

    static final String ALIAS = "venture-games";

    /**
     * The key, created on first use: RSA 2048 in the Android Keystore with a
     * self-signed certificate. It never leaves the Keystore; apksig signs through
     * it. Every game Venture installs carries the same key, so updates install
     * over the game and keep its data.
     */
    static synchronized KeyStore.PrivateKeyEntry ensure() throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        KeyStore.Entry entry = store.getEntry(ALIAS, null);
        if (entry instanceof KeyStore.PrivateKeyEntry) return (KeyStore.PrivateKeyEntry) entry;
        KeyPairGenerator generator = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_RSA, "AndroidKeyStore");
        Calendar start = Calendar.getInstance();
        start.add(Calendar.DAY_OF_YEAR, -1);
        Calendar end = Calendar.getInstance();
        end.add(Calendar.YEAR, 100);
        generator.initialize(new KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_SIGN)
                .setKeySize(2048)
                .setDigests(KeyProperties.DIGEST_SHA256, KeyProperties.DIGEST_SHA512)
                .setSignaturePaddings(KeyProperties.SIGNATURE_PADDING_RSA_PKCS1)
                .setCertificateSubject(new X500Principal("CN=Venture games"))
                .setCertificateSerialNumber(BigInteger.ONE)
                .setCertificateNotBefore(start.getTime())
                .setCertificateNotAfter(end.getTime())
                .build());
        generator.generateKeyPair();
        return (KeyStore.PrivateKeyEntry) store.getEntry(ALIAS, null);
    }

    /** The key's certificate, DER-encoded, or null before the first game was signed. */
    static byte[] certificate() {
        try {
            KeyStore store = KeyStore.getInstance("AndroidKeyStore");
            store.load(null);
            Certificate certificate = store.getCertificate(ALIAS);
            return certificate == null ? null : certificate.getEncoded();
        } catch (Exception e) {
            return null;
        }
    }

    /** Whether the installed package is signed with the user's game key. */
    static boolean signs(PackageManager packages, String packageName) {
        byte[] ours = certificate();
        if (ours == null) return false;
        try {
            PackageInfo info = packages.getPackageInfo(packageName, PackageManager.GET_SIGNING_CERTIFICATES);
            SigningInfo signing = info.signingInfo;
            if (signing == null) return false;
            Signature[] signers = signing.hasMultipleSigners() ? signing.getApkContentsSigners()
                                                               : signing.getSigningCertificateHistory();
            if (signers == null || signers.length == 0) return false;
            // The current signer: the last of a rotation history, or every signer of a multi-signer APK.
            if (signing.hasMultipleSigners()) {
                for (Signature signer : signers)
                    if (!Arrays.equals(signer.toByteArray(), ours)) return false;
                return true;
            }
            return Arrays.equals(signers[signers.length - 1].toByteArray(), ours);
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
    }
}
