# Seeed EdgeBox ESP100 BACnet Lighting Controller

This smart lighting controller is a simple and reliable BACnet solution aimed to be used with a remote BACnet client (4G private APN) in smart cities. In uses the  <a href="https://www.seeedstudio.com/EdgeBox-ESP-100-p-5490.html" target="_blank">Seeed EdgeBox ESP100</a>.

I (5876) RFM: [TIME INIT] Source: NTP (4G connected) alors qu'on est en WiFi

## Overview

This firmware runs on ESP32-S3 (Seeed EdgeBox ESP100) and provides:

- BACnet/IP server
- Network connectivity through either Wi-Fi or 4G/LTE
- 2 Analog Output (PWM??) lighting zones (Zone A and Zone B)
- Ephemeris managment (sunrise/sunset time according to GPS coordinates)
- BACnet Weekly schedules and special events to control ligthning
- Persistent storage of runtime state (BACnet object values) in FRAM
- Local HTTP dashboard for monitoring and configuration
- RTC support to keep track of time
- NTP time update

The project is implemented in C wtih ESP-IDF framework (FreeRTOS) and uses an open source BACnet stack (<a href="https://github.com/bacnet-stack/bacnet-stack" target="_blank">bacnet-stack</a>).

## Requirements

- ESP-IDF (project has been developed with v5.5.x) :
- VScode extension (but should work with other ESP-IDF installation)
- EdgeBox ESP100 : 
    - CPU : ESP32-S3
    - Internal RAM : 512 KB
    - External RAM (PSRAM) : 8 MB
    - Flash : 16 MB
    - WiFi / BLE / 4G
    - 4 Digital Inputs, 6 Digital Outputs
    - 4 Analog Inputs, 2 Analog Outputs
- 
## Main Capabilities

- BACnet objects initialized at startup:
	- 2 Schedules (1 for each zone)
	- 2 Analog Values (1 for each zone)
	- 2 Trend Log (1 for each Analog Value)
- Two zones con be controlled:
	- AV0 mapped to PWM GPIO42 (Zone A)
	- AV1 mapped to PWM GPIO41 (Zone B)
- Solar control behavior:
	- Daytime: when BACnet schedule reach a daytime, AV0 and AV1 are forced to 0% by internal logic.
	- Night time: when BACnet schedule read a nigh time, AV0 and AV1 output follows each zone schedule/special events.
	- Force lightning : AV0 and AV1 can be force to a specific value at any time (day and night) using BACnet Write service. This will turn on/off light even during daytime.
- Logging :
	- Trend log  object log changes of value for AV0 and AV1. 
	- Boot, sync, connectivity status , and value changes persisted in FRAM event log
- Local configuration and debug (HTTP Server)
	- History trend log available on the user interface
	- FRAM logs available on the user interface
	- Special events configuration with the user interface
- Time handling:
	- NTP synchronization
	- Timezone inferred from configured solar latitude/longitude


## Web Interface and API

The embedded web UI is served on port 80. It includes:

- Live zone A and B status (values of AV1 and AV0)
- Sunrise and sunset time calculation (lat/long + configurable offsets)
- Special events management
- Event log view
- Trend history view

## Default Scheduling

At startup, a default weekly schedules is applied for each schedule object:
- 17:00 to 01:00 -> 100%
- 05:00 to 10:00 -> 100%

But the output still depends on daylight.

## Project Structure

- main
- components/bacnetserver: BACnet service handlers and object table setup
- components/httpserver: embedded web app + REST API
- components/solar: sunrise/sunset and timezone logic
- components/wifi: Wi-Fi connection management
- components/modem: SIM7600 PPP connection and reconnect logic
- components/gpiooutputs: LEDC PWM output control
- components/fram: FRAM read/write access
- components/rtc: RTC driver
- components/ntp: time synchronisation

## Installation, Build, Flash, Monitor

1. Clone this github repository.
2. Install the ESP-IDF framework and set up the development environment.
3. Run the target selection (esp32s3) `idf.py set-target esp32s3`
4. Run menuconfig `idf.py menuconfig` to configure the project :
   - the connectivity mode (WiFi or 4G)
   - MODEM_APN : the 4G APN (in case of 4G connectivity)
   - WIFI_SSID / WIFI_PASSWORD : the WiFi credentials (in case of WiFi connectivity)
   - the GPS coordinates (in order to calculate day and night time)
   - BACNET_DEVICE_NAME / BACNET_DEVICE_ID : the BACnet device id and name. 
5. Run `idf.py build` to build the project.
6. Connect the ESP32 to your computer and select the right COM port.
7. Run `idf.py flash` to flash the project to the ESP32.
8. Monitor the serial output using any serial monitor (115200 bps) to see logs and debug information.
