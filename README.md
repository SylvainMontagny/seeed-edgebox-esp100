# Seeed EdgeBox ESP100 BACnet Lighting Controller

This smart lighting controller is a simple and reliable BACnet solution aimed to be used with a remote BACnet client in smart cities. In uses the  <a href="https://www.seeedstudio.com/EdgeBox-ESP-100-p-5490.html" target="_blank">Seeed EdgeBox ESP100</a>.

The project is implemented in C wtih ESP-IDF framework and uses an open source BACnet stack (<a href="https://github.com/bacnet-stack/bacnet-stack" target="_blank">bacnet-stack</a>).

### Known issues
- 4G connexion is weak and instable.

## 1. Overview
### Application capabilities
This firmware runs on ESP32-S3 (Seeed EdgeBox ESP100) and provides:
- **BACnet/IP server**
- Network connectivity through either **Wi-Fi** or **4G/LTE**
- **2 BACnet Analog Output** for controlling lighting zones (Zone A and Zone B). 
	- AV0 is mapped to ESP32 analog output pin GPIO42 (PWM low pass filtered) for Zone A.
	- AV1 is mapped to ESP32 analog output pin GPIO41 (PWM low pass filtered) for Zone B.
- **2 BACnet schedules** (Weekly and special events) to plan ligthning timetable.
- **2 BACnet Trend logs** (for the 2 Analog Output AV0 and AV1).
- **Ephemeris automatic updates** (sunrise/sunset time according to GPS coordinates and time of year)
- **Persistent storage** of runtime state (BACnet object values, schedules) in FRAM
- **Local HTTP dashboard** for monitoring and configuration
- BACnet Time is maintained :
  - with local **RTC**
  - with NTP 

### Requirements
- **ESP-IDF** (project has been developed with v5.5.x) :
- **VScode extension** (but should work with other ESP-IDF installation)
- Seed Studio EdgeBox ESP100.

As a reminder, the EdgeBox ESP100 has the following capability: 
- CPU : ESP32-S3
- Internal RAM : 512 KB
- External RAM (PSRAM) : 8 MB
- Flash : 16 MB
- WiFi / BLE / 4G
- 4 Digital Inputs, 6 Digital Outputs
- 4 Analog Inputs, 2 Analog Outputs
 
 ## 2. Application details

### Web Interface
The embedded web UI is served on port 80. It includes:
- Live zone A and B status (values of AV1 and AV0)
- Sunrise and sunset time calculations (lat/long + configurable offsets)
- Special events management
- Event log view
- Trend log history view

### Sunrise and sunset time
Sunrise and sunset time are calculated every day according to the GPS coordinate provided in the web interface and the time of year. 

Two offsets are provided to increase the lighting period:
- Positive sunset offset set the sunset time clock back. 
- Positive sunrise offet set the sunrise clock forward.

During the day, between sunrise and sunset (after applying offset), the controller has the status **"Daytime"**.

During the night, after sunset and before sunrise (after applying offset), the controller has the status **"Night time"**.

### Light control control behavior
**Daytime and nightime control:**

Whatever the time slot allocated in the schedule (weekly and special), the lighting zone is always off during daytime.

**Schedule control:** 

During night time, BACnet schedule reach a nigh time, AV0 and AV1 output follows each zone schedule/special events.

**Force lightning:**

Lightning can be forced to a specific value at any time (day and night) using BACnet Write on AV0 and AV1. At this point, AV0 and AV1 value can be overwritten by :
- a new BACnet write property on AV0 or AV1.
- a new shedule event (weekly or special).
- a new daylight/night time status 

### Logs
- Trend log  object log changes of value for AV0 and AV1. 
- Boot, sync, connectivity status , and value changes persisted in FRAM event log


### Default Scheduling
At startup, a default weekly schedules is applied for each schedule object:
- 17:00 to 01:00 -> 100%
- 05:00 to 10:00 -> 100%


### Software architecture
The firmware has the following structure based on components :
- components/bacnetserver: BACnet service handlers and object table setup
- components/httpserver: embedded web app + REST API
- components/solar: sunrise/sunset and timezone logic
- components/wifi: Wi-Fi connection management
- components/modem: SIM7600 PPP connection and reconnect logic
- components/gpiooutputs: LEDC PWM output control
- components/fram: FRAM read/write access
- components/rtc: RTC driver
- components/ntp: time synchronisation

## 3. Installation, Build, Flash, Monitor
This use this application:
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
