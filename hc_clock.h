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
 * hc_clock.h - System time synchronization module.
 */
const char *hc_clock_help (int level);

void hc_clock_initialize   (int argc, const char **argv);
void hc_clock_synchronize  (const struct timeval *source,
                            const struct timeval *local, int latency);
int  hc_clock_synchronized (void);
void hc_clock_reference    (struct timeval *reference);
int  hc_clock_dispersion   (void);

/* Live database.
 */
#define HC_CLOCK_STATUS "ClockStatus"

typedef struct {
    struct timeval cycle;
    struct timeval reference;
    int   drift;
    int   avgdrift;
    short precision;
    char  synchronized;
    char  count;
    int   accumulator;
    int   sampling;
} hc_clock_status;

#define HC_CLOCK_METRICS "ClockMetrics"

typedef struct {
    int drift;
    int adjust;
} hc_clock_metrics;

