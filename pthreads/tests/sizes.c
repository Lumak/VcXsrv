#define _WIN32_WINNT 0x400

#include "test.h"
/* Cheating here - sneaking a peek at library internals */
#include "../config.h"
#include "../implement.h"

int
main()
{
  printf("Sizes of pthreads-win32 structs\n");
  printf("-------------------------------\n");
  printf("%30s %4d\n", "pthread_t", (int)sizeof(pthread_t));
  printf("%30s %4d\n", "__ptw32_thread_t", (int)sizeof(__ptw32_thread_t));
  printf("%30s %4d\n", "pthread_attr_t_", (int)sizeof(struct pthread_attr_t_));
  printf("%30s %4d\n", "sem_t_", (int)sizeof(struct sem_t_));
  printf("%30s %4d\n", "pthread_mutex_t_", (int)sizeof(struct pthread_mutex_t_));
  printf("%30s %4d\n", "pthread_mutexattr_t_", (int)sizeof(struct pthread_mutexattr_t_));
  printf("%30s %4d\n", "pthread_spinlock_t_", (int)sizeof(struct pthread_spinlock_t_));
  printf("%30s %4d\n", "pthread_barrier_t_", (int)sizeof(struct pthread_barrier_t_));
  printf("%30s %4d\n", "pthread_barrierattr_t_", (int)sizeof(struct pthread_barrierattr_t_));
  printf("%30s %4d\n", "pthread_key_t_", (int)sizeof(struct pthread_key_t_));
  printf("%30s %4d\n", "pthread_cond_t_", (int)sizeof(struct pthread_cond_t_));
  printf("%30s %4d\n", "pthread_condattr_t_", (int)sizeof(struct pthread_condattr_t_));
  printf("%30s %4d\n", "pthread_rwlock_t_", (int)sizeof(struct pthread_rwlock_t_));
  printf("%30s %4d\n", "pthread_rwlockattr_t_", (int)sizeof(struct pthread_rwlockattr_t_));
  printf("%30s %4d\n", "pthread_once_t_", (int)sizeof(struct pthread_once_t_));
  printf("%30s %4d\n", "__ptw32_cleanup_t", (int)sizeof(struct __ptw32_cleanup_t));
  printf("%30s %4d\n", "__ptw32_mcs_node_t_", (int)sizeof(struct __ptw32_mcs_node_t_));
  printf("%30s %4d\n", "sched_param", (int)sizeof(struct sched_param));
  printf("-------------------------------\n");

  return 0;
}
