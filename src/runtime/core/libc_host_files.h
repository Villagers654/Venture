// VFS/descriptor bridge needed by native libraries that require host CRT fds.
// Implemented by Part B alongside its descriptor table. No guest fd may be
// passed directly to a host CRT function.
#pragma once
namespace venture {
// Open a guest path via the VFS. Flags and mode are Linux values. Returns a new
// owned host CRT fd, or -1 with the calling guest thread's Linux errno set.
int open_host_fd_for_guest(const char* path, int flags, int mode);
// Duplicate the open description of a guest fd into an owned host CRT fd.
int duplicate_host_fd_for_guest(int guest_fd);
// Close/remove the original guest descriptor (used when gzdopen is closed).
int close_guest_fd_for_native_library(int guest_fd);
}  // namespace venture
