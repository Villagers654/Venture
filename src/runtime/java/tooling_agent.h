#pragma once
#include <jni.h>
#include <jvmti.h>
// 0: invoke original body; 1: return replacement; 2: propagate pending JNI exception.
using VentureJvmtiCallback = jint (JNICALL*)(JNIEnv*, jobject, const jvalue*, jint, jvalue*, void*);
using VentureJvmtiRelease = void (*)(void*);
extern "C" {
JNIEXPORT jvmtiEnv* VentureJvmtiEnvironment();
JNIEXPORT jint VentureJvmtiStatus();
JNIEXPORT jint VentureJvmtiInstallHook(jmethodID, VentureJvmtiCallback, void*, VentureJvmtiRelease);
JNIEXPORT jint VentureJvmtiRemoveHook(jmethodID);
JNIEXPORT jint VentureJvmtiDispatchHook(JNIEnv*, jmethodID, jobject, const jvalue*, jint, jvalue*);
}
