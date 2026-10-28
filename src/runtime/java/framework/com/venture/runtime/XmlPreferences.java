package com.venture.runtime;

import android.content.SharedPreferences;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import javax.xml.parsers.DocumentBuilderFactory;
import javax.xml.transform.*;
import javax.xml.transform.dom.DOMSource;
import javax.xml.transform.stream.StreamResult;

/** Android SharedPreferencesImpl XML tags; commits replace one complete file. */
public final class XmlPreferences implements SharedPreferences {
  private static final boolean TRACE = HostEnvironment.getenv("VENTURE_TRACE_PREFERENCES") != null;
  private static final java.util.concurrent.atomic.AtomicLong nextId = new java.util.concurrent.atomic.AtomicLong();
  private final long id = nextId.incrementAndGet();
  private long generation;
  // Confined to the QueuedWork writer, including reentrant inline writes.
  private long diskGeneration;
  // Opt-in menu quit barriers. QueuedWork owns write accounting and failures.
  public static boolean finishPendingWrites() { return android.app.QueuedWork.finishPendingWrites(); }
  public static boolean writesQuiet() {
    return android.app.QueuedWork.getFailureCount() == 0 && android.app.QueuedWork.writesQuiet(1000);
  }
  private final Path file;
  private final Map<String, Object> values = new HashMap<>();
  private final Set<OnSharedPreferenceChangeListener> listeners = new HashSet<>();
  public XmlPreferences(Path path) {
    file = path;
    if (Files.exists(path))
      try {
        var f = DocumentBuilderFactory.newInstance();
        f.setFeature("http://apache.org/xml/features/disallow-doctype-decl", true);
        f.setExpandEntityReferences(false);
        var nodes =
            f.newDocumentBuilder().parse(path.toFile()).getDocumentElement().getChildNodes();
        for (int i = 0; i < nodes.getLength(); i++)
          if (nodes.item(i) instanceof org.w3c.dom.Element e) {
            String key = e.getAttribute("name"), v = e.getAttribute("value");
            Object value = switch (e.getTagName()) {
              case "string" -> e.getTextContent();
              case "int" -> Integer.valueOf(v);
              case "long" -> Long.valueOf(v);
              case "float" -> Float.valueOf(v);
              case "boolean" -> Boolean.valueOf(v);
              case "set" -> {
                Set<String> s = new HashSet<>();
                var strings = e.getElementsByTagName("string");
                for (int j = 0; j < strings.getLength(); j++)
                  s.add(strings.item(j).getTextContent());
                yield Set.copyOf(s);
              }
              default -> throw new IllegalArgumentException("Unknown preference type");
            };
            values.put(key, value);
          }
      } catch (Exception e) {
        throw new IllegalStateException("Cannot read preferences " + path, e);
      }
    trace("load", 0, 0, true, values.size());
  }
  private void trace(String phase, long version, long duration, boolean success, int count) {
    if (TRACE) RuntimeEnvironment.host().log(4, "Preferences",
        "phase=" + phase + " id=" + id + " file=" + Integer.toUnsignedString(file.getFileName().toString().hashCode(), 16)
        + " generation=" + version + " duration_ns=" + duration + " success=" + success + " entries=" + count
        + " caller=" + Thread.currentThread().threadId());
  }
  public synchronized Map<String, ?> getAll() {
    return new HashMap<>(values);
  }
  public synchronized boolean contains(String k) {
    return values.containsKey(k);
  }
  public synchronized String getString(String k, String d) {
    return (String) values.getOrDefault(k, d);
  }
  @SuppressWarnings("unchecked")
  public synchronized Set<String> getStringSet(String k, Set<String> d) {
    Set<String> s = (Set<String>) values.getOrDefault(k, d);
    return s == null ? null : new HashSet<>(s);
  }
  public synchronized int getInt(String k, int d) {
    return (Integer) values.getOrDefault(k, d);
  }
  public synchronized long getLong(String k, long d) {
    return (Long) values.getOrDefault(k, d);
  }
  public synchronized float getFloat(String k, float d) {
    return (Float) values.getOrDefault(k, d);
  }
  public synchronized boolean getBoolean(String k, boolean d) {
    return (Boolean) values.getOrDefault(k, d);
  }
  public synchronized void registerOnSharedPreferenceChangeListener(
      OnSharedPreferenceChangeListener l) {
    listeners.add(l);
  }
  public synchronized void unregisterOnSharedPreferenceChangeListener(
      OnSharedPreferenceChangeListener l) {
    listeners.remove(l);
  }
  public Editor edit() {
    return new Edit();
  }
  private boolean save(Map<String, Object> next, long version) {
    long begin = TRACE ? System.nanoTime() : 0;
    boolean success = false;
    try {
      // An inline synchronous save can supersede an older queued snapshot.
      // Never let that older task roll the durable state back afterwards.
      if (version <= diskGeneration) { success = true; return true; }
      Files.createDirectories(file.getParent());
      var doc = DocumentBuilderFactory.newInstance().newDocumentBuilder().newDocument();
      var map = doc.createElement("map");
      doc.appendChild(map);
      for (var entry : new TreeMap<>(next).entrySet()) {
        Object v = entry.getValue();
        String type = v instanceof String ? "string"
            : v instanceof Integer        ? "int"
            : v instanceof Long           ? "long"
            : v instanceof Float          ? "float"
            : v instanceof Boolean        ? "boolean"
                                          : "set";
        var e = doc.createElement(type);
        e.setAttribute("name", entry.getKey());
        if (v instanceof Set<?> set) {
          for (Object s : new TreeSet<>((Set<String>) set)) {
            var child = doc.createElement("string");
            child.setTextContent(s.toString());
            e.appendChild(child);
          }
        } else if (v instanceof String)
          e.setTextContent((String) v);
        else
          e.setAttribute("value", v.toString());
        map.appendChild(e);
      }
      Path temp = Files.createTempFile(file.getParent(), "preferences-", ".tmp");
      try {
        var transformer = TransformerFactory.newInstance().newTransformer();
        transformer.setOutputProperty(OutputKeys.ENCODING, "UTF-8");
        transformer.transform(new DOMSource(doc), new StreamResult(temp.toFile()));
        try (var channel = java.nio.channels.FileChannel.open(temp, StandardOpenOption.WRITE)) {
          channel.force(true);
        }
        Files.move(temp, file, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
      } finally {
        Files.deleteIfExists(temp);
      }
      diskGeneration = version;
      success = true;
      return true;
    } catch (Exception e) {
      RuntimeEnvironment.host().log(
          6, "SharedPreferences", "Commit failed: " + e.getClass().getSimpleName());
      return false;
    } finally { trace("persist", version, TRACE ? System.nanoTime() - begin : 0, success, next.size()); }
  }
  private final class Edit implements Editor {
    private final Map<String, Object> updates = new HashMap<>();
    private boolean clear;
    private synchronized Editor put(String k, Object v) {
      updates.put(Objects.requireNonNull(k), v);
      return this;
    }
    public Editor putString(String k, String v) {
      return put(k, v);
    }
    public Editor putStringSet(String k, Set<String> v) {
      return put(k, v == null ? null : Set.copyOf(v));
    }
    public Editor putInt(String k, int v) {
      return put(k, v);
    }
    public Editor putLong(String k, long v) {
      return put(k, v);
    }
    public Editor putFloat(String k, float v) {
      return put(k, v);
    }
    public Editor putBoolean(String k, boolean v) {
      return put(k, v);
    }
    public Editor remove(String k) {
      return put(k, null);
    }
    public synchronized Editor clear() {
      clear = true;
      return this;
    }
    private Publication publish(boolean synchronous) {
      Map<String, Object> next;
      List<String> changedKeys;
      List<OnSharedPreferenceChangeListener> callbacks;
      long version;
      synchronized (XmlPreferences.this) {
        next = new HashMap<>(values);
        Set<String> changed = new LinkedHashSet<>();
        synchronized (this) {
          if (clear) {
            changed.addAll(next.keySet());
            next.clear();
            clear = false;
          }
          updates.forEach((k, v) -> {
            if (!Objects.equals(next.get(k), v)) {
              changed.add(k);
              if (v == null) next.remove(k);
              else next.put(k, v);
            }
          });
          updates.clear();
        }
        version = ++generation;
        values.clear();
        values.putAll(next);
        changedKeys = new ArrayList<>(changed);
        callbacks = new ArrayList<>(listeners);
      }
      // A worker-thread submission can perform IO inline. Leave the memory
      // monitor first; diskGeneration prevents older snapshots rolling it back.
      var written = android.app.QueuedWork.queueWrite(() -> save(next, version), synchronous);
      return new Publication(changedKeys, callbacks, version, written);
    }

    public boolean commit() {
      long begin = TRACE ? System.nanoTime() : 0;
      Publication publication = publish(true);
      boolean saved;
      try { saved = publication.written().get(); }
      catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); saved = false; }
      catch (ExecutionException failure) { saved = false; }
      notifyListeners(publication);
      trace("commit", publication.version(), TRACE ? System.nanoTime() - begin : 0, saved, publication.changed().size());
      return saved;
    }
    public void apply() {
      long begin = TRACE ? System.nanoTime() : 0;
      Publication publication = publish(false);
      notifyListeners(publication);
      trace("apply", publication.version(), TRACE ? System.nanoTime() - begin : 0, true, publication.changed().size());
    }
  }
  private record Publication(List<String> changed, List<OnSharedPreferenceChangeListener> callbacks,
                             long version, CompletableFuture<Boolean> written) {}
  private void notifyListeners(Publication publication) {
    Runnable notify = () -> {
      for (String key : publication.changed())
        for (var l : publication.callbacks()) l.onSharedPreferenceChanged(XmlPreferences.this, key);
    };
    var main = android.os.Looper.getMainLooper();
    if (main != null && !main.isCurrentThread())
      new android.os.Handler(main).post(notify);
    else
      notify.run();
  }
}
