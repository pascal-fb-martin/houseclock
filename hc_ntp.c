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
 * hc_ntp.c - The NTP server implementation.
 *
 * SYNOPSYS:
 *
 * int hc_ntp_initialize (const char *service);
 *
 *    Initialize the NTP context. Returns a socket or -1.
 *
 * void hc_ntp_process (const struct timeval *receive, int synchronized);
 *
 *    Process one available NTP message. The receive parameter indicates
 *    when it was detected that data is available. The synchronized
 *    parameter indicates if the local system time is synchronized with
 *    the reference clock (e.g. GPS clock).
 *
 * void hc_ntp_periodic (const struct timeval *now);
 *
 *    Send a periodic NTP time message.
 */

#include "houseclock.h"
#include "hc_ntp.h"
#include "hc_broadcast.h"

#define NTP_VERSION 3
#define NTP_UNIX_EPOCH 2208988800
#define NTP_FRACTION_MS 4310344

#define NTP_US_TO_FRACTION(x) ((uint32_t)((x / 1000) * NTP_FRACTION_MS))

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

    uint32_t rootDispersion;

    char refid[4];

    ntpTimestamp reference;

    ntpTimestamp origin;

    ntpTimestamp receive;

    ntpTimestamp transmit;

} ntpHeaderV3;

// The template for all responses (the first fields never change):
//
ntpHeaderV3 ntpResponse = {
    0x1c, // li=0, vn=3, mode=4.
    1,    // a GPS-equipped server is stratum 1.
    10,   // default poll interval recommended in rfc 5905.
    -10,  // don't expect anything better than a millisecond accuracy.
    0,
    0,
    "GPS",
    {0, 0},
    {0, 0},
    {0, 0},
    {0, 0}
};

static const ntpTimestamp zeroTimestamp = {0, 0};


const char *hc_ntp_help (int level) {

    static const char *ntpHelp[] = {
        " [-service=NAME]",
        "-service=NAME:   name or port for the NTP socket",
        NULL
    };

    return ntpHelp[level];
}

int hc_ntp_initialize (int argc, const char **argv) {

    int i;
    const char *ntpservice = "ntp";

    for (i = 1; i < argc; ++i) {
        hc_match ("-service=", argv[i], &ntpservice);
    }
    if (strcmp(ntpservice, "none") == 0) {
        return 0; // Do not act as a NTP server.
    }
    if (hc_test_mode()) return -1;

    return hc_broadcast_open (ntpservice);
}


void hc_ntp_process (const struct timeval *receive,
                         int synchronized) {

    in_addr_t source;

    // The receive buffer is as large as the max UDP packet: no overflow ever.
    char buffer[0x10000];
    int length = hc_broadcast_receive(buffer, sizeof(buffer), &source);

    if (!synchronized) return; // Ignore all requests until local time is OK.

    if (length >= sizeof(ntpHeaderV3)) {
        struct timeval transmit;
        ntpHeaderV3 *head = (ntpHeaderV3 *)buffer;
        int version = (head->liVnMode >> 3) & 0x7;

        ntpHeaderV3 response;

        if (version < 3) return;
        if (head->liVnMode & 0x7 != 3) return;

        // Now we know this is a client packet with version 3 or above.
        // Build the response using the system clock, which is assumed to
        // be synchronized with the GPS clock.

        ntpResponse.origin = head->origin;
        ntpResponse.receive.seconds = (uint32_t) (receive->tv_sec);
        ntpResponse.receive.fraction = NTP_US_TO_FRACTION(receive->tv_usec);

        gettimeofday (&transmit, NULL);
        ntpResponse.transmit.seconds = (uint32_t) (transmit.tv_sec);
        ntpResponse.transmit.fraction = NTP_US_TO_FRACTION(transmit.tv_usec);

        hc_broadcast_reply
            ((char *)&ntpResponse, sizeof(ntpResponse), source);
    }
}

void hc_ntp_periodic (const struct timeval *wakeup) {

    struct timeval transmit;

    ntpResponse.origin = zeroTimestamp;
    ntpResponse.receive = zeroTimestamp;

    gettimeofday (&transmit, NULL);
    ntpResponse.transmit.seconds = (uint32_t) (transmit.tv_sec);
    ntpResponse.transmit.fraction = NTP_US_TO_FRACTION(transmit.tv_usec);

    hc_broadcast_send ((char *)&ntpResponse, sizeof(ntpResponse));
}

