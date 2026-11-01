package androidx.lifecycle;
// Public marker ABI: androidx.lifecycle:lifecycle-common, which declares no members.
/**
 * Declaration-only contract. SDKs that ship without androidx.lifecycle still
 * name it: Sentry's AppLifecycleIntegration passes its LifecycleWatcher to
 * Lifecycle.addObserver(LifecycleObserver) behind a Class.forName probe. ART
 * defers that unresolved argument type to execution; HotSpot's verifier must
 * load it to check the assignment and rejects the whole class. An interface
 * satisfies the check without loading the watcher. Venture supplies no
 * Lifecycle, owner or DefaultLifecycleObserver, so such probes still fail.
 */
public interface LifecycleObserver {}
