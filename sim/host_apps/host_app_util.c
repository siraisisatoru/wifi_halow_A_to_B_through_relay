/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "host_app_util.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

volatile bool host_app_stop;

static pthread_mutex_t group_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t group_cond = PTHREAD_COND_INITIALIZER;
static unsigned group_running;

struct spawn_ctx
{
    lab_task_fn_t fn;
    void *arg;
};

static void on_signal(int sig)
{
    (void)sig;
    host_app_stop = true;
}

static void usage(const char *prog, const struct host_opt *opts, unsigned n_opts)
{
    fprintf(stderr, "usage: %s [--option value]...\n", prog);
    for (unsigned i = 0; i < n_opts; i++)
    {
        const char *kind = opts[i].type == HOST_OPT_U32 ? "N" :
                           opts[i].type == HOST_OPT_STR ? "STR" :
                                                          "0|1";
        fprintf(stderr, "  --%-22s %-4s %s\n", opts[i].name, kind, opts[i].help);
    }
}

static int parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || v > UINT32_MAX || s[0] == '-')
    {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

int host_app_parse(int argc, char **argv, const char *prog, const struct host_opt *opts,
                   unsigned n_opts)
{
    for (int i = 1; i < argc; i++)
    {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)
        {
            usage(prog, opts, n_opts);
            return -1;
        }
        if (strncmp(arg, "--", 2) != 0 || i + 1 >= argc)
        {
            fprintf(stderr, "%s: bad or incomplete option '%s'\n", prog, arg);
            usage(prog, opts, n_opts);
            return -1;
        }
        const char *val = argv[++i];

        const struct host_opt *o = NULL;
        for (unsigned k = 0; k < n_opts; k++)
        {
            if (strcmp(arg + 2, opts[k].name) == 0)
            {
                o = &opts[k];
                break;
            }
        }
        if (o == NULL)
        {
            fprintf(stderr, "%s: unknown option '%s'\n", prog, arg);
            usage(prog, opts, n_opts);
            return -1;
        }

        uint32_t v = 0;
        switch (o->type)
        {
        case HOST_OPT_STR:
            *(const char **)o->dst = val;
            break;
        case HOST_OPT_U32:
        case HOST_OPT_BOOL:
            if (parse_u32(val, &v) != 0 || (o->type == HOST_OPT_BOOL && v > 1))
            {
                fprintf(stderr, "%s: bad value for %s: '%s'\n", prog, arg, val);
                return -1;
            }
            if (o->type == HOST_OPT_U32)
            {
                *(uint32_t *)o->dst = v;
            }
            else
            {
                *(bool *)o->dst = v != 0;
            }
            break;
        }
    }
    return 0;
}

void host_app_init(void)
{
    lab_platform_init();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static void spawn_trampoline(void *p)
{
    struct spawn_ctx ctx = *(struct spawn_ctx *)p;
    free(p);
    ctx.fn(ctx.arg);

    pthread_mutex_lock(&group_lock);
    group_running--;
    pthread_cond_broadcast(&group_cond);
    pthread_mutex_unlock(&group_lock);
}

int host_app_spawn(lab_task_fn_t fn, void *arg, const char *name)
{
    struct spawn_ctx *ctx = (struct spawn_ctx *)malloc(sizeof(*ctx));
    if (ctx == NULL)
    {
        return -1;
    }
    ctx->fn = fn;
    ctx->arg = arg;

    pthread_mutex_lock(&group_lock);
    group_running++;
    pthread_mutex_unlock(&group_lock);

    /* Same stack/priority as the firmware tasks; ignored by the POSIX port. */
    if (lab_task_create(spawn_trampoline, ctx, name, 6 * 1024, LAB_TASK_PRIO_NORMAL) != 0)
    {
        pthread_mutex_lock(&group_lock);
        group_running--;
        pthread_mutex_unlock(&group_lock);
        free(ctx);
        return -1;
    }
    return 0;
}

void host_app_wait(uint32_t duration_ms)
{
    uint64_t deadline_us = duration_ms != 0 ? lab_time_us() + (uint64_t)duration_ms * 1000U : 0;

    pthread_mutex_lock(&group_lock);
    while (group_running > 0)
    {
        if (deadline_us != 0 && lab_time_us() >= deadline_us)
        {
            host_app_stop = true;
            deadline_us = 0;
        }
        /* Short timed waits so signals and the deadline are noticed promptly. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000 * 1000;
        if (ts.tv_nsec >= 1000 * 1000 * 1000)
        {
            ts.tv_sec++;
            ts.tv_nsec -= 1000 * 1000 * 1000;
        }
        pthread_cond_timedwait(&group_cond, &group_lock, &ts);
    }
    pthread_mutex_unlock(&group_lock);
}
