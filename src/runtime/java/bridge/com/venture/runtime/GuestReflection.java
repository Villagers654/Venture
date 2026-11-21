package com.venture.runtime;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Method.invoke for app code, with ART's access rules.
 *
 * Converted app classes reach Method.invoke through NativeLoader.invoke (the
 * rewriter's reflective native-load interception), and JNI CallObjectMethod on
 * Method.invoke goes through NativeLoader.invokeWithLoader. HotSpot's
 * Method.invoke is caller-sensitive, so it judged access as if NativeLoader
 * were the caller. HotSpot also requires the declaring class itself to be
 * accessible, where ART (art/runtime/reflection.cc VerifyAccess) looks only at
 * the member: a public member is always reachable, otherwise the caller must
 * be the declaring class, a subclass for a protected member on a compatible
 * receiver, or a class of the same runtime package (same loader and package).
 *
 * When HotSpot refuses, the call is judged again with ART's rules against the
 * app class that called Method.invoke (the first frame outside NativeLoader and
 * this class; none for a native thread with no Java frame, which ART refuses
 * for non-public members too). An allowed call runs on an accessible copy of
 * the Method, so the app's own Method object keeps its accessible flag (unless
 * the class cannot list its declared methods; then the app's object is opened).
 */
final class GuestReflection {
  private GuestReflection() {}
  private static final StackWalker WALKER = StackWalker.getInstance(StackWalker.Option.RETAIN_CLASS_REFERENCE);
  private static final ConcurrentHashMap<Method, Method> OPENED = new ConcurrentHashMap<>();

  static Object invoke(Method method, Object target, Object[] args)
      throws IllegalAccessException, InvocationTargetException {
    try {
      return method.invoke(target, args);
    } catch (IllegalAccessException denied) {
      // Method.invoke checks access before arguments and wraps everything the
      // target throws, so this is HotSpot's access check alone.
      Class<?> caller = caller();
      if (!allows(method.getDeclaringClass(), method.getModifiers(), target, caller))
        throw new IllegalAccessException("Class " + (caller == null ? "<native thread>" : caller.getName())
            + " cannot access " + Modifier.toString(method.getModifiers()) + " method " + method
            + " of class " + method.getDeclaringClass().getName());
      Method open = opened(method);
      if (open == null) throw denied; // a JDK class whose module does not open the package
      return open.invoke(target, args);
    }
  }

  static Class<?> caller() {
    return WALKER.walk(s -> s.map(StackWalker.StackFrame::getDeclaringClass)
        .filter(c -> c != GuestReflection.class && c != NativeLoader.class)
        .findFirst().orElse(null));
  }

  /** ART's VerifyAccess(obj, declaring_class, access_flags, calling_class). */
  static boolean allows(Class<?> declaring, int modifiers, Object target, Class<?> caller) {
    if (Modifier.isPublic(modifiers)) return true;
    if (caller == null) return false;
    if (caller == declaring) return true;
    if (Modifier.isPrivate(modifiers)) return false;
    if (Modifier.isProtected(modifiers)) {
      if (target != null && !caller.isInstance(target) && !samePackage(declaring, caller)) return false;
      if (declaring.isAssignableFrom(caller)) return true;
    }
    return samePackage(declaring, caller);
  }

  private static boolean samePackage(Class<?> a, Class<?> b) {
    return a.getClassLoader() == b.getClassLoader() && a.getPackageName().equals(b.getPackageName());
  }

  private static Method opened(Method method) {
    Method open = OPENED.get(method);
    if (open != null) return open;
    try {
      open = method.getDeclaringClass().getDeclaredMethod(method.getName(), method.getParameterTypes());
      if (!open.equals(method)) return null; // a covariant bridge shares name and parameters
      open.setAccessible(true);
    } catch (LinkageError e) {
      // Another declared method names a class the framework lacks, so the class
      // cannot list its methods; open the app's own Method object instead.
      try {
        method.setAccessible(true);
        open = method;
      } catch (RuntimeException closed) {
        return null;
      }
    } catch (ReflectiveOperationException | RuntimeException e) {
      return null; // InaccessibleObjectException: a JDK package not open to the framework
    }
    Method raced = OPENED.putIfAbsent(method, open);
    return raced == null ? open : raced;
  }
}
