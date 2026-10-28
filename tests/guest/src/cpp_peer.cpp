#include "cpp_peer.h"
#include <stdexcept>
#include <string>
VtestBase::~VtestBase() = default;
VtestDerived::~VtestDerived() = default;
int VtestDerived::value() const { return 42; }
const char* VtestError::what() const noexcept { return "peer exception"; }
extern "C" VtestBase* vtest_cpp_make() { return new VtestDerived; }
extern "C" void vtest_cpp_delete(VtestBase* p) { delete p; }
static int unwound = 0;
struct UnwindGuard { ~UnwindGuard() { ++unwound; } };
extern "C" void vtest_cpp_throw() { unwound = 0; UnwindGuard guard; throw VtestError(); }
extern "C" int vtest_cpp_unwound() { return unwound; }
extern "C" bool vtest_cpp_catch(void (*callback)()) {
    try { callback(); } catch (const std::runtime_error& e) { return std::string(e.what()) == "caller exception"; }
    return false;
}
