#include "core/address_space.h"
#include "core/elf.h"
#include "core/guest_storage.h"
#include "core/linker.h"
#include "core/process_runtime.h"
#include "core/thread_runtime.h"
#include "hle/registry.h"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <array>
#include <cstring>

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  try {
    using namespace venture;
    select_process_isa(elf::detect_isa(argv[1]));
    AddressSpace::get().initialize(process_config().isa());
    process_config().native_lib_dir = std::filesystem::absolute(argv[1]).parent_path();
    register_all_hle_libraries();
    Linker::get().set_search_paths({process_config().native_lib_dir.string()});
    GuestThread thread(1); thread.attach_to_current_host_thread();
    auto* library = Linker::get().open(std::filesystem::absolute(argv[1]).string(), kRtldNow | kRtldGlobal, nullptr);
    if (!library) { std::cerr << Linker::get().take_error() << '\n'; return 3; }
    auto fn = library->find_symbol("signal_unwind_fixture");
    GuestStorage output; auto address = output.resize(32);
    std::memset(g2h<void>(address), 0, 32);
    auto status = thread.call(fn, {std::stoul(argv[2]), address});
    shutdown_guest_threads();
    auto* words = g2h<std::uint32_t>(address);
    std::cout << "RESULT " << status;
    for (int i = 0; i < 8; ++i) std::cout << ' ' << words[i];
    std::cout << '\n';
    return static_cast<int>(status);
  } catch (const venture::ProcessExit& exit) { return exit.status; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 4; }
}
