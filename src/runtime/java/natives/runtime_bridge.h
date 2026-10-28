#pragma once
namespace venture::java {
// Connect the framework to the real NDK lifecycle and window owners before launch.
void install_runtime_framework_bridges();
// Android main-thread teardown, before shutting down guest workers or HotSpot.
void destroy_native_activities();
}
