#pragma once
namespace venture::ovrplatform { class Session; }
namespace venture::java {
// Bind the launch-selected account's Session before starting the JVM. Its
// lifetime already covers all Java/guest threads, as for the native SDK thunks.
void install_horizon_bridge(ovrplatform::Session&);
}
