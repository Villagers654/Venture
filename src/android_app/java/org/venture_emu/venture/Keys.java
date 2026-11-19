// SPDX-License-Identifier: GPL-3.0-or-later
package org.venture_emu.venture;

import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;

import java.nio.ByteBuffer;
import java.security.KeyStore;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/**
 * Seals Meta sessions with an AES-256-GCM key that the Android Keystore holds
 * and never releases (common/platform/android/secret_store.cpp). The sealed
 * form is a version byte, the 12-byte IV, then the ciphertext and tag.
 */
final class Keys {
    private Keys() {}

    private static final String STORE = "AndroidKeyStore";
    private static final String ALIAS = "venture-sessions";
    private static final byte VERSION = 1;

    private static synchronized SecretKey key() throws Exception {
        KeyStore store = KeyStore.getInstance(STORE);
        store.load(null);
        KeyStore.Entry entry = store.getEntry(ALIAS, null);
        if (entry instanceof KeyStore.SecretKeyEntry) return ((KeyStore.SecretKeyEntry) entry).getSecretKey();
        KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, STORE);
        generator.init(new KeyGenParameterSpec.Builder(ALIAS,
                KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build());
        return generator.generateKey();
    }

    /** The sealed bytes, or null when the Keystore refused. */
    static byte[] seal(byte[] plain, byte[] context) {
        try {
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.ENCRYPT_MODE, key());
            cipher.updateAAD(context);
            byte[] iv = cipher.getIV();
            byte[] sealed = cipher.doFinal(plain);
            return ByteBuffer.allocate(1 + iv.length + sealed.length).put(VERSION).put(iv).put(sealed).array();
        } catch (Exception e) {
            return null;
        }
    }

    /** The plain bytes, or null when the data is not sealed by this key and context. */
    static byte[] unseal(byte[] sealed, byte[] context) {
        try {
            if (sealed.length < 1 + 12 + 16 || sealed[0] != VERSION) return null;
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.DECRYPT_MODE, key(), new GCMParameterSpec(128, sealed, 1, 12));
            cipher.updateAAD(context);
            return cipher.doFinal(sealed, 13, sealed.length - 13);
        } catch (Exception e) {
            return null;
        }
    }
}
