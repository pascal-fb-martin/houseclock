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
 * hc_broadcast.c - Manage broadcast & UDP communications.
 *
 * This module opens a UDP server socket that accepts broadcast.
 *
 * SYNOPSYS:
 *
 * int hc_broadcast_open (const char *service)
 *
 *    Open the broadcast UDP socket and returns the socket ID.
 *
 * void hc_broadcast_enumerate (void);
 *
 *    Retrieve the local interfaces. Must be called before hc_broadcast_send()
 *    to adjust to network interface changes.
 *
 * void hc_broadcast_send (const char *data, int length, int *address)
 *
 *    Send a data packet in broadcast mode. This transmits a broadcast packet
 *    on each network interface. If address is not null, the interface's
 *    IPv4 address is written to it before each transmission.
 *
 * void hc_broadcast_reply (const char *data, int length,
 *                          const struct sockaddr_in *destination)
 *
 *    Send a response packet to the specified unicast address.
 *
 * int hc_broadcast_receive (char *buffer, int size, in_addr_t *source)
 *
 *    Get the data and source address of a received packet.
 *    Returns the length of the data, or -1.
 *
 * const char *hc_broadcast_format (const struct sockaddr_in *addr);
 *
 *    Get a string representation of the network address. This
 *    returns a string to a static buffer: the value is erased
 *    on each call.
 *
 * int hc_broadcast_local (int address);
 *
 *    Return the local address on the same network as the provided address,
 *    or 0 if no matching network is found.
 *
 * LIMITATIONS:
 *
 * Only supports IPv4 addresses for the time being.
 * Only supports local broadcast (address 255.255.255.255).
 * Only supports one socket per process.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

#include "houseclock.h"
#include "hc_broadcast.h"

static int udpserver = -1;

#define UDPCLIENT_MAX 16
static int udpclient[UDPCLIENT_MAX];
static int interface[UDPCLIENT_MAX];
static int ipmask[UDPCLIENT_MAX];
static int udpclient_count = 0;

static struct sockaddr_in netaddress;

static int hc_broadcast_socket (int ipv4, int port) {

    int value;
    int flags;
    int s;

    s = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
       fprintf (stderr, "cannot open socket for port %d: %s\n",
                port, strerror(errno));
       exit (1);
    }

    flags = fcntl(s, F_GETFL, 0);
    fcntl (s, F_SETFL, flags | O_NONBLOCK);

    value = 1;
    if (setsockopt(s, SOL_SOCKET, SO_BROADCAST, &value, sizeof(value)) < 0) {
       fprintf (stderr, "cannot enable broadcast for port %d: %s\n",
                port, strerror(errno));
       exit (1);
    }

    memset(&netaddress, 0, sizeof(netaddress));
    netaddress.sin_family = AF_INET;
    netaddress.sin_addr.s_addr = ipv4;
    netaddress.sin_port = htons(port);

    if (bind(s, (struct sockaddr *)&netaddress, sizeof(netaddress)) < 0) {
       fprintf (stderr,
                "cannot bind to port %d: %s\n", port, strerror(errno));
       exit (1);
    }

    DEBUG printf ("Socket open for address %s\n",
                  hc_broadcast_format(&netaddress));

    return s;
}


void hc_broadcast_enumerate (void) {

    struct ifaddrs *cards;
    struct sockaddr_in *ia;

    while (--udpclient_count >= 0) {
        if (udpclient[udpclient_count] >= 0) {
            close (udpclient[udpclient_count]);
        }
    }

    // Open one UDP client socket for each (real) network interface. This
    // will be used for sending periodic broadcast on each specific network.
    //
    udpclient_count = 0;
    if (getifaddrs(&cards) == 0) {

        struct ifaddrs *cursor;

        for (cursor = cards; cursor != 0; cursor = cursor->ifa_next) {

            DEBUG printf ("Network interface %s\n", cursor->ifa_name);

            if ((cursor->ifa_addr == 0) || (cursor->ifa_netmask == 0)) continue;
            if (cursor->ifa_addr->sa_family != AF_INET)  continue;

            ia = (struct sockaddr_in *) (cursor->ifa_addr);
            if (ia->sin_addr.s_addr == INADDR_LOOPBACK) continue;
            interface[udpclient_count] = ia->sin_addr.s_addr;

            ia = (struct sockaddr_in *) (cursor->ifa_netmask);
            ipmask[udpclient_count] = ia->sin_addr.s_addr;

            udpclient[udpclient_count] =
                hc_broadcast_socket(ia->sin_addr.s_addr, 0);

            if (++udpclient_count >= UDPCLIENT_MAX) break;
        }
        freeifaddrs(cards);
    }
}

int hc_broadcast_open (const char *service) {

    int value;
    int port;

    // Open the UDP server socket for receiving NTP requests and sending
    // responses. This must be the last socket to open.
    //
    struct servent *entry = getservbyname(service, "udp");

    if (entry == NULL) {
        if (isdigit (service[0])) {
           port = atoi(service);
        }
    } else {
        port = ntohs(entry->s_port);
    }

    if (port <= 0) {
        fprintf (stderr, "invalid service name %s\n", service);
        exit (1);
    }

    DEBUG printf ("Opening UDP port %d (name: %s)\n",
                  port, (entry == NULL)?"(null)":entry->s_name);

    udpserver = hc_broadcast_socket(INADDR_ANY, port);

    value = 1024 * 1024;
    if (setsockopt(udpserver,
                   SOL_SOCKET, SO_RCVBUF, &value, sizeof(value)) < 0) {
       fprintf (stderr, "cannot set receive buffer to %d for service %s: %s\n",
                value, service, strerror(errno));
       exit (1);
    }
    value = 1024 * 1024;
    if (setsockopt(udpserver,
                   SOL_SOCKET, SO_SNDBUF, &value, sizeof(value)) < 0) {
       fprintf (stderr, "cannot set send buffer to %d for service %s: %s\n",
                value, service, strerror(errno));
       exit (1);
    }

    netaddress.sin_addr.s_addr = INADDR_BROADCAST;

    return udpserver;
}


void hc_broadcast_send (const char *data, int length, int *address) {

    int i;

    for (i = 0; i < udpclient_count; ++i) {
        if (udpclient[i] < 0) continue;
        if (address != 0) *address = interface[i];
        sendto (udpclient[i], data, length, 0,
                (struct sockaddr *)&netaddress, sizeof(netaddress));
    }
}


const char *hc_broadcast_format (const struct sockaddr_in *addr) {

    static char formatted[80];

    snprintf (formatted, sizeof(formatted), "%d.%d.%d.%d:%d",
              addr->sin_addr.s_addr & 0xff,
              (addr->sin_addr.s_addr >> 8) & 0xff,
              (addr->sin_addr.s_addr >> 16) & 0xff,
              (addr->sin_addr.s_addr >> 24) & 0xff,
              addr->sin_port);

    return formatted;
}

int hc_broadcast_local (int address) {

    int i;

    if (udpclient_count <= 0) return INADDR_LOOPBACK;

    for (i = udpclient_count - 1; i >= 0; --i) {
        if ((interface[i] & ipmask[i]) == (address & ipmask[i]))
            return interface[i];
    }
    return 0;
}

void hc_broadcast_reply
        (const char *data, int length, const struct sockaddr_in *destination) {

    if (udpserver < 0) return;

    sendto (udpserver, data, length, 0,
            (struct sockaddr *)destination, sizeof(struct sockaddr_in));
}

int hc_broadcast_receive (char *buffer, int size,
                          struct sockaddr_in *source) {

    int length;
    socklen_t srclength = sizeof(struct sockaddr_in);

    if (udpserver < 0) return 0;

    length = recvfrom (udpserver, buffer, size, 0,
		               (struct sockaddr *)source, &srclength);

    return length;
}

