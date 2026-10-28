#include <cstdint>
// Unity startup runs .init_array; unload reverses __cxa_atexit registration.
// The observer lives in the caller, allowing it to inspect events after dlclose.
static int constructors = 0;
static void (*observer)(int) = nullptr;
struct Object {
    int id;
    explicit Object(int i) : id(i) { constructors = constructors * 10 + i; }
    ~Object() { if (observer) observer(id); }
};
static Object first(1);
static Object second(2);
extern "C" int __cxa_atexit(void (*)(void*), void*, void*);
extern "C" void* __dso_handle;
static void registered(void*) { if (observer) observer(3); }
extern "C" int vtest_lifecycle_attach(void (*callback)(int)) {
    observer = callback;
    if (__cxa_atexit(registered, nullptr, __dso_handle) != 0) return -1;
    return constructors;
}
