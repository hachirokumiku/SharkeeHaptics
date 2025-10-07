# SharkeeHaptics
ESP8266 haptic system for VRChat

## Overview
SharkeeHaptics is a wireless haptic feedback system for VRChat using ESP8266 microcontrollers and DRV2605 haptic motor drivers. The system consists of multiple ESP8266 clients that receive OSC messages from a Python router application.

## Features
- **11 Body Locations**: Head, Chest, Upper Arms (L/R), Hips, Upper Legs (L/R), Lower Legs (L/R), Feet (L/R)
- **mDNS Discovery**: Automatic device discovery using mDNS hostnames
- **Web Configuration**: Each ESP8266 has a web interface for configuration
- **OTA Updates**: Wireless firmware updates via ArduinoOTA
- **Real-time Haptic Control**: DRV2605 real-time playback mode for smooth haptic feedback
- **Python GUI Router**: Desktop application to route VRChat OSC to ESP8266 devices
- **Manual IP Override**: Option to manually set IP addresses for devices

## Hardware Requirements
- ESP8266 modules (NodeMCU, Wemos D1 Mini, etc.)
- Adafruit DRV2605 haptic motor drivers
- LRA or ERM haptic motors
- Power supply (3.7V LiPo batteries recommended)

## Software Requirements
### Arduino (ESP8266)
- Arduino IDE with ESP8266 board support
- Libraries:
  - ESP8266WiFi
  - ESP8266WebServer
  - ESP8266mDNS
  - Adafruit_DRV2605
  - OSCMessage
  - ArduinoOTA

### Python Router
- Python 3.7+
- python-osc library

## Setup Instructions

### 1. ESP8266 Setup
1. Install required Arduino libraries
2. Open `Sharkee_Haptics.ino` in Arduino IDE
3. Update WiFi credentials:
   ```cpp
   const char* ssid = "YOUR_WIFI_SSID";
   const char* password = "YOUR_WIFI_PASSWORD";
   ```
4. Upload to ESP8266
5. Connect to the device's web interface (http://[device-ip]/)
6. Configure Device ID and Receiver location
7. Device will restart and advertise itself via mDNS

### 2. Python Router Setup
1. Install Python dependencies:
   ```bash
   pip install -r requirements.txt
   ```
2. Run the router:
   ```bash
   python sharkee_gui.py
   ```
3. Click "Start Server" to begin routing OSC messages
4. The router listens on port 9001 for VRChat OSC messages

## Configuration

### ESP8266 Web Interface
Each ESP8266 hosts a web interface on port 80 that shows:
- Device ID and receiver assignment
- mDNS hostname
- IP address and WiFi RSSI
- Battery level
- Test haptic button
- Configuration options

### VRChat Setup
Configure VRChat avatar parameters to send OSC messages to:
- `/avatar/parameters/SharkeeHead`
- `/avatar/parameters/SharkeeChest`
- `/avatar/parameters/SharkeeUpperArm_L`
- `/avatar/parameters/SharkeeUpperArm_R`
- `/avatar/parameters/SharkeeHips`
- `/avatar/parameters/SharkeeUpperLeg_L`
- `/avatar/parameters/SharkeeUpperLeg_R`
- `/avatar/parameters/SharkeeLowerLeg_L`
- `/avatar/parameters/SharkeeLowerLeg_R`
- `/avatar/parameters/SharkeeFoot_L`
- `/avatar/parameters/SharkeeFoot_R`

Values should be float (0.0 - 1.0) representing intensity.

## Architecture
```
VRChat (OSC) -> Python Router (9001) -> ESP8266 Clients (8000)
                      |
                   mDNS Discovery
                   IP Caching
                   Rate Limiting
```

## Troubleshooting
- **Device not found**: Check that both router and ESP8266 are on the same network
- **No haptic feedback**: Use the web interface test button to verify motor connection
- **High latency**: Adjust `INTENSITY_THRESHOLD` in `sharkee_gui.py` for rate limiting
- **Connection drops**: Check WiFi signal strength (RSSI) in web interface

## License
MIT License - See LICENSE file for details
