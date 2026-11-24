package android.app;

import android.content.Context;
import android.graphics.Typeface;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.attribute.DosFileAttributes;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.function.Consumer;

/**
 * The document picker while the panel is in the headset, where the host's own
 * file dialog (SystemActivities) cannot be seen: a dialog on the panel that
 * browses the host's folders, as a Quest's document picker browses its storage.
 * It answers a folder (OPEN_DOCUMENT_TREE) or a file (OPEN_DOCUMENT,
 * GET_CONTENT); what is picked becomes documents of HostDocuments as from the
 * host's dialog.
 */
final class HeadsetPicker {
  private HeadsetPicker() {}

  /** A row: a folder or file, or the way up. */
  private record Entry(String label, File file, boolean up) {
    @Override public String toString() { return label; }
  }

  /** Shows the picker on the caller's window; `answer` gets the chosen path, or null when cancelled. */
  static void show(Context context, boolean folder, Consumer<String> answer) {
    TextView where = new TextView(context);
    where.setTypeface(Typeface.DEFAULT_BOLD);
    int pad = (int) (16 * context.getResources().getDisplayMetrics().density);
    where.setPadding(pad, pad / 2, pad, pad / 2);
    ListView list = new ListView(context);
    LinearLayout layout = new LinearLayout(context);
    layout.setOrientation(LinearLayout.VERTICAL);
    layout.addView(where, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
        ViewGroup.LayoutParams.WRAP_CONTENT));
    int height = (int) (context.getResources().getDisplayMetrics().heightPixels * 0.55f);
    layout.addView(list, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, height));
    ArrayAdapter<Entry> rows = new ArrayAdapter<>(context, android.R.layout.simple_list_item_1, new ArrayList<>());
    list.setAdapter(rows);
    File[] current = {start()};
    boolean[] answered = {false};
    Runnable[] refresh = new Runnable[1];
    AlertDialog.Builder builder = new AlertDialog.Builder(context)
        .setTitle(folder ? "Choose a folder" : "Choose a file")
        .setView(layout)
        .setNegativeButton("Cancel", null);
    if (folder) builder.setPositiveButton("Use this folder", null);
    AlertDialog dialog = builder.create();
    refresh[0] = () -> {
      rows.clear();
      File dir = current[0];
      if (dir == null) {
        where.setText("This computer");
        for (File root : roots()) rows.add(new Entry(root.getPath(), root, false));
      } else {
        where.setText(dir.getPath());
        rows.add(new Entry("Up one level", dir.getParentFile(), true));
        File[] children = dir.listFiles();
        if (children == null) children = new File[0];
        Arrays.sort(children, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
        for (File child : children) {
          if (hidden(child)) continue;
          if (child.isDirectory()) rows.add(new Entry(child.getName() + File.separator, child, false));
        }
        if (!folder)
          for (File child : children)
            if (!hidden(child) && child.isFile())
              rows.add(new Entry(child.getName(), child, false));
      }
      rows.notifyDataSetChanged();
      list.setSelection(0);
      if (folder && dialog.getButton(AlertDialog.BUTTON_POSITIVE) != null)
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(dir != null);
    };
    list.setOnItemClickListener((parent, view, position, id) -> {
      Entry entry = rows.getItem(position);
      if (entry == null) return;
      if (entry.up() || entry.file().isDirectory()) {
        current[0] = entry.file();  // null above a drive: the list of drives
        refresh[0].run();
      } else if (!folder) {
        answered[0] = true;
        dialog.dismiss();
        answer.accept(entry.file().getPath());
      }
    });
    dialog.setOnShowListener(shown -> {
      refresh[0].run();
      if (folder) {
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
          if (current[0] == null) return;
          answered[0] = true;
          dialog.dismiss();
          answer.accept(current[0].getPath());
        });
      }
    });
    dialog.setOnDismissListener(gone -> {
      if (!answered[0]) {
        answered[0] = true;
        answer.accept(null);
      }
    });
    dialog.show();
  }

  /**
   * Entries a file manager leaves out: dot files, and on Windows hidden or system entries, judged by the entry itself
   * and not by the target of a junction or link (File.isHidden follows those, so Windows' compatibility junctions
   * such as "Application Data" were listed and could not be opened).
   */
  private static boolean hidden(File file) {
    if (file.getName().startsWith(".")) return true;
    try {
      DosFileAttributes attributes =
          Files.readAttributes(file.toPath(), DosFileAttributes.class, LinkOption.NOFOLLOW_LINKS);
      return attributes.isHidden() || attributes.isSystem();
    } catch (IOException | UnsupportedOperationException e) {
      return file.isHidden();
    }
  }

  /** The person's home folder, where the host's dialogs also start. */
  private static File start() {
    File home = new File(System.getProperty("user.home", "."));
    return home.isDirectory() ? home : null;
  }

  /** The drives (Windows) or the file system root, and the home folder. */
  private static List<File> roots() {
    List<File> out = new ArrayList<>();
    File home = start();
    if (home != null) out.add(home);
    for (File root : File.listRoots())
      if (root.exists() && !root.getPath().toLowerCase(Locale.ROOT).startsWith("\\\\")) out.add(root);
    return out;
  }
}
