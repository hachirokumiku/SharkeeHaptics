/*
  Sharkee_Haptics_Router_Client.ino
  
  Configuration for mDNS Router Mode:
  - Requires the Python Router application (sharkee_gui.py) to be running.
  - VRChat sends data to the Python Router on 9001.
  - The Python Router sends unicast data to this device's mDNS hostname on 8000.
  
  - OTA and mDNS hostnames are UNIFIED and based on the Receiver Role (e.g., 'head.local').
  - Includes a Factory Reset button in the Web GUI to wipe config and WiFi credentials.
  
  - Uses DRV2605 realtime mode (DRV2605_MODE_REALTIME).
  - Implements a safety timeout (REALTIME_TIMEOUT_MS) to stop motor if updates cease.
  - Persists configuration (ID, Receiver, Gamma) to EEPROM.
  - Retains web GUI, mDNS, and OTA.
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <ESP8266mDNS.h>
#include <Adafruit_DRV2605.h>
#include <WiFiUdp.h>
#include <OSCMessage.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <WiFiManager.h> 

// ------------------------------------
// --- Configuration & Constants ---
// ------------------------------------

// This unit listens for the Python router on this port
const unsigned int CLIENT_LISTENER_PORT = 8000;
// OSC address sent by the Python router. This unit MUST match this address.
const char* INTERNAL_OSC_ADDRESS = "/sharkeehaptics/set_intensity"; 


// Haptic Logic Configuration
bool isMotorRunning = false;
const float MIN_INTENSITY_THRESHOLD = 0.05f;
const int MAX_DEVICE_ID = 10;
// Timeout to stop motor if realtime updates stop (ms)
const unsigned long REALTIME_TIMEOUT_MS = 500;
unsigned long lastReceivedMs = 0;
// Gamma / exponential mapping for perceived intensity
bool USE_GAMMA_MAPPING = true;
float GAMMA = 2.2f;

// EEPROM addresses for persistent storage
#define EEPROM_SIZE 32
#define DEVICE_ID_ADDR 0
#define RECEIVER_NAME_ADDR 1
#define GAMMA_ADDR 2         
#define GAMMA_FLAG_ADDR 6    
#define BATTERY_LEVEL_PIN A0 

// VRChat Receiver Mapping - all lowercase, MUST match the mDNS Hostname
const int NUM_RECEIVERS = 11;
const char* receiverNames[NUM_RECEIVERS] = {
  "head", "chest", "upperarm_l", "upperarm_r", "hips", "upperleg_l",
  "upperleg_r", "lowerleg_l", "lowerleg_r", "foot_l", "foot_r"
};

// --- Global Objects and State ---
ESP8266WebServer httpServer(80);
WiFiUDP Udp; 
Adafruit_DRV2605 drv;
WiFiManager wm;
int myDeviceID = -1;
int assignedReceiverIndex = 0;
char incomingPacket[256];

// Helper function to get the mDNS hostname (e.g., "head")
String getMDNSHostname() {
  return String(receiverNames[assignedReceiverIndex]);
}

// ------------------------------------
// --- EEPROM / Haptic Helpers ---
// ------------------------------------

int loadDeviceID() {
  int id = EEPROM.read(DEVICE_ID_ADDR);
  if (id >= 0 && id <= 10) return id;
  return -1;
}

void saveDeviceID(int id) {
  EEPROM.write(DEVICE_ID_ADDR, id);
  EEPROM.commit();
}

int loadAssignedReceiverIndex() {
  int idx = EEPROM.read(RECEIVER_NAME_ADDR);
  if (idx >= 0 && idx < NUM_RECEIVERS) return idx;
  return 0;
}

void saveAssignedReceiverIndex(int idx) {
  EEPROM.write(RECEIVER_NAME_ADDR, idx);
  EEPROM.commit();
}

void saveGammaToEEPROM() {
  EEPROM.put(GAMMA_ADDR, GAMMA);
  EEPROM.write(GAMMA_FLAG_ADDR, 0xA5);
  EEPROM.commit();
}

void loadGammaFromEEPROM() {
  if (EEPROM.read(GAMMA_FLAG_ADDR) == 0xA5) {
    float stored = 0.0f;
    EEPROM.get(GAMMA_ADDR, stored);
    if (!isnan(stored) && stored >= 0.5f && stored <= 6.0f) {
      GAMMA = stored;
    } else {
      GAMMA = 2.2f;
    }
  } 
}

float getBatteryPercent() {
  const float V_FULL = 4.2f;
  const float V_EMPTY = 3.3f;
  int raw = analogRead(BATTERY_LEVEL_PIN);
  float voltage = raw * (3.3f / 1023.0f);
  float percent = (voltage - V_EMPTY) / (V_FULL - V_EMPTY) * 100.0f;
  return constrain(percent, 0.0f, 100.0f);
}

// ------------------------------------
// --- Realtime Haptic Control ---
// ------------------------------------

float applyGammaMapping(float intensity) {
  intensity = constrain(intensity, 0.0f, 1.0f);
  if (!USE_GAMMA_MAPPING) return intensity;
  return powf(intensity, GAMMA);
}

static inline uint8_t intensityToRealtimeValue(float intensity) {
  float corrected = applyGammaMapping(intensity);
  uint8_t val = (uint8_t)roundf(constrain(corrected, 0.0f, 1.0f) * 255.0f);
  return val;
}

void setMotorRealtime(float intensity) {
  intensity = constrain(intensity, 0.0f, 1.0f);
  if (intensity < MIN_INTENSITY_THRESHOLD) {
    if (isMotorRunning) {
      drv.setRealtimeValue(0);
      drv.setMode(0);
      isMotorRunning = false;
    }
    return;
  }

  if (!isMotorRunning) {
    drv.setMode(DRV2605_MODE_REALTIME);
    isMotorRunning = true;
  }

  uint8_t speed = intensityToRealtimeValue(intensity);
  drv.setRealtimeValue(speed);
  lastReceivedMs = millis();
}

void hapticSequence(int pulses, int duration_ms) {
    const float FEEDBACK_INTENSITY = 0.7f;
    uint8_t pulseSpeed = intensityToRealtimeValue(FEEDBACK_INTENSITY);
    
    bool wasMotorRunningBefore = isMotorRunning; 
    
    for (int i = 0; i < pulses; i++) {
        if (!isMotorRunning) {
            drv.setMode(DRV2605_MODE_REALTIME);
            isMotorRunning = true;
        }
        drv.setRealtimeValue(pulseSpeed);
        delay(duration_ms);
        drv.setRealtimeValue(0);
        if (i < pulses - 1) { 
            delay(duration_ms);
        }
    }
    
    if (!wasMotorRunningBefore) {
        drv.setRealtimeValue(0);
        drv.setMode(0);
        isMotorRunning = false;
    } else {
        drv.setRealtimeValue(0);
    }
}

void checkRealtimeTimeout() {
  if (isMotorRunning) {
    unsigned long now = millis();
    if (now - lastReceivedMs > REALTIME_TIMEOUT_MS) {
      drv.setRealtimeValue(0);
      drv.setMode(0);
      isMotorRunning = false;
    }
  }
}

// --- Router OSC Input Handler ---
void handleRouterOscInput() {
  int packetSize = Udp.parsePacket();
  if (packetSize != 0) {
    int len = Udp.read((uint8_t*)incomingPacket, sizeof(incomingPacket));
    if (len <= 0) return;
    
    OSCMessage msg;
    msg.fill((uint8_t*)incomingPacket, len);
    
    if (msg.hasError()) return;
    
    // Check if the address matches the router's internal address
    if (msg.match(INTERNAL_OSC_ADDRESS) && msg.isFloat(0)) {
      float intensity = msg.getFloat(0);
      setMotorRealtime(intensity);
    }
  }
}

// ------------------------------------
// --- WEB SERVER HANDLERS ---
// ------------------------------------

// Handler for the Factory Reset action
void handleResetConfig() {
    // 1. Erase all persistent configuration from EEPROM (ID, Receiver, Gamma flags)
    // Writing 0xFF is a safe way to 'blank' the memory.
    for (int i = 0; i < EEPROM_SIZE; ++i) {
        EEPROM.write(i, 0xFF); 
    }
    EEPROM.commit();
    
    // 2. Erase saved WiFi credentials using WiFiManager
    wm.resetSettings();

    // 3. Send success message and reboot
    httpServer.send(200, "text/plain", "Device configuration and WiFi credentials successfully wiped. Restarting now to enter setup mode...");
    delay(100);
    ESP.restart();
}


// HTML is defined using standard C-string concatenation to avoid raw string compiler issues
const char WEB_PAGE[] PROGMEM = 
"<!DOCTYPE html>\n"
"<html>\n"
"<head>\n"
"<title>Sharkee Haptics Client</title>\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"<style>\n"
"/* Modern, dark-mode style */\n"
":root {\n"
"    --bg-dark: #1F2937;\n"
"    --bg-light: #374151;\n"
"    --text-light: #F9FAFB;\n"
"    --text-secondary: #D1D5DB;\n"
"    --accent-blue: #3B82F6;\n"
"    --status-good: #10B981;\n"
"    --status-warn: #FBBF24;\n"
"    --status-error: #EF4444;\n"
"    --button-primary: #10B981;\n"
"    --button-hover: #059669;\n"
"}\n"
"\n"
"body {\n"
"    background-color: var(--bg-dark);\n"
"    color: var(--text-light);\n"
"    font-family: -apple-system, BlinkMacSystemFont, \"Segoe UI\", \n"
"    Roboto, \"Helvetica Neue\", Arial, sans-serif;\n"
"    margin: 0;\n"
"    padding: 20px;\n"
"}\n"
"\n"
".container {\n"
"    max-width: 600px;\n"
"    margin: 0 auto;\n"
"    background-color: var(--bg-light);\n"
"    padding: 25px;\n"
"    border-radius: 12px;\n"
"    box-shadow: 0 4px 12px rgba(0, 0, 0, 0.5);\n"
"}\n"
"\n"
"h1 {\n"
"    font-size: 1.5em;\n"
"    color: var(--accent-blue);\n"
"    margin-bottom: 20px;\n"
"    border-bottom: 2px solid #444;\n"
"    padding-bottom: 10px;\n"
"}\n"
"\n"
".card {\n"
"    background-color: #2F3746;\n"
"    padding: 15px;\n"
"    border-radius: 8px;\n"
"    margin-bottom: 15px;\n"
"    border: 1px solid #444;\n"
"}\n"
"\n"
"label {\n"
"    display: block;\n"
"    margin-bottom: 8px;\n"
"    font-weight: 600;\n"
"    color: var(--text-secondary);\n"
"    font-size: 0.9em;\n"
"}\n"
"\n"
"input[type=\"number\"], select {\n"
"    width: 100%;\n"
"    padding: 10px;\n"
"    margin-bottom: 15px;\n"
"    border-radius: 6px;\n"
"    border: 1px solid #444;\n"
"    background-color: #333;\n"
"    color: var(--text-light);\n"
"    box-sizing: border-box;\n"
"    font-size: 1em;\n"
"}\n"
"\n"
"button { \n"
"    background-color: var(--button-primary);\n"
"    color: white;\n"
"    padding: 12px 15px;\n"
"    border: none;\n"
"    border-radius: 6px; \n"
"    cursor: pointer;\n"
"    width: 100%;\n"
"    transition: background-color 0.2s ease, transform 0.1s ease;\n"
"    font-weight: 600;\n"
"    font-size: 1em;\n"
"    box-shadow: 0 2px 4px rgba(0, 0, 0, 0.3);\n"
"}\n"
"\n"
"button:hover {\n"
"    background-color: var(--button-hover);\n"
"}\n"
"\n"
"button:active {\n"
"    transform: translateY(1px);\n"
"    box-shadow: 0 1px 2px rgba(0, 0, 0, 0.3);\n"
"}\n"
"\n"
".status-grid {\n"
"    display: grid;\n"
"    grid-template-columns: repeat(4, 1fr);\n"
"    gap: 10px;\n"
"    margin-top: 10px;\n"
"}\n"
"\n"
".status-item {\n"
"    background-color: #383838;\n"
"    padding: 10px;\n"
"    border-radius: 6px;\n"
"    text-align: center;\n"
"    font-size: 0.85em;\n"
"    border: 1px solid #444;\n"
"}\n"
"\n"
".status-value {\n"
"    font-weight: 700;\n"
"    font-size: 1.1em;\n"
"    color: var(--status-good);\n"
"    display: block;\n"
"    margin-top: 5px;\n"
"}\n"
"\n"
".message {\n"
"    padding: 10px;\n"
"    border-radius: 6px;\n"
"    margin-bottom: 15px;\n"
"    text-align: center;\n"
"    font-weight: 600;\n"
"}\n"
"\n"
".success {\n"
"    background-color: #10B98133; \n"
"    border: 1px solid var(--status-good);\n"
"    color: var(--status-good);\n"
"}\n"
"\n"
".error {\n"
"    background-color: #EF444433; \n"
"    border: 1px solid var(--status-error);\n"
"    color: var(--status-error);\n"
"}\n"
"\n"
".config-grid {\n"
"    display: grid;\n"
"    grid-template-columns: 1fr 1fr;\n"
"    gap: 15px;\n"
"}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"\n"
"<div class=\"container\">\n"
"    <h1>Sharkee Haptics Client Status</h1>\n"
"    \n"
"    <div class=\"status-grid\">\n"
"        <div class=\"status-item\">Device ID<span class=\"status-value\" id=\"display-id\">---</span></div>\n"
"        <div class=\"status-item\">Receiver<span class=\"status-value\" id=\"display-receiver\">---</span></div>\n"
"        <div class=\"status-item\">Gamma<span class=\"status-value\" id=\"display-gamma-status\">---</span></div>\n"
"        <div class=\"status-item\">Battery<span class=\"status-value\" id=\"display-battery\">---</span></div>\n"
"    </div>\n"
"    \n"
"    <div id=\"message\" class=\"message\" style=\"display:none;\"></div>\n"
"    \n"
"    <div class=\"config-grid\">\n"
"        \n"
"        <div class=\"card\">\n"
"            <h2>1. Set Receiver Role</h2>\n"
"            <label for=\"receiver-select\">Haptic Receiver Location (mDNS Hostname):</label>\n"
"            <select id=\"receiver-select\"></select>\n"
"            <button onclick=\"setReceiverRole()\">Set Role & Restart</button>\n"
"            <div style=\"margin-top:10px; font-size:0.9em; color:#ccc;\">This sets the mDNS hostname (e.g., 'head.local') that the Python router uses to send OSC data to this unit.</div>\n"
"        </div>\n"
"        \n"
"        <div class=\"card\">\n"
"            <h2>2. Set Gamma Correction</h2>\n"
"            <label for=\"gamma-input\">Gamma Value (0.5 to 6.0):</label>\n"
"            <input type=\"number\" id=\"gamma-input\" value=\"2.2\" min=\"0.5\" max=\"6.0\" step=\"0.05\">\n"
"            <button onclick=\"setGamma()\">Set Gamma</button>\n"
"            <div style=\"margin-top:10px; font-size:0.9em; color:#ccc;\">Current effective gamma: <span id=\"display-gamma\" style=\"color: var(--status-good); font-weight: 600;\">2.2</span></div>\n"
"        </div> \n"
"\n"
"    </div>\n"
"\n"
"    <div class=\"card\">\n"
"        \n"
"        <h2>3. Set Device ID</h2>\n"
"        <label for=\"id-input\">Unique Device ID (0 to 10):</label>\n"
"        <input type=\"number\" id=\"id-input\" value=\"-1\" min=\"0\" max=\"10\" step=\"1\">\n"
"        <button onclick=\"setDeviceID()\">Set ID & Restart</button>\n"
"        <div style=\"margin-top:10px; font-size:0.9em; color:#ccc;\">Sets a unique internal ID. OTA hostname is now derived from the Receiver Role. Requires restart.</div>\n"
"    </div>\n"
"    \n"
"    <div class=\"card\" style=\"margin-top: 25px;\">\n"
"        <h2>4. Factory Reset</h2>\n"
"        <div style=\"font-size:0.9em; color:var(--status-error); margin-bottom: 15px;\">\n"
"            WARNING: This will erase all saved settings (Receiver Role, Gamma, Device ID) and Wi-Fi credentials. The device will restart and create a new Wi-Fi setup hotspot.\n"
"        </div>\n"
"        <button onclick=\"factoryReset()\" style=\"background-color: var(--status-error); width: auto; padding: 12px 30px; margin: 0 auto; display: block;\">ERASE ALL & RESET</button>\n"
"    </div>\n"
"\n"
"</div>\n"
"\n"
"<script>\n"
"const RECEIVER_NAMES = [\"Head\",\"Chest\",\"UpperArm_L\",\"UpperArm_R\",\"Hips\",\"UpperLeg_L\",\"UpperLeg_R\",\"LowerLeg_L\",\"LowerLeg_R\",\"Foot_L\",\"Foot_R\"];\n"
"const MAX_DEVICE_ID = 10;\n"
"const messageElement = document.getElementById('message');\n"
"const receiverSelect = document.getElementById('receiver-select');\n"
"\n"
"RECEIVER_NAMES.forEach((name, index) => {\n"
"    const option = document.createElement('option');\n"
"    option.value = index;\n"
"    option.textContent = name;\n"
"    receiverSelect.appendChild(option);\n"
"});\n"
"\n"
"function showMessage(msg, type = 'success') {\n"
"    messageElement.textContent = msg;\n"
"    messageElement.className = 'message ' + (type === 'success' ? 'success' : 'error');\n"
"    messageElement.style.display = 'block';\n"
"}\n"
"\n"
"function fetchData() {\n"
"    fetch('/status_json')\n"
"    .then(response => response.json())\n"
"    .then(data => {\n"
"        document.getElementById('display-id').textContent = data.deviceID === -1 ? 'UNSET' : data.deviceID;\n"
"        document.getElementById('display-receiver').textContent = data.receiverName;\n"
"        document.getElementById('display-gamma-status').textContent = data.useGamma ? 'ON' : 'OFF';\n"
"        const batteryPct = data.batteryPercent.toFixed(0) + '%';\n"
"        document.getElementById('display-battery').textContent = batteryPct;\n"
"        \n"
"        // Set initial values for configuration\n"
"        document.getElementById('id-input').value = data.deviceID;\n"
"        receiverSelect.value = data.receiverIndex;\n"
"        const gammaValue = parseFloat(data.gamma).toFixed(2);\n"
"        document.getElementById('gamma-input').value = gammaValue;\n"
"        document.getElementById('display-gamma').textContent = gammaValue;\n"
"    })\n"
"    .catch(error => {\n"
"        console.error('Error fetching status:', error);\n"
"        showMessage('Could not fetch device status.', 'error');\n"
"    });\n"
"}\n"
"\n"
"function setDeviceID() {\n"
"    const newID = parseInt(document.getElementById('id-input').value);\n"
"    if (newID >= -1 && newID <= MAX_DEVICE_ID) {\n"
"        fetch('/config?action=set_id&id=' + newID)\n"
"        .then(response => response.text())\n"
"        .then(message => {\n"
"            showMessage(message, 'success');\n"
"        })\n"
"        .catch(error => {\n"
"            console.error('Error setting ID:', error);\n"
"            showMessage('Error setting Device ID.', 'error');\n"
"        });\n"
"    } else {\n"
"        showMessage('Invalid Device ID (must be -1 to ' + MAX_DEVICE_ID + ').', 'error');\n"
"    }\n"
"}\n"
"\n"
"function setReceiverRole() {\n"
"    const newIndex = receiverSelect.value;\n"
"    fetch('/config?action=set_receiver&index=' + newIndex)\n"
"    .then(response => response.text())\n"
"    .then(message => {\n"
"        showMessage(message, 'success');\n"
"    })\n"
"    .catch(error => {\n"
"        console.error('Error setting receiver role:', error);\n"
"        showMessage('Error setting Receiver Role.', 'error');\n"
"    });\n"
"}\n"
"\n"
"function setGamma() {\n"
"    const newGamma = parseFloat(document.getElementById('gamma-input').value);\n"
"    if (newGamma >= 0.5 && newGamma <= 6.0) {\n"
"        fetch('/config?action=set_gamma&gamma=' + newGamma)\n"
"        .then(response => {\n"
"            if (response.status === 200) return response.text();\n"
"            throw new Error('Server returned error status ' + response.status);\n"
"        })\n"
"        .then(message => {\n"
"            showMessage(message, 'success');\n"
"            fetchData(); // Refresh display gamma\n"
"        })\n"
"        .catch(error => {\n"
"            console.error('Error setting gamma:', error);\n"
"            showMessage('Error setting Gamma. ' + error.message, 'error');\n"
"        });\n"
"    } else {\n"
"        showMessage('Invalid Gamma Value (0.5 to 6.0).', 'error');\n"
"    }\n"
"}\n"
"\n"
"function factoryReset() {\n"
"    if (confirm(\"Are you sure you want to erase ALL configuration (Receiver, Gamma, WiFi) and restart the device? This will require you to set up the WiFi again.\")) {\n"
"        fetch('/reset_config')\n"
"        .then(response => response.text())\n"
"        .then(message => {\n"
"            showMessage(message, 'error');\n"
"            // Wait for the restart\n"
"            setTimeout(() => { showMessage('Rebooting...', 'error'); }, 1500);\n"
"        })\n"
"        .catch(error => {\n"
"            console.error('Error resetting config:', error);\n"
"            showMessage('Error attempting factory reset.', 'error');\n"
"        });\n"
"    }\n"
"}\n"
"\n"
"document.addEventListener('DOMContentLoaded', () => {\n"
"    fetchData(); // Initial data fetch\n"
"    setInterval(fetchData, 5000); // Poll status every 5 seconds\n"
"});\n"
"\n"
"</script>\n"
"\n"
"</body>\n"
"</html>\n";

void handleRoot() {
    httpServer.send(200, "text/html", WEB_PAGE);
}

// Handler for configuration actions
void handleConfig() {
    String response = "";
    int status_code = 200;

    if (!httpServer.hasArg("action")) {
        status_code = 400;
        response = "Missing action parameter.";
    } else {
        String action = httpServer.arg("action");

        if (action == "set_id" && httpServer.hasArg("id")) {
            int newID = httpServer.arg("id").toInt();
            if (newID >= -1 && newID <= MAX_DEVICE_ID) {
                saveDeviceID(newID);
                // Note: The OTA hostname is now set by Receiver Role, but saving the ID is still useful
                // for internal diagnostics/future use.
                response = "Device ID updated to " + String(newID) + ". Restarting to apply changes...";
                httpServer.send(status_code, "text/plain", response);
                delay(100);
                ESP.restart();
            } else {
                status_code = 400;
                response = "Invalid device ID (must be -1 to " + String(MAX_DEVICE_ID) + ").";
            }
        } else if (action == "set_receiver" && httpServer.hasArg("index")) {
            int newIndex = httpServer.arg("index").toInt();

            if (newIndex >= 0 && newIndex < NUM_RECEIVERS) {
                saveAssignedReceiverIndex(newIndex);
                response = "Receiver role updated to " + String(receiverNames[newIndex]) + ". Restarting to update mDNS/OTA...";
                httpServer.send(status_code, "text/plain", response);
                delay(100);
                ESP.restart();
            } else {
                status_code = 400;
                response = "Invalid receiver index.";
            }
        } else if (action == "set_gamma" && httpServer.hasArg("gamma")) {
            float newGamma = httpServer.arg("gamma").toFloat();
            if (newGamma >= 0.5f && newGamma <= 6.0f) {
                GAMMA = newGamma;
                saveGammaToEEPROM();
                response = "Gamma updated to " + String(GAMMA, 2) + ".";
            } else {
                status_code = 400;
                response = "Invalid gamma (must be between 0.5 and 6.0).";
            }
        } else {
            status_code = 400;
            response = "Invalid action or missing parameter.";
        }
    }
    httpServer.send(status_code, "text/plain", response);
}

// Provides current device status as JSON.
void handleStatusJSON() {
    String json = "{";
    json += "\"deviceID\":" + String(myDeviceID) + ",";
    json += "\"role\":\"Client (mDNS Router Mode)\",";
    json += "\"receiverIndex\":" + String(assignedReceiverIndex) + ",";
    json += "\"receiverName\":\"" + String(receiverNames[assignedReceiverIndex]) + "\",";
    json += "\"gamma\":" + String(GAMMA, 2) + ","; // format to 2 decimal places
    json += "\"useGamma\":" + String(USE_GAMMA_MAPPING ? "true" : "false") + ",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"batteryPercent\":" + String(getBatteryPercent(), 2) + "";
    json += "}";
    httpServer.send(200, "application/json", json);
}

void setupMDNS() {
    String hostname = getMDNSHostname();
    if (MDNS.begin(hostname.c_str())) {
        Serial.printf("mDNS responder started with hostname: %s.local\n", hostname.c_str());
        MDNS.addService("http", "tcp", 80);
        MDNS.addService("osc", "udp", CLIENT_LISTENER_PORT); // Advertise the OSC listener port
    } else {
        Serial.println("Error starting mDNS!");
    }
}

void setupWiFiAndNetwork() {
    wm.setConfigPortalBlocking(false); 
    wm.setConfigPortalTimeout(120); // 2 minutes in captive portal mode
    
    // Connect to existing WiFi or start the captive portal
    if (!wm.autoConnect("SharkeeHapticsSetup")) {
        Serial.println("Failed to connect and hit timeout");
        hapticSequence(5, 50); // Error Buzz
        // Optional: keep running for diagnostics or just let loop handle it
    } else {
        Serial.println("WiFi connected!");
        hapticSequence(2, 75); // Success Buzz
    }

    Serial.print("Local IP: ");
    Serial.println(WiFi.localIP());

    setupMDNS();
    // Web Server setup
    httpServer.on("/", handleRoot);
    httpServer.on("/config", handleConfig);
    httpServer.on("/status_json", handleStatusJSON);
    httpServer.on("/reset_config", handleResetConfig); // Factory Reset Handler
    httpServer.begin();
    Serial.println("Web GUI (Port 80) started.");
    
    // START LISTENING ON CLIENT LISTENER PORT (8000)
    if (Udp.begin(CLIENT_LISTENER_PORT)) {
        Serial.printf("CLIENT: Listening for Router OSC on UDP port %d\n", CLIENT_LISTENER_PORT);
    } else {
        Serial.println("CLIENT: Failed to start OSC listener!");
    }

    // Over-The-Air (OTA) Updates: Use the Receiver Role name for consistency.
    String otaHostname = getMDNSHostname(); // e.g., "head", "chest"
    ArduinoOTA.setHostname(otaHostname.c_str());
    Serial.printf("OTA Hostname set to: %s.local\n", otaHostname.c_str());
    
    ArduinoOTA.begin();
    Serial.println("OTA ready.");
}

void setupHaptic() {
    Serial.print("DRV2605 initialization...");
    if (!drv.begin()) {
        Serial.println("Failed to find DRV2605 chip!");
        while (1) { delay(1); }
    }
    Serial.println("done.");
    drv.selectLibrary(1);
    drv.setMode(0); // idle until realtime begins
}

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n--- Sharkee Haptics mDNS Router Client (Realtime + Gamma) ---");

    EEPROM.begin(EEPROM_SIZE);
    myDeviceID = loadDeviceID();
    assignedReceiverIndex = loadAssignedReceiverIndex();
    loadGammaFromEEPROM(); // load persisted gamma (if any)

    Serial.printf("Loaded Device ID: %d\n", myDeviceID);
    Serial.printf("Assigned Receiver: %s\n", receiverNames[assignedReceiverIndex]);
    Serial.printf("Loaded Gamma: %.2f\n", GAMMA);

    setupHaptic();
    hapticSequence(3, 100); // Boot Sequence Haptic
    // NOTE: The temporary wm.resetSettings() line has been REMOVED here.
    setupWiFiAndNetwork();
}

void loop() {
    // WiFi Manager and mDNS/OTA handlers
    wm.process();
    MDNS.update();
    ArduinoOTA.handle();
    // Network services
    httpServer.handleClient();
    
    // CORE LOGIC: Handle Router OSC
    handleRouterOscInput();
    
    // Haptic safety check
    checkRealtimeTimeout();
    // Small delay to prevent watchdog timer
    delay(1); 
}