package com.venture.runtime.layoutlib;

import java.lang.ref.WeakReference;
import java.util.HashMap;
import java.util.Map;
import java.util.WeakHashMap;

/**
 * Stand-in addresses for Java arrays (VMRuntime.addressOf) in a 2D process:
 * each array gets a unique nonzero number that names it and is never
 * dereferenced; RavenwoodHelperBridge.fromAddress turns it back into the array.
 * Arrays compare by identity, so a WeakHashMap keys them by identity.
 */
public final class ArrayAddresses {
  private ArrayAddresses() {}

  private static final Map<Object, Long> addresses = new WeakHashMap<>();
  private static final Map<Long, WeakReference<Object>> arrays = new HashMap<>();
  private static long next = 0x1000;

  public static synchronized long addressOf(Object array) {
    if (array == null) return 0;
    Long known = addresses.get(array);
    if (known != null) return known;
    long address = next;
    next += 0x1000;
    addresses.put(array, address);
    arrays.put(address, new WeakReference<>(array));
    if (arrays.size() > 2 * addresses.size() + 1024) arrays.values().removeIf(reference -> reference.get() == null);
    return address;
  }

  @SuppressWarnings("unchecked")
  public static synchronized <T> T fromAddress(long address) {
    WeakReference<Object> reference = arrays.get(address);
    Object array = reference == null ? null : reference.get();
    if (array == null) throw new IllegalArgumentException("No array at address 0x" + Long.toHexString(address));
    return (T) array;
  }
}
