#pragma once
#include <exception>

// Visible key functions/typeinfo must be shared across DSOs, as engine plugins do.
struct VtestBase { virtual ~VtestBase(); virtual int value() const = 0; };
struct VtestDerived : VtestBase { ~VtestDerived() override; int value() const override; };
struct VtestError : std::exception { const char* what() const noexcept override; };
extern "C" VtestBase* vtest_cpp_make();
extern "C" void vtest_cpp_delete(VtestBase*);
extern "C" void vtest_cpp_throw();
extern "C" int vtest_cpp_unwound();
extern "C" bool vtest_cpp_catch(void (*callback)());
