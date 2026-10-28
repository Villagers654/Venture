/*
 * Copyright (C) 2023 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * This header is used to define error constants and names;
 * it might be included several times.
 */

// Messages from bionic/libc/private/bionic_errdefs.h; numeric values are Linux asm-generic.
#include <algorithm>
#include <cstring>
#include <string>

#include "hle/libc/libc_a.h"
#include "core/hle_runtime.h"
#include "platform/memory.h"
namespace venture::hle::libc {
namespace {
const char* known_error(int e) {
  switch (e) {
    case 0:
      return "Success";
    case 1:
      return "Operation not permitted";
    case 2:
      return "No such file or directory";
    case 3:
      return "No such process";
    case 4:
      return "Interrupted system call";
    case 5:
      return "I/O error";
    case 6:
      return "No such device or address";
    case 7:
      return "Argument list too long";
    case 8:
      return "Exec format error";
    case 9:
      return "Bad file descriptor";
    case 10:
      return "No child processes";
    case 11:
      return "Try again";
    case 12:
      return "Out of memory";
    case 13:
      return "Permission denied";
    case 14:
      return "Bad address";
    case 15:
      return "Block device required";
    case 16:
      return "Device or resource busy";
    case 17:
      return "File exists";
    case 18:
      return "Cross-device link";
    case 19:
      return "No such device";
    case 20:
      return "Not a directory";
    case 21:
      return "Is a directory";
    case 22:
      return "Invalid argument";
    case 23:
      return "File table overflow";
    case 24:
      return "Too many open files";
    case 25:
      return "Inappropriate ioctl for device";
    case 26:
      return "Text file busy";
    case 27:
      return "File too large";
    case 28:
      return "No space left on device";
    case 29:
      return "Illegal seek";
    case 30:
      return "Read-only file system";
    case 31:
      return "Too many links";
    case 32:
      return "Broken pipe";
    case 33:
      return "Math argument out of domain of func";
    case 34:
      return "Math result not representable";
    case 35:
      return "Resource deadlock would occur";
    case 36:
      return "File name too long";
    case 37:
      return "No record locks available";
    case 38:
      return "Function not implemented";
    case 39:
      return "Directory not empty";
    case 40:
      return "Too many symbolic links encountered";
    case 42:
      return "No message of desired type";
    case 43:
      return "Identifier removed";
    case 44:
      return "Channel number out of range";
    case 45:
      return "Level 2 not synchronized";
    case 46:
      return "Level 3 halted";
    case 47:
      return "Level 3 reset";
    case 48:
      return "Link number out of range";
    case 49:
      return "Protocol driver not attached";
    case 50:
      return "No CSI structure available";
    case 51:
      return "Level 2 halted";
    case 52:
      return "Invalid exchange";
    case 53:
      return "Invalid request descriptor";
    case 54:
      return "Exchange full";
    case 55:
      return "No anode";
    case 56:
      return "Invalid request code";
    case 57:
      return "Invalid slot";
    case 59:
      return "Bad font file format";
    case 60:
      return "Device not a stream";
    case 61:
      return "No data available";
    case 62:
      return "Timer expired";
    case 63:
      return "Out of streams resources";
    case 64:
      return "Machine is not on the network";
    case 65:
      return "Package not installed";
    case 66:
      return "Object is remote";
    case 67:
      return "Link has been severed";
    case 68:
      return "Advertise error";
    case 69:
      return "Srmount error";
    case 70:
      return "Communication error on send";
    case 71:
      return "Protocol error";
    case 72:
      return "Multihop attempted";
    case 73:
      return "RFS specific error";
    case 74:
      return "Not a data message";
    case 75:
      return "Value too large for defined data type";
    case 76:
      return "Name not unique on network";
    case 77:
      return "File descriptor in bad state";
    case 78:
      return "Remote address changed";
    case 79:
      return "Can not access a needed shared library";
    case 80:
      return "Accessing a corrupted shared library";
    case 81:
      return ".lib section in a.out corrupted";
    case 82:
      return "Attempting to link in too many shared libraries";
    case 83:
      return "Cannot exec a shared library directly";
    case 84:
      return "Illegal byte sequence";
    case 85:
      return "Interrupted system call should be restarted";
    case 86:
      return "Streams pipe error";
    case 87:
      return "Too many users";
    case 88:
      return "Socket operation on non-socket";
    case 89:
      return "Destination address required";
    case 90:
      return "Message too long";
    case 91:
      return "Protocol wrong type for socket";
    case 92:
      return "Protocol not available";
    case 93:
      return "Protocol not supported";
    case 94:
      return "Socket type not supported";
    case 95:
      return "Operation not supported on transport endpoint";
    case 96:
      return "Protocol family not supported";
    case 97:
      return "Address family not supported by protocol";
    case 98:
      return "Address already in use";
    case 99:
      return "Cannot assign requested address";
    case 100:
      return "Network is down";
    case 101:
      return "Network is unreachable";
    case 102:
      return "Network dropped connection because of reset";
    case 103:
      return "Software caused connection abort";
    case 104:
      return "Connection reset by peer";
    case 105:
      return "No buffer space available";
    case 106:
      return "Transport endpoint is already connected";
    case 107:
      return "Transport endpoint is not connected";
    case 108:
      return "Cannot send after transport endpoint shutdown";
    case 109:
      return "Too many references: cannot splice";
    case 110:
      return "Connection timed out";
    case 111:
      return "Connection refused";
    case 112:
      return "Host is down";
    case 113:
      return "No route to host";
    case 114:
      return "Operation already in progress";
    case 115:
      return "Operation now in progress";
    case 116:
      return "Stale NFS file handle";
    case 117:
      return "Structure needs cleaning";
    case 118:
      return "Not a XENIX named type file";
    case 119:
      return "No XENIX semaphores available";
    case 120:
      return "Is a named type file";
    case 121:
      return "Remote I/O error";
    case 122:
      return "Quota exceeded";
    case 123:
      return "No medium found";
    case 124:
      return "Wrong medium type";
    case 125:
      return "Operation Canceled";
    case 126:
      return "Required key not available";
    case 127:
      return "Key has expired";
    case 128:
      return "Key has been revoked";
    case 129:
      return "Key was rejected by service";
    case 130:
      return "Owner died";
    case 131:
      return "State not recoverable";
    case 132:
      return "Operation not possible due to RF-kill";
    case 133:
      return "Memory page has hardware error";
    default:
      return nullptr;
  }
}
}  // namespace
const char* error_string(int e) {
  if (auto s = known_error(e)) return s;
  thread_local std::string unknown;
  unknown = "Unknown error " + std::to_string(e);
  return unknown.c_str();
}
namespace {
const char* error_string32(int e) {
  if (auto s = known_error(e)) return low_string(s);
  thread_local GuestStorage storage;
  return storage.string(error_string(e));
}
int strerror_r_impl(int e, char* d, std::uint64_t n) {
  auto s = error_string(e);
  auto len = std::strlen(s);
  if (n) {
    auto k = std::min<std::uint64_t>(len, n - 1);
    std::uint64_t ignored;
    platform::NativeMemoryFault fault;
    check_memory_range(h2g(d), k, true);
    if (!platform::native_memory(platform::NativeMemoryOp::copy, h2g(d), h2g(s), k, 0, ignored, fault, false))
      throw MemoryFault{fault.address, fault.write, fault.bus};
    const char zero = 0;
    check_memory_range(h2g(d) + k, 1, true);
    if (!platform::guarded_store(h2g(d) + k, &zero, 1)) throw MemoryFault{h2g(d) + k, true};
  }
  // API-32 bionic returns -1/errno=ERANGE on truncation, and formats unknown
  // error numbers successfully (libc/bionic/strerror.cpp).
  if (n <= len) {
    set_error(err::kERANGE);
    return -1;
  }
  return 0;
}
char* gnu_strerror_r(int e, char* d, std::uint64_t n) {
  int saved = *error_location();
  strerror_r_impl(e, d, n);
  set_error(saved);
  return d;
}
const char* strsignal_impl(int n) {
  static const char* const names[] = {"Hangup",
                                      "Interrupt",
                                      "Quit",
                                      "Illegal instruction",
                                      "Trap",
                                      "Aborted",
                                      "Bus error",
                                      "Floating point exception",
                                      "Killed",
                                      "User signal 1",
                                      "Segmentation fault",
                                      "User signal 2",
                                      "Broken pipe",
                                      "Alarm clock",
                                      "Terminated",
                                      "Stack fault",
                                      "Child exited",
                                      "Continue",
                                      "Stopped (signal)",
                                      "Stopped",
                                      "Stopped (tty input)",
                                      "Stopped (tty output)",
                                      "Urgent I/O condition",
                                      "CPU time limit exceeded",
                                      "File size limit exceeded",
                                      "Virtual timer expired",
                                      "Profiling timer expired",
                                      "Window size changed",
                                      "I/O possible",
                                      "Power failure",
                                      "Bad system call"};
  if (n > 0 && n <= 31) return names[n - 1];
  thread_local std::string msg;
  msg = n >= 41 && n <= 64 ? "Real-time signal " + std::to_string(n - 41)
                           : "Unknown signal " + std::to_string(n);
  return msg.c_str();
}
const char* strsignal32(int n) {
  if (n > 0 && n <= 31) return low_string(strsignal_impl(n));
  thread_local GuestStorage storage;
  return storage.string(strsignal_impl(n));
}
}  // namespace
void register_string_errors(HleLibrary& lib) {
  if (process_config().isa() == GuestIsa::a32) {
    bind<&error_string32>(lib, "strerror");
    bind<&strsignal32>(lib, "strsignal");
  } else {
    bind<&error_string>(lib, "strerror");
    bind<&strsignal_impl>(lib, "strsignal");
  }
  bind<&strerror_r_impl>(lib, "strerror_r");
  bind<&gnu_strerror_r>(lib, "__gnu_strerror_r");
}
}  // namespace venture::hle::libc
