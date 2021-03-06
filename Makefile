
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
	gcc -c -g -O -o $@ $<

houseclock: $(OBJS)
	gcc -g -O -o houseclock $(OBJS) -lhouseportal -lechttp -lssl -lcrypto -lrt

package:
	mkdir -p packages
	tar -cf packages/houseclock-`date +%F`.tgz houseclock init.debian Makefile

install:
	if [ -e /etc/init.d/houseclock ] ; then systemctl stop houseclock ; fi
	mkdir -p /usr/local/bin
	rm -f /usr/local/bin/houseclock /etc/init.d/houseclock
	cp houseclock /usr/local/bin
	cp init.debian /etc/init.d/houseclock
	chown root:root /usr/local/bin/houseclock /etc/init.d/houseclock
	chmod 755 /usr/local/bin/houseclock /etc/init.d/houseclock
	mkdir -p $(SHARE)/public/ntp
	cp public/* $(SHARE)/public/ntp
	chmod 644 $(SHARE)/public/ntp/*
	chmod 755 $(SHARE) $(SHARE)/public $(SHARE)/public/ntp
	if [ -e /etc/init.d/ntp ] ; then systemctl stop ntp ; systemctl disable ntp ; fi
	if [ -e /etc/init.d/chrony ] ; then systemctl stop chrony ; systemctl disable chrony ; fi
	systemctl daemon-reload
	systemctl enable houseclock
	systemctl start houseclock

uninstall:
	systemctl stop houseclock
	systemctl disable houseclock
	rm -f /usr/local/bin/houseclock 
	rm -f /etc/init.d/houseclock
	systemctl daemon-reload
	if [ -e /etc/init.d/ntp ] ; then systemctl enable ntp ; systemctl start ntp ; fi
	if [ -e /etc/init.d/chrony ] ; then systemctl enable chrony ; systemctl start chrony ; fi

purge: uninstall

