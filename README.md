# HouseClock - A Simple Network Time Server with GPS synchronization and Web console

## Overview

This is a project to create a stratum 1 SNTP server based on a local GPS time source for home use. This project depends on [echttp](https://github.com/pascal-fb-martin/echttp) and [HousePortal](https://github.com/pascal-fb-martin/houseportal).

The main goal is to setup a local time server on a Raspberry Pi (or any Linux server) that is not dependent on an Internet link, is easy to install, simple to configure and that can be monitored from a web browser. It automatically runs as a client when no GPS device is available.

This software is used to maintain a reasonably accurate time for eight IP security cameras and one sprinkler controller.

## What Makes HouseClock Different?

This time server does not depend on a PPS (Pulse Per Second) interface. It works with any GPS device that sends common NMEA sentences, including cheap USB GPS receivers. It is easy to configure through a few command line options, with the defaults working with most Linux configurations.

Why not use gpsd and ntpd? These are designed for PPS, which is not supported by most (any?) USB GPS receivers. The installation instructions are quite long and arcane. Many people have done it, but the average person will find it technically demanding. In addition, GPS receiver with PPS output are more difficult to find and more expensive.

PPS is not necessary for most people: 1/10 second accuracy is more than enough for home use cases and most USB GPS receivers send the GPS fix within a few milliseconds anyway. On the flip side, the GPS receiver likely introduces a calculation latency, which might be variable. PPS is only needed with a slow GPS connection, or when really high accuracy is required, typically for scientific purpose..

Unless debug mode was explicitly enabled, this software does not produce any traditional log (periodic log could trash a SD card or eMMC storage), however its status is easily accessible through a Web interface. The web interface provides both the current time and GPS information, and an history of events that covers interactions with time providers and clients.

## Accuracy

By default the software tries to maintain a 10ms accuracy goal, i.e. it start adjusting the OS time when the drift is outside the -10ms to 10ms range. That goal is adjustable (option -precision=N) but 10ms is the best one can reasonably expect. This software does not support any accuracy goal below 10ms.

The software runs two threads: one for time synchronization and NTP communication (high priority thread), the other for the web server (low priority thread).

To further minimise timing errors, this software detects the beginning of each GPS cycle, and calculates the timing of this cycle based on the receive time, the count of characters received and the transmission speed. GPS receivers tend to send the fix information in the first NMEA sentence, reducing the error estimate.

The delta between the OS time and the GPS time is still subject to some unpredictable variations due to OS scheduling, network and clock interrupts, or the GPS device internal timing. In order to avoid readjusting the clock continuously due to these, an average drift is calculated over a 10 seconds period when the time source is a GPS device.

Tests show random variations that remain within a -20ms to 20ms range. The test was conducted in the following conditions:

- VK-162 USB GPS Navigation Module (bought for less than $15).
- Raspberry Pi 4, overall sustained CPU usage of around 20% (running motion).
- Two Ethernet interfaces (one receiving four VGA video streams continuously).

During that test, the Raspberry Pi clock experienced an averag drift of about 2ms to 4ms every 10 seconds, resulting into a cycle of adjustment every minute or so (adjust the clock down for about 30s, then let the clock drift for another 30s).

> [!CAUTION]
> if another time synchronization service is running, then the time delta may sometimes jump in the -50ms to 50ms range. To avoid this, the make install target detects and disables systemd-timesyncd, chrony and ntpd.

## Server Mode

This software runs as a stratum 1 time server if a GPS device is detected and a fix was obtained. When server, it answers to client requests and sends periodic NTP messages in local broadcast mode.

## Client Mode

When no GPS device is available, the software acts as a NTP broadcast client, listening to NTP broadcast messages. In this mode it will select a specific server as time source and stick to this server as long as it operates. If the current time source disappears, the client will switch to another available time source.

In client mode, and while synchronized to a NTP broadcast server, the software acts as a NTP unicast server, stratum level set to the broadcast server's level plus one (i.e. stratum 2 if the broadcast server is stratum 1).

## Installation

* It is recommended to remove ModemManager, unless needed. On Debian: `sudo apt purge modemmanager ; sudo apt autoremove`. (The ModemManager service automatically tries to talk to USB serial devices, assuming that it must be a modem. This may cause occasional GPS link failures. Even while the GPS link is apparently quickly recovered, this is an annoyance easily avoided.)
* Install the OpenSSL development package(s).
* Install [echttp](https://github.com/pascal-fb-martin/echttp).
* Install [HousePortal](https://github.com/pascal-fb-martin/houseportal).
* Clone this GitHub repository.
* make
* sudo make install

This installs HouseClock as a service (for Debian's systemd) and starts it. If ntpd or chrony was installed, this stops and disables it. This also disables systemd-timesyncd.

## Configuration

With default options, HouseClock will read from the GPS device at /dev/ttyACM0, apply a GPS receiver latency of 70ms, listen for HTTP requests on a dynamic tcp port number and handle NTP communication on the ntp udp port (123).

All these default can be changed through command line options. For example, if the GPS device is different, [HousePortal](https://github.com/pascal-fb-martin/houseportal) was not installed and the http port is already in use (httpd is running), one can force other values using the -http-service and -gps options:
```
houseclock -http-service=8080 -gps=/dev/tty0
```

If HousePortal has been installed, you can let HouseClock use a dynamic port number that it will register with HousePortal. (Note that HouseClock does not currently sign its redirect message to HousePortal.) The benefit of using HousePortal is that all your local http applications will share access through port 80, without having to manually assign port numbers. For example "http://machine/ntp/status" will be redirected to "http://machine:N/ntp/status" (where N is the current HouseClock HTTP port).

For more information about the supported options, a complete help is available:
```
houseclock -h
```

The service options can be configured by creating file /etc/default/houseclock and set the following shell variables:

* **GPS_OPTS**: GPS related options.
* **NTP_OPTS**: NTP related options.
* **HTTP_OPTS**: HTTP related options.
* **OTHER_OPTS**: general purpose options.

For example:
```
GPS_OPTS="-gps=/dev/tty0"
NTP_OPTS="-ntp-period=30"
HTTP_OPTS="-http-service=8080"
```

## Calibration

It is possible to verify the accuracy of the time synchronization after installation by running the houseclock program in the foreground with test mode enabled and a reference NTP server. For example:
```
houseclock -test -ntp-service=0 -ntp-reference=3.debian.pool.ntp.org
```

(The `-ntp-service=0` option is used to avoid a port conflict with the running houseclock service.)

The program then prints the estimated offset of the local time compared to the NTP server time: positive if the local time is less than the NTP server time, negative if it is greater.

Please do not run houseclock in such a calibration mode for long periods, as this generates a significant rate of requests to the NTP server.

If the offset shown oscillates around a stable value that is outside of the interval [-10ms, 10ms], one can add this offset's average to the GPS receiver latency using the `-latency` option. For example if the average offset with the reference NTP server is 30ms and the default latency of 70ms was used, the time discrepancy can be corrected by adding the option `-latency=100` when launching the HouseClock service. (The sign of the offset matters: if the average offset was -30ms, the GPS latency to use would be 40.)

