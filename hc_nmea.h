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
 * hc_nmea.h - NMEA protocol decoder.
 */
const char *hc_nmea_help (int level);

int hc_nmea_initialize (int argc, const char **argv);
int hc_nmea_process (const struct timeval *received);

/* The GPS database:
 */
#define HC_NMEA_STATUS "GpsStatus"

typedef struct {
    char fix;
    char time[20];
    char date[20];
    char latitude[20];
    char longitude[20];
    char hemisphere[2];
    struct timeval timestamp;
} hc_nmea_status;

void hc_nmea_convert (char *buffer, int size,
                      const char *source, char hemisphere);

