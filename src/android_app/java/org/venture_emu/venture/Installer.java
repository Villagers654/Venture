// SPDX-License-Identifier: GPL-3.0-or-later
package org.venture_emu.venture;

import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;

import com.android.apksig.ApkSigner;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.cert.X509Certificate;
import java.util.Collections;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Installs the games Venture adapts for this headset (docs/design/native-headsets.md,
 * 5.2), and Venture's own updates: signing with the user's game key (apksig), a
 * PackageInstaller session the user confirms, the expansion files under
 * Android/obb, and starting an installed game. Called by the C++ core on its
 * worker threads (common/platform/android/installer.cpp); every call blocks
 * until it is done and returns null on success or a sentence for the user.
 */
final class Installer {
    private Installer() {}

    private static final AtomicInteger sessions = new AtomicInteger();
    // How long an install waits for the user to confirm it.
    private static final long CONFIRM_MINUTES = 30;

    /** Signs `unsigned` into `signed` with the game key, keeping its alignment. */
    static String sign(String unsigned, String signed, int minSdk) {
        try {
            KeyStore.PrivateKeyEntry key = GameKey.ensure();
            PrivateKey privateKey = key.getPrivateKey();
            List<X509Certificate> certificates =
                    Collections.singletonList((X509Certificate) key.getCertificate());
            ApkSigner.SignerConfig signer =
                    new ApkSigner.SignerConfig.Builder("venture", privateKey, certificates).build();
            new ApkSigner.Builder(Collections.singletonList(signer))
                    .setInputApk(new File(unsigned))
                    .setOutputApk(new File(signed))
                    .setMinSdkVersion(minSdk)
                    .setV1SigningEnabled(false)
                    .setV2SigningEnabled(true)
                    .setV3SigningEnabled(true)
                    .setAlignmentPreserved(true)
                    .setCreatedBy("Venture")
                    .build()
                    .sign();
            return null;
        } catch (Exception e) {
            return "Could not sign the game: " + e;
        }
    }

    /** The installed version code of a package, or -1 when it is not installed. */
    static long installedVersion(String packageName) {
        try {
            PackageInfo info = Native.appContext.getPackageManager().getPackageInfo(packageName, 0);
            return Build.VERSION.SDK_INT >= 28 ? info.getLongVersionCode() : info.versionCode;
        } catch (PackageManager.NameNotFoundException e) {
            return -1;
        }
    }

    /** Whether the installed package is signed with the user's game key. */
    static boolean signedByUs(String packageName) {
        return GameKey.signs(Native.appContext.getPackageManager(), packageName);
    }

    /**
     * Installs (or updates over, keeping its data) `apk` for `packageName`. The
     * system asks the user to confirm; this waits for the answer.
     */
    static String install(String apk, String packageName) {
        Context context = Native.appContext;
        PackageManager packages = context.getPackageManager();
        if (!packages.canRequestPackageInstalls()) {
            Intent settings = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    Uri.parse("package:" + context.getPackageName()));
            try {
                Device.start(settings);
            } catch (Exception ignored) {
            }
            return "Allow Venture to install apps in the settings that just opened, then try again.";
        }
        PackageInstaller installer = packages.getPackageInstaller();
        PackageInstaller.SessionParams params =
                new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(packageName);
        params.setInstallReason(PackageManager.INSTALL_REASON_USER);
        if (Build.VERSION.SDK_INT >= 31)
            params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
        File file = new File(apk);
        int id;
        try {
            id = installer.createSession(params);
        } catch (IOException e) {
            return "Could not start the install: " + e.getMessage();
        }
        final String action = context.getPackageName() + ".INSTALL_RESULT." + sessions.incrementAndGet();
        final CountDownLatch done = new CountDownLatch(1);
        final int[] status = {PackageInstaller.STATUS_FAILURE};
        final String[] message = {null};
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context c, Intent intent) {
                int code = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
                if (code == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                    Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
                    if (confirm != null) {
                        try {
                            Device.start(confirm);
                            return;
                        } catch (Exception ignored) {
                        }
                    }
                }
                status[0] = code;
                message[0] = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                done.countDown();
            }
        };
        IntentFilter filter = new IntentFilter(action);
        if (Build.VERSION.SDK_INT >= 33)
            context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        else
            context.registerReceiver(receiver, filter);
        try (PackageInstaller.Session session = installer.openSession(id)) {
            try (InputStream in = new FileInputStream(file);
                 OutputStream out = session.openWrite("base.apk", 0, file.length())) {
                copy(in, out);
                session.fsync(out);
            }
            Intent intent = new Intent(action).setPackage(context.getPackageName());
            int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
            PendingIntent pending = PendingIntent.getBroadcast(context, id, intent, flags);
            session.commit(pending.getIntentSender());
            if (!done.await(CONFIRM_MINUTES, TimeUnit.MINUTES)) return "The install was not confirmed.";
        } catch (Exception e) {
            try {
                installer.abandonSession(id);
            } catch (Exception ignored) {
            }
            return "The install failed: " + e.getMessage();
        } finally {
            context.unregisterReceiver(receiver);
        }
        switch (status[0]) {
            case PackageInstaller.STATUS_SUCCESS:
                return null;
            case PackageInstaller.STATUS_FAILURE_ABORTED:
                return "The install was cancelled.";
            case PackageInstaller.STATUS_FAILURE_CONFLICT:
                // Another copy of the game, signed by someone else, is installed:
                // only uninstalling it (and its saves) makes room.
                return "Another copy of this game is installed. Uninstall it first; its saves on this headset go with it.";
            case PackageInstaller.STATUS_FAILURE_STORAGE:
                return "There is not enough space on the headset to install this game.";
            case PackageInstaller.STATUS_FAILURE_INCOMPATIBLE:
                return "This headset cannot install this game" + (message[0] != null ? ": " + message[0] : ".");
            default:
                return "The install failed" + (message[0] != null ? ": " + message[0] : ".");
        }
    }

    private static File obbFolder(String packageName) {
        return new File(Environment.getExternalStorageDirectory(), "Android/obb/" + packageName);
    }

    /** Android/obb/<package>, where the Venture libraries in the game leave their logs. */
    static String obbPath(String packageName) {
        return obbFolder(packageName).getAbsolutePath();
    }

    /**
     * Copies the game's expansion and asset files (and their catalog, plan.json)
     * into Android/obb/<package>, where the game reads them, replacing older copies.
     */
    static String copyObbs(String packageName, String[] files) {
        File folder = obbFolder(packageName);
        if (!folder.isDirectory() && !folder.mkdirs())
            return "Venture cannot write the game's expansion files to " + folder + ".";
        byte[] buffer = new byte[1 << 20];
        for (String path : files) {
            File source = new File(path);
            File target = new File(folder, source.getName());
            if (target.isFile() && target.length() == source.length()
                    && target.lastModified() >= source.lastModified()) continue;
            File partial = new File(folder, source.getName() + ".part");
            try (InputStream in = new FileInputStream(source); OutputStream out = new FileOutputStream(partial)) {
                for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
            } catch (IOException e) {
                partial.delete();
                return "Could not copy " + source.getName() + " to " + folder + ": " + e.getMessage();
            }
            if (!partial.renameTo(target)) {
                target.delete();
                if (!partial.renameTo(target)) return "Could not place " + target + ".";
            }
        }
        return null;
    }

    /** Removes a file Venture put in Android/obb/<package> (a deleted asset file). */
    static String removeObb(String packageName, String name) {
        File target = new File(obbFolder(packageName), name);
        if (!target.getParentFile().equals(obbFolder(packageName))) return "Not a file of this game.";
        return !target.exists() || target.delete() ? null : "Could not remove " + target + ".";
    }

    /** Starts an installed game's launch activity. */
    static String launch(String packageName) {
        Intent intent = Native.appContext.getPackageManager().getLaunchIntentForPackage(packageName);
        if (intent == null) return "The game is installed but has nothing to start.";
        try {
            Device.start(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            return null;
        } catch (Exception e) {
            return "Could not start the game: " + e.getMessage();
        }
    }

    /**
     * What this headset reports about itself for the plan (native_host::HostProfile),
     * as JSON: ABIs (all, and the 32-bit ones), SDK level, page size is read natively, whether the Oculus
     * Platform service exists, and the launch markers that the device's own VR
     * apps share (docs/design/native-headsets.md, section 6).
     */
    static String probe() {
        Context context = Native.appContext;
        PackageManager packages = context.getPackageManager();
        try {
            JSONObject out = new JSONObject();
            JSONArray abis = new JSONArray();
            for (String abi : Build.SUPPORTED_ABIS) abis.put(abi);
            out.put("abis", abis);
            // The 32-bit ABIs on their own: an ARM32 game runs natively only
            // where the CPU still executes AArch32 at EL0.
            JSONArray abis32 = new JSONArray();
            for (String abi : Build.SUPPORTED_32_BIT_ABIS) abis32.put(abi);
            out.put("abis32", abis32);
            out.put("sdk", Build.VERSION.SDK_INT);
            out.put("device", Build.MANUFACTURER + " " + Build.MODEL);
            out.put("platformService", hasService(packages, "com.oculus.horizon"));
            out.put("installPermission", packages.canRequestPackageInstalls());
            markers(packages, out);
            return out.toString();
        } catch (Exception e) {
            return "{}";
        }
    }

    /**
     * The OpenXR runtime each runtime broker names for each of this device's
     * ABIs (the Khronos Android loader's broker contract:
     * content://AUTHORITY/openxr/1/abi/ABI/runtimes/active), as a JSON array of
     * {broker, abi, package, library, present}. A 32-bit game finds a runtime only
     * where a broker names one for its 32-bit ABI. For the headset check.
     */
    static String runtimes() {
        Context context = Native.appContext;
        JSONArray out = new JSONArray();
        String[][] brokers = {{"system", "org.khronos.openxr.system_runtime_broker"},
                              {"installable", "org.khronos.openxr.runtime_broker"}};
        for (String abi : Build.SUPPORTED_ABIS) {
            for (String[] broker : brokers) {
                Uri uri = new Uri.Builder().scheme("content").authority(broker[1]).appendPath("openxr")
                        .appendPath("1").appendPath("abi").appendPath(abi).appendPath("runtimes")
                        .appendPath("active").build();
                try (Cursor cursor = context.getContentResolver().query(uri, null, null, null, null)) {
                    if (cursor == null || !cursor.moveToFirst()) continue;
                    String pkg = column(cursor, "package_name");
                    String dir = column(cursor, "native_lib_dir");
                    String so = column(cursor, "so_filename");
                    JSONObject row = new JSONObject();
                    row.put("broker", broker[0]);
                    row.put("abi", abi);
                    row.put("package", pkg);
                    row.put("library", dir + "/" + so);
                    row.put("present", dir != null && so != null && new File(dir, so).exists());
                    out.put(row);
                } catch (Exception e) {
                    // No such broker, or it refused this app: nothing to report.
                }
            }
        }
        return out.toString();
    }

    private static String column(Cursor cursor, String name) {
        int index = cursor.getColumnIndex(name);
        return index < 0 ? null : cursor.getString(index);
    }

    private static boolean hasService(PackageManager packages, String packageName) {
        try {
            packages.getPackageInfo(packageName, 0);
            return true;
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
    }

    // Standard categories every Android launcher filter may carry.
    private static boolean standard(String category) {
        return category.equals(Intent.CATEGORY_LAUNCHER) || category.equals(Intent.CATEGORY_DEFAULT)
                || category.equals(Intent.CATEGORY_INFO) || category.equals(Intent.CATEGORY_LEANBACK_LAUNCHER)
                || category.equals(Intent.CATEGORY_HOME);
    }

    /**
     * The device's own (system) apps whose MAIN filter carries a category beyond
     * the standard ones are its VR apps. A category at least two of them share is
     * a launch category; an <application> meta-data entry most of them share (same
     * key and value) is launch meta-data. No vendor names: whatever this shell uses.
     */
    private static void markers(PackageManager packages, JSONObject out) throws Exception {
        Intent main = new Intent(Intent.ACTION_MAIN);
        List<ResolveInfo> found = packages.queryIntentActivities(main,
                PackageManager.GET_RESOLVED_FILTER | PackageManager.MATCH_ALL);
        Map<String, Integer> categories = new TreeMap<>();
        Set<String> seen = new HashSet<>();  // package + "\n" + category, each counted once
        Set<String> vrApps = new TreeSet<>();
        for (ResolveInfo info : found) {
            ActivityInfo activity = info.activityInfo;
            if (activity == null || info.filter == null) continue;
            ApplicationInfo app = activity.applicationInfo;
            if ((app.flags & ApplicationInfo.FLAG_SYSTEM) == 0) continue;
            for (Iterator<String> it = info.filter.categoriesIterator(); it != null && it.hasNext(); ) {
                String category = it.next();
                if (standard(category)) continue;
                vrApps.add(app.packageName);
                if (seen.add(app.packageName + "\n" + category)) {
                    Integer n = categories.get(category);
                    categories.put(category, n == null ? 1 : n + 1);
                }
            }
        }
        JSONArray launch = new JSONArray();
        for (Map.Entry<String, Integer> e : categories.entrySet())
            if (e.getValue() >= 2) launch.put(e.getKey());
        out.put("launchCategories", launch);
        // Meta-data shared by most VR apps.
        Map<String, Integer> metadata = new TreeMap<>();
        int apps = vrApps.size();
        for (String packageName : vrApps) {
            ApplicationInfo app;
            try {
                app = packages.getApplicationInfo(packageName, PackageManager.GET_META_DATA);
            } catch (PackageManager.NameNotFoundException missing) {
                continue;
            }
            Bundle data = app.metaData;
            if (data == null) continue;
            for (String key : data.keySet()) {
                Object value = data.get(key);
                if (!(value instanceof String) && !(value instanceof Boolean)) continue;
                String entry = key + "\n" + value;
                Integer n = metadata.get(entry);
                metadata.put(entry, n == null ? 1 : n + 1);
            }
        }
        JSONArray launchMetadata = new JSONArray();
        for (Map.Entry<String, Integer> e : metadata.entrySet()) {
            if (e.getValue() < 2 || e.getValue() * 2 < apps) continue;
            String[] pair = e.getKey().split("\n", 2);
            launchMetadata.put(new JSONArray().put(pair[0]).put(pair[1]));
        }
        out.put("launchMetadata", launchMetadata);
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buffer = new byte[1 << 20];
        for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
    }
}
