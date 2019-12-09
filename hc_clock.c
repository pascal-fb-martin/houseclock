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

#define HC_CLOCK_DRIFT_DEPTH 120
static hc_clock_status *hc_clock_status_db = 0;
static int *hc_clock_drift_db = 0;


const char *hc_clock_help (int level) {

    static const char *clockHelp[] = {
        " [-drift] [-precision=N]",
        "-drift        Print the measured drift (test mode).\n"
        "-precision=N: precision of the time synchronization in milliseconds.",
        NULL
    };
    return clockHelp[level];
}

static void hc_clock_start_learning (void) {
    hc_clock_status_db->count = 0;
    hc_clock_status_db->accumulator = 0;
}

void hc_clock_initialize (int argc, const char **argv) {

    int i;
    int precision;
    const char *precision_option = "10"; // ms

    clockShowDrift = 0;

    for (i = 1; i < argc; ++i) {
        hc_match ("-precision=", argv[i], &precision_option);
        clockShowDrift |= hc_present ("-drift", argv[i]);
    }
    precision = atoi(precision_option);

    i = hc_db_new (HC_CLOCK_DRIFT, sizeof(int), HC_CLOCK_DRIFT_DEPTH);
    if (i != 0) {
        fprintf (stderr, "cannot create %s: %s\n", HC_CLOCK_DRIFT, strerror(i));
        exit (1);
    }
    hc_clock_drift_db = (int *) hc_db_get (HC_CLOCK_DRIFT);
    for (i = 0; i < HC_CLOCK_DRIFT_DEPTH; ++i) hc_clock_drift_db[i] = 0;

    i = hc_db_new (HC_CLOCK_STATUS, sizeof(hc_clock_status), 1);
    if (i != 0) {
        fprintf (stderr,
                 "cannot create %s: %s\n", HC_CLOCK_STATUS, strerror(i));
        exit (1);
    }
    hc_clock_status_db = (hc_clock_status *)hc_db_get (HC_CLOCK_STATUS);
    hc_clock_status_db->synchronized = 0;
    hc_clock_status_db->precision = precision;
    hc_clock_status_db->drift = 0;
    hc_clock_start_learning ();
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
        printf ("Forcing time from %ld.%03.3d to %ld.%03.3d, "
                    "based on source clock %ld.%03.3d & latency %d\n",
                (long)(now.tv_sec), (int)(now.tv_usec/1000),
                (long)(corrected.tv_sec), (int)(corrected.tv_usec/1000),
                (long)(source->tv_sec), (int)(source->tv_usec/1000), latency);
    }
    if (settimeofday (&corrected, NULL) != 0) {
        printf ("settimeofday() error %d\n", errno);
        return;
    }
    DEBUG {
       gettimeofday (&corrected, 0);
       printf ("Time set to %ld.%03.3d\n",
               (long)(corrected.tv_sec), (int)(corrected.tv_usec/1000));
    }
    hc_clock_status_db->reference = corrected;
    hc_clock_status_db->synchronized = 1;
}

static void hc_clock_adjust (time_t drift) {

    struct timeval delta;

    delta.tv_sec = (drift / 1000);
    delta.tv_usec = (drift % 1000) * 1000;
    if (adjtime (&delta, NULL) != 0) {
        printf ("adjtime() error %d\n", errno);
    }
    gettimeofday (&hc_clock_status_db->reference, NULL);
}

void hc_clock_synchronize(const struct timeval *source,
                          const struct timeval *local, int latency) {

    static int FirstCall = 1;

    if (hc_clock_drift_db == 0) return;
    if (hc_clock_status_db == 0) return;

    time_t drift = ((source->tv_sec - local->tv_sec) * 1000)
                 + ((source->tv_usec - local->tv_usec) / 1000) + latency;

    time_t absdrift = (drift < 0)? (0 - drift) : drift;

    hc_clock_drift_db[source->tv_sec%HC_CLOCK_DRIFT_DEPTH] = (int)drift;
    hc_clock_status_db->drift = (int)drift;
    hc_clock_status_db->timestamp = *local;

    if (clockShowDrift || hc_test_mode()) {
        printf ("[%d] %8.3f\n",
                local->tv_sec%HC_CLOCK_DRIFT_DEPTH, drift/1000.0);
        if (hc_test_mode()) {
            if (absdrift < hc_clock_status_db->precision) {
                hc_clock_status_db->synchronized = 1;
            } else {
                hc_clock_status_db->synchronized = 0;
            }
            return;
        }
    }

    if (FirstCall || absdrift >= 10000) {
        // Too much of a difference: force system time.
        hc_clock_force (source, local, latency);
        hc_clock_start_learning();
        FirstCall = 0;
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
        printf ("Average drift: %d ms\n", drift);

    if (absdrift < hc_clock_status_db->precision) {
        DEBUG printf ("Clock is synchronized.\n");
        hc_clock_status_db->synchronized = 1;
    } else {
        // Source and local system time have drifted apart
        // by a small difference: adjust the time progressively.
        //
        DEBUG {
            printf ("Time adjust at %ld.%3.3d (local), drift=%d ms\n",
                    (long)local->tv_sec, (int)local->tv_usec/1000, drift);
        }
        if (absdrift > 50 * hc_clock_status_db->precision) {
            DEBUG printf ("Synchronization was lost.\n");
            hc_clock_status_db->synchronized = 0; // Lost it, for now.
        }
        hc_clock_adjust (drift);
    }
    hc_clock_start_learning();
}

int hc_clock_synchronized (void) {
    if (hc_clock_status_db == 0) return 0;
    return hc_clock_status_db->synchronized;
}

void hc_clock_reference (struct timeval *reference) {
    if (hc_clock_status_db == 0) return;
    *reference = hc_clock_status_db->reference;
}

int hc_clock_dispersion (void) {
    int drift;
    if (hc_clock_status_db == 0) return 0;
    drift = (int)(hc_clock_status_db->avgdrift);
    if (drift < 0) return 0 - drift;
    return drift;
}

