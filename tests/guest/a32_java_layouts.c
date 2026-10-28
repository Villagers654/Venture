// Compile with NDK r30 clang --target=armv7a-linux-androideabi23 -c.
// Android jni.h and native_activity.h are the wire ABI, independent of the
// host OpenJDK headers used to compile the bridge.
#include <stddef.h>
#include <jni.h>
#include <android/native_activity.h>
#include <android/native_window.h>
_Static_assert(sizeof(void*) == 4, "ARM ILP32 probe");
_Static_assert(sizeof(struct JNINativeInterface) == 932, "JNIEnv table");
_Static_assert(sizeof(struct JNIInvokeInterface) == 32, "JavaVM table");
_Static_assert(sizeof(jvalue) == 8, "jvalue union stride");
_Static_assert(sizeof(JNINativeMethod) == 12, "native registration stride");
_Static_assert(sizeof(JavaVMAttachArgs) == 12, "attach arguments");
_Static_assert(offsetof(JavaVMAttachArgs, group) == 8, "attach group token");
_Static_assert(sizeof(ANativeActivity) == 40, "NativeActivity");
_Static_assert(sizeof(ANativeActivityCallbacks) == 64, "activity callbacks");
_Static_assert(offsetof(ANativeActivity, sdkVersion) == 24, "activity SDK");
_Static_assert(offsetof(ANativeActivity, instance) == 28, "activity instance");
_Static_assert(offsetof(ANativeActivity, assetManager) == 32, "activity assets");
_Static_assert(offsetof(ANativeActivityCallbacks, onInputQueueCreated) == 44, "queue callback");
_Static_assert(sizeof(ANativeWindow_Buffer) == 44, "window buffer");
_Static_assert(offsetof(ANativeWindow_Buffer, bits) == 16, "window pixel pointer");
