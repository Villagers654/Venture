package com.venture.runtime;

import java.lang.reflect.Method;

/** Optional embedded-JVM hook service, enabled only after its natives are bound. */
public final class MethodHooks {
    private MethodHooks() {}
    private static volatile boolean enabled;
    public static boolean invokeLoadHook(Method method, Object receiver, String name) {
        return enabled && invoke0(method, receiver, name);
    }
    private static native boolean invoke0(Method method, Object receiver, String name);
}
