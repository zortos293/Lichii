// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Windows stand-in for the pthread subset Banana Native's code uses, over Win32's slim reader/writer
// locks, condition variables, one-time initialization and threads (plat_windows.c). Mutexes are not
// recursive, as nothing here makes a recursive one. The objects are as large as the Win32 ones they
// hold, so this header does not include windows.h.
#pragma once
#include <stddef.h>
#include <time.h>

typedef struct pthread_win32 *pthread_t;   // the thread's handle and its result (plat_windows.c)
typedef struct { void *srw; } pthread_mutex_t;   // SRWLOCK
typedef struct { void *cv; } pthread_cond_t;     // CONDITION_VARIABLE
typedef struct { void *srw; unsigned long writer; } pthread_rwlock_t;   // SRWLOCK, and the thread ID that holds it exclusively
typedef struct { void *once; } pthread_once_t;   // INIT_ONCE
typedef struct { size_t stack_size; } pthread_attr_t;
typedef int pthread_mutexattr_t;
typedef int pthread_condattr_t;
typedef int pthread_rwlockattr_t;

#define PTHREAD_MUTEX_INITIALIZER {0}
#define PTHREAD_COND_INITIALIZER {0}
#define PTHREAD_RWLOCK_INITIALIZER {0, 0}
#define PTHREAD_ONCE_INIT {0}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);   // EBUSY when held
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime);   // CLOCK_REALTIME deadline; ETIMEDOUT
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

int pthread_rwlock_rdlock(pthread_rwlock_t *l);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *l);   // EBUSY when held exclusively
int pthread_rwlock_wrlock(pthread_rwlock_t *l);
int pthread_rwlock_unlock(pthread_rwlock_t *l);

int pthread_once(pthread_once_t *o, void (*fn)(void));

int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size);
int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
int pthread_detach(pthread_t t);
int pthread_join(pthread_t t, void **result);
pthread_t pthread_self(void);
int pthread_setname_np(pthread_t t, const char *name);
