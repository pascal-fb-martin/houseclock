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
}

static int hc_metrics_aggregate (time_t cursor, time_t end,
                                 long long *offset, long long *adjust) {

    // Collect the data for the specified period.
    // The data must be aggregated on the basis of the sampling rate,
    // which varies depending on the time source (GPS of another NTP server).
    //
    int sampling = clock_db->sampling;
    if (sampling <= 0) return 0;

    int count = 0;
    int subcount = 0;
    int offset_accumulator = 0;
    int adjust_accumulator = 0;
    while (cursor < end) {
        int index = (int) (cursor % clock_metrics_count);
        offset_accumulator += abs(clock_metrics_db[index].drift);
        adjust_accumulator += clock_metrics_db[index].adjust;
        cursor += 1;
        if (++subcount >= sampling) {
            offset[count] = offset_accumulator;
            adjust[count] = adjust_accumulator;
            count += 1;
            subcount = offset_accumulator = adjust_accumulator = 0;
        }
    }
    if (subcount > 0) {
        offset[count] = offset_accumulator;
        adjust[count] = adjust_accumulator;
        count += 1; // Include last item.
    }
    return count;
}

int hc_metrics_status (char *buffer, int size, const char *host, time_t now) {

    if (! hc_metrics_attach_clock()) return 0;

    time_t reference = now - 1; // Avoid the current second: still counting.
    reference -= (reference % 300); // Aligned on a 5 minutes period.

    int cursor;
    cursor = snprintf (buffer, size,
                       "{\"host\":\"%s\","
                            "\"timestamp\":%lld,\"metrics\":{\"period\":300"
                            ",\"sampling\":%d,\"clock\":",
                       host, clock_db->sampling, (long long)reference);
    int start = cursor;

    long long offset[300];
    long long adjust[300];
    int count = hc_metrics_aggregate (reference - 300, reference, offset, adjust);
    if (count <= 0) return 0; // No data to report.

    // Now that we have our final metrics for that 5 minutes period, lets
    // reduce it and report.
    cursor += echttp_reduce_json (buffer+cursor, size-cursor,
                                  "offset", offset, count, "ms");
    if (cursor >= size) return 0;

    cursor += echttp_reduce_json (buffer+cursor, size-cursor,
                                  "adjust", adjust, count, "");
    if (cursor >= size) return 0;
    if (cursor <= start) return 0; // No data to report.

    buffer[start] = '{';
    cursor += snprintf (buffer+cursor, size-cursor, "}}}");
    if (cursor >= size) return 0;

    return cursor;
}

int hc_metrics_details (char *buffer, int size,
                        const char *host, time_t now, time_t since) {

    if (! hc_metrics_attach_clock()) return 0;

    int sampling = clock_db->sampling;
    time_t reference = now - 1; // Avoid the current second: still counting.
    reference -= (reference % sampling); // Aligned on the sampling period.

    if (since < reference - 300) since = reference - 300;

    int cursor;
    cursor = snprintf (buffer, size,
                       "{\"host\":\"%s\","
                            "\"timestamp\":%lld,\"Metrics\":{\"period\":300"
                            ",\"sampling\":%d,\"clock\":",
                       host, sampling, (long long)reference);
    int start = cursor;

    time_t timestamp[300];
    long long offset[300];
    long long adjust[300];
    int count = hc_metrics_aggregate (since, reference, offset, adjust);
    if (count <= 0) return 0; // No data to report.

    int i;
    for (i = 0; i < count; ++i) timestamp[i] = since + i;

    // Now that we have our final metrics for that 5 minutes period, lets
    // reduce it and report.
    cursor += echttp_reduce_details_json (buffer+cursor, size-cursor, since,
                                          "offset", "ms", reference,
                                          sampling, count, timestamp, offset);
    if (cursor >= size) return 0;

    cursor += echttp_reduce_details_json (buffer+cursor, size-cursor, since,
                                          "adjust", "", reference,
                                          sampling, count, timestamp, adjust);
    if (cursor >= size) return 0;
    if (cursor <= start) return 0; // No data to report.

    buffer[start] = '{';
    cursor += snprintf (buffer+cursor, size-cursor, "}}}");
    if (cursor >= size) return 0;

    return cursor;
}

