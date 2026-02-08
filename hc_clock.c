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
 * hc_clock.c - The module that controls the system clock synchronization.
 *
 * This module interface with the OS to correct the indicated drift.
 *
 * SYNOPSYS:
 *
 * const char *hc_clock_help (int level)
 *
 *    Prints the help information, two levels:
 *      0: short help for one-line argument information.
 *      1: multi-line description of each argument.
 *
 * void hc_clock_initialize (int argc, const char **argv)
 *
 *    Reset the clock synchronization status, retrieve and store the clock
 *    options from the program's command line arguments.
 *
 *    The command line options processed here are:
 *      -precision=<N>  The clock accuracy target for synchronization (ms).
 *      -drift          Print the measured drift (debug)
 *
 * void hc_clock_synchronize(const struct timeval *source,
 *                           const struct timeval *local, int latency);
 *
 *    Called to synchronize the local time based on a source clock.
 *    The local time parameter represents an estimate of the exact moment
 *    when the source time was received. The latency represents an estimate
 *    of the transmission delay, i.e. the delta between the moment the
 *    time was sampled at the source and the moment when it was received
 *    by this machine. This function calculates a drift between the two
 *    times and corrects the local time if needed.
 *
 * int hc_clock_synchronized (void)
 *
 *    Return 1 when the local system time was synchronized with
 *    the source clock.
 *
 * void hc_clock_reference  (struct timeval *reference);
 * int  hc_clock_dispersion (void);
 *
 *    These two functions are intended for supporting the NTP module.
 *    The reference time is the time of the latest clock adjustment.
 *    The dispersion is the average drift from the source clock
 *    for the latest period. (This does not use the maximum drift
 *    because this is too influenced by the OS response time, which
 *    is unrelated to the accuracy of the local clock.)
 */

#include <time.h>
#include <errno.h>

#include "houseclock.h"
#include "hc_clock.h"
#include "hc_db.h"

#define HC_CLOCK_LEARNING_PERIOD 10

static int clockShowDrift = 0;

// Allocate enough spaces in the metrics table for 6 minutes worth of data,
// which allows some time to gather the previous 5 minutes statistics.
//
#define HC_CLOCK_METRICS_DEPTH 360

static hc_clock_status *hc_clock_status_db = 0;
static hc_clock_metrics *hc_clock_metrics_db = 0;


const char *hc_clock_help (int level) {

    static const char *clockHelp[] = {
        " [-drift] [-precision=N]",
        "-drift        Print the measured drift (test mode).",
        "-precision=N: precision of the time synchronization in milliseconds.",
        NULL
    };
    return clockHelp[level];
}

static void hc_clock_start_learning (const struct timeval *local) {
    hc_clock_status_db->count = 0;
    hc_clock_status_db->accumulator = 0;
    hc_clock_status_db->cycle = *local;
}

void hc_clock_initialize (int argc, const char **argv) {

    static hc_clock_metrics CleanMetrics = {0, 0};

    int i;
    int precision;
    const char *precision_option = "10"; // ms

    clockShowDrift = 0;

    for (i = 1; i < argc; ++i) {
        echttp_option_match ("-precision=", argv[i], &precision_option);
        clockShowDrift |= echttp_option_present ("-drift", argv[i]);
    }
    precision = atoi(precision_option);

    i = hc_db_new (HC_CLOCK_METRICS,
                   sizeof(hc_clock_metrics), HC_CLOCK_METRICS_DEPTH);
    if (i != 0) {
        fprintf (stderr, "[%s %d] cannot create %s: %s\n",
                 __FILE__, __LINE__, HC_CLOCK_METRICS, strerror(i));
        exit (1);
    }
    hc_clock_metrics_db = (hc_clock_metrics *) hc_db_get (HC_CLOCK_METRICS);
    for (i = HC_CLOCK_METRICS_DEPTH - 1; i >= 0; --i) {
        hc_clock_metrics_db[i] = CleanMetrics;
    }

    i = hc_db_new (HC_CLOCK_STATUS, sizeof(hc_clock_status), 1);
    if (i != 0) {
        fprintf (stderr, "[%s %d] cannot create %s: %s\n",
                 __FILE__, __LINE__, HC_CLOCK_STATUS, strerror(i));
        exit (1);
    }
    hc_clock_status_db = (hc_clock_status *)hc_db_get (HC_CLOCK_STATUS);
    hc_clock_status_db->synchronized = 0;
    hc_clock_status_db->precision = precision;
    hc_clock_status_db->sampling = 0;
    hc_clock_status_db->drift = 0;

    struct timeval now;
    gettimeofday (&now, NULL);
    hc_clock_start_learning (&now);
}

static void hc_clock_force (const struct timeval *source,
                            const struct timeval *local, int latency) {

    struct timeval now;
    struct timeval corrected = *source;

    gettimeofday (&now, NULL);

    // Correct the source time to adjust for the time spent since it was
    // acquired, as estimated using the local clock (now).
    //
    corrected.tv_sec += (now.tv_sec - local->tv_sec);
    corrected.tv_usec += (now.tv_usec - local->tv_usec) + (latency * 1000);
    if (corrected.tv_usec > 1000000) {
        corrected.tv_sec += 1;
        corrected.tv_usec -= 1000000;
    } else if (corrected.tv_usec < 0) {
        corrected.tv_sec -= 1;
        corrected.tv_usec += 1000000;
    }

    DEBUG {
        printf ("Forcing time from %lld.%03d to %lld.%03d, "
                    "based on source clock %lld.%03d & latency %d\n",
                (long long)(now.tv_sec), (int)(now.tv_usec/1000),
                (long long)(corrected.tv_sec), (int)(corrected.tv_usec/1000),
                (long long)(source->tv_sec), (int)(source->tv_usec/1000), latency);
    }
    if (settimeofday (&corrected, NULL) != 0) {
        printf ("settimeofday() error %d\n", errno);
        return;
    }
    DEBUG {
       gettimeofday (&corrected, 0);
       printf ("Time set to %lld.%03d\n",
               (long long)(corrected.tv_sec), (int)(corrected.tv_usec/1000));
    }
    hc_clock_status_db->reference = corrected;
    hc_clock_status_db->synchronized = 1;
}

static void hc_clock_adjust (time_t drift) {

    struct timeval delta;

    delta.tv_sec = (drift / 1000);
    delta.tv_usec = (drift % 1000) * 1000;
    if (delta.tv_usec < 0) {
        // Per the GNU libc documentation, tv_usec must be positive, and
        // microsecond time = (tv_sec * 1000000) + tv_usec.
        delta.tv_sec -= 1;
        delta.tv_usec += 1000000;
    }
    if (adjtime (&delta, NULL) != 0) {
        printf ("adjtime() error %d\n", errno);
    }
    gettimeofday (&hc_clock_status_db->reference, NULL);
}

// Cleanup outdated metrics. It would be bad to increment these forever.
// This resets metrics that were skipped due to the synchronization period,
// plus the metrics for the current second, when it is a "new" time slot.
//
static void hc_clock_cleanup_metrics (time_t now) {

    static time_t lastCleanup = 0;
    if (!lastCleanup) lastCleanup = now; // First call.

    while (lastCleanup < now) {
        lastCleanup += 1;
        int index = lastCleanup % HC_CLOCK_METRICS_DEPTH;
        hc_clock_metrics_db[index].adjust = 0;
        hc_clock_metrics_db[index].drift = 0;
    }
}

void hc_clock_synchronize(const struct timeval *source,
                          const struct timeval *local, int latency) {

    static int CallPeriod = 0;
    static int CallCount = 0;
    static time_t LatestCall = 0;

    if (hc_clock_metrics_db == 0) return;
    if (hc_clock_status_db == 0) return;

    time_t now = time(0);
    hc_clock_cleanup_metrics (now);

    time_t previous_call = LatestCall;
    LatestCall = now;

    if (previous_call) {
        // Estimate the synchronization period by calculating an average that
        // is rounded to the closest integer. That gives the sampling rate for
        // the metrics recorded in this function.
        //
        // In order to limit the losses that come with integer divisions,
        // the code increments the count by 10 (instead of 1) and multiply
        // the accumulated period by 100, and finally round the average
        // result according to the low order digit left (which is then
        // eliminated).
        //
        // In other words, the code takes care to always keep a one digit
        // fraction to maintain some precision and allow for the final
        // rounding.
        //
        int period = (int) (now - previous_call) * 100;
        if (CallPeriod >= 20000) {
            // Avoid calculation overflow and lingering effect when the
            // synchronization period changes. Note that a limit of 200
            // seconds means that we readjust every 100 seconds, since
            // the adjustment resets to 100 seconds (not 0).
            CallCount /= 2;
            CallPeriod /= 2;
        }
        CallPeriod += period;
        CallCount += 10;
        int average = CallPeriod / CallCount;
        if (average < 10) average = 1; // Don't get 0.
        else if (average % 10 >= 5) average = (average / 10) + 1;
        else average = (average / 10);
        hc_clock_status_db->sampling = average;
    }

    time_t drift = ((source->tv_sec - local->tv_sec) * 1000)
                 + ((source->tv_usec - local->tv_usec) / 1000) + latency;

    time_t absdrift = (drift < 0)? (0 - drift) : drift;

    hc_clock_metrics_db[now%HC_CLOCK_METRICS_DEPTH].drift = (int)drift;
    hc_clock_status_db->drift = (int)drift;

    if (clockShowDrift || hc_test_mode()) {
        printf ("[%d] %8.3f\n",
                (int)(local->tv_sec%HC_CLOCK_METRICS_DEPTH), drift/1000.0);
        if (hc_test_mode()) {
            if (absdrift < hc_clock_status_db->precision) {
                hc_clock_status_db->synchronized = 1;
            } else {
                hc_clock_status_db->synchronized = 0;
            }
            return;
        }
    }

    if ((!previous_call) || absdrift >= 10000) {
        // Too much of a difference: force system time.
        hc_clock_metrics_db[now%HC_CLOCK_METRICS_DEPTH].adjust += 1;
        hc_clock_force (source, local, latency);
        hc_clock_start_learning(source);
        return;
    }

    // Accumulate an average drift, to eliminate one-time issues.
    // (Do this only if the latency is greater than 0: this indicates
    // a local clock source, sensitive to OS delays.)
    //
    hc_clock_status_db->accumulator += (int)drift;
    hc_clock_status_db->count += 1;
    if ((latency > 0) &&
        (hc_clock_status_db->count < HC_CLOCK_LEARNING_PERIOD)) return;

    // We reached the end of a learning period.
    // At this point we consider only the average drift
    // calculated over the past learning period.
    //
    drift = hc_clock_status_db->accumulator / hc_clock_status_db->count;
    absdrift = (drift < 0)? (0 - drift) : drift;
    hc_clock_status_db->avgdrift = (int)drift;
    if (clockShowDrift)
        printf ("Average drift: %lld ms\n", (long long)drift);

    if (absdrift < hc_clock_status_db->precision) {
        DEBUG printf ("Clock is synchronized.\n");
        hc_clock_status_db->synchronized = 1;
    } else {
        // Source and local system time have drifted apart
        // by a small difference: adjust the time progressively.
        //
        DEBUG {
            printf ("Time adjust at %lld.%03d (local), drift=%lld ms\n",
                    (long long)(local->tv_sec),
                    (int)(local->tv_usec/1000), (long long)drift);
        }
        if (absdrift > 50 * hc_clock_status_db->precision) {
            DEBUG printf ("Synchronization was lost.\n");
            hc_clock_status_db->synchronized = 0; // Lost it, for now.
        }
        hc_clock_metrics_db[now%HC_CLOCK_METRICS_DEPTH].adjust += 1;
        hc_clock_adjust (drift);
    }
    hc_clock_start_learning(local);
}

int hc_clock_synchronized (void) {
    if (hc_clock_status_db == 0) return 0;
    return hc_clock_status_db->synchronized;
}

void hc_clock_reference (struct timeval *reference) {
    static struct timeval zero = {0, 0};
    if (hc_clock_status_db == 0) {
        *reference = zero;
        return;
    }
    *reference = hc_clock_status_db->reference;
}

int hc_clock_dispersion (void) {
    int drift;
    if (hc_clock_status_db == 0) return 0;
    drift = (int)(hc_clock_status_db->avgdrift);
    if (drift < 0) return 0 - drift;
    return drift;
}

