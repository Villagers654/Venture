// SPDX-License-Identifier: GPL-3.0-or-later
package org.venture_emu.venture;

import android.app.Activity;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

/**
 * Game files the user picks on the headset, through Android's own document
 * picker (the Storage Access Framework): an APK with its expansion files, a
 * ZIP, or a folder. Venture reads them through the picker's grant, with no
 * storage permission, and copies what it imports into its own storage
 * (common/library/import.h, documents). The C++ core calls list and open on
 * its worker threads (launcher/platform/android/documents.cpp).
 */
final class Documents {
    private Documents() {}

    // Request codes of their own, beside Permissions' and above SDL's counters.
    private static final int PICK_FILES = 0x5640, PICK_FOLDER = 0x5641;
    // Entries looked at per pick and folder depth, as the desktop's folder scan.
    private static final int LIMIT = 20000, DEPTH = 3;

    /** Opens the picker for files (several at once) or a folder; false when none can show. */
    static boolean pick(final boolean folder) {
        Context context = Device.context();
        if (!(context instanceof Activity)) return false;
        final Activity activity = (Activity) context;
        final Intent intent;
        if (folder) {
            intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        } else {
            // Expansion files have no MIME type of their own: any file, several at once.
            intent = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                    .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        }
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        if (intent.resolveActivity(activity.getPackageManager()) == null) return false;
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    activity.startActivityForResult(intent, folder ? PICK_FOLDER : PICK_FILES);
                } catch (Exception e) {
                    Native.documentsPicked(new String[0]);
                }
            }
        });
        return true;
    }

    /** The activity's result; false when it was not a pick of these. */
    static boolean onResult(int code, int result, Intent data) {
        if (code != PICK_FILES && code != PICK_FOLDER) return false;
        List<String> uris = new ArrayList<>();
        if (result == Activity.RESULT_OK && data != null) {
            ClipData clip = data.getClipData();
            if (clip != null) {
                for (int i = 0; i < clip.getItemCount(); ++i)
                    if (clip.getItemAt(i).getUri() != null) uris.add(clip.getItemAt(i).getUri().toString());
            } else if (data.getData() != null) {
                uris.add(data.getData().toString());
            }
        }
        Native.documentsPicked(uris.toArray(new String[0]));
        return true;
    }

    /**
     * What the picked documents hold, as JSON {root, files: [{path, size, uri}]}:
     * a picked file by its name, a picked folder's files (three levels deep) by
     * their '/'-separated path under it. Keeps the grants across restarts, so
     * an interrupted import resumes.
     */
    static String list(String[] picked) {
        ContentResolver resolver = Native.appContext.getContentResolver();
        JSONObject out = new JSONObject();
        JSONArray files = new JSONArray();
        try {
            String root = "";
            for (String text : picked) {
                Uri uri = Uri.parse(text);
                try {
                    resolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (SecurityException e) {
                    // Not persistable (a provider that does not offer it): this run only.
                }
                if (isTree(uri)) {
                    String id = DocumentsContract.getTreeDocumentId(uri);
                    if (picked.length == 1) root = name(resolver, DocumentsContract.buildDocumentUriUsingTree(uri, id));
                    walk(resolver, uri, id, "", 0, files);
                } else {
                    String[] row = describe(resolver, uri);
                    if (row != null)
                        files.put(new JSONObject().put("path", row[0]).put("size", Long.parseLong(row[1]))
                                .put("uri", text));
                }
            }
            out.put("root", root);
            out.put("files", files);
            return out.toString();
        } catch (Exception e) {
            return "{\"error\":" + JSONObject.quote(String.valueOf(e.getMessage())) + "}";
        }
    }

    private static boolean isTree(Uri uri) {
        java.util.List<String> parts = uri.getPathSegments();
        return parts.size() >= 2 && parts.get(0).equals("tree") && !parts.contains("document");
    }

    private static void walk(ContentResolver resolver, Uri tree, String parent, String prefix, int depth,
                             JSONArray out) throws Exception {
        if (depth >= DEPTH || out.length() >= LIMIT) return;
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parent);
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                            DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE};
        try (Cursor c = resolver.query(children, columns, null, null, null)) {
            while (c != null && c.moveToNext() && out.length() < LIMIT) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                if (name == null || name.contains("/")) continue;
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    walk(resolver, tree, id, prefix + name + "/", depth + 1, out);
                } else {
                    out.put(new JSONObject().put("path", prefix + name).put("size", c.isNull(3) ? 0 : c.getLong(3))
                            .put("uri", DocumentsContract.buildDocumentUriUsingTree(tree, id).toString()));
                }
            }
        }
    }

    private static String name(ContentResolver resolver, Uri uri) {
        String[] row = describe(resolver, uri);
        return row == null ? "" : row[0];
    }

    // {display name, size} of one document.
    private static String[] describe(ContentResolver resolver, Uri uri) {
        String[] columns = {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        try (Cursor c = resolver.query(uri, columns, null, null, null)) {
            if (c == null || !c.moveToFirst()) return null;
            String name = c.getString(0);
            if (name == null || name.contains("/")) return null;
            return new String[] {name, String.valueOf(c.isNull(1) ? 0 : c.getLong(1))};
        } catch (Exception e) {
            return null;
        }
    }

    /** A file descriptor the caller owns for reading the document, or -1. */
    static int open(String uri) {
        try {
            ParcelFileDescriptor fd =
                    Native.appContext.getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
            return fd == null ? -1 : fd.detachFd();
        } catch (Exception e) {
            return -1;
        }
    }
}
