// Diagnostic fixture: a non-throwing suspend acknowledgement must not wait
// for the condition mutex owner. No game, graphics or account data is used.
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static volatile sig_atomic_t waiting, acknowledged;
static void handler(int signal) { (void)signal; ++acknowledged; }
static void* worker(void* unused) {
 (void)unused; pthread_mutex_lock(&lock); waiting = 1;
 pthread_cond_wait(&condition, &lock); pthread_mutex_unlock(&lock); return 0;
}
__attribute__((visibility("default"))) int signal_unwind_fixture(unsigned mode, uint32_t* output) {
 struct sigaction action = {0}; action.sa_handler = handler;
 sigemptyset(&action.sa_mask); sigaction(SIGUSR1, &action, 0);
 pthread_t thread; pthread_create(&thread, 0, worker, 0);
 while (!waiting) usleep(1000);
 pthread_mutex_lock(&lock);
 if (mode == 1) pthread_cond_signal(&condition); // Already reacquiring the mutex.
 pthread_kill(thread, SIGUSR1);
 for (int i = 0; i < 200 && !acknowledged; ++i) usleep(1000);
 int timely = acknowledged;
 if (timely) {
  pthread_kill(thread, SIGUSR1);
  for (int i = 0; i < 200 && acknowledged < 2; ++i) usleep(1000);
  timely = acknowledged >= 2;
 }
 pthread_cond_signal(&condition); pthread_mutex_unlock(&lock); pthread_join(thread, 0);
 output[0] = timely; output[1] = acknowledged; output[2] = 1;
 return timely ? 0 : 1;
}
