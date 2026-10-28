#include "vtest.h"
#include <cstdlib>
#include <cwchar>
#include <strings.h>

// Asset parsers, serialization and IL2CPP generated code call these constantly.
// memcpy requires disjoint buffers; overlap is deliberately tested with memmove.
extern "C" int vtest_main(int, char**) {
    Vtest q("strings");
    char buffer[64], copy[64];
    memset(buffer, 0x41, sizeof(buffer));
    CHECK(buffer[63] == 'A');
    memcpy(buffer, "abcdefghij", 11);
    memcpy(copy, buffer, 11);
    CHECK(memcmp(copy, buffer, 11) == 0);
    memmove(buffer + 2, buffer, 8);
    CHECK(memcmp(buffer, "ababcdefgh", 10) == 0);
    memmove(buffer, buffer + 2, 8);
    buffer[8] = 0;
    CHECK(strcmp(buffer, "abcdefgh") == 0);
    CHECK(memchr(buffer, 'd', 8) == buffer + 3);
    q.step("memory: copy overlap fill OK");
    strcpy(buffer, "quest"); strcat(buffer, "port");
    CHECK(strlen(buffer) == 9 && strnlen(buffer, 4) == 4);
    CHECK(strncmp(buffer, "quest", 5) == 0 && strcasecmp("Quest", "qUEST") == 0);
    CHECK(strchr(buffer, 'p') == buffer + 5 && strrchr("ababa", 'a') != nullptr);
    CHECK(strstr(buffer, "port") == buffer + 5);
    CHECK(strspn("abc123", "abc") == 3 && strcspn("abc123", "123") == 3);
    CHECK(strpbrk(buffer, "xp") == buffer + 5);
    strncpy(copy, "xy", 4);
    CHECK(copy[0] == 'x' && copy[2] == 0 && copy[3] == 0);
    char* duplicated = strdup(buffer);
    REQUIRE(duplicated != nullptr);
    CHECK(strcmp(duplicated, buffer) == 0); free(duplicated);
    char tokens[] = "one,,two:three";
    char* state = nullptr;
    char* token = strtok_r(tokens, ",:", &state);
    REQUIRE(token && strcmp(token, "one") == 0);
    token = strtok_r(nullptr, ",:", &state);
    REQUIRE(token && strcmp(token, "two") == 0);
    token = strtok_r(nullptr, ",:", &state);
    REQUIRE(token && strcmp(token, "three") == 0);
    CHECK(strtok_r(nullptr, ",:", &state) == nullptr);
    q.step("strings: search tokenize duplicate OK");
    wchar_t wide[32];
    wcscpy(wide, L"Ven"); wcscat(wide, L"ture");
    CHECK(wcslen(wide) == 7 && wcscmp(wide, L"Venture") == 0);
    CHECK(wcschr(wide, L't') == wide + 3 && wcsstr(wide, L"ture") == wide + 3);
    wchar_t moved[32]; wmemcpy(moved, wide, 8);
    wmemmove(moved + 1, moved, 7); moved[8] = 0;
    CHECK(wcscmp(moved, L"VVenture") == 0);
    wmemset(moved, L'x', 3); CHECK(wmemcmp(moved, L"xxx", 3) == 0);
    mbstate_t mb{}; wchar_t wc = 0;
    CHECK(mbrtowc(&wc, "A", 1, &mb) == 1 && wc == L'A');
    CHECK(wcrtomb(copy, L'B', &mb) == 1 && copy[0] == 'B');
    q.step("wide: copy search conversion OK");
    return q.finish();
}
