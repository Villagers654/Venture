#include "java/jni32.h"
#include "java/internal.h"
#include "core/guest_storage.h"
#include "core/address_space.h"
#include <map>
#include <limits>

namespace venture::java::jni32 {
namespace {
struct Reference {
  jobject value;
  jobjectRefType kind;
  ~Reference() {
    // Locals are owned by the corresponding host JNI frame (or detach).
    // JVM weak globals, not C++ weak_ptrs, determine Java reachability.
    if (kind == JNILocalRefType) return;
    if (auto* env = env_no_attach()) {
      if (kind == JNIWeakGlobalRefType)
        env->DeleteWeakGlobalRef(value);
      else
        env->DeleteGlobalRef(value);
    }
  }
};
struct Method {
  jmethodID value;
  jclass owner = nullptr;
};
struct Field {
  jfieldID value;
  jclass owner = nullptr;
};
Token32Registry& registry() {
  static auto* r = new Token32Registry;
  return *r;
}
struct Copy {
  GuestStorage memory;
  jobject owner = nullptr;  // global, keeps array/string alive until release
  unsigned family = 0;
  jsize length = 0;
  ~Copy() {
    if (auto* env = env_no_attach(); env && owner) env->DeleteGlobalRef(owner);
  }
};
struct Locals {
  std::vector<std::unique_ptr<Token32Registry::Scope>> scopes;
  std::vector<std::size_t> native_floors;
  std::vector<std::vector<std::shared_ptr<Reference>>> pins;
};
struct Copies {
  std::mutex mutex;
  std::map<GuestAddr, std::unique_ptr<Copy>> values;
};
Copies& copies() {
  // JNI array/string element pointers remain valid until Release*, including
  // across native returns and releases by another attached thread.
  static auto* state = new Copies;
  return *state;
}
Locals& local_state() {
  auto& state = thread_state().a32_state;
  if (!state) state = std::make_shared<Locals>();
  return *static_cast<Locals*>(state.get());
}
std::mutex ids_mutex;
std::map<jmethodID, std::uint32_t> methods;
std::map<jfieldID, std::uint32_t> fields;
Reference& resolve(std::uint32_t token, std::shared_ptr<Reference>& pin) {
  pin = registry().resolve(Token32<Reference>{token});
  if (!pin) throw std::invalid_argument("stale, foreign-thread or invalid JNI reference token");
  return *pin;
}
template <class Id, class Record>
std::uint32_t id_token(Id value, std::map<Id, std::uint32_t>& ids) {
  if (!value) return 0;
  std::lock_guard lock(ids_mutex);
  if (auto it = ids.find(value); it != ids.end()) return it->second;
  auto token = registry().insert(std::make_shared<Record>(Record{value})).value;
  ids.emplace(value, token);
  return token;
}
template <class Record>
auto id_value(std::uint32_t value) {
  auto ptr = registry().resolve(Token32<Record>{value});
  if (!ptr) throw std::invalid_argument("invalid JNI ID token");
  return ptr->value;
}
void write_copy_flag(GuestAddr out) {
  if (out) {
    jboolean yes = JNI_TRUE;
    hle::a32::write_bytes(out, &yes, 1);
  }
}
// JNI Functions, array elements: mode 0 copies and frees, COMMIT copies and
// retains, ABORT frees without copying. Region APIs avoid holding a JVM pin
// while arbitrary guest code runs (also for GetPrimitiveArrayCritical).
constexpr unsigned widths[] = {1, 1, 2, 2, 4, 8, 4, 8};
void array_region(JNIEnv* e, unsigned family, jobject a, jsize n, void* p, bool put) {
#define REGION(N, C, T)                                                           \
  case N:                                                                         \
    if (put)                                                                      \
      e->Set##C##ArrayRegion(static_cast<T##Array>(a), 0, n, static_cast<T*>(p)); \
    else                                                                          \
      e->Get##C##ArrayRegion(static_cast<T##Array>(a), 0, n, static_cast<T*>(p)); \
    break
  switch (family) {
    REGION(0, Boolean, jboolean);
    REGION(1, Byte, jbyte);
    REGION(2, Char, jchar);
    REGION(3, Short, jshort);
    REGION(4, Int, jint);
    REGION(5, Long, jlong);
    REGION(6, Float, jfloat);
    REGION(7, Double, jdouble);
    default:
      throw std::invalid_argument("invalid primitive array kind");
  }
#undef REGION
}
unsigned array_family(JNIEnv* e, jobject a) {
  constexpr const char* names[] = {"[Z", "[B", "[C", "[S", "[I", "[J", "[F", "[D"};
  for (unsigned i = 0; i < 8; ++i) {
    auto cls = e->FindClass(names[i]);
    if (!cls) throw std::runtime_error("cannot resolve primitive array class");
    bool match = e->IsInstanceOf(a, cls);
    e->DeleteLocalRef(cls);
    if (match) return i;
  }
  throw std::invalid_argument("expected primitive array");
}
}  // namespace
void push_scope() { local_state().scopes.push_back(std::make_unique<Token32Registry::Scope>(registry())); }
PinScope::PinScope() { local_state().pins.emplace_back(); state_ = thread_state().a32_state; }
PinScope::~PinScope() {
  if (auto state = state_.lock()) static_cast<Locals*>(state.get())->pins.pop_back();
}
void pop_scope() {
  auto& locals = local_state();
  auto floor = locals.native_floors.empty() ? 1 : locals.native_floors.back();
  if (locals.scopes.size() <= floor) throw std::invalid_argument("JNI PopLocalFrame without PushLocalFrame");
  locals.scopes.pop_back();
}
NativeScope::NativeScope() : depth_(local_state().scopes.size()) {
  push_scope();
  local_state().native_floors.push_back(depth_ + 1);
  state_ = thread_state().a32_state;
}
NativeScope::~NativeScope() { pop(nullptr); }
jobject NativeScope::pop(jobject result) {
  if (!active_) return result;
  active_ = false;
  auto state = state_.lock();
  if (!state) return result;
  auto& locals = *static_cast<Locals*>(state.get());
  // Guest-pushed frames left open by a native return must also be unwound.
  while (locals.scopes.size() > depth_ + 1) {
    result = env_no_attach()->PopLocalFrame(result);
    locals.scopes.pop_back();
  }
  if (locals.scopes.size() > depth_) locals.scopes.resize(depth_);
  locals.native_floors.pop_back();
  return result;
}
void clear_thread() { thread_state().a32_state.reset(); }
std::uint32_t reference(jobject value, jobjectRefType kind) {
  auto& locals = local_state();
  if (!value) return 0;
  if (kind == JNILocalRefType && locals.scopes.empty()) push_scope();
  auto ref = std::make_shared<Reference>();
  ref->value = value;
  ref->kind = kind;
  return registry()
      .insert(std::move(ref), kind == JNILocalRefType ? TokenLifetime::local : TokenLifetime::global,
              kind == JNILocalRefType ? locals.scopes.back().get() : nullptr)
      .value;
}
jobject object(std::uint32_t token) {
  if (!token) return nullptr;
  std::shared_ptr<Reference> pin;
  auto value = resolve(token, pin).value;
  auto& pins = local_state().pins;
  if (!pins.empty()) pins.back().push_back(std::move(pin));
  return value;
}
void delete_reference(std::uint32_t token, jobjectRefType kind) {
  if (!token) return;
  std::shared_ptr<Reference> pin;
  auto& ref = resolve(token, pin);
  if (ref.kind != kind) throw std::invalid_argument("JNI reference deletion kind mismatch");
  if (kind == JNILocalRefType) env_no_attach()->DeleteLocalRef(ref.value);
  registry().erase(Token32<Reference>{token});
}
std::uint32_t method(jmethodID value) { return id_token<jmethodID, Method>(value, methods); }
jmethodID method(std::uint32_t value) { return id_value<Method>(value); }
std::uint32_t field(jfieldID value) { return id_token<jfieldID, Field>(value, fields); }
jfieldID field(std::uint32_t value) { return id_value<Field>(value); }
void retain_id_owner(std::uint32_t token, bool is_method, jclass cls) {
  if (!token || !cls) return;
  // JNI IDs remain valid while their declaring class is live. Keep a class
  // global with our stable token so a later host ID reuse cannot alias it.
  auto* env = env_no_attach();
  auto owner = static_cast<jclass>(env->NewGlobalRef(cls));
  if (!owner) return;
  bool keep = false;
  {
    std::lock_guard lock(ids_mutex);
    if (is_method) {
      auto id = registry().resolve(Token32<Method>{token});
      if (id && !id->owner) {
        id->owner = owner;
        keep = true;
      }
    } else {
      auto id = registry().resolve(Token32<Field>{token});
      if (id && !id->owner) {
        id->owner = owner;
        keep = true;
      }
    }
  }
  if (!keep) env->DeleteGlobalRef(owner);
}

bool special(JNIEnv* env, hle::CallContext& c) {
  auto& locals = local_state();
  const auto name = c.info().name;
  if (name == "PushLocalFrame") {
    // An attached thread can push a frame before creating its first reference.
    if (locals.scopes.empty()) push_scope();
    auto status = env->PushLocalFrame(static_cast<jint>(c.x(1)));
    if (status == JNI_OK) {
      try {
        push_scope();
      } catch (...) {
        env->PopLocalFrame(nullptr);
        throw;
      }
    }
    c.return_x(static_cast<std::uint32_t>(status));
    return true;
  }
  if (name == "PopLocalFrame") {
    auto floor = locals.native_floors.empty() ? 1 : locals.native_floors.back();
    if (locals.scopes.size() <= floor)
      throw std::invalid_argument("JNI PopLocalFrame without PushLocalFrame");
    auto result = env->PopLocalFrame(object(c.x(1)));
    pop_scope();
    c.return_x(reference(result));
    return true;
  }
  if (name == "NewGlobalRef" || name == "NewLocalRef" || name == "NewWeakGlobalRef") {
    auto obj = object(c.x(1));
    auto kind = name == "NewGlobalRef"       ? JNIGlobalRefType
                : name == "NewWeakGlobalRef" ? JNIWeakGlobalRefType
                                             : JNILocalRefType;
    auto ref = kind == JNIGlobalRefType       ? env->NewGlobalRef(obj)
               : kind == JNIWeakGlobalRefType ? env->NewWeakGlobalRef(obj)
                                              : env->NewLocalRef(obj);
    c.return_x(reference(ref, kind));
    return true;
  }
  if (name == "DeleteGlobalRef" || name == "DeleteLocalRef" || name == "DeleteWeakGlobalRef") {
    delete_reference(c.x(1), name == "DeleteGlobalRef"       ? JNIGlobalRefType
                             : name == "DeleteWeakGlobalRef" ? JNIWeakGlobalRefType
                                                             : JNILocalRefType);
    return true;
  }
  if (name == "GetObjectRefType") {
    auto ref = registry().resolve(Token32<Reference>{static_cast<std::uint32_t>(c.x(1))});
    c.return_x(ref ? ref->kind : JNIInvalidRefType);
    return true;
  }
  const bool utf = name == "GetStringUTFChars",
             chars = name == "GetStringChars" || name == "GetStringCritical";
  unsigned family = 0;
  const bool critical = name == "GetPrimitiveArrayCritical";
  const bool elements = name.starts_with("Get") && name.ends_with("ArrayElements");
  if (utf || chars || critical || elements) {
    auto obj = object(c.x(1));
    auto copy = std::make_unique<Copy>();
    if (utf || chars) {
      auto str = static_cast<jstring>(obj);
      copy->family = utf ? 8 : 9;
      copy->length = utf ? env->GetStringUTFLength(str) : env->GetStringLength(str);
      if (env->ExceptionCheck()) {
        c.return_x(0);
        return true;
      }
      copy->memory.resize((std::uint64_t(copy->length) + 1) * (utf ? 1 : 2));
      if (utf)
        env->GetStringUTFRegion(str, 0, env->GetStringLength(str), g2h<char>(copy->memory.address()));
      else
        env->GetStringRegion(str, 0, copy->length, g2h<jchar>(copy->memory.address()));
      std::memset(g2h(copy->memory.address() + std::uint64_t(copy->length) * (utf ? 1 : 2)), 0, utf ? 1 : 2);
    } else {
      if (critical)
        family = array_family(env, obj);
      else {
        constexpr const char* names[] = {"Boolean", "Byte", "Char",  "Short",
                                         "Int",     "Long", "Float", "Double"};
        while (family < 8 && name != "Get" + std::string(names[family]) + "ArrayElements") ++family;
      }
      copy->family = family;
      copy->length = env->GetArrayLength(static_cast<jarray>(obj));
      if (env->ExceptionCheck()) {
        c.return_x(0);
        return true;
      }
      copy->memory.resize(std::max<std::uint64_t>(1, std::uint64_t(copy->length) * widths[family]));
      array_region(env, family, obj, copy->length, g2h(copy->memory.address()), false);
    }
    if (env->ExceptionCheck()) {
      c.return_x(0);
      return true;
    }
    copy->owner = env->NewGlobalRef(obj);
    if (!copy->owner) {
      c.return_x(0);
      return true;
    }
    auto address = copy->memory.address();
    write_copy_flag(c.x(2));
    {
      auto& state = copies();
      std::lock_guard lock(state.mutex);
      state.values.emplace(address, std::move(copy));
    }
    c.return_x(address);
    return true;
  }
  if (name.starts_with("Release") &&
      (name.ends_with("ArrayElements") || name == "ReleasePrimitiveArrayCritical" ||
       name == "ReleaseStringChars" || name == "ReleaseStringUTFChars" || name == "ReleaseStringCritical")) {
    // JNI permits Release* with a pending exception. Our copied-array path
    // uses region APIs internally, so preserve the pending exception while
    // running those host JNI operations.
    auto pending = env->ExceptionOccurred();
    if (pending) env->ExceptionClear();
    struct RestoreException {
      JNIEnv* env;
      jthrowable pending;
      ~RestoreException() {
        if (pending) {
          env->ExceptionClear();
          env->Throw(pending);
          env->DeleteLocalRef(pending);
        }
      }
    } restore{env, pending};
    // ART's ReleaseStringUTFChars ignores jstring and deletes only the copy
    // (android-12.1.0_r1 runtime/jni/jni_internal.cc). In particular, never
    // resolve a guest local reference here: it may already have been deleted.
    const bool release_utf = name == "ReleaseStringUTFChars";
    if (release_utf && !c.x(2)) return true;  // delete[] nullptr is a no-op
    auto& state = copies();
    std::lock_guard lock(state.mutex);
    auto it = state.values.find(c.x(2));
    if (it == state.values.end())
      throw std::invalid_argument("unknown JNI release buffer");
    if (!release_utf && !env->IsSameObject(object(c.x(1)), it->second->owner))
      throw std::invalid_argument("JNI release buffer/owner mismatch");
    auto& copy = *it->second;
    if (copy.family >= 8) {
      if ((copy.family == 8) != (name == "ReleaseStringUTFChars") || name.ends_with("ArrayElements") ||
          name == "ReleasePrimitiveArrayCritical")
        throw std::invalid_argument("JNI release family mismatch");
      state.values.erase(it);
      return true;
    }
    constexpr const char* release_names[] = {"ReleaseBooleanArrayElements", "ReleaseByteArrayElements",
                                             "ReleaseCharArrayElements",    "ReleaseShortArrayElements",
                                             "ReleaseIntArrayElements",     "ReleaseLongArrayElements",
                                             "ReleaseFloatArrayElements",   "ReleaseDoubleArrayElements"};
    if (name != "ReleasePrimitiveArrayCritical" && name != release_names[copy.family])
      throw std::invalid_argument("JNI release family mismatch");
    auto mode = static_cast<jint>(c.x(3));
    if (mode != 0 && mode != JNI_COMMIT && mode != JNI_ABORT)
      throw std::invalid_argument("invalid JNI array release mode");
    if (mode != JNI_ABORT)
      array_region(env, copy.family, copy.owner, copy.length, g2h(copy.memory.address()), true);
    if (mode != JNI_COMMIT) state.values.erase(it);
    return true;
  }
  if (name == "GetDirectBufferAddress") {
    auto obj = object(c.x(1));
    auto* ptr = env->GetDirectBufferAddress(obj);
    if (ptr && (h2g(ptr) > UINT32_MAX ||
                !AddressSpace::get().is_guest_accessible(
                    h2g(ptr), std::max<jlong>(1, env->GetDirectBufferCapacity(obj)), true))) {
      throw_java(env, "java/lang/UnsupportedOperationException",
                 "direct buffer storage is outside the A32 guest arena");
      ptr = nullptr;
    }
    c.return_x(h2g(ptr));
    return true;
  }
  if (name == "NewDirectByteBuffer") {
    hle::a32::ArgCursor32 args(c.cpu());
    args.word();
    auto address = args.word();
    auto size = static_cast<jlong>(args.pair());
    if (size < 0 || (size && !AddressSpace::get().is_guest_accessible(address, size, true)))
      throw std::invalid_argument("direct buffer storage is not guest-accessible");
    c.return_x(reference(env->NewDirectByteBuffer(g2h(address), size)));
    return true;
  }
  return false;
}

hle::a32::CallBuilder32 native_arguments(const Descriptor& d, const HostArguments& host, HostAbi abi,
                                         GuestAddr env) {
  hle::a32::CallBuilder32 out;
  out.pointer(env).value(reference(g2h<_jobject>(host.gp[1])));
  unsigned gp = 2, simd = 0, position = 2, stack = 0;
  for (auto type : d.parameters) {
    bool fp = type == Type::Float || type == Type::Double;
    std::uint64_t bits;
    if (abi == HostAbi::Win64) {
      bits = position < 4 ? (fp ? host.fp[position] : host.gp[position]) : host.stack[position - 4];
      ++position;
    } else if (fp)
      bits = simd < 8 ? host.fp[simd++] : host.stack[stack++];
    else
      bits = gp < 6 ? host.gp[gp++] : host.stack[stack++];
    using K = hle::a32::Kind;
    K kind = K::U32;
    switch (type) {
      case Type::Object:
        bits = reference(g2h<_jobject>(bits));
        break;
      case Type::Boolean:
        kind = K::U8;
        break;
      case Type::Byte:
        kind = K::I8;
        break;
      case Type::Char:
        kind = K::U16;
        break;
      case Type::Short:
        kind = K::I16;
        break;
      case Type::Int:
        kind = K::I32;
        break;
      case Type::Long:
        kind = K::I64;
        break;
      case Type::Float:
        kind = K::F32;
        bits &= UINT32_MAX;
        break;
      case Type::Double:
        kind = K::F64;
        break;
      default:
        throw std::invalid_argument("void JNI native parameter");
    }
    out.add({kind}, bits);
  }
  return out;
}
}  // namespace venture::java::jni32
