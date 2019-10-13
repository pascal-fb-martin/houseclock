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
#include "hc_http.h"

#include "echttp.h"
#include "echttp_static.h"

static pid_t parent;


static void hc_background (int fd, int mode) {
    if (kill (parent, 0) < 0) exit(0);
}

static const char *hc_http_status (const char *method, const char *uri,
                                   const char *data, int length) {
    static char buffer[1024];
    static hc_nmea_status *status_db = 0;
    char latitude[20];
    char longitude[20];

    if (status_db == 0) {
        if (hc_db_get_count (HC_NMEA_STATUS) != 1
            || hc_db_get_size (HC_NMEA_STATUS) != sizeof(hc_nmea_status)) {
            fprintf (stderr, "wrong data structure for table %s\n",
                     HC_NMEA_STATUS);
            exit (1);
        }
        status_db = (hc_nmea_status *) hc_db_get (HC_NMEA_STATUS);
    }

    // This conversion is not made when decoding the NMEA stream to avoid
    // consuming CPU in the high-priority time synchronization process.
    //
    hc_nmea_convert (latitude, sizeof(latitude),
                     status_db->latitude, status_db->hemisphere[0]);
    hc_nmea_convert (longitude, sizeof(longitude),
                     status_db->longitude, status_db->hemisphere[1]);

    snprintf (buffer, sizeof(buffer),
              "{\"gps\":{\"fix\":%s,\"time\":\"%s\",\"date\":\"%s\""
              ",\"latitude\":%s,\"longitude\":%s}}",
              status_db->fix?"true":"false",
              status_db->time, status_db->date,
              latitude, longitude);
    echttp_content_type_json();
    return buffer;
}

void hc_http (int argc, const char **argv) {
    parent = getppid();
    if (echttp_open (argc, argv) <= 0) exit(1);

    echttp_route_uri ("/status", hc_http_status);
    echttp_static_route ("/ui", "/usr/local/lib/houseclock/public");
    echttp_background (&hc_background);
    echttp_loop();
    exit (0);
}

