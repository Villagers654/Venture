package com.venture.dex;

import java.io.*;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.math.RoundingMode;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.*;
import java.nio.file.*;
import java.nio.file.attribute.BasicFileAttributes;
import java.nio.file.attribute.PosixFilePermissions;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.*;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.*;

/** Convert/verify APK bytecode. Exit 2 means the cache is NOT launchable; see report.json.
 *
 * java -cp <tools> com.venture.dex.ApkCache base.apk --package com.example --cache CACHE
 *     [--framework JAR]... [--hierarchy TSV]... [--force] [--result-file FILE]
 *
 * No APK is modified, no application static initializer is run, and no missing class is
 * synthesized. This is the launch-path driver: it runs on the bundled JDK, so launches need
 * no Python. It keeps the cache layout, identities, locking and verification gates of the
 * former tools/dex/convert_apk.py driver. Its own dependency root (jdk/, asm/, dex2jar/) is
 * two levels above its class directory: bin/java for java/dex/classes, .deps for
 * .deps/dex-tools/<digest>.
 */
public final class ApkCache {
    // Framework package changed: never reuse converted classes linked to the old package.
    static final String PIPELINE = "dex2jar-2.4/asm-9.8/venture-3";
    static final boolean WINDOWS = File.separatorChar == '\\';
    // Verification/compiler options come from this pipeline, not launcher-wide
    // Java option injection (which could change the verifier or its class path).
    static final Set<String> BLOCKED_ENVIRONMENT = Set.of("JAVA_TOOL_OPTIONS", "_JAVA_OPTIONS", "JDK_JAVA_OPTIONS", "CLASSPATH");
    static Path tools, deps;

    public static void main(String[] args) throws Exception {
        tools = Path.of(ApkCache.class.getProtectionDomain().getCodeSource().getLocation().toURI()).toAbsolutePath().normalize();
        deps = tools.getParent().getParent();
        if (args.length == 1 && args[0].equals("--translation-identity")) {
            // Build/staging only: hash the immutable bundled toolchain once.
            System.out.println(dumps(translationIdentityUncached(), true, -1));
            return;
        }
        long started = System.nanoTime();
        String apk = null, pkg = null, cache = null, resultFile = null;
        boolean force = false;
        List<Path> framework = new ArrayList<>(), hierarchy = new ArrayList<>();
        for (int i = 0; i < args.length; ++i) {
            String arg = args[i];
            int eq = arg.startsWith("--") ? arg.indexOf('=') : -1;
            String option = eq > 0 ? arg.substring(0, eq) : arg;
            switch (option) {
                case "--package", "--cache", "--framework", "--hierarchy", "--result-file" -> {
                    String value = eq > 0 ? arg.substring(eq + 1) : ++i < args.length ? args[i] : null;
                    if (value == null) usage("argument " + option + ": expected one argument");
                    switch (option) {
                        case "--package" -> pkg = value;
                        case "--cache" -> cache = value;
                        case "--framework" -> framework.add(Path.of(value));
                        case "--hierarchy" -> hierarchy.add(Path.of(value));
                        default -> resultFile = value;
                    }
                }
                case "--force" -> force = true;
                default -> {
                    if ((arg.startsWith("-") && arg.length() > 1) || apk != null) usage("unrecognized arguments: " + arg);
                    apk = arg;
                }
            }
        }
        if (apk == null || pkg == null || cache == null) usage("the following arguments are required: apk, --package, --cache");
        Map.Entry<Path, Map<String, Object>> converted = convert(Path.of(apk), pkg, Path.of(cache), framework, force, hierarchy);
        Path dest = converted.getKey();
        Map<String, Object> report = converted.getValue();
        if (!report.containsKey("ready") || !report.containsKey("failure_count")) throw new IllegalStateException("incomplete report.json");
        Map<String, Object> result = new LinkedHashMap<>();
        result.put("jar", dest.resolve("app.jar").toString());
        result.put("report", dest.resolve("report.json").toString());
        result.put("ready", report.get("ready"));
        result.put("translation_complete", report.getOrDefault("translation_complete", false));
        result.put("classes", report.get("jar_classes"));
        result.put("failures", report.get("failure_count"));
        result.put("seconds", round(seconds(started), 3));
        String text = dumps(result, false, -1);
        if (resultFile != null) Files.writeString(absolute(Path.of(resultFile)), text, StandardCharsets.UTF_8);
        System.out.println(text);
        System.out.flush();
        System.exit(truthy(report.get("ready")) ? 0 : 2);
    }

    static void usage(String error) {
        System.err.println("usage: ApkCache [-h] --package PACKAGE --cache CACHE [--framework FRAMEWORK] [--hierarchy HIERARCHY] [--force] [--result-file RESULT_FILE] apk");
        System.err.println("ApkCache: error: " + error);
        System.exit(2);
    }

    @SuppressWarnings("try")  // the lock is held for the scope, never referenced
    static Map.Entry<Path, Map<String, Object>> convert(Path apk, String pkg, Path cache, List<Path> framework,
                                                       boolean force, List<Path> hierarchy) throws Exception {
        apk = absolute(apk);
        cache = absolute(cache);
        List<Path> frameworks = new ArrayList<>(), ancestry = new ArrayList<>();
        for (Path p : framework) frameworks.add(absolute(p));
        for (Path p : hierarchy) ancestry.add(absolute(p));
        String key = sha256(pkg.getBytes(StandardCharsets.UTF_8)).substring(0, 32);
        try (FileChannel lock = exclusiveFile(cache.resolve(".locks").resolve(key))) {
            var result = convertLocked(apk, pkg, cache, frameworks, force, ancestry);
            // The runtime's own <cache>/<package>/<version>/java holds one package
            // only; shared CLI caches are never pruned.
            Path version = cache.getParent(), owner = version == null ? null : version.getParent();
            if (cache.getFileName().toString().equals("java") && owner != null
                    && owner.getFileName().toString().equals(pkg))
                pruneGenerations(cache, result.getKey(), result.getValue());
            return result;
        }
    }

    /** Each Venture update converts the APK again; earlier generations are only
     *  disk use. Called under the package lock, so no other conversion is in
     *  progress. A file a running JVM still holds open stays until a later launch. */
    static void pruneGenerations(Path cache, Path current, Map<String, Object> report) {
        Set<Path> keep = new HashSet<>();
        keep.add(absolute(current));
        Object raw = report.get("translation_report");
        // Reports record extended-length paths (filesystemString).
        String slash = String.valueOf((char) 92), prefix = slash + slash + "?" + slash, text = raw instanceof String s ? s : "";
        if (text.startsWith(prefix + "UNC" + slash)) text = slash + slash + text.substring(prefix.length() + 4);
        else if (text.startsWith(prefix)) text = text.substring(prefix.length());
        if (!text.isEmpty()) keep.add(absolute(Path.of(text)).getParent());
        try (DirectoryStream<Path> entries = Files.newDirectoryStream(cache)) {
            for (Path entry : entries) {
                String name = entry.getFileName().toString();
                if (name.equals(".locks") || (name.equals("translation") && text.isEmpty())) continue;
                if (!name.equals("linked") && !name.equals("translation")) {
                    deleteTree(entry);  // earlier layouts and abandoned work directories
                    continue;
                }
                try (DirectoryStream<Path> contexts = Files.newDirectoryStream(entry)) {
                    for (Path context : contexts) {
                        boolean holds = false;
                        for (Path kept : keep) holds |= absolute(context).equals(kept.getParent());
                        if (!holds) {
                            deleteTree(context);
                            continue;
                        }
                        try (DirectoryStream<Path> generations = Files.newDirectoryStream(context)) {
                            for (Path generation : generations)
                                if (Files.isDirectory(generation, LinkOption.NOFOLLOW_LINKS) && !keep.contains(absolute(generation)))
                                    deleteTree(generation);
                        }
                    }
                }
            }
        } catch (IOException e) {
            // Pruning is best effort; the conversion itself succeeded.
        }
    }

    /** Deletes what it can; never follows links (a link is removed, not its target). */
    static void deleteTree(Path root) {
        try {
            Files.walkFileTree(root, new SimpleFileVisitor<>() {
                @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attributes) {
                    try { Files.delete(file); } catch (IOException e) { /* in use */ }
                    return FileVisitResult.CONTINUE;
                }
                @Override public FileVisitResult visitFileFailed(Path file, IOException e) {
                    return FileVisitResult.CONTINUE;
                }
                @Override public FileVisitResult postVisitDirectory(Path directory, IOException e) {
                    try { Files.delete(directory); } catch (IOException ignored) { /* not empty */ }
                    return FileVisitResult.CONTINUE;
                }
            });
        } catch (IOException e) {
            // best effort
        }
    }

    static Map.Entry<Path, Map<String, Object>> convertLocked(Path apk, String pkg, Path cache, List<Path> framework,
                                                             boolean force, List<Path> hierarchy) throws Exception {
        if (pkg.isEmpty() || !pkg.matches("[a-zA-Z0-9._]+") || pkg.equals(".") || pkg.equals(".."))
            throw new IllegalArgumentException("invalid package name");
        long started = System.nanoTime();
        List<String> names = new ArrayList<>();
        List<byte[]> dexes = new ArrayList<>();
        Set<String> classes = new HashSet<>();
        Set<List<Object>> natives = new HashSet<>();
        String dexsha;
        try (ZipFile archive = new ZipFile(apk.toFile())) {
            dexsha = DexSurface.identityApk(archive, names);
        }
        Map<String, Object> phases = new LinkedHashMap<>();
        phases.put("dex_identity", seconds(started));
        // Distributed tools are immutable compiled classes with all dependencies in java/.
        if (!Files.isRegularFile(tools.resolve("ready"))) throw new IllegalStateException("incomplete bundled DEX tools");
        Map<String, Object> identity = conversionIdentity(pkg, dexsha, framework, hierarchy);
        @SuppressWarnings("unchecked") Map<String, Object> translation = (Map<String, Object>)identity.get("translation");
        // Different frameworks/rewriters must coexist: HotSpot keeps loaded JARs
        // open on Windows, so replacing a DEX-only cache entry can fail mid-launch.
        String artifactKey = identityKey(identity);
        // The runtime supplies <cache>/<package>/<version>/java already. Avoid
        // adding the package twice; identities also keep shared CLI caches safe.
        Path directory = cache;
        Path context = directory.resolve("linked").resolve(artifactKey);
        if (!force) {
            var cached = cachedArtifact(context, identity);
            if (cached != null && truthy(cached.getValue().get("translation_complete"))) return cached;
        }
        // A warm artifact needs only content identity and the verified JAR.
        // Allocate and parse DEX classes/native declarations only on a miss.
        long censusStarted = System.nanoTime();
        names.clear();
        try (ZipFile archive = new ZipFile(apk.toFile())) {
            if (!dexsha.equals(DexSurface.readApk(archive, names, dexes, classes, natives)))
                throw new IOException("APK changed while preparing conversion");
        }
        phases.put("dex_census", seconds(censusStarted));
        Files.createDirectories(context.getParent());
        Path work = mkdtemp(context.getParent());
        Map<String, Object> report = new LinkedHashMap<>();
        report.put("package", pkg);
        report.put("dex_sha256", dexsha);
        report.put("identity", identity);
        report.put("ready", false);
        report.put("dex_classes", classes.size());
        report.put("dex_natives", natives.size());
        List<Object> failures = new ArrayList<>();
        report.put("failures", failures);
        try {
            long phaseStarted = System.nanoTime();
            Object[] translated = translate(names, dexes, dexsha, directory, translation, force);
            dexes.clear();
            Path raw = (Path) translated[0];
            @SuppressWarnings("unchecked") Map<String, Object> rawReport = (Map<String, Object>) translated[1];
            boolean hit = (Boolean) translated[2];
            report.put("translation_cache_hit", hit);
            report.put("translation_report", filesystemString(raw.resolve("report.json")));
            failures.addAll((List<?>) Objects.requireNonNull(rawReport.get("failures"), "failures"));
            phases.put("translation", seconds(phaseStarted));
            if (!hit) {
                Map<?, ?> rawPhases = (Map<?, ?>) rawReport.get("phases");
                phases.put("dex_parse", getOrZero(rawPhases, "parse"));
                phases.put("translate", getOrZero(rawPhases, "translate"));
                phases.put("translation_write", getOrZero(rawPhases, "write"));
            }
            Path merged = raw.resolve("app.jar");
            if (Files.exists(merged)) {
                String cp = String.join(File.pathSeparator, tools.toString(), deps.resolve("asm/asm.jar").toString());
                List<String> verifyEntries = new ArrayList<>();
                for (Path p : framework) verifyEntries.add(p.toString());
                verifyEntries.add(tools.resolve("venture-java-bridge.jar").toString());
                String verifyCp = String.join(File.pathSeparator, verifyEntries);
                List<String> ancestry = new ArrayList<>();
                for (Path p : hierarchy) ancestry.add(p.toString());
                phaseStarted = System.nanoTime();
                run(List.of(javaTool("java"), "-cp", cp, "com.venture.dex.Rewrite", merged.toString(),
                        work.resolve("app.jar").toString(), work.resolve("census.tsv").toString(), verifyCp,
                        String.join(File.pathSeparator, ancestry)), work.resolve("link.log"), true);
                phases.put("link", seconds(phaseStarted));
                for (var phase : javaPhases(work.resolve("link.log")).entrySet()) phases.put("link_" + phase.getKey(), phase.getValue());
                Set<String> emitted = new HashSet<>();
                Set<List<Object>> declared = new HashSet<>();
                int rewrites = 0;
                for (String line : splitlines(readText(work.resolve("census.tsv")))) {
                    String[] row = line.split("\t", -1);
                    switch (row[0]) {
                        case "C" -> emitted.add(at(row, 1));
                        case "N" -> {
                            List<Object> tuple = new ArrayList<>(Arrays.asList(row).subList(1, Math.min(4, row.length)));
                            tuple.add(at(row, 4).equals("true"));
                            declared.add(tuple);
                        }
                        case "E" -> failures.add(map("stage", "converter-method", "class", at(row, 1), "method", at(row, 2), "detail", at(row, 3)));
                        case "R" -> ++rewrites;
                        case "F" -> failures.add(map("stage", "frames", "class", at(row, 1), "method", at(row, 2), "detail", at(row, 3)));
                        default -> { }
                    }
                }
                report.put("jar_classes", emitted.size());
                report.put("jar_natives", declared.size());
                report.put("rewrites", rewrites);
                declarations(failures, "class", classes, emitted);
                declarations(failures, "native", natives, declared);
                phaseStarted = System.nanoTime();
                int verified = run(List.of(javaTool("java"), "-Xverify:all", "-cp", tools.toString(), "com.venture.dex.Verify",
                        work.resolve("app.jar").toString(), work.resolve("verification.jsonl").toString(), verifyCp),
                        work.resolve("verification.log"), false);
                phases.put("verification", seconds(phaseStarted));
                phases.putAll(javaPhases(work.resolve("verification.log")));
                if (Files.exists(work.resolve("verification.jsonl"))) {
                    for (String line : splitlines(readText(work.resolve("verification.jsonl")))) {
                        Map<String, Object> failure = new LinkedHashMap<>();
                        failure.put("stage", "hotspot");
                        @SuppressWarnings("unchecked") Map<String, Object> parsed = (Map<String, Object>) parseJson(line);
                        failure.putAll(parsed);
                        failures.add(failure);
                    }
                }
                boolean hotspot = false;
                for (Object f : failures) hotspot |= "hotspot".equals(((Map<?, ?>) f).get("stage"));
                if (verified != 0 && !hotspot)
                    failures.add(map("stage", "hotspot", "exit", verified, "detail", "verifier process failed; see verification.log"));
                report.put("jar_sha256", fileHash(work.resolve("app.jar")));
            }
            report.put("ready", Files.exists(merged) && failures.isEmpty());
            // Android applications routinely bundle optional integrations whose SDK
            // dependencies are absent. Preserve strict eager-verification status,
            // but distinguish missing linkage from lost/invalid translated code.
            // The runtime may load intact classes lazily with HotSpot -Xverify:all;
            // a deferred class still fails verification/linkage if actually reached.
            boolean deferred = true;
            for (Object o : failures) deferred &= deferredLinkage((Map<?, ?>) o);
            report.put("translation_complete", Files.exists(merged) && deferred);
        } catch (Exception e) {
            failures.add(map("stage", "pipeline", "detail", message(e)));
        }
        report.put("seconds", round(seconds(started), 3));
        Map<String, Object> rounded = new LinkedHashMap<>();
        for (var phase : phases.entrySet()) rounded.put(phase.getKey(), round(phase.getValue(), 6));
        report.put("phases", rounded);
        report.put("failure_count", failures.size());
        return publish(work, context, report);
    }

    static boolean deferredLinkage(Map<?, ?> f) {
        if ("frames".equals(f.get("stage"))) return str(f, "detail").contains("missing frame hierarchy:");
        if ("hotspot".equals(f.get("stage"))) {
            // A class that does not verify fails when it is reached, as ART
            // fails a method when it runs; bundled SDK code a VR game never
            // reaches must not stop the game.
            Object kind = f.get("kind");
            // HotSpot refuses app classes in java.* packages, which ART loads;
            // Stremio's libVLC bundles Blu-ray menu classes in java.awt and java.io.
            if ("java.lang.SecurityException".equals(kind)) return str(f, "detail").startsWith("Prohibited package name:");
            return "java.lang.NoClassDefFoundError".equals(kind) || "java.lang.ClassNotFoundException".equals(kind)
                || "java.lang.VerifyError".equals(kind);
        }
        return false;
    }

    static <T> void declarations(List<Object> failures, String label, Set<T> expected, Set<T> actual) {
        List<T> missing = new ArrayList<>(), extra = new ArrayList<>();
        for (T e : expected) if (!actual.contains(e)) missing.add(e);
        for (T a : actual) if (!expected.contains(a)) extra.add(a);
        missing.sort(ApkCache::comparePython);
        extra.sort(ApkCache::comparePython);
        for (T m : missing) failures.add(map("stage", "declarations", "missing_" + label, m));
        for (T x : extra) failures.add(map("stage", "declarations", "extra_" + label, x));
    }

    /** Python ordering of the census values: str by code point, bool, tuples lexicographically. */
    static int comparePython(Object a, Object b) {
        if (a instanceof List<?> x && b instanceof List<?> y) {
            for (int i = 0; i < Math.min(x.size(), y.size()); ++i) {
                int c = comparePython(x.get(i), y.get(i));
                if (c != 0) return c;
            }
            return Integer.compare(x.size(), y.size());
        }
        if (a instanceof Boolean x && b instanceof Boolean y) return Boolean.compare(x, y);
        String s = String.valueOf(a), t = String.valueOf(b);
        for (int i = 0, j = 0; i < s.length() && j < t.length(); ) {
            int p = s.codePointAt(i), q = t.codePointAt(j);
            if (p != q) return Integer.compare(p, q);
            i += Character.charCount(p);
            j += Character.charCount(q);
        }
        return Integer.compare(s.codePointCount(0, s.length()), t.codePointCount(0, t.length()));
    }

    /** Raw DEX-to-JAR translation, keyed only by DEX bytes and translator identity. */
    static Object[] translate(List<String> names, List<byte[]> dexes, String dexsha, Path directory,
                              Map<String, Object> translation, boolean force) throws Exception {
        Map<String, Object> identity = new LinkedHashMap<>();
        identity.put("dex", dexsha);
        identity.putAll(translation);
        Path context = directory.resolve("translation").resolve(identityKey(identity));
        var cached = force ? null : cachedArtifact(context, identity);
        if (cached != null) return new Object[]{cached.getKey(), cached.getValue(), true};
        Files.createDirectories(context.getParent());
        Path work = mkdtemp(context.getParent());
        Map<String, Object> report = new LinkedHashMap<>();
        report.put("identity", identity);
        List<Object> failures = new ArrayList<>();
        report.put("failures", failures);
        Map<String, Object> phases = new LinkedHashMap<>();
        long started = System.nanoTime();
        try {
            List<Path> jars = new ArrayList<>();
            List<String> command = new ArrayList<>(List.of(javaTool("java"), "-Xmx2g", "-cp",
                String.join(File.pathSeparator, tools.toString(), deps.resolve("asm/asm.jar").toString(), deps.resolve("dex2jar/lib") + File.separator + "*"),
                "com.venture.dex.Convert"));
            for (int i = 0; i < names.size(); ++i) {
                Path dex = work.resolve(names.get(i));
                Files.write(dex, dexes.get(i));
                command.add(dex.toString());
                command.add(work.resolve(withSuffix(names.get(i), ".jar")).toString());
                command.add(work.resolve(names.get(i) + ".errors.tsv").toString());
            }
            dexes.clear();  // the files are the converter's input; free the heap copy
            // All files have explicit absolute names. Win32's process working
            // directory cannot be this deep, even with a device-prefixed path.
            int code = run(command, work.resolve("conversion.log"), false);
            for (String name : names) {
                Path jar = work.resolve(withSuffix(name, ".jar"));
                if (code != 0 || !Files.exists(jar)) failures.add(map("stage", "converter", "dex", name, "exit", code));
                else jars.add(jar);
            }
            phases.put("convert", seconds(started));
            phases.putAll(javaPhases(work.resolve("conversion.log")));
            started = System.nanoTime();
            Path app = work.resolve("app.jar");
            if (jars.size() == 1) {
                move(jars.get(0), app);
            } else if (!jars.isEmpty()) {
                try (ZipOutputStream out = new ZipOutputStream(new BufferedOutputStream(Files.newOutputStream(app)))) {
                    Set<String> seen = new HashSet<>();
                    for (Path jar : jars) {
                        try (ZipFile in = new ZipFile(jar.toFile())) {
                            for (var entries = in.entries(); entries.hasMoreElements(); ) {
                                ZipEntry entry = entries.nextElement();
                                String name = entry.getName();
                                if (!name.endsWith(".class")) continue;
                                if (!seen.add(name)) throw new IllegalArgumentException("duplicate converted class " + name);
                                byte[] bytes;
                                try (var stream = in.getInputStream(entry)) { bytes = stream.readAllBytes(); }
                                ZipEntry merged = new ZipEntry(name);
                                merged.setTime(0);  // as Convert's entries: content-only bytes
                                out.putNextEntry(merged);
                                out.write(bytes);
                                out.closeEntry();
                            }
                        }
                    }
                }
                for (Path jar : jars) Files.delete(jar);
            }
            if (!jars.isEmpty()) report.put("jar_sha256", fileHash(app));
            for (Path archive : glob(work, "*error*.zip")) failures.add(map("stage", "converter", "error_archive", archive.getFileName().toString()));
            for (Path methodErrors : glob(work, "*.errors.tsv"))
                for (String line : splitlines(readText(methodErrors))) failures.add(map("stage", "converter-method", "detail", line));
            phases.put("merge", seconds(started));
            report.put("phases", phases);
            var published = publish(work, context, report);
            return new Object[]{published.getKey(), published.getValue(), false};
        } finally {
            if (Files.exists(work)) deleteFlatDirectory(work);
        }
    }

    static Map<String, Object> conversionIdentity(String pkg, String dexsha, List<Path> framework,
                                                   List<Path> hierarchy) throws IOException {
        Map<String, Object> translation = translationIdentity();
        Path dexClasses = tools.resolve("com/venture/dex");
        Map<String, Object> identity = new LinkedHashMap<>();
        identity.put("package", pkg);
        identity.put("pipeline", PIPELINE);
        identity.put("dex", dexsha);
        identity.put("rewriter", hashes(sortedGlob(dexClasses, "*.class")));
        identity.put("driver", fileHash(dexClasses.resolve("ApkCache.class")));
        identity.put("parser", fileHash(dexClasses.resolve("DexSurface.class")));
        identity.put("hierarchy", hashes(hierarchy));
        identity.put("framework", hashes(framework));
        identity.put("translation", translation);
        identity.put("bridge", fileHash(tools.resolve("venture-java-bridge.jar")));
        return identity;
    }

    @SuppressWarnings("unchecked")
    static Map<String, Object> translationIdentity() throws IOException {
        Path manifest = tools.resolve("translation-identity.json");
        if (Files.isRegularFile(manifest)) {
            Map<String, Object> identity = (Map<String, Object>)parseJson(readText(manifest));
            if (!PIPELINE.equals(identity.get("pipeline")))
                throw new IOException("bundled translation identity pipeline mismatch");
            return identity;
        }
        // Developer/CLI builds do not necessarily have a staged immutable
        // bundle. Keep hashing their actual dependency bytes.
        return translationIdentityUncached();
    }

    static Map<String, Object> translationIdentityUncached() throws IOException {
        // Only DEX translation inputs, never framework/bridge code or frame ancestry.
        // Hash the actual compiler/dependency bytes, not just a human version label.
        List<String> runtimeFiles = WINDOWS
            ? List.of("release", "lib/modules", "bin/java.exe", "bin/server/jvm.dll", "bin/java.dll", "bin/zip.dll", "bin/jimage.dll", "bin/jli.dll")
            : List.of("release", "lib/modules", "bin/java", "lib/server/libjvm.so", "lib/libjava.so", "lib/libzip.so", "lib/libjimage.so", "lib/libjli.so");
        Map<String, Object> identity = new LinkedHashMap<>();
        identity.put("pipeline", PIPELINE);
        identity.put("converter", hashes(sortedGlob(tools.resolve("com/venture/dex"), "Convert*.class")));
        List<Path> libraries = new ArrayList<>();
        libraries.add(deps.resolve("asm/asm.jar"));
        libraries.addAll(sortedGlob(deps.resolve("dex2jar/lib"), "*.jar"));
        identity.put("libraries", hashes(libraries));
        List<Path> jdk = new ArrayList<>();
        for (String file : runtimeFiles) jdk.add(deps.resolve("jdk").resolve(file));
        identity.put("jdk", hashes(jdk));
        return identity;
    }

    static Map.Entry<Path, Map<String, Object>> cachedArtifact(Path context, Map<String, Object> identity) {
        try {
            Object generation = ((Map<?, ?>) parseJson(readText(context.resolve("current.json")))).get("generation");
            if (!(generation instanceof String name) || !name.matches("g-[A-Za-z0-9_-]+")) return null;
            Path dest = context.resolve(name);
            @SuppressWarnings("unchecked") Map<String, Object> report = (Map<String, Object>) parseJson(readText(dest.resolve("report.json")));
            if (Objects.equals(report.get("identity"), identity) && fileHash(dest.resolve("app.jar")).equals(report.get("jar_sha256")))
                return Map.entry(dest, report);
        } catch (Exception e) {
            // Missing, partial or foreign entries are cache misses.
        }
        return null;
    }

    static Map.Entry<Path, Map<String, Object>> publish(Path work, Path context, Map<String, Object> report) throws IOException {
        // Published generations are immutable, even under --force or cache repair.
        // HotSpot holds JARs open on Windows; an atomic pointer switches future
        // launches without renaming/replacing anything an existing JVM is using.
        Files.createDirectories(context);
        for (Path dex : glob(work, "*.dex")) Files.delete(dex);
        writeText(work.resolve("report.json"), dumps(report, false, 2).replace("\n", System.lineSeparator()));
        Path dest = context.resolve(work.getFileName().toString());
        move(work, dest);
        Path pointer = context.resolve(work.getFileName() + ".json");
        writeText(pointer, dumps(map("generation", dest.getFileName().toString()), false, -1));
        move(pointer, context.resolve("current.json"), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
        return Map.entry(dest, report);
    }

    /** Windows real-time scanners and indexers briefly open fresh files without
     *  delete sharing, so a rename fails with ACCESS_DENIED until they finish.
     *  Retry for a bounded time, as npm's graceful-fs does for EPERM renames. */
    static void move(Path source, Path target, CopyOption... options) throws IOException {
        long deadline = System.nanoTime() + 10_000_000_000L;
        for (long delay = 10;; delay = Math.min(delay * 2, 500)) {
            try {
                Files.move(source, target, options);
                return;
            } catch (AccessDeniedException e) {
                if (!WINDOWS || System.nanoTime() > deadline) throw e;
                try {
                    Thread.sleep(delay);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    throw e;
                }
            }
        }
    }

    /** Cross-process cache publication lock: byte 0 on Windows (as msvcrt.locking), whole file elsewhere.
     * Nothing is written: LockFileEx locks past EOF, and a write to a range another
     * process holds would fail instead of waiting. */
    static FileChannel exclusiveFile(Path path) throws Exception {
        Files.createDirectories(path.getParent());
        FileChannel channel = FileChannel.open(path, StandardOpenOption.CREATE, StandardOpenOption.READ, StandardOpenOption.WRITE);
        try {
            if (WINDOWS) {
                long deadline = System.nanoTime() + 600_000_000_000L;
                while (channel.tryLock(0, 1, false) == null) {
                    if (System.nanoTime() >= deadline) throw new IOException("cache lock timed out: " + path.getFileName());
                    Thread.sleep(100);
                }
            } else {
                channel.lock();
            }
            return channel;
        } catch (Exception e) {
            channel.close();
            throw e;
        }
    }

    static int run(List<String> command, Path log, boolean check) throws IOException, InterruptedException {
        String launcher = System.getProperty("venture.java.launcher");
        ProcessBuilder builder = new ProcessBuilder(launcher == null ? command : List.of(launcher, "--stdin"));
        builder.environment().keySet().removeIf(name -> BLOCKED_ENVIRONMENT.contains(name.toUpperCase(Locale.ROOT)));
        builder.redirectErrorStream(true);
        builder.redirectOutput(log.toFile());
        if (launcher == null) builder.redirectInput(ProcessBuilder.Redirect.INHERIT);
        Process child = builder.start();
        if (launcher != null) {
            try (OutputStream input = child.getOutputStream()) {
                input.write(dumps(map("command", command), false, -1).getBytes(StandardCharsets.UTF_8));
            }
        }
        int code = child.waitFor();
        if (check && code != 0)
            throw new IOException("Command '" + pyRepr(command) + "' returned non-zero exit status " + code + ".");
        return code;
    }

    static Map<String, Double> javaPhases(Path path) throws IOException {
        Map<String, Double> phases = new LinkedHashMap<>();
        for (String line : splitlines(readText(path))) {
            if (!line.startsWith("phases ")) continue;
            String[] fields = line.trim().split("\\s+");
            for (int i = 1; i < fields.length; ++i) {
                String[] pair = fields[i].split("=", -1);
                if (pair.length != 2) throw new IllegalArgumentException("invalid phase field: " + fields[i]);
                phases.merge(pair[0], Double.parseDouble(pair[1].trim()), Double::sum);
            }
        }
        return phases;
    }

    static String javaTool(String name) {
        // HotSpot/JDK file APIs support long ordinary paths.
        return deps.resolve("jdk/bin").resolve(name + (WINDOWS ? ".exe" : "")).toString();
    }

    static Path absolute(Path path) {
        return path.toAbsolutePath().normalize();
    }

    /** The extended-length form the former driver recorded for report paths. */
    static String filesystemString(Path path) {
        String text = path.toString();
        if (!WINDOWS || text.startsWith("\\\\?\\")) return text;
        return text.startsWith("\\\\") ? "\\\\?\\UNC\\" + text.substring(2) : "\\\\?\\" + text;
    }

    static Path mkdtemp(Path directory) throws IOException {
        // tempfile.mkdtemp(prefix="g-"): eight characters from its alphabet, mode 0700.
        final String alphabet = "abcdefghijklmnopqrstuvwxyz0123456789_";
        SecureRandom random = new SecureRandom();
        for (int attempt = 0; attempt < 10000; ++attempt) {
            StringBuilder name = new StringBuilder("g-");
            for (int i = 0; i < 8; ++i) name.append(alphabet.charAt(random.nextInt(alphabet.length())));
            Path path = directory.resolve(name.toString());
            try {
                if (WINDOWS) Files.createDirectory(path);
                else Files.createDirectory(path, PosixFilePermissions.asFileAttribute(PosixFilePermissions.fromString("rwx------")));
                return path;
            } catch (FileAlreadyExistsException e) {
                // try another name
            }
        }
        throw new FileAlreadyExistsException("No usable temporary directory name found");
    }

    /** Work directories hold only files this driver created; never recurse into links. */
    static void deleteFlatDirectory(Path directory) throws IOException {
        try (DirectoryStream<Path> entries = Files.newDirectoryStream(directory)) {
            for (Path entry : entries) Files.delete(entry);
        }
        Files.delete(directory);
    }

    static List<Path> glob(Path directory, String pattern) throws IOException {
        List<Path> out = new ArrayList<>();
        try (DirectoryStream<Path> entries = Files.newDirectoryStream(directory, pattern)) {
            for (Path entry : entries) out.add(entry);
        }
        return out;
    }

    /** pathlib ordering for one directory: case-insensitive names on Windows. */
    static List<Path> sortedGlob(Path directory, String pattern) throws IOException {
        List<Path> out = glob(directory, pattern);
        out.sort((a, b) -> comparePython(sortName(a), sortName(b)));
        return out;
    }

    static String sortName(Path path) {
        String name = path.getFileName().toString();
        return WINDOWS ? name.toLowerCase(Locale.ROOT) : name;
    }

    static String withSuffix(String name, String suffix) {
        int dot = name.lastIndexOf('.');
        return (dot > 0 ? name.substring(0, dot) : name) + suffix;
    }

    static List<String> hashes(List<Path> paths) throws IOException {
        List<String> out = new ArrayList<>();
        for (Path p : paths) out.add(fileHash(p));
        return out;
    }

    static String fileHash(Path path) throws IOException {
        try (InputStream in = Files.newInputStream(path)) {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] buffer = new byte[1 << 20];
            for (int n; (n = in.read(buffer)) > 0; ) digest.update(buffer, 0, n);
            return DexSurface.hex(digest.digest());
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    static String sha256(byte[] bytes) throws Exception {
        return DexSurface.hex(MessageDigest.getInstance("SHA-256").digest(bytes));
    }

    static String identityKey(Map<String, Object> identity) throws Exception {
        // 128-bit directory names keep Windows paths short. A hit still compares
        // the complete identity and JAR digest, so a collision cannot reuse code.
        return sha256(dumps(identity, true, -1).getBytes(StandardCharsets.UTF_8)).substring(0, 32);
    }

    static String readText(Path path) throws IOException {
        // Strict UTF-8, as the former driver's read_text(encoding="utf-8").
        return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(Files.readAllBytes(path))).toString();
    }

    static void writeText(Path path, String text) throws IOException {
        Files.writeString(path, text, StandardCharsets.UTF_8);
    }

    /** Python str.splitlines(). */
    static List<String> splitlines(String text) {
        List<String> lines = new ArrayList<>();
        int start = 0;
        for (int i = 0; i < text.length(); ++i) {
            char c = text.charAt(i);
            if (c == '\n' || c == '\r' || c == 0x0b || c == 0x0c || c == 0x1c || c == 0x1d || c == 0x1e
                || c == 0x85 || c == 0x2028 || c == 0x2029) {
                lines.add(text.substring(start, i));
                if (c == '\r' && i + 1 < text.length() && text.charAt(i + 1) == '\n') ++i;
                start = i + 1;
            }
        }
        if (start < text.length()) lines.add(text.substring(start));
        return lines;
    }

    static String at(String[] row, int index) {
        if (index >= row.length) throw new IndexOutOfBoundsException("list index out of range");
        return row[index];
    }

    static String str(Map<?, ?> map, String key) {
        Object value = map.get(key);
        return value == null ? "" : value.toString();
    }

    static Object getOrZero(Map<?, ?> map, String key) {
        Object value = map.get(key);
        return value == null ? (Object) 0L : value;
    }

    static Map<String, Object> map(Object... pairs) {
        Map<String, Object> out = new LinkedHashMap<>();
        for (int i = 0; i < pairs.length; i += 2) out.put((String) pairs[i], pairs[i + 1]);
        return out;
    }

    static String message(Exception e) {
        return e.getMessage() != null ? e.getMessage() : e.toString();
    }

    static boolean truthy(Object value) {
        if (value instanceof Boolean b) return b;
        if (value instanceof Number n) return n.doubleValue() != 0;
        if (value instanceof String s) return !s.isEmpty();
        if (value instanceof Collection<?> c) return !c.isEmpty();
        if (value instanceof Map<?, ?> m) return !m.isEmpty();
        return value != null;
    }

    static double seconds(long started) {
        return (System.nanoTime() - started) / 1e9;
    }

    /** Python round(x, digits): half-even on the exact binary value; integers stay integers. */
    static Object round(Object value, int digits) {
        if (!(value instanceof Double d) || d.isNaN() || d.isInfinite()) return value;
        return new BigDecimal(d).setScale(digits, RoundingMode.HALF_EVEN).doubleValue();
    }

    // ---- JSON as Python's json module writes and reads it ----

    /** json.dumps(value, sort_keys=sortKeys, indent=indent or None, ensure_ascii=True). */
    static String dumps(Object value, boolean sortKeys, int indent) {
        StringBuilder out = new StringBuilder();
        dump(out, value, sortKeys, indent, 0);
        return out.toString();
    }

    static void dump(StringBuilder out, Object value, boolean sortKeys, int indent, int level) {
        if (value == null) out.append("null");
        else if (value instanceof Boolean b) out.append(b ? "true" : "false");
        else if (value instanceof Double d) out.append(pyFloat(d));
        else if (value instanceof Float f) out.append(pyFloat(f.doubleValue()));
        else if (value instanceof Number n) out.append(n.toString());
        else if (value instanceof String s) quote(out, s);
        else if (value instanceof Map<?, ?> map) {
            if (map.isEmpty()) { out.append("{}"); return; }
            List<Object> keys = new ArrayList<>(map.keySet());
            if (sortKeys) keys.sort(ApkCache::comparePython);
            out.append('{');
            boolean first = true;
            for (Object key : keys) {
                if (!first) out.append(indent >= 0 ? "," : ", ");
                first = false;
                newline(out, indent, level + 1);
                quote(out, String.valueOf(key));
                out.append(": ");
                dump(out, map.get(key), sortKeys, indent, level + 1);
            }
            newline(out, indent, level);
            out.append('}');
        } else if (value instanceof Collection<?> list) {
            if (list.isEmpty()) { out.append("[]"); return; }
            out.append('[');
            boolean first = true;
            for (Object item : list) {
                if (!first) out.append(indent >= 0 ? "," : ", ");
                first = false;
                newline(out, indent, level + 1);
                dump(out, item, sortKeys, indent, level + 1);
            }
            newline(out, indent, level);
            out.append(']');
        } else throw new IllegalArgumentException("Object of type " + value.getClass().getName() + " is not JSON serializable");
    }

    static void newline(StringBuilder out, int indent, int level) {
        if (indent < 0) return;
        out.append('\n');
        out.append(" ".repeat(indent * level));
    }

    static void quote(StringBuilder out, String s) {
        out.append('"');
        for (int i = 0; i < s.length(); ++i) {
            char c = s.charAt(i);
            switch (c) {
                case '"' -> out.append("\\\"");
                case '\\' -> out.append("\\\\");
                case '\n' -> out.append("\\n");
                case '\r' -> out.append("\\r");
                case '\t' -> out.append("\\t");
                case '\b' -> out.append("\\b");
                case '\f' -> out.append("\\f");
                default -> {
                    if (c < 0x20 || c > 0x7e) out.append(String.format(Locale.ROOT, "\\u%04x", (int) c));
                    else out.append(c);
                }
            }
        }
        out.append('"');
    }

    /** float.__repr__ (shortest round-trip digits), as json.dumps writes floats. */
    static String pyFloat(double d) {
        if (Double.isNaN(d)) return "NaN";
        if (Double.isInfinite(d)) return d > 0 ? "Infinity" : "-Infinity";
        if (d == 0) return (Double.doubleToRawLongBits(d) < 0) ? "-0.0" : "0.0";
        String sign = d < 0 ? "-" : "";
        BigDecimal exact = new BigDecimal(Double.toString(Math.abs(d))).stripTrailingZeros();
        String digits = exact.unscaledValue().toString();
        int point = digits.length() - exact.scale(); // value = 0.digits * 10^point
        if (point > -4 && point <= 16) {
            if (point <= 0) return sign + "0." + "0".repeat(-point) + digits;
            if (point >= digits.length()) return sign + digits + "0".repeat(point - digits.length()) + ".0";
            return sign + digits.substring(0, point) + "." + digits.substring(point);
        }
        int exponent = point - 1;
        String mantissa = digits.length() == 1 ? digits : digits.charAt(0) + "." + digits.substring(1);
        return sign + mantissa + "e" + (exponent < 0 ? "-" : "+") + (Math.abs(exponent) < 10 ? "0" : "") + Math.abs(exponent);
    }

    /** repr() of a list of str, for messages matching subprocess.CalledProcessError. */
    static String pyRepr(List<String> values) {
        StringBuilder out = new StringBuilder("[");
        for (int i = 0; i < values.size(); ++i) {
            if (i > 0) out.append(", ");
            String s = values.get(i);
            char q = s.indexOf('\'') >= 0 && s.indexOf('"') < 0 ? '"' : '\'';
            out.append(q);
            for (char c : s.toCharArray()) {
                if (c == '\\' || c == q) out.append('\\').append(c);
                else if (c == '\n') out.append("\\n");
                else if (c == '\r') out.append("\\r");
                else if (c == '\t') out.append("\\t");
                else if (c < 0x20 || c == 0x7f) out.append(String.format(Locale.ROOT, "\\x%02x", (int) c));
                else out.append(c);
            }
            out.append(q);
        }
        return out.append(']').toString();
    }

    /** json.loads: objects keep key order (last duplicate wins), integers vs floats as Python. */
    static Object parseJson(String text) {
        int[] at = {0};
        skipSpace(text, at);
        Object value = parseValue(text, at);
        skipSpace(text, at);
        if (at[0] != text.length()) throw new IllegalArgumentException("Extra data");
        return value;
    }

    static void skipSpace(String s, int[] at) {
        while (at[0] < s.length() && " \t\n\r".indexOf(s.charAt(at[0])) >= 0) ++at[0];
    }

    static Object parseValue(String s, int[] at) {
        if (at[0] >= s.length()) throw new IllegalArgumentException("Expecting value");
        char c = s.charAt(at[0]);
        if (c == '{') {
            Map<String, Object> map = new LinkedHashMap<>();
            ++at[0];
            skipSpace(s, at);
            if (at[0] < s.length() && s.charAt(at[0]) == '}') { ++at[0]; return map; }
            while (true) {
                skipSpace(s, at);
                if (at[0] >= s.length() || s.charAt(at[0]) != '"') throw new IllegalArgumentException("Expecting property name");
                String key = parseString(s, at);
                skipSpace(s, at);
                expect(s, at, ':');
                skipSpace(s, at);
                map.put(key, parseValue(s, at));
                skipSpace(s, at);
                if (at[0] < s.length() && s.charAt(at[0]) == ',') { ++at[0]; continue; }
                expect(s, at, '}');
                return map;
            }
        }
        if (c == '[') {
            List<Object> list = new ArrayList<>();
            ++at[0];
            skipSpace(s, at);
            if (at[0] < s.length() && s.charAt(at[0]) == ']') { ++at[0]; return list; }
            while (true) {
                skipSpace(s, at);
                list.add(parseValue(s, at));
                skipSpace(s, at);
                if (at[0] < s.length() && s.charAt(at[0]) == ',') { ++at[0]; continue; }
                expect(s, at, ']');
                return list;
            }
        }
        if (c == '"') return parseString(s, at);
        for (String literal : new String[]{"true", "false", "null", "NaN", "Infinity", "-Infinity"}) {
            if (s.startsWith(literal, at[0])) {
                at[0] += literal.length();
                switch (literal) {
                    case "true": return true;
                    case "false": return false;
                    case "null": return null;
                    case "NaN": return Double.NaN;
                    case "Infinity": return Double.POSITIVE_INFINITY;
                    default: return Double.NEGATIVE_INFINITY;
                }
            }
        }
        Matcher number = Pattern.compile("(-?(?:0|[1-9][0-9]*))(\\.[0-9]+)?([eE][-+]?[0-9]+)?").matcher(s);
        number.region(at[0], s.length());
        if (!number.lookingAt()) throw new IllegalArgumentException("Expecting value");
        at[0] = number.end();
        if (number.group(2) == null && number.group(3) == null) {
            BigInteger integer = new BigInteger(number.group(1));
            return integer.bitLength() < 64 ? (Object) integer.longValue() : integer;
        }
        return Double.parseDouble(number.group());
    }

    static void expect(String s, int[] at, char c) {
        if (at[0] >= s.length() || s.charAt(at[0]) != c) throw new IllegalArgumentException("Expecting '" + c + "' delimiter");
        ++at[0];
    }

    static String parseString(String s, int[] at) {
        StringBuilder out = new StringBuilder();
        ++at[0];
        while (true) {
            if (at[0] >= s.length()) throw new IllegalArgumentException("Unterminated string");
            char c = s.charAt(at[0]++);
            if (c == '"') return out.toString();
            if (c < 0x20) throw new IllegalArgumentException("Invalid control character");
            if (c != '\\') { out.append(c); continue; }
            if (at[0] >= s.length()) throw new IllegalArgumentException("Unterminated string");
            char e = s.charAt(at[0]++);
            switch (e) {
                case '"', '\\', '/' -> out.append(e);
                case 'b' -> out.append('\b');
                case 'f' -> out.append('\f');
                case 'n' -> out.append('\n');
                case 'r' -> out.append('\r');
                case 't' -> out.append('\t');
                case 'u' -> {
                    if (at[0] + 4 > s.length()) throw new IllegalArgumentException("Invalid \\uXXXX escape");
                    out.append((char) Integer.parseInt(s.substring(at[0], at[0] + 4), 16));
                    at[0] += 4;
                }
                default -> throw new IllegalArgumentException("Invalid \\escape");
            }
        }
    }
}
