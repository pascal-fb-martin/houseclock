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
 * - the estimated timing of the $ in the NMEA sentence (see later).
 * - the GPS UTC time.
 *
 * In order to protect against GPS initialization problems, the module
 * waits for a GPS fix to have been available for 10 seconds before
 * using the GPS time.
 *
 * The module determines the first sentence of a fix cycle as the first
 * sentence in the first data received after a 500ms silence interval.
 *
 * The transmission speed is estimated based on the average transmission time
 * of all subsequent blocks of data within a fix cycle. The initial speed
 * value is set to a reasonable default for a USB pseudo serial.
 *
 * That speed is then used to estimate the actual transmission time for
 * any character in the NMEA stream, and then retrieve when the start of
 * a sentence was received. This estimation is subject to instabilities:
 * the goal is to reach a precision of about 1/10 or 1/100 second, which
 * is way more than needed for a home network.
 *
 * There are two mode for estimating when the GPS data transmission started:
 * - Normal mode: the module considers the first sentence that completed
 *   the GPS fix data (time and position).
 * - Burst mode: the module considers the first sentence of the complete
 *   fix cycle (a.k.a. burst).
 *
 * These two mode are most often equivalent, as many GPS receivers start
 * each cycle with GPSRMC, which provides both the time and position. In
 * that case, the normal mode is more reliable as it relies on the content
 * of the NMEA data instead of timing. This is why it is the default mode.
 *
 * Once the module has decided which sentence to consider, it uses the
 * estimated start time of this sentence, minus the configured latency,
 * as the comparison point with the GPS time, i.e. the local time used
 * to calculate the local time delta.
 *
 * If there is a significant difference, adjtime() is called to correct
 * the local time, unless the delta is too large, in which case the local
 * time is just reset.
 *
 * SYNOPSYS:
 *
 * const char *hc_nmea_help (int level)
 *
 *    Prints the help information, two levels:
 *      0: short help for one-line argument information.
 *      1: multi-line description of each argument.
 *
 * void hc_nmea_initialize (int argc, const char **argv)
 *
 *    Reset the NMEA decoder status, retrieve and store the NMEA options
 *    from the program's command line arguments.
 *
 *    The command line options processed here are:
 *      -gps=<dev>      Name of the system device to read the NMEA data from.
 *      -latency=<N>    Delay between the GPS fix and the 1st sentence (ms).
 *      -burst          Use burst start as the GPS fix timing reference.
 *      -baud=<N>       GPS line baud speed.
 *
 *    The default GPS device is /dev/ttyACM0. If no baud option is used,
 *    the default OS configuration is used.
 *
 *    The latency depends on the GPS device. It can be estimated by using
 *    the options -drift and -latency=0, and then estimating the average
 *    drift, on a machine where the time is already synchronized using NTP.
 *    Default is 70 ms.
 *
 * int hc_nmea_listen (void);
 *
 *    Return the file descriptor to listen to, or else -1 (no device).
 *
 * int hc_nmea_process (const struct timeval *received)
 *
 *    Called when new data is available, with the best know receive time,
 *    typically when the application was notified that data is available.
 *    This time will be associated with the last received byte.
 *
 * void hc_nmea_periodic (const struct timeval *now);
 *
 *    This function must be called at regular interval. It is used to detect
 *    stale NMEA and GPS data.
 *
 * void hc_nmea_active (void);
 *
 *    True if there is an active GPS unit accessible.
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
#include "hc_db.h"
#include "hc_clock.h"
#include "hc_tty.h"
#include "hc_nmea.h"

static int gpsLatency;

static char gpsBuffer[2048]; // 2 seconds of NMEA data, even in worst case.
static int  gpsCount = 0;    // How much NMEA data is stored.

#define GPSFLAGS_NEWFIX    1
#define GPSFLAGS_NEWBURST  2

#define GPS_EXPIRES 5

static const char *gpsDevice = "/dev/ttyACM0";
static int gpsTty = -1;

static int gpsUseBurst = 0;
static int gpsPrivacy = 0;
static int gpsShowNmea = 0;
static int gpsSpeed = 0;

static time_t gpsInitialized = 0;

static hc_nmea_status *hc_nmea_status_db = 0;

const char *hc_nmea_help (int level) {

    static const char *nmeaHelp[] = {
        " [-gps=DEV] [-baud=N] [-latency=N] [-burst] [-privacy]",
        "-gps=DEV:     device from which to read the NMEA data (/dev/ttyACM0).",
        "-latency=N:   delay between the GPS fix and the 1st NMEA sentence (70).",
        "-baud=N:      GPS device's baud speed (default: use OS default).",
        "-show-nmea:   trace NMEA sentences.",
        "-burst:       Use burst start as the GPS timing reference",
        "-privacy:     do not export location",
        NULL
    };
    return nmeaHelp[level];
}

static void hc_nmea_reset (void) {

    int i;
    gpsCount = 0;
    hc_nmea_status_db->fix = 0;
    hc_nmea_status_db->fixtime = 0;
    hc_nmea_status_db->gpsdevice[0] = 0;
    hc_nmea_status_db->gpsdate[0] = 0;
    hc_nmea_status_db->gpstime[0] = 0;
    hc_nmea_status_db->latitude[0] = 0;
    hc_nmea_status_db->longitude[0] = 0;
    hc_nmea_status_db->textcount = 0;
    hc_nmea_status_db->gpscount = 0;

    if (gpsTty >= 0) close(gpsTty);
    gpsTty = -1;
}

void hc_nmea_initialize (int argc, const char **argv) {

    int i;
    const char *latency_option = "70";
    const char *speed_option = "0";

    gpsDevice = "/dev/ttyACM0";
    gpsUseBurst = 0;

    for (i = 1; i < argc; ++i) {
        echttp_option_match ("-gps=", argv[i], &gpsDevice);
        echttp_option_match ("-baud=", argv[i], &speed_option);
        echttp_option_match ("-latency=", argv[i], &latency_option);
        if (echttp_option_present ("-burst", argv[i])) gpsUseBurst = 1;
        if (echttp_option_present ("-privacy", argv[i])) gpsPrivacy = 1;
        if (echttp_option_present ("-show-nmea", argv[i])) gpsShowNmea = 1;
    }
    gpsLatency = atoi(latency_option);
    gpsSpeed = atoi(speed_option);

    if (hc_nmea_status_db == 0) {
        i = hc_db_new (HC_NMEA_STATUS, sizeof(hc_nmea_status), 1);
        if (i != 0) {
            fprintf (stderr,
                     "[%s %d] cannot create %s: %s\n",
                     __FILE__, __LINE__, HC_NMEA_STATUS, strerror(i));
            exit(1);
        }
        hc_nmea_status_db = (hc_nmea_status *) hc_db_get (HC_NMEA_STATUS);
    }

    hc_nmea_reset();
    hc_nmea_listen ();

    gpsInitialized = time(0);
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

    char *gpsDate = hc_nmea_status_db->gpsdate;
    char *gpsTime = hc_nmea_status_db->gpstime;

    if ((gpsDate[0] == 0) || (gpsTime[0] == 0)) return 0;

    // Decode the NMEA time into a GMT timeval value.
    // TBD: GPS rollover?
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

    if (++(hc_nmea_status_db->gpscount) >= HC_NMEA_DEPTH)
        hc_nmea_status_db->gpscount = 0;
    decoded = hc_nmea_status_db->history + hc_nmea_status_db->gpscount;

    strncpy (decoded->sentence, sentence, sizeof(decoded->sentence));
    decoded->timing = *timing;
    decoded->flags = 0;
}

static void hc_nmea_mark (int flags, const struct timeval *timestamp) {
    hc_nmea_status_db->history[hc_nmea_status_db->gpscount].flags = flags;
    hc_nmea_status_db->timestamp = *timestamp;
}

static void hc_nmea_store_position (char **fields) {
    if (! gpsPrivacy) {
        strncpy (hc_nmea_status_db->latitude,
                 fields[0], sizeof(hc_nmea_status_db->latitude));
        strncpy (hc_nmea_status_db->longitude,
                 fields[2], sizeof(hc_nmea_status_db->longitude));
        hc_nmea_status_db->hemisphere[0] = fields[1][0];
        hc_nmea_status_db->hemisphere[1] = fields[3][0];
    }
    hc_nmea_status_db->fix = 1;
    hc_nmea_status_db->fixtime = time(0);
}

static int hc_nmea_is_valid_talker (const char *name) {

    // We only accept GP (GPS), GA (Galileo) and GL (Glonass).
    static char isvalid[128] = {0};
    if (!isvalid['P']) {
        isvalid['P'] = isvalid['A'] = isvalid['L'] = 1;
    }

    if (name[0] != 'G') return 0;
    return (int)(isvalid[name[1] & 0x7f]);
}

static int hc_nmea_decode (char *sentence) {

    char *fields[80]; // large enough for no overflow ever.
    int count;
    int newfix = 0;

    char *gpsDate = hc_nmea_status_db->gpsdate;
    char *gpsTime = hc_nmea_status_db->gpstime;

    count = hc_nmea_splitfields(sentence, fields);

    if (!hc_nmea_is_valid_talker(fields[0])) return 0;

    const char *message = fields[0] + 2;

    if (strcmp ("RMC", message) == 0) {
        // GPRMC,time,A|V,lat,N|S,long,E|W,speed,course,date,variation,E|W,...
        if (count > 12) {
            if (hc_nmea_valid (fields[2], fields[12])) {
                newfix =
                    hc_nmea_isnew(fields[1], gpsTime) |
                    hc_nmea_isnew(fields[9], gpsDate);
                if (newfix) hc_nmea_store_position (fields+3);
            } else {
                hc_nmea_status_db->fix = 0;
            }
        } else {
            DEBUG printf ("Invalid RMC sentence: too few fields\n");
        }
    } else if (strcmp ("GGA", message) == 0) {
        // GPGGA,time,lat,N|S,long,E|W,0|1|2|3|4|5|6|7|8,count,...
        if (count > 6) {
            char fix = fields[6][0];
            int  sats = atoi(fields[7]);
            if (fix >= '1' && fix <= '5' && sats >= 3) {
                newfix = hc_nmea_isnew(fields[1], gpsTime);
                if (newfix) hc_nmea_store_position (fields+2);
            } else {
                hc_nmea_status_db->fix = 0;
            }
        } else {
            DEBUG printf ("Invalid GGA sentence: too few fields\n");
        }
    } else if (strcmp ("GLL", message) == 0) {
        // GPGLL,lat,N|S,long,E|W,time,A|V,A|D|E|N|S
        if (count > 7) {
            if (hc_nmea_valid (fields[6], fields[7])) {
                newfix = hc_nmea_isnew(fields[5], gpsTime);
                if (newfix) hc_nmea_store_position (fields+1);
            } else {
                hc_nmea_status_db->fix = 0;
            }
        } else {
            DEBUG printf ("Invalid GLL sentence: too few fields\n");
        }
    } else if (strcmp ("TXT", message) == 0) {
        int count = hc_nmea_status_db->textcount;
        if (count < HC_NMEA_TEXT_LINES) {
            strncpy (hc_nmea_status_db->text[count].line,
                 fields[4], sizeof (hc_nmea_status_db->text[0].line));
            hc_nmea_status_db->textcount += 1;
        }
    }

    return newfix?GPSFLAGS_NEWFIX:0;
}

static int hc_nmea_ready (int flags) {

    if (gpsShowNmea && flags) {
        printf ("(%s fix, %s burst)\n",
                (flags & GPSFLAGS_NEWFIX) ? "new" : "old",
                (flags & GPSFLAGS_NEWBURST) ? "new" : "old");
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

    // gpsTotal and gpsDuration are used to estimate the NMEA data's
    // transfer speed. They represent accumulated statistics that are never
    // reset, only divided by two when the values are too large (to lower
    // the weight of older samples).
    // Since the NMEA data arrives in periodic burst, this code cannot
    // count the interval between two bursts in this estimate. To skip
    // these intervals, it ignores the first block of data received after
    // a 300ms "silence". (Might need improvement if all the NMEA data is
    // received as a single block, in which case gpsTotal and gpsDuration
    // would never be incremented.)
    //
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

    if (gpsCount == sizeof(gpsBuffer)) {
        gpsCount = 0; // Buffer should never be full: forget accumulated data.
    }
    length = read (gpsTty, gpsBuffer+gpsCount, sizeof(gpsBuffer)-gpsCount);
    if (length <= 0) {
        hc_nmea_reset();
        return -1;
    }
    gpsCount += length;

    // Calculate timing.
    //
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
        if(gpsShowNmea)
            printf ("Calculated speed: %d.%03d Bytes/s\n",
                    speed/1000, speed%1000);
    } else {
        speed = 115000; // Arbitrary speed at the beginning.
    }

    if (previous.tv_usec > 0 && interval > 500) {
        hc_nmea_timing (received, &bursttiming, speed, gpsCount);
        if (gpsShowNmea) {
            printf ("Data received at %d.%03d, burst started at %d.%03d\n",
                     received->tv_sec, received->tv_usec/1000,
                     bursttiming.tv_sec, bursttiming.tv_usec/1000);
        }
        // Whatever GPS time we got before is now old.
        hc_nmea_status_db->gpsdate[0] = hc_nmea_status_db->gpstime[0] = 0;
        flags = GPSFLAGS_NEWBURST;
    }
    previous = *received;

    // Analyze the NMEA data we have accumulated.
    //
    leftover = hc_nmea_splitlines (sentences);

    for (i = 0; sentences[i] >= 0; ++i) {

        int start = sentences[i];

        // Calculate the timing of the '$'.
        struct timeval timing;
        hc_nmea_timing (received, &timing, speed, gpsCount - start);

        if (gpsBuffer[start++] != '$') continue; // Skip invalid sentence.

        if (gpsShowNmea) {
            printf ("%11d.%03.3d: %s\n",
                    timing.tv_sec, timing.tv_usec/1000, gpsBuffer+start);
        }

        hc_nmea_record (gpsBuffer+start, &timing);

        flags |= hc_nmea_decode (gpsBuffer+start);

        hc_nmea_mark (flags, &bursttiming);

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

    if (leftover > 0) {
        gpsCount -= leftover;
        if (gpsCount > 0)
            memmove (gpsBuffer, gpsBuffer+leftover, gpsCount);
    }

    return gpsTty;
}

void hc_nmea_convert (char *buffer, int size,
                      const char *source, char hemisphere) {
    char *sep;
    int digits;
    if (hemisphere == 'W' || hemisphere == 'S') {
        buffer[0] = '-';
        buffer += 1;
        size -= 1;
    }
    sep = strchr (source, '.');
    if (sep) {
        digits = sep - source - 2;
    } else {
        digits = strlen(source) - 2;
    }
    strncpy (buffer, source, digits);
    buffer[digits] = 0;
    double degrees = atoi(buffer);
    double minutes = atof(source+digits);
    snprintf (buffer, size, "%f", degrees + (minutes / 60.0));
}


void hc_nmea_periodic (const struct timeval *now) {

    // Do not check during initialization.
    if ((gpsInitialized == 0) || (hc_nmea_status_db == 0)) return;
    if (now->tv_sec <= gpsInitialized + GPS_EXPIRES) return;

    if (now->tv_sec > hc_nmea_status_db->timestamp.tv_sec + GPS_EXPIRES) {
        if (gpsShowNmea) {
            printf ("GPS data expired at %u\n", (unsigned int)now->tv_sec);
        }
        if (gpsTty >= 0) {
            hc_nmea_reset();
        }
    }
}

int hc_nmea_listen (void) {
    if (gpsTty >= 0) return gpsTty;

    static time_t LastTry = 0;
    time_t now = time(0);
    if (now < LastTry + 5) return gpsTty;

    LastTry = now;
    gpsTty = open(gpsDevice, O_RDONLY);
    if (gpsTty < 0) return gpsTty;

    // Remove echo of characters from the GPS device.
    hc_tty_set (gpsTty, gpsSpeed);
    snprintf (hc_nmea_status_db->gpsdevice,
              sizeof(hc_nmea_status_db->gpsdevice), "%s", gpsDevice);
    return gpsTty;
}

int hc_nmea_active (void) {
    if (gpsTty < 0 ) return 0;
    if (hc_nmea_status_db == 0) return 0;
    if (hc_nmea_status_db->fixtime + GPS_EXPIRES < time(0)) return 0;
    return 1;
}

