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

int  hc_nmea_initialize (int argc, const char **argv);
int  hc_nmea_process (const struct timeval *received);
void hc_nmea_periodic (const struct timeval *now);
int  hc_nmea_active (void);

/* The GPS database:
 */
#define HC_NMEA_STATUS "GpsStatus"
#define HC_NMEA_TEXT_LINES 16
#define HC_NMEA_DEPTH 32
#define HC_NMEA_MAX_SENTENCE 81 // NMEA sentence is no more than 80 characters.

typedef struct {
    char sentence[HC_NMEA_MAX_SENTENCE];
    char flags;
    struct timeval timing;
} gpsSentence;

typedef struct {
    char   fix;
    time_t fixtime;
    char   gpstime[20];
    char   gpsdate[20];
    char   latitude[20];
    char   longitude[20];
    char   hemisphere[2];
    struct timeval timestamp;
    struct {
        char line[HC_NMEA_MAX_SENTENCE];
    } text[HC_NMEA_TEXT_LINES];
    int textcount;
    gpsSentence history[HC_NMEA_DEPTH];
    int gpscount;
} hc_nmea_status;

void hc_nmea_convert (char *buffer, int size,
                      const char *source, char hemisphere);

