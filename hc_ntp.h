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
 * hc_ntp.h - The NTP server implementation.
 */
#include <arpa/inet.h>
#include <netdb.h>

const char *hc_ntp_help (int level);

int  hc_ntp_initialize (int argc, const char **argv);
void hc_ntp_process    (const struct timeval *receive);
void hc_ntp_periodic   (const struct timeval *now);

#define HC_NTP_DEPTH 128
#define HC_NTP_POOL  4
#define HC_NTP_STATUS "NtpStatus"

struct hc_ntp_traffic {
    int received;
    int client;
    int broadcast;
    time_t timestamp;
};

struct hc_ntp_client {
    struct sockaddr_in address;
    struct timeval origin;
    struct timeval local;
    int logged;
};

struct hc_ntp_server {
    struct timeval origin;
    struct timeval local;
    short  stratum;
    struct sockaddr_in address;
    char   name[48];
    int logged;
};

typedef struct {
    char   mode;
    int8_t source;
    short  stratum;
    int    era;
    struct hc_ntp_server  pool[HC_NTP_POOL];

    struct hc_ntp_traffic live;
    struct hc_ntp_traffic latest;
    struct hc_ntp_traffic history[HC_NTP_DEPTH];
    struct hc_ntp_client  clients[HC_NTP_DEPTH];
} hc_ntp_status;

