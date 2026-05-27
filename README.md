# seed_edgebox_esp100
## Description

This project implements a bacnet server for the Seed EdgeBox ESP100. 
It is originaly developed for a public lighting project, having PWM outputs to control the brightness of the lamps,depending on the time of the day and bacnet schedules.
It is implemented in C ans uses the BACnet stack from the BACnet Stack project.

## Built With

The project was made with freeRTOS in C, using the ESP-IDF framework for the ESP32 microcontroller available as a VSCode extension. 
The BACnet stack is used for the BACnet/IP communication.

The versions used for the project are:
- ESP-IDF: v5.5.4
- BACnet Stack: v0.8.0
- FreeRTOS: v10.5.1

## Installation  

To build and flash the project to the ESP32, follow these steps:

1. Clone the github repository.
2. Install the ESP-IDF framework and set up the development environment.
3. Configure the project by editing the sdkconfig file to set the wifi/4G credential, GPS coordinates, and other settings.
4. Use the VSCode extension build tool or navigate to the project directory and run `idf.py build` to build the project.
5. Connect the ESP32 to your computer and use the VSCode extension flash tool, or run `idf.py flash` to flash the project to the ESP32.
6. Monitor the serial output using the VSCode extension monitor tool, or any other serial monitor, to see logs and debug information.

## Usage

Once the project is built and flashed to the ESP32, it will start the BACnet server and connect to the configured wifi/4G network.  

You can then use a BACnet client such as yabe to connect to the server and interact with the BACnet objects.

You can also access the http server for configuration and monitoring by entering the ESP32's IP address in a web browser.

The PWM outputs can be used to control the brightness of the lamps based on the schedules and solar calculations.
There is two pwm outputs, called zone A and zone B, they are independent, each having an associated analog value, schedule and trend log object.
The solar calculation provide a flag indicating if the sun is up or down, the PWM values are 100 in case the sun is down and 0 in case the sun is up.

The default weeky schedule will set the PWM values to 0 during the night between 01:00 and 05:00, there is the possiblitity to add special event for specific days and times to set the PWM to a chosen value, this can be done directly from a bacnet client or using the http server. 

There is the possibility of adding an offset to sunset or sunrise time by changing the analog values named "Sunrise offset analog value" and "Sunset offset analog value", the sunrise offset is the time in minutes after sunrise , and the sunset offset is the time in minutes before sunset.

You can also edit the analog output "Zone A output analog value" and "Zone B output analog value" to set the PWM values directly, this will override the schedule and solar calculation until the next scheduled change or solar event.

Trend Log object will log the pwm values each times they change. 

The bacnet device time is the current time in the timezone of the GPS coordinates.

## Features

The following features are implemented:
- BACnet/IP communication
- BACnet objects: Device, Analog Output, Schedule, Trend Log
- Wifi connectivity
- 4G connectivity 
- two PWM GPIO outputs 
- Time synchronization with NTP server
- fram memory for data persistence
- http server for configuration and monitoring
- Solar calculation based on GPS coordinates and time of the day
- 8Mb external psram allow for a large number of bacnet objects and data logging

