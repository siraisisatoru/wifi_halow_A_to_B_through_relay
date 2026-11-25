/*
 * Copyright 2026 siraisisatoru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * Shared plumbing for the simulation host apps: option parsing, signal-driven stop flag and a
 * task group that waits for lab_task_create() tasks to finish.
 */

#ifndef HOST_APP_UTIL_H
#define HOST_APP_UTIL_H

#include <stdbool.h>
#include <stdint.h>

#include "lab_platform.h"

enum host_opt_type
{
    HOST_OPT_U32,
    HOST_OPT_STR,
    HOST_OPT_BOOL,
};

struct host_opt
{
    /** Option name without the leading "--". */
    const char *name;
    enum host_opt_type type;
    /** uint32_t *, const char ** or bool *. */
    void *dst;
    const char *help;
};

/** Set by SIGINT/SIGTERM (and by host_app_wait() when a duration elapses). */
extern volatile bool host_app_stop;

/**
 * Parse "--name value" pairs into @p opts. Prints usage and returns -1 on error or --help.
 * Defaults must already be stored in the destinations.
 */
int host_app_parse(int argc, char **argv, const char *prog, const struct host_opt *opts,
                   unsigned n_opts);

/** lab_platform_init() plus SIGINT/SIGTERM handlers that set host_app_stop. */
void host_app_init(void);

/** Start @p fn as a lab task that the group counts until it returns. */
int host_app_spawn(lab_task_fn_t fn, void *arg, const char *name);

/**
 * Block until every spawned task returned. When @p duration_ms is non-zero, set host_app_stop
 * after that long (tasks then finish their final reports).
 */
void host_app_wait(uint32_t duration_ms);

#endif /* HOST_APP_UTIL_H */
