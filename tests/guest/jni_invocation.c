// Freestanding guest fixture: JNI invocation ABI from jni.h, on both ARM ABIs.
// No host JNI table or host function pointer may be returned to this code.
typedef int jint;
struct Invoke;
typedef const struct Invoke* JavaVM;
struct Invoke {
  void *reserved0, *reserved1, *reserved2;
  jint (*destroy)(JavaVM*);
  jint (*attach)(JavaVM*, void**, void*);
  jint (*detach)(JavaVM*);
  jint (*get_env)(JavaVM*, void**, jint);
  jint (*attach_daemon)(JavaVM*, void**, void*);
};
extern void* dlsym(void*, const char*);
extern char* dlerror(void);
#define REQUIRE(value, code) do { if (!(value)) return code; } while (0)
int vtest_jni_invocation(void) {
#if __SIZEOF_POINTER__ == 4
  void* scope = (void*)-1; // bionic ARM RTLD_DEFAULT
#else
  void* scope = (void*)0;
#endif
  jint (*enumerate)(JavaVM**, jint, jint*) = dlsym(scope, "JNI_GetCreatedJavaVMs");
  void* (*get_namespace)(const char*) = dlsym(scope, "android_get_exported_namespace");
  REQUIRE(enumerate && get_namespace, 1);
  JavaVM* vms[2] = {(JavaVM*)0x11111111, (JavaVM*)0x22222222};
  struct { jint before, count, after; } counts = {17, -1, 23};
  REQUIRE(enumerate(vms, 0, &counts.count) == 0, 2);
  REQUIRE(counts.count == 1 && counts.before == 17 && counts.after == 23, 3);
  REQUIRE(vms[0] == (JavaVM*)0x11111111 && vms[1] == (JavaVM*)0x22222222, 4);
  REQUIRE(enumerate(vms, 1, &counts.count) == 0, 5);
  REQUIRE(counts.count == 1 && vms[0] && vms[1] == (JavaVM*)0x22222222, 6);
  JavaVM* vm = vms[0];
  REQUIRE(enumerate(vms, 1, (jint*)0) == 0 && vms[0] == vm, 7);
  struct { void* env; void* guard; } out = {(void*)1, (void*)0x33333333};
  jint initial = (*vm)->get_env(vm, &out.env, 0x00010006);
  REQUIRE(initial == 0 || initial == -2, 8);
  REQUIRE((initial == 0) == (out.env != 0), 9);
  REQUIRE((*vm)->attach(vm, &out.env, (void*)0) == 0 && out.env, 10);
  void* attached = out.env;
  REQUIRE((*vm)->get_env(vm, &out.env, 0x00010006) == 0 && out.env == attached, 11);
  REQUIRE(out.guard == (void*)0x33333333, 12);
  if (initial == -2) REQUIRE((*vm)->detach(vm) == 0, 13);
  dlerror();
  REQUIRE(!get_namespace((const char*)0), 14);
  REQUIRE(!get_namespace(""), 15);
  REQUIRE(!get_namespace("default") && !get_namespace("vtest_absent_namespace"), 16);
  REQUIRE(!dlerror(), 17);
  dlsym(scope, "vtest_absent_symbol");
  get_namespace("vtest_absent_namespace");
  REQUIRE(dlerror() != 0, 18); // namespace lookup must preserve pending dlerror
  return 0;
}
