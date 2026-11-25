/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200809L
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lab_platform.h"

static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rand_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t rand_state;

void lab_platform_init(void)
{
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
}

uint64_t lab_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void lab_sleep_ms(uint32_t ms)
{
    struct timespec req = { .tv_sec = (time_t)(ms / 1000U),
                            .tv_nsec = (long)(ms % 1000U) * 1000000L };
    while (nanosleep(&req, &req) != 0 && errno == EINTR)
    {
    }
}

void lab_out_line(const char *line)
{
    pthread_mutex_lock(&out_lock);
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    pthread_mutex_unlock(&out_lock);
}

void lab_platform_log_write(enum lab_log_level level, const char *msg)
{
    static const char tags[] = { 'E', 'W', 'I', 'D' };
    char tag = (unsigned)level < sizeof(tags) ? tags[level] : '?';

    pthread_mutex_lock(&out_lock);
    fprintf(stderr, "%c lab: %s\n", tag, msg);
    pthread_mutex_unlock(&out_lock);
}

static void rand_seed(void)
{
    uint64_t seed = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0)
    {
        if (read(fd, &seed, sizeof(seed)) != (ssize_t)sizeof(seed))
        {
            seed = 0;
        }
        close(fd);
    }
    seed ^= lab_time_us() ^ ((uint64_t)getpid() << 32);
    rand_state = seed != 0 ? seed : 0x9E3779B97F4A7C15ULL;
}

uint32_t lab_random_u32(void)
{
    pthread_mutex_lock(&rand_lock);
    if (rand_state == 0)
    {
        rand_seed();
    }
    /* xorshift64* */
    rand_state ^= rand_state >> 12;
    rand_state ^= rand_state << 25;
    rand_state ^= rand_state >> 27;
    uint64_t v = rand_state * 2685821657736338717ULL;
    pthread_mutex_unlock(&rand_lock);
    return (uint32_t)(v >> 32);
}

void *lab_malloc(size_t size)
{
    return malloc(size);
}

void lab_free(void *p)
{
    free(p);
}

struct task_start
{
    lab_task_fn_t fn;
    void *arg;
};

static void *task_trampoline(void *p)
{
    struct task_start start = *(struct task_start *)p;
    free(p);
    start.fn(start.arg);
    return NULL;
}

int lab_task_create(lab_task_fn_t fn,
                    void *arg,
                    const char *name,
                    uint32_t stack_bytes,
                    enum lab_task_prio prio)
{
    (void)name;
    (void)stack_bytes;
    (void)prio;

    struct task_start *start = (struct task_start *)malloc(sizeof(*start));
    if (start == NULL)
    {
        return -1;
    }
    start->fn = fn;
    start->arg = arg;

    pthread_t th;
    if (pthread_create(&th, NULL, task_trampoline, start) != 0)
    {
        free(start);
        return -1;
    }
    pthread_detach(th);
    return 0;
}
