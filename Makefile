# houseclock - A simple GPS Time Server with Web console
#
# Copyright 2025, Pascal Martin
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor,
# Boston, MA  02110-1301, USA.
#
# WARNING
#
# This Makefile depends on echttp and houseportal (dev) being installed.

prefix=/usr/local
SHARE=$(prefix)/share/house

INSTALL=/usr/bin/install

HAPP=houseclock

# Application build. --------------------------------------------

OBJS= hc_db.o hc_http.o hc_clock.o hc_tty.o hc_nmea.o hc_broadcast.o hc_ntp.o hc_metrics.o houseclock.o

all: houseclock

broadcast: hc_broadcast.o

main: houseclock.o

ntp: hc_ntp.o

nmea: hc_nmea.o

clock: hc_clock.c

clean:
	rm -f *.o houseclock

rebuild: clean all

%.o: %.c
	gcc -c -Os -o $@ $<

houseclock: $(OBJS)
	gcc -Os -o houseclock $(OBJS) -lhouseportal -lechttp -lssl -lcrypto -lrt

# Minimal tar file for installation -------------------------------

package:
	mkdir -p packages
	tar -cf packages/houseclock-`date +%F`.tgz houseclock init.debian systemd.service public Makefile

# Distribution agnostic file installation -----------------------

install-ui: install-preamble
	$(INSTALL) -m 0755 -d $(DESTDIR)$(SHARE)/public/ntp
	$(INSTALL) -m 0644 public/* $(DESTDIR)$(SHARE)/public/ntp

install-app: install-ui
	$(INSTALL) -m 0755 -s houseclock $(DESTDIR)$(prefix)/bin
	touch $(DESTDIR)/etc/default/houseclock

uninstall-app:
	rm -rf $(DESTDIR)$(SHARE)/public/ntp
	rm -f $(DESTDIR)$(prefix)/bin/houseclock

purge-app:

purge-config:
	rm -f $(DESTDIR)/etc/default/houseclock

# Systemd specific: cleanup of other NTP servers.

clean-systemd::
	if [ "x$(DESTDIR)" = "x" ] ; then if [ -e /etc/init.d/ntp ] ; then systemctl stop ntp ; systemctl disable ntp ; fi ; fi
	if [ "x$(DESTDIR)" = "x" ] ; then if [ -e /etc/init.d/chrony ] ; then systemctl stop chrony ; systemctl disable chrony ; fi ; fi

# System installation. ------------------------------------------

include $(SHARE)/install.mak

