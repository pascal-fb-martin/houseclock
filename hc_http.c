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
#include "hc_broadcast.h"
#include "hc_clock.h"
#include "hc_ntp.h"
#include "hc_http.h"

#include "echttp.h"
#include "echttp_static.h"

static pid_t parent;


static hc_nmea_status *nmea_db = 0;
static hc_ntp_status *ntp_db = 0;

static char JsonBuffer[8192];

static void hc_background (int fd, int mode) {
    if (kill (parent, 0) < 0) exit(0);
}

static void *hc_http_attach (const char *name) {
    void *p = hc_db_get (name);
    if (p == 0) {
        fprintf (stderr, "Cannot attach to %s\n", name);
        echttp_error (503, "Service Temporarily Unavailable");
    }
    return p;
}

static void *hc_http_attach_nmea (void) {

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
}

static void *hc_http_attach_ntp (void) {

    if (ntp_db == 0) {
        ntp_db = (hc_ntp_status *) hc_http_attach (HC_NTP_STATUS);
        if (ntp_db == 0) return "";
        if (hc_db_get_count (HC_NTP_STATUS) != 1
            || hc_db_get_size (HC_NTP_STATUS) != sizeof(hc_ntp_status)) {
            fprintf (stderr, "wrong data structure for table %s\n",
                     HC_NTP_STATUS);
            exit (1);
        }
    }
}

static const char *hc_http_status (const char *method, const char *uri,
                                   const char *data, int length) {
    static hc_clock_status *clock_db = 0;

    char latitude[20];
    char longitude[20];
    const char *date = "010100";

    hc_http_attach_nmea();

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

    hc_http_attach_ntp();

    // This conversion is not made when decoding the NMEA stream to avoid
    // consuming CPU in the high-priority time synchronization process.
    // If the GPS position information is not set, or if the client is not
    // from a local network, report the position of Greenwich.
    //
    if (echttp_islocal() == 0 ||
            nmea_db->latitude[0] == 0 || nmea_db->longitude[0] == 0) {
        strncpy (latitude, "0.0", sizeof(latitude));
        strncpy (longitude, "0.0", sizeof(longitude));
    } else {
        hc_nmea_convert (latitude, sizeof(latitude),
                         nmea_db->latitude, nmea_db->hemisphere[0]);
        hc_nmea_convert (longitude, sizeof(longitude),
                         nmea_db->longitude, nmea_db->hemisphere[1]);
    }

    if (nmea_db->gpsdate[0] > 0) date = nmea_db->gpsdate;

    snprintf (JsonBuffer, sizeof(JsonBuffer),
              "{\"gps\":{\"fix\":%s, \"fixtime\":%u"
              ",\"gpstime\":\"%s\",\"gpsdate\":\"%4d%2.2s%2.2s\""
              ",\"latitude\":%s,\"longitude\":%s}"
              ",\"clock\":{\"synchronized\":%s,\"reference\":%zd.%03d"
              ",\"precision\":%d,\"drift\":%d,\"avgdrift\":%d"
              ",\"timestamp\":%zd.%03d}"
              ",\"learn\":{\"count\":%d,\"accumulator\":%d}"
              ",\"ntp\":{\"mode\":\"%c\",\"received\":%d,\"processed\":%d"
              ",\"broadcast\":%d}"
              ",\"mem\":{\"space\":%d,\"used\":%d}}",
              nmea_db->fix?"true":"false",
              (unsigned int)nmea_db->fixtime,
              nmea_db->gpstime,
              2000 + (date[4]-'0')*10 + (date[5]-'0'), date+2, date,
              latitude, longitude,
              clock_db->synchronized?"true":"false",
              (size_t)clock_db->reference.tv_sec,
              clock_db->reference.tv_usec/1000,
              clock_db->precision,
              clock_db->drift,
              clock_db->avgdrift,
              (size_t) (clock_db->timestamp.tv_sec),
              clock_db->timestamp.tv_usec/1000,
              clock_db->count, clock_db->accumulator,
              ntp_db->mode,
              ntp_db->latest.received,
              ntp_db->latest.client,
              ntp_db->latest.broadcast,
              hc_db_get_space(), hc_db_get_used());

    echttp_content_type_json();
    return JsonBuffer;
}

static const char *hc_http_gps (const char *method, const char *uri,
                                const char *data, int length) {
    const char *prefix = "";
    int i;
    char buffer[1024];

    hc_http_attach_nmea();

    strncpy (JsonBuffer, "{\"text\":[\"", sizeof(JsonBuffer));
    for (i = 0; i < nmea_db->textcount; ++i) {
        strcat (JsonBuffer, prefix);
        strcat (JsonBuffer, nmea_db->text[i].line);
        prefix = "\",\"";
    }
    strcat (JsonBuffer, "\"]");

    prefix = ",\"history\":[{\"sentence\":\"";
    for (i = 0; i < HC_NMEA_DEPTH; ++i) {
        gpsSentence *item = nmea_db->history + i;
        if (item->timing.tv_sec == 0) continue;
        snprintf (buffer, sizeof(buffer),
                  "%s%s\",\"timestamp\":[%u,%d],\"flags\":%d}",
                  prefix,
                  item->sentence,
                  item->timing.tv_sec, item->timing.tv_usec / 1000,
                  item->flags);
        strcat (JsonBuffer, buffer);
        prefix = ",{\"sentence\":\"";
    }
    strcat (JsonBuffer, "]}");

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

static const char *hc_http_ntp (const char *method, const char *uri,
                                const char *data, int length) {

    int i;
    const char *source;
    const char *quote = "\"";
    char buffer[1024];
    const char *prefix = "";

    hc_http_attach_ntp();

    if (ntp_db->stratum == 1) {
        source = "GPS";
    } else if (ntp_db->source >= 0) {
        source = ntp_db->pool[ntp_db->source].name;
    } else {
        source = "null";
        quote = "";
    }

    snprintf (JsonBuffer, sizeof(JsonBuffer),
              "{\"ntp\":{\"source\":%s%s%s,\"mode\":\"%c\",\"stratum\":%d",
              quote, source, quote, ntp_db->mode, ntp_db->stratum);

    prefix = ",\"clients\":[";
    for (i = 0; i < HC_NTP_DEPTH; ++i) {
        int delta;
        struct hc_ntp_client *client = ntp_db->clients + i;

        if (client->local.tv_sec == 0) continue;
        delta = ((client->local.tv_sec - client->origin.tv_sec) * 1000)
                + ((client->local.tv_usec - client->origin.tv_usec) / 1000);
        snprintf (buffer, sizeof(buffer),
           "%s{\"address\":\"%s\",\"timestamp\":%d.%03d,\"delta\":%d}",
           prefix,
           hc_broadcast_format(&(client->address)),
           client->local.tv_sec, client->local.tv_usec / 1000, delta);
        strcat (JsonBuffer, buffer);
        prefix = ",";
    }
    if (prefix[1] == 0) strcat(JsonBuffer, "]");

    prefix = ",\"servers\":[";
    for (i = 0; i < HC_NTP_POOL; ++i) {
        int delta;
        struct hc_ntp_server *server = ntp_db->pool + i;

        if (server->local.tv_sec == 0) continue;
        delta = ((server->local.tv_sec - server->origin.tv_sec) * 1000)
                + ((server->local.tv_usec - server->origin.tv_usec) / 1000);
        snprintf (buffer, sizeof(buffer),
           "%s{\"address\":\"%s\",\"timestamp\":%d.%03d,"
               "\"delta\":%d,\"stratum\":%d}",
           prefix,
           server->name,
           server->local.tv_sec,
           server->local.tv_usec / 1000, delta, server->stratum);
        strcat (JsonBuffer, buffer);
        prefix = ",";
    }
    if (prefix[1] == 0) strcat(JsonBuffer, "]");
    strcat (JsonBuffer, "}}");

    echttp_content_type_json();
    return JsonBuffer;
}

const char *hc_http_help (int level) {
    return echttp_help(level);
}

void hc_http (int argc, const char **argv) {
    parent = getppid();
    if (echttp_open (argc, argv) <= 0) exit(1);

    echttp_route_uri ("/status", hc_http_status);
    echttp_route_uri ("/clock/drift", hc_http_clockdrift);
    echttp_route_uri ("/gps", hc_http_gps);
    echttp_route_uri ("/ntp", hc_http_ntp);
    echttp_static_route ("/ui", "/usr/local/lib/houseclock/public");
    echttp_background (&hc_background);
    echttp_loop();
    exit (0);
}

