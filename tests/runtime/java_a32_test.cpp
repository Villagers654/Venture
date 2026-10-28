#include "xr/display_timing.h"
#include <doctest/doctest.h>
#include "fake_cpu32.h"
#include "java/internal.h"
#include "java/jni32.h"
#include "core/guest_storage.h"
#include "core/hle_runtime.h"
#include "core/process.h"
#include "android/handles32.h"
#include "android/native_activity.h"
#include "android/native_window.h"
#include "android/input.h"
#include "android/looper.h"
#include "android/choreographer.h"
#include "core/wait.h"
#include "core/error.h"
#include "android/package_assets.h"
#include "core/libc_host_files.h"
#include "android/framework_bridge.h"
#include "hle/libandroid/libandroid.h"
#include "core/linker.h"
#include "core/elf.h"
#include <thread>
#include <filesystem>
#include <fstream>
#ifdef VENTURE_TEST_JNI_INVOCATION_FIXTURE
#include "java_invocation_fixture.h"
#endif

#include "../async_wait.h"

using namespace venture;
using namespace venture::java;
using venture::hle::a32::CallBuilder32;
namespace {
JNIEnv* start() {
  JvmOptions options;
  options.java_home = VENTURE_TEST_JAVA_HOME;
  options.agent_path = VENTURE_TEST_JVM_AGENT;
  options.class_path = {VENTURE_TEST_BRIDGE_JAR, VENTURE_TEST_JAVA_CLASSES};
  options.extra_options = {"-Xcheck:jni", "-Xverify:all"};
  std::string error;
  REQUIRE_MESSAGE(start_jvm(options, &error), error);
  auto* env = host_env();
  if (!current_loader()) {
    auto cls = env->FindClass("java/lang/ClassLoader");
    REQUIRE(cls);
    auto get = env->GetStaticMethodID(cls, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    REQUIRE(get);
    auto loader = env->CallStaticObjectMethod(cls, get);
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(loader);
    set_app_class_loader(loader);
    env->DeleteLocalRef(loader);
    env->DeleteLocalRef(cls);
  }
  guest_env(GuestThread::adopt_current());
  return env;
}
GuestCallResult invoke(const char* name, CallBuilder32 args = {}) {
  auto& thread = GuestThread::adopt_current();
  test::LowMemory32 memory;
  test::FakeCpu32 cpu;
  cpu.set_sp(memory.address + 65536);
  args.apply(cpu, memory.address);
  cpu.set_x(0, guest_env(thread));
  for (auto entry : jni_entries())
    if (std::string_view(entry.name) == name) {
      hle::FunctionInfo info{entry.handler, nullptr, "JNIEnv", name, 0, hle::a32::descriptor("JNIEnv", name)};
      REQUIRE(info.a32);
      hle::CallContext ctx(thread, cpu, info);
      entry.handler(ctx);
      return {cpu.x(0), cpu.x(1), cpu.v(0), GuestIsa::a32};
    }
  FAIL("missing JNI entry");
  return {};
}
CallBuilder32 args(std::initializer_list<std::uint64_t> words) {
  CallBuilder32 b;
  b.value(0u);
  for (auto word : words) b.pointer(word);
  return b;
}
std::uint32_t method_id(std::uint32_t cls, std::string name, std::string sig, bool stat = false) {
  return static_cast<std::uint32_t>(invoke(stat ? "GetStaticMethodID" : "GetMethodID",
                                           args({cls, h2g(low_string(name)), h2g(low_string(sig))}))
                                        .x0);
}
GuestCallResult ndk(const char* name, const CallBuilder32& args) {
  static bool registered = [] {
    hle::register_libandroid();
    return true;
  }();
  (void)registered;
  auto* lib = Linker::get().open("libandroid.so", kRtldNow, nullptr);
  REQUIRE(lib);
  auto fn = lib->find_symbol(name);
  REQUIRE(fn);
  return GuestThread::adopt_current().call32(fn, args);
}
}  // namespace
TEST_CASE("A32 JNI tables, refs and every invocation family use tokens") {
  auto* env = start();
  auto& thread = GuestThread::adopt_current();
  const auto wrapper = guest_env(thread);
  auto table = *g2h<std::uint32_t>(wrapper);
  for (unsigned i = 0; i < 4; ++i) CHECK(g2h<std::uint32_t>(table)[i] == 0);
  for (const auto& entry : jni_entries()) {
    auto thunk = g2h<std::uint32_t>(table)[entry.slot];
    REQUIRE(thunk);
    CHECK(AddressSpace::get().is_guest_executable(thunk));
    const auto word = *g2h<std::uint32_t>(thunk);
    CHECK((word & 0xfff00000) == 0xe3000000);  // ARM MOVW
  }
  auto cls = static_cast<std::uint32_t>(invoke("FindClass", args({h2g(low_string("BridgeFixture"))})).x0);
  REQUIRE(cls);
  REQUIRE_FALSE(env->ExceptionCheck());
  auto ctor = method_id(cls, "<init>", "(I)V");
  auto obj = static_cast<std::uint32_t>(invoke("NewObject", args({cls, ctor, 17})).x0);
  REQUIRE(obj);
  struct Row {
    const char *type, *name, *sig;
    std::uint64_t bits;
  };
  Row rows[] = {{"Object", "obj", "()Ljava/lang/Object;", 0},
                {"Boolean", "bool", "()Z", 1},
                {"Byte", "byt", "()B", std::uint32_t(-128)},
                {"Char", "chr", "()C", 0xffee},
                {"Short", "shrt", "()S", std::uint32_t(-32000)},
                {"Int", "integer", "()I", std::uint32_t(-1234567)},
                {"Long", "lng", "()J", std::uint64_t(-0x123456789abcdefLL)},
                {"Float", "flt", "()F", std::bit_cast<std::uint32_t>(1.25f)},
                {"Double", "dbl", "()D", std::bit_cast<std::uint64_t>(-2.125)},
                {"Void", "nothing", "()V", 0}};
  for (const auto& row : rows)
    for (auto mode : {"", "Static", "Nonvirtual"})
      for (auto form : {"", "V", "A"})
        for (bool mixed : {false, true}) {
          const bool stat = std::string_view(mode) == "Static",
                     nonvirtual = std::string_view(mode) == "Nonvirtual";
          std::string signature = mixed ? "(JFJFJFJFJFJFJFJFJFJF)" + std::string(row.sig).substr(2) : row.sig;
          auto mid = method_id(cls, (stat ? "s" : "") + std::string(mixed ? "mixed" : "") + row.name,
                               signature, stat);
          auto b = args({stat ? cls : obj});
          if (nonvirtual) b.value(cls);
          b.value(mid);
          test::LowMemory32 values;
          if (*form) {
            b.pointer(values.address);
            for (unsigned i = 0; i < 10; ++i) {
              if (*form == 'A') {
                values.at<jvalue>()[2 * i].j = 100 + i;
                values.at<jvalue>()[2 * i + 1].f = float(i) + 0.25f;
              } else {
                values.at<std::uint64_t>()[2 * i] = 100 + i;
                values.at<double>()[2 * i + 1] = double(i) + 0.25;
              }
            }
          } else if (mixed)
            for (unsigned i = 0; i < 10; ++i) b.value(std::int64_t(100 + i)).value(double(i) + 0.25);
          auto name = "Call" + std::string(mode) + row.type + "Method" + form;
          CAPTURE(name);
          auto result = invoke(name.c_str(), b);
          REQUIRE_FALSE(env->ExceptionCheck());
          if (std::string_view(row.type) == "Object") {
            REQUIRE(result.x0);
            if (!stat) CHECK(env->IsSameObject(jni32::object(result.x0), jni32::object(obj)));
            invoke("DeleteLocalRef", args({result.x0}));
          } else if (std::string_view(row.type) != "Void") {
            CHECK((std::string_view(row.type) == "Long" || std::string_view(row.type) == "Double"
                       ? result.u64()
                       : result.x0) == row.bits);
          }
        }
  auto global = invoke("NewGlobalRef", args({obj})).x0;
  auto weak = invoke("NewWeakGlobalRef", args({obj})).x0;
  CHECK(invoke("GetObjectRefType", args({global})).x0 == JNIGlobalRefType);
  CHECK(invoke("GetObjectRefType", args({weak})).x0 == JNIWeakGlobalRefType);
  CHECK(invoke("PushLocalFrame", args({32})).i32() == JNI_OK);
  auto local = invoke("NewLocalRef", args({weak})).x0;
  auto promoted = invoke("PopLocalFrame", args({local})).x0;
  CHECK_THROWS_AS(jni32::object(local), std::invalid_argument);
  CHECK(invoke("IsSameObject", args({promoted, global})).x0 == JNI_TRUE);
  std::thread other([&] {
    auto& adopted = GuestThread::adopt_current();
    test::LowMemory32 out;
    auto vm_table = *g2h<std::uint32_t>(guest_vm());
    *out.at<std::uint64_t>() = 0xdeadbeef00000000ull;
    auto get =
        adopted.call32(g2h<std::uint32_t>(vm_table)[6],
                       CallBuilder32{}.pointer(guest_vm()).pointer(out.address).value(JNI_VERSION_1_6));
    CHECK(get.i32() == JNI_EDETACHED);
    CHECK(*out.at<std::uint32_t>() == 0);
    CHECK(*out.at<std::uint32_t>(4) == 0xdeadbeef);
    auto attach = adopted.call32(g2h<std::uint32_t>(vm_table)[4],
                                 CallBuilder32{}.pointer(guest_vm()).pointer(out.address).value(0u));
    CHECK(attach.i32() == JNI_OK);
    CHECK(*out.at<std::uint32_t>() == guest_env(adopted));
    CHECK(*out.at<std::uint32_t>(4) == 0xdeadbeef);
    CHECK_THROWS_AS(jni32::object(obj), std::invalid_argument);
    CHECK(invoke("IsSameObject", args({global, weak})).x0 == JNI_TRUE);
    CHECK(detach_host() == JNI_OK);
  });
  other.join();
  invoke("DeleteLocalRef", args({promoted}));
  CHECK_THROWS_AS(jni32::object(promoted), std::invalid_argument);
  invoke("DeleteWeakGlobalRef", args({weak}));
  invoke("DeleteGlobalRef", args({global}));
  CHECK_THROWS_AS(jni32::object(global), std::invalid_argument);
  auto ephemeral = invoke("NewStringUTF", args({h2g(low_string("weak-only referent"))})).x0;
  auto weak_only = invoke("NewWeakGlobalRef", args({ephemeral})).x0;
  invoke("DeleteLocalRef", args({ephemeral}));
  auto system = env->FindClass("java/lang/System");
  REQUIRE(system);
  auto gc = env->GetStaticMethodID(system, "gc", "()V");
  REQUIRE(gc);
  for (int i = 0; i < 5 && !invoke("IsSameObject", args({weak_only, 0})).x0; ++i) {
    env->CallStaticVoidMethod(system, gc);
    REQUIRE_FALSE(env->ExceptionCheck());
  }
  CHECK(invoke("IsSameObject", args({weak_only, 0})).x0 == JNI_TRUE);
  CHECK(invoke("NewLocalRef", args({weak_only})).x0 == 0);
  invoke("DeleteWeakGlobalRef", args({weak_only}));
  env->DeleteLocalRef(system);
  CHECK(method_id(cls, "lng", "()J") == method_id(cls, "lng", "()J"));
  test::LowMemory32 cell;
  *cell.at<std::uint64_t>() = 0xfeedface00000000ull;
  CHECK(invoke("GetJavaVM", args({cell.address})).i32() == JNI_OK);
  CHECK(*cell.at<std::uint32_t>(4) == 0xfeedface);
  CHECK(*cell.at<std::uint32_t>() == guest_vm());
  auto vm_table = *g2h<std::uint32_t>(guest_vm());
  *cell.at<std::uint64_t>() = 0xabcdef1200000000ull;
  auto status =
      thread.call32(g2h<std::uint32_t>(vm_table)[6],
                    CallBuilder32{}.pointer(guest_vm()).pointer(cell.address).value(JNI_VERSION_1_6));
  CHECK(status.i32() == JNI_OK);
  CHECK(*cell.at<std::uint32_t>(4) == 0xabcdef12);
  CHECK(*cell.at<std::uint32_t>() == wrapper);
}
TEST_CASE("A32 JNI copied buffers honor commit abort and object jvalue padding") {
  auto* env = start();
  auto string = invoke("NewStringUTF", args({h2g(low_string("hello"))})).x0;
  test::LowMemory32 mem;
  auto text = invoke("GetStringUTFChars", args({string, mem.address})).x0;
  CHECK(*mem.at<jboolean>() == JNI_TRUE);
  CHECK(std::string(g2h<char>(text)) == "hello");
  CHECK(text <= UINT32_MAX);
  invoke("ReleaseStringUTFChars", args({string, text}));
  auto array = invoke("NewIntArray", args({3})).x0;
  auto copy = invoke("GetIntArrayElements", args({array, 0})).x0;
  g2h<jint>(copy)[1] = 42;
  invoke("ReleaseIntArrayElements", args({array, copy, JNI_COMMIT}));
  jint check[3]{};
  env->GetIntArrayRegion(static_cast<jintArray>(jni32::object(array)), 0, 3, check);
  CHECK(check[1] == 42);
  g2h<jint>(copy)[1] = 99;
  invoke("ReleaseIntArrayElements", args({array, copy, JNI_ABORT}));
  env->GetIntArrayRegion(static_cast<jintArray>(jni32::object(array)), 0, 3, check);
  CHECK(check[1] == 42);
  auto critical = invoke("GetPrimitiveArrayCritical", args({array, 0})).x0;
  g2h<jint>(critical)[2] = -17;
  invoke("ReleasePrimitiveArrayCritical", args({array, critical, 0}));
  env->GetIntArrayRegion(static_cast<jintArray>(jni32::object(array)), 0, 3, check);
  CHECK(check[2] == -17);
  auto global_array = invoke("NewGlobalRef", args({array})).x0;
  auto shared_copy = invoke("GetIntArrayElements", args({array, 0})).x0;
  std::thread releaser([&] {
    guest_env(GuestThread::adopt_current());
    g2h<jint>(shared_copy)[0] = 73;
    invoke("ReleaseIntArrayElements", args({global_array, shared_copy, 0}));
    CHECK_FALSE(host_env()->ExceptionCheck());
  });
  releaser.join();
  invoke("DeleteGlobalRef", args({global_array}));
  env->GetIntArrayRegion(static_cast<jintArray>(jni32::object(array)), 0, 3, check);
  CHECK(check[0] == 73);
  const char* families[]{"Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double"};
  const unsigned widths[]{1, 1, 2, 2, 4, 8, 4, 8};
  for (unsigned i = 0; i < 8; ++i) {
    auto family = std::string(families[i]);
    CAPTURE(family);
    auto a = invoke(("New" + family + "Array").c_str(), args({2})).x0;
    auto p = invoke(("Get" + family + "ArrayElements").c_str(), args({a, 0})).x0;
    REQUIRE(p);
    std::memset(g2h(p), 1, 2 * widths[i]);
    invoke(("Release" + family + "ArrayElements").c_str(), args({a, p, 0}));
    p = invoke("GetPrimitiveArrayCritical", args({a, 0})).x0;
    REQUIRE(p);
    for (unsigned byte = 0; byte < 2 * widths[i]; ++byte) CHECK(g2h<std::uint8_t>(p)[byte] == 1);
    invoke("ReleasePrimitiveArrayCritical", args({a, p, JNI_ABORT}));
    invoke("DeleteLocalRef", args({a}));
    REQUIRE_FALSE(env->ExceptionCheck());
  }
  const jchar unicode[]{'A', 0, 0x20ac, 0xd83d, 0xde00};
  std::memcpy(mem.at<void>(), unicode, sizeof(unicode));
  auto wide = invoke("NewString", args({mem.address, std::size(unicode)})).x0;
  REQUIRE(wide);
  for (auto critical : {false, true}) {
    auto p = invoke(critical ? "GetStringCritical" : "GetStringChars", args({wide, 0})).x0;
    REQUIRE(p);
    CHECK(std::memcmp(g2h(p), unicode, sizeof(unicode)) == 0);
    invoke(critical ? "ReleaseStringCritical" : "ReleaseStringChars", args({wide, p}));
  }
  auto pending_copy = invoke("GetIntArrayElements", args({array, 0})).x0;
  auto exception_class = env->FindClass("java/lang/IllegalStateException");
  REQUIRE(exception_class);
  env->ThrowNew(exception_class, "release must preserve this exception");
  invoke("ReleaseIntArrayElements", args({array, pending_copy, 0}));
  REQUIRE(env->ExceptionCheck());
  env->ExceptionClear();
  env->DeleteLocalRef(exception_class);
  auto cls = invoke("GetObjectClass", args({string})).x0;
  auto equals = method_id(cls, "equals", "(Ljava/lang/Object;)Z");
  *mem.at<std::uint64_t>() = (0xdeadcafeull << 32) | string;
  CHECK(invoke("CallBooleanMethodA", args({string, equals, mem.address})).x0 == JNI_TRUE);
  auto direct = invoke("NewDirectByteBuffer", args({mem.address}).value(std::int64_t(128))).x0;
  CHECK(invoke("GetDirectBufferAddress", args({direct})).x0 == mem.address);
  CHECK(invoke("GetDirectBufferCapacity", args({direct})).i64() == 128);
  REQUIRE_FALSE(env->ExceptionCheck());
  int high_storage[4]{};
  auto high = jni32::reference(env->NewDirectByteBuffer(high_storage, sizeof(high_storage)));
  CHECK(invoke("GetDirectBufferAddress", args({high})).x0 == 0);
  REQUIRE(env->ExceptionCheck());
  env->ExceptionClear();
}
TEST_CASE("A32 JNI instance and static fields use aligned pairs and softfp") {
  auto* env = start();
  auto cls = invoke("FindClass", args({h2g(low_string("A32Fields"))})).x0;
  REQUIRE(cls);
  auto ctor = method_id(cls, "<init>", "()V");
  auto object = invoke("NewObject", args({cls, ctor})).x0;
  REQUIRE(object);
  struct Field {
    const char *name, *suffix, *sig;
    std::uint64_t bits;
    hle::a32::Kind kind;
  };
  using K = hle::a32::Kind;
  Field fields[] = {{"z", "Boolean", "Z", 1, K::U8},
                    {"b", "Byte", "B", std::uint64_t(-101), K::I8},
                    {"c", "Char", "C", 0xffee, K::U16},
                    {"s", "Short", "S", std::uint64_t(-12345), K::I16},
                    {"i", "Int", "I", std::uint64_t(-0x1234567), K::I32},
                    {"j", "Long", "J", 0xfedcba9876543210ull, K::I64},
                    {"f", "Float", "F", std::bit_cast<std::uint32_t>(-1.25f), K::F32},
                    {"d", "Double", "D", std::bit_cast<std::uint64_t>(7.125), K::F64},
                    {"l", "Object", "Ljava/lang/Object;", object, K::U32}};
  for (bool stat : {false, true})
    for (auto field : fields) {
      auto id = invoke(stat ? "GetStaticFieldID" : "GetFieldID",
                       args({cls, h2g(low_string((stat ? "s" : "") + std::string(field.name))),
                             h2g(low_string(field.sig))}))
                    .x0;
      REQUIRE(id);
      auto prefix = stat ? "Static" : "";
      auto get = "Get" + std::string(prefix) + field.suffix + "Field";
      auto set = "Set" + std::string(prefix) + field.suffix + "Field";
      invoke(set.c_str(), args({stat ? cls : object, id}).add({field.kind}, field.bits));
      REQUIRE_FALSE(env->ExceptionCheck());
      auto result = invoke(get.c_str(), args({stat ? cls : object, id}));
      REQUIRE_FALSE(env->ExceptionCheck());
      if (field.kind == K::I64 || field.kind == K::F64)
        CHECK(result.u64() == field.bits);
      else if (std::string_view(field.suffix) == "Object")
        CHECK(invoke("IsSameObject", args({result.x0, object})).x0 == JNI_TRUE);
      else
        CHECK(result.x0 == static_cast<std::uint32_t>(field.bits));
    }
}
TEST_CASE("A32 NDK opaque tokens and output cells preserve canaries") {
  start();
  auto config = ndk("AConfiguration_new", {}).x0;
  REQUIRE(config);
  CHECK(config <= UINT32_MAX);
  ndk("AConfiguration_fromAssetManager",
      CallBuilder32{}.value(std::uint32_t(config)).pointer(android::package_asset_manager()));
  CHECK(ndk("AConfiguration_getSdkVersion", CallBuilder32{}.value(std::uint32_t(config))).i32() == 32);
  ndk("AConfiguration_delete", CallBuilder32{}.value(std::uint32_t(config)));
  CHECK_THROWS_AS(android::handles32::resolve(config, "AConfiguration"), std::invalid_argument);
  auto queue = android::create_input_queue();
  auto qt = android::handles32::encode(h2g(queue), "AInputQueue");
  android::InputEvent event{};
  event.type = 1;
  event.key_code = 66;
  REQUIRE(android::enqueue_input(queue, event));
  test::LowMemory32 mem;
  *mem.at<std::uint64_t>() = 0xfeedface00000000ull;
  CHECK(ndk("AInputQueue_getEvent", CallBuilder32{}.pointer(qt).pointer(mem.address)).i32() == 0);
  CHECK(*mem.at<std::uint32_t>(4) == 0xfeedface);
  auto et = *mem.at<std::uint32_t>();
  CHECK(ndk("AKeyEvent_getKeyCode", CallBuilder32{}.value(et)).i32() == 66);
  ndk("AInputQueue_finishEvent", CallBuilder32{}.pointer(qt).value(et).value(1));
  CHECK_THROWS_AS(android::handles32::resolve(et, "AInputEvent"), std::invalid_argument);
  android::destroy_input_queue(queue);
  auto looper = ndk("ALooper_prepare", CallBuilder32{}.value(1)).x0;
  REQUIRE(looper);
  queue = android::create_input_queue();
  qt = android::handles32::encode(h2g(queue), "AInputQueue");
  ndk("AInputQueue_attachLooper",
      CallBuilder32{}.pointer(qt).pointer(looper).value(77).value(0u).pointer(mem.address + 64));
  REQUIRE(android::enqueue_input(queue, event));
  *mem.at<std::uint64_t>(16) = 0xcafebabe00000000ull;
  CHECK(ndk("ALooper_pollAll",
            CallBuilder32{}.value(0).pointer(mem.address).pointer(mem.address + 8).pointer(mem.address + 16))
            .i32() == 77);
  CHECK(*mem.at<std::uint32_t>(16) == mem.address + 64);
  CHECK(*mem.at<std::uint32_t>(20) == 0xcafebabe);
  android::destroy_input_queue(queue);
  auto window = android::create_native_window(32, 16);
  auto wt = android::handles32::encode(h2g(window), "ANativeWindow");
  CHECK(android::native_window_from_guest(wt) == window);
  CHECK(ndk("ANativeWindow_getWidth", CallBuilder32{}.pointer(wt)).i32() == 32);
  std::memset(mem.at<void>(), 0xcc, 64);
  CHECK(ndk("ANativeWindow_lock", CallBuilder32{}.pointer(wt).pointer(mem.address).value(0u)).i32() == 0);
  CHECK(*mem.at<std::uint32_t>(44) == 0xcccccccc);
  CHECK(*mem.at<std::uint32_t>(16) != 0);
  CHECK(ndk("ANativeWindow_unlockAndPost", CallBuilder32{}.pointer(wt)).i32() == 0);
  ndk("ANativeWindow_release", CallBuilder32{}.pointer(wt));
  CHECK(android::native_window_from_guest(wt) == nullptr);
}
TEST_CASE("A32 RegisterNatives uses 12-byte records and real ARM return registers") {
  auto* env = start();
  auto host_cls = env->FindClass("BridgeFixture");
  REQUIRE(host_cls);
  auto cls = jni32::reference(host_cls);
  test::LowMemory32 code(false, true), records;
  struct Native {
    const char* name;
    const char* sig;
    std::vector<std::uint32_t> instructions;
  };
  Native natives[] = {{"reference", "(Ljava/lang/Object;)Ljava/lang/Object;", {0xe1a00002, 0xe12fff1e}},
                      {"reverse", "(JFJFJFJFJFJFJFJFJFJF)J", {0xe1a00002, 0xe1a01003, 0xe12fff1e}},
                      {"fp", "()F", {0xe3a00000, 0xe3430fa0, 0xe12fff1e}},
                      {"dp", "()D", {0xe3a00000, 0xe3a01000, 0xe34c1001, 0xe12fff1e}},
                      {"narrow", "()B", {0xe3e0007f, 0xe12fff1e}},
                      {"unicode", "()C", {0xe30f0fee, 0xe12fff1e}}};
  for (unsigned i = 0; i < std::size(natives); ++i) {
    auto& n = natives[i];
    std::memcpy(code.at<void>(i * 32), n.instructions.data(), n.instructions.size() * 4);
    records.at<std::uint32_t>()[3 * i] = h2g32(low_string(n.name));
    records.at<std::uint32_t>()[3 * i + 1] = h2g32(low_string(n.sig));
    records.at<std::uint32_t>()[3 * i + 2] = static_cast<std::uint32_t>(code.address + i * 32);
  }
  CHECK(invoke("RegisterNatives", args({cls, records.address, std::size(natives)})).i32() == JNI_OK);
  auto string = env->NewStringUTF("native object return");
  auto method = env->GetStaticMethodID(host_cls, "reference", "(Ljava/lang/Object;)Ljava/lang/Object;");
  REQUIRE(method);
  auto result = env->CallStaticObjectMethod(host_cls, method, string);
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(env->IsSameObject(result, string));
  env->DeleteLocalRef(result);
  env->DeleteLocalRef(string);
  {
    auto& config = process_config();
    const auto old_data = config.data_dir;
    config.data_dir = std::filesystem::temp_directory_path() / "venture-a32-native-paths";
    const auto host_path = (config.data_dir / "files/start.ini").generic_string();
    auto path = env->NewStringUTF(host_path.c_str());
    auto returned = static_cast<jstring>(env->CallStaticObjectMethod(host_cls, method, path));
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(returned);
    auto chars = env->GetStringUTFChars(returned, nullptr);
    REQUIRE(chars);
    CHECK(std::string(chars) == config.guest_data_dir() + "/files/start.ini");
    env->ReleaseStringUTFChars(returned, chars);
    chars = env->GetStringUTFChars(path, nullptr);
    CHECK(std::string(chars) == host_path);
    env->ReleaseStringUTFChars(path, chars);
    env->DeleteLocalRef(returned);
    env->DeleteLocalRef(path);
    config.data_dir = old_data;
  }
  method = env->GetStaticMethodID(host_cls, "exerciseReverse", "()J");
  REQUIRE(method);
  CHECK(env->CallStaticLongMethod(host_cls, method) == 1);
  REQUIRE_FALSE(env->ExceptionCheck());
  method = env->GetStaticMethodID(host_cls, "fp", "()F");
  REQUIRE(method);
  CHECK(env->CallStaticFloatMethod(host_cls, method) == 1.25f);
  REQUIRE_FALSE(env->ExceptionCheck());
  method = env->GetStaticMethodID(host_cls, "dp", "()D");
  REQUIRE(method);
  CHECK(env->CallStaticDoubleMethod(host_cls, method) == -2.125);
  REQUIRE_FALSE(env->ExceptionCheck());
  method = env->GetStaticMethodID(host_cls, "narrow", "()B");
  REQUIRE(method);
  CHECK(env->CallStaticByteMethod(host_cls, method) == -128);
  REQUIRE_FALSE(env->ExceptionCheck());
  method = env->GetStaticMethodID(host_cls, "unicode", "()C");
  REQUIRE(method);
  CHECK(env->CallStaticCharMethod(host_cls, method) == 0xffee);
  REQUIRE_FALSE(env->ExceptionCheck());
  CHECK(invoke("UnregisterNatives", args({cls})).i32() == JNI_OK);
}
TEST_CASE("A32 NativeActivity has ILP32 fields and guest-writable callback slots") {
  auto* env = start();
  test::LowMemory32 code(false, true);
  const std::uint32_t create_code[]{0xe580101c,
                                    0xe12fff1e};  // str r1,[r0,#28]; bx lr (saved-state -> instance)
  std::memcpy(code.at<void>(), create_code, sizeof(create_code));
  struct ActivityLibrary : Library {
    GuestAddr entry;
    explicit ActivityLibrary(GuestAddr p) : entry(p) {}
    GuestAddr find_symbol(std::string_view, std::string_view = {}) const override { return entry; }
    GuestIsa isa() const override { return GuestIsa::a32; }
  } library(code.address);
  auto cls = env->FindClass("java/lang/Object");
  REQUIRE(cls);
  auto ctor = env->GetMethodID(cls, "<init>", "()V");
  REQUIRE(ctor);
  auto object = env->NewObject(cls, ctor);
  REQUIRE_FALSE(env->ExceptionCheck());
  auto token = jni32::reference(env->NewGlobalRef(object), JNIGlobalRefType);
  const std::byte saved[]{std::byte{1}, std::byte{2}, std::byte{3}};
  std::string error;
  auto activity =
      android::NativeActivity::create(library, guest_vm(), guest_env(GuestThread::adopt_current()), token,
                                      &error, "ANativeActivity_onCreate", saved);
  REQUIRE_MESSAGE(activity, error);
  auto* wire = g2h<android::GuestNativeActivity32>(activity->address());
  CHECK(wire->callbacks == activity->address() + 40);
  CHECK(wire->clazz == token);
  CHECK(wire->vm == guest_vm());
  CHECK(wire->assetManager == android::package_asset_manager());
  CHECK(wire->sdkVersion == 32);
  CHECK(std::string(g2h<char>(wire->internalDataPath)) == process_config().guest_data_dir() + "/files");
  CHECK(std::string(g2h<char>(wire->externalDataPath)) == process_config().guest_external_files_dir());
  CHECK(std::string(g2h<char>(wire->obbPath)) == process_config().guest_obb_dir());
  REQUIRE(wire->instance);
  CHECK(std::memcmp(g2h(wire->instance), saved, 3) == 0);
  auto* callbacks = g2h<android::GuestActivityCallbacks32>(wire->callbacks);
  callbacks->slots[6] = static_cast<std::uint32_t>(code.address);  // focus -> stores boolean
  activity->focus(true);
  CHECK(wire->instance == 1);
  activity->focus(false);
  CHECK(wire->instance == 0);
  callbacks->slots[11] = static_cast<std::uint32_t>(code.address);  // input queue -> token
  auto queue = android::create_input_queue();
  activity->input_queue(queue);
  CHECK(android::handles32::resolve(wire->instance, "AInputQueue").get() == queue);
  activity->input_queue(nullptr);
  activity->destroy();
  android::destroy_input_queue(queue);
  // A real callback calls env->NewStringUTF(savedState) and records its token.
  // The Java/native transition owns that local, so it is stale after return.
  const std::uint32_t jni_code[]{0xe92d4010, 0xe1a04000, 0xe5940008, 0xe5902000,
                                 0xe592229c, 0xe12fff32, 0xe584001c, 0xe8bd8010};
  std::memcpy(code.at<void>(32), jni_code, sizeof(jni_code));
  library.entry = code.address + 32;
  const char text[] = "callback local";
  auto local_activity =
      android::NativeActivity::create(library, guest_vm(), guest_env(GuestThread::adopt_current()), token,
                                      &error, "ANativeActivity_onCreate", std::as_bytes(std::span(text)));
  REQUIRE_MESSAGE(local_activity, error);
  auto local_token = g2h<android::GuestNativeActivity32>(local_activity->address())->instance;
  REQUIRE(local_token);
  CHECK_THROWS_AS(jni32::object(local_token), std::invalid_argument);
  local_activity->destroy();
  jni32::delete_reference(token, JNIGlobalRefType);
  env->DeleteLocalRef(object);
  env->DeleteLocalRef(cls);
}

#ifdef VENTURE_TEST_JNI_INVOCATION_FIXTURE
#endif
