# HouseClock - A GPS Simple Network Time Server with Web console

# Overview

This is a project to create a stratum 1 SNTP server based on a local GPS time source for home use. This project depends on [echttp](https://github.com/pascal-fb-martin/echttp).

The main goal is to setup a local time server not dependent on an Internet link, that is easy to install, simple to configure and that can be monitored from a web browser. It automatically runs as a client when no GPS device is available.

# What Makes HouseClock Special?

This time server does not depend on a PPS (Pulse Per Second) interface. It works with any GPS device that sends common NMEA sentences, including cheap USB GPS receivers. It is easy to configure through a few command line options, with the defaults working with most Linux configurations.

Unless debug mode was explicitly enabled, this software does not produce any log (periodic log could trash a SD card or eMMC storage), but its status is easily accessible through a Web interface.

Why not use gpsd and ntpd? These are designed for PPS, which is not supported by most (any?) USB GPS receivers. The installation instructions are quite long and arcane. Many people have done it, but the average person will find it technically demanding.

PPS is not necessary: 1/10 second accuracy is more than enough for home use cases and most USB GPS receivers send the GPS fix within a few milliseconds anyway. On the flip side, the GPS receiver likely introduce a calculation latency. PPS is only needed with a slow GPS connection, and when really high accuracy is required, typically for scientific purpose..

For slower speed USB receivers, this software detects the beginning of each GPS cycle, and calculates the timing of this cycle based on the receive time, the count of characters received and the transmission speed. GPS receivers tend to send the fix information in the first NMEA sentence, reducing the error estimate.

Tests on a NTP-synchronized workstation with the VK-162 Usb GPS Navigation Module that can be bough for less than $15 show random variations that remain within a -10ms to 10ms range. The average delta with the synchronized time *slowly* changes between 40ms and 0ms: it could be an effect of fluctuation within the NTP synchronization itself (NTP pool on the Internet).

# Server Mode

This software runs as a stratum 1 time server if a GPS device is detected and a fix was obtained. When server, it answers to client requests and sends periodic NTP messages in local broadcast mode.

# Client Mode

When no GPS device is available, the software acts as a NTP broadcast client, listening to NTP broadcast messages. In this mode it will select a specific server as time source and stick to this server as long as it operates. If the current time source disappears, the client will switch to another available time source.

In client mode, and while synchronized to a NTP broadcast server, the software acts as a NTP unicast server, stratum level set to the broadcast server's level plus one (i.e. stratum 2 is the broadcast server is stratum 1).

# Installation

* Clone this GitHub repository.
* make
* sudo make install

This installs HouseClock as a service (for Debian's systemd) and starts it. If ntpd or chrony was installed, this stops and disables it.

# Configuration

With default options, HouseClock will read from the GPS device at /dev/ttyACM0, listen for HTTP requests on the http tcp port (80) and handle NTP communication on the ntp udp port (123).

All these default can be changed through command line options. For example, if the GPS device is different and the http port is already in use (httpd is running), one can force other values using the -http-service and -gps options:
```
houseclock -http-service=8080 -gps=/dev/tty0
```

For more information about available options, a complete help is available:
```
houseclock -h
````

The service options can be configured by creating file /etc/default/houseclock and set the following shell variables:

**GPS_OPTS**: GPS related options.
**NTP_OPTS**: NTP related options.
**HTTP_OPTS**: HTTP related options.
**OTHER_OPTS**: general purpose options.

For example:
```
GPS_OPTS="-gps=/dev/tty0"
NTP_OPTS="-ntp-period=30"
HTTP_OPTS="-http-service=8080"
```

