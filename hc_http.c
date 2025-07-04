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
#include <time.h>

#include "houseclock.h"
#include "hc_db.h"
#include "hc_nmea.h"
#include "hc_broadcast.h"
#include "hc_clock.h"
#include "hc_ntp.h"
#include "hc_http.h"

#include "echttp_cors.h"
#include "echttp_static.h"
#include "houseportalclient.h"
#include "houselog.h"

static pid_t parent;

static long hc_known_clients[256]; // Enough to store IP v4 address.
static long hc_known_servers[256]; // Enough to store IP v4 address.

static hc_clock_status *clock_db = 0;
static hc_nmea_status *nmea_db = 0;
static hc_ntp_status *ntp_db = 0;
static int *drift_db = 0;
static int drift_count;
static int *adjust_db = 0;
static int adjust_count;

static char hc_hostname[256] = {0};

static char JsonBuffer[65536];

static void *hc_http_attach (const char *name) {
    void *p = hc_db_get (name);
    if (p == 0) {
        fprintf (stderr, "Cannot attach to %s\n", name);
        echttp_error (503, "Service Temporarily Unavailable");
    }
    return p;
}

static int hc_http_attach_clock (void) {

    if (clock_db == 0) {
        clock_db = (hc_clock_status *) hc_http_attach (HC_CLOCK_STATUS);
        if (clock_db == 0) return 0;
        if (hc_db_get_count (HC_CLOCK_STATUS) != 1
            || hc_db_get_size (HC_CLOCK_STATUS) != sizeof(hc_clock_status)) {
            fprintf (stderr, "[%s %d] wrong data structure for table %s\n",
                     __FILE__, __LINE__, HC_CLOCK_STATUS);
            exit (1);
        }
    }
    return 1;
}

static int hc_http_attach_drift (void) {

    if (drift_db == 0) {
        drift_db = (int *) hc_http_attach (HC_CLOCK_DRIFT);
        if (drift_db == 0) return 0;
        drift_count = hc_db_get_count (HC_CLOCK_DRIFT);
        if (hc_db_get_size (HC_CLOCK_DRIFT) != sizeof(int)) {
            fprintf (stderr, "[%s %d] wrong data structure for table %s\n",
                     __FILE__, __LINE__, HC_CLOCK_DRIFT);
            exit (1);
        }
    }
    return 1;
}

static int hc_http_attach_adjust (void) {

    if (adjust_db == 0) {
        adjust_db = (int *) hc_http_attach (HC_CLOCK_ADJUST);
        if (adjust_db == 0) return 0;
        adjust_count = hc_db_get_count (HC_CLOCK_ADJUST);
        if (hc_db_get_size (HC_CLOCK_ADJUST) != sizeof(int)) {
            fprintf (stderr, "[%s %d] wrong data structure for table %s\n",
                     __FILE__, __LINE__, HC_CLOCK_ADJUST);
            exit (1);
        }
    }
    return 1;
}

static int hc_http_attach_nmea (void) {

    if (nmea_db == 0) {
        nmea_db = (hc_nmea_status *) hc_http_attach (HC_NMEA_STATUS);
        if (nmea_db == 0) return 0;
        if (hc_db_get_count (HC_NMEA_STATUS) != 1
            || hc_db_get_size (HC_NMEA_STATUS) != sizeof(hc_nmea_status)) {
            fprintf (stderr, "[%s %d] wrong data structure for table %s\n",
                     __FILE__, __LINE__, HC_NMEA_STATUS);
            exit (1);
        }
    }
    return 1;
}

static int hc_http_attach_ntp (void) {

    if (ntp_db == 0) {
        ntp_db = (hc_ntp_status *) hc_http_attach (HC_NTP_STATUS);
        if (ntp_db == 0) return 0;
        if (hc_db_get_count (HC_NTP_STATUS) != 1
            || hc_db_get_size (HC_NTP_STATUS) != sizeof(hc_ntp_status)) {
            fprintf (stderr, "[%s %d] wrong data structure for table %s\n",
                     __FILE__, __LINE__, HC_NTP_STATUS);
            exit (1);
        }
    }
    return 1;
}


static void hc_background (int fd, int mode) {

    static time_t LastParentCheck = 0;
    static time_t LastActivityCheck = 0;
    static time_t LastDriftCheck = 0;

    time_t now = time(0);

    if (now < LastParentCheck) {
        LastParentCheck = 0; // Always check when time changed backward.
    }

    if (now >= LastParentCheck + 3) {
       if (kill (parent, 0) < 0) {
           fprintf (stderr, "[%s %d] Parent disappeared, exit now\n",
                    __FILE__, __LINE__);
           exit(1);
       }
       LastParentCheck = now;
    }

    if (hc_http_attach_ntp() && (now >= LastActivityCheck + 5)) {

        // Generate local events for new or unsynchronized clients.
        // We generate a local "cache" of known clients to limit the number of
        // events generated when the clent is not synchronized. The cache key
        // is the low 7 bits of the IP address, plus the ninth bit: this works
        // best for me because I have two subnets, while I don't have anywhere
        // close to 127 machines at home.
        // This should work fine for most home networks.
        //
        int i;
        for (i = 0; i < HC_NTP_DEPTH; ++i) {
            struct hc_ntp_client *client = ntp_db->clients + i;

            // Do not consider events that are empty or too old (risk of
            // race condition)
            //
            if ((client->local.tv_sec < LastActivityCheck)
                    || (client->local.tv_sec == 0)) continue;

            // Do not consider events that were already detected.
            //
            if (client->logged) continue;

            int delta = (int)(client->origin.tv_sec - client->local.tv_sec);
            const char *unit = "S";
 
            if (abs(delta) >= 600) {
                delta = delta / 60;
                unit = "MN";
            } else if (abs(delta) < 10) {
                long adr = ntohl(client->address.sin_addr.s_addr);
                int  hash = (int) ((adr & 0x7f) | ((adr & 0x100) >> 1));
                if (hc_known_clients[hash] == adr) continue;
                hc_known_clients[hash] = adr;

                delta = (delta * 1000) +
                   ((client->origin.tv_usec - client->local.tv_usec) / 1000);
                unit = "MS";
            }
            houselog_event_local ("CLIENT",
                                  hc_broadcast_format (&(client->address)),
                                  "ACTIVE", "DELTA %d %s", delta, unit);
            client->logged = 1;
        }

        // Generate events for newly detected servers, using a similar cache
        // as for clients to limit the rate of events when synchronized.
        //
        for (i = 0; i < HC_NTP_POOL; ++i) {
            struct hc_ntp_server *server = ntp_db->pool + i;

            // Do not consider events that are empty or too old (risk of
            // race condition)
            //
            if ((server->local.tv_sec < LastActivityCheck)
                    || (server->local.tv_sec == 0)) continue;

            // Do not consider events that were already detected.
            //
            if (server->logged) continue;
 
            int delta = (int)(server->origin.tv_sec - server->local.tv_sec);
            const char *unit = "S";
 
            if (abs(delta) >= 600) {
                delta = delta / 60;
                unit = "MN";
            } else if (abs(delta) < 10) {
                long adr = ntohl(server->address.sin_addr.s_addr);
                int  hash = (int) ((adr & 0x7f) | ((adr & 0x100) >> 1));
                if (hc_known_servers[hash] == adr) continue;
                hc_known_servers[hash] = adr;

                delta = (delta * 1000) +
                   ((server->origin.tv_usec - server->local.tv_usec) / 1000);
                unit = "MS";
            }
            houselog_event ("SERVER", server->name, "ACTIVE",
                            "STRATUM %d, DELTA %d %s", server->stratum, delta, unit);
            server->logged = 1;
        }
        LastActivityCheck = now;
    }

    if (hc_http_attach_drift() && (now >= LastDriftCheck + drift_count)) {
        int i;
        int max = 0;
        static int MaxDriftLogged = 0;

        // Only record the "significant" drift events, or else too many
        // events would be generated.
        //
        for (i = 0; i < drift_count; ++i) {
            if (abs(max) < abs(drift_db[i])) max = drift_db[i];
        }
        if (max >= 10000) {
            if (abs(max) > MaxDriftLogged) {
                houselog_event
                    ("CLOCK", houselog_host(), "DRIFT", "BY %d MS", max);
                MaxDriftLogged = abs(max);
            }
        } else {
            MaxDriftLogged = 0; // That drift was repaired.
        }
        LastDriftCheck = now;
    }

    if (hc_http_attach_nmea()) {
        static int GpsTimeLock = 0;

        if (nmea_db->fix && nmea_db->gpsdate[0] && nmea_db->gpstime[0]) {
            if (!GpsTimeLock) {
                houselog_event
                    ("GPS", nmea_db->gpsdevice, "ACQUIRED", "CLOCK %s %s", nmea_db->gpsdate, nmea_db->gpstime);
                GpsTimeLock = 1;
            }
        } else {
            if (GpsTimeLock) {
                houselog_event
                    ("GPS", nmea_db->gpsdevice, "LOST", "CLOCK");
                GpsTimeLock = 0;
            }
        }
    }
    houseportal_background (now);
    houselog_background (now);
}

static size_t hc_http_status_gps (char *cursor, int size, const char *prefix) {

    char latitude[20];
    char longitude[20];
    const char *date = "010100";

    if (! hc_http_attach_nmea()) return 0;

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

    if (nmea_db->fix) {
       snprintf (cursor, size,
                 "%s\"gps\":{\"fix\":true, \"fixtime\":%u"
                 ",\"gpstime\":\"%s\",\"gpsdate\":\"%4d%2.2s%2.2s\""
                 ",\"latitude\":%s,\"longitude\":%s}",
                 prefix,
                 (unsigned int)nmea_db->fixtime,
                 nmea_db->gpstime,
                 2000 + (date[4]-'0')*10 + (date[5]-'0'), date+2, date,
                 latitude, longitude);
    } else {
       snprintf (cursor, size, "%s\"gps\":{\"fix\":false}", prefix);
    }

    return strlen(cursor);
}

static size_t hc_http_status_time (char *cursor, int size, const char *prefix) {
    if (! hc_http_attach_clock()) return 0;

    snprintf (cursor, size,
              "%s\"time\":{\"synchronized\":%s,\"reference\":%zd.%03d"
              ",\"precision\":%d,\"drift\":%d,\"avgdrift\":%d"
              ",\"cycle\":%zd.%03d}",
              prefix,
              clock_db->synchronized?"true":"false",
              (size_t)clock_db->reference.tv_sec,
              clock_db->reference.tv_usec/1000,
              clock_db->precision,
              clock_db->drift,
              clock_db->avgdrift,
              (size_t) (clock_db->cycle.tv_sec),
              clock_db->cycle.tv_usec/1000);

    return strlen(cursor);
}

static size_t hc_http_status_ntp (char *cursor, int size, const char *prefix) {

    const char *source;
    const char *quote = "\"";

    if (!hc_http_attach_ntp()) return 0;

    if (ntp_db->stratum == 1) {
        source = "GPS";
    } else if (ntp_db->source >= 0) {
        source = ntp_db->pool[ntp_db->source].name;
    } else {
        source = "null";
        quote = "";
    }

    snprintf (cursor, size,
              "%s\"ntp\":{\"source\":%s%s%s,\"mode\":\"%c\",\"stratum\":%d}",
              prefix,
              quote, source, quote,
              ntp_db->mode,
              ntp_db->stratum);

    return strlen(cursor);
}

static const char *hc_http_status (const char *method, const char *uri,
                                   const char *data, int length) {
    char *cursor = JsonBuffer;
    int size = sizeof(JsonBuffer);
    const char *prefix = "";
    int added;

    if (hc_hostname[0] == 0) {
        gethostname (hc_hostname, sizeof(hc_hostname));
    }
    added = snprintf (cursor, size,
                      "{\"host\":\"%s\",\"proxy\":\"%s\",\"timestamp\":%ld,\"clock\":{",
                      hc_hostname, houseportal_server(), (long)time(0));
    if (added > 0) {
        cursor += added;
        size -= added;
    }

    added = hc_http_status_gps(cursor, size, prefix);
    if (added > 0) {
        cursor += added;
        size -= added;
        prefix = ",";
    }

    added = hc_http_status_time(cursor, size, prefix);
    if (added > 0) {
        cursor += added;
        size -= added;
        prefix = ",";
    }

    added = hc_http_status_ntp(cursor, size, prefix);
    if (added > 0) {
        cursor += added;
        size -= added;
        prefix = ",";
    }

    snprintf (cursor, size,
              "%s\"mem\":{\"space\":%d,\"used\":%d}}}",
              prefix, hc_db_get_space(), hc_db_get_used());

    echttp_content_type_json();
    return JsonBuffer;
}

static const char *hc_http_gps (const char *method, const char *uri,
                                const char *data, int length) {
    const char *prefix = "";
    int i;
    int has_sentence = 0;
    char buffer[1024];

    if (! hc_http_attach_nmea()) return "";

    snprintf (JsonBuffer, sizeof(JsonBuffer), "{\"gps\":{\"fix\":%s",
              nmea_db->fix ? "true" : "false");

    if (nmea_db->textcount > 0) {
        prefix = ",\"text\":[\"";
        for (i = 0; i < nmea_db->textcount; ++i) {
            strcat (JsonBuffer, prefix);
            strcat (JsonBuffer, nmea_db->text[i].line);
            prefix = "\",\"";
        }
        strcat (JsonBuffer, "\"]");
    }
    prefix = ",\"history\":[";

    for (i = 0; i < HC_NMEA_DEPTH; ++i) {
        gpsSentence *item = nmea_db->history + i;
        if (item->timing.tv_sec == 0) continue;
        snprintf (buffer, sizeof(buffer),
                  "%s{\"sentence\":\"%s\",\"timestamp\":[%u,%d],\"flags\":%d}",
                  prefix,
                  item->sentence,
                  item->timing.tv_sec, item->timing.tv_usec / 1000,
                  item->flags);
        strcat (JsonBuffer, buffer);
        prefix = ",";
        has_sentence = 1;
    }
    if (has_sentence) strcat (JsonBuffer, "]");
    strcat (JsonBuffer, "}}");

    echttp_content_type_json();
    return JsonBuffer;
}

static const char *hc_http_clockdrift (const char *method, const char *uri,
                                       const char *data, int length) {

    if (! hc_http_attach_drift()) return "";
    if (! hc_http_attach_adjust()) return "";

    int len = snprintf (JsonBuffer, sizeof(JsonBuffer),
                        "{\"timestamp\":%lld,\"clock\":{\"drift\":[%d",
                        (long long)time(0), drift_db[0]);

    int room = sizeof(JsonBuffer) - len;
    char *p = JsonBuffer + len;
    int i;
    for (i = 1; i < drift_count; ++i) {
        len = snprintf (p, room, ",%d", drift_db[i]);
        room -= len;
        p += len;
    }
    len = snprintf (p, room, "],\"adjust\":[%d", adjust_db[0]);
    room -= len;
    p += len;
    for (i = 1; i < adjust_count; ++i) {
        len = snprintf (p, room, ",%d", adjust_db[i]);
        room -= len;
        p += len;
    }
    snprintf (p, room, "%s", "]}}");
    echttp_content_type_json();
    return JsonBuffer;
}

static const char *hc_http_ntp (const char *method, const char *uri,
                                const char *data, int length) {

    int i;
    char buffer[1024];
    const char *prefix = "";

    if (! hc_http_attach_ntp()) return "";

    snprintf (JsonBuffer, sizeof(JsonBuffer),
              "{\"ntp\":{\"mode\":\"%c\"", ntp_db->mode);

    prefix = ",\"clients\":[";
    for (i = 0; i < HC_NTP_DEPTH; ++i) {
        int delta;
        struct hc_ntp_client *client = ntp_db->clients + i;

        if (client->local.tv_sec == 0) continue;
        delta = ((client->origin.tv_sec - client->local.tv_sec) * 1000)
                + ((client->origin.tv_usec - client->local.tv_usec) / 1000);
        snprintf (buffer, sizeof(buffer),
           "%s{\"address\":\"%s\",\"timestamp\":%d.%03d,"
           "\"delta\":%d}",
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
        delta = ((server->origin.tv_sec - server->local.tv_sec) * 1000)
                + ((server->origin.tv_usec - server->local.tv_usec) / 1000);
        snprintf (buffer, sizeof(buffer),
           "%s{\"address\":\"%s\",\"timestamp\":%d.%03d,"
               "\"delta\":%d,\"stratum\":%d}",
           prefix,
           server->name,
           server->local.tv_sec,
           server->local.tv_usec / 1000,
           delta, server->stratum);
        strcat (JsonBuffer, buffer);
        prefix = ",";
    }
    if (prefix[1] == 0) strcat(JsonBuffer, "]");
    strcat (JsonBuffer, "}}");

    echttp_content_type_json();
    return JsonBuffer;
}

static const char *hc_http_traffic (const char *method, const char *uri,
                                    const char *data, int length) {

    int i;
    char buffer[1024];
    const char *prefix;

    if (! hc_http_attach_ntp()) return "";

    snprintf (JsonBuffer, sizeof(JsonBuffer),
              "{\"ntp\":{\"mode\":\"%c\"", ntp_db->mode);

    prefix = ",\"traffic\":[";
    for (i = 0; i < HC_NTP_DEPTH; ++i) {
        struct hc_ntp_traffic *sample = ntp_db->history + i;

        if (sample->timestamp == 0) continue;

        snprintf (buffer, sizeof(buffer),
           "%s{\"timestamp\":%d,"
           "\"received\":%d,"
           "\"client\":%d,"
           "\"broadcast\":%d}",
           prefix, sample->timestamp,
           sample->received, sample->client, sample->broadcast);
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

static void hc_protect (const char *method, const char *uri) {
    echttp_cors_protect(method, uri);
}

void hc_http (int argc, const char **argv) {

    char *service;

    parent = getppid();

    echttp_default ("-http-service=dynamic");

    argc = echttp_open (argc, argv);
    if (argc < 0) {
        fprintf (stderr, "[%s %d] echttp_open() failed\n", __FILE__, __LINE__);
        exit(1);
    }
    if (echttp_dynamic_port()) {
        static const char *path[] = {"clock:/ntp"};
        houseportal_initialize (argc, argv);
        houseportal_declare (echttp_port(4), path, 1);
    }
    houselog_initialize ("ntp", argc, argv);

    echttp_cors_allow_method("GET");
    echttp_protect (0, hc_protect);

    echttp_route_uri ("/ntp/status", hc_http_status);
    echttp_route_uri ("/ntp/traffic", hc_http_traffic);
    echttp_route_uri ("/ntp/drift", hc_http_clockdrift);
    echttp_route_uri ("/ntp/gps", hc_http_gps);
    echttp_route_uri ("/ntp/server", hc_http_ntp);
    echttp_static_route ("/", "/usr/local/share/house/public");
    echttp_background (&hc_background);
    houselog_event ("SERVICE", "ntp", "STARTED", "ON %s", houselog_host());
    echttp_loop();
    exit (0);
}

