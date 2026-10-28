package com.venture.runtime;

/** Immutable host diagnostics; guest libc environment is separate. */
public final class HostEnvironment {
  private HostEnvironment() {}
  public static String getenv(String name) {
    return System.getenv(name);
  }
}
