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
 * hc_broadcast.h - Manage broadcast & UDP communications.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

int hc_broadcast_open (const char *service);

void hc_broadcast_enumerate (void);
void hc_broadcast_send (const char *data, int length, int *address);

void hc_broadcast_reply
        (const char *data, int length, const struct sockaddr_in *destination);

int  hc_broadcast_receive
        (char *buffer, int size, struct sockaddr_in *source);

const char *hc_broadcast_format (const struct sockaddr_in *addr);

int hc_broadcast_local (int address);

