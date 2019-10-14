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
 * void hc_clock_synchronize(const struct timeval *gps,
 *                           const struct timeval *local, int latency);
 *
 *    Called when a significant drift was detected, based on GPS time.
 *    Take the gps time matching the "local" time, calculate a drift
 *    that consider the stated latency and correct the local time if
 *    needed.
 *
 * int hc_clock_synchronized (void)
 *
 *    Return 1 when the local system time was synchronized with GPS time.
 */

#include <time.h>

#include "houseclock.h"
#include "hc_clock.h"
#include "hc_db.h"

static int clockPrecision;
static int clockShowDrift = 0;

static int clockSynchronized = 0;

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

void hc_clock_initialize (int argc, const char **argv) {

    int i;
    const char *precision_option = "50"; // ms

    clockShowDrift = 0;

    for (i = 1; i < argc; ++i) {
        hc_match ("-precision=", argv[i], &precision_option);
        clockShowDrift |= hc_present ("-drift", argv[i]);
    }
    clockPrecision = atoi(precision_option);

    clockSynchronized = 0;

    i = hc_db_new (HC_CLOCK_DRIFT, sizeof(int), 120);
    if (i != 0) {
        fprintf (stderr, "cannot create %s: %s\n", HC_CLOCK_DRIFT, strerror(i));
        exit (1);
    }
    hc_clock_drift_db = (int *) hc_db_get (HC_CLOCK_DRIFT);
    for (i = 0; i < 120; ++i) hc_clock_drift_db[i] = 0;
}

void hc_clock_synchronize(const struct timeval *gps,
                          const struct timeval *local, int latency) {

    time_t drift = ((gps->tv_sec - local->tv_sec) * 1000)
                 + ((gps->tv_usec - local->tv_usec) / 1000) + latency;

    time_t absdrift = (drift < 0)? (0 - drift) : drift;

    if (hc_clock_drift_db) hc_clock_drift_db[local->tv_sec%120] = (int)drift;

    if (clockShowDrift || hc_test_mode()) {
        printf ("[%d]=%8.3f\n", local->tv_sec%120, drift/1000.0);
        return;
    }

    if (absdrift < clockPrecision) {
        clockSynchronized = 1;
        return;
    }

    // GPS and local system time have drifted apart.

    DEBUG {
        printf ("Detected drift of %s%ld.%03.3d seconds\n",
                (drift < 0)?"-":"",
                (long)(absdrift/1000), (int)(absdrift%1000));
        printf ("   GPS = %ld.%3.3d, System = %ld.%3.3d\n",
                gps->tv_sec, gps->tv_usec, local->tv_sec, local->tv_usec);
    }

    if (absdrift >= 10000) {

        // Too much of a difference: force system time.
        //
        struct timeval now;
        struct timeval adjusted = *gps;

        gettimeofday (&now, NULL);

        // Adjust the GMT time from the time of the fix to this
        // current time, as estimated using the local clock.
        //
        adjusted.tv_sec += (now.tv_sec - local->tv_sec);
        adjusted.tv_usec += (now.tv_usec - local->tv_usec);
        if (adjusted.tv_usec > 1000000) {
            adjusted.tv_sec += 1;
            adjusted.tv_usec -= 1000000;
        } else if (adjusted.tv_usec < 0) {
            adjusted.tv_sec -= 1;
            adjusted.tv_usec += 1000000;
        }

        DEBUG {
            printf ("Forcing time to %ld.%03.3d seconds\n",
                    (long)(adjusted.tv_sec), (int)(adjusted.tv_usec/1000));
        }
        settimeofday (&adjusted, NULL);

    } else {

        // Small difference: adjust the time progressively.
        //
        time_t leftover;
        struct timeval delta;

        // What is the current situation in the kernel?
        // We do not want to interrupt an adjustment that
        // is meant to correct the current drift, however
        // if the drift has reversed sign, we need to reverse
        // direction of the adjustment. (Remember that the drift
        // value must be significant to have gotten here.)

        adjtime (NULL, &delta);
        leftover = (delta.tv_sec * 1000) + (delta.tv_usec / 1000);

        if (leftover * drift <= 0) { // Different adjustment
           DEBUG printf ("New adjustment: %d replaces %d\n",
                         drift, leftover);
           delta.tv_sec = (drift / 1000);
           delta.tv_usec = (drift % 1000) * 1000;
           adjtime (&delta, NULL);
        }
    }
}

int hc_clock_synchronized (void) {
    return clockSynchronized;
}

