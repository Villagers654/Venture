# Linux release baseline (tools/linux-baseline/Containerfile, glibc 2.35).
# The C++ runtime (libc++/libc++abi), libgcc and libatomic are linked
# statically, so shipped binaries need only glibc and kernel/desktop libraries.
# Archive symbols stay local: no libc++ or libatomic symbol is exported to
# dlopen'ed drivers, OpenXR runtimes or the JVM, which bring their own runtime.
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
set(venture_baseline_link "-stdlib=libc++ -static-libstdc++ -static-libgcc -fuse-ld=lld -Wl,--exclude-libs,ALL -Wl,--as-needed")
if(EXISTS /opt/venture-baseline/static)
  string(APPEND venture_baseline_link " -L/opt/venture-baseline/static")
endif()
set(CMAKE_EXE_LINKER_FLAGS_INIT "${venture_baseline_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${venture_baseline_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${venture_baseline_link}")
# OpenSSL (libcurl's TLS and digests) is linked from its static archives.
set(OPENSSL_USE_STATIC_LIBS TRUE)
