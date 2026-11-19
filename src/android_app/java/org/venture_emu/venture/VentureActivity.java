// SPDX-License-Identifier: GPL-3.0-or-later
package org.venture_emu.venture;

import android.content.Intent;
import android.os.Bundle;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

import org.libsdl.app.SDLActivity;

/**
 * Venture on an Android headset: the launcher's ImGui interface on SDL3's
 * Android backend, shown by the headset as a 2D panel. Everything above the
 * window is the shared C++ launcher in libventure.so.
 */
public class VentureActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        // SDL3 is linked into libventure.so.
        return new String[] {"venture"};
    }

    @Override
    protected String getMainSharedObject() {
        // Native libraries stay in the APK (extractNativeLibs="false"): the
        // already-loaded library is found by its name.
        return "libventure.so";
    }

    /** adb's shell user (AID_SHELL). */
    private static final int SHELL_UID = 2000;

    /**
     * The launcher's command line (its smoke options, as on the desktop) comes
     * from /data/local/tmp/venture-args, one argument per line, when adb's
     * shell user owns it. Only that user (and root) can write there, so
     * development tools (tools/android/headset_play_test.py) can drive the
     * launcher this way. Without the file, the launcher has no arguments.
     */
    @Override
    protected String[] getArguments() {
        File file = new File("/data/local/tmp/venture-args");
        try {
            if (!file.isFile() || android.system.Os.stat(file.getPath()).st_uid != SHELL_UID) return new String[0];
            List<String> out = new ArrayList<>();
            for (String line : Files.readAllLines(file.toPath(), StandardCharsets.UTF_8))
                if (!line.isEmpty()) out.add(line);
            return out.toArray(new String[0]);
        } catch (Exception e) {
            return new String[0];
        }
    }

    @Override
    public void loadLibraries() {
        super.loadLibraries();
        Native.init(this);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        SignIn.deliverIntent(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        SignIn.deliverIntent(intent);
    }
}
