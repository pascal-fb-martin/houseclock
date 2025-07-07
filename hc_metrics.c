/* houseclock - A simple GPS Time Server with Web console
 *
 * Copyright 2019, Pascal Martin
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 *
 * hc_metrics.c - Publish houseclock's time synchronization metrics.
 *
 * SYNOPSYS:
 *
 * void hc_metrics_initialize (int argc, const char **argv);
 *
 *    Setup the initial context.
 *
 * int hc_metrics_status (char *buffer, int size, const char *host, time_t now);
 *
 *    Return the latest metrics in JSON format.
 *
 * int hc_metrics_details (char *buffer, int size,
 *                         const char *host, time_t now, time_t since);
 *
 *    Return the full latest metrics in JSON format.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include "echttp_reduce.h"

#include "houseclock.h"
#include "hc_db.h"
#include "hc_clock.h"

static hc_clock_status *clock_db = 0;
static hc_clock_metrics *clock_metrics_db = 0;
static int clock_metrics_count;

static time_t StartupTime = 0;

#define TRACE DEBUG printf

#define METRICS_STATUS_DEPTH 300

static void *hc_metrics_attach (const char *name) {
    void *p = hc_db_get (name);
    if (p == 0) {
        fprintf (stderr, "Cannot attach to %s\n", name);
        echttp_error (503, "Service Temporarily Unavailable");
    }
    return p;
}

static int hc_metrics_attach_clock (void) {

    if (clock_db == 0) {
        clock_db = (hc_clock_status *) hc_metrics_attach (HC_CLOCK_STATUS);
        if (clock_db == 0) return 0;
        if (hc_db_get_count (HC_CLOCK_STATUS) != 1
            || hc_db_get_size (HC_CLOCK_STATUS) != sizeof(hc_clock_status)) {
            fprintf (stderr, "[%s %d] wrong data structure for table %s\n",
                     __FILE__, __LINE__, HC_CLOCK_STATUS);
            exit (1);
        }
    }

    if (clock_metrics_db == 0) {
        clock_metrics_db = (hc_clock_metrics *) hc_metrics_attach (HC_CLOCK_METRICS);
        if (clock_metrics_db == 0) return 0;
        clock_metrics_count = hc_db_get_count (HC_CLOCK_METRICS);
        if (hc_db_get_size (HC_CLOCK_METRICS) != sizeof(hc_clock_metrics)) {
            fprintf (stderr, "[%s %d] wrong data structure for table %s\n",
                     __FILE__, __LINE__, HC_CLOCK_METRICS);
            exit (1);
        }
    }
    return 1;
}

void hc_metrics_initialize (int argc, const char **argv) {
    StartupTime = time(0);
}

// The raw clock metrics are sparse (index of 1 second vs. variable sampling
// rate) and need to be aggregated into a compact form.
//
static void hc_metrics_aggregate (time_t since, time_t cursor,
                                  long long *offset, long long *adjust) {

    // Collect the data for the specified period.
    // The data must be aggregated on the basis of the sampling rate,
    // which varies depending on the time source (GPS of another NTP server).
    //
    int sampling = clock_db->sampling;

    int subcount = 0;
    int offset_accumulator = 0;
    int adjust_accumulator = 0;
    int destination = (int) ((cursor / sampling) % METRICS_STATUS_DEPTH);
    while (cursor > since) {
        int source = (int) (cursor % clock_metrics_count);
        offset_accumulator += abs(clock_metrics_db[source].drift);
        adjust_accumulator += clock_metrics_db[source].adjust;
        cursor -= 1;
        if (++subcount >= sampling) {
TRACE ("hc_metrics_aggregate: accumulated offset = %d, adjust = %d at source index %d, destination %d\n", offset_accumulator, adjust_accumulator, source, destination);
            offset[destination] = offset_accumulator;
            adjust[destination] = adjust_accumulator;
            subcount = offset_accumulator = adjust_accumulator = 0;
            destination = (int) ((cursor / sampling) % METRICS_STATUS_DEPTH);
        }
    }
    if (subcount > 0) {
        offset[destination] = offset_accumulator;
        adjust[destination] = adjust_accumulator;
    }
}

int hc_metrics_status (char *buffer, int size, const char *host, time_t now) {

    if (! hc_metrics_attach_clock()) return 0;
    if (clock_db->sampling <= 0) return 0;

    time_t reference = now - 1; // Avoid the current second: still counting.
    reference -= (reference % METRICS_STATUS_DEPTH); // Aligned.
    time_t origin = reference - METRICS_STATUS_DEPTH;
    if (origin < StartupTime) return 0; // Too early.

    int cursor;
    cursor = snprintf (buffer, size,
                       "{\"host\":\"%s\",\"timestamp\":%lld,\"metrics\":{"
                            "\"period\":%d,\"sampling\":%d,\"clock\":",
                       host, (long long)reference,
                       METRICS_STATUS_DEPTH, clock_db->sampling);
    int start = cursor;

    long long offset[METRICS_STATUS_DEPTH];
    long long adjust[METRICS_STATUS_DEPTH];
    hc_metrics_aggregate (origin, reference, offset, adjust);

    // Now that the final metrics for the reporting period are ready,
    // lets reduce and report.
    cursor += echttp_reduce_json (buffer+cursor, size-cursor,
                                  "offset", offset, METRICS_STATUS_DEPTH, "ms");
    if (cursor >= size) return 0;

    cursor += echttp_reduce_json (buffer+cursor, size-cursor,
                                  "adjust", adjust, METRICS_STATUS_DEPTH, "");
    if (cursor >= size) return 0;
    if (cursor <= start) return 0; // No data to report.

    buffer[start] = '{';
    cursor += snprintf (buffer+cursor, size-cursor, "}}}");
    if (cursor >= size) return 0;

    return cursor;
}

int hc_metrics_details (char *buffer, int size,
                        const char *host, time_t now, time_t since) {

TRACE ("hc_metrics_details: request now = %lld, since = %lld (start = %lld)\n", (long long)now, (long long)since, (long long)StartupTime);
    if (! hc_metrics_attach_clock()) return 0;

    int sampling = clock_db->sampling;
    if (sampling <= 0) return 0;

    time_t reference = now - 1; // Avoid the current second: still counting.
    reference -= (reference % sampling); // Aligned on the sampling period.
TRACE ("hc_metrics_details: reference = %lld (index %d)\n", (long long)reference, (int) (reference % clock_metrics_count));
    time_t origin = reference - METRICS_STATUS_DEPTH;

    // Never collect more than available.
    //
    if (since < origin) since = origin;
    if (since < StartupTime) since = StartupTime;

TRACE ("hc_metrics_details: since = %lld (index %d)\n", (long long)since, (int) (since % clock_metrics_count));
    if (since >= reference) return 0; // Final consistency check.

    int cursor;
    cursor = snprintf (buffer, size,
                       "{\"host\":\"%s\",\"timestamp\":%lld,\"Metrics\":{"
                            "\"period\":%d,\"sampling\":%d,\"clock\":",
                       host, (long long)reference,
                       METRICS_STATUS_DEPTH, sampling);
    int start = cursor;

    // Always aggregate the whole period, even if asked for less.
    // The JSON generation will skip the values that are too old.
    time_t timestamp[METRICS_STATUS_DEPTH];
    long long offset[METRICS_STATUS_DEPTH];
    long long adjust[METRICS_STATUS_DEPTH];
    hc_metrics_aggregate (origin, reference, offset, adjust);

    int index;
    time_t value;
    memset (timestamp, 0, sizeof(timestamp));
    for (value = reference; value > origin; value -= sampling) {
        index = (value / sampling) % METRICS_STATUS_DEPTH;
        timestamp[index] = value;
    }

    // Now that the final metrics for that reporting period are ready,
    // lets reduce and report.
    cursor += echttp_reduce_details_json (buffer+cursor, size-cursor, since,
                                          "offset", "ms", reference,
                                          sampling, METRICS_STATUS_DEPTH,
                                          timestamp, offset);
    if (cursor >= size) return 0;

    cursor += echttp_reduce_details_json (buffer+cursor, size-cursor, since,
                                          "adjust", "", reference,
                                          sampling, METRICS_STATUS_DEPTH,
                                          timestamp, adjust);
    if (cursor >= size) return 0;
    if (cursor <= start) return 0; // No data to report.

    buffer[start] = '{';
    cursor += snprintf (buffer+cursor, size-cursor, "}}}");
    if (cursor >= size) return 0;
TRACE ("hc_metrics_details: result = %s\n", buffer);

    return cursor;
}

