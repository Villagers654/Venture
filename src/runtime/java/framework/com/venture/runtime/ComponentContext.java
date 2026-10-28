package com.venture.runtime;

import android.content.Context;
import android.content.res.Resources;
import android.view.Display;
import java.util.Objects;

/** Per-component ContextImpl equivalent. Environment access stays in Context;
 * receiver/service dispatchers belong to the outer Activity or Service. */
public final class ComponentContext extends Context {
  private final Context environment, outer;
  public ComponentContext(Context environment, Context outer) {
    this.environment = Objects.requireNonNull(environment);
    this.outer = Objects.requireNonNull(outer);
  }
  @Override public Context ventureOuterContext() { return outer; }
  @Override public Resources getResources() { return environment.getResources(); }
  @Override public Display getDisplay() { return environment.getDisplay(); }
  @Override public int checkPermission(String permission, int pid, int uid) {
    return environment.checkPermission(permission, pid, uid);
  }
}
