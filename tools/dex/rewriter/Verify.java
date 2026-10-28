package com.venture.dex;

import java.io.*;
import java.net.*;
import java.nio.file.*;
import java.util.*;
import java.util.jar.*;

/** Run with -Xverify:all. Loading, linking and reflecting never initialize app classes. */
public final class Verify {
    private static String escape(String s) {
        return s.replace("\\", "\\\\").replace("\"", "\\\"").replace("\r", "\\r").replace("\n", "\\n").replace("\t", "\\t");
    }
    public static void main(String[] args) throws Exception {
        List<URL> urls = new ArrayList<>();
        for (String path : args[2].split(java.util.regex.Pattern.quote(File.pathSeparator)))
            if (!path.isEmpty()) urls.add(Path.of(path).toUri().toURL());
        urls.add(Path.of(args[0]).toUri().toURL());
        int failures = 0;
        long loading = 0, verifying = 0;
        try (var loader = new URLClassLoader(urls.toArray(URL[]::new), ClassLoader.getPlatformClassLoader());
             JarFile jar = new JarFile(args[0]); PrintWriter log = new PrintWriter(args[1], "UTF-8")) {
            var entries = jar.entries();
            while (entries.hasMoreElements()) {
                String file = entries.nextElement().getName();
                if (!file.endsWith(".class")) continue;
                String name = file.substring(0, file.length() - 6).replace('/', '.');
                long start = System.nanoTime();
                boolean loaded = false;
                try {
                    Class<?> cls = Class.forName(name, false, loader);
                    loading += System.nanoTime() - start;
                    loaded = true;
                    // HotSpot verifies method bodies at linking/reflection; resolve descriptor types too.
                    start = System.nanoTime();
                    cls.getDeclaredMethods(); cls.getDeclaredConstructors(); cls.getDeclaredFields();
                } catch (LinkageError | ClassNotFoundException | SecurityException e) {
                    ++failures;
                    log.println("{\"class\":\"" + escape(name) + "\",\"kind\":\"" + e.getClass().getName()
                        + "\",\"detail\":\"" + escape(String.valueOf(e.getMessage())) + "\"}");
                } finally {
                    if (loaded) verifying += System.nanoTime() - start;
                    else loading += System.nanoTime() - start;
                }
            }
        }
        System.out.printf(Locale.ROOT, "phases load=%.6f verify=%.6f%n", loading/1e9, verifying/1e9);
        System.out.println("verification failures: " + failures);
        if (failures != 0) System.exit(2);
    }
}
