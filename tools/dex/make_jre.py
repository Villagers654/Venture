#!/usr/bin/env python3
"""Build a redistributable runtime from the pinned JDK; retain all applicable notices."""
import argparse
import shutil
import subprocess
from pathlib import Path
from convert_apk import DEPS, java_tool

# r2-java outputs/external_type_union.json: java.util.logging and sun.misc
# {Unsafe,Signal,SignalHandler}. java.xml supports framework resource parsers.
# java.desktop backs the framework's android.graphics Bitmap/BitmapFactory/Paint
# (java.awt.image, javax.imageio, java.awt.Font); the JVM runs it headless.
# No java.sql or java.net.http.
MODULES = "java.base,java.logging,java.xml,java.desktop,jdk.unsupported"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    subprocess.run([java_tool("jlink"), "--module-path", str(DEPS / "jdk/jmods"), "--add-modules", MODULES,
                    "--strip-debug", "--no-header-files", "--no-man-pages", "--output", str(args.output)], check=True)
    # jlink supplies legal/<module>; retain distribution-level notices as well.
    for name in ("NOTICE", "LICENSE", "ADDITIONAL_LICENSE_INFO", "ASSEMBLY_EXCEPTION"):
        source = DEPS / "jdk" / name
        if source.exists():
            shutil.copy2(source, args.output / name)
    (args.output / "venture-modules.txt").write_text(MODULES + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
