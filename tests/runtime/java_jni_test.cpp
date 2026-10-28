#include <doctest/doctest.h>

#include <bit>
#include <cstring>
#include <map>
#include <filesystem>
#include <fstream>
#include "core/elf.h"
#include "core/linker.h"
#include "core/process.h"
#include "core/thread_runtime.h"
#include <thread>

#include "fake_cpu.h"
#include "java/internal.h"
#include "java_test_support.h"
#ifdef VENTURE_TEST_JNI_INVOCATION_FIXTURE
#include "java_invocation_fixture.h"
#endif

using namespace venture;
using namespace venture::java;
namespace {
void invoke(const char* name, test::FakeCpu& cpu) {
  auto& thread = GuestThread::adopt_current();
  cpu.xs[0] = guest_env(thread);
  for (auto entry : jni_entries())
    if (std::string_view(entry.name) == name) {
      hle::FunctionInfo info{entry.handler, nullptr, "JNIEnv", name, 0};
      hle::CallContext ctx(thread, cpu, info);
      entry.handler(ctx);
      return;
    }
  FAIL("JNI slot absent");
}
jmethodID method(jclass cls, const char* name, const char* signature, bool is_static = false) {
  test::FakeCpu cpu;
  cpu.xs[1] = h2g(cls);
  cpu.xs[2] = h2g(name);
  cpu.xs[3] = h2g(signature);
  invoke(is_static ? "GetStaticMethodID" : "GetMethodID", cpu);
  return g2h<_jmethodID>(cpu.xs[0]);
}
// A complete AArch64 ELF, loaded by the production linker. Its exported
// functions contain real HLE thunks, preserving JNI callback observability.
void write_library(const std::string& name, const std::map<std::string, GuestAddr>& symbols) {
  std::array<std::byte, 4096> bytes{};
  auto put = [&]<class T>(std::size_t offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
  };
  elf::Header header{};
  std::memcpy(header.ident, "\177ELF\2\1\1", 7);
  header.type = 3; header.machine = 183; header.version = 1; header.ehsize = sizeof(header);
  header.phoff = 64; header.phentsize = sizeof(elf::Phdr); header.phnum = 2;
  put(0, header);
  put(64, elf::Phdr{1, 5, 0, 0, 0, 4096, 4096, 4096});
  put(120, elf::Phdr{2, 4, 0x200, 0x200, 0, 96, 96, 8});
  const std::array<elf::Dyn, 6> dynamic{{{4, 0x700}, {5, 0x800}, {6, 0x300}, {10, 1024}, {11, 24}, {0, 0}}};
  put(0x200, dynamic);
  put(0x700, std::uint32_t{1});
  put(0x704, static_cast<std::uint32_t>(symbols.size() + 1));
  put(0x708, symbols.empty() ? std::uint32_t{0} : std::uint32_t{1});
  std::uint32_t index = 1, offset = 1;
  for (const auto& [symbol, address] : symbols) {
    const auto code = 0xc00 + 32 * index;
    put(0x300 + 24 * index, elf::Sym{offset, 0x12, 0, 1, code, 32});
    put(0x70c + 4 * index, index < symbols.size() ? index + 1 : 0);
    std::memcpy(bytes.data() + 0x800 + offset, symbol.c_str(), symbol.size() + 1);
    std::memcpy(bytes.data() + code, g2h(address), 32);  // one thunk slot
    offset += static_cast<std::uint32_t>(symbol.size() + 1);
    ++index;
  }
  auto directory = std::filesystem::path(VENTURE_TEST_JAVA_CLASSES) / "native";
  std::filesystem::create_directories(directory);
  std::ofstream file(directory / name, std::ios::binary);
  file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  file.close();
  Linker::get().set_search_paths({directory.string()});
}
}  // namespace

TEST_CASE("HotSpot JNI tables, all invocation families and native trampolines") {
  register_jni_library();
  auto* art = Linker::get().open("libart.so", kRtldNow, nullptr);
  REQUIRE(art);
  auto enumerate = art->find_symbol("JNI_GetCreatedJavaVMs");
  REQUIRE(enumerate);
  jint vm_count = -1;
  CHECK(GuestThread::adopt_current().call(enumerate, {0, 0, h2g(&vm_count)}) == JNI_OK);
  CHECK(vm_count == 0); // enumeration must not start a VM
  JvmOptions options;
  options.java_home = VENTURE_TEST_JAVA_HOME;
  options.agent_path = VENTURE_TEST_JVM_AGENT;
  options.class_path = {VENTURE_TEST_BRIDGE_JAR, VENTURE_TEST_JAVA_CLASSES};
  std::string error;
  REQUIRE_MESSAGE(start_jvm(options, &error), error);
  auto* env = host_env();
  REQUIRE(env);
  {
    auto* library = Linker::get().open("libventure_java_bridge.so", kRtldNow, nullptr);
    REQUIRE(library);
    const auto install = library->find_symbol("VentureJavaInstallHook");
    const auto remove = library->find_symbol("VentureJavaRemoveHook");
    REQUIRE(install);
    REQUIRE(remove);
    auto system = env->FindClass("java/lang/System");
    REQUIRE(system);
    // The event callback enters a different JNI native frame. Retain captured
    // references globally, rather than borrowing this invocation frame's locals.
    auto system_local = system;
    system = static_cast<jclass>(env->NewGlobalRef(system_local));
    env->DeleteLocalRef(system_local);
    REQUIRE(system);
    auto rewritten = env->FindClass("com/venture/runtime/NativeLoader");
    REQUIRE(rewritten);
    auto load = method(system, "loadLibrary", "(Ljava/lang/String;)V", true);
    REQUIRE(load);
    auto alias = env->GetStaticMethodID(rewritten, "loadLibrary", "(Ljava/lang/String;)V");
    REQUIRE(alias);
    auto name = env->NewStringUTF("venture_missing_hook_fixture");
    REQUIRE(name);
    auto name_local = name;
    name = static_cast<jstring>(env->NewGlobalRef(name_local));
    env->DeleteLocalRef(name_local);
    REQUIRE(name);
    int calls = 0;
    int releases = 0;
    jobject expected_receiver = system;
    bool call_original = false;
    test::java_guest_call = [&](GuestAddr id, const GuestCallArgs& args) {
      if (id == 9002) {
        REQUIRE(args.integers.size() == 1);
        CHECK(args.integers[0] == 0x1234);
        ++releases;
        return GuestCallResult{};
      }
      CHECK(id == 9001);
      REQUIRE(args.integers.size() == 5);
      CHECK(env->IsSameObject(g2h<_jobject>(args.integers[1]), expected_receiver));
      auto argument = g2h<jvalue>(args.integers[2]);
      CHECK(env->IsSameObject(argument[0].l, name));
      ++calls;
      if (call_original) {
        // The active hook must bypass itself and enter the real guest linker once.
        test::FakeCpu cpu;
        cpu.xs[1] = h2g(system); cpu.xs[2] = h2g(load); cpu.xs[3] = h2g(argument);
        invoke("CallStaticVoidMethodA", cpu);
        CHECK(env->ExceptionCheck());
        env->ExceptionClear();
      }
      return GuestCallResult{1};
    };
    const auto callback = test::java_thunk(9001, 5);
    const auto release_callback = test::java_thunk(9002, 1);
    auto& thread = GuestThread::adopt_current();
    CHECK(static_cast<jint>(thread.call(install, {1, callback, 0, 0})) == 23); // invalid ID is rejected before HotSpot
    REQUIRE(thread.call(install, {h2g(load), callback, 0x1234, release_callback}) == 0);
    // A host JNI call must keep host library semantics even while guest
    // System.loadLibrary is replaced; no guest callback may capture it.
    env->CallStaticVoidMethod(system, load, name);
    REQUIRE(env->ExceptionCheck());
    env->ExceptionClear();
    CHECK(calls == 0);
    auto image_class = env->FindClass("java/awt/image/BufferedImage");
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(image_class);
    auto image_ctor = env->GetMethodID(image_class, "<init>", "(III)V");
    REQUIRE(image_ctor);
    auto image = env->NewObject(image_class, image_ctor, 1, 1, 2);
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(image);
    CHECK(calls == 0);
    env->DeleteLocalRef(image);
    env->DeleteLocalRef(image_class);
    env->CallStaticVoidMethod(rewritten, alias, name);
    REQUIRE_FALSE(env->ExceptionCheck());
    CHECK(calls == 1);
    jvalue argument{}; argument.l = name;
    test::FakeCpu cpu;
    cpu.xs[1] = h2g(system); cpu.xs[2] = h2g(load); cpu.xs[3] = h2g(&argument);
    invoke("CallStaticVoidMethodA", cpu);
    REQUIRE_FALSE(env->ExceptionCheck());
    CHECK(calls == 2);
    auto reflected = env->ToReflectedMethod(system, load, JNI_TRUE);
    REQUIRE(reflected);
    auto object = env->FindClass("java/lang/Object");
    REQUIRE(object);
    auto arguments = env->NewObjectArray(1, object, name);
    REQUIRE(arguments);
    invoke_reflective(env, reflected, nullptr, arguments);
    REQUIRE_FALSE(env->ExceptionCheck());
    CHECK(calls == 3);
    REQUIRE(thread.call(install, {h2g(load), callback, 0x1234, release_callback}) == 0);
    CHECK(releases == 1);
    call_original = true;
    env->CallStaticVoidMethod(rewritten, alias, name);
    REQUIRE_FALSE(env->ExceptionCheck());
    CHECK(calls == 4);
    REQUIRE(thread.call(remove, {h2g(load)}) == 0);
    CHECK(releases == 2);
    env->CallStaticVoidMethod(rewritten, alias, name);
    CHECK(env->ExceptionCheck());
    env->ExceptionClear();
    CHECK(calls == 4);
    auto runtime_class = env->FindClass("java/lang/Runtime");
    REQUIRE(runtime_class);
    auto get_runtime = env->GetStaticMethodID(runtime_class, "getRuntime", "()Ljava/lang/Runtime;");
    REQUIRE(get_runtime);
    auto runtime_local = env->CallStaticObjectMethod(runtime_class, get_runtime);
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(runtime_local);
    auto runtime = env->NewGlobalRef(runtime_local);
    REQUIRE(runtime);
    env->DeleteLocalRef(runtime_local);
    expected_receiver = runtime;
    call_original = false;
    auto runtime_load = method(runtime_class, "loadLibrary", "(Ljava/lang/String;)V");
    REQUIRE(runtime_load);
    auto runtime_alias = env->GetStaticMethodID(rewritten, "loadLibrary", "(Ljava/lang/Runtime;Ljava/lang/String;)V");
    REQUIRE(runtime_alias);
    REQUIRE(thread.call(install, {h2g(runtime_load), callback, 0x1234, release_callback}) == 0);
    env->CallVoidMethod(runtime, runtime_load, name);
    REQUIRE(env->ExceptionCheck());
    env->ExceptionClear();
    CHECK(calls == 4);
    env->CallStaticVoidMethod(rewritten, runtime_alias, runtime, name);
    REQUIRE_FALSE(env->ExceptionCheck());
    CHECK(calls == 5);
    env->CallStaticVoidMethod(rewritten, runtime_alias, static_cast<jobject>(nullptr), name);
    CHECK(env->ExceptionCheck());
    env->ExceptionClear();
    CHECK(calls == 5);
    REQUIRE(thread.call(remove, {h2g(runtime_load)}) == 0);
    CHECK(releases == 3);
    env->DeleteGlobalRef(runtime);
    env->DeleteLocalRef(runtime_class);
    test::java_guest_call = {};
    env->DeleteGlobalRef(name);
    env->DeleteLocalRef(rewritten);
    env->DeleteGlobalRef(system);
    env->DeleteLocalRef(reflected);
    env->DeleteLocalRef(object);
    env->DeleteLocalRef(arguments);
  }
  {
    // Inspect the running VM, not just the options we intended to supply.
    auto factory = env->FindClass("java/lang/management/ManagementFactory");
    REQUIRE(factory);
    auto get = env->GetStaticMethodID(factory, "getRuntimeMXBean",
                                     "()Ljava/lang/management/RuntimeMXBean;");
    REQUIRE(get);
    auto bean = env->CallStaticObjectMethod(factory, get);
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(bean);
    auto bean_class = env->FindClass("java/lang/management/RuntimeMXBean");
    REQUIRE(bean_class);
    auto arguments = env->GetMethodID(bean_class, "getInputArguments", "()Ljava/util/List;");
    REQUIRE(arguments);
    auto flags = env->CallObjectMethod(bean, arguments);
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(flags);
    auto list = env->FindClass("java/util/List");
    REQUIRE(list);
    auto contains = env->GetMethodID(list, "contains", "(Ljava/lang/Object;)Z");
    REQUIRE(contains);
    for (const char* flag : {"-Xcheck:jni", "-Xverify:all"}) {
      auto word = env->NewStringUTF(flag);
      auto present = env->CallBooleanMethod(flags, contains, word);
      REQUIRE_FALSE(env->ExceptionCheck());
      CHECK(present == JNI_TRUE);
      env->DeleteLocalRef(word);
    }
    for (auto ref : {jobject(factory), bean, jobject(bean_class), flags, jobject(list)})
      env->DeleteLocalRef(ref);
  }
  {
    auto runtime = env->FindClass("java/lang/Runtime");
    REQUIRE(runtime);
    auto get = env->GetStaticMethodID(runtime, "getRuntime", "()Ljava/lang/Runtime;");
    auto total = env->GetMethodID(runtime, "totalMemory", "()J");
    auto maximum = env->GetMethodID(runtime, "maxMemory", "()J");
    REQUIRE(get); REQUIRE(total); REQUIRE(maximum);
    auto instance = env->CallStaticObjectMethod(runtime, get);
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(instance);
    auto initial = env->CallLongMethod(instance, total);
    REQUIRE_FALSE(env->ExceptionCheck());
    CHECK(initial < 128 * 1024 * 1024);
    auto limit = env->CallLongMethod(instance, maximum);
    REQUIRE_FALSE(env->ExceptionCheck());
    CHECK(limit <= 1024ll * 1024 * 1024);
    // A real allocation larger than the initial heap must grow successfully.
    auto array = env->NewByteArray(64 * 1024 * 1024);
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(array);
    CHECK(env->GetArrayLength(array) == 64 * 1024 * 1024);
    auto grown = env->CallLongMethod(instance, total);
    REQUIRE_FALSE(env->ExceptionCheck());
    CHECK(grown > initial);
    env->DeleteLocalRef(array);
    env->DeleteLocalRef(instance);
    env->DeleteLocalRef(runtime);
  }
  auto cls = env->FindClass("BridgeFixture");
  REQUIRE(cls);
  auto ctor = method(cls, "<init>", "(I)V");
  REQUIRE(ctor);
  auto object = env->NewObject(cls, ctor, 17);
  REQUIRE_FALSE(env->ExceptionCheck());
  REQUIRE(object);
  auto& thread = GuestThread::adopt_current();
  auto guest = guest_env(thread);
  auto* table = g2h<GuestAddr>(*g2h<GuestAddr>(guest));
  for (int i = 0; i < 4; ++i) CHECK(table[i] == 0);
  for (auto entry : jni_entries()) CHECK(table[entry.slot] != 0);
  REQUIRE(jni_entries().size() == 229);
  {
    test::FakeCpu cpu;
    cpu.xs[1] = h2g("venture/test/Absent");
    invoke("FindClass", cpu);
    CHECK(cpu.xs[0] == 0);
    REQUIRE(env->ExceptionCheck());
    auto failure = env->ExceptionOccurred();
    env->ExceptionClear();
    auto expected = env->FindClass("java/lang/NoClassDefFoundError");
    REQUIRE(expected);
    CHECK(env->IsInstanceOf(failure, expected));
    env->DeleteLocalRef(failure);
    env->DeleteLocalRef(expected);
  }
  struct Expected {
    const char* type;
    const char* name;
    const char* sig;
    std::uint64_t bits;
  };
  Expected expected[] = {{"Object", "obj", "()Ljava/lang/Object;", 0},
                         {"Boolean", "bool", "()Z", 1},
                         {"Byte", "byt", "()B", static_cast<std::uint64_t>(-128)},
                         {"Char", "chr", "()C", 0xffee},
                         {"Short", "shrt", "()S", static_cast<std::uint64_t>(-32000)},
                         {"Int", "integer", "()I", static_cast<std::uint64_t>(-1234567)},
                         {"Long", "lng", "()J", static_cast<std::uint64_t>(-0x123456789abcdefLL)},
                         {"Float", "flt", "()F", std::bit_cast<std::uint32_t>(1.25f)},
                         {"Double", "dbl", "()D", std::bit_cast<std::uint64_t>(-2.125)},
                         {"Void", "nothing", "()V", 0}};
  for (auto& row : expected)
    for (auto mode : {"", "Static", "Nonvirtual"})
      for (auto form : {"", "V", "A"}) {
        bool stat = std::string_view(mode) == "Static", nonvirtual = std::string_view(mode) == "Nonvirtual";
        std::string name = (stat ? "s" : "") + std::string(row.name);
        auto mid = method(cls, name.c_str(), row.sig, stat);
        REQUIRE(mid);
        test::FakeCpu cpu;
        cpu.xs[1] = h2g(stat ? cls : object);
        unsigned index = 2;
        if (nonvirtual) cpu.xs[index++] = h2g(cls);
        cpu.xs[index++] = h2g(mid);
        hle::GuestVaListStruct list{};
        if (std::string_view(form) == "V") cpu.xs[index] = h2g(&list);
        std::string function = "Call" + std::string(mode) + row.type + "Method" + form;
        invoke(function.c_str(), cpu);
        REQUIRE_FALSE(env->ExceptionCheck());
        if (std::string_view(row.type) == "Object") {
          auto returned = g2h<_jobject>(cpu.xs[0]);
          REQUIRE(returned);
          if (!stat) CHECK(env->IsSameObject(returned, object));
          env->DeleteLocalRef(returned);
        } else if (std::string_view(row.type) == "Float" || std::string_view(row.type) == "Double")
          CHECK(cpu.vs[0].lo == row.bits);
        else if (std::string_view(row.type) != "Void")
          CHECK(cpu.xs[0] == row.bits);
      }
  // Exercise every return/dispatch/form with >8 GP, >8 promoted FP, and
  // interleaved overflow. The Java bodies check every parameter individually.
  for (auto& row : expected)
    for (auto mode : {"", "Static", "Nonvirtual"})
      for (auto form : {"", "V", "A"}) {
        bool stat = std::string_view(mode) == "Static", nonvirtual = std::string_view(mode) == "Nonvirtual";
        std::string name = (stat ? "s" : "") + std::string("mixed") + row.name;
        std::string sig = "(JFJFJFJFJFJFJFJFJFJF)" + std::string(row.sig).substr(2);
        auto mid = method(cls, name.c_str(), sig.c_str(), stat);
        REQUIRE(mid);
        test::FakeCpu cpu;
        cpu.xs[1] = h2g(stat ? cls : object);
        unsigned index = 2;
        if (nonvirtual) cpu.xs[index++] = h2g(cls);
        cpu.xs[index++] = h2g(mid);
        jvalue array[20]{};
        std::uint64_t stack[24]{}, gp[5]{}, vr[16]{};
        unsigned count = 0;
        for (unsigned i = 0; i < 10; ++i) {
          array[2 * i].j = 100 + i;
          array[2 * i + 1].f = i + 0.25f;
          auto fp = std::bit_cast<std::uint64_t>(i + 0.25);
          if (std::string_view(form) == "V") {
            if (i < 5)
              gp[i] = 100 + i;
            else
              stack[count++] = 100 + i;
            if (i < 8)
              vr[2 * i] = fp;
            else
              stack[count++] = fp;
          } else {
            if (i < 8 - index)
              cpu.xs[index + i] = 100 + i;
            else
              stack[count++] = 100 + i;
            if (i < 8)
              cpu.vs[i].lo = fp;
            else
              stack[count++] = fp;
          }
        }
        cpu.use_stack(stack);
        hle::GuestVaListStruct list{h2g(stack), h2g(gp + 5), h2g(vr + 16), -40, -128};
        if (std::string_view(form) == "V") cpu.xs[index] = h2g(&list);
        if (std::string_view(form) == "A") cpu.xs[index] = h2g(array);
        std::string function = "Call" + std::string(mode) + row.type + "Method" + form;
        invoke(function.c_str(), cpu);
        REQUIRE_FALSE(env->ExceptionCheck());
        if (std::string_view(row.type) == "Object") {
          auto ref = g2h<_jobject>(cpu.xs[0]);
          REQUIRE(ref);
          if (!stat) CHECK(env->IsSameObject(ref, object));
          env->DeleteLocalRef(ref);
        } else if (std::string_view(row.type) == "Float" || std::string_view(row.type) == "Double")
          CHECK(cpu.vs[0].lo == row.bits);
        else if (std::string_view(row.type) != "Void")
          CHECK(cpu.xs[0] == row.bits);
      }
  {
    auto mid = env->GetStaticMethodID(cls, "reflectedOnly", "(IF)D");
    REQUIRE(mid);
    auto reflected = env->ToReflectedMethod(cls, mid, JNI_TRUE);
    REQUIRE(reflected);
    test::FakeCpu cpu;
    cpu.xs[1] = h2g(reflected);
    invoke("FromReflectedMethod", cpu);
    REQUIRE_FALSE(env->ExceptionCheck());
    CHECK(cpu.xs[0] == h2g(mid));
    cpu.xs[1] = h2g(cls);
    cpu.xs[2] = h2g(mid);
    cpu.xs[3] = 17;
    cpu.vs[0].lo = std::bit_cast<std::uint64_t>(0.75);
    invoke("CallStaticDoubleMethod", cpu);
    REQUIRE_FALSE(env->ExceptionCheck());
    CHECK(std::bit_cast<double>(cpu.vs[0].lo) == 17.75);
    env->DeleteLocalRef(reflected);
  }
  {
    auto child = env->FindClass("BridgeFixture$Child");
    REQUIRE(child);
    auto create = env->GetMethodID(child, "<init>", "()V");
    REQUIRE(create);
    auto instance = env->NewObject(child, create);
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(instance);
    auto integer = method(cls, "integer", "()I");
    test::FakeCpu cpu;
    cpu.xs[1] = h2g(instance);
    cpu.xs[2] = h2g(integer);
    invoke("CallIntMethod", cpu);
    REQUIRE_FALSE(env->ExceptionCheck());
    CHECK(cpu.xs[0] == 777);
    cpu.xs[1] = h2g(instance);
    cpu.xs[2] = h2g(cls);
    cpu.xs[3] = h2g(integer);
    invoke("CallNonvirtualIntMethod", cpu);
    REQUIRE_FALSE(env->ExceptionCheck());
    CHECK(static_cast<jint>(cpu.xs[0]) == -1234567);
    env->DeleteLocalRef(instance);
    env->DeleteLocalRef(child);
  }
  {
    for (auto form : {"", "V", "A"}) {
      test::FakeCpu cpu;
      cpu.xs[1] = h2g(cls);
      cpu.xs[2] = h2g(ctor);
      jvalue array[1]{};
      array[0].i = 42;
      std::uint64_t stack[1] = {42};
      hle::GuestVaListStruct list{h2g(stack), 0, 0, 0, 0};
      cpu.xs[3] = std::string_view(form) == "V"   ? h2g(&list)
                  : std::string_view(form) == "A" ? h2g(array)
                                                  : 42;
      invoke(("NewObject" + std::string(form)).c_str(), cpu);
      REQUIRE_FALSE(env->ExceptionCheck());
      auto result = g2h<_jobject>(cpu.xs[0]);
      REQUIRE(result);
      auto field = env->GetFieldID(cls, "seed", "I");
      REQUIRE(field);
      CHECK(env->GetIntField(result, field) == 42);
      env->DeleteLocalRef(result);
    }
  }
  {
    // UE4 passes File.getPath() into nativeSetGlobalActivity from Java. Both
    // host register and stack arguments must enter the guest mount namespace.
    auto& config = process_config();
    const auto old_data = config.data_dir;
    config.data_dir = std::filesystem::temp_directory_path() / "venture-native-paths";
    const auto filename = u"files/\U0001f680.ini";
    const auto host_path = to_modified_utf8((config.data_dir / filename).generic_u16string());
    const auto guest_path = config.guest_data_dir() + "/" + to_modified_utf8(filename);
    const char* signature = "(Ljava/lang/String;IJDLjava/lang/String;)Ljava/lang/String;";
    JNINativeMethod native{const_cast<char*>("paths"), const_cast<char*>(signature),
                           g2h(test::java_thunk(103, 6, 1, 0))};
    REQUIRE(register_natives(env, cls, &native, 1) == JNI_OK);
    auto paths = env->GetStaticMethodID(cls, "paths", signature);
    REQUIRE(paths);
    auto text = env->NewStringUTF(host_path.c_str());
    const std::string plain_text = "ordinary text \xc0\x80\xed\xa0\x80";
    auto plain = env->NewStringUTF(plain_text.c_str());
    std::string expected_last;
    auto check_text = [&](jobject object, const std::string& expected) {
      const char* value = env->GetStringUTFChars(static_cast<jstring>(object), nullptr);
      REQUIRE(value);
      CHECK(std::string(value) == expected);
      env->ReleaseStringUTFChars(static_cast<jstring>(object), value);
    };
    test::java_guest_call = [&](GuestAddr fn, const GuestCallArgs& args) {
      CHECK(fn == 103);
      check_text(g2h<_jobject>(args.integers[2]), guest_path);
      CHECK(args.integers[3] == 7);
      CHECK(args.integers[4] == 0x123456789abcdefull);
      CHECK(args.doubles[0] == 1.25);
      auto last = g2h<_jobject>(args.integers[5]);
      if (last) check_text(last, expected_last);
      return GuestCallResult{args.integers[5]};
    };
    for (auto last : {text, plain, static_cast<jstring>(nullptr)}) {
      expected_last = last == text ? guest_path : plain_text;
      auto returned = env->CallStaticObjectMethod(cls, paths, text, 7, jlong{0x123456789abcdef}, 1.25, last);
      REQUIRE_FALSE(env->ExceptionCheck());
      if (returned) {
        check_text(returned, expected_last);
        if (last == plain) CHECK(env->IsSameObject(returned, plain));
        env->DeleteLocalRef(returned);
      } else CHECK(last == nullptr);
      check_text(text, host_path); // Java's original string remains usable by JDK I/O.
    }
    env->DeleteLocalRef(text); env->DeleteLocalRef(plain);
    config.data_dir = old_data;
    test::java_guest_call = {};
  }
  {
    JNINativeMethod native[] = {
        {const_cast<char*>("reverse"), const_cast<char*>("(JFJFJFJFJFJFJFJFJFJF)J"), g2h(test::java_thunk(100, 8, 8, 6))},
        {const_cast<char*>("touch"), const_cast<char*>("(JLjava/lang/Object;IIIIIJJIIIIIIFF)Z"), g2h(test::java_thunk(101, 8, 2, 9))},
        {const_cast<char*>("narrow"), const_cast<char*>("()B"), g2h(test::java_thunk(102, 2, 0, 0))}};
    REQUIRE(register_natives(env, cls, native, 3) == JNI_OK);
    test::java_guest_call = [&](GuestAddr fn, const GuestCallArgs& args) {
      CHECK(args.integers[0] == guest);
      CHECK(args.integers.size() <= 8);
      CHECK(args.doubles.size() <= 8);
      if (fn == 100) {
        REQUIRE(args.integers.size() == 8);
        REQUIRE(args.doubles.size() == 8);
        REQUIRE(args.stack.size() == 6);
        CHECK(args.integers[2] == 1);
        CHECK(args.integers[7] == 6);
        CHECK(args.stack[0] == 7);
        CHECK(args.stack[1] == 8);
        CHECK(args.stack[2] == 9);
        CHECK(args.stack[3] == std::bit_cast<std::uint32_t>(9.25f));
        CHECK(args.stack[4] == 10);
        CHECK(args.stack[5] == std::bit_cast<std::uint32_t>(10.25f));
        return GuestCallResult{123456789};
      }
      if (fn == 101) {
        CHECK(args.integers[2] == 0x123456789abcdef);
        CHECK(args.stack.size() == 9);
        CHECK(std::bit_cast<std::uint64_t>(args.doubles[0]) == std::bit_cast<std::uint32_t>(1.25f));
        return GuestCallResult{1};
      }
      return GuestCallResult{0x80};
    };
    auto exercise = env->GetStaticMethodID(cls, "exerciseReverse", "()J");
    REQUIRE(exercise);
    CHECK(env->CallStaticLongMethod(cls, exercise) == 123456789);
    REQUIRE_FALSE(env->ExceptionCheck());
    exercise = env->GetStaticMethodID(cls, "exerciseTouch", "()Z");
    REQUIRE(exercise);
    CHECK(env->CallStaticBooleanMethod(cls, exercise) == JNI_TRUE);
    REQUIRE_FALSE(env->ExceptionCheck());
    exercise = env->GetStaticMethodID(cls, "narrow", "()B");
    REQUIRE(exercise);
    CHECK(env->CallStaticByteMethod(cls, exercise) == -128);
    REQUIRE_FALSE(env->ExceptionCheck());
    // JNI cannot let C++ exit exceptions cross HotSpot's native transition.
    // They must terminate the Java invocation, preserving real faults as errors.
    for (bool fault : {false, true}) {
      test::java_guest_call = [=](GuestAddr, const GuestCallArgs&) -> GuestCallResult {
        if (fault) throw std::runtime_error("native regression fault");
        throw ThreadExit(0);
      };
      env->CallStaticByteMethod(cls, exercise);
      REQUIRE(env->ExceptionCheck());
      auto exception = env->ExceptionOccurred();
      env->ExceptionClear();
      auto expected = env->FindClass(fault ? "java/lang/InternalError" : "java/lang/ThreadDeath");
      REQUIRE(expected);
      CHECK(env->IsInstanceOf(exception, expected));
      env->DeleteLocalRef(expected);
      env->DeleteLocalRef(exception);
    }
  }
  {
    JNINativeMethod native[] = {{const_cast<char*>("unicode"), const_cast<char*>("()C"), g2h(test::java_thunk(200, 2, 0, 0))},
                                {const_cast<char*>("signedShort"), const_cast<char*>("()S"), g2h(test::java_thunk(201, 2, 0, 0))},
                                {const_cast<char*>("fp"), const_cast<char*>("()F"), g2h(test::java_thunk(202, 2, 0, 0))},
                                {const_cast<char*>("dp"), const_cast<char*>("()D"), g2h(test::java_thunk(203, 2, 0, 0))},
                                {const_cast<char*>("reference"),
                                 const_cast<char*>("(Ljava/lang/Object;)Ljava/lang/Object;"), g2h(test::java_thunk(204, 3, 0, 0))},
                                {const_cast<char*>("throwsGuest"), const_cast<char*>("()V"), g2h(test::java_thunk(205, 2, 0, 0))},
                                {const_cast<char*>("recurse"), const_cast<char*>("(I)I"), g2h(test::java_thunk(206, 3, 0, 0))}};
    // Go through the guest table, not just the registration helper.
    test::FakeCpu registration;
    registration.xs[1] = h2g(cls);
    registration.xs[2] = h2g(native);
    registration.xs[3] = std::size(native);
    invoke("RegisterNatives", registration);
    REQUIRE(registration.xs[0] == JNI_OK);
    REQUIRE_FALSE(env->ExceptionCheck());
    auto recursive = method(cls, "javaRecurse", "(I)I", true);
    REQUIRE(recursive);
    test::java_guest_call = [&](GuestAddr fn, const GuestCallArgs& args) {
      if (fn == 200) return GuestCallResult{0xffffffee};
      if (fn == 201) return GuestCallResult{0xffff8001};
      if (fn == 202) return GuestCallResult{0, 0, {std::bit_cast<std::uint32_t>(-1.25f), 0}};
      if (fn == 203) return GuestCallResult{0, 0, {std::bit_cast<std::uint64_t>(1.0 / 3.0), 0}};
      if (fn == 204) return GuestCallResult{h2g(env->NewLocalRef(g2h<_jobject>(args.integers[2])))};
      if (fn == 205) {
        throw_java(env, "java/lang/IllegalStateException", "guest pending exception");
        return GuestCallResult{};
      }
      test::FakeCpu cpu;
      cpu.xs[1] = args.integers[1];
      cpu.xs[2] = h2g(recursive);
      cpu.xs[3] = args.integers[2];
      invoke("CallStaticIntMethod", cpu);
      return GuestCallResult{cpu.xs[0]};
    };
    auto id = env->GetStaticMethodID(cls, "unicode", "()C");
    REQUIRE(id);
    CHECK(env->CallStaticCharMethod(cls, id) == 0xffee);
    REQUIRE_FALSE(env->ExceptionCheck());
    id = env->GetStaticMethodID(cls, "signedShort", "()S");
    REQUIRE(id);
    CHECK(env->CallStaticShortMethod(cls, id) == -32767);
    REQUIRE_FALSE(env->ExceptionCheck());
    id = env->GetStaticMethodID(cls, "fp", "()F");
    REQUIRE(id);
    CHECK(env->CallStaticFloatMethod(cls, id) == -1.25f);
    REQUIRE_FALSE(env->ExceptionCheck());
    id = env->GetStaticMethodID(cls, "dp", "()D");
    REQUIRE(id);
    CHECK(env->CallStaticDoubleMethod(cls, id) == doctest::Approx(1.0 / 3.0));
    REQUIRE_FALSE(env->ExceptionCheck());
    id = env->GetStaticMethodID(cls, "reference", "(Ljava/lang/Object;)Ljava/lang/Object;");
    REQUIRE(id);
    auto returned = env->CallStaticObjectMethod(cls, id, object);
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(returned);
    CHECK(env->IsSameObject(returned, object));
    env->DeleteLocalRef(returned);
    id = env->GetStaticMethodID(cls, "recurse", "(I)I");
    REQUIRE(id);
    CHECK(env->CallStaticIntMethod(cls, id, 12) == 19);
    REQUIRE_FALSE(env->ExceptionCheck());
    id = env->GetStaticMethodID(cls, "throwsGuest", "()V");
    REQUIRE(id);
    env->CallStaticVoidMethod(cls, id);
    REQUIRE(env->ExceptionCheck());
    test::FakeCpu cpu;
    invoke("ExceptionCheck", cpu);
    CHECK(cpu.xs[0] == JNI_TRUE);
    invoke("ExceptionOccurred", cpu);
    REQUIRE(cpu.xs[0]);
    auto exception = g2h<_jthrowable>(cpu.xs[0]);
    invoke("ExceptionClear", cpu);
    CHECK_FALSE(env->ExceptionCheck());
    env->DeleteLocalRef(exception);
  }
  {
    // Identity pointers remain usable until Release/Pop, and direct buffers
    // preserve the native address rather than exposing managed object layouts.
    test::FakeCpu cpu;
    const char* utf = "hello\xc0\x80world";
    cpu.xs[1] = h2g(utf);
    invoke("NewStringUTF", cpu);
    auto string = g2h<_jstring>(cpu.xs[0]);
    REQUIRE(string);
    cpu.xs[1] = h2g(string);
    invoke("GetStringLength", cpu);
    CHECK(cpu.xs[0] == 11);
    cpu.xs[1] = h2g(string);
    cpu.xs[2] = 0;
    invoke("GetStringUTFChars", cpu);
    auto bytes = cpu.xs[0];
    CHECK(std::string(g2h<char>(bytes)) == utf);
    // ART release owns the UTF copy independently of the jstring argument.
    // Unity 2020 supplies these filename bytes as its release reference.
    env->DeleteLocalRef(string);
    cpu.xs[1] = 0x6e6f736a2e;
    cpu.xs[2] = bytes;
    invoke("ReleaseStringUTFChars", cpu);
    REQUIRE_FALSE(env->ExceptionCheck());
    // A stale/foreign buffer must remain an observable failure, not reach JVM.
    invoke("ReleaseStringUTFChars", cpu);
    REQUIRE(env->ExceptionCheck());
    env->ExceptionClear();
    cpu.xs[1] = 0;
    cpu.xs[2] = 0;
    invoke("ReleaseStringUTFChars", cpu);
    REQUIRE_FALSE(env->ExceptionCheck());
    cpu.xs[1] = 4;
    invoke("NewIntArray", cpu);
    auto array = cpu.xs[0];
    jint values[4] = {1, 2, 3, 4};
    cpu.xs[1] = array;
    cpu.xs[2] = 0;
    cpu.xs[3] = 4;
    cpu.xs[4] = h2g(values);
    invoke("SetIntArrayRegion", cpu);
    cpu.xs[1] = array;
    cpu.xs[2] = 0;
    invoke("GetPrimitiveArrayCritical", cpu);
    auto ptr = cpu.xs[0];
    REQUIRE(ptr);
    g2h<jint>(ptr)[2] = 99;
    cpu.xs[1] = array;
    cpu.xs[2] = ptr;
    cpu.xs[3] = 0;
    invoke("ReleasePrimitiveArrayCritical", cpu);
    env->GetIntArrayRegion(g2h<_jintArray>(array), 0, 4, values);
    CHECK(values[2] == 99);
    cpu.xs[1] = h2g(values);
    cpu.xs[2] = sizeof(values);
    invoke("NewDirectByteBuffer", cpu);
    auto buffer = cpu.xs[0];
    cpu.xs[1] = buffer;
    invoke("GetDirectBufferAddress", cpu);
    CHECK(cpu.xs[0] == h2g(values));
    cpu.xs[1] = buffer;
    invoke("GetDirectBufferCapacity", cpu);
    CHECK(cpu.xs[0] == sizeof(values));
    for (auto ref : {array, buffer}) {
      cpu.xs[1] = ref;
      invoke("DeleteLocalRef", cpu);
    }
    REQUIRE_FALSE(env->ExceptionCheck());
  }
  {
    std::atomic<bool> attached = false, detached = false, affine = false;
    std::thread worker([&] {
      CHECK(env_no_attach() == nullptr);
      auto* worker_env = host_env();
      attached = worker_env != nullptr && worker_env != env;
      auto worker_guest = guest_env(GuestThread::adopt_current());
      affine = worker_guest != guest;
      CHECK(guest_env(GuestThread::adopt_current()) == worker_guest);
      detached = detach_host() == JNI_OK && env_no_attach() == nullptr;
    });
    worker.join();
    CHECK(attached);
    CHECK(detached);
    CHECK(affine);
  }
  {
    // The real Bait/Gorilla libgame pattern: exports but no JNI_OnLoad.
    write_library("libexports.so", {{"Java_ExportFixture_value", test::java_thunk(300, 3)},
        {"Java_ExportFixture_overloaded__I", test::java_thunk(301, 3)},
        {"Java_ExportFixture_overloaded__J", test::java_thunk(302, 3)}});
    auto classType = env->FindClass("java/lang/Class");
    REQUIRE(classType);
    auto getLoader = env->GetMethodID(classType, "getClassLoader", "()Ljava/lang/ClassLoader;");
    REQUIRE(getLoader);
    auto loader = env->CallObjectMethod(cls, getLoader);
    REQUIRE_FALSE(env->ExceptionCheck());
    auto initialized = env->GetStaticFieldID(cls, "exportsInitialized", "Z");
    REQUIRE(initialized);
    REQUIRE(load_guest_library("libexports.so", loader, &error));
    REQUIRE_FALSE(env->ExceptionCheck());
    CHECK_FALSE(env->GetStaticBooleanField(cls, initialized));  // binding did not initialize ExportFixture
    test::java_guest_call = [](GuestAddr fn, const GuestCallArgs& args) {
      return GuestCallResult{fn + args.integers[2]};
    };
    auto exported = env->FindClass("ExportFixture");
    REQUIRE(exported);
    auto mid = env->GetStaticMethodID(exported, "value", "(I)I");
    REQUIRE(mid);
    CHECK(env->CallStaticIntMethod(exported, mid, 12) == 312);
    REQUIRE_FALSE(env->ExceptionCheck());
    mid = env->GetStaticMethodID(exported, "overloaded", "(I)J");
    REQUIRE(mid);
    CHECK(env->CallStaticLongMethod(exported, mid, 7) == 308);
    REQUIRE_FALSE(env->ExceptionCheck());
    mid = env->GetStaticMethodID(exported, "overloaded", "(J)J");
    REQUIRE(mid);
    CHECK(env->CallStaticLongMethod(exported, mid, static_cast<jlong>(0x100000000)) == 0x10000012eLL);
    REQUIRE_FALSE(env->ExceptionCheck());
    CHECK(load_guest_library("libexports.so", loader, &error));
    CHECK_FALSE(load_guest_library("libexports.so", nullptr, &error));
    write_library("libonload.so", {{"JNI_OnLoad", test::java_thunk(500, 2)}});
    int loads = 0;
    test::java_guest_call = [&](GuestAddr fn, const GuestCallArgs& args) {
      CHECK(fn == 500);
      CHECK(args.integers[0] == guest_vm());
      CHECK(args.integers[1] == 0);
      ++loads;
      return GuestCallResult{JNI_VERSION_1_6};
    };
    CHECK(load_guest_library("libonload.so", loader, &error));
    CHECK(load_guest_library("libonload.so", loader, &error));
    CHECK(loads == 1);
    env->DeleteLocalRef(exported);
    env->DeleteLocalRef(loader);
    env->DeleteLocalRef(classType);
  }
  {
    // An unused JNI export owner may depend on an absent Android service class
    // (UE4's WebView callbacks). Library load and other exports must still work.
    auto fixture = env->FindClass("DependencyFixture");
    REQUIRE(fixture);
    auto create = env->GetStaticMethodID(fixture, "loader", "()Ljava/lang/ClassLoader;");
    REQUIRE(create);
    auto loader = env->CallStaticObjectMethod(fixture, create);
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(loader);
    write_library("liboptionalexports.so", {
        {"Java_DependencyFixture_00024Optional_value", test::java_thunk(600, 3)},
        {"Java_DependencyFixture_00024Ready_value", test::java_thunk(601, 3)}});
    REQUIRE_MESSAGE(load_guest_library("liboptionalexports.so", loader, &error), error);
    REQUIRE_FALSE(env->ExceptionCheck());
    test::java_guest_call = [](GuestAddr fn, const GuestCallArgs& args) {
      return GuestCallResult{fn + args.integers[2]};
    };
    auto ready = find_class(env, "DependencyFixture$Ready", loader);
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(ready);
    auto value = env->GetStaticMethodID(ready, "value", "(I)I");
    REQUIRE(value);
    CHECK(env->CallStaticIntMethod(ready, value, 7) == 608);
    REQUIRE_FALSE(env->ExceptionCheck());
    auto optional = find_class(env, "DependencyFixture$Optional", loader);
    CHECK_FALSE(optional);
    REQUIRE(env->ExceptionCheck());
    auto failure = env->ExceptionOccurred();
    env->ExceptionClear();
    auto missing = env->FindClass("java/lang/NoClassDefFoundError");
    REQUIRE(missing);
    CHECK(env->IsInstanceOf(failure, missing));
    env->DeleteLocalRef(failure);
    env->DeleteLocalRef(missing);
    auto enable = env->GetStaticMethodID(fixture, "enable", "(Ljava/lang/ClassLoader;)V");
    REQUIRE(enable);
    env->CallStaticVoidMethod(fixture, enable, loader);
    REQUIRE_FALSE(env->ExceptionCheck());
    optional = find_class(env, "DependencyFixture$Optional", loader);
    REQUIRE_FALSE(env->ExceptionCheck());
    REQUIRE(optional);
    value = env->GetStaticMethodID(optional, "value", "(I)I");
    REQUIRE(value);
    CHECK(env->CallStaticIntMethod(optional, value, 9) == 609);
    REQUIRE_FALSE(env->ExceptionCheck());
    write_library("libinvalidexports.so", {{"Java_DependencyFixture_00024Invalid_value", test::java_thunk(602, 3)}});
    CHECK_FALSE(load_guest_library("libinvalidexports.so", loader, &error));
    REQUIRE(env->ExceptionCheck());
    failure = env->ExceptionOccurred();
    env->ExceptionClear();
    auto invalid = env->FindClass("java/lang/VerifyError");
    REQUIRE(invalid);
    CHECK(env->IsInstanceOf(failure, invalid));
    env->DeleteLocalRef(invalid);
    env->DeleteLocalRef(failure);
    env->DeleteLocalRef(optional);
    env->DeleteLocalRef(ready);
    env->DeleteLocalRef(loader);
    env->DeleteLocalRef(fixture);
  }
  {
    auto* vm_words = g2h<GuestAddr>(*g2h<GuestAddr>(guest_vm()));
    auto vmInvoke = [&](unsigned slot, test::FakeCpu& cpu) {
      auto* code = g2h<const std::uint32_t>(vm_words[slot]);
      auto id = ((code[0] >> 5) & 0xffffu) | (((code[1] >> 5) & 0xffffu) << 16);
      const auto& entry = hle::function(id);
      cpu.xs[0] = guest_vm();
      hle::CallContext ctx(GuestThread::adopt_current(), cpu, entry);
      entry.handler(ctx);
    };
    GuestAddr output = 0;
    test::FakeCpu cpu;
    cpu.xs[1] = h2g(&output);
    cpu.xs[2] = JNI_VERSION_1_6;
    vmInvoke(6, cpu);
    CHECK(cpu.xs[0] == JNI_OK);
    CHECK(output == guest);
    cpu.xs[2] = JNI_VERSION_21;
    vmInvoke(6, cpu);
    CHECK(static_cast<jint>(cpu.xs[0]) == JNI_EVERSION);
    CHECK(output == 0);
    std::thread worker([&] {
      test::FakeCpu c;
      GuestAddr address = 42;
      c.xs[1] = h2g(&address);
      c.xs[2] = JNI_VERSION_1_6;
      vmInvoke(6, c);
      CHECK(static_cast<jint>(c.xs[0]) == JNI_EDETACHED);
      CHECK(address == 0);
      c.xs[2] = 0;
      vmInvoke(7, c);
      CHECK(c.xs[0] == JNI_OK);
      CHECK(address != 0);
      CHECK(address != guest);
      vmInvoke(5, c);
      CHECK(c.xs[0] == JNI_OK);
      CHECK(env_no_attach() == nullptr);
    });
    worker.join();
  }
  env->DeleteLocalRef(object);
  env->DeleteLocalRef(cls);
#ifdef VENTURE_TEST_JNI_INVOCATION_FIXTURE
  CHECK(test::run_jni_invocation_fixture() == 0);
  std::thread invocation_worker([] { CHECK(test::run_jni_invocation_fixture() == 0); });
  invocation_worker.join();
#endif
  REQUIRE_MESSAGE(stop_jvm(&error), error);
  vm_count = -1;
  CHECK(GuestThread::adopt_current().call(enumerate, {0, 0, h2g(&vm_count)}) == JNI_OK);
  CHECK(vm_count == 0);
}
