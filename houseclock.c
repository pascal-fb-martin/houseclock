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
 * hc_main.c - Main loop of the houseclock program.
 *
 * SYNOPSYS:
 *
 * int hc_match (const char *reference, const char *input, char **value)
 *
 *   Decode the value found in the input after the reference string, if
 *   the input matches the reference string. Otherwise just return 0.
 *
 *   This is used to retrieve values from command line arguments.
 *
 * int hc_present (const char *reference, const char *input)
 *
 *   Return true if the option matches the reference string. Used for
 *   boolean options.
 *
 * int hc_debug_enabled (void)
 *
 *   Return true if debug mode option (-debug) was enabled. Mostly used
 *   in the definition of DEBUG.
 */

#include <stdlib.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#include "houseclock.h"
#include "hc_db.h"
#include "hc_clock.h"
#include "hc_nmea.h"
#include "hc_ntp.h"
#include "hc_broadcast.h"

#include "hc_http.h"


int hc_match (const char *reference,
              const char *input, const char **value) {

    size_t length = strlen(reference);

    if (strncmp (reference, input, length)) return 0;

    if (value) *value = input + length;
    return 1;
}

int hc_present (const char *reference, const char *input) {
    return strcmp (reference, input) == 0;
}

static int HcDebug = 0;
static int HcTest = 0;

int hc_debug_enabled (void) {
    return HcDebug;
}

int hc_test_mode (void) {
    return HcTest;
}

static void hc_help (const char *argv0) {

    int i = 1;
    const char *help;

    printf ("%s [-h] [-debug] [-test]%s%s%s\n",
            argv0, hc_ntp_help(0), hc_nmea_help(0), hc_http_help(0));

    printf ("\nGeneral options:\n");
    printf ("   -h:              print this help.\n");
    printf ("   -debug           prints a lot of debug traces.\n");
    printf ("   -test            prints time drift compare to GPS.\n");
    printf ("   -db=N            Size of the internal database, in MB\n");

    printf ("\nNTP options:\n");
    help = hc_ntp_help(i=1);
    while (help) {
        printf ("   %s\n", help);
        help = hc_ntp_help(++i);
    }
    printf ("\nGPS options:\n");
    help = hc_nmea_help(i=1);
    while (help) {
        printf ("   %s\n", help);
        help = hc_nmea_help(++i);
    }
    printf ("\nHTTP options:\n");
    help = hc_http_help(i=1);
    while (help) {
        printf ("   %s\n", help);
        help = hc_http_help(++i);
    }
    exit (0);
}

int main (int argc, const char **argv) {

    int maxfd = 1;
    int count;
    int ntpsocket;

    int gpstty = -1;

    time_t last_period = 0;
    struct timeval now;
    const char *dbsizestr = "0";

    // These strange statements are to make sure that fds 0 to 2 are
    // reserved, since this application might output some errors.
    // 3 descriptors are wasted if 0, 1 and 2 are already open. No big deal.
    //
    open ("/dev/null", O_RDONLY);
    dup(open ("/dev/null", O_WRONLY));

    int i;
    for (i = 1; i < argc; ++i) {
        if (strcmp("-h", argv[i]) == 0) {
            hc_help(argv[0]);
        }
        hc_match ("-db=", argv[i], &dbsizestr);
        if (hc_present ("-debug", argv[i])) HcDebug = 1;
        if (hc_present ("-test", argv[i])) HcTest = 1;
    }

    // Start the web interface.
    hc_db_create (atoi(dbsizestr)*1024*1024);
    pid_t httpid = fork();
    if (httpid == 0) {
        nice (19); // The HTTP server is low priority.
        hc_http (argc, argv);
    }
    if (httpid < 0) {
        fprintf (stderr, "Cannot fork: %s\n", strerror (errno));
        exit (1);
    }

    nice (-20); // The NTP server is high priority.

    hc_clock_initialize (argc, argv);

    gpstty = hc_nmea_initialize (argc, argv);
    if (maxfd <= gpstty) maxfd = gpstty + 1;

    ntpsocket = hc_ntp_initialize (argc, argv);
    if (!HcTest) {
        if (ntpsocket < 0) return 1;
    }
    if (maxfd <= ntpsocket) maxfd = ntpsocket + 1;


    putenv("TZ=UTC"); // Always use UTC time.

    for (;;) {
        struct timeval timeout;
        fd_set readset;

        FD_ZERO(&readset);
        if (ntpsocket > 0) {
            FD_SET(ntpsocket, &readset);
        }

        if (gpstty >= 0) {
            FD_SET(gpstty, &readset);
        }

        gettimeofday(&now, NULL);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        count = select(maxfd+1, &readset, NULL, NULL, &timeout);
        gettimeofday(&now, NULL);

        if (count >= 0) {
            if (gpstty > 0) {
                if (FD_ISSET(gpstty, &readset)) {
                   gpstty = hc_nmea_process (&now);
                }
            }
            if (ntpsocket) {
                if (FD_ISSET(ntpsocket, &readset)) {
                    hc_ntp_process (&now);
                }
            }
        }

        if (now.tv_sec > last_period) {
            if (ntpsocket > 0) {
                hc_ntp_periodic (&now);
            }
            if (gpstty < 0) {
                gpstty = hc_nmea_initialize (argc, argv);
                if (maxfd <= gpstty) maxfd = gpstty + 1;
            } else {
                hc_nmea_periodic (&now);
            }
            last_period = now.tv_sec;

            int wstatus;
            if (waitpid (httpid, &wstatus, WNOHANG) == httpid) {
                exit(1);
            }
        }
    }
}

