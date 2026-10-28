// Windows java.exe uses ANSI for both its installation and argument paths.
// Use the standard JNI Invocation API in a UTF-8-manifested process instead.
#include <jni.h>
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include "common/paths.h"
#include "platform/java_host.h"

namespace {
std::string classpath(const std::string& text) {
  // Wildcards are a java launcher feature, not a JNI VM option feature.
  std::string result;
  bool first = true;
  auto add = [&](const std::string& entry) {
    if (!first) result += ';';
    first = false;
    result += entry;
  };
  for (std::size_t start = 0; start <= text.size();) {
    auto end = text.find(';', start);
    if (end == text.npos) end = text.size();
    auto entry = text.substr(start, end - start);
    auto path = venture::paths::from_utf8(entry);
    if (path.filename() == "*") {
      std::vector<std::filesystem::path> jars;
      for (const auto& file : std::filesystem::directory_iterator(path.parent_path()))
        if (file.is_regular_file() && (file.path().extension() == ".jar" || file.path().extension() == ".JAR"))
          jars.push_back(file.path());
      std::sort(jars.begin(), jars.end());
      for (const auto& jar : jars) add(venture::paths::utf8(jar));
    } else add(entry);
    if (end == text.size()) break;
    start = end + 1;
  }
  return result;
}
}

int main() {
  try {
    int count = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv) throw std::runtime_error("cannot read JVM request argument");
    std::filesystem::path request = count == 2 ? argv[1] : L"";
    LocalFree(argv);
    if (request.empty()) return 64;
    nlohmann::json data;
    if (request == L"--stdin") std::cin >> data;
    else {
      std::ifstream input(request);
      input >> data;
    }
    auto command = data.at("command").get<std::vector<std::string>>();
    if (command.size() < 2) throw std::runtime_error("empty JVM request");
    auto home = venture::paths::from_utf8(command[0]).parent_path().parent_path();
    std::vector<std::string> options;
    std::size_t at = 1;
    for (; at < command.size() && command[at].starts_with('-'); ++at) {
      if (command[at] == "-cp") {
        if (++at == command.size()) throw std::runtime_error("missing JVM classpath");
        options.push_back("-Djava.class.path=" + classpath(command[at]));
      } else options.push_back(command[at]);
    }
    if (at == command.size()) throw std::runtime_error("missing JVM main class");
    options.push_back("-Dventure.java.launcher=" + venture::paths::utf8(venture::paths::executable()));
    std::vector<JavaVMOption> native_options;
    for (auto& option : options) native_options.push_back({option.data(), nullptr});
    JavaVMInitArgs init{JNI_VERSION_1_8, static_cast<jint>(native_options.size()), native_options.data(), JNI_FALSE};
    auto create = reinterpret_cast<jint(JNICALL*)(JavaVM**, void**, void*)>(venture::java::platform::jvm_entry(home));
    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    if (create(&vm, reinterpret_cast<void**>(&env), &init) != JNI_OK)
      throw std::runtime_error("cannot start converter JVM");
    auto main_class = command[at++];
    std::replace(main_class.begin(), main_class.end(), '.', '/');
    auto cls = env->FindClass(main_class.c_str());
    auto method = cls ? env->GetStaticMethodID(cls, "main", "([Ljava/lang/String;)V") : nullptr;
    auto strings = env->FindClass("java/lang/String");
    auto arguments = strings ? env->NewObjectArray(static_cast<jsize>(command.size() - at), strings, nullptr) : nullptr;
    for (std::size_t i = at; arguments && i < command.size() && !env->ExceptionCheck(); ++i) {
      // NewString takes UTF-16, not JNI's modified UTF-8. Preserve supplementary
      // characters and embedded separators in classpaths/arguments too.
      auto wide = venture::paths::from_utf8(command[i]).native();
      auto text = env->NewString(reinterpret_cast<const jchar*>(wide.data()), static_cast<jsize>(wide.size()));
      env->SetObjectArrayElement(arguments, static_cast<jsize>(i - at), text);
      env->DeleteLocalRef(text);
    }
    if (method && arguments && !env->ExceptionCheck()) env->CallStaticVoidMethod(cls, method, arguments);
    const bool failed = env->ExceptionCheck() || !method || !arguments;
    if (env->ExceptionCheck()) env->ExceptionDescribe();
    vm->DestroyJavaVM();
    return failed ? 1 : 0;
  } catch (const std::exception& e) {
    std::cerr << "Converter JVM: " << e.what() << '\n';
    return 1;
  }
}
