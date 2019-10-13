
OBJS= hc_db.o hc_http.o hc_clock.o hc_nmea.o hc_broadcast.o hc_ntp.o houseclock.o

all: houseclock

broadcast: hc_broadcast.o

main: houseclock.o

ntp: hc_ntp.o

nmea: hc_nmea.o

clock: hc_clock.c

clean:
	rm -f *.o houseclock

%.o: %.c
	gcc -c -g -O -o $@ $<

houseclock: $(OBJS)
	gcc -g -O -o houseclock $(OBJS) -lechttp -lrt

