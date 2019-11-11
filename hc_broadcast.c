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
 * void hc_broadcast_send (const char *data, int length)
 *
 *    Send a data packet in broadcast mode.
 *
 * void hc_broadcast_reply (const char *data, int length,
 *                          in_addr_t destination)
 *
 *    Send a response packet to the specified unicast address.
 *
 * int hc_broadcast_receive (char *buffer, int size, in_addr_t *source)
 *
 *    Get the data and source address of a received packet.
 *    Returns the length of the data, or -1.
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

#include "houseclock.h"
#include "hc_broadcast.h"

static int udpserver = -1;

static struct sockaddr_in netaddress;

int hc_broadcast_open (const char *service) {

    int value;
    int port = -1;
    int flags;

    struct servent *entry = getservbyname(service, "udp");

    if (entry == NULL) {
       if (isdigit (service[0])) {
          port = atoi(service);
	   }
    } else {
	   port = ntohs(entry->s_port);
    }

    if (port < 0) {
	   fprintf (stderr, "invalid service name %s\n", service);
	   exit (1);
    }

    DEBUG printf ("Opening UDP port %d (name: %s)\n",
                  port, (entry == NULL)?"(null)":entry->s_name);

    udpserver = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpserver < 0) {
       fprintf (stderr, "cannot open socket for service %s\n", service);
       exit (1);
    }

    flags = fcntl(udpserver, F_GETFL, 0);
    fcntl (udpserver, F_SETFL, flags | O_NONBLOCK);

    value = 1024 * 1024;
    if (setsockopt(udpserver, SOL_SOCKET, SO_RCVBUF,
			      &value, sizeof(value)) < 0) {
       fprintf (stderr, "cannot set receive buffer to %d for service %s\n",
                value, service);
       exit (1);
    }
    value = 1024 * 1024;
    if (setsockopt(udpserver, SOL_SOCKET, SO_SNDBUF,
			      &value, sizeof(value)) < 0) {
       fprintf (stderr, "cannot set send buffer to %d for service %s\n",
                value, service);
       exit (1);
    }

    value = 1;
    if (setsockopt(udpserver, SOL_SOCKET, SO_BROADCAST,
			      &value, sizeof(value)) < 0) {
       fprintf (stderr, "cannot enable broadcast for service %s\n", service);
       exit (1);
    }

    memset(&netaddress, 0, sizeof(netaddress));
    netaddress.sin_family = AF_INET;
    netaddress.sin_addr.s_addr = INADDR_ANY;
    netaddress.sin_port = htons(port);

    if (bind(udpserver, (struct sockaddr *)&netaddress,
			             sizeof(netaddress)) < 0) {
       fprintf (stderr, "cannot bind to service %s: %s\n",
                service, strerror(errno));
       exit (1);
    }
    return udpserver;
}


void hc_broadcast_send (const char *data, int length) {

    if (udpserver < 0) return;
    netaddress.sin_addr.s_addr = 0xffffffff; // 255.255.255.255

    sendto (udpserver, data, length, 0,
               (struct sockaddr *)&netaddress, sizeof(netaddress));
}


const char *hc_broadcast_format (struct sockaddr_in *addr) {
    static char formatted[80];
    snprintf (formatted, sizeof(formatted), "%d.%d.%d.%d:%d",
              addr->sin_addr.s_addr & 0xff,
              (addr->sin_addr.s_addr >> 8) & 0xff,
              (addr->sin_addr.s_addr >> 16) & 0xff,
              (addr->sin_addr.s_addr >> 24) & 0xff,
              addr->sin_port);
    return formatted;
}

void hc_broadcast_reply
        (const char *data, int length, struct sockaddr_in *destination) {

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

