
OBJS= hc_db.o hc_http.o hc_clock.o hc_tty.o hc_nmea.o hc_broadcast.o hc_ntp.o houseclock.o

SHARE=/usr/local/share/house

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

install-files:
	mkdir -p /usr/local/bin
	rm -f /usr/local/bin/houseclock
	cp houseclock /usr/local/bin
	chown root:root /usr/local/bin/houseclock
	chmod 755 /usr/local/bin/houseclock
	mkdir -p $(SHARE)/public/ntp
	cp public/* $(SHARE)/public/ntp
	chmod 644 $(SHARE)/public/ntp/*
	chmod 755 $(SHARE) $(SHARE)/public $(SHARE)/public/ntp

uninstall-files:
	rm -rf $(SHARE)/public/ntp
	rm -f /usr/local/bin/houseclock 

purge-config:

# Distribution agnostic systemd support -------------------------

install-systemd:
	if [ -e /lib/systemd/system/systemd-timesyncd.service ] ; then systemctl stop systemd-timesyncd ; systemctl disable systemd-timesyncd ; fi
	if [ -e /etc/init.d/ntp ] ; then systemctl stop ntp ; systemctl disable ntp ; fi
	if [ -e /lib/systemd/system/ntp.service ] ; then systemctl stop ntp ; systemctl disable ntp ; fi
	if [ -e /etc/init.d/chrony ] ; then systemctl stop chrony ; systemctl disable chrony ; fi
	if [ -e /lib/systemd/system/chrony.service ] ; then systemctl stop chrony ; systemctl disable chrony ; fi
	cp systemd.service /lib/systemd/system/houseclock.service
	chown root:root /lib/systemd/system/houseclock.service
	systemctl daemon-reload
	systemctl enable houseclock
	systemctl start houseclock

uninstall-systemd: stop-systemd
	if [ -e /lib/systemd/system/systemd-timesyncd.service ] ; then systemctl enable systemd-timesyncd ; systemctl start systemd-timesyncd ; fi
	if [ -e /etc/init.d/ntp ] ; then systemctl enable ntp ; systemctl start ntp ; fi
	if [ -e /lib/systemd/system/ntp.service ] ; then systemctl enable ntp ; systemctl start ntp ; fi
	if [ -e /etc/init.d/chrony ] ; then systemctl enable chrony ; systemctl start chrony ; fi
	if [ -e /lib/systemd/system/chrony.service ] ; then systemctl start chrony ; systemctl start chrony ; fi

stop-systemd:
	if [ -e /etc/init.d/houseclock ] ; then systemctl stop houseclock ; systemctl disable houseclock ; rm -f /etc/init.d/houseclock ; fi
	if [ -e /lib/systemd/system/houseclock.service ] ; then systemctl stop houseclock ; systemctl disable houseclock ; rm -f /lib/systemd/system/houseclock.service ; fi

# Debian GNU/Linux install --------------------------------------

install-debian: stop-systemd install-files install-systemd

uninstall-debian: uninstall-systemd uninstall-files

purge-debian: uninstall-debian purge-config

# Void Linux install --------------------------------------------

install-void: install-files

uninstall-void: uninstall-files

purge-void: uninstall-void purge-config

# Default install (Debian GNU/Linux) ----------------------------

install: install-debian

uninstall: uninstall-debian

purge: purge-debian

