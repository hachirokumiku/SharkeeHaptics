/*
  Sharkee_Haptics_Final_Curve_v19.ino
  
  Goal: Achieve "super robust" haptics by relying entirely on a single intensity curve 
        that changes both the amplitude (Gain) AND the texture (Effect/Waveform) based on 
        the input value (0.0 to 1.0).
*/

// *** Haptic Library Includes ***
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

// DRV2605 I2C Address
#define DRV2605_ADDR 0x5A 

// DRV2605 Register Definitions (Low-Level Control)
#define REG_MODE 0x01
#define REG_LIBRARY 0x03
#define REG_WAVEFORM_SEQ_START 0x04
#define REG_GO 0x0C
#define REG_CONTROL3 0x1D
#define REG_CONTROL4 0x1E
#define REG_RATED_VOLTAGE 0x16
#define REG_ODT_GAIN 0x17 // LRA Gain Control Register

// Mode Bytes
#define MODE_STANDBY 0b01000000 // STANDBY (0x40)
#define MODE_RTP 0b00000005     // RTP
#define MODE_INTERNAL_TRIGGER 0b00000001 // Playback Mode

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
// Single Address: All VRChat haptic sources (petting, force, hits) should map to this one address.
const char* INTERNAL_OSC_ADDRESS = "/sharkeehaptics/set_intensity";

// Haptic Logic 
const float MIN_INTENSITY_THRESHOLD = 0.05f; // Minimum input before activating
const uint8_t LRA_LIBRARY = 6;              

// 1. PETTING ZONE: Low Intensity (0% - 33%) - Smooth, Sustained Hums
const uint8_t PET_EFFECT_ARRAY[] = {1, 3, 5, 8, 11, 15, 20, 24, 28, 30}; 
const int NUM_PET_EFFECTS = sizeof(PET_EFFECT_ARRAY) / sizeof(PET_EFFECT_ARRAY[0]);

// 2. FORCE ZONE: Medium Intensity (33% - 66%) - Stronger, Standard Thumps/Rumbles (Original array)
const uint8_t FORCE_EFFECT_ARRAY[] = {4, 1, 11, 18, 37, 43, 47, 49, 64, 66}; 
const int NUM_FORCE_EFFECTS = sizeof(FORCE_EFFECT_ARRAY) / sizeof(FORCE_EFFECT_ARRAY[0]);

// 3. IMPACT ZONE: High Intensity (66% - 100%) - Sharp, High-Frequency Clicks/Bumps
const uint8_t IMPACT_EFFECT_ARRAY[] = {68, 70, 72, 75, 78, 80, 82, 85, 87, 89}; 
const int NUM_IMPACT_EFFECTS = sizeof(IMPACT_EFFECT_ARRAY) / sizeof(IMPACT_EFFECT_ARRAY[0]);


// Complex Waveform Sequence (Triggered at Index 9 of the current array)
const uint8_t FX_IMPACT_HIT = 49;     
const uint8_t FX_RESIDUAL_HUM = 37;   
const uint8_t FX_PAUSE = 0xFF;        
const uint8_t SEQUENCE_TRIGGER_INDEX = 9; 

// LRA Amplitude (Gain is set directly via REG_ODT_GAIN, 0-127)
const uint8_t MIN_GAIN_VALUE = 20;     // Minimum gain value (out of 127)
const uint8_t MAX_GAIN_VALUE = 127;    // Maximum gain value (0x7F)

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
  // 1. Check Chip
  if (drv.begin() != HAPTIC_SUCCESS) {
    Serial.println("DRV2605 not found. Check wiring.");
    while (1) delay(100); 
  }
  
  // 2. Set Actuator Type and Feedback/Control Registers for LRA
  // FIX: Added 0xFF mask as the third argument to all writeRegBits calls
  drv.writeRegBits(REG_MODE, 0xFF, 0b00000000); // Out of standby
  drv.setActuatorType(LRA); // Use the library helper for this specific setting
  drv.writeRegBits(REG_LIBRARY, 0xFF, LRA_LIBRARY); // Set LRA library (6)
  
  // Set LRA specific control registers for optimal performance
  drv.writeRegBits(0x1A, 0xFF, 0b10110100); // Set Feedback Control: LRA, Auto-Cal, 2.0ms Sample Time
  drv.writeRegBits(REG_CONTROL3, 0xFF, 0b10100000); // Set Control3: LRA_DRIVE_MODE to 'Open-Loop'
  drv.writeRegBits(REG_CONTROL4, 0xFF, 0b00100000); // Set Control4: Auto-Brake (0x20)
  
  // 3. Set Initial Gain and Standby
  drv.writeRegBits(REG_ODT_GAIN, 0xFF, 0b00000000); // Gain off (0x00)
  drv.writeRegBits(REG_MODE, 0xFF, MODE_STANDBY); // Enter standby mode (0x40)
}

float getBatteryPercent() {
  const float V_FULL = 4.2f;
  const float V_EMPTY = 3.3f;
  int raw = analogRead(BATTERY_LEVEL_PIN);
  float voltage = raw * (3.3f / 1023.0f);
  float percent = (voltage - V_EMPTY) / (V_FULL - V_EMPTY) * 100.0f;
  return constrain(percent, 0.0f, 100.0f);
}

// ------------------------------------------------------------------
// --- CORE HAPTIC EXECUTION FUNCTION (Intensity Curve Logic) ---
// ------------------------------------------------------------------
void setMotorLibraryEffect(float intensity) {
  intensity = constrain(intensity, 0.0f, 1.0f);
  
  // --- A. STOP CONDITION ---
  if (intensity < MIN_INTENSITY_THRESHOLD) { 
    drv.writeRegBits(REG_ODT_GAIN, 0xFF, 0x00); // Gain off
    drv.writeRegBits(REG_MODE, 0xFF, MODE_STANDBY); // Standby
    return;
  }
  
  // --- B. NORMALIZATION ---
  // Scale the input from the threshold up to 1.0 to a new 0.0 to 1.0 range
  float normalizedIntensity = (intensity - MIN_INTENSITY_THRESHOLD) / (1.0f - MIN_INTENSITY_THRESHOLD);
  normalizedIntensity = constrain(normalizedIntensity, 0.0f, 1.0f);
  
  // --- C. TEXTURE ZONES: Select Effect Array Based on Normalized Intensity ---
  const uint8_t* current_effect_array = FORCE_EFFECT_ARRAY;
  int num_effects = NUM_FORCE_EFFECTS;
  
  // Zone 1: Petting (0% - 33%)
  if (normalizedIntensity <= 0.33f) {
      current_effect_array = PET_EFFECT_ARRAY;
      num_effects = NUM_PET_EFFECTS;
  } 
  // Zone 2: Force (33% - 66%) - uses the original array
  else if (normalizedIntensity <= 0.66f) {
      current_effect_array = FORCE_EFFECT_ARRAY;
      num_effects = NUM_FORCE_EFFECTS;
  } 
  // Zone 3: Impact (66% - 100%)
  else {
      current_effect_array = IMPACT_EFFECT_ARRAY;
      num_effects = NUM_IMPACT_EFFECTS;
  }

  // Map the normalized intensity to the 10 available effects in the selected array
  int effectIndex = (int)roundf(normalizedIntensity * (num_effects - 1));
  
  // --- D. CALCULATE AMPLITUDE GAIN (Mapped to MIN_GAIN_VALUE to MAX_GAIN_VALUE: 20-127) ---
  // High normalizedIntensity results in high gain_value.
  uint8_t gain_value = (uint8_t)roundf(normalizedIntensity * (MAX_GAIN_VALUE - MIN_GAIN_VALUE)) + MIN_GAIN_VALUE;
  gain_value = constrain(gain_value, MIN_GAIN_VALUE, MAX_GAIN_VALUE); 

  // --- E. EXECUTE WAVEFORM ---
  drv.writeRegBits(REG_MODE, 0xFF, MODE_INTERNAL_TRIGGER); // Internal Trigger/Playback Mode
  drv.writeRegBits(REG_ODT_GAIN, 0xFF, gain_value); // Set Gain
 
  // Check for the complex sequence only at max intensity (index 9) in the highest texture zone (IMPACT)
  if (effectIndex == SEQUENCE_TRIGGER_INDEX && current_effect_array == IMPACT_EFFECT_ARRAY) {
    drv.writeRegBits(REG_WAVEFORM_SEQ_START + 0, 0xFF, FX_IMPACT_HIT);    
    drv.writeRegBits(REG_WAVEFORM_SEQ_START + 1, 0xFF, FX_PAUSE);         
    drv.writeRegBits(REG_WAVEFORM_SEQ_START + 2, 0xFF, FX_RESIDUAL_HUM);  
    drv.writeRegBits(REG_WAVEFORM_SEQ_START + 3, 0xFF, 0); // Terminate
    drv.writeRegBits(REG_GO, 0xFF, 0x01); // Go!
    
  } else {
    // Single Effect from the currently selected array
    uint8_t effect_to_play = current_effect_array[effectIndex];
    drv.writeRegBits(REG_WAVEFORM_SEQ_START + 0, 0xFF, effect_to_play); 
    drv.writeRegBits(REG_WAVEFORM_SEQ_START + 1, 0xFF, 0); // Terminate
    drv.writeRegBits(REG_GO, 0xFF, 0x01); // Go!
  }
}
// ------------------------------------------------------------------

// Helper to run a single waveform test
void runTestWaveform(uint8_t effect, uint8_t gain_percent) {
    // FIX: Added 0xFF mask
    drv.writeRegBits(REG_MODE, 0xFF, MODE_INTERNAL_TRIGGER);
    
    // Scale 0-100 test gain to the MIN_GAIN_VALUE to MAX_GAIN_VALUE (20-127)
    uint8_t gain_value = (uint8_t)roundf(((float)gain_percent / 100.0f) * (MAX_GAIN_VALUE - MIN_GAIN_VALUE)) + MIN_GAIN_VALUE;
    gain_value = constrain(gain_value, MIN_GAIN_VALUE, MAX_GAIN_VALUE);
    
    // FIX: Added 0xFF mask
    drv.writeRegBits(REG_ODT_GAIN, 0xFF, gain_value); 
    
    // FIX: Added 0xFF mask
    drv.writeRegBits(REG_WAVEFORM_SEQ_START + 0, 0xFF, effect);
    drv.writeRegBits(REG_WAVEFORM_SEQ_START + 1, 0xFF, 0);
    drv.writeRegBits(REG_GO, 0xFF, 0x01); 
    delay(400); 
    
    // FIX: Added 0xFF mask
    drv.writeRegBits(REG_MODE, 0xFF, MODE_STANDBY); 
}

// ------------------------------------
// --- Network & Communication ---
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

    // Listen only for the single, master intensity address
    if (strcmp(addressBuffer, INTERNAL_OSC_ADDRESS) == 0) {
      float intensity = 0.0f;
      
      if (msg.isFloat(0)) {
        intensity = msg.getFloat(0);
      } else if (msg.isInt(0)) {
        int val = msg.getInt(0);
        // Map 0-255 or 0-100 integers to 0.0-1.0 float
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
  json += "\"haptic_mode\":\"Intensity Curve (Pet, Force, Impact Zones)\""; 
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
      
      // Test the Petting Zone
      for(int i = 0; i < NUM_PET_EFFECTS; i++) {
          runTestWaveform(PET_EFFECT_ARRAY[i], 25);
          delay(50); 
      }
      // Test the Force Zone
      for(int i = 0; i < NUM_FORCE_EFFECTS; i++) {
          runTestWaveform(FORCE_EFFECT_ARRAY[i], 55);
          delay(50); 
      }
      // Test the Impact Zone
      for(int i = 0; i < NUM_IMPACT_EFFECTS; i++) {
          runTestWaveform(IMPACT_EFFECT_ARRAY[i], 90);
          delay(50); 
      }

      // Complex sequence test at max gain
      drv.writeRegBits(REG_MODE, 0xFF, MODE_INTERNAL_TRIGGER);
      drv.writeRegBits(REG_ODT_GAIN, 0xFF, MAX_GAIN_VALUE); 
      
      drv.writeRegBits(REG_WAVEFORM_SEQ_START + 0, 0xFF, FX_IMPACT_HIT);    
      drv.writeRegBits(REG_WAVEFORM_SEQ_START + 1, 0xFF, FX_PAUSE);         
      drv.writeRegBits(REG_WAVEFORM_SEQ_START + 2, 0xFF, FX_RESIDUAL_HUM);  
      drv.writeRegBits(REG_WAVEFORM_SEQ_START + 3, 0xFF, 0);                
      drv.writeRegBits(REG_GO, 0xFF, 0x01);
      delay(800);
      
      drv.writeRegBits(REG_ODT_GAIN, 0xFF, 0x00);
      drv.writeRegBits(REG_MODE, 0xFF, MODE_STANDBY);
      response = "Tested all three texture zones and complex sequence. Switched back to standby.";
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
    html += "<p>Mode: <b>Intensity Curve (Pet, Force, Impact Zones)</b></p>"; 
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
// --- Setup/Boot Logic (Unchanged) ---
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

        Wire.begin(); // Ensure Wire (I2C) is initialized before talking to DRV2605
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
  Wire.begin(); // Initialized here for EEPROM/AP Mode and checked again in Station Mode.
  EEPROM.begin(EEPROM_SIZE);

  assignedReceiverIndex = EEPROM.read(RECEIVER_NAME_ADDR);
  if (assignedReceiverIndex < 0 || assignedReceiverIndex >= NUM_RECEIVERS) {
    assignedReceiverIndex = 0; 
  }
  
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
