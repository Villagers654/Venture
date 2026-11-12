package dalvik.annotation.optimization;

import java.lang.annotation.*;

// libcore: a static native with primitive parameters and result, called without
// JNIEnv* or jclass. Apps bundle their own copy; Venture's JNI bridge reads it
// from the class file (com.venture.runtime.NativeMethods.criticalNatives).
@Retention(RetentionPolicy.CLASS)
@Target(ElementType.METHOD)
public @interface CriticalNative {}
