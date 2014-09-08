/*
 * Copyright 2003-2013 Jeffrey K. Hollingsworth
 *
 * This file is part of Active Harmony.
 *
 * Active Harmony is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Active Harmony is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Active Harmony.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \page cache Point Caching/Replay layer (cache.so)
 *
 * This processing layer records point/performance pairs in a local
 * cache as they are reported by clients.  If the strategy later
 * generates any points that exist in the cache, this layer will
 * return the associated recorded performance immediately.  Note that
 * any outer layers (include the Harmony server and client) will not
 * be notified upon cache hit.
 *
 * The cache may optionally be initialized by a log file generated
 * from the [Point Logger](\ref logger).
 *
 * **Configuration Variables**
 * Key          | Type   | Default | Description
 * ------------ | ------ | ------- | -----------
 * CACHE_FILE   | String | (none)  | Log file generated by the Point Logger.
 */

#include "session-core.h"
#include "hsignature.h"
#include "hpoint.h"
#include "hperf.h"
#include "hutil.h"
#include "defaults.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

const char harmony_layer_name[] = "cache";

typedef struct {
    hpoint_t point;
    hperf_t *perf;
} cache_t;

int find_max_strlen(void);
int load_logger_file(const char *logger);
int safe_scanstr(FILE *fd, int bounds_idx, const char **match);
int pt_equiv(const hpoint_t *a, const hpoint_t *b);

static hrange_t *range;
static cache_t *cache;
static int cache_len, cache_cap;
static int skip;
static int i_cnt;
static int o_cnt;
static char *buf;
static int buflen;

/* Initialize global variables.  Also loads data into cache from a log
 * file if configuration variable CACHE_FILE is defined.
 */
int cache_init(hsignature_t *sig)
{
    const char *filename;

    i_cnt = sig->range_len;
    o_cnt = atoi( session_getcfg(CFGKEY_PERF_COUNT) );
    range = sig->range;

    cache = NULL;
    cache_len = cache_cap = 0;

    filename = session_getcfg("CACHE_FILE");
    if (filename) {
        buflen = find_max_strlen() + 1;
        buf = malloc(sizeof(char) * buflen);
        if (!buf) {
            session_error("Could not allocate memory for string buffer");
            return -1;
        }

        if (load_logger_file(filename) != 0)
            return -1;

        free(buf);
    }

    return 0;
}

/* Generate function: look up point in array.
 * Sets status to HFLOW_RETURN with trial's performance set to retrieved value
 * if the point is found.
 * Otherwise, sets status to HFLOW_ACCEPT (pass point on in plugin workflow)
 */
int cache_generate(hflow_t *flow, htrial_t *trial)
{
    int i;

    /* For now, we rely on a linear cache lookup. */
    for (i = 0; i < cache_len; ++i) {
        if (pt_equiv(&trial->point, &cache[i].point)) {
            hperf_copy(trial->perf, cache[i].perf);
            flow->status = HFLOW_RETURN;
            skip = 1;
            return 0;
        }
    }

    flow->status = HFLOW_ACCEPT;
    return 0;
}

/* Analyze each trial as it passes through the system.  Add the
 * observed point/performance pair to the cache unless it was a result
 * of a cache hit.
 */
int cache_analyze(hflow_t *flow, htrial_t *trial)
{
    if (!skip) {
        if (cache_len == cache_cap) {
            if (array_grow(&cache, &cache_cap, sizeof(cache_t)) != 0) {
                session_error("Could not allocate more memory for cache");
                return -1;
            }
        }

        hpoint_init(&cache[cache_len].point, trial->point.n);
        hpoint_copy(&cache[cache_len].point, &trial->point);
        cache[cache_len].perf = hperf_clone(trial->perf);
        ++cache_len;
    }
    skip = 0;

    flow->status = HFLOW_ACCEPT;
    return 0;
}

int cache_fini(void)
{
    int i;

    for (i = 0; i < cache_len; ++i) {
        hpoint_fini(&cache[i].point);
        hperf_fini(cache[i].perf);
    }
    free(cache);

    return 0;
}

/* Search the parameter space for any HVAL_STR dimensions, and return
 * the length of the largest possible string.
 */
int find_max_strlen(void)
{
    int i, j, len, max = 0;

    for (i = 0; i < i_cnt; ++i) {
        if (range[i].type == HVAL_STR) {
            for (j = 0; j < range[i].bounds.s.set_len; ++j) {
                len = strlen(range[i].bounds.s.set[j]) + 1;
                if (max < len)
                    max = len;
            }
        }
    }
    return max;
}

/* Initialize the in-memory cache using a log file generated from the
 * logger layer during a prior tuning session.
 *
 * Note: This function must be kept in sync with the output routines
 *       of the logger layer.
 */
int load_logger_file(const char *filename)
{
    char c;
    int i, ret;
    FILE *fp;

    fp = fopen(filename, "r");
    if (!fp) {
        session_error("Could not open log file.");
        return -1;
    }

    while (fscanf(fp, " %c", &c) != EOF) {
        /* Skip any line that doesn't start with 'P'. */
        if (c != 'P') {
            fscanf(fp, "%*[^\n] ");
            continue;
        }
        fscanf(fp, "oint #%*d: ( ");

        /* Prepare a new point in the memory cache. */
        if (cache_len == cache_cap) {
            if (array_grow(&cache, &cache_cap, sizeof(cache_t)) != 0) {
                session_error("Could not allocate more memory for cache");
                return -1;
            }
        }
        hpoint_init(&cache[cache_len].point, i_cnt);
        cache[cache_len].perf = hperf_alloc(o_cnt);
        if (!cache[cache_len].perf) {
            session_error("Error allocating memory for performance in cache");
            return -1;
        }

        /* Parse point data. */
        for (i = 0; i < i_cnt; ++i) {
            hval_t *v = &cache[cache_len].point.val[i];

            if (i > 0) fscanf(fp, " ,");

            v->type = range[i].type;
            switch (range[i].type) {
            case HVAL_INT:  ret = fscanf(fp, "%ld", &v->value.i); break;
            case HVAL_REAL: ret = fscanf(fp, "%*f[%la]", &v->value.r); break;
            case HVAL_STR:  ret = safe_scanstr(fp, i, &v->value.s); break;
            default:
                session_error("Invalid point value type");
                return -1;
            }

            if (ret != 1) {
                session_error("Error parsing point data from logfile");
                return -1;
            }
        }

        /* Parse performance data. */
        fscanf(fp, " ) => (");
        for (i = 0; i < o_cnt; ++i) {
            if (i > 0) fscanf(fp, " ,");
            if (fscanf(fp, " %*f[%la]", &cache[cache_len].perf->p[i]) != 1) {
                session_error("Error parsing performance data from logfile");
                return -1;
            }
        }

        /* Discard the rest of the line after the right parenthesis. */
        if (fscanf(fp, " %c%*[^\n]", &c) != 1 || c != ')') {
            session_error("Error parsing point data from logfile");
            return -1;
        }
        ++cache_len;
    }
    fclose(fp);
    return 0;
}

/* Parse a double-quoted string value from a file stream.  Static
 * variables buf and buflen must be initialized prior to use.
 *
 * Return values designed to match scanf family.
 */
int safe_scanstr(FILE *fp, int bounds_idx, const char **match)
{
    int i;
    str_bounds_t *str_bounds = &range[bounds_idx].bounds.s;

    fscanf(fp, " \"");
    for (i = 0; i < buflen; ++i) {
        int c = fgetc(fp);

        if (c == '\\')
            c = fgetc(fp);
        if (c == '\"' || c == EOF)
            break;

        buf[i] = c;
    }
    if (i == sizeof(buf)) {
        session_error("Input HVAL_STR overrun");
        return EOF;
    }
    buf[i] = '\0';

    for (i = 0; i < str_bounds->set_len; ++i) {
        if (strcmp(buf, str_bounds->set[i]) == 0)
            break;
    }
    if (i == str_bounds->set_len) {
        session_error("Invalid HVAL_STR value");
        return 0;
    }

    *match = str_bounds->set[i];
    return 1;
}

int pt_equiv(const hpoint_t *a, const hpoint_t *b)
{
    int i;

    if (a->n != b->n)
        return 0;

    for (i = 0; i < a->n; ++i) {
        if (a->val[i].type != b->val[i].type)
            return 0;

        switch (a->val[i].type) {
        case HVAL_INT:
            if (a->val[i].value.i != b->val[i].value.i)
                return 0;
            break;

        case HVAL_REAL:
            if (a->val[i].value.r < b->val[i].value.r ||
                a->val[i].value.r > b->val[i].value.r)
                return 0;
            break;

        case HVAL_STR:
            if (strcmp(a->val[i].value.s, b->val[i].value.s))
                return 0;
            break;

        default:
            session_error("Invalid point value type");
            return 0;
        }
    }
    return 1;
}
