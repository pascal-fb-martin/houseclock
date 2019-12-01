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
 * hc_ntp.c - The (S)NTP implementation.
 *
 *    This module behaves as a SNTP stratum 1 server when the NMEA feed
 *    is active (GPS device is present and a fix was obtained), and as
 *    a SNTP broadcast client otherwise.
 *
 * SYNOPSYS:
 *
 * int hc_ntp_initialize (const char *service);
 *
 *    Initialize the NTP context. Returns a socket or -1.
 *
 * void hc_ntp_process (const struct timeval *receive);
 *
 *    Process one available NTP message. The receive parameter indicates
 *    when it was detected that data is available.
 *
 * void hc_ntp_periodic (const struct timeval *now);
 *
 *    Send a periodic NTP time message.
 */

#include "houseclock.h"
#include "hc_db.h"
#include "hc_ntp.h"
#include "hc_nmea.h"
#include "hc_clock.h"
#include "hc_broadcast.h"

#define NTP_VERSION 3
#define NTP_UNIX_EPOCH 2208988800ull


typedef struct {
    uint16_t seconds;
    uint16_t fraction;
} ntpTimeshort;

typedef struct {
    uint32_t seconds;
    uint32_t fraction;
} ntpTimestamp;

typedef struct
{

    uint8_t liVnMode;
    uint8_t stratum;
    uint8_t poll;
    uint8_t precision;

    uint32_t rootDelay;

    ntpTimeshort rootDispersion;

    char refid[4];

    ntpTimestamp reference;

    ntpTimestamp origin;

    ntpTimestamp receive;

    ntpTimestamp transmit;

} ntpHeaderV3;

// The template for all responses (the first fields never change):
//
ntpHeaderV3 ntpResponse = {
    0x24, // li=0, vn=4, mode=4.
    1,    // a GPS-equipped server is stratum 1.
    10,   // default poll interval recommended in rfc 5905.
    -10,  // don't expect anything better than a millisecond accuracy.
    0,
    {0,0},
    "GPS",
    {0, 0}, // reference.
    {0, 0}, // origin (same as request)
    {0, 0}, // receive.
    {0, 0}  // transmit.
};

ntpHeaderV3 ntpBroadcast = {
    0x25, // li=0, vn=4, mode=5.
    1,    // a GPS-equipped server is stratum 1.
    10,   // default poll interval recommended in rfc 5905.
    -10,  // don't expect anything better than a millisecond accuracy.
    0,
    {0,0},
    "GPS",
    {0, 0}, // Reference.
    {0, 0}, // origin (always 0)
    {0, 0}, // receive (always 0)
    {0, 0}  // transmit.
};

static const ntpTimestamp zeroTimestamp = {0, 0};

static hc_ntp_status *hc_ntp_status_db = 0;

static int hc_ntp_period;


const char *hc_ntp_help (int level) {

    static const char *ntpHelp[] = {
        " [-ntp-service=NAME] [-ntp-period=INT]",
        "-ntp-service=NAME:   name or port for the NTP socket",
        "-ntp-period=INT:     how often the NTP server advertises itself",
        NULL
    };

    return ntpHelp[level];
}

int hc_ntp_initialize (int argc, const char **argv) {

    int i;
    const char *ntpservice = "ntp";
    const char *ntpperiod = "300";

    for (i = 1; i < argc; ++i) {
        hc_match ("-ntp-service=", argv[i], &ntpservice);
        hc_match ("-ntp-period=", argv[i], &ntpperiod);
    }
    if (strcmp(ntpservice, "none") == 0) {
        return 0; // Do not act as a NTP server.
    }
    hc_ntp_period = atoi(ntpperiod);
    if (hc_ntp_period < 10) hc_ntp_period = 10;

    i = hc_db_new (HC_NTP_STATUS, sizeof(hc_ntp_status), 1);
    if (i != 0) {
        fprintf (stderr, "cannot create %s: %s\n", HC_NTP_STATUS, strerror(i));
        exit (1);
    }
    hc_ntp_status_db = (hc_ntp_status *) hc_db_get (HC_NTP_STATUS);
    hc_ntp_status_db->live.received = 0;
    hc_ntp_status_db->live.client = 0;
    hc_ntp_status_db->live.broadcast = 0;
    hc_ntp_status_db->live.timestamp = 0;
    for (i = 0; i < HC_NTP_DEPTH; ++i) {
        hc_ntp_status_db->history[i].received = 0;
        hc_ntp_status_db->history[i].client = 0;
        hc_ntp_status_db->history[i].broadcast = 0;
        hc_ntp_status_db->history[i].timestamp = 0;
    }
    for (i = 0; i < HC_NTP_POOL; ++i) {
        hc_ntp_status_db->pool[i].local.tv_sec = 0;
    }
    hc_ntp_status_db->source = -1;

    if (hc_test_mode()) return -1;

    return hc_broadcast_open (ntpservice);
}


static uint32_t fraction2usec(uint32_t fraction)
{
    return (uint32_t)((double)fraction * 1.0e6 / 4294967296.0);
}
 
static uint32_t usec2fraction(uint32_t usec)
{
    return (uint32_t)((double)usec * 4294967296.0 / 1.0e6);
}

static void hc_ntp_get_timestamp (struct timeval *local,
                                  const ntpTimestamp *ntp) {
    local->tv_sec = ntohl(ntp->seconds) - NTP_UNIX_EPOCH;
    local->tv_usec = fraction2usec(ntohl(ntp->fraction));
}

static void hc_ntp_set_timestamp (ntpTimestamp *ntp,
                                  const struct timeval *local) {
    ntp->seconds = htonl((uint32_t) (local->tv_sec) + NTP_UNIX_EPOCH);
    ntp->fraction = htonl(usec2fraction(local->tv_usec));
}

static void hc_ntp_set_reference (ntpHeaderV3 *packet) {

    struct timeval timestamp;
    hc_clock_reference (&timestamp);
    hc_ntp_set_timestamp (&(packet->reference), &timestamp);
}

static void hc_ntp_set_dispersion (int dispersion, ntpHeaderV3 *packet) {
    if (dispersion > 1000) {
        packet->rootDispersion.seconds = htons((uint16_t) (dispersion / 1000));
        dispersion = dispersion % 1000;
    } else {
        packet->rootDispersion.seconds = 0;
    }
    packet->rootDispersion.fraction =
        htons((uint16_t) (double)(dispersion / 1e3) * 65536.);
}

static int hc_ntp_get_dispersion (const ntpHeaderV3 *packet) {

    int dispersion = ntohs((packet->rootDispersion.fraction * 1e3) / 65536.);
    if (packet->rootDispersion.seconds != 0) {
        dispersion += ntohs(packet->rootDispersion.seconds) * 1000;
    }
    return dispersion;
}

static void hc_ntp_broadcast (const ntpHeaderV3 *head,
                              const struct sockaddr_in *source,
                              const struct timeval *receive) {

    int i, next;
    time_t death = receive->tv_sec - (hc_ntp_period * 3);
    const char *name = hc_broadcast_format(source);

    if (hc_debug_enabled())
        printf ("Received broadcast from %s at %ld.%03.3d "
                "(transmit=%u/%08x dispersion=%dms)\n",
                name,
                (long)(receive->tv_sec), (int)(receive->tv_usec / 1000),
                ntohl(head->transmit.seconds),
                ntohl(head->transmit.fraction),
                hc_ntp_get_dispersion(head));

    hc_ntp_status_db->live.broadcast += 1;

    // Search if that broadcasting server is already known.
    //
    next = -1;
    for (i = 0; i < HC_NTP_POOL; ++i) {
        if (strcmp (name, hc_ntp_status_db->pool[i].name) == 0) break;
        if (hc_ntp_status_db->pool[i].local.tv_sec < death) {
            // Forget a time server that stopped talking.
            if (i == hc_ntp_status_db->source) {
                hc_ntp_status_db->source = -1;
            }
            if (next < 0) next = i; // Good slot for a new server.
        }
    }

    // Make sure we do not keep following a time server that died,
    // even if last on the list.
    //
    if (hc_ntp_status_db->source >= 0) {
        int source = hc_ntp_status_db->source;
        if (source != i) {
            if (hc_ntp_status_db->pool[source].local.tv_sec < death) {
                hc_ntp_status_db->source = -1;
                if (next < 0) next = source; // Good slot for a new server.
            }
        }
    }

    // If not known yet it goes to an empty slot, or replaces a dead server.
    //
    if (i >= HC_NTP_POOL) {
        if (next < 0) return; // Too many active servers to choose from.
        i = next;
        strncpy (hc_ntp_status_db->pool[i].name,
                 name, sizeof(hc_ntp_status_db->pool[0].name));
    }

    // Store the latest time information.
    //
    hc_ntp_status_db->pool[i].local = *receive;
    hc_ntp_get_timestamp
         (&(hc_ntp_status_db->pool[i].origin), &(head->transmit));

    // Elect a time source.
    //
    if (hc_ntp_status_db->source < 0) {
        if (hc_debug_enabled())
            printf ("New time source %s\n", hc_ntp_status_db->pool[i].name);

        hc_ntp_status_db->source = i;
    }

    // Synchronize our time on the elected time source.
    //
    if (i == hc_ntp_status_db->source) {
        hc_clock_synchronize (&(hc_ntp_status_db->pool[i].origin), receive, 0);
    }
}

static void hc_ntp_request (const ntpHeaderV3 *head,
                            const struct sockaddr_in *source,
                            const struct timeval *receive) {

    // Build the response using the local system clock, if it has been
    // synchronized with the GPS clock.

    int dispersion;
    struct timeval transmit;

    if (!hc_clock_synchronized()) {
        if (hc_debug_enabled())
            printf ("Ignoring request from %s: not synchronized\n",
                    hc_broadcast_format (source));
        return; // Ignore all requests until local time is OK.
    }

    hc_ntp_status_db->live.client += 1;

    ntpResponse.origin = head->transmit;

    dispersion = hc_clock_dispersion();
    hc_ntp_set_dispersion (dispersion, &ntpResponse);
    hc_ntp_set_reference (&ntpResponse);
    hc_ntp_set_timestamp (&ntpResponse.receive, receive);

    gettimeofday (&transmit, NULL);
    hc_ntp_set_timestamp (&ntpResponse.transmit, &transmit);

    hc_broadcast_reply ((char *)&ntpResponse, sizeof(ntpResponse), source);

    if (hc_debug_enabled())
        printf ("Response to %s at %d.%0.03d: "
                "origin=%u/%08x, reference=%u/%08x, "
                "receive=%u/%08x, transmit=%u/%08x dispersion=%dms\n",
            hc_broadcast_format (source),
            (long)(transmit.tv_sec),
            (int)(transmit.tv_usec / 1000),
            ntohl(ntpResponse.origin.seconds),
            ntohl(ntpResponse.origin.fraction),
            ntohl(ntpResponse.reference.seconds),
            ntohl(ntpResponse.reference.fraction),
            ntohl(ntpResponse.receive.seconds),
            ntohl(ntpResponse.receive.fraction),
            ntohl(ntpResponse.transmit.seconds),
            ntohl(ntpResponse.transmit.fraction),
            dispersion);
}


void hc_ntp_process (const struct timeval *receive) {

    struct sockaddr_in source;

    // The receive buffer is as large as the max UDP packet: no overflow ever.
    char buffer[0x10000];
    int length = hc_broadcast_receive(buffer, sizeof(buffer), &source);

    hc_ntp_status_db->live.received += 1;

    if (length >= sizeof(ntpHeaderV3)) {
        ntpHeaderV3 *head = (ntpHeaderV3 *)buffer;
        int version = (head->liVnMode >> 3) & 0x7;

        switch (head->liVnMode & 0x7) {
            case 6: break; // Control.
            case 5: // Server broadcast.
                if (! hc_nmea_active()) {
                    hc_ntp_broadcast(head, &source, receive);
                }
                break;
            case 4: break; // Server response.
            case 3: // Client request.
                if (hc_nmea_active()) {
                    hc_ntp_request (head, &source, receive);
                }
                break;
            default:
                if (hc_debug_enabled())
                    printf ("Ignore packet from %s: version=%d, mode=%d\n",
                            hc_broadcast_format (&source),
                            version, head->liVnMode & 0x7);
                break;
        }
    }
}

void hc_ntp_periodic (const struct timeval *wakeup) {

    static time_t latestPeriod = 0;
    static time_t latestBroadcast = 0;

    if (latestPeriod == 0) {
        latestPeriod = wakeup->tv_sec / 10;
    } else if (wakeup->tv_sec / 10 > latestPeriod) {
        int slot = latestPeriod % HC_NTP_DEPTH;
        hc_ntp_status_db->live.timestamp = latestPeriod * 10;
        hc_ntp_status_db->latest = hc_ntp_status_db->live;
        hc_ntp_status_db->history[slot] = hc_ntp_status_db->live;
        latestPeriod += 1;
    }

    if (hc_nmea_active()) {
        if (hc_clock_synchronized() &&
            (wakeup->tv_sec > latestBroadcast + hc_ntp_period)) {

            struct timeval timestamp;
            int dispersion = hc_clock_dispersion();

            hc_ntp_set_dispersion (dispersion, &ntpBroadcast);
            hc_ntp_set_reference (&ntpBroadcast);

            gettimeofday (&timestamp, NULL);
            hc_ntp_set_timestamp (&ntpBroadcast.transmit, &timestamp);

            hc_broadcast_send ((char *)&ntpBroadcast, sizeof(ntpBroadcast));
            latestBroadcast = wakeup->tv_sec;
            hc_ntp_status_db->live.broadcast += 1;

            if (hc_debug_enabled())
                printf ("Sent broadcast packet at %ld.%03.3d: "
                        "transmit=%u/%08x, dispersion=%dms\n",
                        (long)(timestamp.tv_sec),
                        (int)(timestamp.tv_usec / 1000),
                        ntohl(ntpBroadcast.transmit.seconds),
                        ntohl(ntpBroadcast.transmit.fraction),
                        dispersion);
        }
        hc_ntp_status_db->mode = 'S';
    } else {
        hc_ntp_status_db->mode = 'C';
    }
}

