#include "common/process.h"
#include <windows.h>

#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <thread>
namespace venture {
namespace {
// CreateProcessW's Unicode environment block: the current variables with
// `overrides` applied, sorted by name case-insensitively as Windows requires.
std::wstring environment_block(const Environment &overrides) {
  auto name = [](std::wstring_view entry) {
    // Drive-current-directory entries ("=C:=C:\...") start with '='.
    return entry.substr(0, entry.find(L'=', 1));
  };
  auto same = [](std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), int(a.size()), b.data(), int(b.size()),
                                TRUE) == CSTR_EQUAL;
  };
  std::vector<std::wstring> entries;
  for (auto &[key, value] : overrides)
    entries.push_back(paths::from_utf8(key).wstring() + L"=" +
                      paths::from_utf8(value).wstring());
  const size_t added = entries.size();
  if (auto strings = GetEnvironmentStringsW()) {
    for (auto p = strings; *p; p += wcslen(p) + 1) {
      std::wstring_view entry(p);
      if (std::none_of(entries.begin(), entries.begin() + added,
                       [&](auto &o) { return same(name(o), name(entry)); }))
        entries.emplace_back(entry);
    }
    FreeEnvironmentStringsW(strings);
  }
  std::sort(entries.begin(), entries.end(), [&](auto &a, auto &b) {
    auto x = name(a), y = name(b);
    return CompareStringOrdinal(x.data(), int(x.size()), y.data(), int(y.size()),
                                TRUE) == CSTR_LESS_THAN;
  });
  std::wstring block;
  for (auto &e : entries) {
    block += e;
    block += L'\0';
  }
  block += L'\0';
  return block;
}
} // namespace
std::uint32_t current_process_id() { return GetCurrentProcessId(); }
struct Process::Impl {
  HANDLE handle = nullptr;
  std::optional<int> exit;
  std::shared_ptr<PipeChannel> channel;
  std::jthread broker;
  ~Impl() {
    if (broker.joinable()) {
      broker.request_stop();
      CancelSynchronousIo(reinterpret_cast<HANDLE>(broker.native_handle()));
      if (channel)
        channel->close();
      broker.join();
    }
    if (handle)
      CloseHandle(handle);
  }
};
Process::Process() : impl_(std::make_unique<Impl>()) {}
Process::~Process() = default;
Process::Process(Process &&) noexcept = default;
Process &Process::operator=(Process &&) noexcept = default;
void Process::spawn(const fs::path &exe, const std::vector<std::string> &args,
                    const fs::path &log, bool console) {
  spawn(exe, args, log, console, {});
}
void Process::spawn(const fs::path &exe, const std::vector<std::string> &args,
                    const fs::path &log, bool console,
                    PipeChannel::Handler broker,
                    const Environment &environment) {
  if (active())
    throw std::runtime_error("This game is already running.");
  if (!fs::is_regular_file(exe))
    throw std::runtime_error(
        "Runtime executable was not found. Set its path in Settings.");
  auto absolute = fs::absolute(exe);
  std::wstring cmd = quote_windows_arg(absolute.wstring());
  for (auto &a : args)
    cmd += L" " + quote_windows_arg(paths::from_utf8(a).wstring());
  std::array<std::shared_ptr<PipeChannel>, 2> channels{};
  if (broker) {
    channels = PipeChannel::pair();
    auto handles = channels[1]->handles();
    cmd += L" --credential-read " + std::to_wstring(handles[0]) +
           L" --credential-write " + std::to_wstring(handles[1]);
  }
  fs::create_directories(log.parent_path());
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE)
    throw std::runtime_error("Could not create the runtime log.");
  HANDLE input =
      CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = output;
  si.StartupInfo.hStdInput = input;
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<char> attributes(size);
  si.lpAttributeList =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
  bool initialized =
      InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size);
  std::vector<HANDLE> inherited{input, output};
  if (broker)
    for (auto h : channels[1]->handles())
      inherited.push_back(reinterpret_cast<HANDLE>(h));
  bool setup =
      initialized && UpdateProcThreadAttribute(
                         si.lpAttributeList, 0,
                         PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                         inherited.size() * sizeof(HANDLE), nullptr, nullptr);
  PROCESS_INFORMATION pi{};
  std::wstring block;
  if (!environment.empty())
    block = environment_block(environment);
  BOOL ok = setup &&
            CreateProcessW(
                absolute.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                EXTENDED_STARTUPINFO_PRESENT |
                    (console ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW) |
                    (block.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT),
                block.empty() ? nullptr : block.data(),
                absolute.parent_path().c_str(), &si.StartupInfo, &pi);
  if (initialized)
    DeleteProcThreadAttributeList(si.lpAttributeList);
  CloseHandle(output);
  if (input != INVALID_HANDLE_VALUE)
    CloseHandle(input);
  if (!ok)
    throw std::runtime_error("Windows could not start the runtime. Check the "
                             "executable path and permissions.");
  CloseHandle(pi.hThread);
  impl_ = std::make_unique<Impl>();
  impl_->handle = pi.hProcess;
  if (broker) {
    channels[1]->close();
    impl_->channel = channels[0];
    impl_->broker = std::jthread(
        [channel = channels[0], handler = std::move(broker)](
            std::stop_token stop) { channel->serve(handler, stop); });
  }
}
bool Process::active() const { return impl_->handle && !impl_->exit; }
bool Process::request_stop() {
  if (!active())
    return false;
  // The runtime's RuntimeStop event (runtime/platform/win32/runtime_stop.cpp).
  const auto name = "Local\\Venture.Stop." + std::to_string(GetProcessId(impl_->handle));
  HANDLE event = OpenEventA(EVENT_MODIFY_STATE, FALSE, name.c_str());
  if (!event)
    return false;
  const bool set = SetEvent(event) != 0;
  CloseHandle(event);
  return set;
}
std::optional<int> Process::poll() {
  if (active() && WaitForSingleObject(impl_->handle, 0) == WAIT_OBJECT_0) {
    DWORD code = 0;
    if (GetExitCodeProcess(impl_->handle, &code))
      impl_->exit = int(code);
  }
  return impl_->exit;
}
int run_tool(const fs::path &exe, const std::vector<std::string> &args,
             const std::function<void(std::string_view)> &output,
             std::string *error, std::stop_token stop) {
  auto absolute = fs::absolute(exe);
  std::wstring cmd = quote_windows_arg(absolute.wstring());
  for (auto &a : args)
    cmd += L" " + quote_windows_arg(paths::from_utf8(a).wstring());
  const auto failure = "Could not start " + paths::utf8(exe.filename()) + ".";
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE out_read = nullptr, out_write = nullptr, err_read = nullptr, err_write = nullptr;
  if (!CreatePipe(&out_read, &out_write, &sa, 0))
    throw std::runtime_error(failure);
  if (!CreatePipe(&err_read, &err_write, &sa, 0)) {
    CloseHandle(out_read);
    CloseHandle(out_write);
    throw std::runtime_error(failure);
  }
  // Only the child's ends are inherited.
  SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
  HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = input;
  si.StartupInfo.hStdOutput = out_write;
  si.StartupInfo.hStdError = err_write;
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<char> attributes(size);
  si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
  bool initialized = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size);
  HANDLE inherited[] = {input, out_write, err_write};
  bool setup = initialized && input != INVALID_HANDLE_VALUE &&
               UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                         sizeof(inherited), nullptr, nullptr);
  PROCESS_INFORMATION pi{};
  BOOL ok = setup && CreateProcessW(absolute.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                    EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                                    &si.StartupInfo, &pi);
  if (initialized)
    DeleteProcThreadAttributeList(si.lpAttributeList);
  CloseHandle(out_write);
  CloseHandle(err_write);
  if (input != INVALID_HANDLE_VALUE)
    CloseHandle(input);
  if (!ok) {
    CloseHandle(out_read);
    CloseHandle(err_read);
    throw std::runtime_error(failure);
  }
  CloseHandle(pi.hThread);
  // Both pipes drain on their own threads; `output` runs on the stdout one
  // while this thread waits. A daemon the tool started (adb's server) may
  // hold the pipes open, so once the tool exited and they are drained, the
  // pending reads are cancelled.
  std::string errors;
  std::exception_ptr failed;
  std::atomic<bool> out_done{false}, err_done{false};
  std::jthread out_reader([&] {
    std::vector<char> buffer(64 * 1024);
    DWORD n = 0;
    while (ReadFile(out_read, buffer.data(), DWORD(buffer.size()), &n, nullptr) && n) {
      if (failed || stop.stop_requested()) continue;
      try {
        output({buffer.data(), n});
      } catch (...) {
        failed = std::current_exception();
        TerminateProcess(pi.hProcess, 1);
      }
    }
    out_done = true;
  });
  std::jthread err_reader([&] {
    char buffer[4096];
    DWORD n = 0;
    while (ReadFile(err_read, buffer, sizeof buffer, &n, nullptr) && n)
      if (errors.size() < 64 * 1024) errors.append(buffer, n);
    err_done = true;
  });
  bool terminated = false;
  while (WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT)
    if (!terminated && stop.stop_requested()) {
      TerminateProcess(pi.hProcess, 1);
      terminated = true;
    }
  for (int idle = 0; !(out_done && err_done) && idle < 3;) {
    DWORD out_avail = 0, err_avail = 0;
    PeekNamedPipe(out_read, nullptr, 0, nullptr, &out_avail, nullptr);
    PeekNamedPipe(err_read, nullptr, 0, nullptr, &err_avail, nullptr);
    idle = out_avail || err_avail ? 0 : idle + 1;
    Sleep(30);
  }
  // A reader between two reads misses a cancel, so cancel until it ends.
  while (!out_done || !err_done) {
    if (!out_done) CancelSynchronousIo(reinterpret_cast<HANDLE>(out_reader.native_handle()));
    if (!err_done) CancelSynchronousIo(reinterpret_cast<HANDLE>(err_reader.native_handle()));
    Sleep(10);
  }
  out_reader.join();
  err_reader.join();
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(out_read);
  CloseHandle(err_read);
  if (failed)
    std::rethrow_exception(failed);
  if (error)
    *error = std::move(errors);
  return int(code);
}
void open_folder(const fs::path &p) {
  if (!fs::is_directory(p) ||
      reinterpret_cast<INT_PTR>(ShellExecuteW(
          nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
    throw std::runtime_error("Could not open the game folder.");
}
void reveal_file(const fs::path &file) {
  const std::wstring args = L"/select,\"" + file.wstring() + L"\"";
  if (!fs::is_regular_file(file) ||
      reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                                              SW_SHOWNORMAL)) <= 32)
    throw std::runtime_error("Could not show the file.");
}
void start_detached(const fs::path &executable, const std::vector<std::string> &args) {
  std::wstring parameters;
  for (const auto &a : args) {
    if (!parameters.empty()) parameters += L' ';
    parameters += quote_windows_arg(paths::from_utf8(a).wstring());
  }
  if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", executable.c_str(),
                                              parameters.empty() ? nullptr : parameters.c_str(),
                                              executable.parent_path().c_str(), SW_SHOWNORMAL)) <= 32)
    throw std::runtime_error("Could not start " + paths::utf8(executable.filename()) + ".");
}
void open_url(const std::string &url) {
  if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", paths::from_utf8(url).c_str(), nullptr, nullptr,
                                              SW_SHOWNORMAL)) <= 32)
    throw std::runtime_error("Could not open the link.");
}
} // namespace venture
