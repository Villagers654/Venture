#include "vtest.h"
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// Save games, asset streams and memory-mapped bundles require a coherent VFS.
// Operate only below argv[1]/vtest-files, which must not already exist.
extern "C" int vtest_main(int argc, char** argv) {
    Vtest q("files"); REQUIRE(argc >= 2 && argv[1]);
    std::string directory = std::string(argv[1]) + "/vtest-files";
    std::string file = directory + "/data", renamed = directory + "/renamed";
    REQUIRE(mkdir(directory.c_str(), 0700) == 0);
    FILE* stream = fopen(file.c_str(), "wb+"); REQUIRE(stream);
    const char contents[] = "Venture";
    constexpr auto length = sizeof(contents) - 1;
    CHECK(fwrite(contents, 1, length, stream) == length && fflush(stream) == 0);
    CHECK(ftell(stream) == length && fseek(stream, 0, SEEK_SET) == 0);
    char buffer[16] = {};
    CHECK(fread(buffer, 1, length, stream) == length && memcmp(buffer, contents, length) == 0);
    CHECK(fread(buffer, 1, 1, stream) == 0 && feof(stream));
    CHECK(fclose(stream) == 0);
    q.step("files: stdio write flush seek read EOF OK");
    int fd = open(file.c_str(), O_RDWR); REQUIRE(fd >= 0);
    CHECK(lseek(fd, 5, SEEK_SET) == 5 && write(fd, "-", 1) == 1);
    CHECK(pwrite(fd, contents + 5, 1, 5) == 1 && lseek(fd, 0, SEEK_CUR) == 6);
    memset(buffer, 0, sizeof(buffer)); CHECK(pread(fd, buffer, length, 0) == length && strcmp(buffer, contents) == 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0 && read(fd, buffer, length) == length);
    struct stat st{}; CHECK(fstat(fd, &st) == 0 && st.st_size == length && S_ISREG(st.st_mode));
    CHECK(stat(file.c_str(), &st) == 0 && st.st_size == length);
    CHECK(lstat(file.c_str(), &st) == 0 && S_ISREG(st.st_mode));
    CHECK(access(file.c_str(), R_OK | W_OK) == 0);
    q.step("files: descriptors positional IO stat access OK");
    long page = sysconf(_SC_PAGESIZE); REQUIRE(page > 0);
    void* mapping = mmap(nullptr, size_t(page), PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    REQUIRE(mapping != MAP_FAILED);
    CHECK(memcmp(mapping, contents, length) == 0); static_cast<char*>(mapping)[0] = 'X';
    CHECK(msync(mapping, size_t(page), MS_SYNC) == 0 && munmap(mapping, size_t(page)) == 0);
    CHECK(pread(fd, buffer, 1, 0) == 1 && buffer[0] == contents[0]); CHECK(close(fd) == 0);
    mapping = mmap(nullptr, size_t(page), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    REQUIRE(mapping != MAP_FAILED);
    auto bytes = static_cast<unsigned char*>(mapping);
    CHECK(bytes[0] == 0 && bytes[page - 1] == 0); bytes[0] = 42;
    CHECK(mprotect(mapping, size_t(page), PROT_READ) == 0 && bytes[0] == 42);
    CHECK(mprotect(mapping, size_t(page), PROT_READ | PROT_WRITE) == 0); bytes[0] = 17;
    CHECK(munmap(mapping, size_t(page)) == 0);
    q.step("files: private file mmap anonymous mmap mprotect msync OK");
    REQUIRE(rename(file.c_str(), renamed.c_str()) == 0);
    errno = 0; CHECK(access(file.c_str(), F_OK) == -1 && errno == ENOENT);
    DIR* dir = opendir(directory.c_str()); REQUIRE(dir);
    int matches = 0; errno = 0;
    while (dirent* entry = readdir(dir)) if (strcmp(entry->d_name, "renamed") == 0) ++matches;
    CHECK(errno == 0 && matches == 1 && closedir(dir) == 0);
    char* absolute = realpath(renamed.c_str(), nullptr); REQUIRE(absolute);
    CHECK(absolute[0] == '/' && access(absolute, F_OK) == 0); free(absolute);
    char* cwd = getcwd(nullptr, 0); REQUIRE(cwd); CHECK(cwd[0] == '/'); free(cwd);
    CHECK(unlink(renamed.c_str()) == 0 && rmdir(directory.c_str()) == 0);
    q.step("files: rename directory iteration realpath cwd cleanup OK");
    return q.finish();
}
