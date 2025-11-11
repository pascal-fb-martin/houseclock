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
 * hc_tty.c - Handle setting up a TTY device.
 *
 * This module hides the TTY configuration's OS interface.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/termios.h>

#include "hc_tty.h"

int hc_tty_set (int fd, int baud) {

    int speed;
    struct termios settings;

    if (! isatty(fd)) return 0; // No need to setup anything.

    // When running as a service, this might become the controlling TTY.
    // Since HouseClock is designed to survice GPS failure, it must ignore
    // any TTY failure signal.
    signal (SIGHUP, SIG_IGN);

    if (tcgetattr(fd,&settings) != 0) return errno;

    // Optionally set the speed (if not 0). This is mostly useless with
    // current USB GPS devices, but let's still allow it for special cases.

    switch(baud) {
        case      0: speed =      B0; break;
        case     50: speed =     B50; break;
        case     75: speed =     B75; break;
        case    110: speed =    B110; break;
        case    134: speed =    B134; break;
        case    150: speed =    B150; break;
        case    200: speed =    B200; break;
        case    300: speed =    B300; break;
        case    600: speed =    B600; break;
        case   1200: speed =   B1200; break;
        case   1800: speed =   B1800; break;
        case   2400: speed =   B2400; break;
        case   4800: speed =   B4800; break;
        case   9600: speed =   B9600; break;
        case  19200: speed =  B19200; break;
        case  38400: speed =  B38400; break;
        case  57600: speed =  B57600; break;
        case 115200: speed = B115200; break;
#ifdef B230400
        case 230400: speed = B230400; break;
#endif
#ifdef B460800
        case 460800: speed = B460800; break;
#endif
#ifdef B921600
        case 921600: speed = B921600; break;
#endif
        default: speed = B4800; break;
    }

    if (speed != B0) {
        cfsetispeed(&settings, speed);
        cfsetospeed(&settings, speed);
    }

    // Set the TTY as raw & non-blocking, 8 bits no parity, 1 stop bit,
    // and VMIN = 0 and VTIME = 0 (return NMEA data as soon as received,
    // so that the timing calculations means something).

    memset(settings.c_cc, 0, sizeof(settings.c_cc));

    settings.c_iflag = settings.c_oflag = settings.c_lflag = 0;

    settings.c_cflag &= ~(CSTOPB | PARENB | PARODD | CRTSCTS | CSIZE);
    settings.c_cflag |= (CREAD | CLOCAL | CS8);

    if (tcsetattr(fd, TCSANOW, &settings) != 0) return errno;
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & !O_NONBLOCK) != 0) return errno;

    tcflush(fd, TCIOFLUSH);
    return 0;
}

#ifdef TTY_TEST
int main (int argc, char **argv) {

    hc_tty_set (open (argv[1], O_RDWR|O_NONBLOCK|O_NOCTTY), 4800);

    // Just wait until killed.
    for (;;) sleep(1);
}
#endif

