#pragma once
namespace venture::java {
// Call before start_jvm(). Adds the registrar through jvm.h's existing
// contract.
void register_framework_natives();
}  // namespace venture::java
