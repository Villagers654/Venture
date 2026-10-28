"""Verify the shipped framework's public Manifest API through JVM reflection."""
import os, pathlib, subprocess, sys
jdk, classes, framework, expected = map(pathlib.Path, sys.argv[1:])
java = jdk / "bin" / ("java.exe" if os.name == "nt" else "java")
subprocess.run([str(java), "-cp", os.pathsep.join(map(str, [classes, framework])),
                "ManifestApiFixture", str(expected)], check=True, timeout=30)
