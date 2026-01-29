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

#include <string.h>

#include "houseclock.h"
#include "hc_db.h"
#include "hc_ntp.h"
#include "hc_nmea.h"
#include "hc_clock.h"
#include "hc_broadcast.h"

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

// The template for all requests (the first fields never change):
//
ntpHeaderV3 ntpRequest = {
    0x23, // li=0, vn=4, mode=3.
    0,    // A client has no stratum of its own.
    10,   // default poll interval recommended in rfc 5905.
    -10,  // don't expect anything better than a millisecond accuracy.
    0,
    {0,0},
    "   ",
    {0, 0}, // reference.
    {0, 0}, // origin (same as request)
    {0, 0}, // receive.
    {0, 0}  // transmit.
};

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
static int hc_ntp_client_cursor = 0;

static int hc_ntp_broadcast = 0; // Force periodic broadcast even without GPS

static const char *ReferenceNtpServerName = 0;
static struct sockaddr_in ReferenceNtpServer = {0};

const char *hc_ntp_help (int level) {

    static const char *ntpHelp[] = {
        " [-ntp-service=NAME] [-ntp-period=INT] [-ntp-reference=NAME]",
        "-ntp-service=NAME:   name or port for the NTP socket",
        "-ntp-period=INT:     how often the NTP server advertises itself",
        "-ntp-reference=NAME: external reference NTP server, for calibration only",
        "-ntp-broadcast:      do periodic broadcast even without GPS",
        NULL
    };

    return ntpHelp[level];
}

static void hc_ntp_resolve (const char *name) {

    struct addrinfo hints = {0};
    struct addrinfo *resolved;

    hints.ai_flags = AI_ADDRCONFIG;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    int status = getaddrinfo (name, "ntp", &hints, &resolved);
    if (status) {
        fprintf (stderr, "[%s %d] cannot resolve %s: %s\n",
                 __FILE__, __LINE__, name, strerror(status));
        return; // Failed.
    }

    if (resolved->ai_addrlen <= sizeof(ReferenceNtpServer)) {
        memcpy (&ReferenceNtpServer, resolved->ai_addr, resolved->ai_addrlen);
    }
    freeaddrinfo (resolved);
}

int hc_ntp_initialize (int argc, const char **argv) {

    int i;
    const char *ntpservice = "ntp";
    const char *ntpperiod = "300";

    for (i = 1; i < argc; ++i) {
        echttp_option_match ("-ntp-service=", argv[i], &ntpservice);
        echttp_option_match ("-ntp-period=", argv[i], &ntpperiod);
        echttp_option_match ("-ntp-reference=", argv[i], &ReferenceNtpServerName);
        if (echttp_option_present ("-ntp-broadcast", argv[i])) {
            hc_ntp_broadcast = 1;
        }
    }
    if (ReferenceNtpServerName) hc_ntp_resolve (ReferenceNtpServerName);

    if (strcmp(ntpservice, "none") == 0) {
        return 0; // Do not act as a NTP server.
    }
    hc_ntp_period = atoi(ntpperiod);
    if (hc_ntp_period < 10) hc_ntp_period = 10;

    i = hc_db_new (HC_NTP_STATUS, sizeof(hc_ntp_status), 1);
    if (i != 0) {
        fprintf (stderr, "[%s %d] cannot create %s: %s\n",
                 __FILE__, __LINE__, HC_NTP_STATUS, strerror(i));
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
    hc_ntp_status_db->mode = 'I';
    hc_ntp_status_db->stratum = 0;

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

static void hc_ntp_broadcastmsg (const ntpHeaderV3 *head,
                                 const struct sockaddr_in *source,
                                 const struct timeval *receive) {

    int i, sender, available, weakest, worst;
    time_t death = receive->tv_sec - (hc_ntp_period * 3);
    const char *name = hc_broadcast_format(source);
    int ipaddress = source->sin_addr.s_addr;

    // This function handles any stratum value, even while this software
    // only send broadcast with stratum 1. We do so because we might receive
    // broadcast packets from other NTP software (e.g. ntpd or chrony).

    if (hc_debug_enabled())
        printf ("Received broadcast from %s at %ld.%03.3d: "
                "stratum=%d transmit=%u/%08x\n",
                name,
                (long)(receive->tv_sec), (int)(receive->tv_usec / 1000),
                head->stratum,
                ntohl(head->transmit.seconds),
                ntohl(head->transmit.fraction));

    if (head->stratum == 0) return;

    hc_ntp_status_db->live.broadcast += 1;

    // Search if that broadcasting server is already known.
    // This loop also looks for available slots, remove dead servers
    // and detect the time server with the highest stratum (weakest),
    // which could be sacrified if we just found a better time server.
    //
    weakest = -1;
    worst = head->stratum;
    sender = -1;
    available = -1;
    for (i = 0; i < HC_NTP_POOL; ++i) {
        if (ipaddress == hc_ntp_status_db->pool[i].address.sin_addr.s_addr) {
            sender = i;
        } else if (hc_ntp_status_db->pool[i].local.tv_sec < death) {
            // Forget a time server that stopped talking.
            if (hc_ntp_status_db->source == i) {
                hc_ntp_status_db->source = -1;
            }
            hc_ntp_status_db->pool[i].stratum = 0;

            if (available < 0) available = i; // Good slot for a new server.
        } else if (hc_ntp_status_db->pool[i].stratum > worst) {
            weakest = i; // This is the lowest quality server.
            worst = hc_ntp_status_db->pool[i].stratum;
        }
    }

    // If that time server was not known yet it goes to an empty slot,
    // replaces a dead server or else replaces the lowest-quality server.
    //
    if (sender < 0) {
        char *column;

        if (available < 0) {
            if (weakest < 0) return; // Too many good NTP servers?
            available = weakest;
            if (hc_ntp_status_db->source == weakest) {
                hc_ntp_status_db->source = -1;
            }
        }
        sender = available;
        strncpy (hc_ntp_status_db->pool[sender].name,
                 name, sizeof(hc_ntp_status_db->pool[0].name));
        column = strchr (hc_ntp_status_db->pool[sender].name, ':');
        if (column) *column = 0; // No need for the sender port number anymore.
        if (hc_debug_enabled())
            printf ("Assigned slot %d (current source: %d)\n",
                    sender, hc_ntp_status_db->source);
    }

    // Store the latest information from that server.
    //
    hc_ntp_status_db->pool[sender].address = *source;
    hc_ntp_status_db->pool[sender].local = *receive;
    hc_ntp_status_db->pool[sender].stratum = head->stratum;
    hc_ntp_get_timestamp
         (&(hc_ntp_status_db->pool[sender].origin), &(head->transmit));
    hc_ntp_status_db->pool[sender].logged = 0;

    // Elect a time source. Choose the lowest stratum available.
    //
    if (hc_ntp_status_db->source < 0) {

        short stratum = 255;
        available = sender;
        for (i = 0; i < HC_NTP_POOL; ++i) {
            if (hc_ntp_status_db->pool[i].local.tv_sec < death) continue;
            if (hc_ntp_status_db->pool[i].stratum <= 0) continue;
            if (hc_ntp_status_db->pool[i].stratum < stratum) {
                available = i;
                stratum = hc_ntp_status_db->pool[i].stratum;
                if (stratum == 1) break; // Cannot get lower than this.
            }
        }
        if (hc_debug_enabled())
            printf ("New time source %s (stratum %d)\n",
                    hc_ntp_status_db->pool[available].name,
                    hc_ntp_status_db->pool[available].stratum);

        hc_ntp_status_db->source = available;

    } else if (sender != hc_ntp_status_db->source) {

        if (hc_ntp_status_db->pool[sender].stratum <
               hc_ntp_status_db->pool[hc_ntp_status_db->source].stratum) {
            if (hc_debug_enabled())
                printf ("Better time source %s (stratum %d)\n",
                        hc_ntp_status_db->pool[sender].name,
                        hc_ntp_status_db->pool[sender].stratum);
            hc_ntp_status_db->source = sender;
        }
    }

    // Synchronize our time on the elected time source.
    //
    if (sender == hc_ntp_status_db->source) {
        hc_clock_synchronize
            (&(hc_ntp_status_db->pool[sender].origin), receive, 0);
        hc_ntp_status_db->stratum = hc_ntp_status_db->pool[sender].stratum + 1;
        if (hc_debug_enabled())
            printf ("Using time from NTP server %s\n",
                    hc_ntp_status_db->pool[sender].name);
    }
}

static void hc_ntp_request (void) {

    if (!ReferenceNtpServer.sin_family) return; // No address available.

    struct timeval transmit;
    gettimeofday (&transmit, NULL);
    hc_ntp_set_timestamp (&ntpRequest.transmit, &transmit);
    hc_broadcast_reply
        ((char *)(&ntpRequest), sizeof(ntpRequest), &ReferenceNtpServer);
}

static long long hc_ntp_interval (const struct timeval *t0,
                                  const struct timeval *t1) {
   return ((long long)(t1->tv_sec - t0->tv_sec) * 1000)
              + ((t1->tv_usec - t0->tv_usec) / 1000);
}

static void hc_ntp_responsemsg (const ntpHeaderV3 *head,
                                const struct sockaddr_in *source,
                                const struct timeval *receive) {

    struct timeval origin;
    struct timeval svrtransmit;
    struct timeval svrreceive;
    hc_ntp_get_timestamp (&origin, &(head->origin));
    hc_ntp_get_timestamp (&svrreceive, &(head->receive));
    hc_ntp_get_timestamp (&svrtransmit, &(head->transmit));

    long long offset = hc_ntp_interval (&origin, &svrreceive) -
                       hc_ntp_interval (&svrtransmit, receive);
    offset /= 2;

    if (hc_test_mode()) {
        long long latency = hc_ntp_interval (&origin, receive);
        printf ("%lld ms offset with time server %s (%s), response latency %lld ms\n",
                offset, ReferenceNtpServerName, inet_ntoa (source->sin_addr), latency);
    }
    // FUTURE: use the computed offset (or an average) to adjust the GPS
    // offset as a mean for automatic calibration.
}

static void hc_ntp_respond (const ntpHeaderV3 *head,
                            const struct sockaddr_in *source,
                            const struct timeval *receive) {

    // Build the response using the local system clock, if it has been
    // synchronized with GPS or remote broadcast server.

    int dispersion;
    struct timeval transmit;

    if (hc_nmea_active()) {
        ntpResponse.stratum = 1;
        strncpy (ntpResponse.refid, "GPS", sizeof(ntpResponse.refid));
    } else {
        int ntpsource = hc_ntp_status_db->source;
        if (ntpsource < 0) return; // No time source.

        ntpResponse.stratum = (uint8_t) hc_ntp_status_db->stratum;
        *((int *)(ntpResponse.refid)) =
            hc_ntp_status_db->pool[ntpsource].address.sin_addr.s_addr;
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
                "stratum=%d origin=%u/%08x reference=%u/%08x "
                "receive=%u/%08x transmit=%u/%08x dispersion=%dms\n",
            hc_broadcast_format (source),
            (long)(transmit.tv_sec),
            (int)(transmit.tv_usec / 1000),
            ntpResponse.stratum,
            ntohl(ntpResponse.origin.seconds),
            ntohl(ntpResponse.origin.fraction),
            ntohl(ntpResponse.reference.seconds),
            ntohl(ntpResponse.reference.fraction),
            ntohl(ntpResponse.receive.seconds),
            ntohl(ntpResponse.receive.fraction),
            ntohl(ntpResponse.transmit.seconds),
            ntohl(ntpResponse.transmit.fraction),
            dispersion);

    if (++hc_ntp_client_cursor >= HC_NTP_DEPTH) hc_ntp_client_cursor = 0;

    hc_ntp_status_db->clients[hc_ntp_client_cursor].address = *source;
    hc_ntp_get_timestamp
        (&(hc_ntp_status_db->clients[hc_ntp_client_cursor].origin),
         &(ntpResponse.origin));
    hc_ntp_status_db->clients[hc_ntp_client_cursor].local = *receive;
    hc_ntp_status_db->clients[hc_ntp_client_cursor].logged = 0;
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
            case 5: // Broadcast from a remote server.
                if (! hc_nmea_active()) {
                    hc_ntp_broadcastmsg (head, &source, receive);
                }
                break;
            case 4: // Server response (used in test mode).
                hc_ntp_responsemsg (head, &source, receive);
                break;
            case 3: // Client request.
                if ((hc_ntp_status_db->stratum > 0)
                        && hc_clock_synchronized()) {
                    hc_ntp_respond (head, &source, receive);
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
    static time_t latestRequest = 0;

    if (latestPeriod == 0) {
        latestPeriod = wakeup->tv_sec / 10;
    } else if (wakeup->tv_sec / 10 > latestPeriod) {
        int slot = latestPeriod % HC_NTP_DEPTH;
        hc_ntp_status_db->live.timestamp = latestPeriod * 10;
        hc_ntp_status_db->latest = hc_ntp_status_db->live;
        hc_ntp_status_db->history[slot] = hc_ntp_status_db->live;

        hc_ntp_status_db->live.received = 0;
        hc_ntp_status_db->live.client = 0;
        hc_ntp_status_db->live.broadcast = 0;
        latestPeriod += 1;
    }

    if (wakeup->tv_sec >= latestRequest + 10) {
        hc_ntp_request ();
        latestRequest = wakeup->tv_sec;
    }
    if (hc_test_mode()) return;

    if (hc_ntp_broadcast || hc_nmea_active()) {
        if (hc_clock_synchronized() &&
            (wakeup->tv_sec >= latestBroadcast + hc_ntp_period)) {

            struct timeval timestamp;
            int dispersion = hc_clock_dispersion();

            hc_ntp_set_dispersion (dispersion, &ntpBroadcast);
            hc_ntp_set_reference (&ntpBroadcast);

            hc_broadcast_enumerate();

            gettimeofday (&timestamp, NULL);
            hc_ntp_set_timestamp (&ntpBroadcast.transmit, &timestamp);

            hc_broadcast_send ((char *)&ntpBroadcast, sizeof(ntpBroadcast), 0);

            latestBroadcast = wakeup->tv_sec;
            hc_ntp_status_db->live.broadcast += 1;
            hc_ntp_status_db->stratum = 1;

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
        hc_ntp_status_db->source = -1;
    } else {
        hc_ntp_status_db->mode = 'C';
        if (hc_ntp_status_db->source >= 0) {
            int source = hc_ntp_status_db->source;
            time_t death = wakeup->tv_sec - (hc_ntp_period * 3);
            if (hc_ntp_status_db->pool[source].local.tv_sec < death) {
                hc_ntp_status_db->source = -1;
            }
        }
        if (hc_ntp_status_db->source < 0) {
            hc_ntp_status_db->stratum = 0;
        }
    }
}

