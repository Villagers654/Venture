#pragma once
#include <mutex>
namespace venture {
// A worker restart owns its exit decision from intent claim through pipe
// publication. Recursive entry lets the claiming JNI caller commit that
// decision through the ordinary native exit/restart paths on the same thread.
inline std::recursive_mutex worker_exit_mutex;
// Flush only host-owned evidence, without joining guest/JVM/device teardown.
// The supervisor bounds and identifies each stage.
void flush_guest_process_files(int status);
// Install once before guest threads start; preserves the core/audio layer boundary.
void set_guest_exit_capture_flush(void (*flush)());
}
