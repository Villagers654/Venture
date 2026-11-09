// An empty shared object linked under a system library's soname (libc.so,
// libstdc++.so) so a freestanding guest fixture records DT_NEEDED for it.
// Venture binds those imports to its HLE library, never to this file.
void vtest_soname_stub(void) {}
