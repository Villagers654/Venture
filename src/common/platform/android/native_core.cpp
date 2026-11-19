// SPDX-License-Identifier: GPL-3.0-or-later
// libventure_core.so: every object of the shared C++ core (store client,
// library, downloads, manifest adaptation, planner) linked against the NDK
// alone, so a symbol the NDK lacks fails the build. The APK's own library is
// libventure.so, the launcher on SDL3 (src/launcher, src/android_app).
#include <jni.h>

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM*, void*) { return JNI_VERSION_1_6; }
