import android.os.*;
import com.venture.runtime.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

/** JDK-only behavioral tests. No installed game, native library, JIT or JUnit. */
public final class FrameworkTest {
  private static int checks;
  private static void check(boolean ok, String message) {
    checks++;
    if (!ok)
      throw new AssertionError(message);
  }
  public static class OptionalParameter {}
  public static class NativeStartupActivity extends android.app.NativeActivity {
    public void createOnly() { super.onCreate(null); }
  }
  private static void censusValues() {
    byte[] bytes = {0, 1, 2, (byte) 251, (byte) 255};
    check(android.util.Base64.encodeToString(bytes, 0).equals("AAEC+/8=\n"), "Android Base64 DEFAULT ends in LF");
    check(android.util.Base64.encodeToString(bytes, 2 | 8 | 1).equals("AAEC-_8"), "URL-safe unpadded Base64");
    check(Arrays.equals(bytes, android.util.Base64.decode(" AAEC-_8\r\n", 8)), "Base64 accepts whitespace and omitted padding");
    // Firebase writes its heartbeat through a Base64OutputStream it then reads back.
    try {
      var encoded = new java.io.ByteArrayOutputStream();
      try (var stream = new android.util.Base64OutputStream(encoded, 2 | 8 | 1)) { stream.write(bytes); }
      var decoded = new android.util.Base64InputStream(new java.io.ByteArrayInputStream(encoded.toByteArray()), 8).readAllBytes();
      check(encoded.toString().equals("AAEC-_8") && Arrays.equals(bytes, decoded), "Base64 streams encode and decode");
    } catch (java.io.IOException e) { throw new java.io.UncheckedIOException(e); }
    check(android.util.Base64.encodeToString(bytes, 1, 3, 2).equals("AQL7"), "Base64 offset/length bounds");
    check(android.util.Base64.encodeToString(new byte[57], 4).endsWith("\r\n")
        && android.util.Base64.encode(new byte[58], 4).length == 84, "Base64 wraps at 76 with final CRLF");
    check(android.util.Base64.encode(new byte[0], 0).length == 0, "empty Base64 has no newline");
    boolean invalid = false;
    try { android.util.Base64.decode("a===", 0); } catch (IllegalArgumentException expected) { invalid = true; }
    check(invalid, "Base64 rejects malformed padding");
    var pair = android.util.Pair.create("a", null);
    check(android.util.Log.getStackTraceString(null).isEmpty(), "Log stack trace of no throwable is empty");
    check(android.util.Log.getStackTraceString(new RuntimeException(new java.net.UnknownHostException())).isEmpty(),
          "Log hides network-unavailable stack traces");
    check(android.util.Log.getStackTraceString(new RuntimeException("x")).contains("RuntimeException: x"), "Log prints stack traces");
    check(pair.equals(new android.util.Pair<>("a", null)) && pair.hashCode() == "a".hashCode(), "Pair null equality and hash");
    var sparse = new android.util.SparseArray<Object>(0);
    Object first = new String("same"), equal = new String("same");
    sparse.put(7, first); sparse.put(-4, equal); sparse.append(2, null); sparse.append(7, first);
    check(sparse.size() == 3 && sparse.keyAt(0) == -4 && sparse.keyAt(2) == 7, "SparseArray sorts and replaces appended keys");
    check(sparse.get(2, first) == null && sparse.get(3, first) == first, "SparseArray distinguishes missing and null");
    check(sparse.indexOfValue(first) == 2 && sparse.indexOfValue(new String("same")) == -1,
        "SparseArray indexOfValue uses identity");
    check(sparse.indexOfKey(0) == ~1, "SparseArray missing-key index preserves insertion point");
    var copy = sparse.clone(); copy.removeAtRange(1, 99);
    check(copy.size() == 1 && sparse.size() == 3 && copy.valueAt(0) == equal, "SparseArray clone owns arrays and shares values");
    copy.clear(); check(copy.size() == 0 && sparse.size() == 3, "SparseArray clear releases independent entries");
    check(android.util.Size.parseSize("1920*1080").equals(new android.util.Size(1920, 1080)), "Size parses both separators");
    check(android.util.SizeF.parseSizeF("1.5x2.25").getHeight() == 2.25f, "SizeF fractional dimensions");
    var sizeParcel = Parcel.obtain();
    new android.util.SizeF(1.5f, 2.25f).writeToParcel(sizeParcel, 0); sizeParcel.setDataPosition(0);
    check(android.util.SizeF.CREATOR.createFromParcel(sizeParcel).equals(new android.util.SizeF(1.5f, 2.25f)), "SizeF parcel preserves both float dimensions");
    sizeParcel.recycle();
    invalid = false;
    try { new android.util.SizeF(Float.NaN, 1); } catch (IllegalArgumentException expected) { invalid = true; }
    check(invalid, "SizeF rejects nonfinite values");
    var range = new android.util.Range<>(10, 20);
    check(range.hashCode() == 671, "Range uses Android's multiply-XOR hash");
    check(range.contains(10) && range.contains(20) && !range.contains(9) && range.clamp(30) == 20, "Range includes both endpoints");
    check(range.intersect(15, 25).equals(new android.util.Range<>(15, 20))
        && range.extend(0).equals(new android.util.Range<>(0, 20)), "Range intersection and union");
    invalid = false;
    try { range.intersect(21, 30); } catch (IllegalArgumentException expected) { invalid = true; }
    check(invalid, "Range rejects disjoint intersections");
    float[] rotation = new float[9], angles = new float[3];
    android.hardware.SensorManager.getRotationMatrixFromVector(rotation, new float[]{0, 0, 0});
    check(Arrays.equals(rotation, new float[]{1,0,0,0,1,0,0,0,1}), "zero rotation vector makes identity");
    check(android.hardware.SensorManager.getRotationMatrix(rotation, null, new float[]{0,0,9.81f}, new float[]{0,1,0})
        && android.hardware.SensorManager.getOrientation(rotation, angles) == angles
        && angles[0] == 0 && angles[1] == 0 && angles[2] == 0, "gravity/magnetic matrix and orientation");
    check(!android.hardware.SensorManager.getRotationMatrix(rotation, null, new float[3], new float[]{0,1,0}), "free fall has no orientation matrix");
    check(android.hardware.SensorManager.remapCoordinateSystem(rotation, 2, 0x81, rotation)
        && rotation[1] == 1 && rotation[3] == -1, "sensor axes remap correctly in place");
    check(!android.hardware.SensorManager.remapCoordinateSystem(rotation, 1, 1, new float[9]), "duplicate sensor axes are invalid");
  }
  private static void censusWidgets(android.content.Context context) {
    var box = new android.widget.CheckBox(context);
    var changes = new ArrayList<Boolean>();
    box.setOnCheckedChangeListener((button, checked) -> {
      changes.add(checked);
      if (checked) button.setChecked(false);
    });
    box.setChecked(true);
    check(changes.equals(List.of(true)) && !box.isChecked(), "CheckBox suppresses recursive checked notifications");
    box.setOnCheckedChangeListener((button, checked) -> changes.add(checked));
    var clickedState = new ArrayList<Boolean>();
    box.setOnClickListener(view -> clickedState.add(box.isChecked()));
    check(box.isClickable() && box.performClick() && clickedState.equals(List.of(true)), "CheckBox toggles before click dispatch");
    check(box.getAccessibilityClassName().equals("android.widget.CheckBox"), "CheckBox accessibility type");
    var view = new android.view.View(context);
    check(view.getMeasuredWidth() == 0 && view.getMeasuredHeight() == 0, "detached unmeasured View has zero measured size");
    view.setMinimumWidth(40); view.setMinimumHeight(30);
    view.measure(android.view.View.MeasureSpec.makeMeasureSpec(90, android.view.View.MeasureSpec.EXACTLY), 0);
    check(view.getMeasuredWidth() == 90 && view.getMeasuredHeight() == 30, "View measurement honors exact and unspecified sizes");
    view.layout(0, 0, 100, 80);
    check(view.getWidth() == 100 && view.getMeasuredWidth() == 90, "layout bounds and measurement are independent");
    view.attach();
    check(view.getMeasuredWidth() == 90 && view.getMeasuredHeight() == 30, "attachment preserves explicit measurement");
    view.detach();
    var hosted = new android.view.View(context) {
      @Override protected void onAttachedToWindow() {
        check(getMeasuredWidth() == 0 && getMeasuredHeight() == 0, "measurement follows attachment callbacks");
      }
    };
    var window = new android.view.Window(context);
    window.setContentView(hosted); window.attach();
    check(window.getDecorView().getMeasuredWidth() == 0 && hosted.getMeasuredHeight() == 0,
        "window measures decor and content on the next traversal");
    window.detach();
    int limited = android.view.View.resolveSizeAndState(80,
        android.view.View.MeasureSpec.makeMeasureSpec(50, android.view.View.MeasureSpec.AT_MOST), 0);
    check(limited == (50 | android.view.View.MEASURED_STATE_TOO_SMALL), "at-most measurement reports clipped state");
    var editor = new android.widget.EditText(context) {
      @Override protected void onSelectionChanged(int start, int end) { clickedState.add(start == 1 && end == 3); }
    };
    editor.setText("abcd"); editor.setSelection(1, 3); editor.append("ef");
    check(editor.getSelectionStart() == 1 && editor.getSelectionEnd() == 3 && clickedState.get(1), "editor selection callback and retained spans");
    check(editor.getText().toString().equals("abcdef"), "TextView append edits its existing buffer");
    var connection = new android.view.inputmethod.BaseInputConnection(editor, true);
    var wrapper = new android.view.inputmethod.InputConnectionWrapper(null, false);
    wrapper.setTarget(connection);
    check(wrapper.setSelection(1, 3) && wrapper.commitText("X", 1) && editor.getText().toString().equals("aXdef"), "InputConnectionWrapper delegates edit results");
    boolean immutable = false;
    try { wrapper.setTarget(connection); } catch (SecurityException expected) { immutable = true; }
    check(immutable, "immutable input wrapper cannot be retargeted after initialization");
    boolean negative = false;
    try { wrapper.getTextBeforeCursor(-1, 0); } catch (IllegalArgumentException expected) { negative = true; }
    check(negative, "input wrapper validates requested text length");
  }
  public static class NativeMetadataFixture {
    public native int call(Object value);
    public void optional(OptionalParameter value) {}
  }
  private static BinaryResources.Node manifestNode(String tag, String... attributes) throws Exception {
    var constructor = BinaryResources.Node.class.getDeclaredConstructor(String.class);
    constructor.setAccessible(true);
    var node = constructor.newInstance(tag);
    for (int i = 0; i < attributes.length; i += 2)
      node.attributes.put(attributes[i], new BinaryResources.Value(3, 0, attributes[i + 1]));
    return node;
  }
  private static void lenientUris() {
    var built = new android.net.Uri.Builder().scheme("mailto").appendQueryParameter("subject", "x").build();
    check(built.isHierarchical() && built.getPath().equals("") && built.getQueryParameter("subject").equals("x"),
        "Builder retains hierarchy without authority or path");
    check(built.buildUpon().appendQueryParameter("b", "y").build().getEncodedQuery().equals("subject=x&b=y"),
        "buildUpon preserves query on empty hierarchical path");
    check(new android.net.Uri.Builder().scheme("foo").build().getPath().equals(""), "empty built hierarchy has empty path");
    var opaque = new android.net.Uri.Builder().scheme("file").encodedOpaquePart("//a?q=x").build();
    check(opaque.isOpaque() && opaque.getEncodedPath() == null && opaque.getEncodedAuthority() == null
        && opaque.getEncodedQuery() == null && opaque.buildUpon().build().isOpaque(), "Builder retains opaque structure despite slash prefix");
    var relative = new android.net.Uri.Builder().encodedPath("a:b").encodedQuery("").build();
    check(relative.isRelative() && relative.getScheme() == null && relative.getPath().equals("a:b")
        && relative.getEncodedQuery().equals(""), "Builder retains relative path and empty components");
    for (String punctuation : List.of(" ", "{", "}", "|", "^", "%")) {
      String raw = "https://example.test/a" + punctuation + "b?q=" + punctuation + "#" + punctuation;
      var uri = android.net.Uri.parse(raw);
      check(uri.getScheme().equals("https") && uri.getHost().equals("example.test"), "StringUri accepts " + punctuation);
      check(uri.getEncodedPath().equals("/a" + punctuation + "b") && uri.getEncodedQuery().equals("q=" + punctuation), "encoded URI components preserve " + punctuation);
      check(new android.content.Intent(android.content.Intent.ACTION_VIEW, uri).setPackage("test.pkg")
          .toUri(android.content.Intent.URI_ANDROID_APP_SCHEME).equals("android-app://test.pkg/https/example.test/a" + punctuation + "b?q=" + punctuation + "#" + punctuation),
          "android-app serialization preserves lenient data " + punctuation);
    }
    var uri = android.net.Uri.parse("https://user@EXAMPLE.test:443/a%2Fb//c/?q=a+b%20c#f");
    check(uri.getPort() == 443 && uri.getUserInfo().equals("user") && uri.getPathSegments().equals(List.of("a/b", "c")), "URI splits encoded paths before decoding");
    check(uri.getQueryParameter("q").equals("a b c") && android.net.Uri.decode("a+b").equals("a+b"), "only query parameters decode plus as space");
    check(android.net.Uri.decode("%").equals("\ufffd") && uri.buildUpon().build().equals(uri), "malformed escapes are lenient and builder preserves components");
    check(android.net.Uri.parse("mailto:a b@example.test").isOpaque() && android.net.Uri.parse("relative/a").isRelative(), "opaque and relative StringUri classification");
    check(new android.content.Intent().setType("text/plain").setData(uri).getType() == null
        && new android.content.Intent().setData(uri).setType("text/plain").getData() == null, "Intent data/type setters clear the other component");
  }
  private static void intentMatching() throws Exception {
    var type = new android.content.IntentFilter("view", "image/*");
    check(type.matchData("image/png", "content", android.net.Uri.parse("content://media/a")) >= 0
        && type.matchData("image/*", "file", android.net.Uri.parse("file:///a")) >= 0, "MIME-only filters accept content and file schemes");
    check(type.matchData("image/png", "https", android.net.Uri.parse("https://example.test/a")) == -2
        && type.matchData("text/plain", null, null) == -1, "MIME filters distinguish data and type mismatches");
    var web = new android.content.IntentFilter("view");
    web.addDataScheme("https"); web.addDataAuthority("*.example.test", "443"); web.addDataPath("/assets/", PatternMatcher.PATTERN_PREFIX);
    check(web.match("view", null, "https", android.net.Uri.parse("https://A.EXAMPLE.test:443/assets/a%20b"), null, "test") >= 0,
        "authority wildcard/port and decoded path matching");
    check(web.matchData(null, "https", android.net.Uri.parse("https://a.example.test/assets/a")) == -2
        && web.matchData(null, "https", android.net.Uri.parse("https://a.example.test:443/other")) == -2, "authority and path reject mismatches");
    check(web.match("other", null, "https", android.net.Uri.parse("https://a.example.test:443/assets/a"), null, "test") == -3
        && web.match("view", null, "https", android.net.Uri.parse("https://a.example.test:443/assets/a"), Set.of("extra"), "test") == -4,
        "intent action and category rejection codes");
    var mail = new android.content.IntentFilter("view"); mail.addDataScheme("mailto"); mail.addDataSchemeSpecificPart("user@", PatternMatcher.PATTERN_PREFIX);
    check(mail.matchData(null, "mailto", android.net.Uri.parse("mailto:user@example.test")) >= 0
        && mail.matchData(null, "mailto", android.net.Uri.parse("mailto:other@example.test")) == -2, "opaque scheme-specific matching");
    check(new PatternMatcher("/a.*", PatternMatcher.PATTERN_SIMPLE_GLOB).match("/abc")
        && new PatternMatcher("/[a-c]{2,3}", PatternMatcher.PATTERN_ADVANCED_GLOB).match("/abc")
        && !new PatternMatcher("/a*a", PatternMatcher.PATTERN_ADVANCED_GLOB).match("/aaa"), "Android glob matching is nonbacktracking");
    var labelManifest = manifestNode("manifest", "package", "test.identity");
    var labelApp = manifestNode("application", "label", "Cook\u2019s \u4e16\u754c");
    labelManifest.children.add(labelApp);
    check(ApplicationIdentity.label(labelManifest, id -> null).equals("Cook\u2019s \u4e16\u754c"), "literal APK label");
    labelApp.attributes.put("label", new BinaryResources.Value(1, 1, null));
    check(ApplicationIdentity.label(labelManifest, id -> id == 1 ? new BinaryResources.Value(1, 2, null)
        : new BinaryResources.Value(3, 0, "Resource label")).equals("Resource label"), "label resource aliases");
    labelApp.attributes.clear();
    check(ApplicationIdentity.label(labelManifest, id -> null).equals("test.identity"), "missing label uses package");
    var manifest = manifestNode("manifest", "package", "test.resolve");
    var app = manifestNode("application"); manifest.children.add(app);
    for (String name : List.of(".Disabled", ".Enabled")) {
      var activity = manifestNode("activity", "name", name);
      if (name.equals(".Disabled")) activity.attributes.put("enabled", new BinaryResources.Value(18, 0, null));
      var filter = manifestNode("intent-filter");
      filter.children.add(manifestNode("action", "name", android.content.Intent.ACTION_MAIN));
      filter.children.add(manifestNode("category", "name", android.content.Intent.CATEGORY_LAUNCHER));
      activity.children.add(filter); app.children.add(activity);
    }
    var receiver = manifestNode("activity", "name", ".Images");
    var filter = manifestNode("intent-filter");
    filter.children.add(manifestNode("action", "name", "view"));
    filter.children.add(manifestNode("category", "name", "android.intent.category.DEFAULT"));
    filter.children.add(manifestNode("data", "mimeType", "image/*", "scheme", "https", "host", "example.test", "pathPrefix", "/images/"));
    receiver.children.add(filter); app.children.add(receiver);
    var model = new PackageModel(manifest); var pm = new android.content.pm.PackageManager(model);
    check(model.launcher.equals("test.resolve.Enabled") && pm.getLaunchIntentForPackage("test.resolve").getComponent().getClassName().equals(model.launcher),
        "launcher and package resolver share enabled activity selection");
    var intent = new android.content.Intent("view").setDataAndType(android.net.Uri.parse("https://example.test/images/a.png"), "image/png");
    check(pm.queryIntentActivities(intent, android.content.pm.PackageManager.MATCH_DEFAULT_ONLY).get(0).activityInfo.name.equals("test.resolve.Images"),
        "manifest MIME and URI constraints are resolved together");
    check(pm.queryIntentActivities(intent.setType("text/plain"), 0).isEmpty(), "manifest resolver rejects mismatched MIME");
    var alias = manifestNode("activity-alias", "name", ".Alias", "targetActivity", ".Enabled");
    var aliasFilter = manifestNode("intent-filter");
    aliasFilter.children.add(manifestNode("action", "name", android.content.Intent.ACTION_MAIN));
    aliasFilter.children.add(manifestNode("category", "name", "android.intent.category.INFO"));
    alias.children.add(aliasFilter); app.children.add(alias);
    var aliasModel = new PackageModel(manifest);
    check(aliasModel.launchIntent().getComponent().getClassName().equals("test.resolve.Alias")
        && aliasModel.launcher.equals("test.resolve.Enabled")
        && aliasModel.resolveActivities(aliasModel.launchIntent(), 0).get(0).activityInfo.targetActivity.equals("test.resolve.Enabled"),
        "launcher aliases retain their component identity and executable target");
  }
  private static void manifestResolutionRegressions() throws Exception {
    var manifest = manifestNode("manifest", "package", "test.filters");
    var app = manifestNode("application"); manifest.children.add(app);
    var activity = manifestNode("activity", "name", ".Main"); app.children.add(activity);
    for (String scheme : List.of("fb123", "fbconnect")) {
      var filter = manifestNode("intent-filter");
      filter.children.add(manifestNode("action", "name", "view"));
      filter.children.add(manifestNode("data", "scheme", scheme, "host", "cct.test"));
      activity.children.add(filter);
    }
    var model = new PackageModel(manifest);
    for (String scheme : List.of("fb123", "fbconnect"))
      check(model.resolveActivities(new android.content.Intent("view", android.net.Uri.parse(scheme + "://cct.test")), 0).size() == 1,
          "value-equal filters keep independent data matchers: " + scheme);
    activity.attributes.put("theme", new BinaryResources.Value(1, 0x7f010001, null));
    activity.attributes.put("screenOrientation", new BinaryResources.Value(16, 6, null));
    activity.attributes.put("configChanges", new BinaryResources.Value(16, 0x480, null));
    activity.attributes.put("parentActivityName", new BinaryResources.Value(3, 0, ".Parent"));
    var alias = manifestNode("activity-alias", "name", ".Alias", "targetActivity", ".Main");
    alias.attributes.put("theme", new BinaryResources.Value(1, 0x7f010002, null));
    app.children.add(alias);
    var aliasInfo = new PackageModel(manifest).activities.get(1);
    check(aliasInfo.theme == 0x7f010001 && aliasInfo.screenOrientation == 6 && aliasInfo.configChanges == 0x480
        && aliasInfo.parentActivityName.equals("test.filters.Parent"), "alias inherits target properties, ignoring undeclared alias theme");
    alias.attributes.put("parentActivityName", new BinaryResources.Value(3, 0, ".AliasParent"));
    check(new PackageModel(manifest).activities.get(1).parentActivityName.equals("test.filters.AliasParent"), "alias can override parent activity");

    // A non-privileged manifest cannot elevate its activity priority above zero.
    app.children.clear();
    for (String name : List.of(".First", ".Second")) {
      var candidate = manifestNode("activity", "name", name); app.children.add(candidate);
      var filter = manifestNode("intent-filter"); candidate.children.add(filter);
      filter.children.add(manifestNode("action", "name", android.content.Intent.ACTION_MAIN));
      filter.children.add(manifestNode("category", "name", android.content.Intent.CATEGORY_LAUNCHER));
      filter.attributes.put("priority", new BinaryResources.Value(16, name.equals(".Second") ? 10 : 0, null));
    }
    model = new PackageModel(manifest);
    check(model.launcher.equals("test.filters.First") && model.activityFilters.get(1).priority() == 0, "positive activity priority is capped");
    app.children.get(1).children.get(0).children.add(manifestNode("category", "name", "android.intent.category.DEFAULT"));
    model = new PackageModel(manifest);
    check(model.launcher.equals("test.filters.Second"), "default filter wins equal priority before match specificity");
    app.children.get(1).children.get(0).attributes.put("priority", new BinaryResources.Value(16, -1, null));
    check(new PackageModel(manifest).launcher.equals("test.filters.First"), "negative priority remains below zero");
    var pm = new android.content.pm.PackageManager(model);
    for (var intent : List.of(new android.content.Intent(), new android.content.Intent("missing"),
        new android.content.Intent().setPackage("absent"), new android.content.Intent().setClassName("test.filters", "test.filters.First"),
        new android.content.Intent().setClassName("test.filters", "test.filters.Absent"),
        new android.content.Intent(android.content.Intent.ACTION_MAIN).addCategory(android.content.Intent.CATEGORY_LAUNCHER))) {
      var result = pm.queryIntentActivities(intent, 0);
      int size = result.size();
      result.sort(Comparator.comparing(r -> r.activityInfo.name));
      result.add(new android.content.pm.ResolveInfo()); result.clear();
      check(pm.queryIntentActivities(intent, 0).size() == size, "every activity query returns an independent mutable list");
    }

    app.children.clear(); app.children.add(activity); activity.children.clear();
    var filter = manifestNode("intent-filter"); activity.children.add(filter);
    filter.children.add(manifestNode("action", "name", "view"));
    var data = manifestNode("data"); filter.children.add(data);
    String[] keys = {"mimeType", "scheme", "host", "port", "pathPrefix"};
    String[] values = {"image/*", "https", "example.test", "443", "/images/"};
    Map<Integer, BinaryResources.Value> resources = new HashMap<>();
    for (int i = 0; i < keys.length; i++) {
      int id = 0x7f020000 + i;
      data.attributes.put(keys[i], new BinaryResources.Value(1, id, null));
      resources.put(id, new BinaryResources.Value(3, 0, values[i]));
    }
    resources.put(0x7f020005, resources.get(0x7f020000));
    resources.put(0x7f020000, new BinaryResources.Value(1, 0x7f020005, null));
    var image = new android.content.Intent("view").setDataAndType(android.net.Uri.parse("https://example.test:443/images/a.png"), "image/png");
    check(new PackageModel(manifest, resources::get).resolveActivities(image, 0).size() == 1,
        "manifest resolves data resource references, including reference chains");
    check(new PackageModel(manifest, resources::get).resolveActivities(
        new android.content.Intent("view").setDataAndType(android.net.Uri.parse("https://example.test:444/images/a.png"), "image/png"), 0).isEmpty(),
        "resolved data port still constrains matching");
    // ResourceTable's configuration guard must reject locale-dependent data,
    // rather than silently choosing this host's locale for an intent filter.
    var table = new ResourceTable(null);
    var entriesField = ResourceTable.class.getDeclaredField("entries"); entriesField.setAccessible(true);
    @SuppressWarnings("unchecked")
    var entries = (Map<Integer, List<ResourceTable.Entry>>) entriesField.get(table);
    byte[] plain = new byte[64]; plain[0] = 64;
    entries.put(1, List.of(new ResourceTable.Entry("test", "string", "scheme",
        new BinaryResources.Value(3, 0, "https"), 0, Map.of(), plain)));
    check(table.nonConfigurationValue(1).text().equals("https"), "unqualified manifest resource is available before RuntimeEnvironment initialization");
    byte[] localized = plain.clone(); localized[8] = 'e'; localized[9] = 'n';
    entries.put(2, List.of(new ResourceTable.Entry("test", "string", "scheme",
        new BinaryResources.Value(3, 0, "https"), 0, Map.of(), localized)));
    boolean qualified = false;
    try { table.nonConfigurationValue(2); } catch (IllegalArgumentException expected) { qualified = true; }
    check(qualified, "configuration-dependent manifest resource is rejected");
    var hostField = RuntimeEnvironment.class.getDeclaredField("host"); hostField.setAccessible(true);
    Object oldHost = hostField.get(null);
    var warnings = new ArrayList<String>();
    hostField.set(null, java.lang.reflect.Proxy.newProxyInstance(Host.class.getClassLoader(), new Class<?>[]{Host.class}, (p, method, args) -> {
      if (method.getName().equals("log")) { warnings.add((String) args[2]); return null; }
      throw new UnsupportedOperationException();
    }));
    try {
      for (var invalid : List.of(new BinaryResources.Value(1, 0x7fffffff, null), new BinaryResources.Value(2, 123, null),
          new BinaryResources.Value(3, 0, "secret-invalid-mime"))) {
        data.attributes.put("mimeType", invalid);
        var partial = new PackageModel(manifest, resources::get);
        check(partial.activities.size() == 1 && partial.activityFilters.isEmpty(), "bad manifest data skips entire filter without aborting package startup");
      }
      data.attributes.put("mimeType", new BinaryResources.Value(3, 0, "image/*"));
      data.attributes.put("scheme", new BinaryResources.Value(1, 2, null));
      check(new PackageModel(manifest, table::nonConfigurationValue).activityFilters.isEmpty(), "unresolvable configuration skips whole filter");
      data.attributes.put("scheme", new BinaryResources.Value(1, 0x7f020001, null));
      data.attributes.put("port", new BinaryResources.Value(3, 0, "secret-invalid-port"));
      check(new PackageModel(manifest, resources::get).activityFilters.isEmpty(), "malformed literal port cannot abort startup");
      check(warnings.size() == 5 && warnings.stream().noneMatch(s -> s.contains("secret")), "skipped filters warn without logging data values");
    } finally { hostField.set(null, oldHost); }
  }
  // GameAnalytics reads query rows through SQLiteCursor's CursorWindow types.
  // Dynamic receivers are process-wide (ActivityManagerService): any Context's
  // broadcast reaches them, on the registration Handler, with the owner Context.
  private static void broadcasts() throws Exception {
    var thread = new HandlerThread("receiver");
    thread.start();
    var owner = new android.content.Context();
    var sender = new android.content.Context();
    var received = new CopyOnWriteArrayList<Object>();
    var delivered = new CountDownLatch(1);
    var receiver = new android.content.BroadcastReceiver() {
      public void onReceive(android.content.Context context, android.content.Intent intent) {
        received.addAll(List.of(context, Looper.myLooper(), intent.getAction()));
        delivered.countDown();
      }
    };
    owner.registerReceiver(receiver, new android.content.IntentFilter("test.PROCESS_WIDE"), null,
        new Handler(thread.getLooper()));
    sender.sendBroadcast(new android.content.Intent("test.PROCESS_WIDE"));
    check(delivered.await(2, TimeUnit.SECONDS) && received.equals(List.of(owner, thread.getLooper(), "test.PROCESS_WIDE")),
        "a broadcast from another Context runs onReceive on the registration Handler with the registering Context");
    boolean foreign = false;
    try { sender.unregisterReceiver(receiver); } catch (IllegalArgumentException expected) { foreign = true; }
    check(foreign, "only the registering Context unregisters its receiver");
    owner.unregisterReceiver(receiver);
    var handler = new Handler(thread.getLooper());
    var actions = new CopyOnWriteArrayList<String>();
    var blocked = new CountDownLatch(1);
    var resume = new CountDownLatch(1);
    var multi = new android.content.BroadcastReceiver() {
      public void onReceive(android.content.Context context, android.content.Intent intent) {
        actions.add(intent.getAction());
      }
    };
    try {
      handler.post(() -> {
        blocked.countDown();
        try { resume.await(); } catch (InterruptedException e) { throw new AssertionError(e); }
      });
      check(blocked.await(2, TimeUnit.SECONDS), "receiver queue is deterministically blocked");
      var first = new android.content.IntentFilter("test.FIRST");
      owner.registerReceiver(multi, first, null, handler);
      first.addAction("test.MUTATED");
      var second = new android.content.IntentFilter("test.SECOND");
      second.addAction("test.FIRST");
      owner.registerReceiver(multi, second, null, handler);
      sender.sendBroadcast(new android.content.Intent("test.FIRST"));
      sender.sendBroadcast(new android.content.Intent("test.SECOND"));
      sender.sendBroadcast(new android.content.Intent("test.MUTATED"));
      boolean differentHandler = false;
      try { owner.registerReceiver(multi, first, null, new Handler(thread.getLooper())); }
      catch (IllegalStateException expected) { differentHandler = true; }
      check(differentHandler, "repeat registration validates Handler identity");
      var drained = new CountDownLatch(1);
      handler.post(drained::countDown);
      resume.countDown();
      check(drained.await(2, TimeUnit.SECONDS) && actions.equals(List.of("test.FIRST", "test.SECOND")),
          "all registered filter snapshots survive and overlapping filters deliver once");
      owner.unregisterReceiver(multi);
      actions.clear();

      var blockedAgain = new CountDownLatch(1);
      var resumeAgain = new CountDownLatch(1);
      handler.post(() -> {
        blockedAgain.countDown();
        try { resumeAgain.await(); } catch (InterruptedException e) { throw new AssertionError(e); }
      });
      try {
        check(blockedAgain.await(2, TimeUnit.SECONDS), "cancellation queue is blocked");
        owner.registerReceiver(multi, new android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED), null, handler);
        owner.registerReceiver(multi, new android.content.IntentFilter("test.OLD"), null, handler);
        sender.sendBroadcast(new android.content.Intent("test.OLD"));
        owner.unregisterReceiver(multi);
        // The same Java receiver starts a new lifetime. Old sticky and normal
        // deliveries must not be revived by this registration.
        owner.registerReceiver(multi, new android.content.IntentFilter("test.NEW"), null, handler);
        sender.sendBroadcast(new android.content.Intent("test.OLD"));
        sender.sendBroadcast(new android.content.Intent("test.NEW"));
        var cancelled = new CountDownLatch(1);
        handler.post(cancelled::countDown);
        resumeAgain.countDown();
        check(cancelled.await(2, TimeUnit.SECONDS) && actions.equals(List.of("test.NEW")),
            "unregister forgets queued sticky and ordinary broadcasts across re-registration");
        owner.unregisterReceiver(multi);
        boolean twice = false;
        try { owner.unregisterReceiver(multi); } catch (IllegalArgumentException expected) { twice = true; }
        check(twice, "one unregister removes every filter and a second unregister fails");
      } finally { resumeAgain.countDown(); }
    } finally {
      resume.countDown();
      thread.quitSafely();
      thread.join(5000);
    }
  }
  // Process-local system services: PendingIntent kinds, alarms, notifications, OBB storage.
  private static void platformServices(android.content.Context context) throws Exception {
    var thread = new HandlerThread("platform-services");
    thread.start();
    var handler = new Handler(thread.getLooper());
    var actions = new CopyOnWriteArrayList<String>();
    var asyncResults = new CopyOnWriteArrayList<BroadcastReceiverResult>();
    var delivered = new CountDownLatch(2);
    var receiver = new android.content.BroadcastReceiver() {
      public void onReceive(android.content.Context c, android.content.Intent intent) {
        actions.add(intent.getAction());
        setResultCode(7);
        var pending = goAsync();
        asyncResults.add(new BroadcastReceiverResult(pending, pending.getResultCode()));
        pending.finish();
        delivered.countDown();
      }
    };
    context.registerReceiver(receiver, new android.content.IntentFilter("test.ALARM"), null, handler);
    var alarm = android.app.PendingIntent.getBroadcast(context, 3, new android.content.Intent("test.ALARM"),
        android.app.PendingIntent.FLAG_IMMUTABLE);
    check(alarm.isBroadcast() && !alarm.isActivity() && android.app.PendingIntent.getBroadcast(context, 3,
        new android.content.Intent("test.ALARM"), android.app.PendingIntent.FLAG_IMMUTABLE | android.app.PendingIntent.FLAG_NO_CREATE) == alarm,
        "broadcast PendingIntent tokens are kept by kind and filter identity");
    check(android.app.PendingIntent.getActivity(context, 3, new android.content.Intent("test.ALARM"),
        android.app.PendingIntent.FLAG_IMMUTABLE | android.app.PendingIntent.FLAG_NO_CREATE) == null,
        "an activity token is distinct from a broadcast token for the same Intent");
    alarm.send();
    var alarms = (android.app.AlarmManager) context.getSystemService(android.content.Context.ALARM_SERVICE);
    // The suite's main Looper has finished, so the alarm runs on this test Handler.
    alarms.setExact(android.app.AlarmManager.ELAPSED_REALTIME_WAKEUP, SystemClock.elapsedRealtime() + 20, "send", () -> {
      try { alarm.send(); } catch (android.app.PendingIntent.CanceledException e) { throw new AssertionError(e); }
    }, handler);
    check(delivered.await(2, TimeUnit.SECONDS) && actions.equals(List.of("test.ALARM", "test.ALARM")),
        "a broadcast PendingIntent is delivered when sent and when its alarm fires");
    check(asyncResults.size() == 2 && asyncResults.get(0).code() == 7, "goAsync hands over the pending broadcast result");
    boolean finishedTwice = false;
    try { asyncResults.get(0).result().finish(); } catch (IllegalStateException expected) { finishedTwice = true; }
    check(finishedTwice, "a pending broadcast result finishes once");
    var listenerFired = new CountDownLatch(1);
    var replaced = new java.util.concurrent.atomic.AtomicInteger();
    android.app.AlarmManager.OnAlarmListener listener = () -> { replaced.incrementAndGet(); listenerFired.countDown(); };
    alarms.set(android.app.AlarmManager.ELAPSED_REALTIME, SystemClock.elapsedRealtime() + 60_000, "late", listener, handler);
    alarms.set(android.app.AlarmManager.ELAPSED_REALTIME, SystemClock.elapsedRealtime() + 10, "soon", listener, handler);
    check(listenerFired.await(2, TimeUnit.SECONDS) && replaced.get() == 1, "setting a listener alarm again replaces it");
    var cancelledFired = new java.util.concurrent.atomic.AtomicBoolean();
    android.app.AlarmManager.OnAlarmListener cancelled = () -> cancelledFired.set(true);
    alarms.set(android.app.AlarmManager.RTC, System.currentTimeMillis() + 20, "cancel", cancelled, handler);
    alarms.cancel(cancelled);
    Thread.sleep(80);
    var drained = new CountDownLatch(1);
    handler.post(drained::countDown);
    check(drained.await(2, TimeUnit.SECONDS) && !cancelledFired.get(), "a cancelled alarm does not fire");
    context.unregisterReceiver(receiver);
    thread.quitSafely();

    var notifications = (android.app.NotificationManager) context.getSystemService(android.content.Context.NOTIFICATION_SERVICE);
    var channel = new android.app.NotificationChannel("emulation", "Emulation", android.app.NotificationManager.IMPORTANCE_LOW);
    channel.setDescription("first");
    notifications.createNotificationChannel(channel);
    notifications.createNotificationChannel(new android.app.NotificationChannel("emulation", "Running",
        android.app.NotificationManager.IMPORTANCE_HIGH));
    var stored = notifications.getNotificationChannel("emulation");
    check(stored != null && stored.getName().equals("Running") && stored.getImportance() == android.app.NotificationManager.IMPORTANCE_LOW,
        "recreating a channel updates its name but cannot raise its importance");
    check(notifications.areNotificationsEnabled() && notifications.getImportance() == android.app.NotificationManager.IMPORTANCE_DEFAULT,
        "the app's notifications are enabled");
    var action = new android.app.Notification.Action.Builder(
        android.graphics.drawable.Icon.createWithResource(context, 5), "Pause", alarm).build();
    var notification = new android.app.Notification.Builder(context, "emulation").addAction(action)
        .setProgress(10, 4, false).setGroupSummary(true).build();
    check(notification.actions.length == 1 && notification.actions[0].icon == 5 && notification.actions[0].actionIntent == alarm
        && notification.extras.getInt("android.progressMax") == 10
        && (notification.flags & android.app.Notification.FLAG_GROUP_SUMMARY) != 0,
        "a built notification carries its actions, progress and flags");
    notifications.notify(1, notification);
    notifications.cancel(1);

    var storage = (android.os.storage.StorageManager) context.getSystemService(android.content.Context.STORAGE_SERVICE);
    check(storage != null && !storage.isObbMounted("/sdcard/main.obb") && storage.getMountedObbPath("/sdcard/main.obb") == null,
        "no OBB image is mounted");
    boolean missingObb = false;
    try { storage.mountObb(Files.createTempDirectory("obb").resolve("absent.obb").toString(), null,
        new android.os.storage.OnObbStateChangeListener() {}); }
    catch (IllegalArgumentException expected) { missingObb = true; }
    check(missingObb, "mounting a missing OBB file is rejected");
    var activities = (android.app.ActivityManager) context.getSystemService(android.content.Context.ACTIVITY_SERVICE);
    check(activities.getHistoricalProcessExitReasons(null, 0, 0).isEmpty(), "no earlier process exits are recorded");
    var attributes = new android.media.AudioAttributes.Builder()
        .setLegacyStreamType(android.media.AudioManager.STREAM_ALARM).build();
    check(attributes.getUsage() == android.media.AudioAttributes.USAGE_ALARM
        && attributes.getVolumeControlStream() == android.media.AudioManager.STREAM_ALARM,
        "a legacy stream type maps to its usage and volume stream");
    check(GuestFiles.child(new java.io.File("C:\\cache"), "C:\\data\\no_backup\\work.lck").equals("\\data\\no_backup\\work.lck")
        && GuestFiles.child(null, "C:\\data").equals("C:\\data") && GuestFiles.child("C:\\cache", "work.lck").equals("work.lck"),
        "an absolute child joins under its parent as on Android");

    var env = RuntimeEnvironment.get();
    String pkg = env.manifest.info.packageName;
    check(RuntimeEnvironment.toHostPath("/storage/emulated/0/Android/obb/" + pkg + "/DRM1.mp4")
            .equals(new java.io.File(env.obb, "DRM1.mp4").getPath())
        && RuntimeEnvironment.toHostPath("/data/user/0/" + pkg + "/cache").equals(env.cache.getPath())
        && RuntimeEnvironment.toHostPath("/storage/emulated/0/Android/obbx")
            .equals(new java.io.File(env.data, "external/Android/obbx").getPath())
        && RuntimeEnvironment.toHostPath("/proc/self/status").equals("/proc/self/status")
        && RuntimeEnvironment.toHostPath("relative/file").equals("relative/file"),
        "Android absolute paths of the application's mounts map to host paths");

    var locales = context.getSystemService(android.app.LocaleManager.class);
    check(locales != null && locales.getApplicationLocales().isEmpty(), "apps follow the system locales by default");
    locales.setApplicationLocales(LocaleList.forLanguageTags("es-ES"));
    check(locales.getApplicationLocales().get(0).getLanguage().equals("es"), "an app locale selection is kept");
    locales.setApplicationLocales(LocaleList.getEmptyLocaleList());

    var flags = new android.util.SparseBooleanArray();
    flags.put(9, true); flags.put(2, false); flags.put(5, true);
    check(flags.size() == 3 && flags.keyAt(0) == 2 && !flags.valueAt(0) && flags.get(9) && !flags.get(7)
        && flags.indexOfValue(true) == 1, "SparseBooleanArray keeps keys sorted");
    var longs = new android.util.SparseLongArray();
    longs.put(3, Long.MAX_VALUE);
    check(longs.get(3) == Long.MAX_VALUE && longs.get(4, -1) == -1, "SparseLongArray");

    var gate = new ConditionVariable();
    check(!gate.block(1), "a closed ConditionVariable times out");
    gate.open();
    check(gate.block(1) && gate.block(0), "an open ConditionVariable passes");

    var audio = (android.media.AudioManager) context.getSystemService(android.content.Context.AUDIO_SERVICE);
    var changes = new CopyOnWriteArrayList<Integer>();
    var focus = new android.media.AudioFocusRequest.Builder(android.media.AudioManager.AUDIOFOCUS_GAIN)
        .setOnAudioFocusChangeListener(changes::add).build();
    check(audio.requestAudioFocus(focus) == android.media.AudioManager.AUDIOFOCUS_REQUEST_GRANTED
        && audio.abandonAudioFocusRequest(focus) == android.media.AudioManager.AUDIOFOCUS_REQUEST_GRANTED
        && audio.generateAudioSessionId() != audio.generateAudioSessionId(), "audio focus requests and session ids");

    var hevc = Arrays.stream(new android.media.MediaCodecList(android.media.MediaCodecList.ALL_CODECS).getCodecInfos())
        .filter(info -> Arrays.asList(info.getSupportedTypes()).contains("video/hevc")).findFirst().orElseThrow();
    var caps = hevc.getCapabilitiesForType("video/hevc");
    check(!hevc.isEncoder() && caps.getVideoCapabilities().isSizeSupported(8192, 4096)
        && Arrays.stream(caps.profileLevels).anyMatch(level ->
            level.profile == android.media.MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10)
        && caps.getAudioCapabilities() == null, "MediaCodecList lists the host HEVC decoder");
    var format = android.media.MediaFormat.createVideoFormat("video/hevc", 8192, 4096);
    format.setFeatureEnabled(android.media.MediaCodecInfo.CodecCapabilities.FEATURE_TunneledPlayback, false);
    check(!format.getFeatureEnabled(android.media.MediaCodecInfo.CodecCapabilities.FEATURE_TunneledPlayback)
        && format.getLong("durationUs", 7) == 7 && format.getFloat("frame-rate", 30f) == 30f, "MediaFormat defaults");
  }
  private record BroadcastReceiverResult(android.content.BroadcastReceiver.PendingResult result, int code) {}
  private static void cancelledBroadcasts() throws Exception {
    var thread = new HandlerThread("cancelled-receiver");
    thread.start();
    var handler = new Handler(thread.getLooper());
    var blocked = new CountDownLatch(1);
    var release = new CountDownLatch(1);
    var drained = new CountDownLatch(1);
    var received = new CopyOnWriteArrayList<String>();
    var owner = new android.content.Context();
    var receiver = new android.content.BroadcastReceiver() {
      public void onReceive(android.content.Context context, android.content.Intent intent) {
        received.add(intent.getAction());
      }
    };
    handler.post(() -> {
      blocked.countDown();
      try { release.await(); } catch (InterruptedException e) { throw new AssertionError(e); }
    });
    try {
      check(blocked.await(5, TimeUnit.SECONDS), "receiver Handler is blocked before deliveries queue");
      var filter = new android.content.IntentFilter("test.CANCELLED");
      filter.addAction(android.content.Intent.ACTION_BATTERY_CHANGED);
      owner.registerReceiver(receiver, filter, null, handler); // queues sticky delivery
      owner.sendBroadcast(new android.content.Intent("test.CANCELLED"));
      owner.unregisterReceiver(receiver);
      // A fresh registration of the same receiver must not revive old work.
      owner.registerReceiver(receiver, new android.content.IntentFilter("test.CANCELLED"), null, handler);
      owner.sendBroadcast(new android.content.Intent("test.CANCELLED"));
      handler.post(drained::countDown);
      release.countDown();
      check(drained.await(5, TimeUnit.SECONDS), "receiver queue drains");
      check(received.equals(List.of("test.CANCELLED")),
          "unregister cancels pending broadcast and sticky work without cancelling a fresh registration");
      owner.unregisterReceiver(receiver);
    } finally {
      release.countDown();
      thread.quitSafely();
      thread.join(5000);
    }
  }
  private static void cursors() {
    var window = new android.database.CursorWindow("rows");
    check(window.setNumColumns(5) && window.allocRow() && window.allocRow(), "window rows allocate");
    check(window.putLong(42, 0, 0) && window.putDouble(1.5, 0, 1) && window.putString(" 12.5e1xyz", 0, 2)
        && window.putBlob(new byte[]{1, 2}, 0, 3) && window.putNull(0, 4), "window fields store");
    check(!window.putLong(1, 2, 0) && !window.putLong(1, 0, 5) && !window.setNumColumns(4), "window bounds hold");
    check(window.getType(0, 0) == android.database.Cursor.FIELD_TYPE_INTEGER
        && window.getType(0, 1) == android.database.Cursor.FIELD_TYPE_FLOAT
        && window.getType(0, 2) == android.database.Cursor.FIELD_TYPE_STRING
        && window.getType(0, 3) == android.database.Cursor.FIELD_TYPE_BLOB
        && window.getType(0, 4) == android.database.Cursor.FIELD_TYPE_NULL
        && window.getType(1, 0) == android.database.Cursor.FIELD_TYPE_NULL, "window field types");
    check(window.getString(0, 0).equals("42") && window.getString(0, 1).equals("1.5") && window.getString(0, 4) == null,
        "numbers read as strings like the native window");
    window.putDouble(123456789.0, 1, 1);
    window.putDouble(1e-5, 1, 2);
    window.putDouble(100000.0, 1, 3);
    check(window.getString(1, 1).equals("1.23457e+08") && window.getString(1, 2).equals("1e-05")
        && window.getString(1, 3).equals("100000"), "doubles read as C %g strings");
    check(window.getLong(0, 2) == 12 && window.getDouble(0, 2) == 125.0 && window.getInt(0, 1) == 1
        && window.getLong(0, 4) == 0, "strings parse their numeric prefix, null reads zero");
    // android_database_CursorWindow.cpp nativeGetLong: strtoll(value, NULL, 0) picks the base from the prefix.
    var parsed = new android.database.CursorWindow("strtoll");
    String[] texts = {"0x1F", "010", "08", "-0x10", " \t+0X7fffffffffffffffff", "0x", "-9223372036854775808",
                      "-99999999999999999999", "", "٣"};
    long[] parsedValues = {31, 8, 0, -16, Long.MAX_VALUE, 0, Long.MIN_VALUE, Long.MIN_VALUE, 0, 0};
    check(parsed.setNumColumns(texts.length) && parsed.allocRow(), "strtoll window allocates");
    boolean bases = true;
    for (int i = 0; i < texts.length; ++i) bases &= parsed.putString(texts[i], 0, i) && parsed.getLong(0, i) == parsedValues[i];
    check(bases, "TEXT reads as strtoll base 0: hex, octal, ASCII digits and clamped overflow");
    check(Arrays.equals(window.getBlob(0, 2), " 12.5e1xyz\0".getBytes(java.nio.charset.StandardCharsets.UTF_8)),
        "string blobs keep the stored terminator");
    boolean typed = false;
    try { window.getBlob(0, 0); } catch (android.database.sqlite.SQLiteException expected) { typed = true; }
    check(typed, "integer fields are not blobs");
    boolean blob = false;
    try { window.getLong(0, 3); } catch (android.database.sqlite.SQLiteException expected) { blob = true; }
    check(blob, "blobs do not convert to numbers");
    var rows = window;
    var cursor = new android.database.AbstractWindowedCursor() {
      { mWindow = rows; }
      public int getCount() { return 2; }
      public String[] getColumnNames() { return new String[]{"id", "score", "text", "data", "note"}; }
    };
    boolean before = false;
    try { cursor.getLong(0); } catch (android.database.CursorIndexOutOfBoundsException expected) {
      before = expected.getMessage().equals("Index -1 requested, with a size of 2");
    }
    check(before, "reads before the first row fail with the Android message");
    check(cursor.moveToNext() && cursor.isFirst() && cursor.getLong(0) == 42 && cursor.getType(3) == 4
        && cursor.isNull(4), "cursor reads the current window row");
    check(cursor.getColumnIndex("T.SCORE") == 1 && cursor.getColumnIndex("missing") == -1
        && cursor.getColumnIndexOrThrow("note") == 4, "column lookup ignores table and case");
    check(cursor.moveToNext() && cursor.isLast() && !cursor.moveToNext() && cursor.isAfterLast()
        && cursor.getPosition() == 2 && !cursor.move(-5) && cursor.isBeforeFirst(), "cursor positions clamp");
    cursor.close();
    check(cursor.isClosed() && !cursor.hasWindow(), "closing a cursor releases its window");
  }
  // GameAnalytics (Puzzling Places) samples FPS with an infinitely repeating
  // ValueAnimator started on the main Looper and ends it from the GA thread's posts.
  private static void valueAnimators() throws Exception {
    var offLooper = new java.util.concurrent.atomic.AtomicBoolean();
    var plain = new Thread(() -> {
      try { android.animation.ValueAnimator.ofFloat(0, 1).start(); }
      catch (android.util.AndroidRuntimeException expected) { offLooper.set(true); }
    });
    plain.start();
    plain.join(5000);
    check(offLooper.get(), "animators refuse threads without a Looper");
    var thread = new HandlerThread("animator");
    thread.start();
    var handler = new Handler(thread.getLooper());
    var values = new CopyOnWriteArrayList<Float>();
    var events = new CopyOnWriteArrayList<String>();
    var ended = new CountDownLatch(1);
    var onLooper = new CopyOnWriteArrayList<Boolean>();
    var listener = new android.animation.AnimatorListenerAdapter() {
      public void onAnimationStart(android.animation.Animator a) { events.add("start"); }
      public void onAnimationRepeat(android.animation.Animator a) { events.add("repeat"); }
      public void onAnimationCancel(android.animation.Animator a) { events.add("cancel"); }
      public void onAnimationEnd(android.animation.Animator a) { events.add("end"); ended.countDown(); }
    };
    handler.post(() -> {
      var a = android.animation.ValueAnimator.ofFloat(2, 4).setDuration(60);
      a.setInterpolator(null);
      a.addListener(listener);
      a.addUpdateListener(v -> { values.add((Float) v.getAnimatedValue()); onLooper.add(Looper.myLooper() == thread.getLooper()); });
      a.start();
      check(values.size() == 1 && values.get(0) == 2f && events.equals(List.of("start")) && a.isRunning(),
          "start notifies listeners and applies the initial value synchronously");
    });
    check(ended.await(5, TimeUnit.SECONDS), "finite animator ends on Choreographer frames");
    check(values.size() >= 3 && values.get(values.size() - 1) == 4f && !onLooper.contains(false),
        "frames deliver interpolated values on the starting Looper and finish at the end value");
    boolean ordered = true;
    for (int i = 1; i < values.size(); ++i) ordered &= values.get(i) >= values.get(i - 1);
    check(ordered && events.equals(List.of("start", "end")), "linear values are monotonic and end is notified once");

    var repeats = new CopyOnWriteArrayList<Integer>();
    var excessRepeats = new CopyOnWriteArrayList<Long>();
    var infiniteEnded = new CountDownLatch(1);
    var finalValue = new float[1];
    handler.post(() -> {
      var a = android.animation.ValueAnimator.ofInt(10, 20).setDuration(20);
      a.setRepeatCount(android.animation.ValueAnimator.INFINITE);
      a.addListener(new android.animation.AnimatorListenerAdapter() {
        public void onAnimationRepeat(android.animation.Animator x) { repeats.add(1); }
        public void onAnimationEnd(android.animation.Animator x) {
          finalValue[0] = (Integer) ((android.animation.ValueAnimator) x).getAnimatedValue();
          infiniteEnded.countDown();
        }
      });
      var endRequested = new java.util.concurrent.atomic.AtomicBoolean();
      a.addUpdateListener(v -> {
        // AOSP animateBasedOnTime: at most one onAnimationRepeat per iteration entered.
        if (repeats.size() > v.getCurrentPlayTime() / 20) excessRepeats.add(v.getCurrentPlayTime());
        if (v.getCurrentPlayTime() >= 100 && endRequested.compareAndSet(false, true)) v.end();
      });
      a.start();
    });
    check(infiniteEnded.await(5, TimeUnit.SECONDS) && repeats.size() >= 2 && finalValue[0] == 20f,
        "infinite repetition continues until end(), which applies the end value");
    check(excessRepeats.isEmpty(), "onAnimationRepeat fires once per new iteration, not on every later frame");

    var reversed = new CopyOnWriteArrayList<Float>();
    var reversedTimes = new CopyOnWriteArrayList<Long>();
    var reverseRepeats = new CopyOnWriteArrayList<Long>();
    var reverseEnded = new CountDownLatch(1);
    handler.post(() -> {
      var a = android.animation.ValueAnimator.ofFloat(1).setDuration(50);
      a.setInterpolator(null);
      a.setRepeatCount(1);
      a.setRepeatMode(android.animation.ValueAnimator.REVERSE);
      a.addUpdateListener(v -> { reversed.add((Float) v.getAnimatedValue()); reversedTimes.add(v.getCurrentPlayTime()); });
      a.addListener(new android.animation.AnimatorListenerAdapter() {
        public void onAnimationRepeat(android.animation.Animator x) { reverseRepeats.add(((android.animation.ValueAnimator) x).getCurrentPlayTime()); }
        public void onAnimationEnd(android.animation.Animator x) { reverseEnded.countDown(); }
      });
      a.start();
    });
    check(reverseEnded.await(5, TimeUnit.SECONDS) && reversed.get(0) == 0f
        && reversed.get(reversed.size() - 1) == 0f && Collections.max(reversed) > 0.5f,
        "a single value animates from zero and REVERSE returns to the start");
    boolean reverseTracks = true;
    for (int i = 0; i + 1 < reversed.size(); ++i) {  // the final frame snaps to the start value
      long t = reversedTimes.get(i);
      float expected = t < 50 ? t / 50f : 1f - (t - 50) / 50f;
      reverseTracks &= Math.abs(reversed.get(i) - expected) < 1e-3f;
    }
    check(reverseRepeats.size() == 1 && reverseRepeats.get(0) >= 50 && reversedTimes.get(reversedTimes.size() - 1) >= 100
        && reverseTracks, "a finite REVERSE plays every iteration for its full duration with one repeat between them");

    var frameTimes = new CopyOnWriteArrayList<Long>();
    var duplicateFrames = new CopyOnWriteArrayList<Long>();
    var resumedEnded = new CountDownLatch(1);
    var delayedEnded = new CountDownLatch(1);
    handler.post(() -> {
      var framing = new java.util.concurrent.atomic.AtomicBoolean();
      var a = android.animation.ValueAnimator.ofFloat(0, 1).setDuration(80);
      a.addUpdateListener(v -> {
        if (!framing.get()) return;  // the synchronous start value
        if (!frameTimes.isEmpty() && frameTimes.get(frameTimes.size() - 1) == v.getCurrentPlayTime())
          duplicateFrames.add(v.getCurrentPlayTime());
        frameTimes.add(v.getCurrentPlayTime());
      });
      a.addListener(new android.animation.AnimatorListenerAdapter() {
        public void onAnimationEnd(android.animation.Animator x) { resumedEnded.countDown(); }
      });
      a.start();
      a.pause();
      a.resume();  // start()'s frame callback is still pending
      framing.set(true);
      var delayed = android.animation.ValueAnimator.ofFloat(0, 1).setDuration(20);
      delayed.setStartDelay(20);
      delayed.addListener(new android.animation.AnimatorListenerAdapter() {
        public void onAnimationEnd(android.animation.Animator x) { delayedEnded.countDown(); }
      });
      delayed.start();
      delayed.pause();
      handler.postDelayed(delayed::resume, 60);
    });
    check(resumedEnded.await(5, TimeUnit.SECONDS) && frameTimes.size() >= 2 && duplicateFrames.isEmpty(),
        "resume keeps a single frame callback, so each frame advances the animator once");
    check(delayedEnded.await(5, TimeUnit.SECONDS), "an animator paused during its start delay runs after resume");

    var cancelEvents = new CopyOnWriteArrayList<String>();
    var cancelled = new CountDownLatch(1);
    handler.post(() -> {
      var a = android.animation.ValueAnimator.ofFloat(0, 1).setDuration(10_000);
      a.setStartDelay(10_000);
      a.addListener(new android.animation.AnimatorListenerAdapter() {
        public void onAnimationStart(android.animation.Animator x) { cancelEvents.add("start"); }
        public void onAnimationCancel(android.animation.Animator x) { cancelEvents.add("cancel"); }
        public void onAnimationEnd(android.animation.Animator x) { cancelEvents.add("end"); }
      });
      a.start();
      check(!a.isRunning() && a.isStarted() && cancelEvents.isEmpty(), "a start delay defers running and start listeners");
      a.cancel();
      check(!a.isStarted() && cancelEvents.equals(List.of("start", "cancel", "end")),
          "cancel notifies start, cancel and end in AOSP order");
      cancelled.countDown();
    });
    check(cancelled.await(5, TimeUnit.SECONDS), "delayed animator cancellation completes");

    // AOSP 14 AnimationHandler: once the thread's last visible window withdraws its
    // request, infinite animators pause after the background delay (GameAnalytics' FPS
    // sampler in a stopped Activity); finite ones finish; a visible window resumes them.
    long savedDelay = android.animation.Animator.getBackgroundPauseDelay();
    android.animation.Animator.setBackgroundPauseDelay(50);
    var window = new Object();
    var infiniteFrames = new java.util.concurrent.atomic.AtomicInteger();
    var finiteEnded = new CountDownLatch(1);
    var bg = new android.animation.ValueAnimator[2];
    var visibleRun = new CountDownLatch(1);
    handler.post(() -> {
      android.animation.AnimationHandler.requestAnimatorsEnabled(true, window);
      bg[0] = android.animation.ValueAnimator.ofFloat(0, 1).setDuration(30);
      bg[0].setRepeatCount(android.animation.ValueAnimator.INFINITE);
      bg[0].addUpdateListener(v -> infiniteFrames.incrementAndGet());
      bg[0].start();
      bg[1] = android.animation.ValueAnimator.ofFloat(0, 1).setDuration(400);
      bg[1].addListener(new android.animation.AnimatorListenerAdapter() {
        public void onAnimationEnd(android.animation.Animator x) { finiteEnded.countDown(); }
      });
      bg[1].start();
      check(bg[0].getTotalDuration() == android.animation.Animator.DURATION_INFINITE && bg[1].getTotalDuration() == 400,
          "infinite repeat reports DURATION_INFINITE total duration");
      visibleRun.countDown();
    });
    check(visibleRun.await(5, TimeUnit.SECONDS), "background-pause animators start");
    Thread.sleep(150);
    check(infiniteFrames.get() > 1, "a visible window's infinite animator runs");
    var hidden = new CountDownLatch(1);
    handler.post(() -> { android.animation.AnimationHandler.requestAnimatorsEnabled(false, window); hidden.countDown(); });
    check(hidden.await(5, TimeUnit.SECONDS), "window visibility withdrawn");
    Thread.sleep(300);
    var pausedState = new boolean[2];
    var probed = new CountDownLatch(1);
    handler.post(() -> { pausedState[0] = bg[0].isPaused(); pausedState[1] = bg[1].isPaused(); probed.countDown(); });
    check(probed.await(5, TimeUnit.SECONDS) && pausedState[0] && !pausedState[1],
        "only the infinite animator pauses after the background delay");
    int framesWhilePaused = infiniteFrames.get();
    check(finiteEnded.await(5, TimeUnit.SECONDS), "a finite animator still finishes in the background");
    Thread.sleep(150);
    check(infiniteFrames.get() == framesWhilePaused, "a background-paused animator requests no frames");
    var shown = new CountDownLatch(1);
    handler.post(() -> { android.animation.AnimationHandler.requestAnimatorsEnabled(true, window); shown.countDown(); });
    check(shown.await(5, TimeUnit.SECONDS), "window visible again");
    Thread.sleep(150);
    check(infiniteFrames.get() > framesWhilePaused, "a visible window resumes the background-paused animator");
    var stopped = new CountDownLatch(1);
    handler.post(() -> { bg[0].cancel(); android.animation.AnimationHandler.requestAnimatorsEnabled(false, window); stopped.countDown(); });
    check(stopped.await(5, TimeUnit.SECONDS), "background-pause fixture cleanup");
    android.animation.Animator.setBackgroundPauseDelay(savedDelay);
    thread.quitSafely();
    thread.join(5000);

    var stat = new StatFs(System.getProperty("java.io.tmpdir"));
    check(stat.getBlockSizeLong() > 0 && stat.getTotalBytes() >= stat.getFreeBytes()
        && stat.getFreeBytes() >= stat.getAvailableBytes() && stat.getAvailableBytes() >= 0
        && stat.getBlockCountLong() * stat.getBlockSizeLong() == stat.getTotalBytes(),
        "StatFs reports consistent block geometry and byte totals for an existing path");
    boolean badPath = false;
    try { new StatFs(Path.of(System.getProperty("java.io.tmpdir"), "venture-missing-statfs", "x").toString()); }
    catch (IllegalArgumentException expected) { badPath = true; }
    check(badPath, "StatFs of a missing path throws IllegalArgumentException");
    // GameAnalytics' device info: total storage of the data and system volumes, process CPU time.
    check(RuntimeEnvironment.toGuestPath(Environment.getDataDirectory().toString()).equals("/data")
        && new StatFs(Environment.getDataDirectory().getAbsolutePath()).getTotalBytes() > 0
        && new StatFs(Environment.getRootDirectory().getAbsolutePath()).getTotalBytes() > 0,
        "data and root directories are existing host volumes");
    long cpuBefore = android.os.Process.getElapsedCpuTime();
    long spin = System.nanoTime(); double sink = 0;
    while (System.nanoTime() - spin < 50_000_000L) sink += Math.sqrt(sink + 1);
    check(cpuBefore >= 0 && android.os.Process.getElapsedCpuTime() >= cpuBefore && sink > 0, "process CPU time is monotonic milliseconds");
    var memoryHostField = RuntimeEnvironment.class.getDeclaredField("host"); memoryHostField.setAccessible(true);
    Object memoryOldHost = memoryHostField.get(null);
    memoryHostField.set(null, java.lang.reflect.Proxy.newProxyInstance(Host.class.getClassLoader(), new Class<?>[]{Host.class},
        (p, method, args) -> {
          if (method.getName().equals("processMemory")) return new long[]{4096, 1024};
          if (method.getName().equals("audioOutputProperties")) return new int[]{0, 0};  // no output device
          throw new UnsupportedOperationException(method.getName());
        }));
    try {
      check(android.os.Debug.getNativeHeapAllocatedSize() == 4096L * 1024 && android.os.Debug.getNativeHeapSize() == 4096L * 1024
          && android.os.Debug.getNativeHeapFreeSize() == 0 && android.os.Debug.getPss() == 5120,
          "native heap is the unshared resident bytes and PSS is in KiB");
      var memoryInfo = new android.os.Debug.MemoryInfo();
      android.os.Debug.getMemoryInfo(memoryInfo);
      boolean allStats = true;
      for (String summary : new String[]{"java-heap", "native-heap", "code", "stack", "graphics", "private-other", "system", "total-pss", "total-swap"})
        allStats &= memoryInfo.getMemoryStat("summary." + summary) != null;
      check(allStats && memoryInfo.getMemoryStat("summary.native-heap").equals("4096") && memoryInfo.getMemoryStat("summary.system").equals("1024")
          && memoryInfo.getMemoryStat("summary.total-pss").equals("5120") && memoryInfo.getMemoryStat("summary.unknown") == null,
          "memory info has every AOSP summary stat");
      var audioManager = new android.media.AudioManager();
      check("48000".equals(audioManager.getProperty(android.media.AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE))
          && "240".equals(audioManager.getProperty(android.media.AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER)),
          "audio output properties answer without an output device");
      // Photon Voice's echo-cancelling input routes voice chat (Android 12+).
      var voice = new android.media.AudioManager() {
        public android.media.AudioDeviceInfo[] getDevices(int flags) {
          return new android.media.AudioDeviceInfo[]{new android.media.AudioDeviceInfo(7, false, "speaker")};
        }
      };
      voice.setMode(android.media.AudioManager.MODE_IN_COMMUNICATION);
      voice.setMicrophoneMute(true);
      var communication = voice.getAvailableCommunicationDevices();
      check(voice.getMode() == android.media.AudioManager.MODE_IN_COMMUNICATION && voice.isMicrophoneMute()
          && communication.size() == 1 && voice.setCommunicationDevice(communication.get(0))
          && voice.getCommunicationDevice().getId() == 7
          && !voice.setCommunicationDevice(new android.media.AudioDeviceInfo(8, false, "gone")),
          "voice chat sets its mode and communication device");
    } finally {
      memoryHostField.set(null, memoryOldHost);
    }
    long started = android.os.Process.getStartUptimeMillis();
    check(started <= android.os.SystemClock.uptimeMillis() && started == android.os.Process.getStartElapsedRealtime()
        && android.os.Process.is64Bit(), "process start time is on the uptime clock");
    // Unreal Engine 5's GameActivity reads the main thread's priority; its network monitor tags sockets.
    check(android.os.Process.getThreadPriority(0) == android.os.Process.THREAD_PRIORITY_DEFAULT, "threads start at default priority");
    android.os.Process.setThreadPriority(-4);
    check(android.os.Process.getThreadPriority(android.os.Process.myTid()) == -4, "thread priority reads back the last set value");
    android.os.Process.setThreadPriority(0);
    android.net.TrafficStats.setThreadStatsTag(7);
    check(android.net.TrafficStats.getAndSetThreadStatsTag(8) == 7 && android.net.TrafficStats.getThreadStatsTag() == 8, "traffic tags are per thread");
    android.net.TrafficStats.clearThreadStatsTag();
    check(android.net.TrafficStats.getThreadStatsTag() == -1 && android.net.TrafficStats.getTotalRxBytes() == android.net.TrafficStats.UNSUPPORTED,
        "traffic counters are unsupported");
    // Firebase checks androidx UserManagerCompat.isUserUnlocked through the API 23 class lookup.
    var users = new android.content.Context().getSystemService(android.os.UserManager.class);
    check(users != null && users.isUserUnlocked() && !users.isManagedProfile(), "the owner user is unlocked");
    check(new android.content.Context().getSystemService(android.os.PowerManager.class) != null
        && new android.content.Context().getSystemService(String.class) == null, "class service lookup follows service names");
    var wifi = new android.content.Context().getSystemService(android.net.wifi.WifiManager.class);
    var multicast = wifi.createMulticastLock("venture");
    multicast.acquire(); multicast.acquire(); multicast.release();
    boolean stillHeld = multicast.isHeld();
    multicast.release();
    check(wifi.isWifiEnabled() && stillHeld && !multicast.isHeld(), "Wi-Fi multicast locks are reference counted");
    boolean encoderRejected = false, unknownRejected = false;
    try { android.media.MediaCodec.createByCodecName("OMX.google.h264.encoder"); } catch (java.io.IOException e) { encoderRejected = true; }
    try { android.media.MediaCodec.createByCodecName("OMX.example.unknown"); } catch (IllegalArgumentException e) { unknownRejected = true; }
    check(encoderRejected && unknownRejected, "codecs by name decode known formats only");
    // androidx.emoji2's startup initializer subclasses ReplacementSpan.
    android.text.style.ReplacementSpan span = new android.text.style.ReplacementSpan() {
      @Override public int getSize(android.graphics.Paint paint, CharSequence text, int start, int end,
                                   android.graphics.Paint.FontMetricsInt metrics) { return 1; }
      @Override public void draw(android.graphics.Canvas canvas, CharSequence text, int start, int end, float x,
                                 int top, int y, int bottom, android.graphics.Paint paint) { canvas.save(); }
    };
    var canvas = new android.graphics.Canvas();
    span.draw(canvas, "e", 0, 1, 0, 0, 0, 0, null);
    check(span instanceof android.text.style.UpdateAppearance && canvas.getSaveCount() == 2,
        "replacement spans draw on an inert canvas");
  }
  private static void disabledTelemetry() throws Exception {
    // Reflect the exact class and method contracts requested by native clients.
    var loggerClass = Class.forName("com.oculus.os.UnifiedTelemetryLogger");
    var eventClass = Class.forName("com.oculus.os.AnalyticsEvent");
    var get = loggerClass.getMethod("getInstance", android.content.Context.class);
    var report = loggerClass.getMethod("reportEvent", eventClass, boolean.class);
    var extra = eventClass.getMethod("setExtra", String.class, Object.class);
    var logger = get.invoke(null, new android.content.Context());
    check(logger != null && get.getReturnType() == loggerClass
        && report.getReturnType() == void.class && extra.getReturnType() == eventClass,
        "device telemetry method descriptors");
    check(get.invoke(null, new android.content.Context()) == logger,
        "device telemetry logger is shared across contexts");
    var event = eventClass.getConstructor(String.class).newInstance("fixture_event");
    Object payload = new Object() {
      public String toString() { throw new AssertionError("disabled telemetry serialized payload"); }
    };
    check(extra.invoke(event, "fixture_key", payload) == event, "event extras preserve chaining");
    report.invoke(logger, event, false);
    report.invoke(logger, event, true);
  }

  public static void main(String[] args) throws Exception {
    if (args.length == 1 && args[0].equals("--host-quit")) {
      hostQuitFixture();
      return;
    }
    check(AppMain.MENU_QUIT && !AppMain.QUIT_DIAGNOSTICS,
        "menu termination is on and quit diagnostics are off in the default fixture");
    check(AppMain.quitProgress() == 0 && AppMain.quitNativeCount() == 0,
        "default lifecycle has no menu termination admission state before a quit");
    disabledTelemetry();
    var power = new PowerManager();
    check(Float.isNaN(power.getThermalHeadroom(0)) && Float.isNaN(power.getThermalHeadroom(60)),
        "unavailable thermal prediction is NaN");
    var vibrationContext = new android.content.Context();
    var vibrations = (VibratorManager) vibrationContext.getSystemService(android.content.Context.VIBRATOR_MANAGER_SERVICE);
    check(vibrations != null && vibrations == vibrationContext.getSystemService(android.content.Context.VIBRATOR_MANAGER_SERVICE),
        "vibrator manager is a cached context service");
    check(vibrations.getVibratorIds().length == 0 && !vibrations.getVibrator(123).hasVibrator(),
        "absent Android vibrator devices have no IDs or vibration capability");
    check(vibrations.getDefaultVibrator() == vibrationContext.getSystemService(android.content.Context.VIBRATOR_SERVICE)
        && !vibrations.getDefaultVibrator().hasAmplitudeControl(), "default vibrator is the context vibrator");
    var styledEditor = new android.widget.EditText(vibrationContext, null, 0);
    styledEditor.setText("editor");
    styledEditor.setSelection(1, 4);
    styledEditor.getText().replace(1, 4, "X");
    check(styledEditor.getText().toString().equals("eXor") && styledEditor.getContext() == vibrationContext,
        "three-argument EditText construction retains editable text and context");
    // With platform attribute inspection disabled, AOSP does not dereference
    // the style data. AppCompat calls this hook unconditionally on API 29+.
    styledEditor.saveAttributeDataForStyleable(null, null, null, null, 1, 2);
    var attributeSources = styledEditor.getAttributeSourceResourceMap();
    check(attributeSources.isEmpty() && styledEditor.getAttributeResolutionStack(1).length == 0,
        "disabled view attribute inspection exposes empty source and resolution data");
    attributeSources.put(1, 2);
    check(styledEditor.getAttributeSourceResourceMap().isEmpty(), "attribute inspection returns a detached map");
    var keyListener = new android.text.method.KeyListener() {
      public int getInputType() { return 1; }
      public boolean onKeyDown(android.view.View view, android.text.Editable text, int code, android.view.KeyEvent event) {
        text.append('!'); return true;
      }
      public boolean onKeyUp(android.view.View view, android.text.Editable text, int code, android.view.KeyEvent event) { return true; }
      public boolean onKeyOther(android.view.View view, android.text.Editable text, android.view.KeyEvent event) { return false; }
      public void clearMetaKeyState(android.view.View view, android.text.Editable text, int states) {}
    };
    styledEditor.setKeyListener(keyListener);
    check(styledEditor.getKeyListener() == keyListener && styledEditor.getInputType() == 1,
        "custom key listener supplies editor input type");
    check(styledEditor.dispatchKeyEvent(new android.view.KeyEvent(0, 29)) && styledEditor.getText().toString().equals("eXor!"),
        "editor key listener consumes events and edits the actual buffer");
    styledEditor.setKeyListener(null);
    check(styledEditor.getKeyListener() == null && styledEditor.getInputType() == 0, "removing key listener disables key input");
    class CustomEditable extends android.text.SpannableStringBuilder {
      CustomEditable(CharSequence source) { super(source); }
    }
    var factoryInputs = new ArrayList<String>();
    styledEditor.setEditableFactory(new android.text.Editable.Factory() {
      public android.text.Editable newEditable(CharSequence source) {
        factoryInputs.add(source.toString()); return new CustomEditable(source);
      }
    });
    styledEditor.setFilters(new android.text.InputFilter[]{new android.text.InputFilter.LengthFilter(3)});
    styledEditor.setText("longer");
    check(styledEditor.getText() instanceof CustomEditable && styledEditor.getText().toString().equals("lon")
        && factoryInputs.equals(java.util.List.of("eXor!", "lon")),
        "editable factories replace existing buffers and receive filtered replacement content");
    check(!styledEditor.isInEditMode(), "runtime text editor is not an IDE layout preview");
    styledEditor.setFocusable(true);
    styledEditor.setClickable(true);
    styledEditor.setLongClickable(true);
    check(styledEditor.isFocusable() && styledEditor.isClickable() && styledEditor.isLongClickable(),
        "editor focus and click flags can be saved and restored by AppCompat");
    styledEditor.setClickable(false);
    styledEditor.setOnClickListener(view -> {});
    check(styledEditor.isClickable(), "installing a click listener sets the Android clickable flag");
    censusValues();
    intentMatching();
    manifestResolutionRegressions();
    lenientUris();
    var launchUri = new android.content.Intent(android.content.Intent.ACTION_MAIN)
        .setComponent(new android.content.ComponentName("com.Ubisoft.ACNexusVR", "com.unity3d.player.UnityPlayerActivity"))
        .addCategory(android.content.Intent.CATEGORY_LAUNCHER);
    check(launchUri.toUri(0).equals("#Intent;action=android.intent.action.MAIN;category=android.intent.category.LAUNCHER;component=com.Ubisoft.ACNexusVR/com.unity3d.player.UnityPlayerActivity;end"),
        "Unity launch intent is serialized with Android's URI grammar");
    check(new android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse("https://example.test/a%20b?q=x#f"))
              .toUri(android.content.Intent.URI_INTENT_SCHEME)
              .equals("intent://example.test/a%20b?q=x#f#Intent;scheme=https;end"),
        "intent scheme preserves encoded data and elides the default VIEW action");
    check(new android.content.Intent(android.content.Intent.ACTION_MAIN).setPackage("test.pkg")
              .toUri(android.content.Intent.URI_ANDROID_APP_SCHEME).equals("android-app://test.pkg"),
        "android-app URI elides its default package and MAIN action");
    check(new android.content.Intent(android.content.Intent.ACTION_VIEW, android.net.Uri.parse("https://example.test/a?q=x"))
              .setPackage("test.pkg").toUri(android.content.Intent.URI_ANDROID_APP_SCHEME)
              .equals("android-app://test.pkg/https/example.test/a?q=x"), "android-app URI carries encoded data");
    boolean missingUriPackage = false;
    try { new android.content.Intent().toUri(android.content.Intent.URI_ANDROID_APP_SCHEME); }
    catch (IllegalArgumentException expected) { missingUriPackage = true; }
    check(missingUriPackage, "android-app URI requires an explicit package");
    check(new android.content.Intent().setType("text/plain").setFlags(0x10000000)
              .setComponent(new android.content.ComponentName("test.pkg", "test.pkg.Main"))
              .toUri(0).equals("#Intent;type=text/plain;launchFlags=0x10000000;component=test.pkg/.Main;end"),
        "URI carries flags, MIME type and the abbreviated component");
    var extraUri = new android.content.Intent().putExtra("key ;", "caf\u00e9 +").putExtra("array", new int[]{1,2});
    check(extraUri.toUri(0).equals("#Intent;S.key%20%3B=caf%C3%A9%20%2B;end"),
        "URI percent-encodes UTF-8 scalar extras and omits arrays like Android");
    check(android.net.Uri.encode("_-!.~'()* /+").equals("_-!.~'()*%20%2F%2B"),
        "Android URI escaping preserves its safe punctuation");
    check(android.net.Uri.encode(null) == null, "URI encoding preserves null");
    var hostField = RuntimeEnvironment.class.getDeclaredField("host");
    hostField.setAccessible(true);
    Object previousHost = hostField.get(null);
    var networkType = new java.util.concurrent.atomic.AtomicInteger(-2);
    var routeQueries = new java.util.concurrent.atomic.AtomicInteger();
    var changingRoute = new java.util.concurrent.atomic.AtomicBoolean();
    var internetConnected = new java.util.concurrent.atomic.AtomicBoolean(false);
    var networkHost = java.lang.reflect.Proxy.newProxyInstance(Host.class.getClassLoader(),
        new Class<?>[]{Host.class}, (proxy, method, values) -> {
          if (method.getName().equals("activeNetworkType")) {
            int query = routeQueries.getAndIncrement();
            return changingRoute.get() ? (query % 2 == 0 ? 1 : 9) : networkType.get();
          }
          if (method.getName().equals("hasInternetConnectivity")) return internetConnected.get();
          throw new UnsupportedOperationException();
        });
    hostField.set(null, networkHost);
    try {
      var connectivity = new android.net.ConnectivityManager();
      for (int type : new int[]{-1, 17, 0, -2}) {
        networkType.set(type);
        check(connectivity.isActiveNetworkMetered() == (type == 0 || type == -2), "VPN and unknown routes retain Quest unmetered policy: " + type);
        var caps = connectivity.getNetworkCapabilities(connectivity.getActiveNetwork());
        if (caps != null) check(caps.hasCapability(android.net.NetworkCapabilities.NET_CAPABILITY_NOT_METERED) == (type != 0)
            && caps.hasCapability(android.net.NetworkCapabilities.NET_CAPABILITY_NOT_ROAMING) == (type != 0), "metering APIs agree for transport " + type);
      }
      // Exercise two observer ticks deterministically, with the host changing
      // transport on every query. A second enumeration used to lose a tick.
      var tickEvents = new ArrayList<String>();
      var tickCallback = new android.net.ConnectivityManager.NetworkCallback() {
        public void onAvailable(android.net.Network n) { tickEvents.add("available"); }
        public void onLost(android.net.Network n) { tickEvents.add("lost"); }
      };
      var registration = Class.forName("android.net.ConnectivityManager$Registration").getDeclaredConstructor(
          android.net.ConnectivityManager.class, android.net.ConnectivityManager.NetworkCallback.class, android.net.NetworkRequest.class);
      registration.setAccessible(true);
      var tick = (Runnable) registration.newInstance(connectivity, tickCallback, null);
      routeQueries.set(0); changingRoute.set(true);
      tick.run(); tick.run();
      check(routeQueries.get() == 2 && tickEvents.equals(List.of("available", "lost", "available")),
          "observer queries route once per tick and reports replacement in the same tick");
      changingRoute.set(false); networkType.set(-2);
      check(connectivity.getActiveNetworkInfo() == null, "no active host route is disconnected");
      check(connectivity.getAllNetworks().length == 0, "without a host route no networks are listed");
      networkType.set(1);
      var all = connectivity.getAllNetworks();
      check(all.length == 1 && all[0] == connectivity.getActiveNetwork()
          && connectivity.getNetworkCapabilities(all[0]).hasTransport(android.net.NetworkCapabilities.TRANSPORT_WIFI),
          "GameAnalytics' API 28+ network enumeration lists the active route with its transport");
      networkType.set(-2);
      networkType.set(1);
      var wifi = connectivity.getActiveNetworkInfo();
      check(wifi.isConnected() && wifi.isConnectedOrConnecting() && wifi.getType() == 1
          && wifi.getTypeName().equals("WIFI"), "Wi-Fi transport comes from the host snapshot");
      check(wifi.isAvailable() && !connectivity.getNetworkInfo(9).isAvailable()
          && !connectivity.getNetworkInfo(9).isConnected(), "only the active host route is available");
      networkType.set(9);
      check(connectivity.getActiveNetworkInfo().getTypeName().equals("ETHERNET"),
          "network changes are reflected by subsequent queries");
      networkType.set(-2);
      check(connectivity.getActiveNetworkInfo() == null && !connectivity.getNetworkInfo(1).isConnected(),
          "disconnection is reflected without reporting an active network");
      check(wifi.isConnected(), "NetworkInfo retains its original snapshot");
      networkType.set(-1);
      check(connectivity.getActiveNetworkInfo().isConnected()
          && connectivity.getActiveNetworkInfo().getTypeName().equals("UNKNOWN"),
          "unknown connected transport is not fabricated as Wi-Fi");
      check(connectivity.getNetworkInfo(12345) == null, "unknown network type has no inventory entry");
      var changes = new LinkedBlockingQueue<String>();
      var callback = new android.net.ConnectivityManager.NetworkCallback() {
        public void onAvailable(android.net.Network network) { changes.add("available"); }
        public void onLost(android.net.Network network) { changes.add("lost"); }
        public void onCapabilitiesChanged(android.net.Network network, android.net.NetworkCapabilities caps) {
          changes.add(caps.hasTransport(1) ? "wifi" : caps.hasTransport(3) ? "ethernet" : "unknown");
        }
      };
      networkType.set(1);
      connectivity.registerDefaultNetworkCallback(callback);
      try {
        check("available".equals(changes.poll(3, TimeUnit.SECONDS))
            && "wifi".equals(changes.poll(3, TimeUnit.SECONDS)), "network callback receives initial host transport");
        var active = connectivity.getActiveNetwork();
        check(active == connectivity.getActiveNetwork() && connectivity.getNetworkCapabilities(active).hasTransport(1),
            "active network identity is stable within the connection");
        networkType.set(9);
        check("lost".equals(changes.poll(3, TimeUnit.SECONDS))
            && "available".equals(changes.poll(3, TimeUnit.SECONDS))
            && "ethernet".equals(changes.poll(3, TimeUnit.SECONDS)), "network callback reflects transport replacement");
        check(connectivity.getNetworkCapabilities(active) == null, "old network capabilities expire after replacement");
        networkType.set(-2);
        check("lost".equals(changes.poll(3, TimeUnit.SECONDS)), "network callback reflects disconnection");
      } finally { connectivity.unregisterNetworkCallback(callback); }
      networkType.set(1);
      check(changes.poll(1200, TimeUnit.MILLISECONDS) == null, "unregistered callback receives no further updates");
      var builder = new android.net.NetworkRequest.Builder().addTransportType(1);
      var wifiRequest = builder.build();
      builder.addTransportType(3); // Existing request retains its original filter.
      connectivity.registerNetworkCallback(wifiRequest, callback);
      try {
        check("available".equals(changes.poll(3, TimeUnit.SECONDS))
            && "wifi".equals(changes.poll(3, TimeUnit.SECONDS)), "request observes matching host transport");
        networkType.set(9);
        check("lost".equals(changes.poll(3, TimeUnit.SECONDS))
            && changes.poll(1200, TimeUnit.MILLISECONDS) == null,
            "nonmatching transport loses the request without mutating the built filter");
      } finally { connectivity.unregisterNetworkCallback(callback); }
      networkType.set(1);
      var wrathRequest = new android.net.NetworkRequest.Builder().addCapability(12).addCapability(16);
      for (int transport = 0; transport <= 4; transport++) wrathRequest.addTransportType(transport);
      var caps = connectivity.getNetworkCapabilities(connectivity.getActiveNetwork());
      check(caps.hasCapability(12) && !caps.hasCapability(16), "default route has INTERNET independently of validation");
      check(caps.hasCapability(11) && caps.hasCapability(18) && !connectivity.isActiveNetworkMetered(),
          "Quest Wi-Fi is unmetered and not roaming");
      connectivity.registerNetworkCallback(wrathRequest.build(), callback);
      try {
        check(changes.poll(1200, TimeUnit.MILLISECONDS) == null,
            "Wrath request does not receive an unverified Internet connection");
        internetConnected.set(true);
        check("available".equals(changes.poll(3, TimeUnit.SECONDS))
            && "wifi".equals(changes.poll(3, TimeUnit.SECONDS)), "Wrath INTERNET+VALIDATED request receives validated Wi-Fi");
        check(connectivity.getNetworkCapabilities(connectivity.getActiveNetwork()).hasCapability(16)
            && !caps.hasCapability(16), "capability snapshots retain their own validation state");
        internetConnected.set(false);
        check("lost".equals(changes.poll(3, TimeUnit.SECONDS)), "validation loss updates a request without changing transport");
      } finally { connectivity.unregisterNetworkCallback(callback); }
      boolean invalidTransport = false, invalidCapability = false;
      try { new android.net.NetworkRequest.Builder().addTransportType(9); }
      catch (IllegalArgumentException expected) { invalidTransport = true; }
      try { new android.net.NetworkRequest.Builder().addCapability(33); }
      catch (IllegalArgumentException expected) { invalidCapability = true; }
      check(invalidTransport && invalidCapability, "Android 12L request rejects out-of-range filters");
    } finally { hostField.set(null, previousHost); }
    var manifest = manifestNode("manifest", "package", "test.intent");
    var app = manifestNode("application");
    var declaredActivity = manifestNode("activity", "name", ".Main");
    var filter = manifestNode("intent-filter");
    filter.children.add(manifestNode("action", "name", "test.action"));
    filter.children.add(manifestNode("category", "name", "test.category"));
    declaredActivity.children.add(filter);
    app.children.add(declaredActivity);
    manifest.children.add(app);
    var packageManager = new android.content.pm.PackageManager(new PackageModel(manifest));
    String bodyPermission = "com.oculus.permission.BODY_TRACKING";
    check(packageManager.checkPermission(bodyPermission, "test.intent") == -1,
        "undeclared body permission is denied");
    manifest.children.add(manifestNode("uses-permission", "name", bodyPermission));
    manifest.children.add(manifestNode("uses-permission", "name", "com.oculus.permission.EYE_TRACKING"));
    var trackingPermissions = new android.content.pm.PackageManager(new PackageModel(manifest));
    check(trackingPermissions.checkPermission(bodyPermission, "test.intent") == 0
        && trackingPermissions.checkPermission(bodyPermission, "other.package") == -1,
        "declared install-time body permission is granted only to its package");
    check(trackingPermissions.checkPermission("com.oculus.permission.EYE_TRACKING", "test.intent") == -1,
        "body install-time grant does not grant dangerous eye permission");
    check(!trackingPermissions.hasSystemFeature("com.oculus.software.body_tracking"),
        "body permission grant does not fabricate tracking hardware");
    var vulkanVersion = new java.util.concurrent.atomic.AtomicInteger();
    Host featureHost = (Host) java.lang.reflect.Proxy.newProxyInstance(Host.class.getClassLoader(),
        new Class<?>[]{Host.class}, (proxy, method, arguments) -> {
          if (method.getName().equals("vulkanHardwareVersion")) return vulkanVersion.get();
          throw new UnsupportedOperationException(method.getName());
        });
    hostField.set(null, featureHost);
    try {
      String vulkan = android.content.pm.PackageManager.FEATURE_VULKAN_HARDWARE_VERSION;
      String level = android.content.pm.PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL;
      check(!packageManager.hasSystemFeature(vulkan) && !packageManager.hasSystemFeature(level),
          "unavailable Vulkan is absent from feature queries");
      check(Arrays.stream(packageManager.getSystemAvailableFeatures()).noneMatch(f -> vulkan.equals(f.name)),
          "unavailable Vulkan is absent from the feature inventory");
      vulkanVersion.set(0x400003);
      check(packageManager.hasSystemFeature(vulkan, 0x400003)
          && !packageManager.hasSystemFeature(vulkan, 0x401000)
          && packageManager.hasSystemFeature(level, 0) && !packageManager.hasSystemFeature(level, 1),
          "Vulkan feature queries honor API version and hardware level");
      var feature = Arrays.stream(packageManager.getSystemAvailableFeatures())
          .filter(f -> vulkan.equals(f.name)).findFirst().orElseThrow();
      String text = feature.toString();
      int start = text.indexOf("v=") + 2;
      int discovered = Integer.parseInt(text.substring(start, text.indexOf(' ', start)));
      check(discovered == vulkanVersion.get() && discovered > 0x400000,
          "UE4 discovers the Vulkan version through FeatureInfo.toString");
      feature.version = 0;
      check(packageManager.hasSystemFeature(vulkan, vulkanVersion.get()), "feature inventory is a snapshot");
      var glFeature = new android.content.pm.FeatureInfo();
      glFeature.reqGlEsVersion = 0x30002;
      check(glFeature.toString().contains("glEsVers=3.2 fl=0x0"), "unnamed FeatureInfo describes GLES");
    } finally { hostField.set(null, previousHost); }
    var matchingIntent = new android.content.Intent("test.action").addCategory("test.category");
    var sparseInts = new android.util.SparseIntArray(0);
    sparseInts.append(40, 4); sparseInts.append(-10, 1); sparseInts.put(20, 2); sparseInts.put(40, 5);
    check(sparseInts.size() == 3 && sparseInts.keyAt(0) == -10 && sparseInts.valueAt(2) == 5
        && sparseInts.get(30, 7) == 7 && sparseInts.indexOfKey(30) == ~2,
        "sparse integer map keeps sorted keys and insertion-point lookup");
    var sparseCopy = sparseInts.clone();
    sparseCopy.setValueAt(0, 9); sparseCopy.removeAt(1); sparseCopy.delete(40); sparseCopy.delete(999);
    check(sparseCopy.toString().equals("{-10=9}") && sparseInts.toString().equals("{-10=1, 20=2, 40=5}")
        && sparseInts.indexOfValue(5) == 2 && sparseInts.indexOfValue(8) == -1,
        "sparse clone owns arrays and deletion retains indexing");
    sparseCopy.clear(); check(sparseCopy.size() == 0 && sparseCopy.get(-10) == 0, "sparse clear and missing default");
    var componentApp = new android.app.Application() {
      public android.content.Context getApplicationContext() { return this; }
    };
    var componentContext = new android.content.Context() {
      public android.content.Context getApplicationContext() { return componentApp; }
    };
    var componentEvents = new ArrayList<String>();
    var componentCallback = new android.content.ComponentCallbacks2() {
      public void onConfigurationChanged(android.content.res.Configuration config) {
        componentEvents.add("config:" + config.orientation);
      }
      public void onLowMemory() { componentEvents.add("low"); componentContext.unregisterComponentCallbacks(this); }
      public void onTrimMemory(int level) { componentEvents.add("trim:" + level); }
    };
    componentContext.registerComponentCallbacks(componentCallback);
    var componentConfig = new android.content.res.Configuration(); componentConfig.orientation = 2;
    componentApp.onConfigurationChanged(componentConfig); componentApp.onTrimMemory(20);
    componentApp.onLowMemory(); componentApp.onTrimMemory(40);
    check(componentEvents.equals(List.of("config:2", "trim:20", "low")),
        "Context routes component callbacks to Application and supports removal during dispatch");
    check(packageManager.queryIntentActivities(matchingIntent, 0).get(0).activityInfo.name.equals("test.intent.Main"),
        "installed activity resolves by manifest action and category");
    check(packageManager.queryIntentActivities(new android.content.Intent("absent.action"), 0).isEmpty(),
        "uninstalled system handler is absent");
    check(packageManager.queryIntentActivities(matchingIntent,
        android.content.pm.PackageManager.MATCH_DEFAULT_ONLY).isEmpty(), "default-only requires the default category");
    check(packageManager.queryIntentActivities(new android.content.Intent("test.action").addCategory("missing"), 0).isEmpty(),
        "every intent category must be declared by the filter");
    check(packageManager.queryIntentActivities(new android.content.Intent().setClassName("test.intent", "test.intent.Main"),
        android.content.pm.PackageManager.MATCH_DEFAULT_ONLY).size() == 1, "explicit activity bypasses filters");
    check(packageManager.queryIntentActivities(new android.content.Intent("test.action").setPackage("absent.package"), 0).isEmpty(),
        "package restriction cannot resolve another installed APK");
    check(packageManager.queryIntentActivities(new android.content.Intent("test.action", android.net.Uri.parse("https://example.test")), 0).isEmpty(),
        "data-less filters reject URI intents without throwing");
    check(packageManager.queryIntentActivities(new android.content.Intent(), 0).isEmpty(), "global query without action/data/type has no candidates");
    check(packageManager.queryIntentActivities(new android.content.Intent().setPackage("test.intent"), 0).size() == 1,
        "package-scoped query supplies candidates even without an action");
    // JNI descriptors recovered from Epic 6419's IL2CPP AndroidClient.
    var horizon = horizonos.supplement.hzplatformclientcore.HorizonPlatformCore.class;
    var config = horizonos.supplement.hzplatformclientcore.AndroidHorizonPlatformConfig.class;
    check(config.getConstructor(android.content.Context.class, String.class) != null, "Horizon config ABI");
    check(horizon.getConstructor() != null, "Horizon constructor ABI");
    check(horizon.getMethod("setup", config).getReturnType() == void.class, "Horizon setup ABI");
    check(horizon.getMethod("getGlobalSessionId").getReturnType() == long.class, "Horizon global session ABI");
    for (String method : List.of("makeRequest", "makeSession"))
      check(horizon.getMethod(method, String.class, String.class, int.class, String.class, int.class)
          .getReturnType() == long.class, "Horizon request ABI");
    check(horizon.getMethod("getMessageCount", long.class).getReturnType() == int.class, "Horizon count ABI");
    check(horizon.getMethod("popMessage", long.class, boolean.class).getReturnType() == Object.class, "Horizon pop ABI");
    check(horizon.getMethod("stopSession", long.class).getReturnType() == void.class, "Horizon stop ABI");
    var horizonMessage = horizonos.supplement.hzplatformclientcore.HorizonPlatformCore.Message.class;
    check(horizonMessage.getField("requestId").getType() == long.class, "Horizon request ID field ABI");
    for (String field : List.of("statusCode", "cookie"))
      check(horizonMessage.getField(field).getType() == int.class, "Horizon integer field ABI");
    for (String field : List.of("statusMessage", "data"))
      check(horizonMessage.getField(field).getType() == String.class, "Horizon string field ABI");
    check(android.provider.Settings.Global.getInt(null, "vrmode_developer_mode") == 0,
        "VrApi developer services are disabled");
    check(android.provider.Settings.Global.getInt(null, "venture_missing_setting", 17) == 17,
        "missing global setting uses caller default");
    boolean settingMissing = false;
    try { android.provider.Settings.Global.getInt(null, "venture_missing_setting"); }
    catch (android.provider.Settings.SettingNotFoundException e) { settingMissing = true; }
    check(settingMissing, "global integer lookup without a default throws for missing settings");
    var ssaidUser = Files.createTempDirectory("venture-ssaid-");
    byte[] ssaidKey = com.venture.runtime.Ssaid.userKey(ssaidUser);
    check(ssaidKey.length == 32 && Arrays.equals(com.venture.runtime.Ssaid.userKey(ssaidUser), ssaidKey),
        "ANDROID_ID user key is created once and reread");
    var signers = new android.content.pm.Signature[]{new android.content.pm.Signature(new byte[]{1, 2, 3})};
    String ssaid = com.venture.runtime.Ssaid.derive(ssaidKey, signers);
    check(ssaid.matches("[0-9a-f]{16}") && ssaid.equals(com.venture.runtime.Ssaid.derive(ssaidKey, signers)),
        "ANDROID_ID is 16 stable lowercase hex digits");
    check(com.venture.runtime.Ssaid.derive(new byte[32], signers).equals("90adc4d50b0c90e2"),
        "ANDROID_ID follows the AOSP length-prefixed certificate HMAC");
    check(!ssaid.equals(com.venture.runtime.Ssaid.derive(ssaidKey,
            new android.content.pm.Signature[]{new android.content.pm.Signature(new byte[]{1, 2, 4})})),
        "ANDROID_ID is scoped by signing certificate");
    try (var paths = Files.walk(ssaidUser)) {
      for (var path : paths.sorted(Comparator.reverseOrder()).toList()) Files.delete(path);
    }
    var location = FrameworkTest.class.getProtectionDomain().getCodeSource().getLocation();
    try (var loader = new java.net.URLClassLoader(new java.net.URL[]{location}, FrameworkTest.class.getClassLoader()) {
      protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
        if (name.equals("FrameworkTest$OptionalParameter")) throw new ClassNotFoundException(name);
        if (name.equals("FrameworkTest$NativeMetadataFixture")) {
          var cls = findLoadedClass(name);
          return cls == null ? findClass(name) : cls;
        }
        return super.loadClass(name, resolve);
      }
    }) {
      var cls = loader.loadClass("FrameworkTest$NativeMetadataFixture");
      check(Arrays.equals(NativeLoader.nativeMethods(cls), new String[]{"call", "(Ljava/lang/Object;)I"}),
          "native declarations do not resolve optional parameter types");
      boolean missing = false;
      try { cls.getDeclaredMethods(); } catch (NoClassDefFoundError e) { missing = true; }
      check(missing, "reflection fixture actually contains an unavailable dependency");
    }
    Looper.prepareMainLooper();
    var resolver = new android.content.ContentResolver();
    check(android.provider.Settings.Global.getInt(resolver, "venture_missing_setting", 37) == 37,
        "missing global integer setting returns caller default");
    for (String key : List.of("venture_missing_setting", android.provider.Settings.Global.DEVICE_NAME)) {
      boolean missingSetting = false;
      try { android.provider.Settings.Global.getInt(resolver, key); }
      catch (android.provider.Settings.SettingNotFoundException e) {
        missingSetting = key.equals(e.getMessage());
      }
      check(missingSetting, "missing or noninteger global setting throws with setting name");
    }
    var sourceClosed = new java.util.concurrent.atomic.AtomicBoolean();
    android.media.MediaDataSource source = new android.media.MediaDataSource() {
      public long getSize() { return 1L << 33; }
      public int readAt(long position, byte[] buffer, int offset, int size) throws java.io.IOException {
        if (sourceClosed.get()) throw new java.io.IOException("closed source");
        if (size == 0) return 0;
        if (position >= getSize()) return -1;
        buffer[offset] = (byte) (position >>> 32);
        return 1;
      }
      public void close() { sourceClosed.set(true); }
    };
    byte[] mediaBytes = new byte[3];
    check(source.readAt(1L << 32, mediaBytes, 1, 1) == 1 && mediaBytes[1] == 1,
        "app MediaDataSource dispatch preserves 64-bit positions and buffer offsets");
    check(source.readAt(source.getSize(), mediaBytes, 0, 1) == -1, "app source owns EOF semantics");
    ((java.io.Closeable) source).close();
    boolean sourceRejected = false;
    try { source.readAt(0, mediaBytes, 0, 1); } catch (java.io.IOException expected) { sourceRejected = true; }
    check(sourceRejected, "Closeable ownership and source IO errors reach the caller");
    var media = new android.media.MediaPlayer();
    check(!media.isPlaying() && media.getCurrentPosition() == 0
        && media.getVideoWidth() == 0 && media.getVideoHeight() == 0, "new MediaPlayer is idle without decoding");
    media.setLooping(true);
    check(media.isLooping(), "idle MediaPlayer preserves looping configuration");
    boolean mediaRejected = false;
    try { media.prepare(); } catch (IllegalStateException e) { mediaRejected = true; }
    check(mediaRejected, "MediaPlayer cannot prepare without a data source");
    mediaRejected = false;
    try { media.prepareAsync(); } catch (IllegalStateException e) { mediaRejected = true; }
    check(mediaRejected, "MediaPlayer cannot prepare asynchronously without a data source");
    mediaRejected = false;
    try { media.selectTrack(0); } catch (IllegalStateException e) { mediaRejected = true; }
    check(mediaRejected, "idle MediaPlayer rejects track selection");
    mediaRejected = false;
    try { media.setDataSource("unavailable.mp4"); } catch (java.io.IOException e) { mediaRejected = true; }
    check(mediaRejected && !media.isPlaying(), "unavailable decoder fails explicitly and does not start playback");
    media.reset();
    check(!media.isLooping() && !media.isPlaying(), "MediaPlayer reset returns to idle");
    media.release(); media.release();
    mediaRejected = false;
    try { media.isPlaying(); } catch (IllegalStateException e) { mediaRejected = true; }
    check(mediaRejected, "released MediaPlayer rejects use");
    HandlerThread thread = new HandlerThread("framework-queue-test");
    thread.start();
    Handler h = new Handler(thread.getLooper());
    List<Integer> order = new CopyOnWriteArrayList<>();
    CountDownLatch done = new CountDownLatch(3);
    long deadline = SystemClock.uptimeMillis() + 70;
    Runnable forbidden = () -> {
      throw new AssertionError("Cancelled callback ran");
    };
    h.postAtTime(forbidden, deadline);
    h.removeCallbacks(forbidden);
    for (int i = 0; i < 3; i++) {
      final int n = i;
      h.postAtTime(() -> {
        check(SystemClock.uptimeMillis() >= deadline, "message deadline");
        order.add(n);
        done.countDown();
      }, deadline);
    }
    check(done.await(3, TimeUnit.SECONDS), "queue wakes and delivers");
    check(order.equals(List.of(0, 1, 2)), "equal deadline FIFO");
    var asyncDone = new CountDownLatch(1);
    Handler.createAsync(thread.getLooper(), message -> {
      check(message.isAsynchronous(), "async Handler marks queued messages"); asyncDone.countDown(); return true;
    }).sendEmptyMessage(17);
    check(asyncDone.await(3, TimeUnit.SECONDS), "async Handler delivers on its Looper");
    var messageDone = new CountDownLatch(2);
    var received = new CopyOnWriteArrayList<Message>();
    Handler destination = new Handler(thread.getLooper(), message -> {
      check(Looper.myLooper() == thread.getLooper(), "Messenger dispatches on recipient Looper");
      received.add(message);
      messageDone.countDown();
      return true;
    });
    Messenger messenger = new Messenger(destination);
    check(messenger.equals(new Messenger(destination))
        && messenger.hashCode() == new Messenger(messenger.getBinder()).hashCode(),
        "Messenger identity follows Handler Binder");
    Parcel messengerParcel = Parcel.obtain();
    messenger.writeToParcel(messengerParcel, 0);
    Messenger.writeMessengerOrNullToParcel(null, messengerParcel);
    messengerParcel.setDataPosition(0);
    check(Messenger.CREATOR.createFromParcel(messengerParcel).equals(messenger)
        && Messenger.readMessengerOrNullFromParcel(messengerParcel) == null,
        "Messenger parcel retains endpoint and null");
    messengerParcel.recycle();
    Message localMessage = Message.obtain(null, 11, 12, 13);
    messenger.send(localMessage);
    // Hide queryLocalInterface to exercise AIDL parcel dispatch in the hosted Binder.
    Binder proxyBinder = new Binder() {
      protected boolean onTransact(int code, Parcel input, Parcel reply, int flags) throws RemoteException {
        return messenger.getBinder().transact(code, input, reply, flags);
      }
    };
    Message proxyMessage = Message.obtain(null, 21, 22, 23);
    proxyMessage.replyTo = messenger;
    proxyMessage.getData().putString("payload", "download progress");
    new Messenger(proxyBinder).send(proxyMessage);
    check(messageDone.await(3, TimeUnit.SECONDS), "Messenger local and parcel sends complete");
    check(received.get(0) == localMessage && localMessage.sendingUid == android.os.Process.myUid(),
        "local Messenger keeps message identity and assigns caller UID");
    Message delivered = received.get(1);
    check(delivered != proxyMessage && delivered.what == 21 && delivered.arg1 == 22
        && delivered.arg2 == 23 && delivered.replyTo.equals(messenger)
        && delivered.getData().getString("payload").equals("download progress"),
        "parcel Messenger preserves message payload and reply endpoint");
    delivered.getData().putString("payload", "changed");
    check(proxyMessage.getData().getString("payload").equals("download progress"),
        "parcel Messenger copies bundle data");
    boolean unsupportedMessenger = false;
    try { new Messenger(new Binder()).send(Message.obtain()); }
    catch (RemoteException e) { unsupportedMessenger = true; }
    check(unsupportedMessenger, "unknown Messenger transaction fails observably");
    boolean callbackParcel = false, objectParcel = false;
    Parcel invalidMessage = Parcel.obtain();
    try { Message.obtain(h, () -> {}).writeToParcel(invalidMessage, 0); }
    catch (RuntimeException e) { callbackParcel = true; }
    try { Message.obtain(h, 1, new Object()).writeToParcel(invalidMessage, 0); }
    catch (RuntimeException e) { objectParcel = true; }
    check(callbackParcel && objectParcel, "Message rejects callbacks and nonparcelable payloads");
    invalidMessage.recycle();
    var filtered = new CountDownLatch(1);
    var restored = new CountDownLatch(1);
    h.post(() -> {
      var adapter = new android.widget.ArrayAdapter<String>(null, 0,
          List.of("Blue sea", "Red sky", "Seaside"));
      adapter.getFilter().filter("se", count -> {
        check(Looper.myLooper() == thread.getLooper(), "filter publishes on creating Looper");
        check(count == 2 && adapter.getItem(0).equals("Blue sea"), "filter matches word prefixes");
        adapter.add("Green hill");
        adapter.getFilter().filter(null, restoredCount -> {
          check(restoredCount == 4 && adapter.getPosition("Green hill") == 3,
              "filter reset retains edits to original collection");
          restored.countDown();
        });
        filtered.countDown();
      });
    });
    check(filtered.await(3, TimeUnit.SECONDS) && restored.await(3, TimeUnit.SECONDS),
        "background filtering and reset complete");
    var devices = new java.util.concurrent.atomic.AtomicReference<>(
        new android.media.AudioDeviceInfo[]{new android.media.AudioDeviceInfo(1, false, "first")});
    var audio = new android.media.AudioManager() {
      public android.media.AudioDeviceInfo[] getDevices(int flags) { return devices.get().clone(); }
    };
    var initialRoute = new CountDownLatch(1);
    var changedRoute = new CountDownLatch(1);
    var routeEvents = new CopyOnWriteArrayList<String>();
    var callback = new android.media.AudioDeviceCallback() {
      public void onAudioDevicesAdded(android.media.AudioDeviceInfo[] values) {
        check(Looper.myLooper() == thread.getLooper(), "audio callback uses requested Looper");
        for (var value : values) routeEvents.add("+" + value.getProductName());
        if (values[0].getProductName().equals("first")) initialRoute.countDown();
        else changedRoute.countDown();
      }
      public void onAudioDevicesRemoved(android.media.AudioDeviceInfo[] values) {
        for (var value : values) routeEvents.add("-" + value.getProductName());
      }
    };
    try {
      audio.registerAudioDeviceCallback(callback, h);
      check(initialRoute.await(3, TimeUnit.SECONDS), "audio callback reports initial inventory");
      devices.set(new android.media.AudioDeviceInfo[]{new android.media.AudioDeviceInfo(1, false, "second")});
      check(changedRoute.await(3, TimeUnit.SECONDS), "audio callback reports route replacement");
      check(routeEvents.equals(List.of("+first", "-first", "+second")), "audio inventory delta order");
    } finally { audio.unregisterAudioDeviceCallback(callback); }
    h.postDelayed(forbidden, 60_000);
    check(thread.quitSafely(), "safe quit accepted");
    thread.join(3000);
    check(!thread.isAlive(), "safe quit discards future messages");
    check(!h.post(() -> {}), "dead queue rejects sends");
    Bundle b = new Bundle();
    b.putString("unicode", "save 雪 🎮");
    b.putInt("count", 4);
    Bundle copy = new Bundle(b);
    b.putInt("count", 8);
    check(copy.getInt("count") == 4, "Bundle map copied");
    Parcel p = Parcel.obtain();
    p.writeBundle(copy);
    p.writeLong(0x1122334455667788L);
    p.setDataPosition(0);
    check(p.readBundle().getString("unicode").equals("save 雪 🎮"), "parcel bundle");
    check(p.readLong() == 0x1122334455667788L, "parcel long width");
    p.recycle();
    Binder binder = new Binder() {
      protected boolean onTransact(int code, Parcel input, Parcel reply, int flags) throws RemoteException {
        if (code != 7) return super.onTransact(code, input, reply, flags);
        input.enforceInterface("test.local");
        reply.writeInt(input.readInt() + 1);
        return true;
      }
    };
    IInterface local = () -> binder;
    binder.attachInterface(local, "test.local");
    check(binder.queryLocalInterface("test.local") == local, "Binder local interface identity");
    Parcel reply = Parcel.obtain();
    p.writeInterfaceToken("test.local"); p.writeInt(40);
    check(binder.transact(7, p, reply, 0) && reply.readInt() == 41, "Binder parcel cursor and dispatch");
    check(!binder.transact(8, p, reply, 0), "unknown Binder transaction rejected");
    p.recycle(); reply.recycle();
    var deliveries = new ArrayList<android.content.Intent>();
    var permissions = new android.content.Context() {
      public String getPackageName() { return "test.local"; }
      public android.content.pm.PackageManager getPackageManager() {
        return new android.content.pm.PackageManager(null) {
          public int checkPermission(String permission, String pkg) {
            return pkg.equals("test.local") && permission.equals("android.permission.READ_EXTERNAL_STORAGE")
                ? PERMISSION_GRANTED : PERMISSION_DENIED;
          }
        };
      }
    };
    var wrappedPermissions = new android.content.ContextWrapper(permissions);
    check(wrappedPermissions.checkPermission("android.permission.READ_EXTERNAL_STORAGE", -1,
        android.os.Process.myUid()) == 0, "permission query delegates UID grant through ContextWrapper");
    check(permissions.checkSelfPermission("android.permission.READ_EXTERNAL_STORAGE") == 0
        && permissions.checkCallingOrSelfPermission("android.permission.CAMERA") == -1,
        "self and local Binder permission queries use package grants");
    check(permissions.checkPermission("android.permission.READ_EXTERNAL_STORAGE", -1, 10001) == -1,
        "unknown UID has no hosted permission grants");
    boolean nullPermission = false;
    try { permissions.checkSelfPermission(null); }
    catch (IllegalArgumentException e) { nullPermission = true; }
    check(nullPermission, "null permission follows ContextImpl argument validation");
    var context = new android.content.Context() {
      public String getPackageName() { return "test.local"; }
      public void startActivity(android.content.Intent intent) { deliveries.add(intent); }
    };
    var rectangle = new android.graphics.RectF(-1.5f, 2.5f, 4, 8);
    var rectangleParcel = Parcel.obtain(); rectangle.writeToParcel(rectangleParcel, 0);
    rectangleParcel.setDataPosition(0);
    var restoredRectangle = android.graphics.RectF.CREATOR.createFromParcel(rectangleParcel);
    check(restoredRectangle.contains(-1.5f, 2.5f) && !restoredRectangle.contains(4, 8)
        && restoredRectangle.width() == 5.5f && restoredRectangle.height() == 5.5f,
        "RectF parcel preserves floating coordinates and exclusive edges");
    var persistent = new PersistableBundle(); persistent.putLong("counter", 1L << 40);
    var persistentParcel = Parcel.obtain(); persistent.writeToParcel(persistentParcel, 0);
    persistentParcel.setDataPosition(0);
    var restoredPersistent = PersistableBundle.CREATOR.createFromParcel(persistentParcel);
    restoredPersistent.putLong("counter", 7);
    check(persistent.getLong("counter") == 1L << 40 && restoredPersistent.getLong("counter") == 7,
        "PersistableBundle parcel owns its value map and retains 64-bit values");
    var imageButton = new android.widget.ImageButton(context);
    var buttonClicks = new java.util.concurrent.atomic.AtomicInteger();
    imageButton.setOnClickListener(view -> buttonClicks.incrementAndGet());
    check(imageButton.performClick() && buttonClicks.get() == 1,
        "ImageButton dispatches View clicks");
    imageButton.setImageResource(0);
    check(imageButton.getDrawable() == null, "image resource zero clears without resource lookup");
    imageButton.setImportantForAutofill(android.view.View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS);
    check(imageButton.getImportantForAutofill() == 8, "view retains its autofill exclusion policy without a service");
    boolean autofillPolicyRejected = false;
    try { imageButton.setImportantForAutofill(3); }
    catch (IllegalArgumentException e) { autofillPolicyRejected = true; }
    check(autofillPolicyRejected && imageButton.getImportantForAutofill() == 8,
        "invalid autofill policy cannot replace the stored view policy");
    var launch = new android.content.Intent("test.launch").putExtra("generation", 1);
    int tokenFlags = android.app.PendingIntent.FLAG_IMMUTABLE | android.app.PendingIntent.FLAG_ONE_SHOT;
    var token = android.app.PendingIntent.getActivity(context, 7, launch, tokenFlags);
    check(android.app.PendingIntent.getActivity(context, 7, launch,
        tokenFlags | android.app.PendingIntent.FLAG_NO_CREATE) == token, "PendingIntent lookup identity");
    launch.putExtra("generation", 2);
    check(android.app.PendingIntent.getActivity(context, 7, launch,
        tokenFlags | android.app.PendingIntent.FLAG_UPDATE_CURRENT) == token, "PendingIntent update identity");
    token.send(context, 0, new android.content.Intent("ignored immutable fill-in"));
    check(deliveries.size() == 1 && deliveries.get(0).getIntExtra("generation", 0) == 2,
        "PendingIntent sends updated extras and ignores immutable fill-in");
    boolean cancelled = false;
    try { token.send(); } catch (android.app.PendingIntent.CanceledException e) { cancelled = true; }
    check(cancelled, "one-shot PendingIntent consumed");
    boolean senderCancelled = false;
    try { token.getIntentSender().sendIntent(context, 0, null, null, null); }
    catch (android.content.IntentSender.SendIntentException e) { senderCancelled = true; }
    check(senderCancelled, "IntentSender shares token cancellation");
    check(android.app.PendingIntent.getActivity(context, 7, launch,
        tokenFlags | android.app.PendingIntent.FLAG_NO_CREATE) == null, "consumed token removed");
    var router = new android.media.MediaRouter();
    check(router.getRouteCount() == 1 && router.getSelectedRoute(2).getPresentationDisplay() == null,
        "local route has no external presentation display");
    var motion = android.view.MotionEvent.obtain(10, 20, 2, 15, 25, 0);
    var point = new android.view.MotionEvent.PointerCoords(); point.x = 30; point.y = 40;
    motion.addBatch(25, new android.view.MotionEvent.PointerCoords[]{point}, 1);
    point.x = 100;
    check(motion.getDownTime() == 10 && motion.getEventTime() == 25 && motion.getX() == 30,
        "motion sample timestamps and coordinate ownership");
    check(motion.getHistorySize() == 1 && motion.getHistoricalAxisValue(0, 0, 0) == 15,
        "motion history retains previous sample");
    check(motion.findPointerIndex(0) == 0 && motion.findPointerIndex(1) == -1, "pointer identity");
    var keyEvent = new android.view.KeyEvent(10, 15, 0, 29, 3, 0x40, 7, 99, 0, 0x101);
    check(keyEvent.getUnicodeChar() == 'A' && keyEvent.getRepeatCount() == 3 && keyEvent.getScanCode() == 99,
        "key event preserves data and normalizes modifiers");
    var activity = new android.app.Activity();
    var editor = new android.widget.EditText(context);
    var edits = new ArrayList<String>();
    var watcher = new android.text.TextWatcher() {
      public void beforeTextChanged(CharSequence s, int start, int count, int after) { edits.add("before:" + s); }
      public void onTextChanged(CharSequence s, int start, int before, int count) { edits.add("on:" + s); }
      public void afterTextChanged(android.text.Editable value) { edits.add("after:" + value); }
    };
    editor.addTextChangedListener(watcher);
    editor.getText().append("first");
    editor.getText().append(" second");
    editor.setText("replacement");
    check(edits.equals(List.of("before:", "on:first", "after:first", "before:first",
        "on:first second", "after:first second", "before:first second", "on:replacement", "after:replacement")),
        "text watchers observe old/new values across edits and replacement");
    editor.setSelection(3);
    editor.getText().insert(3, "++");
    check(android.text.Selection.getSelectionStart(editor.getText()) == 5,
        "selection point follows inserted text");
    editor.removeTextChangedListener(watcher);
    int oldEdits = edits.size();
    editor.getText().clear();
    check(edits.size() == oldEdits, "removed text listener is not notified");
    editor.setId(123);
    var clicks = new ArrayList<Integer>();
    var dialog = new android.app.AlertDialog.Builder(context).setView(editor)
        .setPositiveButton("Continue", (d, which) -> clicks.add(which)).create();
    check(dialog.findViewById(123) == editor, "dialog retains custom content");
    var shown = new ArrayList<android.content.DialogInterface>();
    android.content.DialogInterface.OnShowListener onShow = shown::add;
    var callbackDialog = new android.app.Dialog(context);
    callbackDialog.setOnShowListener(onShow);
    callbackDialog.setOnShowListener(null);
    callbackDialog.setOnShowListener(onShow);
    boolean presentationUnavailable = false;
    try { callbackDialog.show(); }
    catch (UnsupportedOperationException expected) { presentationUnavailable = true; }
    check(presentationUnavailable && shown.isEmpty() && !callbackDialog.isShowing(),
        "unsupported dialog presentation does not fabricate a show callback");
    callbackDialog.getWindow().getDecorView().layout(0, 0, 200, 100);
    var outsideTouch = android.view.MotionEvent.obtain(0, 0, android.view.MotionEvent.ACTION_OUTSIDE, 0, 0, 0);
    callbackDialog.setCanceledOnTouchOutside(false);
    check(!callbackDialog.getWindow().shouldCloseOnTouch(context, outsideTouch), "outside cancellation disabled");
    var canceled = new ArrayList<android.content.DialogInterface>();
    callbackDialog.setOnCancelListener(canceled::add);
    callbackDialog.setCancelable(false);
    callbackDialog.setCanceledOnTouchOutside(true);
    check(callbackDialog.getWindow().shouldCloseOnTouch(context, outsideTouch), "outside cancellation enabled");
    callbackDialog.cancel();
    check(canceled.equals(List.of(callbackDialog)), "outside cancellation restores cancelability");
    check(!callbackDialog.onTouchEvent(outsideTouch), "unshown dialog does not consume touches");
    dialog.getButton(-1).performClick();
    check(clicks.equals(List.of(-1)), "dialog button delivers selected identifier");
    var root = new android.widget.FrameLayout(context);
    var frameDisplay = new android.view.Display() {
      public int getWidth() { return 1280; }
      public int getHeight() { return 720; }
    };
    var frameView = new android.view.View(context) {
      public android.view.Display getDisplay() { return frameDisplay; }
    };
    frameView.layout(20, 30, 120, 80);
    var displayFrame = new android.graphics.Rect(-1, -1, -1, -1);
    frameView.getWindowVisibleDisplayFrame(displayFrame);
    check(displayFrame.left == 0 && displayFrame.top == 0 && displayFrame.right == 1280 && displayFrame.bottom == 720,
        "visible display frame uses window display coordinates, not the queried view's bounds");
    frameView.setX(123); frameView.setY(456);
    frameView.getDrawingRect(displayFrame);
    check(displayFrame.left == 0 && displayFrame.top == 0 && displayFrame.right == 100 && displayFrame.bottom == 50,
        "drawing rectangle uses local layout dimensions without position or translation");
    var child = new android.view.View(context);
    root.addView(child);
    check(child.getWindowToken() == null, "detached view has no window token");
    root.attach();
    check(child.getWindowToken() != null && child.getWindowToken() == root.getWindowToken(),
        "attached hierarchy shares a Binder window identity");
    var uiNotifications = new java.util.concurrent.atomic.AtomicInteger();
    int immersive = android.view.View.SYSTEM_UI_FLAG_FULLSCREEN | android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
    root.setOnSystemUiVisibilityChangeListener(flags -> {
      uiNotifications.incrementAndGet();
      root.setSystemUiVisibility(immersive);
    });
    root.setSystemUiVisibility(immersive);
    root.setSystemUiVisibility(immersive);
    root.setSystemUiVisibility(android.view.View.SYSTEM_UI_FLAG_FULLSCREEN);
    check(uiNotifications.get() == 0 && root.getSystemUiVisibility() == android.view.View.SYSTEM_UI_FLAG_FULLSCREEN,
        "UI requests retain flags without synthesizing system-change callbacks or recursing");
    root.detach();
    check(child.getWindowToken() == null, "window token unavailable after detach");
    var initialLayoutRoot = new android.widget.FrameLayout(context);
    var initialLayoutChild = new android.view.View(context);
    initialLayoutRoot.addView(initialLayoutChild);
    var initialExactChild = new android.view.View(context);
    initialLayoutRoot.addView(initialExactChild, 300, android.view.ViewGroup.LayoutParams.WRAP_CONTENT);
    var initialLayouts = new java.util.concurrent.atomic.AtomicInteger();
    initialLayoutRoot.getViewTreeObserver().addOnGlobalLayoutListener(() -> {
      check(Looper.myLooper() == Looper.getMainLooper(), "initial global layout uses main Looper");
      check(initialLayoutChild.isAttachedToWindow(), "initial layout observes fully attached hierarchy");
      check(initialLayoutRoot.getMeasuredWidth() == initialLayoutRoot.getWidth()
          && initialLayoutChild.getMeasuredHeight() == initialLayoutChild.getHeight(),
          "traversal measures the hierarchy before global layout");
      check(initialExactChild.getMeasuredWidth() == 300
          && initialExactChild.getMeasuredHeight() == initialExactChild.getHeight(),
          "traversal measures exact LayoutParams sizes");
      initialLayouts.incrementAndGet();
    });
    initialLayoutRoot.attach();
    initialLayoutRoot.attach();
    check(initialLayouts.get() == 0, "initial global layout is deferred until attachment finishes");
    check(initialLayoutRoot.getMeasuredWidth() == 0 && initialExactChild.getMeasuredWidth() == 0,
        "attachment does not measure before the traversal");
    var initialLateChild = new android.view.View(context);
    initialLayoutRoot.addView(initialLateChild, 120, 80);
    check(initialLateChild.isAttachedToWindow() && initialLateChild.getMeasuredWidth() == 0,
        "addView to an attached parent defers measurement");
    check(android.graphics.SurfaceTexture.OnFrameAvailableListener.class.getMethod(
        "onFrameAvailable", android.graphics.SurfaceTexture.class).getReturnType() == void.class,
        "video callback contract resolves without an available media backend");
    int[] bitmapColors = {0x80112233, 0xff445566, 0xff778899, 0xffaabbcc};
    var bitmap = android.graphics.Bitmap.createBitmap(
        bitmapColors, 2, 2, android.graphics.Bitmap.Config.ARGB_8888);
    bitmapColors[0] = 0;
    check(bitmap.getPixel(0, 0) == 0x80112233 && bitmap.getPixel(1, 1) == 0xffaabbcc,
        "bitmap color factory copies ARGB pixels");
    check(!bitmap.isMutable(), "bitmap color factory is immutable");
    boolean immutableBitmap = false;
    try { bitmap.setPixel(0, 0, 0); } catch (IllegalStateException e) { immutableBitmap = true; }
    check(immutableBitmap, "immutable bitmap rejects writes");
    var reversedBitmap = android.graphics.Bitmap.createBitmap(
        bitmapColors, 2, -2, 2, 2, android.graphics.Bitmap.Config.ARGB_8888);
    check(reversedBitmap.getPixel(0, 0) == 0xff778899 && reversedBitmap.getPixel(1, 1) == 0xff445566,
        "bitmap color factory supports negative stride");
    boolean bitmapBounds = false;
    try { android.graphics.Bitmap.createBitmap(bitmapColors, 3, 2, 2, 2,
        android.graphics.Bitmap.Config.ARGB_8888); }
    catch (ArrayIndexOutOfBoundsException e) { bitmapBounds = true; }
    check(bitmapBounds, "bitmap color factory validates source array bounds");
    var pointerIcon = android.view.PointerIcon.create(bitmap, 1, 0);
    var pointerView = new android.view.View(context);
    pointerView.setPointerIcon(pointerIcon);
    check(pointerView.getPointerIcon() == pointerIcon, "view retains custom pointer icon");
    boolean pointerBounds = false;
    try { android.view.PointerIcon.create(bitmap, 2, 0); }
    catch (IllegalArgumentException e) { pointerBounds = true; }
    check(pointerBounds, "pointer hotspot must lie inside bitmap");
    pointerView.requestPointerCapture();
    check(!pointerView.hasPointerCapture(), "unavailable pointer backend never grants capture");
    pointerView.releasePointerCapture();
    root.layout(20, 30, 200, 300); child.layout(4, 7, 50, 60); child.setTranslationY(0.75f);
    int[] pointInWindow = new int[2]; child.getLocationInWindow(pointInWindow);
    check(Arrays.equals(pointInWindow, new int[]{24, 38}), "view location accumulates parents and translation");
    editor.setFilters(new android.text.InputFilter[]{new android.text.InputFilter.LengthFilter(4)});
    editor.setText("123456"); editor.getText().append("extra");
    check(editor.getText().toString().equals("1234"), "TextView filters apply to replacement and direct edits");
    var events = new ArrayList<String>();
    var fragment = new android.app.Fragment() {
      public void onAttach(android.app.Activity a) { events.add("attach"); }
      public void onDetach() { events.add("detach"); }
    };
    var manager = activity.getFragmentManager();
    check(!fragment.getRetainInstance(), "fragments default to no configuration retention");
    fragment.setRetainInstance(true);
    manager.beginTransaction().add(fragment, "permission").commit();
    check(!fragment.isAdded(), "fragment commit is deferred");
    check(manager.executePendingTransactions() && manager.findFragmentByTag("permission") == fragment
        && fragment.getRetainInstance(),
        "fragment transaction attachment");
    manager.beginTransaction().remove(fragment).commitNow();
    check(!fragment.isAdded() && fragment.getActivity() == null && events.equals(List.of("attach", "detach")),
        "fragment removal detaches owner");
    fragment.setRetainInstance(false);
    check(!fragment.getRetainInstance(), "fragment retention policy can be cleared after removal");
    var unityFragment = new android.app.Fragment();
    var offMain = new Object[2];
    var unityThread = new Thread(() -> {
      offMain[0] = manager.findFragmentByTag("96489");
      manager.beginTransaction().add(0, unityFragment, "96489").commit();
      try { manager.beginTransaction().remove(unityFragment).commitNow(); }
      catch (IllegalStateException e) { offMain[1] = e; }
    });
    unityThread.start(); unityThread.join();
    check(offMain[0] == null && offMain[1] != null && !unityFragment.isAdded(),
        "fragment lookup and deferred commit work off the main thread; commitNow does not");
    check(manager.executePendingTransactions() && manager.findFragmentByTag("96489") == unityFragment,
        "off-thread fragment commit attaches on the main thread");
    manager.beginTransaction().remove(unityFragment).commitNow();
    Path testRoot = Path.of("build/framework-tests");
    Files.createDirectories(testRoot);
    try (var loader = new dalvik.system.PathClassLoader("", FrameworkTest.class.getClassLoader())) {
      check(loader.loadClass("android/os/Message") == Message.class
          && loader.loadClass("android.os.Message") == Message.class,
          "Android class loader accepts dotted and slash names with one class identity");
      boolean absentClass = false;
      try { loader.loadClass("missing/game/Class"); } catch (ClassNotFoundException e) { absentClass = true; }
      check(absentClass, "Android class loader still rejects absent classes");
    }
    try (var loader = new dalvik.system.DelegateLastClassLoader("", FrameworkTest.class.getClassLoader())) {
      check(loader.loadClass("android/os/Message") == Message.class,
          "delegate-last loader normalizes names before platform delegation");
    }
    var preferenceActivity = new android.app.Activity() {
      public String getPackageName() { return "test.game"; }
      public android.content.ComponentName getComponentName() {
        return new android.content.ComponentName(getPackageName(), "test.game.ui.PlayerActivity");
      }
      public java.io.File getDataDir() { return testRoot.resolve("activity-preferences").toFile(); }
    };
    check(preferenceActivity.getLocalClassName().equals("ui.PlayerActivity"), "activity local name");
    var activityPreferences = preferenceActivity.getPreferences(0);
    check(activityPreferences == preferenceActivity.getSharedPreferences("ui.PlayerActivity", 0),
        "activity preferences use local class filename");
    check(activityPreferences.edit().putInt("launches", 3).commit()
        && preferenceActivity.getPreferences(0).getInt("launches", 0) == 3,
        "activity preferences persist through normal XML store");
    check(Files.exists(testRoot.resolve("activity-preferences/shared_prefs/ui.PlayerActivity.xml")),
        "activity preferences persisted to expected filename");
    var externalActivity = new android.app.Activity() {
      public String getPackageName() { return "test.game"; }
      public android.content.ComponentName getComponentName() {
        return new android.content.ComponentName(getPackageName(), "test.gameplay.PlayerActivity");
      }
    };
    check(externalActivity.getLocalClassName().equals("test.gameplay.PlayerActivity"),
        "local name does not strip a partial package prefix");
    Path assetFile = Files.createTempFile(testRoot, "asset-region-", ".bin");
    try {
      Files.writeString(assetFile, "beforeCONTENTSafter");
      var descriptor = ParcelFileDescriptor.open(assetFile.toFile(), ParcelFileDescriptor.MODE_READ_ONLY);
      var asset = new android.content.res.AssetFileDescriptor(descriptor, 6, 8);
      try (var stream = asset.createInputStream()) {
        check(asset.getStartOffset() == 6 && asset.getLength() == 8, "asset region metadata");
        check(new String(stream.readAllBytes(), java.nio.charset.StandardCharsets.UTF_8).equals("CONTENTS"),
            "asset stream bounds include optimized JDK reads");
        check(stream.read() == -1 && stream.read(new byte[0]) == 0, "asset region EOF");
      }
      check(!descriptor.getFileDescriptor().valid(), "asset stream closes owned descriptor");
      descriptor = ParcelFileDescriptor.open(assetFile.toFile(), ParcelFileDescriptor.MODE_READ_WRITE);
      try (var stream = new android.content.res.AssetFileDescriptor(descriptor, 6, 3).createOutputStream()) {
        stream.write("XYZextra".getBytes(java.nio.charset.StandardCharsets.UTF_8));
      }
      check(Files.readString(assetFile).equals("beforeXYZTENTSafter"), "bounded asset writes preserve surrounding bytes");
    } finally { Files.deleteIfExists(assetFile); }
    Path directory = Files.createTempDirectory(testRoot, "preferences-");
    Path file = directory.resolve("preferences.xml");
    try {
      XmlPreferences preferences = new XmlPreferences(file);
      List<String> changed = new ArrayList<>();
      preferences.registerOnSharedPreferenceChangeListener((prefs, key) -> changed.add(key));
      check(preferences.edit()
                .putString("escaped", "<&雪>")
                .putInt("int", 7)
                .putLong("long", 1L << 45)
                .putFloat("float", 1.25f)
                .putBoolean("bool", true)
                .putStringSet("set", Set.of("one", "two"))
                .commit(),
          "atomic commit");
      XmlPreferences reloaded = new XmlPreferences(file);
      check(reloaded.getString("escaped", "").equals("<&雪>"), "XML escaping");
      check(reloaded.getInt("int", 0) == 7 && reloaded.getLong("long", 0) == 1L << 45,
          "XML integer widths");
      check(reloaded.getBoolean("bool", false) && reloaded.getFloat("float", 0) == 1.25f,
          "XML scalar types");
      Set<String> set = reloaded.getStringSet("set", null);
      set.clear();
      check(reloaded.getStringSet("set", null).size() == 2, "preference set defensive copy");
      check(changed.size() == 6, "preference notifications");
      check(reloaded.edit().clear().putInt("survivor", 1).commit(), "clear then updates");
      check(new XmlPreferences(file).getAll().size() == 1, "clear persistence");
      Files.writeString(file,
          "<!DOCTYPE x [<!ENTITY e SYSTEM 'file:///private'>]><map><string "
              + "name='x'>&e;</string></map>");
      boolean rejected = false;
      try {
        new XmlPreferences(file);
      } catch (IllegalStateException e) {
        rejected = true;
      }
      check(rejected, "external XML entities rejected");
    } finally {
      Files.deleteIfExists(file);
      Files.deleteIfExists(directory);
    }
    var json = new org.json.JSONObject("{\"x\":[true,12,\"雪\"]}");
    check(json.getJSONArray("x").getString(2).equals("雪"), "Android JSON");
    var parser = org.xmlpull.v1.XmlPullParserFactory.newInstance().newPullParser();
    parser.setInput(new java.io.StringReader("<root a='3'>hello &amp; world</root>"));
    check(parser.nextTag() == 2 && parser.getAttributeValue(null, "a").equals("3"),
        "XMLPull attributes");
    check(parser.nextText().equals("hello & world"), "XMLPull text");
    for (int size = 0; size < 32; size++) {
      boolean rejected = false;
      try {
        BinaryResources.xml(new byte[size]);
      } catch (IllegalArgumentException e) {
        rejected = true;
      }
      check(rejected, "truncated binary XML " + size);
    }
    var insets = new android.view.WindowInsets(new android.graphics.Rect(1, 2, 3, 4));
    check(insets.getSystemWindowInsetBottom() == 4, "inset values");
    check(insets.consumeSystemWindowInsets().isConsumed(), "insets consumed");
    check(insets.getSystemWindowInsetTop() == 2 && !insets.isConsumed(), "insets immutable");
    var builder = new android.view.WindowInsets.Builder().setInsets(android.view.WindowInsets.Type.ime(),
        android.graphics.Insets.of(0,0,0,80)).setVisible(android.view.WindowInsets.Type.ime(), true);
    var built = builder.build();
    builder.setInsets(android.view.WindowInsets.Type.ime(), android.graphics.Insets.NONE);
    check(built.getInsets(android.view.WindowInsets.Type.ime()).bottom == 80
        && built.isVisible(android.view.WindowInsets.Type.ime()), "insets Builder snapshots typed values and visibility");
    boolean managerAbsent = false;
    try { Class.forName("com.google.android.play.core.assetpacks.AssetPackManager"); }
    catch (ClassNotFoundException e) { managerAbsent = true; }
    check(managerAbsent, "callback contracts do not advertise a Google asset service");
    check(android.content.pm.PackageManager.GET_INTENT_FILTERS == 32,
        "legacy Unity package query flag");
    check(java.lang.reflect.Modifier.isAbstract(com.google.android.play.core.tasks.Task.class.getModifiers())
        && java.lang.reflect.Modifier.isAbstract(com.google.android.play.core.assetpacks.AssetPackStates.class.getModifiers()),
        "legacy Play Core contracts provide no service results");
    check(com.google.android.play.core.tasks.OnCompleteListener.class
        .getMethod("onComplete", com.google.android.play.core.tasks.Task.class).getReturnType() == void.class,
        "legacy Play Core completion callback uses its own Task ABI");
    var taskEvents = new ArrayList<String>();
    var remainingTasks = new java.util.concurrent.atomic.AtomicInteger(3);
    Runnable completeTask = () -> { if (remainingTasks.decrementAndGet() == 0) Looper.getMainLooper().quitSafely(); };
    var task = new AsyncTask<Integer, Integer, Integer>() {
      protected Integer doInBackground(Integer... values) {
        check(Looper.myLooper() != Looper.getMainLooper(), "AsyncTask runs off main thread");
        publishProgress(values[0]); return values[0] * 2;
      }
      protected void onProgressUpdate(Integer... progress) { taskEvents.add("progress:" + progress[0]); }
      protected void onPostExecute(Integer result) {
        check(Looper.myLooper() == Looper.getMainLooper(), "AsyncTask completion uses main Looper");
        taskEvents.add("result:" + result); completeTask.run();
      }
    };
    var cancelledTask = new AsyncTask<Void, Void, Void>() {
      protected Void doInBackground(Void... ignored) { throw new AssertionError("cancelled task ran"); }
      protected void onCancelled(Void ignored) { taskEvents.add("cancelled"); completeTask.run(); }
    };
    var scheduled = new ArrayList<Runnable>();
    cancelledTask.executeOnExecutor(scheduled::add);
    check(cancelledTask.cancel(false), "AsyncTask cancellation accepted before worker starts");
    scheduled.get(0).run();
    check(task.execute(9).get(3, TimeUnit.SECONDS) == 18, "AsyncTask exposes its background result");
    var broadcasts = new ArrayList<String>();
    vibrationContext.registerReceiver(new android.content.BroadcastReceiver() {
      public void onReceive(android.content.Context context, android.content.Intent intent) {
        broadcasts.add(intent.getAction());
        completeTask.run();
      }
    }, new android.content.IntentFilter("test.BROADCAST"));
    vibrationContext.sendBroadcast(new android.content.Intent("test.OTHER"));
    vibrationContext.sendBroadcast(new android.content.Intent("test.BROADCAST"));
    check(broadcasts.isEmpty(), "broadcasts are delivered asynchronously");
    new Handler(Looper.getMainLooper()).postDelayed(() -> { throw new AssertionError("AsyncTask callback timeout"); }, 3000);
    Looper.loop();
    check(broadcasts.equals(List.of("test.BROADCAST")), "only matching receivers get the broadcast");
    check(initialLayouts.get() == 1, "initial global layout delivered once for an attachment");
    check(initialLateChild.getMeasuredWidth() == 120 && initialLateChild.getMeasuredHeight() == 80,
        "next traversal measures a view added to an attached parent");
    initialLayoutRoot.detach();
    check(taskEvents.contains("cancelled") && taskEvents.indexOf("progress:9") < taskEvents.indexOf("result:18"),
        "AsyncTask progress precedes result and cancellation delivers");
    check(task.getStatus() == AsyncTask.Status.FINISHED && cancelledTask.getStatus() == AsyncTask.Status.FINISHED,
        "AsyncTask completion finishes both tasks");
    // Configure the production Context with the single view prepared by the
    // native launcher. File children and streams remain ordinary JDK objects.
    Path obbRoot = Files.createTempDirectory(testRoot, "obb-view-").toAbsolutePath();
    Path obbView = obbRoot.resolve("data/external/Android/obb/test.game");
    Files.createDirectories(obbView);
    Files.writeString(obbRoot.resolve("base.apk"), "APK outside view");
    Files.writeString(obbRoot.resolve("plan.json"), "plan outside view");
    Files.writeString(obbView.resolve("main.1.test.game.obb"), "expansion");
    var providerWarnings = new ArrayList<String>();
    var shaderCache = new String[1];
    var libraryLoads = new ArrayList<String>();
    var libraryLoaders = new ArrayList<ClassLoader>();
    var nativeCreates = new int[1];
    var obbHost = (Host) java.lang.reflect.Proxy.newProxyInstance(Host.class.getClassLoader(),
        new Class<?>[]{Host.class}, (proxy, method, values) -> {
          if (method.getName().equals("loadLibrary")) { libraryLoads.add((String) values[0]); libraryLoaders.add((ClassLoader) values[1]); return null; }
          if (method.getName().equals("setupShadersDiskCache")) { shaderCache[0] = (String) values[0]; return null; }
          if (method.getName().equals("readAsset"))
            return "AndroidManifest.xml".equals(values[0]) ? obbManifest() : null;
          if (method.getName().equals("log")) { providerWarnings.add((String) values[2]); return null; }
          if (method.getName().equals("createSurface")) return 1L;
          if (method.getName().equals("releaseSurface")) return null;
          if (method.getName().equals("createNativeActivity")) {
            var fixtureActivity = (android.app.NativeActivity) values[0];
            check(fixtureActivity.getWindow().surfaceCallback() == fixtureActivity,
                "NativeActivity prepares its detached surface before native startup");
            nativeCreates[0]++;
            return 1L;
          }
          if (method.getName().equals("guestTid")) return 1;
          if (method.getName().equals("setThreadPriority")) return null;
          throw new UnsupportedOperationException(method.getName());
        });
    RuntimeEnvironment.configure(new String[]{"test.game", obbRoot.resolve("base.apk").toString(),
        obbRoot.resolve("data").toString(), obbRoot.resolve("cache").toString(), obbView.toString(),
        obbRoot.resolve("lib").toString(), "32", obbRoot.resolve("app.jar").toString()}, obbHost);
    var obbContext = new android.content.Context();
    var nativeInfo = new android.content.pm.ActivityInfo();
    nativeInfo.name = NativeStartupActivity.class.getName();
    nativeInfo.packageName = "test.game";
    RuntimeEnvironment.get().manifest.activities.add(nativeInfo);
    var nativeApp = new android.app.Application();
    nativeApp.ventureAttach(obbContext);
    var nativeActivity = new NativeStartupActivity();
    nativeActivity.ventureAttach(nativeApp);
    nativeActivity.setIntent(new android.content.Intent().setComponent(
        new android.content.ComponentName("test.game", nativeInfo.name)));
    nativeActivity.createOnly();
    check(nativeCreates[0] == 1 && nativeActivity.getWindow().surfaceCallback() == nativeActivity,
        "NativeActivity completes Java creation without attaching its surface");
    // AOSP ContextImpl: code_cache is a sibling of cache in the data directory;
    // handleBindApplication names the EGL blob cache inside it.
    var codeCache = obbContext.getCodeCacheDir();
    check(codeCache.equals(obbRoot.resolve("data").resolve("code_cache").toFile().getAbsoluteFile())
        && codeCache.isDirectory()
        && RuntimeEnvironment.toGuestPath(codeCache.toString()).equals("/data/user/0/test.game/code_cache"),
        "code cache directory is data/code_cache");
    android.app.ActivityThread.ventureAttach();
    android.app.ActivityThread.currentActivityThread().ventureBindApplication(obbContext);
    check(new java.io.File(codeCache, "com.android.opengl.shaders_cache").getAbsolutePath().equals(shaderCache[0]),
        "binding the application names the EGL shader cache file");
    censusWidgets(obbContext);
    valueAnimators();
    cursors();
    broadcasts();
    cancelledBroadcasts();
    platformServices(obbContext);
    // UE 5.4 BatteryReceiver/VolumeReceiver register with API 26 flags on the GameThread.
    var battery = obbContext.registerReceiver(null, new android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED),
        android.content.Context.RECEIVER_EXPORTED);
    check(battery != null && battery.getIntExtra("level", -1) == 100, "flagged registration returns the sticky intent");
    boolean bothExported = false;
    try {
      obbContext.registerReceiver(null, new android.content.IntentFilter("x"),
          android.content.Context.RECEIVER_EXPORTED | android.content.Context.RECEIVER_NOT_EXPORTED);
    } catch (IllegalArgumentException expected) { bothExported = true; }
    check(bothExported, "receivers cannot be both exported and not exported");
    check(System.getProperty("http.proxyHost") != null || (android.net.Proxy.getHost(obbContext) == null
        && android.net.Proxy.getPort(obbContext) == -1), "no configured proxy reads as null and -1");
    // UE 5.4 AndroidThunkJava_SetNativeDisplayRefreshRate hints the rate on its surface.
    var surface = new android.view.Surface(4, 4);
    surface.setFrameRate(72f, android.view.Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
    boolean badRate = false;
    try { surface.setFrameRate(Float.NaN, 0); } catch (IllegalArgumentException expected) { badRate = true; }
    surface.release();
    boolean released = false;
    try { surface.setFrameRate(72f, 0); } catch (IllegalStateException expected) { released = true; }
    check(badRate && released, "frame-rate hints validate their rate and surface");
    // UE 5.4 AndroidThunkJava_Vibrate builds a one-shot effect for the device vibrator.
    var effect = android.os.VibrationEffect.createOneShot(20, android.os.VibrationEffect.DEFAULT_AMPLITUDE);
    new android.os.Vibrator().vibrate(effect);
    check(providerWarnings.remove("No host vibration device"), "vibrating without a device warns");
    boolean zeroAmplitude = false, zeroDuration = false;
    try { android.os.VibrationEffect.createOneShot(20, 0); } catch (IllegalArgumentException expected) { zeroAmplitude = true; }
    try { android.os.VibrationEffect.createOneShot(0, 255); } catch (IllegalArgumentException expected) { zeroDuration = true; }
    check(effect.getDuration() == 20 && zeroAmplitude && zeroDuration, "one-shot vibration effects validate like AOSP");
    var layout = new android.view.WindowManager.LayoutParams();
    check(layout.preferredDisplayModeId == 0 && layout.preferredRefreshRate == 0, "no preferred display mode by default");
    android.os.BaseBundle base = new Bundle();  // bytecode names BaseBundle as the accessor owner
    base.putInt("i", 3); base.putLong("l", 4L); base.putString("s", "v"); base.putBoolean("b", true); base.putDouble("d", 0.5);
    check(base.getInt("i", 9) == 3 && base.getInt("x", 9) == 9 && base.getLong("l") == 4 && base.getBoolean("x", true)
        && base.getDouble("d", 1) == 0.5 && base.getDouble("x", 2) == 2 && base.size() == 5, "typed BaseBundle accessors");
    boolean noDisplay = false;
    try { obbContext.getDisplay(); } catch (UnsupportedOperationException expected) { noDisplay = true; }
    check(noDisplay, "non-visual contexts have no display");
    var rect = new android.graphics.Rect(0, 0, 10, 5);
    check(rect.contains(0, 0) && rect.contains(9, 4) && !rect.contains(10, 4) && !rect.contains(-1, 0)
        && !new android.graphics.Rect().contains(0, 0), "Rect.contains is half-open and empty rects contain nothing");
    check(new android.bluetooth.BluetoothGattCallback() {} != null
        && new android.bluetooth.le.ScanCallback() {} != null,
        "Bluetooth callbacks can be constructed without an adapter or service");
    var bluetooth = (android.bluetooth.BluetoothManager)
        obbContext.getSystemService(android.content.Context.BLUETOOTH_SERVICE);
    check(bluetooth != null && bluetooth == obbContext.getSystemService("bluetooth"),
        "Bluetooth framework manager is a cached service");
    check(bluetooth.getAdapter() != null
        && android.bluetooth.BluetoothAdapter.getDefaultAdapter() != null,
        "Bluetooth adapter exists and is enabled without paired audio devices");
    check(obbContext.getPackageManager().hasSystemFeature(android.content.pm.PackageManager.FEATURE_BLUETOOTH)
        && obbContext.getPackageManager().hasSystemFeature(android.content.pm.PackageManager.FEATURE_BLUETOOTH_LE),
        "Bluetooth hardware features agree with the available adapter");
    var bluetoothApp = obbContext.getApplicationInfo();
    int savedBluetoothTarget = bluetoothApp.targetSdkVersion, savedPlatformSdk = Build.VERSION.SDK_INT;
    var bluetoothPermissions = RuntimeEnvironment.get().manifest.permissions;
    var savedBluetoothPermissions = new HashSet<>(bluetoothPermissions);
    try {
      bluetoothPermissions.clear();
      bluetoothPermissions.add("android.permission.BLUETOOTH");
      bluetoothApp.targetSdkVersion = 30;
      check(bluetooth.getAdapter().isEnabled()
          && bluetooth.getAdapter().getState() == android.bluetooth.BluetoothAdapter.STATE_ON
          && bluetooth.getConnectedDevices(android.bluetooth.BluetoothProfile.GATT).isEmpty()
          && bluetooth.getDevicesMatchingConnectionStates(android.bluetooth.BluetoothProfile.GATT_SERVER,
              new int[] {android.bluetooth.BluetoothProfile.STATE_CONNECTED}).isEmpty()
          && bluetooth.getAdapter().getBondedDevices().isEmpty()
          && bluetooth.getAdapter().getProfileConnectionState(android.bluetooth.BluetoothProfile.HEADSET) == 0
          && bluetooth.getConnectionState(null, android.bluetooth.BluetoothProfile.GATT) == 0,
          "legacy declared Bluetooth permission permits empty device queries");
      bluetoothApp.targetSdkVersion = 31;
      bluetoothPermissions.add("android.permission.BLUETOOTH_CONNECT");
      check(bluetooth.getAdapter().isEnabled(), "target 31 can read adapter power without CONNECT grant");
      // Declaring a dangerous permission is not a runtime grant.
      for (Runnable query : new Runnable[] {
          () -> bluetooth.getConnectedDevices(android.bluetooth.BluetoothProfile.GATT),
          () -> bluetooth.getDevicesMatchingConnectionStates(android.bluetooth.BluetoothProfile.GATT_SERVER, new int[] {2}),
          () -> bluetooth.getConnectionState(null, android.bluetooth.BluetoothProfile.GATT),
          () -> bluetooth.getAdapter().getBondedDevices(),
          () -> bluetooth.getAdapter().getProfileConnectionState(android.bluetooth.BluetoothProfile.A2DP)}) {
        boolean denied = false;
        try { query.run(); } catch (SecurityException expected) { denied = true; }
        check(denied, "target 31 device query enforces denied BLUETOOTH_CONNECT");
      }
      Build.VERSION.SDK_INT = 30;
      check(bluetooth.getConnectedDevices(android.bluetooth.BluetoothProfile.GATT).isEmpty(),
          "pre-31 platform uses the legacy permission even for a newer target");
      Build.VERSION.SDK_INT = 32;
      var grantedBluetooth = new android.bluetooth.BluetoothManager(new android.content.Context() {
        public int checkPermission(String permission, int pid, int uid) {
          return "android.permission.BLUETOOTH_CONNECT".equals(permission) ? 0 : -1;
        }
      });
      check(grantedBluetooth.getConnectedDevices(android.bluetooth.BluetoothProfile.GATT).isEmpty()
          && grantedBluetooth.getAdapter().getBondedDevices().isEmpty()
          && grantedBluetooth.getAdapter().getProfileConnectionState(android.bluetooth.BluetoothProfile.HEADSET) == 0,
          "granted CONNECT uses the same enabled but disconnected adapter state");
      bluetoothPermissions.clear(); bluetoothApp.targetSdkVersion = 30;
      boolean deniedLegacy = false;
      try { bluetooth.getAdapter().getBondedDevices(); } catch (SecurityException expected) { deniedLegacy = true; }
      check(deniedLegacy, "legacy device queries require the manifest Bluetooth permission");
      deniedLegacy = false;
      try { bluetooth.getAdapter().isEnabled(); } catch (SecurityException expected) { deniedLegacy = true; }
      check(deniedLegacy, "legacy adapter state queries require the manifest Bluetooth permission");
    } finally {
      bluetoothApp.targetSdkVersion = savedBluetoothTarget; Build.VERSION.SDK_INT = savedPlatformSdk;
      bluetoothPermissions.clear(); bluetoothPermissions.addAll(savedBluetoothPermissions);
    }
    boolean invalidBluetoothProfile = false;
    try { bluetooth.getConnectedDevices(android.bluetooth.BluetoothProfile.A2DP); }
    catch (IllegalArgumentException expected) { invalidBluetoothProfile = true; }
    check(invalidBluetoothProfile, "BluetoothManager rejects non-GATT profiles");
    var installed = obbContext.getPackageManager().getInstalledPackages(0);
    check(installed.size() == 1 && installed.get(0).packageName.equals(obbContext.getPackageName()), "own package is installed");
    check(obbContext.getPackageManager().resolveService(
        new android.content.Intent("android.support.customtabs.action.CustomTabsService"), 0) == null,
        "implicit services resolve to nothing");
    var chooser = android.content.Intent.createChooser(new android.content.Intent("send"), "Share");
    check(chooser.getAction().equals(android.content.Intent.ACTION_CHOOSER)
        && ((android.content.Intent) chooser.getParcelableExtra(android.content.Intent.EXTRA_INTENT)).getAction().equals("send"),
        "chooser wraps its target");
    var provider = new android.content.ContentProvider() {
      public boolean onCreate() { return true; }
      public String getType(android.net.Uri uri) { return "image/png"; }
    };
    RuntimeEnvironment.get().contentProviders.put("test.images", provider);
    check(new android.content.Intent("view", android.net.Uri.parse("content://test.images/a"))
        .resolveType(obbContext.getContentResolver()).equals("image/png"), "content MIME queries reach the owned provider");
    check(obbContext.getContentResolver().getType(android.net.Uri.parse("content://missing/a")) == null,
        "absent content provider has no MIME type");
    RuntimeEnvironment.get().contentProviders.put("test.images", new android.content.ContentProvider() {
      public boolean onCreate() { return true; }
      public String getType(android.net.Uri uri) {
        if (uri.getPath().equals("/error")) throw new AssertionError("provider error");
        throw new UnsupportedOperationException("private provider detail");
      }
    });
    var contentUri = android.net.Uri.parse("content://test.images/a");
    check(obbContext.getContentResolver().getType(contentUri) == null, "provider MIME exceptions return null");
    var contentManifest = manifestNode("manifest", "package", "test.content");
    var contentApp = manifestNode("application"); contentManifest.children.add(contentApp);
    var contentActivity = manifestNode("activity", "name", ".Viewer"); contentApp.children.add(contentActivity);
    var contentFilter = manifestNode("intent-filter"); contentActivity.children.add(contentFilter);
    contentFilter.children.add(manifestNode("action", "name", "view"));
    contentFilter.children.add(manifestNode("data", "scheme", "content"));
    check(new PackageModel(contentManifest).resolveActivities(new android.content.Intent("view", contentUri), 0).size() == 1,
        "provider MIME exception does not abort scheme-only activity resolution");
    boolean providerError = false;
    try { obbContext.getContentResolver().getType(android.net.Uri.parse("content://test.images/error")); }
    catch (AssertionError expected) { providerError = true; }
    check(providerError, "ContentResolver preserves provider Errors");
    check(providerWarnings.size() == 2 && providerWarnings.stream().noneMatch(s -> s.contains("private") || s.contains("content://")),
        "provider MIME failures warn without exposing URI or provider details");
    RuntimeEnvironment.get().contentProviders.clear();
    check(RuntimeEnvironment.toGuestPath(Environment.getDataDirectory().toString()).equals("/data"),
        "Environment.getDataDirectory projects the Android data partition");
    check(RuntimeEnvironment.toGuestPath(Environment.getRootDirectory().toString()).equals("/system"),
        "Environment.getRootDirectory projects the Android system partition");
    check(RuntimeEnvironment.toGuestPath(Environment.getExternalStorageDirectory().toString()).equals("/storage/emulated/0"),
        "Environment.getExternalStorageDirectory projects Quest primary storage");
    check(RuntimeEnvironment.toGuestPath(obbContext.getExternalFilesDir(null).toString()).equals("/storage/emulated/0/Android/data/test.game/files"),
        "Context.getExternalFilesDir projects Quest primary files");
    check(RuntimeEnvironment.toGuestPath(obbContext.getExternalCacheDir().toString()).equals("/storage/emulated/0/Android/data/test.game/cache"),
        "Context.getExternalCacheDir projects Quest primary cache");
    check(RuntimeEnvironment.toGuestPath(obbContext.getObbDir().toString()).equals("/storage/emulated/0/Android/obb/test.game"),
        "Context.getObbDir projects Quest primary expansion storage");
    for (String prefix : List.of("/sdcard", "/storage/emulated/0")) {
      String library = prefix + "/Android/data/test.game/files/libplugin.so";
      NativeLoader.load(library);
      check(libraryLoads.getLast().equals(library), "System.load accepts external storage alias " + prefix);
    }
    var explicitLibraryLoader = new ClassLoader(null) {};
    NativeLoader.loadLibrary0(Runtime.getRuntime(), explicitLibraryLoader, "plugin");
    check(libraryLoads.getLast().equals("libplugin.so") && libraryLoaders.getLast() == explicitLibraryLoader,
        "Android Runtime reflected library load preserves its explicit initiating loader");
    NativeLoader.loadLibrary0(Runtime.getRuntime(), (ClassLoader) null, "bootplugin");
    check(libraryLoaders.getLast() == null, "Android Runtime null loader retains bootstrap namespace");
    NativeLoader.loadLibrary0(Runtime.getRuntime(), FrameworkTest.class, "classplugin");
    check(libraryLoaders.getLast() == FrameworkTest.class.getClassLoader(),
        "Android Runtime caller-class library load preserves the caller's loader");
    NativeLoader.loadLibrary0(Runtime.getRuntime(), explicitLibraryLoader, FrameworkTest.class, "explicitplugin");
    check(libraryLoaders.getLast() == explicitLibraryLoader,
        "Android Runtime explicit loader is not replaced by caller-class loader");
    NativeLoader.load0(Runtime.getRuntime(), explicitLibraryLoader, "/data/plugin.so");
    check(libraryLoads.getLast().equals("/data/plugin.so") && libraryLoaders.getLast() == explicitLibraryLoader,
        "Android Runtime absolute native path preserves its initiating loader");
    int acceptedLibraryLoads = libraryLoads.size();
    boolean invalidRuntimeName = false;
    try { NativeLoader.loadLibrary0(Runtime.getRuntime(), explicitLibraryLoader, "bad/name"); }
    catch (UnsatisfiedLinkError expected) { invalidRuntimeName = true; }
    check(invalidRuntimeName && libraryLoads.size() == acceptedLibraryLoads,
        "Android Runtime invalid library name fails before host linker entry");
    boolean nullRuntimeReceiver = false;
    try { NativeLoader.loadLibrary0(null, explicitLibraryLoader, "plugin"); }
    catch (NullPointerException expected) { nullRuntimeReceiver = true; }
    check(nullRuntimeReceiver && libraryLoads.size() == acceptedLibraryLoads,
        "Android Runtime alias preserves null receiver failure");
    var obbDirectory = obbContext.getObbDir();
    check(Arrays.equals(obbContext.getObbDirs(), new java.io.File[]{obbDirectory}), "getObbDirs contains getObbDir");
    check(Arrays.equals(obbDirectory.list(), new String[]{"main.1.test.game.obb"}), "Java OBB listing excludes APK and plan");
    check(!new java.io.File(obbDirectory, "plan.json").exists()
        && !new java.io.File(obbDirectory, "base.apk").exists(), "Java OBB child metadata excludes installer files");
    try (var stream = new java.io.FileInputStream(new java.io.File(obbDirectory, "main.1.test.game.obb"))) {
      check(new String(stream.readAllBytes()).equals("expansion"), "Java child File streams read OBB bytes");
    }
    Files.writeString(obbView.resolve("late.bundle"), "asset published during session");
    check(new HashSet<>(Arrays.asList(obbDirectory.list())).equals(Set.of("main.1.test.game.obb", "late.bundle")),
        "Java directory sees session asset publication without remounting");
    check(new java.io.File(Environment.getExternalStorageDirectory(), "Android/obb/test.game").equals(obbDirectory),
        "Environment and Context share one OBB view without additional links");
    try (var paths = Files.walk(obbRoot)) {
      for (var path : paths.sorted(Comparator.reverseOrder()).toList()) Files.delete(path);
    }
    choreographerFrameClock();
    System.out.println("PASS " + checks + " framework checks");
  }
  private static void hostQuitFixture() throws Exception {
    Path root = Files.createTempDirectory("venture-host-quit-").toAbsolutePath();
    List<String> lifecycleLogs = new java.util.concurrent.CopyOnWriteArrayList<>();
    Host host = (Host) java.lang.reflect.Proxy.newProxyInstance(Host.class.getClassLoader(),
        new Class<?>[]{Host.class}, (proxy, method, values) -> {
          if (method.getName().equals("readAsset"))
            return "AndroidManifest.xml".equals(values[0]) ? obbManifest() : null;
          if (method.getName().equals("log")) {
            if ("Lifecycle".equals(values[1])) lifecycleLogs.add((String) values[2]);
            return null;
          }
          if (method.getName().equals("setupShadersDiskCache")
              || method.getName().equals("releaseSurface")) return null;
          if (method.getName().equals("createSurface")) return 1L;
          throw new AssertionError("unexpected Host call " + method);
        });
    RuntimeEnvironment.configure(new String[]{"test.game", root.resolve("base.apk").toString(),
        root.resolve("data").toString(), root.resolve("cache").toString(), root.resolve("obb").toString(),
        root.resolve("lib").toString(), "32", root.resolve("app.jar").toString()}, host);
    Looper.prepareMainLooper();
    try {
      preferenceWriteBarrier(root);
      hostQuitLifecycle();
      var callbacks = lifecycleLogs.stream().filter(s -> s.startsWith("Activity on"))
          .map(s -> s.substring(0, s.indexOf(" activity="))).toList();
      check(callbacks.equals(List.of("Activity onPause begin", "Activity onPause returned",
          "Activity onStop begin", "Activity onStop returned", "Activity onDestroy begin", "Activity onDestroy returned")),
          "host shutdown logs acknowledged callbacks in order " + callbacks);
      check(lifecycleLogs.stream().filter(s -> s.startsWith("Activity finish requested")).count() == 1,
          "duplicate finish does not schedule duplicate destruction");
      check(lifecycleLogs.get(lifecycleLogs.size() - 1).startsWith("Activity onDestroy returned"),
          "main loop stays alive through callback completion");
    }
    finally {
      try (var paths = Files.walk(root)) {
        for (var path : paths.sorted(Comparator.reverseOrder()).toList()) Files.delete(path);
      }
    }
    System.out.println("PASS " + checks + " host quit lifecycle checks");
  }
  public static class HostQuitActivity extends android.app.Activity {
    static HostQuitActivity instance;
    static final List<String> events = new ArrayList<>();
    protected void onCreate(Bundle state) { instance = this; }
    protected void onResume() { events.add("resume"); }
    protected void onPause() {
      events.add("pause");
      getSharedPreferences("quit-save", 0).edit().putInt("checkpoint", 42).apply();
      try { Files.writeString(getFilesDir().toPath().resolve("pause-save.txt"), "saved-on-pause"); }
      catch (java.io.IOException failure) { throw new IllegalStateException(failure); }
    }
    protected void onStop() { events.add("stop"); }
    protected void onDestroy() {
      check((AppMain.quitProgress() & 3) == 3,
          "pause/stop and preference barrier precede destruction");
      check(new XmlPreferences(getDataDir().toPath().resolve("shared_prefs/quit-save.xml"))
                .getInt("checkpoint", 0) == 42,
          "pause apply is durable before destruction without explicit commit");
      events.add("destroy");
      new Handler(Looper.getMainLooper()).post(() -> events.add("destroy-job"));
    }
  }
  private static void preferenceWriteBarrier(Path root) throws Exception {
    // QueuedWork owns preference write accounting, as in AOSP: the stop barrier
    // waits for accepted writes still running, then reports them durable.
    var preferences = new XmlPreferences(root.resolve("barrier/prefs.xml"));
    var writeRunning = new CountDownLatch(1);
    var releaseWrite = new CountDownLatch(1);
    android.app.QueuedWork.queueWrite(() -> {
      writeRunning.countDown();
      try { return releaseWrite.await(10, TimeUnit.SECONDS); }
      catch (InterruptedException failure) { Thread.currentThread().interrupt(); return false; }
    });
    check(writeRunning.await(10, TimeUnit.SECONDS), "in-flight write starts");
    preferences.edit().putInt("checkpoint", 17).apply();
    var result = new java.util.concurrent.atomic.AtomicReference<Boolean>();
    var drainer = new Thread(() -> result.set(XmlPreferences.finishPendingWrites()), "preferences-stop-barrier");
    drainer.start();
    try {
      long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
      while (drainer.isAlive() && drainer.getState() != Thread.State.WAITING
          && System.nanoTime() < end) Thread.sleep(1);
      check(drainer.getState() == Thread.State.WAITING && result.get() == null,
          "stop waits for an in-flight preference write to finish");
    } finally { releaseWrite.countDown(); drainer.join(10000); }
    check(!drainer.isAlive() && Boolean.TRUE.equals(result.get()), "stop barrier drains completed writes");
    check(new XmlPreferences(root.resolve("barrier/prefs.xml")).getInt("checkpoint", 0) == 17,
        "drained preferences survive reopening");
  }
  private static void hostQuitLifecycle() throws Exception {
    var env = RuntimeEnvironment.get();
    env.application = new android.app.Application();
    env.application.ventureAttach(new android.content.Context());
    var info = new android.content.pm.ActivityInfo();
    info.name = HostQuitActivity.class.getName();
    info.packageName = env.manifest.info.packageName;
    env.manifest.activities.add(info);
    var handler = new Handler(Looper.getMainLooper());
    AppMain.startActivity(new android.content.Intent().setComponent(
        new android.content.ComponentName(info.packageName, info.name)));
    handler.post(() -> {
      check(HostQuitActivity.instance != null, "host quit activity launched");
      check(AppMain.requestTaskRemoval(), "menu quit task removal posted to main Looper");
      handler.post(() -> {
        var activity = HostQuitActivity.instance;
        check(HostQuitActivity.events.equals(List.of("resume", "pause", "stop")),
            "task removal pauses and stops the Activity " + HostQuitActivity.events);
        check(!activity.isFinishing() && !activity.isDestroyed(), "task removal neither finishes nor destroys");
        check((AppMain.quitProgress() & 7) == 7, "stop completes the lifecycle acknowledgement on task removal");
        check(AppMain.requestShutdown(), "host shutdown posted to main Looper");
        handler.post(() -> {
          check(activity.isFinishing() && !activity.isDestroyed(), "shutdown requests finish without reentrant destruction");
          check(HostQuitActivity.events.equals(List.of("resume", "pause", "stop")), "finish returns before destroy");
          activity.finish(); // Duplicate requests must not schedule duplicate callbacks.
          AppMain.shutdown(); // Must accept onDestroy's work after queued finish.
        });
      });
    });
    handler.postDelayed(() -> { throw new AssertionError("host quit lifecycle did not complete"); }, 10000);
    Looper.loop();
    check(HostQuitActivity.events.equals(List.of("resume", "pause", "stop", "destroy", "destroy-job")),
        "host shutdown finishes the Activity and drains onDestroy work before quitting the main Looper " + HostQuitActivity.events);
    check((AppMain.quitProgress() & 7) == 7, "quit progress retains all acknowledged stages");
    check(Files.readString(HostQuitActivity.instance.getFilesDir().toPath().resolve("pause-save.txt")).equals("saved-on-pause"),
        "host shutdown preserves the pause-time save");
    // A failed preference write must never admit successful OS termination.
    Path invalid = HostQuitActivity.instance.getDataDir().toPath().resolve("not-a-directory");
    Files.writeString(invalid, "occupied");
    check(!new XmlPreferences(invalid.resolve("prefs.xml")).edit().putInt("checkpoint", 99).commit(),
        "preference write failure is reported");
    check(!XmlPreferences.finishPendingWrites() && !XmlPreferences.writesQuiet(),
        "failed save keeps the termination barrier closed");
  }
  private static void choreographerFrameClock() throws Exception {
    // XR pulses carry the predicted display time. While pulses pause, fallback
    // frames continue that display grid and clock; frame times never decrease,
    // and a pulse landing on the interval a fallback frame served runs its
    // callbacks at the next interval. A long period keeps the wall-clock margins
    // far above scheduler jitter.
    final long period = 200_000_000L, lead = 500_000_000L;
    HandlerThread thread = new HandlerThread("choreographer-clock-test");
    thread.start();
    Handler handler = new Handler(thread.getLooper());
    List<Long> times = new CopyOnWriteArrayList<>(), walls = new CopyOnWriteArrayList<>();
    long[] hostFrame = new long[1], resumed = new long[1];
    CountDownLatch done = new CountDownLatch(1);
    long first = System.nanoTime() + lead;
    android.view.Choreographer.dispatchHostVsync(first, period);
    handler.post(() -> android.view.Choreographer.getInstance().postFrameCallback(new android.view.Choreographer.FrameCallback() {
      public void doFrame(long time) {
        times.add(time);
        walls.add(System.nanoTime());
        int n = times.size();
        if (n == 5) { done.countDown(); return; }
        android.view.Choreographer.getInstance().postFrameCallback(this);
        if (n == 2) { // pulses resume on the interval this fallback frame served
          resumed[0] = System.nanoTime() + lead;
          android.view.Choreographer.dispatchHostVsync(resumed[0], period);
        }
        if (n == 3) // a pulse three quarters of a period after a fallback frame
          handler.postDelayed(() -> {
            hostFrame[0] = System.nanoTime() + lead;
            android.view.Choreographer.dispatchHostVsync(hostFrame[0], period);
          }, period * 3 / 4 / 1_000_000);
      }
    }));
    boolean finished = done.await(10, TimeUnit.SECONDS);
    thread.quitSafely();
    thread.join(3000);
    check(finished, "Choreographer frames continue across paused and resumed XR pulses " + times);
    check((times.get(0) - first) % period == 0, "fallback frame continues the host display grid " + times);
    for (int i = 1; i < times.size(); i++) {
      check(times.get(i) > times.get(i - 1), "Choreographer frame time never goes backwards " + times);
      check(walls.get(i) - walls.get(i - 1) >= period / 2, "one Choreographer frame per display interval " + walls);
    }
    check(walls.get(2) - walls.get(1) <= period * 3 / 2 && times.get(2) == resumed[0] + period,
        "pulse on a served interval runs callbacks at the next one " + times + " " + walls);
    check(times.get(3) == hostFrame[0], "resumed host pulse keeps its display frame time " + times);
    choreographerWaitsForEachFrame();
    choreographerDisplayModeEvent();
  }
  private static void choreographerDisplayModeEvent() throws Exception {
    final long original = 200_000_000L, changed = 400_000_000L;
    HandlerThread thread = new HandlerThread("choreographer-mode-test");
    thread.start();
    Handler handler = new Handler(thread.getLooper());
    List<Long> times = new CopyOnWriteArrayList<>();
    CountDownLatch done = new CountDownLatch(1);
    long first = System.nanoTime();
    android.view.Choreographer.dispatchHostVsync(first, original);
    handler.post(() -> {
      var choreographer = android.view.Choreographer.getInstance();
      choreographer.postFrameCallback(new android.view.Choreographer.FrameCallback() {
        public void doFrame(long time) {
          times.add(time);
          if (times.size() == 2) done.countDown();
          else choreographer.postFrameCallback(this);
        }
      });
      handler.postDelayed(() -> RuntimeEnvironment.setDisplayPeriod(changed), 50);
    });
    boolean finished = done.await(5, TimeUnit.SECONDS);
    thread.quitSafely();
    thread.join(3000);
    check(finished, "mode events rearm pending Android callbacks without another XR pulse");
    check((times.get(0) - first) % changed == 0, "pending callback uses the new display grid " + times);
    check(times.get(1) - times.get(0) >= changed, "fallback preserves the changed period " + times);
  }
  private static void choreographerWaitsForEachFrame() throws Exception {
    // A guest whose XR loop waits for each Android frame, with a runtime that
    // returns at once for a late app: every pulse lands right after the fallback
    // frame that released it. Frames must keep the display rate, not 2 periods.
    final long period = 200_000_000L, lead = 500_000_000L;
    HandlerThread thread = new HandlerThread("choreographer-paced-test");
    thread.start();
    List<Long> times = new CopyOnWriteArrayList<>(), walls = new CopyOnWriteArrayList<>();
    List<Long> pulses = new CopyOnWriteArrayList<>();
    CountDownLatch done = new CountDownLatch(1);
    android.view.Choreographer.dispatchHostVsync(System.nanoTime() + lead, period);
    new Handler(thread.getLooper()).post(() -> android.view.Choreographer.getInstance().postFrameCallback(
        new android.view.Choreographer.FrameCallback() {
      public void doFrame(long time) {
        times.add(time);
        walls.add(System.nanoTime());
        if (times.size() == 6) { done.countDown(); return; }
        android.view.Choreographer.getInstance().postFrameCallback(this);
        long predicted = System.nanoTime() + lead;
        pulses.add(predicted);
        android.view.Choreographer.dispatchHostVsync(predicted, period);
      }
    }));
    boolean finished = done.await(10, TimeUnit.SECONDS);
    thread.quitSafely();
    thread.join(3000);
    check(finished, "Android-paced XR loop keeps receiving frames " + times);
    for (int i = 1; i < times.size(); i++) {
      check(walls.get(i) - walls.get(i - 1) >= period / 2, "Android-paced XR loop keeps the display rate " + walls);
      check(times.get(i) == pulses.get(i - 1) + period, "paced frame continues the pulse clock " + times);
    }
    // The failure mode is one frame per two periods. Bound the whole run rather than
    // each gap, so one host scheduling delay under a loaded test machine is tolerated.
    long span = walls.get(walls.size() - 1) - walls.get(0);
    check(span <= (walls.size() - 1) * period * 3 / 2, "Android-paced XR loop keeps the display rate " + walls);
  }
  static byte[] obbManifest() {
    var b = java.nio.ByteBuffer.allocate(512).order(java.nio.ByteOrder.LITTLE_ENDIAN);
    b.putInt(0x00080003).putInt(0);
    int pool = b.position();
    b.putInt(0x001c0001).putInt(0).putInt(4).putInt(0).putInt(0x100).putInt(44).putInt(0);
    for (int i = 0; i < 4; i++) b.putInt(0);
    int start = b.position(), i = 0;
    for (String word : new String[]{"manifest", "package", "test.game", "application"}) {
      b.putInt(pool + 28 + 4 * i++, b.position() - start);
      b.put((byte)word.length()).put((byte)word.length()).put(word.getBytes(java.nio.charset.StandardCharsets.UTF_8)).put((byte)0);
    }
    while ((b.position() & 3) != 0) b.put((byte)0);
    b.putInt(pool + 4, b.position() - pool);
    // AOSP ResXMLTree_node + attrExt; one unnamespaced package attribute.
    b.putInt(0x00100102).putInt(56).putInt(1).putInt(-1).putInt(-1).putInt(0)
        .putInt(0x00140014).putInt(1).putInt(0);
    b.putInt(-1).putInt(1).putInt(2).putInt(0x03000008).putInt(2);
    b.putInt(0x00100102).putInt(36).putInt(1).putInt(-1).putInt(-1).putInt(3)
        .putInt(0x00140014).putInt(0).putInt(0);
    for (int name : new int[]{3, 0})
      b.putInt(0x00100103).putInt(24).putInt(1).putInt(-1).putInt(-1).putInt(name);
    b.putInt(4, b.position());
    return Arrays.copyOf(b.array(), b.position());
  }

}
