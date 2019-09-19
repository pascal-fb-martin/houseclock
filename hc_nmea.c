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
 * hc_nmea.c - NMEA protocol decoder.
 *
 * This module consumes raw NMEA data from a serial port or USB, with
 * receive timing information as precise as possible.
 *
 * This module assumes that the timezone was set to "UTC".
 *
 * Once a NMEA sentence has been decoded, the module determines:
 * - the status of the fix.
 * - the estimated timing of the $ in each NMEA sentence for the last 2 fixes.
 * - the GPS UTC time.
 *
 * In order to protect against GPS initialization problems, the module
 * waits for a GPS fix to have been available for 10 seconds before
 * using the GPS time.
 *
 * The module determines the first sentence of a fix as the sentence in
 * which the fix time changed.
 *
 * The transmission speed is calculated as the average transmission time
 * of the subsequent blocks of data within a fix. That speed is then used
 * to estimate the actual transmission time for any character in the NMEA
 * stream, and then retrieve when the start of the sentence was received.
 * This estimation is subject to instabilities: the goal is to reach a
 * precision of about 1/10 or 1/100 second, which is way more than needed
 * for a home network.
 *
 * Once the module has decided which sentence came first, it uses the
 * estimated start time of this sentence as the comparison point with the
 * GPS time, i.e. the local time used to calculate the local time delta.
 *
 * If there is a different, adjtime() is called to correct the local time,
 * unless the delta is too large, in which case the time is just reset.
 *
 * SYNOPSYS:
 *
 * const char *hc_nmea_help (int level)
 *
 *    Prints the help information, two levels:
 *      0: short help for one-line argument information.
 *      1: multi-line description of each argument.
 *
 * int hc_nmea_initialize (int argc, const char **argv)
 *
 *    Reset the NMEA decoder status, retrieve and store the NMEA options
 *    from the program's command line arguments.
 *
 *    The command line options processed here are:
 *      -gps=<dev>      Name of the system device to read the NMEA data from.
 *      -latency=<N>    Delay between the GPS fix and the 1st sentence (ms).
 *      -burst          Use burst start as the GPS fix timing reference.
 *
 *    The default GPS device is /dev/ttyACM0.
 *
 *    The latency depends on the GPS device. It can be estimated by using
 *    the options -drift and -latency=0, and then estimating the average
 *    drift, on a machine where the time is already synchronized using NTP.
 *    Default is 70 ms.
 *
 * int hc_nmea_process (const struct timeval *received)
 *
 *    Called when new data is available, with the best know receive time,
 *    typically when the application was notified that data is available.
 *    This time will be associated with the last received byte.
 */

/* NMEA sentences:
 *
 * (See: http://aprs.gids.nl/nmea/
 *       https://www.gpsinformation.org/dale/nmea.htm)
 *
 * NMEA frame:
 *    $<sentence>*crc\r\n (crc: 2 hex digits, xor of characters in sentence)
 *
 * The NMEA sentences that matter to us:
 * GPGLL,lat,N|S,long,E|W,time,A,A*crc  - Current position.
 * GPRMC,time,A|V,lat,N|S,long,E|W,speed,course,date,variation,E|W*crc
 * GPGGA,time,lat,N|S,long,E|W,0|1|2|3|4|5|6|7|8,count,hdop,alt,M,sea,M,n/a,n/a
 *
 * Other probably rares:
 * GPTRF,time,date,lat,N|S,long,E|W,alt,iterations,doppler,distance,sat*crc
 * GPZDA,time,day,month,year,timezone,minutes*crc
 *
 * There are 2 possible ways to determine the first sentence of a fix:
 * - timing: the largest delay between sentences indicate a new fix.
 * - time: if the time information changes from one sentence to the next.
 *
 * (Note: if the position changes, then the position's time changes as well.)
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

#include "houseclock.h"
#include "hc_clock.h"
#include "hc_nmea.h"

static int gpsLatency;

static char gpsBuffer[2048]; // 2 seconds of NMEA data, even in worst case.
static int  gpsCount = 0;    // How much NMEA data is stored.

typedef struct {
    char sentence[81]; // NMEA sentence is no more than 80 characters.
    char flags;
    struct timeval timing;
} gpsSentence;

#define GPSFLAGS_NEWFIX    1
#define GPSFLAGS_NEWBURST  2

#define GPS_DEPTH 32

static gpsSentence gpsHistory[GPS_DEPTH];
static int gpsLatest = 0;

// Memorize the latest NMEA values to detect changes.
static char gpsDate[20];
static char gpsTime[20];

static const char *gpsDevice = "/dev/ttyACM0";
static int gpsTty = 0;

static int gpsUseBurst = 0;

const char *hc_nmea_help (int level) {

    static const char *nmeaHelp[] = {
        " [-gps=DEV] [-latency=N] [-burst]",
        "-gps=DEV:     TTY device from which to read the NMEA data.\n"
        "-latency=N:   delay between the GPS fix and the 1st NMEA sentence.\n"
        "-burst:       Use burst start as the GPS timing reference",
        NULL
    };
    return nmeaHelp[level];
}

static void hc_nmea_reset (void) {
    gpsCount = 0;
    gpsLatest = GPS_DEPTH-1;
    int i;
    for (i = 0; i < GPS_DEPTH; ++i) {
        gpsHistory[i].sentence[0] = 0;
    }
    gpsDate[0] = 0;
    gpsTime[0] = 0;
    gpsTty = 0;
}

int hc_nmea_initialize (int argc, const char **argv) {

    int i;
    const char *latency_option = "70";

    gpsDevice = "/dev/ttyACM0";
    gpsUseBurst = 0;

    for (i = 1; i < argc; ++i) {
        hc_match ("-gps=", argv[i], &gpsDevice);
        hc_match ("-latency=", argv[i], &latency_option);
        if (hc_match ("-burst", argv[i], NULL)) {
            gpsUseBurst = 1;
        }
    }
    gpsLatency = atoi(latency_option);

    hc_nmea_reset();
    gpsTty = open(gpsDevice, O_RDONLY);

    hc_clock_initialize (argc, argv);
    return gpsTty;
}


static int hc_nmea_splitlines (int *sentences) {

    int i = 0;
    int count = 0;
    int begin;

    while (i < gpsCount &&
           (gpsBuffer[i] == '\n' || gpsBuffer[i] == '\r')) ++i;
    begin = i;

    for (; i < gpsCount; ++i) {
        if (gpsBuffer[i] == '*') { // Eliminate the CRC part.
            gpsBuffer[i] = 0;
            continue;
        }
        if (gpsBuffer[i] == '\n' || gpsBuffer[i] == '\r') {
            gpsBuffer[i] = 0;
            sentences[count++] = begin;
            while (++i < gpsCount &&
                   (gpsBuffer[i] == '\n' || gpsBuffer[i] == '\r'));
            begin = i;
        }
    }
    sentences[count] = -1; // End of list.
    return begin;
}

static int hc_nmea_splitfields (char *sentence, char *fields[]) {
    int i;
    int count = 0;

    fields[count++] = sentence;

    for (i = 0; sentence[i] > 0; ++i) {
        if (sentence[i] == ',') {
            sentence[i] = 0;
            fields[count++] = sentence + i + 1;
        }
    }
    return count;
}

static char hc_nmea_isnew (const char *received, char *memorized) {
    int i;
    char is_new = 0;
    for (i = 0; received[i] > 0; ++i) {
        if (memorized[i] != received[i]) {
            memorized[i] = received[i];
            is_new = 1;
        }
    }
    if (memorized[i] != 0) {
        memorized[i] = 0;
        is_new = 1;
    }
    return is_new;
}

static int hc_nmea_2digit (const char *ascii) {
    return ascii[1] - '0' + 10 * (ascii[0] - '0');
}

static int hc_nmea_gettime (struct timeval *gmt) {

    time_t now = time(0L);
    struct tm local;

    if ((gpsDate[0] == 0) || (gpsTime[0] == 0)) return 0;

    // Decode the NMEA time into a GMT timeval value.
    // TBD
    localtime_r(&now, &local);
    local.tm_year = 100 + hc_nmea_2digit(gpsDate+4);
    local.tm_mon = hc_nmea_2digit(gpsDate+2) - 1;
    local.tm_mday = hc_nmea_2digit(gpsDate);
    local.tm_hour = hc_nmea_2digit(gpsTime);
    local.tm_min = hc_nmea_2digit(gpsTime+2);
    local.tm_sec = hc_nmea_2digit(gpsTime+4);
    local.tm_isdst = -1;
    gmt->tv_sec = mktime(&local);
    gmt->tv_usec = 0;
    return 1;
}

static int hc_nmea_valid (const char *status, const char *integrity) {

    if ((*status == 'A') && (*integrity == 'A' || *integrity == 'D')) {
        return 1;
    }
    return 0;
}

static void hc_nmea_record (const char *sentence,
                            struct timeval *timing) {

    gpsSentence *decoded;

    if (++gpsLatest >= GPS_DEPTH) gpsLatest = 0;
    decoded = gpsHistory + gpsLatest;

    strncpy (decoded->sentence, sentence, sizeof(decoded->sentence));
    decoded->timing = *timing;
    decoded->flags = 0;
}

static void hc_nmea_mark (int flags) {
    gpsHistory[gpsLatest].flags = flags;
}

static int hc_nmea_decode (char *sentence) {

    char *fields[80]; // large enough for no overflow ever.
    int count;
    int newfix = 0;

    count = hc_nmea_splitfields(sentence, fields);

    if (strcmp ("GPRMC", fields[0]) == 0) {
        // GPRMC,time,A|V,lat,N|S,long,E|W,speed,course,date,variation,E|W,...
        if (count > 12) {
            if (hc_nmea_valid (fields[2], fields[12])) {
                newfix =
                    hc_nmea_isnew(fields[1], gpsTime) |
                    hc_nmea_isnew(fields[9], gpsDate);
            }
        } else {
            DEBUG printf ("Invalid GPRMC sentence: too few fields\n");
        }
    } else if (strcmp ("GPGGA", fields[0]) == 0) {
        // GPGGA,time,lat,N|S,long,E|W,0|1|2|3|4|5|6|7|8,count,...
        if (count > 6) {
            char fix = fields[6][0];
            int  sats = atoi(fields[8]);
            if (fix >= 1 && fix <= 5 && sats >= 3) {
                newfix = hc_nmea_isnew(fields[1], gpsTime);
            }
        } else {
            DEBUG printf ("Invalid GPGGA sentence: too few fields\n");
        }
    } else if (strcmp ("GPGLL", fields[0]) == 0) {
        // GPGLL,lat,N|S,long,E|W,time,A|V,A|D|E|N|S
        if (count > 7) {
            if (hc_nmea_valid (fields[6], fields[7])) {
                newfix = hc_nmea_isnew(fields[5], gpsTime);
            }
        } else {
            DEBUG printf ("Invalid GPGLL sentence: too few fields\n");
        }
    }

    return newfix?GPSFLAGS_NEWFIX:0;
}

static int hc_nmea_ready (int flags) {

    const char *fixinfo = "old";
    const char *burstinfo = "old";

    if (flags & GPSFLAGS_NEWFIX) fixinfo = "new";
    if (flags & GPSFLAGS_NEWBURST) burstinfo = "new";

    if (flags) {
        DEBUG printf ("(%s fix, %s burst)\n", fixinfo, burstinfo);
    }
    return (flags == GPSFLAGS_NEWFIX+GPSFLAGS_NEWBURST);
}

static void hc_nmea_timing (const struct timeval *received,
                            struct timeval *timing, int speed, int count) {

    int64_t usdelta = (count * 1000L) / speed;

    if (usdelta > received->tv_usec) {
        timing->tv_usec = 1000000 + received->tv_usec - usdelta;
        timing->tv_sec = received->tv_sec - 1;
    } else {
        timing->tv_usec = received->tv_usec - usdelta;
        timing->tv_sec = received->tv_sec;
    }
}
                            
int hc_nmea_process (const struct timeval *received) {

    static int64_t gpsTotal = 0;
    static int64_t gpsDuration = 0;
    static struct timeval previous;
    static struct timeval bursttiming;

    static int flags = 0;

    time_t interval;
    int speed;
    int i, j, leftover;
    ssize_t length;
    int sentences [1024]; // Large enough to never overflow.

    length = read (gpsTty, gpsBuffer+gpsCount, sizeof(gpsBuffer)-gpsCount);
    if (length <= 0) {
        close(gpsTty);
        hc_nmea_reset();
        return -1;
    }
    gpsCount += length;

    interval = (received->tv_usec - previous.tv_usec) / 1000 +
               (received->tv_sec - previous.tv_sec) * 1000;

    if (interval < 300) {
        if (gpsTotal > 1000000) {
            gpsTotal /= 2;
            gpsDuration /= 2;
        }
        gpsTotal += length;
        gpsDuration += interval;
    }

    if (gpsDuration > 0) {
        // We multiply the speed by 1000 to get some precision.
        // The other 1000 is because gpsDuration is in milliseconds.
        speed = (1000 * 1000 * gpsTotal) / gpsDuration;
        DEBUG printf ("Calculated speed: %d.%03d Bytes/s\n",
                      speed/1000, speed%1000);
    } else {
        speed = 115000; // Arbitrary speed at the beginning.
    }

    if (previous.tv_usec > 0 && interval > 500) {
        hc_nmea_timing (received, &bursttiming, speed, gpsCount);
        DEBUG printf ("Data received at %d.%03d, burst started at %d.%03d\n",
                      received->tv_sec, received->tv_usec/1000,
                      bursttiming.tv_sec, bursttiming.tv_usec/1000);
        // Whatever GPS time we got before is now old.
        gpsTime[0] = 0;
        flags = GPSFLAGS_NEWBURST;
    }
    previous = *received;

    leftover = hc_nmea_splitlines (sentences);
    if (leftover == 0) return gpsTty;

    for (i = 0; sentences[i] >= 0; ++i) {

        int start = sentences[i];

        // Calculate the timing of the '$'.
        struct timeval timing;
        hc_nmea_timing (received, &timing, speed, gpsCount - start);

        if (gpsBuffer[start++] != '$') continue; // Skip invalid sentence.

        DEBUG {
            printf ("%11d.%03.3d: %s\n",
                    timing.tv_sec, timing.tv_usec/1000, gpsBuffer+start);
        }

        hc_nmea_record (gpsBuffer+start, &timing);

        flags |= hc_nmea_decode (gpsBuffer+start);

        hc_nmea_mark (flags);

        if (hc_nmea_ready(flags)) {
            struct timeval gmt;
            if (hc_nmea_gettime(&gmt)) {
                if (gpsUseBurst)
                   hc_clock_synchronize (&gmt, &bursttiming, gpsLatency);
                else
                   hc_clock_synchronize (&gmt, &timing, gpsLatency);
                flags = 0;
            }
        }
    }

    // Move the leftover to the beginning of the buffer, for future decoding.

    gpsCount -= leftover;
    if (gpsCount > 0)
        memmove (gpsBuffer, gpsBuffer+leftover, gpsCount);

    return gpsTty;
}

