// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* lwIP's threads, semaphores, mutexes and mailboxes on Win32 (issue #109).
 * Each is a pointer to an object in sys_arch.c; NULL is "invalid". */

typedef struct lares_sem *sys_sem_t;
typedef struct lares_mutex *sys_mutex_t;
typedef struct lares_mbox *sys_mbox_t;
typedef void *sys_thread_t;
typedef unsigned long sys_prot_t;

#define sys_sem_valid(s)            ((s) != NULL && *(s) != NULL)
#define sys_sem_set_invalid(s)      do { if ((s) != NULL) *(s) = NULL; } while (0)
#define sys_mutex_valid(m)          ((m) != NULL && *(m) != NULL)
#define sys_mutex_set_invalid(m)    do { if ((m) != NULL) *(m) = NULL; } while (0)
#define sys_mbox_valid(b)           ((b) != NULL && *(b) != NULL)
#define sys_mbox_set_invalid(b)     do { if ((b) != NULL) *(b) = NULL; } while (0)
