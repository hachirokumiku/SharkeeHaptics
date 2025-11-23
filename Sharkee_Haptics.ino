/*
  Sharkee Haptics Realtime Controller (DRV2605 + ESP8266)
  
  -- AP CONFIGURATION VERSION --
  Implements an Access Point (AP) configuration portal to set the Wi-Fi
  credentials, which are saved to EEPROM. If credentials are found, the device
  connects in Station (STA) mode and runs the Haptics Router Client logic.
  
  - NOW LISTENS DIRECTLY TO VRChat OSC ADDRESSES (/avatar/parameters/Receiver_*)
  - Includes Gamma correction and a Real-Time Playback (RTP) watchdog timer.
  - UPDATED: The receiver list now matches the Unity editor "POSITION_DEFINITIONS_59" naming.
  
  Dependencies:
  - ESP8266WiFi, EEPROM, ESP8266mDNS, ArduinoOTA
  - Haptic_DRV2605 (local haptic driver)
  - WiFiUdp, OSCMessage (for receiving commands)
  - DNSServer (for captive portal functionality)
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <ESP8266mDNS.h>
#include "Haptic_DRV2605.h"
#include <WiFiUdp.h>
#include <OSCMessage.h>
#include <ArduinoOTA.h>
#include <DNSServer.h> // For captive portal

// ------------------------------------
// --- Configuration & Constants ---
// ------------------------------------

// ** 1. Network & System Settings **
constexpr unsigned int CLIENT_LISTENER_PORT = 9001;
constexpr unsigned long REALTIME_TIMEOUT_MS = 500;
constexpr float MIN_INTENSITY_THRESHOLD = 0.05f;
constexpr int BATTERY_LEVEL_PIN = A0;

// AP Mode settings
constexpr const char* AP_SSID = "SharkeeHaptics-SETUP";
constexpr const char* AP_PASS = "12345678"; // Min 8 chars for WPA2
constexpr int DNS_PORT = 53;

// ** 2. EEPROM Addresses **
constexpr size_t EEPROM_SIZE = 512;       // Increased size for Wi-Fi credentials
constexpr uint8_t DEVICE_ID_ADDR = 0;     // ID (1 byte)
constexpr uint8_t RECEIVER_NAME_ADDR = 1; // Index (1 byte)
constexpr uint8_t GAMMA_ADDR = 2;         // Gamma (float, 4 bytes)
constexpr uint8_t GAMMA_FLAG_ADDR = 6;    // Gamma flag (1 byte)
constexpr uint8_t WIFI_CONFIG_FLAG = 7;   // Config flag (1 byte)
constexpr uint8_t WIFI_SSID_ADDR = 8;     // SSID (32 bytes max)
constexpr uint8_t WIFI_PASS_ADDR = 40;    // Password (64 bytes max)
// Paired host (auto-pair) storage
constexpr uint8_t PAIRED_HOST_ADDR = 100;  // 4 bytes for IP
constexpr uint8_t PAIRED_HOST_FLAG_ADDR = 104; // flag byte

// ** 3. VRChat Receiver Mapping (uses names from Unity editor configuration) **
// These names correspond to the `/avatar/parameters/Receiver_<name>` OSC addresses.
constexpr const char* RECEIVER_NAMES[] = {
  "Head", "Chest", "Spine", "Hips",

  "UpperArm_L", "LowerArm_L", "Hand_L",

  "UpperArm_R", "LowerArm_R", "Hand_R",

  "UpperLeg_L", "LowerLeg_L", "Foot_L", "Toe_L",

  "UpperLeg_R", "LowerLeg_R", "Foot_R", "Toe_R",

  "Thumb_L_A", "Thumb_L_B", "Thumb_L_C",
  "Index_L_A", "Index_L_B", "Index_L_C",
  "Middle_L_A", "Middle_L_B", "Middle_L_C",
  "Ring_L_A", "Ring_L_B", "Ring_L_C",
  "Little_L_A", "Little_L_B", "Little_L_C",

  "Thumb_R_A", "Thumb_R_B", "Thumb_R_C",
  "Index_R_A", "Index_R_B", "Index_R_C",
  "Middle_R_A", "Middle_R_B", "Middle_R_C",
  "Ring_R_A", "Ring_R_B", "Ring_R_C",
  "Little_R_A", "Little_R_B", "Little_R_C",

  "Shoulder_L", "Shoulder_R", "Neck"
};
constexpr int NUM_RECEIVERS = sizeof(RECEIVER_NAMES) / sizeof(RECEIVER_NAMES[0]);
constexpr const char* VRC_OSC_ADDRESS_PREFIX = "/avatar/parameters/Receiver_";

// --- Global Objects and State ---
ESP8266WebServer httpServer(80);
WiFiUDP Udp;
DNSServer dnsServer; // For captive portal

char incomingPacket[256];
int myDeviceID = -1;
int assignedReceiverIndex = 0;
bool isAPMode = false;
bool pairedHostSet = false;
IPAddress pairedHost;

// Storage for Wi-Fi credentials
char sta_ssid[33] = "";
char sta_password[65] = "";


// ------------------------------------
// --- HapticController Class (DRV2605 Encapsulation) ---
// ------------------------------------

class HapticController {
private:
  Haptic_DRV2605 drv;
  float gamma = 2.2f;
  bool isMotorRunning = false;
  unsigned long lastReceivedMs = 0;

  float applyGammaMapping(float intensity) {
    intensity = constrain(intensity, 0.0f, 1.0f);
    // Apply Gamma correction for better low-end control on LRA motors
    return powf(intensity, gamma);
  }

  uint8_t intensityToRealtimeValue(float intensity) {
    float corrected = applyGammaMapping(intensity);
    uint8_t val = (uint8_t)roundf(constrain(corrected, 0.0f, 1.0f) * 255.0f);
    return val;
  }

public:
  bool begin() {
    Serial.print("DRV2605 initialization...");
    int ret = drv.begin();
    if (ret != HAPTIC_SUCCESS) {
      Serial.println("Failed to find DRV2605 chip!");
      return false;
    }
    Serial.println("done.");
    drv.setWaveformLib(1);
    drv.setMode(REGISTER_MODE);
    return true;
  }

  void setIntensity(float intensity) {
    intensity = constrain(intensity, 0.0f, 1.0f);

    if (intensity < MIN_INTENSITY_THRESHOLD) {
      if (isMotorRunning) {
        drv.setRealtimeValue(0);
        drv.setMode(REGISTER_MODE);
        isMotorRunning = false;
      }
      return;
    }

    if (!isMotorRunning) {
      drv.setMode(STREAM_MODE);
      isMotorRunning = true;
    }

    uint8_t speed = intensityToRealtimeValue(intensity);
    drv.setRealtimeValue(speed);
    lastReceivedMs = millis();
  }

  void checkTimeout() {
    if (isMotorRunning) {
      unsigned long now = millis();
      if (now - lastReceivedMs > REALTIME_TIMEOUT_MS) {
        drv.setRealtimeValue(0);
        drv.setMode(REGISTER_MODE);
        isMotorRunning = false;
        Serial.println("Haptics: Realtime timeout triggered. Motor stopped.");
      }
    }
  }

  void runTestRamp() {
    bool wasRunning = isMotorRunning;
    if (!wasRunning) {
      drv.setMode(STREAM_MODE);
      isMotorRunning = true;
    }
    for (int v = 0; v <= 255; v += 25) {
      drv.setRealtimeValue((uint8_t)v);
      delay(30);
    }
    for (int v = 255; v >= 0; v -= 25) {
      drv.setRealtimeValue((uint8_t)v);
      delay(30);
    }
    drv.setRealtimeValue(0);
    if (!wasRunning) {
      drv.setMode(REGISTER_MODE);
      isMotorRunning = false;
    }
  }

  float getGamma() const { return gamma; }
  void setGamma(float newGamma) {
    gamma = constrain(newGamma, 0.5f, 6.0f);
  }
} hapticController;

// ------------------------------------
// --- EEPROM / Config Helpers ---
// ------------------------------------

void saveWifiConfig() {
  EEPROM.write(WIFI_CONFIG_FLAG, 0xA5);
  EEPROM.put(WIFI_SSID_ADDR, sta_ssid);
  EEPROM.put(WIFI_PASS_ADDR, sta_password);
  EEPROM.commit();
}

bool loadWifiConfig() {
  if (EEPROM.read(WIFI_CONFIG_FLAG) == 0xA5) {
    EEPROM.get(WIFI_SSID_ADDR, sta_ssid);
    EEPROM.get(WIFI_PASS_ADDR, sta_password);
    sta_ssid[32] = '\0'; // Ensure null termination
    sta_password[64] = '\0';
    return strlen(sta_ssid) > 0;
  }
  return false;
}

void clearWifiConfig() {
  EEPROM.write(WIFI_CONFIG_FLAG, 0x00);
  EEPROM.commit();
}

void saveGammaToEEPROM() {
  EEPROM.put(GAMMA_ADDR, hapticController.getGamma());
  EEPROM.write(GAMMA_FLAG_ADDR, 0xA5);
  EEPROM.commit();
}

// --- Paired host helpers ---
void savePairedHostToEEPROM(const IPAddress &ip) {
  for (int i = 0; i < 4; i++) EEPROM.write(PAIRED_HOST_ADDR + i, ip[i]);
  EEPROM.write(PAIRED_HOST_FLAG_ADDR, 0xA5);
  EEPROM.commit();
  pairedHost = ip;
  pairedHostSet = true;
}

void loadPairedHostFromEEPROM() {
  if (EEPROM.read(PAIRED_HOST_FLAG_ADDR) == 0xA5) {
    uint8_t b0 = EEPROM.read(PAIRED_HOST_ADDR + 0);
    uint8_t b1 = EEPROM.read(PAIRED_HOST_ADDR + 1);
    uint8_t b2 = EEPROM.read(PAIRED_HOST_ADDR + 2);
    uint8_t b3 = EEPROM.read(PAIRED_HOST_ADDR + 3);
    pairedHost = IPAddress(b0, b1, b2, b3);
    pairedHostSet = true;
  } else {
    pairedHostSet = false;
  }
}

void clearPairedHost() {
  for (int i = 0; i < 4; i++) EEPROM.write(PAIRED_HOST_ADDR + i, 0);
  EEPROM.write(PAIRED_HOST_FLAG_ADDR, 0x00);
  EEPROM.commit();
  pairedHostSet = false;
}

void loadGammaFromEEPROM() {
  float loadedGamma = 2.2f;
  if (EEPROM.read(GAMMA_FLAG_ADDR) == 0xA5) {
    EEPROM.get(GAMMA_ADDR, loadedGamma);
    if (isnan(loadedGamma) || loadedGamma < 0.5f || loadedGamma > 6.0f) {
      loadedGamma = 2.2f;
    }
  }
  hapticController.setGamma(loadedGamma);
}

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
  // Default to "Head" (index 0 in the Unity list)
  return 0; 
}

void saveAssignedReceiverIndex(int idx) {
  EEPROM.write(RECEIVER_NAME_ADDR, idx);
  EEPROM.commit();
}

// ------------------------------------
// --- System & Utility Helpers ---
// ------------------------------------

String getMDNSHostname() {
  return String(RECEIVER_NAMES[assignedReceiverIndex]);
}

float getBatteryPercent() {
  constexpr float V_FULL = 4.2f;
  constexpr float V_EMPTY = 3.3f;
  int raw = analogRead(BATTERY_LEVEL_PIN);
  float voltage = raw * (3.3f / 1023.0f);
  float percent = (voltage - V_EMPTY) / (V_FULL - V_EMPTY) * 100.0f;
  return constrain(percent, 0.0f, 100.0f);
}

// ------------------------------------
// --- Network & Protocol Handlers ---
// ------------------------------------

// Handles incoming OSC messages directly from VRChat.
void handleVRChatOscInput() {
  int packetSize = Udp.parsePacket();
  if (packetSize == 0) return; // No packet

  int len = Udp.read((uint8_t*)incomingPacket, sizeof(incomingPacket));
  if (len <= 0) return;

  IPAddress remote = Udp.remoteIP();
  // Auto-pair: if no paired host yet, save the first sender as the paired host
  if (!pairedHostSet) {
    savePairedHostToEEPROM(remote);
    Serial.printf("Paired to host %s (auto)", remote.toString().c_str());
  }
  // If paired, ignore packets from other hosts
  if (pairedHostSet && remote != pairedHost) {
    return; // ignore
  }

  OSCMessage msg;
  msg.fill((uint8_t*)incomingPacket, len);

  char addressBuffer[128];
  msg.getAddress(addressBuffer);
  String addr = String(addressBuffer);

  // Only respond to VRChat OSC addresses of the form:
  // /avatar/parameters/Receiver_<name>
  if (!addr.startsWith(VRC_OSC_ADDRESS_PREFIX)) return; // not a VRChat receiver message

  // Extract the receiver name after the prefix
  String recv = addr.substring(strlen(VRC_OSC_ADDRESS_PREFIX));
  // Compare to this device's assigned receiver (case-insensitive)
  if (!recv.equalsIgnoreCase(String(RECEIVER_NAMES[assignedReceiverIndex]))) return;

  // We received an intensity update for *this* actuator
  float intensity = 0.0f;
  bool dataFound = false;
  if (msg.isFloat(0)) {
    intensity = msg.getFloat(0);
    dataFound = true;
  } else if (msg.isInt(0)) {
    int val = msg.getInt(0);
    if (val >= 0 && val <= 255) {
      intensity = (float)val / 255.0f;
      dataFound = true;
    } else if (val >= 0 && val <= 100) {
      intensity = (float)val / 100.0f;
      dataFound = true;
    }
  }

  if (dataFound) {
    hapticController.setIntensity(intensity);
  } else {
    Serial.printf("Received VRChat message for %s but found no valid float or int argument.\n", addressBuffer);
  }
}


// --- Web GUI & Action Handlers ---
// HTML content for the configuration web page.
const char PROGMEM WEB_PAGE_STA[] = R"raw(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Sharkee Haptics Config (STA Mode)</title>
    <style>
        @import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;700&display=swap');
        body { font-family: 'Inter', sans-serif; background-color: #1a1a2e; color: #e4e4e4; margin: 0; padding: 20px; }
        .container { max-width: 520px; margin: 0 auto; background-color: #272747; padding: 20px; border-radius: 12px; box-shadow: 0 4px 12px rgba(0, 0, 0, 0.4); }
        h1 { text-align: center; color: #4CAF50; margin-bottom: 20px; }
        .card { background-color: #313156; padding: 15px; border-radius: 8px; margin-bottom: 15px; }
        .card-header { font-weight: bold; margin-bottom: 10px; color: #9C27B0; font-size: 1.1em; }
        label { display: block; margin-bottom: 5px; color: #ccc; }
        input[type="number"], input[type="text"], input[type="password"], select {
            width: 100%; padding: 10px; margin-bottom: 10px; border-radius: 5px; border: 1px solid #555; background-color: #44446a; color: #fff; box-sizing: border-box;
        }
        button {
            background-color: #4CAF50; color: white; padding: 10px 15px; border: none; border-radius: 5px; cursor: pointer; width: 100%; transition: background-color 0.3s ease, transform 0.1s ease;
            font-weight: bold; margin-top: 5px;
        }
        button.red { background-color: #f44336; }
        button.red:hover { background-color: #e53935; }
        button:hover { background-color: #45a049; }
        button:active { transform: scale(0.98); }
        .status-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 10px; }
        .status-item { background-color: #44446a; padding: 8px; border-radius: 5px; text-align: center; font-size: 0.9em; }
        .status-value { font-weight: bold; font-size: 1.1em; color: #FFEB3B; }
        #message { margin-top: 15px; padding: 10px; border-radius: 5px; text-align: center; font-weight: bold; }
        #message.success { background-color: #1e872e; }
        #message.error { background-color: #c73a3a; }
        .hostname-display { grid-column: 1 / span 2; font-size: 0.8em; color: #aaa; text-align: center; padding-top: 5px; }
        .inline { display: flex; gap: 8px; align-items:center; }
        .small { width: 140px; margin-right: 8px; }
    </style>
</head>
<body>
<div class="container">
    <h1>Sharkee Haptics Client (STA Mode)</h1>

    <div id="status-card" class="card">
        <div class="card-header">Current Status (<span id="role-display">Connected to: %STA_SSID%</span>)</div>
        <div class="status-grid">
            <div class="status-item">Device ID:<br><span id="display-id" class="status-value">?</span></div>
            <div class="status-item">Assigned:<br><span id="display-receiver" class="status-value">?</span></div>
            <div class="status-item">Battery:<br><span id="display-battery" class="status-value">?</span></div>
            <div class="status-item">IP Address:<br><span id="display-ip" class="status-value">?</span></div>
            <div class="hostname-display">mDNS Hostname: <span id="display-hostname" class="status-value" style="font-weight: normal; color: #81D4FA;">?</span></div>
        </div>
        <div id="message"></div>
    </div>
    
    <div class="card">
        <div class="card-header">5. Network Configuration</div>
        <button onclick="clearWifi()" class="red">Forget Wi-Fi Credentials & Restart to AP Mode</button>
    </div>

    <div class="card">
        <div class="card-header">1. Assign VRChat Receiver (Requires Restart)</div>
        <label for="receiver-select">Receiver Target:</label>
        <select id="receiver-select"></select>
        <button onclick="setReceiver()">Set Receiver Target</button>
    </div>

    <div class="card">
        <div class="card-header">2. Set Device ID (Requires Restart for OTA)</div>
        <label for="device-id-input">New Device ID (0-10):</label>
        <input type="number" id="device-id-input" min="0" max="10" value="0">
        <button onclick="setID()">Set ID & Restart</button>
    </div>

    <div class="card">
        <div class="card-header">3. Test Haptic Motor (Realtime)</div>
        <button onclick="testHaptic()">Run Realtime Test Ramp</button>
    </div>

    <div class="card">
        <div class="card-header">4. Gamma Mapping</div>
        <label for="gamma-input">Gamma (exponent) — higher => more low-end control (typical LRA 1.8–2.6)</label>
        <div class="inline">
            <input id="gamma-input" class="small" type="number" step="0.1" min="0.5" max="6.0" value="2.2">
            <button onclick="setGamma()">Set Gamma</button>
        </div>
        <div style="margin-top:10px; font-size:0.9em; color:#ccc;">Current gamma: <span id="display-gamma">2.2</span></div>
    </div>
</div>

<script>
    // Updated RECEIVER_NAMES list in JavaScript to match Unity `POSITION_DEFINITIONS_59`
    const RECEIVER_NAMES = [
      "Head", "Chest", "Spine", "Hips",

      "UpperArm_L", "LowerArm_L", "Hand_L",

      "UpperArm_R", "LowerArm_R", "Hand_R",

      "UpperLeg_L", "LowerLeg_L", "Foot_L", "Toe_L",

      "UpperLeg_R", "LowerLeg_R", "Foot_R", "Toe_R",

      "Thumb_L_A", "Thumb_L_B", "Thumb_L_C",
      "Index_L_A", "Index_L_B", "Index_L_C",
      "Middle_L_A", "Middle_L_B", "Middle_L_C",
      "Ring_L_A", "Ring_L_B", "Ring_L_C",
      "Little_L_A", "Little_L_B", "Little_L_C",

      "Thumb_R_A", "Thumb_R_B", "Thumb_R_C",
      "Index_R_A", "Index_R_B", "Index_R_C",
      "Middle_R_A", "Middle_R_B", "Middle_R_C",
      "Ring_R_A", "Ring_R_B", "Ring_R_C",
      "Little_R_A", "Little_R_B", "Little_R_C",

      "Shoulder_L", "Shoulder_R", "Neck"
    ];
    const MAX_DEVICE_ID = 10;
    const messageElement = document.getElementById('message');
    const receiverSelect = document.getElementById('receiver-select');

    RECEIVER_NAMES.forEach((name, index) => {
        const option = document.createElement('option');
        option.value = index;
        // Format names nicely for display (e.g., "upper_arm_l" -> "Upper Arm L")
        const formattedName = name.replace(/_/g, ' ').split(' ').map(w => w.charAt(0).toUpperCase() + w.slice(1)).join(' ');
        option.textContent = formattedName; 
        receiverSelect.appendChild(option);
    });

    function showMessage(msg, type = 'success') {
        messageElement.textContent = msg;
        messageElement.className = (type === 'success') ? 'success' : 'error';
    }

    function fetchData() {
        fetch('/status_json')
            .then(response => response.json())
            .then(data => {
                document.getElementById('display-id').textContent = data.deviceID === -1 ? 'UNSET' : data.deviceID;
                
                // Format receiver name for display
                const rawReceiverName = RECEIVER_NAMES[data.receiverIndex];
                const formattedReceiverName = rawReceiverName.replace(/_/g, ' ').split(' ').map(w => w.charAt(0).toUpperCase() + w.slice(1)).join(' ');

                document.getElementById('display-receiver').textContent = formattedReceiverName.toUpperCase();
                document.getElementById('display-battery').textContent = `${data.battery}%`;
                document.getElementById('display-ip').textContent = data.ip;
                document.getElementById('display-hostname').textContent = data.hostname;
                document.getElementById('display-gamma').textContent = parseFloat(data.gamma).toFixed(2);
                
                // Set form values
                receiverSelect.value = data.receiverIndex;
                document.getElementById('gamma-input').value = parseFloat(data.gamma).toFixed(2);

                const idDisplay = document.getElementById('display-id');
                idDisplay.style.color = (data.deviceID === -1) ? 'red' : '#FFEB3B';
                const batteryDisplay = document.getElementById('display-battery');
                batteryDisplay.style.color = (data.battery < 20) ? 'red' : '#FFEB3B';

                setTimeout(fetchData, 3000);
            })
            .catch(error => {
                document.getElementById('display-id').textContent = 'ERR';
                document.getElementById('display-ip').textContent = 'WIFI DOWN';
                console.error('Error fetching status:', error);
                setTimeout(fetchData, 5000);
            });
    }

    function sendAction(action, data = {}) {
        messageElement.className = '';
        messageElement.textContent = 'Processing...';
        const formData = new URLSearchParams();
        formData.append('action', action);
        for (const key in data) formData.append(key, data[key]);
        return fetch('/action', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: formData
        })
        .then(response => {
            if (!response.ok) return response.text().then(text => { throw new Error(text || response.statusText); });
            return response.text();
        })
        .then(text => {
            showMessage(text);
            if (action !== 'test' && !text.includes('Restarting')) fetchData();
        })
        .catch(error => showMessage(`Error: ${error.message}`, 'error'));
    }

    function setID() {
        const idInput = document.getElementById('device-id-input').value;
        const id = parseInt(idInput);
        if (id >= 0 && id <= MAX_DEVICE_ID) sendAction('set_id', { id: id });
        else showMessage("ID must be between 0 and 10.", 'error');
    }

    function setReceiver() {
        const index = parseInt(receiverSelect.value);
        if (index >= 0 && index < RECEIVER_NAMES.length) sendAction('set_receiver', { index: index });
    }

    function testHaptic() {
        sendAction('test');
    }

    function setGamma() {
        const g = parseFloat(document.getElementById('gamma-input').value);
        if (isNaN(g) || g < 0.5 || g > 6.0) {
            showMessage("Gamma must be between 0.5 and 6.0", 'error');
            return;
        }
        sendAction('set_gamma', { gamma: g });
    }
    
    function clearWifi() {
        sendAction('clear_wifi');
    }

    document.addEventListener('DOMContentLoaded', fetchData);
</script>
</body>
</html>
)raw";

const char PROGMEM WEB_PAGE_AP[] = R"raw(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Wi-Fi Setup Portal</title>
    <style>
        @import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;700&display=swap');
        body { font-family: 'Inter', sans-serif; background-color: #1a1a2e; color: #e4e4e4; margin: 0; padding: 20px; text-align: center; }
        .container { max-width: 400px; margin: 50px auto; background-color: #272747; padding: 25px; border-radius: 12px; box-shadow: 0 8px 16px rgba(0, 0, 0, 0.6); }
        h1 { color: #FFD700; margin-bottom: 10px; }
        h2 { color: #81D4FA; font-size: 1.1em; margin-bottom: 20px; }
        label { display: block; margin-bottom: 5px; color: #ccc; text-align: left; font-weight: bold; }
        input[type="text"], input[type="password"] {
            width: 100%; padding: 12px; margin-bottom: 20px; border-radius: 8px; border: none; background-color: #44446a; color: #fff; box-sizing: border-box;
            font-size: 1.1em;
        }
        button {
            background-color: #4CAF50; color: white; padding: 12px 20px; border: none; border-radius: 8px; cursor: pointer; width: 100%; transition: background-color 0.3s ease, transform 0.1s ease;
            font-weight: bold; font-size: 1.2em;
        }
        button:hover { background-color: #45a049; }
        .status { margin-top: 20px; padding: 10px; border-radius: 5px; background-color: #313156; color: #fff; }
    </style>
</head>
<body>
<div class="container">
    <h1>Wi-Fi Setup Portal</h1>
    <h2>Connect to your network to enable haptics.</h2>
    <div class="status">
        Access Point: <strong>%AP_SSID%</strong> (Password: <strong>%AP_PASS%</strong>)
    </div>
    <form method="POST" action="/save_wifi" style="margin-top: 30px;">
        <label for="ssid">Target Wi-Fi SSID:</label>
        <input type="text" id="ssid" name="ssid" placeholder="Your Home Wi-Fi Name" required>

        <label for="pass">Target Wi-Fi Password:</label>
        <input type="password" id="pass" name="pass" placeholder="Your Wi-Fi Password" required>

        <button type="submit">Save & Connect</button>
    </form>
</div>
</body>
</html>
)raw";

void handleRoot() {
  if (isAPMode) {
    String html = FPSTR(WEB_PAGE_AP);
    html.replace("%AP_SSID%", AP_SSID);
    html.replace("%AP_PASS%", AP_PASS);
    httpServer.send(200, "text/html", html);
  } else {
    String html = FPSTR(WEB_PAGE_STA);
    html.replace("%STA_SSID%", sta_ssid);
    httpServer.send(200, "text/html", html);
  }
}

void handleSaveWifi() {
  if (!httpServer.hasArg("ssid") || !httpServer.hasArg("pass")) {
    httpServer.send(400, "text/plain", "Missing SSID or Password");
    return;
  }

  String newSsid = httpServer.arg("ssid");
  String newPass = httpServer.arg("pass");
  
  if (newSsid.length() > 32 || newPass.length() > 64) {
      httpServer.send(400, "text/plain", "SSID or Password too long.");
      return;
  }

  newSsid.toCharArray(sta_ssid, sizeof(sta_ssid));
  newPass.toCharArray(sta_password, sizeof(sta_password));
  
  saveWifiConfig();

  httpServer.send(200, "text/plain", "Wi-Fi credentials saved. Restarting to connect...");
  delay(500);
  ESP.restart();
}


void handleConfigAction() {
  String response = "OK";
  int status_code = 200;

  if (httpServer.method() != HTTP_POST) {
      status_code = 405;
      response = "Method not allowed.";
  } else if (httpServer.hasArg("action")) {
    String action = httpServer.arg("action");

    if (action == "clear_wifi") {
        clearWifiConfig();
        response = "Wi-Fi configuration cleared. Restarting to AP Mode...";
        httpServer.send(status_code, "text/plain", response);
        delay(100);
        ESP.restart();
    }
    else if (action == "clear_pair") {
      clearPairedHost();
      response = "Paired host cleared.";
    }
    else if (action == "test") {
      hapticController.runTestRamp();
      response = "Realtime test complete.";
    }
    else if (action == "set_id" && httpServer.hasArg("id")) {
      int newID = httpServer.arg("id").toInt();
      if (newID >= 0 && newID <= 10) {
        saveDeviceID(newID);
        response = "ID updated to " + String(newID) + ". Restarting...";
        httpServer.send(status_code, "text/plain", response);
        delay(100);
        ESP.restart();
      } else {
        status_code = 400;
        response = "Invalid ID (must be 0-10).";
      }
    }
    else if (action == "set_receiver" && httpServer.hasArg("index")) {
      int newIndex = httpServer.arg("index").toInt();
      if (newIndex >= 0 && newIndex < NUM_RECEIVERS) {
        assignedReceiverIndex = newIndex;
        saveAssignedReceiverIndex(assignedReceiverIndex);
        response = "Receiver set to " + String(RECEIVER_NAMES[newIndex]) + ". Restarting to update mDNS...";
        httpServer.send(status_code, "text/plain", response);
        delay(100);
        ESP.restart();
      } else {
        status_code = 400;
        response = "Invalid receiver index.";
      }
    }
    else if (action == "set_gamma" && httpServer.hasArg("gamma")) {
      float newGamma = httpServer.arg("gamma").toFloat();
      if (newGamma >= 0.5f && newGamma <= 6.0f) {
        hapticController.setGamma(newGamma);
        saveGammaToEEPROM(); // Persist the new gamma
        response = "Gamma updated to " + String(hapticController.getGamma(), 2) + ".";
      } else {
        status_code = 400;
        response = "Invalid gamma (must be between 0.5 and 6.0).";
      }
    }
    else {
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
  json += "\"role\":\"Client (VRChat Listener Mode - Realtime)\",";
  json += "\"receiverIndex\":" + String(assignedReceiverIndex) + ",";
  json += "\"receiverName\":\"" + String(RECEIVER_NAMES[assignedReceiverIndex]) + "\",";
  json += "\"hostname\":\"" + getMDNSHostname() + ".local\",";
  json += "\"listeningOn\":" + String(CLIENT_LISTENER_PORT) + ",";
  json += "\"battery\":" + String((int)getBatteryPercent()) + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"gamma\":" + String(hapticController.getGamma(), 2);
  json += "}";
  httpServer.send(200, "application/json", json);
}

// ------------------------------------
// --- Network Setup Flow ---
// ------------------------------------

void setupMDNS() {
  String hostname = getMDNSHostname();
  if (MDNS.begin(hostname.c_str())) {
    Serial.printf("mDNS responder started with hostname: %s.local\n", hostname.c_str());
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("Error starting mDNS!");
  }
}

void setupAPMode() {
  Serial.println("\nStarting AP Mode for Wi-Fi Configuration...");
  isAPMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);

  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  // Setup DNS server for captive portal
  dnsServer.start(DNS_PORT, "*", apIP);

  httpServer.on("/", HTTP_GET, handleRoot);
  httpServer.on("/save_wifi", HTTP_POST, handleSaveWifi);
  // Handle common captive-portal checks and redirect everything else to root
  httpServer.onNotFound([](){
    if (!isAPMode) {
      // If not AP mode, fall back to normal root
      handleRoot();
      return;
    }
    String uri = httpServer.uri();
    // Android/Chrome captive portal probe
    if (uri == "/generate_204") {
      httpServer.send(204, "text/plain", "");
      return;
    }
    // Windows/Apple probes
    if (uri == "/ncsi.txt" || uri == "/hotspot-detect.html") {
      httpServer.send(200, "text/plain", "OK");
      return;
    }
    // Redirect everything else to the root configuration page
    httpServer.sendHeader("Location", "/");
    httpServer.send(302, "text/html", "<html><body>Redirecting to configuration portal...</body></html>");
  });
  httpServer.begin();
  Serial.println("Config Portal started.");
}

void setupSTAMode() {
  Serial.printf("\nConnecting to saved Wi-Fi: %s\n", sta_ssid);
  isAPMode = false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(sta_ssid, sta_password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 60) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected! IP: " + WiFi.localIP().toString());
    
    // Haptics Client Setup (only if connected)
    setupMDNS();
    httpServer.on("/", HTTP_GET, handleRoot);
    httpServer.on("/status_json", HTTP_GET, handleStatusJSON);
    httpServer.on("/action", HTTP_POST, handleConfigAction);
    httpServer.begin();

    if (Udp.begin(CLIENT_LISTENER_PORT)) {
      Serial.printf("CLIENT: Listening for VRChat OSC directly on Port %d.\n", CLIENT_LISTENER_PORT);
    } else {
      Serial.println("CLIENT: Failed to start OSC listener!");
    }
    
    // OTA Setup
    if (myDeviceID != -1) {
      ArduinoOTA.setHostname(("sharkeehaptics" + String(myDeviceID)).c_str());
    } else {
      ArduinoOTA.setHostname("sharkeehaptics-unassigned");
    }
    ArduinoOTA.begin();
    Serial.println("OTA ready.");
    
  } else {
    // Failed to connect - reboot to AP mode to retry configuration
    Serial.println("\nFailed to connect to Wi-Fi. Rebooting to AP Mode...");
    clearWifiConfig(); // clear bad credentials
    delay(2000);
    ESP.restart();
  }
}

// ------------------------------------
// --- Main Setup and Loop ---
// ------------------------------------

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n--- Sharkee Haptics mDNS Client (VRChat Direct Listener) ---");

  EEPROM.begin(EEPROM_SIZE);
  myDeviceID = loadDeviceID();
  assignedReceiverIndex = loadAssignedReceiverIndex(); // Now defaults to 'Head' (index 0)
  loadGammaFromEEPROM();
  loadPairedHostFromEEPROM();

  Serial.printf("Loaded Device ID: %d\n", myDeviceID);
  Serial.printf("Assigned Receiver: %s\n", RECEIVER_NAMES[assignedReceiverIndex]);
  Serial.printf("Loaded Gamma: %.2f\n", hapticController.getGamma());
  Serial.printf("Total Receivers: %d\n", NUM_RECEIVERS);

  // Initialize haptic controller early
  if (!hapticController.begin()) { 
    Serial.println("FATAL ERROR: Haptics chip not found. Check wiring.");
  }

  // Determine configuration state
  if (loadWifiConfig()) {
    setupSTAMode(); // Stored credentials found, attempt connection
  } else {
    setupAPMode(); // No credentials found, launch configuration portal
  }
}

void loop() {
  if (isAPMode) {
    dnsServer.processNextRequest();
    httpServer.handleClient();
  } else {
    handleVRChatOscInput(); // Renamed and updated function
    httpServer.handleClient();
    ArduinoOTA.handle();
    MDNS.update();
    hapticController.checkTimeout(); // Check the watchdog timer
  }
}
