// Compile with NDK clang --target=armv7a-linux-androideabi23 -fsyntax-only.
#include <zlib.h>
#include <stddef.h>
_Static_assert(sizeof(z_stream) == 56, "ARM z_stream size");
_Static_assert(_Alignof(z_stream) == 4, "ARM z_stream alignment");
_Static_assert(offsetof(z_stream, state) == 28, "ARM state offset");
_Static_assert(offsetof(z_stream, adler) == 48, "ARM adler offset");
