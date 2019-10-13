# HouseClock - A GPS Network Time Server with Web console

This is a project to create a simple stratum 1 NTP server based on a local GPS time source for home use. This project depends on [echttp](https://github.com/pascal-fb-martin/echttp).

The main goal is to setup a local time server not dependent on an Internet link, easy to install, simple to configure and that can be monitored from a web browser.

Why not use gpsd and ntpd? These are designed for PPS, which is not supported by most USB GPS receivers. The installation instructions are quite long and arcane. Many people have done it, but the average person will find it technically demanding.

PPS is not necessary: 1/10 second accuracy is enough for home uses and my USB GPS receiver sends the GPS fix within a few milliseconds anyway. PPS is more useful for a slow GPS connection. On the flip side, the GPS receiver likely introduce a calculation latency..

For slower speed USB receivers, a solution is to detect the beginning of each GPS cycle, and calculate the timing of this cycle based on the receive time, the count of characters received and the transmission speed. GPS receivers tend to send the fix information in the first NMEA sentence, reducing the error estimate.

Tests on a NTP-synchronized workstation with the VK-162 Usb GPS Navigation Module that I bough for less than $15 show random variations that remain within a -10ms to 10ms range. The average delta with the synchronized time *slowly* changes between 40ms and 0ms: it could be an effect of fluctuation within the NTP synchronization itself.

