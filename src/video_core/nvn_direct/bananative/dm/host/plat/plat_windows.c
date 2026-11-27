// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Platform layer, Windows side (Lichii): the plat.h services the NVN direct renderer uses (tables,
// clocks, safe memory reads, thread CPU time; the pointer map and SHA-256 are plat_portable.c),
// and the POSIX subset bananative/shim/win32 declares for the rest of the code (pthreads, clocks
// and sleeps, the environment, anonymous and read-only file mappings).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <errno.h>
#include <io.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>
#include "../dm.h"
#include <dirent.h>
#include <dlfcn.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

// ---- Object file conventions ----
// Each table is a COFF grouped section: the linker sorts name$a, name$m (the entries, PLAT_TABLE) and name$z into one section, so the
// entries lie between a marker in $a and one in $z. The markers are pointer-sized, as aligned as the entries, so none leaves a gap.
// A linker may pad between contributions with zeros (incremental linking): the readers skip entries without a name
#define TABLE(n)                                                                                   \
    __attribute__((used, section(#n "$a"))) static const void *const n##_start = NULL;            \
    __attribute__((used, section(#n "$z"))) static const void *const n##_stop = NULL;
TABLE(swhle) TABLE(swforce) TABLE(swnvn)
const void *plat_table(const char *name, unsigned long *size) {
    const char *b = NULL, *e = NULL;
    if (!strcmp(name, "swhle")) { b = (const char *)(&swhle_start + 1); e = (const char *)&swhle_stop; }
    else if (!strcmp(name, "swforce")) { b = (const char *)(&swforce_start + 1); e = (const char *)&swforce_stop; }
    else if (!strcmp(name, "swnvn")) { b = (const char *)(&swnvn_start + 1); e = (const char *)&swnvn_stop; }
    *size = b && e > b ? (unsigned long)(e - b) : 0;
    return *size ? b : NULL;
}

// ---- Time ----
static uint64_t filetime_100ns(FILETIME f) { return (uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime; }
int clock_gettime(clockid_t c, struct timespec *ts) {
    uint64_t ns;
    if (c == CLOCK_MONOTONIC) {
        static LARGE_INTEGER freq;   // (the same on every call: a benign race)
        LARGE_INTEGER t;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t);
        uint64_t f = (uint64_t)freq.QuadPart, n = (uint64_t)t.QuadPart;
        ns = n / f * 1000000000ull + n % f * 1000000000ull / f;
    } else if (c == CLOCK_REALTIME) {
        FILETIME f;
        GetSystemTimePreciseAsFileTime(&f);
        ns = (filetime_100ns(f) - 116444736000000000ull) * 100;   // from 1601 to 1970
    } else if (c == CLOCK_THREAD_CPUTIME_ID) {
        FILETIME created, exited, kernel, user;
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) { errno = EINVAL; return -1; }
        ns = (filetime_100ns(kernel) + filetime_100ns(user)) * 100;
    } else {
        errno = EINVAL;
        return -1;
    }
    ts->tv_sec = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}
uint64_t plat_clock_ns(clockid_t c) { struct timespec ts; clock_gettime(c, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
int64_t plat_thread_self_system_ns(void) {
    FILETIME created, exited, kernel, user;
    return GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) ? (int64_t)(filetime_100ns(kernel) * 100) : -1;
}
// Sleeps of less than the scheduler's tick (the replay polls in steps of 50-200 us): a high-resolution waitable timer per thread. Where the system
// cannot make one, a regular waitable timer (it ends on the system timer's tick), and Sleep where there is no timer at all. Each thread tries once
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION   // (Windows 10 1803; older SDKs lack the name, as in common/thread.cpp)
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static _Thread_local HANDLE t_timer;
static _Thread_local int t_timer_tried;
static volatile LONG g_timer_failure_logged;
int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    uint64_t ns = (uint64_t)req->tv_sec * 1000000000ull + (uint64_t)req->tv_nsec;
    if (!t_timer_tried) {
        t_timer_tried = 1;
        t_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!t_timer) {
            DWORD high_resolution_error = GetLastError();
            t_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
            DWORD regular_error = t_timer ? 0 : GetLastError();
            if (!InterlockedExchange(&g_timer_failure_logged, 1)) {
                if (t_timer) LOG("no high-resolution waitable timer (error %lu): sleeps end on the system timer's tick", high_resolution_error);
                else LOG("no high-resolution waitable timer (error %lu) nor a regular one (error %lu): sleeps are rounded up to whole milliseconds", high_resolution_error, regular_error);
            }
        }
    }
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)((ns + 99) / 100);   // relative, in 100 ns steps
    if (t_timer && SetWaitableTimerEx(t_timer, &due, 0, NULL, NULL, NULL, 0)) WaitForSingleObject(t_timer, INFINITE);
    else Sleep((DWORD)((ns + 999999) / 1000000));
    return 0;
}
int usleep(unsigned usec) { struct timespec ts = {(time_t)(usec / 1000000), (long)(usec % 1000000) * 1000}; return nanosleep(&ts, NULL); }

// ---- Environment, strings ----
int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) ? -1 : 0;
}
int unsetenv(const char *name) { return _putenv_s(name, "") ? -1 : 0; }   // (an empty value removes the variable)
size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t n = strlen(src);
    if (size) { size_t k = n < size - 1 ? n : size - 1; memcpy(dst, src, k); dst[k] = 0; }
    return n;
}
char *strcasestr(const char *s, const char *find) {
    size_t n = strlen(find);
    for (; *s; s++) if (!_strnicmp(s, find, n)) return (char *)s;
    return n ? NULL : (char *)s;
}
int gettid(void) { return (int)GetCurrentThreadId(); }
int getpagesize(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwPageSize; }
long sysconf(int name) {
    if (name == _SC_NPROCESSORS_ONLN) return (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    errno = EINVAL;
    return -1;
}

// ---- Files ----
// Paths are UTF-8 (Lichii's). fopen's "e" (close-on-exec) is the C library's "N"
#undef fopen
#undef rename
#undef unlink
static int wide(const char *s, WCHAR *w, int n) { return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) > 0; }
FILE *plat_win32_fopen(const char *path, const char *mode) {
    WCHAR wp[MAX_PATH], wm[16];
    int k = 0;
    for (const char *m = mode; *m && k < 15; m++) wm[k++] = *m == 'e' ? L'N' : (WCHAR)*m;
    wm[k] = 0;
    if (!wide(path, wp, MAX_PATH)) { errno = ENOENT; return NULL; }
    return _wfopen(wp, wm);
}
int plat_win32_rename(const char *from, const char *to) {
    WCHAR wf[MAX_PATH], wt[MAX_PATH];
    if (!wide(from, wf, MAX_PATH) || !wide(to, wt, MAX_PATH)) { errno = ENOENT; return -1; }
    if (MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING)) return 0;
    errno = GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES;
    return -1;
}
int plat_win32_unlink(const char *path) {
    WCHAR wp[MAX_PATH];
    if (!wide(path, wp, MAX_PATH)) { errno = ENOENT; return -1; }
    return _wunlink(wp);
}
int truncate(const char *path, long long length) {
    WCHAR wp[MAX_PATH];
    if (!wide(path, wp, MAX_PATH)) { errno = ENOENT; return -1; }
    HANDLE h = CreateFileW(wp, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { errno = ENOENT; return -1; }
    LARGE_INTEGER at;
    at.QuadPart = length;
    BOOL ok = SetFilePointerEx(h, at, NULL, FILE_BEGIN) && SetEndOfFile(h);
    CloseHandle(h);
    if (!ok) { errno = EIO; return -1; }
    return 0;
}
struct plat_win32_dir { HANDLE h; WIN32_FIND_DATAW f; int first; struct dirent e; };
DIR *opendir(const char *path) {
    char pat[MAX_PATH];
    WCHAR wp[MAX_PATH];
    if (snprintf(pat, sizeof pat, "%s\\*", path) >= (int)sizeof pat || !wide(pat, wp, MAX_PATH)) { errno = ENOENT; return NULL; }
    DIR *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    if ((d->h = FindFirstFileW(wp, &d->f)) == INVALID_HANDLE_VALUE) { free(d); errno = ENOENT; return NULL; }
    d->first = 1;
    return d;
}
struct dirent *readdir(DIR *d) {
    if (!d->first && !FindNextFileW(d->h, &d->f)) return NULL;
    d->first = 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, d->f.cFileName, -1, d->e.d_name, sizeof d->e.d_name, NULL, NULL)) d->e.d_name[0] = 0;
    return &d->e;
}
int closedir(DIR *d) { FindClose(d->h); free(d); return 0; }

// ---- Libraries ----
void *dlopen(const char *name, int flags) { (void)flags; return (void *)LoadLibraryA(name); }
void *dlsym(void *lib, const char *name) { return (void *)GetProcAddress((HMODULE)lib, name); }
int dlclose(void *lib) { return FreeLibrary((HMODULE)lib) ? 0 : -1; }

// ---- Scheduling ----
int sched_yield(void) { SwitchToThread(); return 0; }
static HANDLE thread_handle(int tid, DWORD access) { return tid ? OpenThread(access, FALSE, (DWORD)tid) : GetCurrentThread(); }
int sched_setaffinity(int tid, size_t size, const cpu_set_t *set) {
    (void)size;
    HANDLE h = thread_handle(tid, THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION);
    if (!h) { errno = ESRCH; return -1; }
    DWORD_PTR ok = SetThreadAffinityMask(h, (DWORD_PTR)set->bits);
    if (tid) CloseHandle(h);
    if (!ok) { errno = EINVAL; return -1; }
    return 0;
}
int setpriority(int which, int who, int nice) {
    (void)which;
    int p = nice >= 10 ? THREAD_PRIORITY_LOWEST : nice > 0 ? THREAD_PRIORITY_BELOW_NORMAL : nice == 0 ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_ABOVE_NORMAL;
    HANDLE h = thread_handle(who, THREAD_SET_LIMITED_INFORMATION);
    if (!h) { errno = ESRCH; return -1; }
    BOOL ok = SetThreadPriority(h, p);
    if (who) CloseHandle(h);
    if (!ok) { errno = EACCES; return -1; }
    return 0;
}
int getpriority(int which, int who) {
    (void)which;
    HANDLE h = thread_handle(who, THREAD_QUERY_LIMITED_INFORMATION);
    if (!h) { errno = ESRCH; return -1; }
    int p = GetThreadPriority(h);
    if (who) CloseHandle(h);
    if (p == THREAD_PRIORITY_ERROR_RETURN) { errno = ESRCH; return -1; }
    return p <= THREAD_PRIORITY_LOWEST ? 10 : p < 0 ? 5 : p == 0 ? 0 : -4;
}

// ---- Memory ----
// 0 = every byte copied; an inaccessible page fails the read (ERROR_PARTIAL_COPY) instead of faulting
int plat_mem_read(uint64_t addr, void *out, size_t len) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)addr, out, len, &got) && got == len ? 0 : -1;
}
static DWORD page_prot(int prot) {
    switch (prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) {
    case PROT_NONE: return PAGE_NOACCESS;
    case PROT_READ: return PAGE_READONLY;
    case PROT_EXEC: return PAGE_EXECUTE;
    case PROT_READ | PROT_EXEC: return PAGE_EXECUTE_READ;
    case PROT_READ | PROT_WRITE | PROT_EXEC: return PAGE_EXECUTE_READWRITE;
    default: return PAGE_READWRITE;   // (write implies read on Windows)
    }
}
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long long off) {
    if (!len) { errno = EINVAL; return MAP_FAILED; }
    if (flags & MAP_FIXED) {   // over a reservation of this file's: inaccessible again
        MEMORY_BASIC_INFORMATION mi;
        if (prot != PROT_NONE || !(flags & MAP_ANONYMOUS) || !VirtualQuery(addr, &mi, sizeof mi) || mi.Type != MEM_PRIVATE || mi.State == MEM_FREE) { errno = EINVAL; return MAP_FAILED; }
        if (!VirtualFree(addr, len, MEM_DECOMMIT)) { errno = ENOMEM; return MAP_FAILED; }
        return addr;
    }
    if ((flags & (MAP_SHARED | MAP_PRIVATE)) == MAP_PRIVATE && (flags & MAP_ANONYMOUS)) {
        void *p = VirtualAlloc(NULL, len, prot == PROT_NONE ? MEM_RESERVE : MEM_RESERVE | MEM_COMMIT, page_prot(prot));
        if (!p) { errno = ENOMEM; return MAP_FAILED; }
        return p;
    }
    if ((flags & (MAP_SHARED | MAP_PRIVATE)) == MAP_SHARED) {
        HANDLE section;
        DWORD view;
        if (flags & MAP_ANONYMOUS) {   // shared memory: a section of its own in the page file (zero filled)
            section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)((uint64_t)len >> 32), (DWORD)len, NULL);
            view = prot & PROT_WRITE ? FILE_MAP_WRITE : FILE_MAP_READ;
        } else if (prot == PROT_READ) {
            HANDLE file = (HANDLE)_get_osfhandle(fd);
            section = file == INVALID_HANDLE_VALUE ? NULL : CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
            view = FILE_MAP_READ;
        } else {
            errno = EINVAL;
            return MAP_FAILED;
        }
        if (!section) { errno = ENOMEM; return MAP_FAILED; }
        void *p = MapViewOfFile(section, view, (DWORD)((uint64_t)off >> 32), (DWORD)off, len);
        CloseHandle(section);   // (the view keeps the section)
        if (!p) { errno = ENOMEM; return MAP_FAILED; }
        return p;
    }
    errno = EINVAL;
    return MAP_FAILED;
}
int munmap(void *addr, size_t len) {
    (void)len;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(addr, &mi, sizeof mi) || mi.AllocationBase != addr) { errno = EINVAL; return -1; }
    BOOL ok = mi.Type == MEM_MAPPED ? UnmapViewOfFile(addr) : mi.Type == MEM_PRIVATE ? VirtualFree(addr, 0, MEM_RELEASE) : FALSE;
    if (!ok) { errno = EINVAL; return -1; }
    return 0;
}
int mprotect(void *addr, size_t len, int prot) {
    DWORD old;
    if (!VirtualProtect(addr, len, page_prot(prot), &old)) { errno = EACCES; return -1; }
    return 0;
}

// ---- Address waits ----
int plat_addr_wait(_Atomic uint32_t *a, uint32_t v, uint32_t ms) {
    if (WaitOnAddress((volatile VOID *)a, &v, sizeof v, ms)) return 0;
    errno = GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
    return -1;
}
void plat_addr_wake_one(_Atomic uint32_t *a) { WakeByAddressSingle((PVOID)a); }

// ---- pthreads ----
_Static_assert(sizeof(pthread_mutex_t) == sizeof(SRWLOCK) && sizeof(pthread_cond_t) == sizeof(CONDITION_VARIABLE) && sizeof(pthread_once_t) == sizeof(INIT_ONCE),
               "the pthread stand-ins hold the Win32 objects");
#define SRW(m) ((PSRWLOCK)&(m)->srw)
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) { (void)a; InitializeSRWLock(SRW(m)); return 0; }
int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m) { AcquireSRWLockExclusive(SRW(m)); return 0; }
int pthread_mutex_trylock(pthread_mutex_t *m) { return TryAcquireSRWLockExclusive(SRW(m)) ? 0 : EBUSY; }
int pthread_mutex_unlock(pthread_mutex_t *m) { ReleaseSRWLockExclusive(SRW(m)); return 0; }

#define CV(c) ((PCONDITION_VARIABLE)&(c)->cv)
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) { (void)a; InitializeConditionVariable(CV(c)); return 0; }
int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { SleepConditionVariableSRW(CV(c), SRW(m), INFINITE, 0); return 0; }
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t ns = ((int64_t)abstime->tv_sec - (int64_t)now.tv_sec) * 1000000000ll + (abstime->tv_nsec - now.tv_nsec);
    int64_t ms = ns <= 0 ? 0 : (ns + 999999) / 1000000;
    if (ms >= INFINITE) ms = INFINITE - 1;
    if (SleepConditionVariableSRW(CV(c), SRW(m), (DWORD)ms, 0)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
}
int pthread_cond_signal(pthread_cond_t *c) { WakeConditionVariable(CV(c)); return 0; }
int pthread_cond_broadcast(pthread_cond_t *c) { WakeAllConditionVariable(CV(c)); return 0; }

// An SRW lock is released as it was taken: the thread that holds it exclusively is the one that recorded itself as the writer
int pthread_rwlock_rdlock(pthread_rwlock_t *l) { AcquireSRWLockShared(SRW(l)); return 0; }
int pthread_rwlock_tryrdlock(pthread_rwlock_t *l) { return TryAcquireSRWLockShared(SRW(l)) ? 0 : EBUSY; }
int pthread_rwlock_wrlock(pthread_rwlock_t *l) { AcquireSRWLockExclusive(SRW(l)); l->writer = GetCurrentThreadId(); return 0; }
int pthread_rwlock_unlock(pthread_rwlock_t *l) {
    if (l->writer == GetCurrentThreadId()) { l->writer = 0; ReleaseSRWLockExclusive(SRW(l)); }
    else ReleaseSRWLockShared(SRW(l));
    return 0;
}

static BOOL CALLBACK once_run(PINIT_ONCE o, PVOID fn, PVOID *ctx) { (void)o; (void)ctx; ((void (*)(void))fn)(); return TRUE; }
int pthread_once(pthread_once_t *o, void (*fn)(void)) { InitOnceExecuteOnce((PINIT_ONCE)&o->once, once_run, (PVOID)fn, NULL); return 0; }

int pthread_attr_init(pthread_attr_t *a) { a->stack_size = 0; return 0; }
int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size) { a->stack_size = size; return 0; }

// A thread's record lives until both the thread has ended and it was joined or detached (refs). A thread this file did not start has a
// record of its own with no handle: pthread_self() names it for pthread_setname_np, the one call made with it
struct pthread_win32 { HANDLE h; void *(*fn)(void *); void *arg; void *result; volatile LONG refs; };
static _Thread_local struct pthread_win32 *t_self;
static _Thread_local struct pthread_win32 t_foreign;
static void thread_release(struct pthread_win32 *t) {
    if (InterlockedDecrement(&t->refs)) return;
    CloseHandle(t->h);
    free(t);
}
static unsigned __stdcall thread_start(void *p) {
    struct pthread_win32 *t = p;
    t_self = t;
    t->result = t->fn(t->arg);
    if (t_timer) CloseHandle(t_timer);   // (nanosleep's)
    thread_release(t);
    return 0;
}
int pthread_create(pthread_t *out, const pthread_attr_t *a, void *(*fn)(void *), void *arg) {
    struct pthread_win32 *t = calloc(1, sizeof *t);
    if (!t) return EAGAIN;
    t->fn = fn; t->arg = arg; t->refs = 2;   // the thread's, and the creator's until join or detach
    // The stack glibc gives a thread by default (8 MiB) unless one was asked for: reserved, committed as it is used (the replay's threads run the shader recompiler)
    size_t stack = a && a->stack_size ? a->stack_size : 8u << 20;
    t->h = (HANDLE)_beginthreadex(NULL, (unsigned)stack, thread_start, t, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t->h) { free(t); return EAGAIN; }
    *out = t;
    return 0;
}
int pthread_detach(pthread_t t) { thread_release(t); return 0; }
int pthread_join(pthread_t t, void **result) {
    if (WaitForSingleObject(t->h, INFINITE) != WAIT_OBJECT_0) return EINVAL;
    if (result) *result = t->result;
    thread_release(t);
    return 0;
}
pthread_t pthread_self(void) { return t_self ? t_self : &t_foreign; }
// Thread names for debuggers and crash dumps: SetThreadDescription (Windows 10 1607), looked up as common/thread.cpp does
int pthread_setname_np(pthread_t t, const char *name) {
    typedef HRESULT(WINAPI * set_fn)(HANDLE, PCWSTR);
    static set_fn set;
    if (!set) set = (set_fn)(void *)GetProcAddress(GetModuleHandleW(L"KernelBase.dll"), "SetThreadDescription");
    WCHAR w[64];
    if (!set || !MultiByteToWideChar(CP_UTF8, 0, name, -1, w, (int)(sizeof w / sizeof *w))) return EINVAL;
    return SUCCEEDED(set(t->h ? t->h : GetCurrentThread(), w)) ? 0 : EINVAL;
}
