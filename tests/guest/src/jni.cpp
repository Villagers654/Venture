#include "vtest.h"
#include <jni.h>
static bool loaded = false;
static jint registered_add(JNIEnv*, jclass, jint a, jint b) { return a + b; }
// Unity AndroidJavaObject and native plugins need calls in both directions.
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (!vm || vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass klass = env->FindClass("org/venture/tests/VtestJni");
    if (!klass || env->ExceptionCheck()) return JNI_ERR;
    JNINativeMethod methods[] = {{const_cast<char*>("registeredAdd"), const_cast<char*>("(II)I"), reinterpret_cast<void*>(registered_add)}};
    int result = env->RegisterNatives(klass, methods, 1); env->DeleteLocalRef(klass);
    if (result != JNI_OK || env->ExceptionCheck()) return JNI_ERR;
    loaded = true; return JNI_VERSION_1_6;
}
extern "C" JNIEXPORT jint JNICALL Java_org_venture_tests_VtestJni_nativeMultiply(JNIEnv*, jclass, jint a, jint b) {
    return a * b;
}
extern "C" JNIEXPORT jstring JNICALL Java_org_venture_tests_VtestJni_nativeGreeting(JNIEnv* env, jclass) {
    return env->NewStringUTF("Venture JNI");
}
extern "C" JNIEXPORT jint JNICALL Java_org_venture_tests_VtestJni_runGuest(JNIEnv*, jclass) {
    Vtest q("jni"); CHECK(loaded); q.step("JNI: GetEnv FindClass RegisterNatives OK");
    CHECK(registered_add(nullptr, nullptr, 19, 23) == 42);
    CHECK(Java_org_venture_tests_VtestJni_nativeMultiply(nullptr, nullptr, 6, 7) == 42);
    q.step("JNI: registered and exported native functions OK"); return q.finish();
}
extern "C" int vtest_main(int, char**) {
    return Java_org_venture_tests_VtestJni_runGuest(nullptr, nullptr);
}
