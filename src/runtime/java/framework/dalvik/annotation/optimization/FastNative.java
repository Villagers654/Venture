package dalvik.annotation.optimization;

import java.lang.annotation.*;

// libcore: a native ART calls with fewer transition checks but the usual JNI arguments.
@Retention(RetentionPolicy.CLASS)
@Target(ElementType.METHOD)
public @interface FastNative {}
