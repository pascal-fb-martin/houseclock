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
 * hc_http.c - The houseclock's HTTP server.
 *
 * SYNOPSYS:
 *
 * void hc_http (int argc, const char **argv);
 *
 *    Run the HTTP server until exit is requested, or the parent dies.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>

#include "houseclock.h"
#include "hc_db.h"
#include "hc_nmea.h"
#include "hc_clock.h"
#include "hc_http.h"

#include "echttp.h"
#include "echttp_static.h"

static pid_t parent;


static char JsonBuffer[8192];

static void hc_background (int fd, int mode) {
    if (kill (parent, 0) < 0) exit(0);
}

static void *hc_http_attach (const char *name) {
    void *p = hc_db_get (name);
    if (p == 0) echttp_error (503, "Service Temporarily Unavailable");
    return p;
}

static const char *hc_http_status (const char *method, const char *uri,
                                   const char *data, int length) {
    static hc_nmea_status *nmea_db = 0;
    static hc_clock_status *clock_db = 0;
    char latitude[20];
    char longitude[20];

    if (nmea_db == 0) {
        nmea_db = (hc_nmea_status *) hc_http_attach (HC_NMEA_STATUS);
        if (nmea_db == 0) return "";
        if (hc_db_get_count (HC_NMEA_STATUS) != 1
            || hc_db_get_size (HC_NMEA_STATUS) != sizeof(hc_nmea_status)) {
            fprintf (stderr, "wrong data structure for table %s\n",
                     HC_NMEA_STATUS);
            exit (1);
        }
    }

    if (clock_db == 0) {
        clock_db = (hc_clock_status *) hc_http_attach (HC_CLOCK_STATUS);
        if (clock_db == 0) return "";
        if (hc_db_get_count (HC_CLOCK_STATUS) != 1
            || hc_db_get_size (HC_CLOCK_STATUS) != sizeof(hc_clock_status)) {
            fprintf (stderr, "wrong data structure for table %s\n",
                     HC_CLOCK_STATUS);
            exit (1);
        }
    }

    // This conversion is not made when decoding the NMEA stream to avoid
    // consuming CPU in the high-priority time synchronization process.
    //
    hc_nmea_convert (latitude, sizeof(latitude),
                     nmea_db->latitude, nmea_db->hemisphere[0]);
    hc_nmea_convert (longitude, sizeof(longitude),
                     nmea_db->longitude, nmea_db->hemisphere[1]);

    snprintf (JsonBuffer, sizeof(JsonBuffer),
              "{\"gps\":{\"fix\":%s"
              ",\"time\":[%c%c,%c%c,%c%c],\"date\":[%d,%c%c,%c%c]"
              ",\"latitude\":%s,\"longitude\":%s}"
              ",\"clock\":{\"synchronized\":%s"
              ",\"precision\":%d,\"drift\":%d,\"timestamp\":%zd.%03d}}",
              nmea_db->fix?"true":"false",
              nmea_db->time[0], nmea_db->time[1],
              nmea_db->time[2], nmea_db->time[3],
              nmea_db->time[4], nmea_db->time[5],
              2000 + (nmea_db->date[4]-'0')*10 + (nmea_db->date[5]-'0'),
              nmea_db->date[2], nmea_db->date[3],
              nmea_db->date[0], nmea_db->date[1],
              latitude, longitude,
              clock_db->synchronized?"true":"false",
              clock_db->precision,
              clock_db->drift,
              (size_t) (clock_db->timestamp.tv_sec),
              clock_db->timestamp.tv_usec/1000);
    echttp_content_type_json();
    return JsonBuffer;
}

static const char *hc_http_clockdrift (const char *method, const char *uri,
                                       const char *data, int length) {
    static int *drift_db = 0;
    static int drift_count;

    if (drift_db == 0) {
        drift_db = (int *) hc_http_attach (HC_CLOCK_DRIFT);
        if (drift_db == 0) return "";
        drift_count = hc_db_get_count (HC_CLOCK_DRIFT);
        if (hc_db_get_size (HC_CLOCK_DRIFT) != sizeof(int)) {
            fprintf (stderr, "wrong data structure for table %s\n",
                     HC_CLOCK_DRIFT);
            exit (1);
        }
    }

    snprintf (JsonBuffer, sizeof(JsonBuffer),
              "{\"clock\":{\"drift\":[%d", drift_db[0]);

    int room = sizeof(JsonBuffer) - strlen(JsonBuffer);
    char *p = JsonBuffer + (sizeof(JsonBuffer)-room);
    int i;
    for (i = 1; i < drift_count; ++i) {
        snprintf (p, room, ",%d", drift_db[i]);
        room -= strlen(p);
        p = JsonBuffer + (sizeof(JsonBuffer)-room);
    }
    snprintf (p, room, "%s", "]}}");
    echttp_content_type_json();
    return JsonBuffer;
}

void hc_http (int argc, const char **argv) {
    parent = getppid();
    if (echttp_open (argc, argv) <= 0) exit(1);

    echttp_route_uri ("/status", hc_http_status);
    echttp_route_uri ("/clock/drift", hc_http_clockdrift);
    echttp_static_route ("/ui", "/usr/local/lib/houseclock/public");
    echttp_background (&hc_background);
    echttp_loop();
    exit (0);
}

