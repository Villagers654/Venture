package android.app;

import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import com.venture.runtime.HostDocuments;
import com.venture.runtime.RuntimePermissions;
import com.venture.runtime.layoutlib.PanelHost;
import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * The system's own activities a 2D app starts for a result: the permission
 * dialog and the storage access framework's document picker. Android runs them
 * in other packages; Venture answers them in process. The permission dialog
 * answers as a Quest's does (RuntimePermissions). The picker is the host's file
 * or folder dialog, and what the person picks becomes documents of
 * com.android.externalstorage.documents (HostDocuments).
 */
final class SystemActivities {
  private SystemActivities() {}

  // AOSP Intent values.
  static final String OPEN_DOCUMENT_TREE = "android.intent.action.OPEN_DOCUMENT_TREE",
      OPEN_DOCUMENT = "android.intent.action.OPEN_DOCUMENT", CREATE_DOCUMENT = "android.intent.action.CREATE_DOCUMENT",
      GET_CONTENT = "android.intent.action.GET_CONTENT", EXTRA_TITLE = "android.intent.extra.TITLE",
      EXTRA_ALLOW_MULTIPLE = "android.intent.extra.ALLOW_MULTIPLE";
  static final int GRANT_READ = 0x1, GRANT_WRITE = 0x2, GRANT_PERSISTABLE = 0x40, GRANT_PREFIX = 0x80;

  /** True when the intent was one of these, now answered or being answered. */
  static boolean start(Context who, Activity caller, String callerWho, Intent intent, int requestCode) {
    String action = intent.getAction();
    if (action == null || intent.getComponent() != null) return false;
    switch (action) {
      case PackageManager.ACTION_REQUEST_PERMISSIONS -> {
        String[] names = intent.getStringArrayExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_NAMES);
        if (names == null) names = new String[0];
        RuntimePermissions.request(who.getPackageName(), names);
        int[] results = new int[names.length];
        for (int i = 0; i < names.length; ++i) results[i] = who.checkSelfPermission(names[i]);
        Intent data = new Intent(PackageManager.ACTION_REQUEST_PERMISSIONS);
        data.putExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_NAMES, names);
        data.putExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_RESULTS, results);
        deliver(caller, callerWho, requestCode, Activity.RESULT_OK, data);
        return true;
      }
      case OPEN_DOCUMENT_TREE, OPEN_DOCUMENT, CREATE_DOCUMENT, GET_CONTENT -> {
        int kind = action.equals(OPEN_DOCUMENT_TREE) ? PanelHost.PICK_FOLDER
            : action.equals(CREATE_DOCUMENT) ? PanelHost.PICK_SAVE : PanelHost.PICK_OPEN;
        String name = intent.getStringExtra(EXTRA_TITLE);
        boolean many = intent.getBooleanExtra(EXTRA_ALLOW_MULTIPLE, false);
        Thread picker = new Thread(() -> {
          String[] paths = PanelHost.pick(kind, name, many);
          new Handler(Looper.getMainLooper()).post(() -> answer(caller, callerWho, requestCode, kind, paths));
        }, "Venture document picker");
        picker.setDaemon(true);
        picker.start();
        return true;
      }
      default -> {
        return false;
      }
    }
  }

  private static void answer(Activity caller, String callerWho, int requestCode, int kind, String[] paths) {
    if (paths == null || paths.length == 0) {
      android.util.Log.i("DocumentPicker", "Picker cancelled");
      deliver(caller, callerWho, requestCode, Activity.RESULT_CANCELED, null);
      return;
    }
    try {
      List<Uri> uris = new ArrayList<>();
      for (String path : paths) {
        File file = new File(path);
        if (kind == PanelHost.PICK_FOLDER) {
          if (!file.isDirectory()) throw new IOException("Not a folder");
          uris.add(HostDocuments.treeUri(HostDocuments.documentId(HostDocuments.grant(file), "")));
        } else {
          if (kind == PanelHost.PICK_SAVE && !file.exists() && !file.createNewFile())
            throw new IOException("Cannot create the file");
          if (!file.isFile()) throw new IOException("Not a file");
          // A single document: its folder becomes a root, as a picked tree does.
          String root = HostDocuments.grant(file.getAbsoluteFile().getParentFile());
          uris.add(HostDocuments.documentUri(HostDocuments.documentId(root, file.getName())));
        }
      }
      Intent data = new Intent();
      data.setData(uris.get(0));
      data.addFlags(GRANT_READ | GRANT_WRITE | GRANT_PERSISTABLE | (kind == PanelHost.PICK_FOLDER ? GRANT_PREFIX : 0));
      if (uris.size() > 1) {
        ClipData clip = ClipData.newRawUri(null, uris.get(0));
        for (int i = 1; i < uris.size(); ++i) clip.addItem(new ClipData.Item(uris.get(i)));
        data.setClipData(clip);
      }
      android.util.Log.i("DocumentPicker", "Picked " + uris.size() + " " + (kind == PanelHost.PICK_FOLDER ? "folder" : "file")
          + (uris.size() == 1 ? "" : "s"));
      deliver(caller, callerWho, requestCode, Activity.RESULT_OK, data);
    } catch (IOException | RuntimeException e) {
      android.util.Log.w("DocumentPicker", "Cannot use the picked " + (kind == PanelHost.PICK_FOLDER ? "folder" : "file")
          + ": " + e.getMessage());
      deliver(caller, callerWho, requestCode, Activity.RESULT_CANCELED, null);
    }
  }

  private static void deliver(Activity caller, String callerWho, int requestCode, int resultCode, Intent data) {
    if (caller == null || requestCode < 0) return;
    new Handler(Looper.getMainLooper()).post(
        () -> caller.dispatchActivityResult(callerWho, requestCode, resultCode, data, "system activity"));
  }
}
