/*
 Sharkee_Haptics_Final_Expressive_v14_ERROR_FIXED.ino
 
 FIXED: Reverted Haptic control to use:
 - drv.setGain() instead of drv.setAmplitude()
 - drv.setLibrary() instead of drv.selectLibrary()
 - Enumerated values (e.g., STANDBY_MODE, REGISTER_MODE) for drv.setMode()
 
 This version is compatible with the older/alternative PatternAgents library fork.
*/

// *** Haptic Library Includes (PatternAgents) ***
#include "Haptic_DRV2605.h"

// *** Core ESP/Networking Includes ***
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <OSCMessage.h>
#include <Wire.h> 
#include <EEPROM.h> 
#include <DNSServer.h> 

// ------------------------------------
// --- Configuration & Constants ---
// ------------------------------------

// AP Provisioning Configuration
const char *AP_SSID = "SHARKEE_HAPTICS_SETUP";
const char *AP_PASSWORD = "password"; 
IPAddress apIP(192, 168, 4, 1);

// EEPROM Address Map
#define EEPROM_SIZE 128         
#define RECEIVER_NAME_ADDR 1    
#define WIFI_FLAG_ADDR 10       
#define SSID_ADDR 11            
#define PASS_ADDR 43            

// Wi-Fi Storage Variables
char storedSSID[32];
char storedPASS[64];
bool inAPMode = false;

// Network
const unsigned int CLIENT_LISTENER_PORT = 8000;
const char* INTERNAL_OSC_ADDRESS = "/sharkeehaptics/set_intensity";

// Haptic Logic 
const float MIN_INTENSITY_THRESHOLD = 0.05f; 
const uint8_t LRA_LIBRARY = 6;              

// Array of 10 LRA Library 6 Effects (Low to High Intensity Textures)
const uint8_t MAX_EFFECT_ARRAY[] = {
    4, 1, 11, 18, 37, 43, 47, 49, 64, 66
};
const int NUM_EFFECTS = sizeof(MAX_EFFECT_ARRAY) / sizeof(MAX_EFFECT_ARRAY[0]);

// Complex Waveform Sequence (Triggered at Index 9)
const uint8_t FX_IMPACT_HIT = 49;     
const uint8_t FX_RESIDUAL_HUM = 37;   
const uint8_t FX_PAUSE = 0xFF;        
const uint8_t SEQUENCE_TRIGGER_INDEX = 9; 

// AMPLITUDE SCALING: Now mapped to a 0-100 range for setGain()
const uint8_t MIN_GAIN_SCALE = 10;     // Minimum gain when active (10%)
const uint8_t MAX_GAIN_SCALE = 100;    // Maximum gain (100%)

// Device Naming and Configuration
#define BATTERY_LEVEL_PIN A0 

const int NUM_RECEIVERS = 11; 
const char* receiverNames[NUM_RECEIVERS] = {
 "head", "chest", "upperarm_l", "upperarm_r", "hips", "upperleg_l",
 "upperleg_r", "lowerleg_l", "lowerleg_r", "foot_l", "foot_r"
};
int assignedReceiverIndex = 0;

// ------------------------------------
// --- Global Objects ---
// ------------------------------------
ESP8266WebServer httpServer(80);
DNSServer dnsServer; 
WiFiUDP Udp;
Haptic_DRV2605 drv; 
char incomingPacket[256];

// ------------------------------------
// --- Haptic Control: Core Logic ---
// ------------------------------------

void setupHaptics() {
  // DRV2605 Initialization
  if (drv.begin() != HAPTIC_SUCCESS) {
    Serial.println("DRV2605 not found. Check wiring.");
    while (1) delay(100); 
  }
  
  drv.setActuatorType(LRA); 
  // FIX: Using setLibrary() instead of selectLibrary()
  drv.setLibrary(LRA_LIBRARY); 
  
  // FIX: Using setGain() with the 0-100 scale instead of setAmplitude()
  drv.setGain(0); 
  
  // FIX: Using the enumerated type STANDBY_MODE (which is typically 0)
  drv.setMode(STANDBY_MODE); 
}

float getBatteryPercent() {
 const float V_FULL = 4.2f;
 const float V_EMPTY = 3.3f;
 int raw = analogRead(BATTERY_LEVEL_PIN);
 float voltage = raw * (3.3f / 1023.0f);
 float percent = (voltage - V_EMPTY) / (V_FULL - V_EMPTY) * 100.0f;
 return constrain(percent, 0.0f, 100.0f);
}

void setMotorLibraryEffect(float intensity) {
  intensity = constrain(intensity, 0.0f, 1.0f);
  
  float invertedIntensity = 1.0f - intensity; 
  
  // --- A. STOP CONDITION ---
  if (invertedIntensity < MIN_INTENSITY_THRESHOLD) {
    // FIX: Using setGain(0) and STANDBY_MODE
    drv.setGain(0); 
    drv.stop();
    drv.setMode(STANDBY_MODE); 
    return;
  }
  
  // --- B. NORMALIZATION & MAPPING ---
  float normalizedIntensity = (invertedIntensity - MIN_INTENSITY_THRESHOLD) / (1.0f - MIN_INTENSITY_THRESHOLD);
  normalizedIntensity = constrain(normalizedIntensity, 0.0f, 1.0f);
  
  int effectIndex = (int)roundf(normalizedIntensity * (NUM_EFFECTS - 1));
  
  // --- C. CALCULATE AMPLITUDE GAIN (Mapped to MIN_GAIN_SCALE to MAX_GAIN_SCALE: 10-100) ---
  uint8_t gain_value = (uint8_t)roundf(normalizedIntensity * (MAX_GAIN_SCALE - MIN_GAIN_SCALE)) + MIN_GAIN_SCALE;
  gain_value = constrain(gain_value, MIN_GAIN_SCALE, MAX_GAIN_SCALE); 

  // --- D. EXECUTE WAVEFORM ---
  // FIX: Using REGISTER_MODE (which is typically 1)
  drv.setMode(REGISTER_MODE); 
  // FIX: Using setGain()
  drv.setGain(gain_value); 

  if (effectIndex == SEQUENCE_TRIGGER_INDEX) {
    // 10th level: COMPLEX HIT SEQUENCE
    drv.setWaveform(0, FX_IMPACT_HIT);    
    drv.setWaveform(1, FX_PAUSE);         
    drv.setWaveform(2, FX_RESIDUAL_HUM);  
    drv.setWaveform(3, 0); // Terminate
    drv.go();
    
  } else {
    // Levels 0-8: SINGLE EFFECT from the array
    uint8_t effect_to_play = MAX_EFFECT_ARRAY[effectIndex];
    drv.setWaveform(0, effect_to_play); 
    drv.setWaveform(1, 0); // Terminate the sequence
    drv.go();
  }
}

// Helper to run a single waveform test
void runTestWaveform(uint8_t effect, uint8_t gain_percent) {
    // FIX: Using REGISTER_MODE
    drv.setMode(REGISTER_MODE);
    
    // Scale 0-100 test gain to the MIN_GAIN_SCALE to MAX_GAIN_SCALE (10-100)
    uint8_t gain_value = (uint8_t)roundf(((float)gain_percent / 100.0f) * (MAX_GAIN_SCALE - MIN_GAIN_SCALE)) + MIN_GAIN_SCALE;
    gain_value = constrain(gain_value, MIN_GAIN_SCALE, MAX_GAIN_SCALE);
    
    // FIX: Using setGain()
    drv.setGain(gain_value); 
    
    drv.setWaveform(0, effect);
    drv.setWaveform(1, 0);
    drv.go();
    delay(400); 
    // FIX: Using STANDBY_MODE
    drv.setMode(STANDBY_MODE); 
}

// ------------------------------------
// --- Network & Communication (Unchanged) ---
// ------------------------------------

void handleRouterOscInput() {
 int packetSize = Udp.parsePacket();
 if (packetSize != 0) {
  int len = Udp.read((uint8_t*)incomingPacket, sizeof(incomingPacket));
  if (len <= 0) return;
  OSCMessage msg;
  msg.fill((uint8_t*)incomingPacket, len);

  char addressBuffer[64];
  msg.getAddress(addressBuffer);

  if (strcmp(addressBuffer, INTERNAL_OSC_ADDRESS) == 0) {
   float intensity = 0.0f;
   
   if (msg.isFloat(0)) {
    intensity = msg.getFloat(0);
   } else if (msg.isInt(0)) {
    int val = msg.getInt(0);
    if (val > 1 && val <= 255) {
     intensity = (float)val / 255.0f;
    } else if (val >= 0 && val <= 100) {
     intensity = (float)val / 100.0f;
    }
   }
   setMotorLibraryEffect(intensity); 
  }
 }
}


// ------------------------------------
// --- Web Server Handlers (STATION MODE) ---
// ------------------------------------

void handleStatusJSON() {
 String json = "{";
 json += "\"role\":\"Client (Max Expressive Haptics)\","; 
 json += "\"receiverName\":\"" + String(receiverNames[assignedReceiverIndex]) + "\",";
 json += "\"hostname\":\"" + String(receiverNames[assignedReceiverIndex]) + ".local\",";
 json += "\"listeningOn\":" + String(CLIENT_LISTENER_PORT) + ",";
 json += "\"battery\":" + String((int)getBatteryPercent()) + ",";
 json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
 json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
 json += "\"haptic_mode\":\"10-Step Library + Gain (0-100)\""; // Updated Mode Name
 json += "}";
 httpServer.send(200, "application/json", json);
}

void handleConfigAction() {
  String response = "OK";
  int status_code = 200;
  if (httpServer.method() != HTTP_POST) { status_code = 405; response = "Method not allowed."; } 
  else if (httpServer.hasArg("action")) {
    String action = httpServer.arg("action");

    if (action == "test_all") {
      for(int i = 0; i < NUM_EFFECTS - 1; i++) {
          float testNormalized = (float)i / (NUM_EFFECTS - 2);
          uint8_t gain_percent = (uint8_t)roundf(testNormalized * 90.0f) + 10;
          runTestWaveform(MAX_EFFECT_ARRAY[i], gain_percent);
          delay(100); 
      }
      // Complex sequence test at max gain
      // FIX: Using REGISTER_MODE
      drv.setMode(REGISTER_MODE); 
      // FIX: Using setGain(MAX_GAIN_SCALE)
      drv.setGain(MAX_GAIN_SCALE); 
      
      drv.setWaveform(0, FX_IMPACT_HIT);    
      drv.setWaveform(1, FX_PAUSE);         
      drv.setWaveform(2, FX_RESIDUAL_HUM);  
      drv.setWaveform(3, 0);                
      drv.go();
      delay(800);
      
      // FIX: Using setGain(0) and STANDBY_MODE
      drv.setGain(0); 
      drv.setMode(STANDBY_MODE);
      response = "Tested all 10 levels with amplitude ramping. Switched back to standby.";
    }
    else if (action == "set_receiver" && httpServer.hasArg("index")) {
      int newIndex = httpServer.arg("index").toInt();
      if (newIndex >= 0 && newIndex < NUM_RECEIVERS) {
        EEPROM.write(RECEIVER_NAME_ADDR, newIndex); EEPROM.commit();
        response = "Receiver set to " + String(receiverNames[newIndex]) + ". Restarting...";
        httpServer.send(status_code, "text/plain", response); delay(100); ESP.restart();
      } else { status_code = 400; response = "Invalid receiver index."; }
    }
  }
  httpServer.send(status_code, "text/plain", response);
}

void handleConfigPage() {
    String html = "<html><head><title>Haptic Device Config</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<style>body{font-family:Arial;background-color:#222;color:#eee;} .container{max-width:400px;margin:50px auto;padding:20px;background-color:#333;border-radius:8px;} h2{color:#4CAF50;} label, select, input, button{display:block;width:100%;margin-bottom:10px;padding:8px;border-radius:4px;} select, input{background-color:#444;color:#eee;border:1px solid #555;} button{background-color:#9C27B0;color:white;border:none;cursor:pointer;}</style>";
    html += "</head><body><div class='container'><h2>Max Expressive Haptics Config</h2>";
    html += "<p>Mode: <b>10-Step Library + Gain (0-100)</b></p>"; // Updated Mode Name
    html += "<p>Hostname: <b>" + String(receiverNames[assignedReceiverIndex]) + ".local</b></p>";
    html += "<p>IP Address: <b>" + WiFi.localIP().toString() + "</b></p>";
    html += "<p>Battery: <b>" + String((int)getBatteryPercent()) + "%</b></p>";
    
    html += "<form action='/action' method='post'><input type='hidden' name='action' value='test_all'><button type='submit'>Run Full Ramping Test Sequence</button></form>";

    html += "<form action='/action' method='post'><label for='receiver_index'>Set Receiver (Hostname)</label><select id='receiver_index' name='index'>";
    for(int i=0; i<NUM_RECEIVERS; i++) {
        html += "<option value='" + String(i) + "'" + (i == assignedReceiverIndex ? " selected" : "") + ">" + receiverNames[i] + "</option>";
    }
    html += "</select><input type='hidden' name='action' value='set_receiver'><button type='submit'>Change Receiver & Restart</button></form>";

    html += "</div></body></html>";
    httpServer.send(200, "text/html", html);
}

// ------------------------------------
// --- Web Server Handlers (AP MODE) ---
// ------------------------------------

void handleWifiConfig() {
    // Serve the Wi-Fi configuration form
    String html = "<html><head><title>Wi-Fi Config</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<style>body{font-family:Arial;background-color:#1e3a8a;color:white;padding:20px;} .container{max-width:400px;margin:auto;padding:20px;background-color:#1c5f88;border-radius:10px;} h2{color:#facc15;} label, input, button{display:block;width:100%;margin-bottom:15px;padding:12px;border-radius:6px;border:none;} input{background-color:white;color:#1e3a8a;} button{background-color:#10b981;color:white;font-weight:bold;cursor:pointer;}</style>";
    html += "</head><body><div class='container'><h2>Sharkee Haptics Wi-Fi Setup</h2>";
    html += "<p>Connect to the network '" + String(AP_SSID) + "' and configure your home network below.</p>";
    html += "<form action='/save' method='post'>";
    html += "<label for='ssid'>Network SSID</label><input type='text' id='ssid' name='ssid' placeholder='Your Wi-Fi Name' required>";
    html += "<label for='pass'>Password</label><input type='password' id='pass' name='pass' placeholder='Your Wi-Fi Password' required>";
    html += "<button type='submit'>Save & Connect</button></form>";
    html += "</div></body></html>";
    httpServer.send(200, "text/html", html);
}

void handleWifiSave() {
    String new_ssid = httpServer.arg("ssid");
    String new_pass = httpServer.arg("pass");

    if (new_ssid.length() > 0 && new_ssid.length() <= 31) {
        new_ssid.toCharArray(storedSSID, sizeof(storedSSID));
        new_pass.toCharArray(storedPASS, sizeof(storedPASS));

        EEPROM.put(SSID_ADDR, storedSSID);
        EEPROM.put(PASS_ADDR, storedPASS);
        EEPROM.write(WIFI_FLAG_ADDR, 0xA5); 
        EEPROM.commit();

        String html = "<html><head><meta http-equiv='refresh' content='5; url=/'><style>body{font-family:Arial;background-color:#1e3a8a;color:white;padding:20px;} .container{max-width:400px;margin:auto;padding:20px;background-color:#1c5f88;border-radius:10px;} h2{color:#facc15;}</style></head><body><div class='container'><h2>Success!</h2><p>Credentials saved for: <b>" + new_ssid + "</b>.</p><p>Device is restarting to connect to your network...</p></div></body></html>";
        httpServer.send(200, "text/html", html);
        
        delay(1000); 
        ESP.restart();
    } else {
        httpServer.send(400, "text/plain", "Invalid SSID. Please try again.");
    }
}

void handleNotFound() {
    httpServer.sendHeader("Location", "http://" + apIP.toString(), true);
    httpServer.send(302, "text/plain", "Redirecting to config portal...");
}

// ------------------------------------
// --- Setup/Boot Logic ---
// ------------------------------------

void setupAPMode() {
    inAPMode = true; 
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID, AP_PASSWORD);

    Serial.print("Starting AP: ");
    Serial.print(AP_SSID);
    Serial.print(" | IP: ");
    Serial.println(WiFi.softAPIP());

    dnsServer.start(53, "*", apIP); 

    httpServer.on("/", handleWifiConfig);
    httpServer.on("/save", HTTP_POST, handleWifiSave);
    httpServer.onNotFound(handleNotFound);
    httpServer.begin();
}


void setupStationMode() {
    Serial.printf("Connecting to %s...", storedSSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(storedSSID, storedPASS);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nConnected!");
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());

        setupHaptics();

        String hostname = String(receiverNames[assignedReceiverIndex]);
        if (MDNS.begin(hostname.c_str())) {
            Serial.printf("mDNS responder started: %s.local\n", hostname.c_str());
        }

        Udp.begin(CLIENT_LISTENER_PORT);
        Serial.printf("Listening for OSC on UDP port %u\n", CLIENT_LISTENER_PORT);

        httpServer.on("/", handleConfigPage);
        httpServer.on("/status", handleStatusJSON);
        httpServer.on("/action", HTTP_POST, handleConfigAction);
        httpServer.begin();

        inAPMode = false;
    } else {
        Serial.println("\nConnection failed! Starting AP Mode for provisioning.");
        setupAPMode(); 
    }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(); 
  EEPROM.begin(EEPROM_SIZE);

  // 1. Load Haptics Configuration (Receiver ID)
  assignedReceiverIndex = EEPROM.read(RECEIVER_NAME_ADDR);
  if (assignedReceiverIndex < 0 || assignedReceiverIndex >= NUM_RECEIVERS) {
    assignedReceiverIndex = 0; 
  }
  
  // 2. Load Wi-Fi Credentials and decide boot mode
  if (EEPROM.read(WIFI_FLAG_ADDR) == 0xA5) {
      EEPROM.get(SSID_ADDR, storedSSID);
      EEPROM.get(PASS_ADDR, storedPASS);
      if (strlen(storedSSID) > 1) {
          setupStationMode(); 
      } else {
          setupAPMode(); 
      }
  } else {
      setupAPMode(); 
  }
}

void loop() {
  if (inAPMode) {
    dnsServer.processNextRequest();
    httpServer.handleClient();
  } else {
    handleRouterOscInput(); 
    httpServer.handleClient();
    MDNS.update();
  }
}
