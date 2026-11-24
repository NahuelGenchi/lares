// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
/* What lwIP needs of Windows (issue #109): threads, semaphores, mutexes,
 * mailboxes, a millisecond clock, a global lock, randomness. */
#include <windows.h>
#include <bcrypt.h>

#include "lwip/err.h"
#include "lwip/sys.h"

struct lares_sem {
    SRWLOCK lock;
    CONDITION_VARIABLE cv;
    unsigned count;
};

struct lares_mutex {
    CRITICAL_SECTION cs;
};

struct lares_mbox {
    SRWLOCK lock;
    CONDITION_VARIABLE not_empty, not_full;
    unsigned head, n, cap;
    void **slots;
};

static CRITICAL_SECTION protect;
static INIT_ONCE protect_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_protect(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once;
    (void)param;
    (void)ctx;
    InitializeCriticalSection(&protect);
    return TRUE;
}

void sys_init(void)
{
    InitOnceExecuteOnce(&protect_once, init_protect, NULL, NULL);
}

u32_t sys_now(void)
{
    return (u32_t)GetTickCount64();
}

sys_prot_t sys_arch_protect(void)
{
    InitOnceExecuteOnce(&protect_once, init_protect, NULL, NULL);
    EnterCriticalSection(&protect);
    return 0;
}

void sys_arch_unprotect(sys_prot_t pval)
{
    (void)pval;
    LeaveCriticalSection(&protect);
}

unsigned int lares_lwip_rand(void)
{
    unsigned int r = 0;
    BCryptGenRandom(NULL, (PUCHAR)&r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return r;
}

void lares_lwip_assert(const char *msg, int line, const char *file)
{
    char buf[512];
    wsprintfA(buf, "lwIP assertion \"%s\" failed at %s:%d\n", msg, file, line);
    OutputDebugStringA(buf);
    fputs(buf, stderr);
    abort();
}

/* ---------------------------------------------------------------- mutexes */

err_t sys_mutex_new(sys_mutex_t *mutex)
{
    struct lares_mutex *m = (struct lares_mutex *)calloc(1, sizeof *m);
    if (!m) return ERR_MEM;
    InitializeCriticalSection(&m->cs);
    *mutex = m;
    return ERR_OK;
}

void sys_mutex_lock(sys_mutex_t *mutex)
{
    EnterCriticalSection(&(*mutex)->cs);
}

void sys_mutex_unlock(sys_mutex_t *mutex)
{
    LeaveCriticalSection(&(*mutex)->cs);
}

void sys_mutex_free(sys_mutex_t *mutex)
{
    if (!sys_mutex_valid(mutex)) return;
    DeleteCriticalSection(&(*mutex)->cs);
    free(*mutex);
    *mutex = NULL;
}

/* ------------------------------------------------------------- semaphores */

/* How long until `deadline`, for a wait that may wake early. */
static DWORD left_ms(ULONGLONG deadline)
{
    ULONGLONG now = GetTickCount64();
    return now >= deadline ? 0 : (DWORD)(deadline - now);
}

err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
    struct lares_sem *s = (struct lares_sem *)calloc(1, sizeof *s);
    if (!s) return ERR_MEM;
    InitializeSRWLock(&s->lock);
    InitializeConditionVariable(&s->cv);
    s->count = count;
    *sem = s;
    return ERR_OK;
}

void sys_sem_signal(sys_sem_t *sem)
{
    struct lares_sem *s = *sem;
    AcquireSRWLockExclusive(&s->lock);
    s->count++;
    ReleaseSRWLockExclusive(&s->lock);
    WakeConditionVariable(&s->cv);
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
    struct lares_sem *s = *sem;
    ULONGLONG start = GetTickCount64(), deadline = start + timeout;
    AcquireSRWLockExclusive(&s->lock);
    while (s->count == 0) {
        DWORD wait = timeout ? left_ms(deadline) : INFINITE;
        if (timeout && wait == 0) {
            ReleaseSRWLockExclusive(&s->lock);
            return SYS_ARCH_TIMEOUT;
        }
        SleepConditionVariableSRW(&s->cv, &s->lock, wait, 0);
    }
    s->count--;
    ReleaseSRWLockExclusive(&s->lock);
    return (u32_t)(GetTickCount64() - start);
}

void sys_sem_free(sys_sem_t *sem)
{
    if (!sys_sem_valid(sem)) return;
    free(*sem);
    *sem = NULL;
}

/* -------------------------------------------------------------- mailboxes */

err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    struct lares_mbox *b = (struct lares_mbox *)calloc(1, sizeof *b);
    if (!b) return ERR_MEM;
    b->cap = size > 0 ? (unsigned)size : 64;
    b->slots = (void **)calloc(b->cap, sizeof(void *));
    if (!b->slots) {
        free(b);
        return ERR_MEM;
    }
    InitializeSRWLock(&b->lock);
    InitializeConditionVariable(&b->not_empty);
    InitializeConditionVariable(&b->not_full);
    *mbox = b;
    return ERR_OK;
}

static void put(struct lares_mbox *b, void *msg)
{
    b->slots[(b->head + b->n) % b->cap] = msg;
    b->n++;
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
    struct lares_mbox *b = *mbox;
    AcquireSRWLockExclusive(&b->lock);
    while (b->n == b->cap) SleepConditionVariableSRW(&b->not_full, &b->lock, INFINITE, 0);
    put(b, msg);
    ReleaseSRWLockExclusive(&b->lock);
    WakeConditionVariable(&b->not_empty);
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
    struct lares_mbox *b = *mbox;
    AcquireSRWLockExclusive(&b->lock);
    if (b->n == b->cap) {
        ReleaseSRWLockExclusive(&b->lock);
        return ERR_MEM;
    }
    put(b, msg);
    ReleaseSRWLockExclusive(&b->lock);
    WakeConditionVariable(&b->not_empty);
    return ERR_OK;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)
{
    return sys_mbox_trypost(mbox, msg);
}

static void *take(struct lares_mbox *b)
{
    void *msg = b->slots[b->head];
    b->head = (b->head + 1) % b->cap;
    b->n--;
    return msg;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
    struct lares_mbox *b = *mbox;
    ULONGLONG start = GetTickCount64(), deadline = start + timeout;
    AcquireSRWLockExclusive(&b->lock);
    while (b->n == 0) {
        DWORD wait = timeout ? left_ms(deadline) : INFINITE;
        if (timeout && wait == 0) {
            ReleaseSRWLockExclusive(&b->lock);
            if (msg) *msg = NULL;
            return SYS_ARCH_TIMEOUT;
        }
        SleepConditionVariableSRW(&b->not_empty, &b->lock, wait, 0);
    }
    void *m = take(b);
    ReleaseSRWLockExclusive(&b->lock);
    WakeConditionVariable(&b->not_full);
    if (msg) *msg = m;
    return (u32_t)(GetTickCount64() - start);
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
    struct lares_mbox *b = *mbox;
    AcquireSRWLockExclusive(&b->lock);
    if (b->n == 0) {
        ReleaseSRWLockExclusive(&b->lock);
        return SYS_MBOX_EMPTY;
    }
    void *m = take(b);
    ReleaseSRWLockExclusive(&b->lock);
    WakeConditionVariable(&b->not_full);
    if (msg) *msg = m;
    return 0;
}

void sys_mbox_free(sys_mbox_t *mbox)
{
    if (!sys_mbox_valid(mbox)) return;
    free((*mbox)->slots);
    free(*mbox);
    *mbox = NULL;
}

/* ---------------------------------------------------------------- threads */

struct thread_start {
    lwip_thread_fn fn;
    void *arg;
};

static DWORD WINAPI thread_main(LPVOID p)
{
    struct thread_start s = *(struct thread_start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread, void *arg, int stacksize, int prio)
{
    (void)name;
    struct thread_start *s = (struct thread_start *)malloc(sizeof *s);
    if (!s) return NULL;
    s->fn = thread;
    s->arg = arg;
    HANDLE h = CreateThread(NULL, stacksize > 0 ? (SIZE_T)stacksize : 0, thread_main, s, 0, NULL);
    if (!h) {
        free(s);
        return NULL;
    }
    /* lwIP's own thread answers the link: just above normal. */
    if (prio > 0) SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(h);
    return (sys_thread_t)h;
}
