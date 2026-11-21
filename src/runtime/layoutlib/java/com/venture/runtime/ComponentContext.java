package com.venture.runtime;

import android.content.Context;
import com.venture.runtime.layoutlib.HostContext;

/** A service's base context in a 2D process; its receivers and connections belong to the service. */
public final class ComponentContext extends HostContext {
  public ComponentContext(Context environment, Context outer) {
    super(java.util.Objects.requireNonNull(outer), null);
  }
}
