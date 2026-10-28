#pragma once
#include "java/abi.h"
#include <memory>
#include "core/token32.h"

namespace venture::java::jni32 {
// Android libnativehelper/include_jni/jni.h: jobject/IDs are pointer words;
// jvalue is an 8-byte union on both Android ABIs. Tokens never alias JVM memory.
std::uint32_t reference(jobject, jobjectRefType = JNILocalRefType);
jobject object(std::uint32_t);
void delete_reference(std::uint32_t, jobjectRefType);
std::uint32_t method(jmethodID);
jmethodID method(std::uint32_t);
std::uint32_t field(jfieldID);
jfieldID field(std::uint32_t);
void retain_id_owner(std::uint32_t token, bool is_method, jclass owner);
void clear_thread();  // before host detach, while its JNIEnv is still usable
void push_scope();
void pop_scope();
class PinScope {
  // Detach destroys this state; an older guard must not retain it or clean a replacement.
  std::weak_ptr<void> state_;
 public:
  PinScope();
  ~PinScope();
  PinScope(const PinScope&) = delete;
};
class NativeScope {
  std::weak_ptr<void> state_;
  std::size_t depth_;
  bool active_ = true;

 public:
  NativeScope();
  ~NativeScope();
  NativeScope(const NativeScope&) = delete;
  jobject pop(jobject result);
};
bool special(JNIEnv*, hle::CallContext&);
hle::a32::CallBuilder32 native_arguments(const Descriptor&, const HostArguments&, HostAbi, GuestAddr);

template <class T>
constexpr bool is_object = std::is_pointer_v<T> && std::is_convertible_v<T, jobject>;
template <class T>
T decode(std::uint64_t bits) {
  if constexpr (is_object<T>)
    return static_cast<T>(object(static_cast<std::uint32_t>(bits)));
  else if constexpr (std::is_same_v<T, jmethodID>)
    return method(static_cast<std::uint32_t>(bits));
  else if constexpr (std::is_same_v<T, jfieldID>)
    return field(static_cast<std::uint32_t>(bits));
  else if constexpr (std::is_pointer_v<T>)
    return g2h<std::remove_pointer_t<T>>(bits);
  else if constexpr (std::is_same_v<T, float>)
    return std::bit_cast<float>(static_cast<std::uint32_t>(bits));
  else if constexpr (std::is_same_v<T, double>)
    return std::bit_cast<double>(bits);
  else
    return static_cast<T>(bits);
}
template <class T>
std::uint64_t encode(T value) {
  if constexpr (is_object<T>)
    return reference(value);
  else if constexpr (std::is_same_v<T, jmethodID>)
    return method(value);
  else if constexpr (std::is_same_v<T, jfieldID>)
    return field(value);
  else if constexpr (std::is_pointer_v<T>)
    return h2g32(value);
  else if constexpr (std::is_same_v<T, float>)
    return std::bit_cast<std::uint32_t>(value);
  else if constexpr (std::is_same_v<T, double>)
    return std::bit_cast<std::uint64_t>(value);
  else
    return static_cast<std::uint64_t>(value);
}
}  // namespace venture::java::jni32
