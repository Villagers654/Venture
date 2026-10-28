// Compile with NDK r30 clang --target=armv7a-linux-androideabi23 -c.
// Independent compiler oracle for the libc ILP32 wire adapters.
#include <stddef.h>
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>
#include <poll.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <signal.h>
#include <setjmp.h>
#include <time.h>
#include <wchar.h>
#include <fenv.h>
#include <dirent.h>
#include <netdb.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/resource.h>
#include <sys/uio.h>
#define SIZE(t,n) _Static_assert(sizeof(t) == (n), #t)
#define OFFSET(t,m,n) _Static_assert(offsetof(t,m) == (n), #t "." #m)
SIZE(pthread_mutex_t,4); SIZE(pthread_cond_t,4); SIZE(pthread_rwlock_t,40);
SIZE(pthread_attr_t,24); SIZE(pthread_t,4); SIZE(pthread_key_t,4); SIZE(sem_t,4);
SIZE(struct sigaction,16); OFFSET(struct sigaction,sa_flags,8);
SIZE(struct sigaction64,20); OFFSET(struct sigaction64,sa_mask,12);
SIZE(stack_t,12); SIZE(sigset_t,4); SIZE(sigset64_t,8);
SIZE(jmp_buf,256); SIZE(sigjmp_buf,260);
SIZE(struct timespec,8); SIZE(struct timeval,8); SIZE(struct tm,44);
SIZE(struct stat,104); SIZE(struct stat64,104);
OFFSET(struct stat,st_size,48); OFFSET(struct stat,st_ino,96);
SIZE(struct dirent,280); OFFSET(struct dirent,d_name,19);
SIZE(struct sysinfo,64); SIZE(struct rlimit,8); SIZE(struct iovec,8);
SIZE(mbstate_t,4); SIZE(fenv_t,4); SIZE(struct addrinfo,32); SIZE(struct hostent,20);
SIZE(struct passwd,24);
SIZE(struct statfs,88); SIZE(struct statvfs,44); SIZE(struct statvfs64,44);
SIZE(struct pollfd,8); OFFSET(struct pollfd,revents,6);
SIZE(socklen_t,4); SIZE(struct sockaddr,16); SIZE(struct sockaddr_in,16);
SIZE(struct sockaddr_in6,28); SIZE(struct sockaddr_un,110);
