/*
  Sharkee_Haptics_Final_Expressive_v8_1.2.4_FIXED.ino
  NOTE: This sketch is tailored specifically for the Adafruit DRV2605 Library v1.2.4.
  FIX: Replaced all unsupported functions with drv.setAudioGain(0-3).
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Adafruit_DRV2605.h>
#include <WiFiUdp.h>
#include <OSCMessage.h>
#include <Wire.h> 
#include <EEPROM.h> 

// ------------------------------------
// --- Configuration & Constants ---
// ------------------------------------

// *** ⚠️ REQUIRED: REPLACE THESE PLACEHOLDERS WITH YOUR WIFI CREDENTIALS ⚠️ ***
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Network
const unsigned int CLIENT_LISTENER_PORT = 8000;
const char* INTERNAL_OSC_ADDRESS = "/sharkeehaptics/set_intensity";

// Haptic Logic 
const float MIN_INTENSITY_THRESHOLD = 0.05f; // Min input to activate haptics
const uint8_t LRA_LIBRARY = 6;              // Library for LRA actuators

// Array of 10 LRA Library 6 Effects (Low to High Intensity Textures)
const uint8_t MAX_EFFECT_ARRAY[] = {
    4, 1, 11, 18, 37, 43, 47, 49, 64, 66
};
const int NUM_EFFECTS = sizeof(MAX_EFFECT_ARRAY) / sizeof(MAX_EFFECT_ARRAY[0]);

// Constants for the Complex Waveform Sequence (Triggered at Index 9)
const uint8_t FX_IMPACT_HIT = 49;     
const uint8_t FX_RESIDUAL_HUM = 37;   
const uint8_t FX_PAUSE = 0xFF;        
const uint8_t SEQUENCE_TRIGGER_INDEX = 9; 
// Gain is now controlled via the 0-3 global gain register (0=off, 1=low, 2=med, 3=high)
const uint8_t MIN_GAIN_LEVEL = 1;     
const uint8_t MAX_GAIN_LEVEL = 3;     

// Device Naming and Configuration
#define EEPROM_SIZE 32
#define RECEIVER_NAME_ADDR 1
#define GAMMA_ADDR 2         
#define GAMMA_FLAG_ADDR 6    
#define BATTERY_LEVEL_PIN A0 

const int NUM_RECEIVERS = 11; 
const char* receiverNames[NUM_RECEIVERS] = {
  "head", "chest", "upperarm_l", "upperarm_r", "hips", "upperleg_l",
  "upperleg_r", "lowerleg_l", "lowerleg_r", "foot_l", "foot_r"
};
int assignedReceiverIndex = 0;
float GAMMA = 2.2f;

// ------------------------------------
// --- Global Objects ---
// ------------------------------------
ESP8266WebServer httpServer(80);
WiFiUDP Udp;
Adafruit_DRV2605 drv;
char incomingPacket[256];

// ------------------------------------
// --- Helper Functions ---
// ------------------------------------

float getBatteryPercent() {
  const float V_FULL = 4.2f;
  const float V_EMPTY = 3.3f;
  int raw = analogRead(BATTERY_LEVEL_PIN);
  float voltage = raw * (3.3f / 1023.0f);
  float percent = (voltage - V_EMPTY) / (V_FULL - V_EMPTY) * 100.0f;
  return constrain(percent, 0.0f, 100.0f);
}

// ------------------------------------
// --- Haptic Control: Core Logic ---
// ------------------------------------

void setMotorLibraryEffect(float intensity) {
  intensity = constrain(intensity, 0.0f, 1.0f);
  
  float invertedIntensity = 1.0f - intensity; 
  
  // --- A. STOP CONDITION ---
  if (invertedIntensity < MIN_INTENSITY_THRESHOLD) {
    // *** FIX: Use setAudioGain(0) ***
    drv.setAudioGain(0); 
    drv.stop();
    drv.setMode(0); // Standby
    return;
  }
  
  // --- B. NORMALIZATION & MAPPING ---
  float normalizedIntensity = (invertedIntensity - MIN_INTENSITY_THRESHOLD) / (1.0f - MIN_INTENSITY_THRESHOLD);
  normalizedIntensity = constrain(normalizedIntensity, 0.0f, 1.0f);
  
  int effectIndex = (int)roundf(normalizedIntensity * (NUM_EFFECTS - 1));
  
  // --- C. CALCULATE AMPLITUDE GAIN (Mapped to 1-3 Register Value) ---
  uint8_t gain_level = (uint8_t)roundf(normalizedIntensity * (MAX_GAIN_LEVEL - MIN_GAIN_LEVEL)) + MIN_GAIN_LEVEL;
  gain_level = constrain(gain_level, MIN_GAIN_LEVEL, MAX_GAIN_LEVEL); 

  // --- D. EXECUTE WAVEFORM ---
  drv.setMode(1); // Internal Trigger/Library Playback Mode
  
  // *** FIX: Correct function for v1.2.4 ***
  drv.setAudioGain(gain_level); 

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
// --- Web Server Handlers ---
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
  json += "\"haptic_mode\":\"10-Step Library + Audio Gain (0-3)\"";
  json += "}";
  httpServer.send(200, "application/json", json);
}

// Helper to run a single waveform test
void runTestWaveform(uint8_t effect, uint8_t gain_percent) {
    drv.setMode(1);
    
    // Convert 0-100% test gain to 1-3 register level
    uint8_t gain_level = (uint8_t)roundf(((float)gain_percent / 100.0f) * (MAX_GAIN_LEVEL - MIN_GAIN_LEVEL)) + MIN_GAIN_LEVEL;
    gain_level = constrain(gain_level, MIN_GAIN_LEVEL, MAX_GAIN_LEVEL);
    
    // *** FIX: Use setAudioGain() ***
    drv.setAudioGain(gain_level); 
    
    drv.setWaveform(0, effect);
    drv.setWaveform(1, 0);
    drv.go();
    delay(400); 
    drv.setMode(0); // Standby after test
}

void handleConfigAction() {
  String response = "OK";
  int status_code = 200;
  if (httpServer.method() != HTTP_POST) { status_code = 405; response = "Method not allowed."; } 
  else if (httpServer.hasArg("action")) {
    String action = httpServer.arg("action");

    if (action == "test_all") {
      // Test the first 9 effects with escalating gain
      for(int i = 0; i < NUM_EFFECTS - 1; i++) {
          float testNormalized = (float)i / (NUM_EFFECTS - 2);
          uint8_t gain_percent = (uint8_t)roundf(testNormalized * 90.0f) + 10;
          runTestWaveform(MAX_EFFECT_ARRAY[i], gain_percent);
          delay(100); 
      }
      
      // Test the complex sequence at max gain (100)
      drv.setMode(1); 
      // *** FIX: Max gain (100) maps to the highest gain level (3) ***
      drv.setAudioGain(MAX_GAIN_LEVEL); 
      
      drv.setWaveform(0, FX_IMPACT_HIT);    
      drv.setWaveform(1, FX_PAUSE);         
      drv.setWaveform(2, FX_RESIDUAL_HUM);  
      drv.setWaveform(3, 0);                
      drv.go();
      delay(800);
      drv.setAudioGain(0); // Reset gain
      drv.setMode(0);
      
      response = "Tested all 10 levels (9 single + 1 sequence) with global gain ramping. Switched back to standby.";
    }
    else if (action == "set_receiver" && httpServer.hasArg("index")) {
      int newIndex = httpServer.arg("index").toInt();
      if (newIndex >= 0 && newIndex < NUM_RECEIVERS) {
        EEPROM.write(RECEIVER_NAME_ADDR, newIndex); EEPROM.commit();
        response = "Receiver set to " + String(receiverNames[newIndex]) + ". Restarting...";
        httpServer.send(status_code, "text/plain", response); delay(100); ESP.restart();
      } else { status_code = 400; response = "Invalid receiver index."; }
    }
    else if (action == "set_gamma" && httpServer.hasArg("gamma")) {
        response = "Gamma is not used in Library Effects mode.";
        status_code = 200; 
    }
  }
  httpServer.send(status_code, "text/plain", response);
}

// *** Function Declaration Order Fixed ***
void handleConfigPage() {
    String html = "<html><head><title>Haptic Device Config</title>";
    html += "<style>body{font-family:Arial;background-color:#222;color:#eee;} .container{max-width:400px;margin:50px auto;padding:20px;background-color:#333;border-radius:8px;} h2{color:#4CAF50;} label, select, input, button{display:block;width:100%;margin-bottom:10px;padding:8px;border-radius:4px;} select, input{background-color:#444;color:#eee;border:1px solid #555;} button{background-color:#9C27B0;color:white;border:none;cursor:pointer;} .status{margin-top:15px;padding:10px;background-color:#444;border-radius:4px;}</style>";
    html += "</head><body><div class='container'><h2>Max Expressive Haptics Config</h2>";
    html += "<p>Mode: <b>10-Step Library + Audio Gain (0-3)</b></p>";
    html += "<p>Hostname: <b>" + String(receiverNames[assignedReceiverIndex]) + ".local</b></p>";
    html += "<p>IP Address: <b>" + WiFi.localIP().toString() + "</b></p>";
    html += "<p>Battery: <b>" + String((int)getBatteryPercent()) + "%</b></p>";
    
    // Test Button
    html += "<form action='/action' method='post'><input type='hidden' name='action' value='test_all'><button type='submit'>Run Full Ramping Test Sequence</button></form>";

    // Set Receiver Dropdown
    html += "<form action='/action' method='post'><label for='receiver_index'>Set Receiver (Hostname)</label><select id='receiver_index' name='index'>";
    for(int i=0; i<NUM_RECEIVERS; i++) {
        html += "<option value='" + String(i) + "'" + (i == assignedReceiverIndex ? " selected" : "") + ">" + receiverNames[i] + "</option>";
    }
    html += "</select><input type='hidden' name='action' value='set_receiver'><button type='submit'>Change Receiver & Restart</button></form>";

    html += "</div></body></html>";
    httpServer.send(200, "text/html", html);
}

// ------------------------------------
// --- Setup and Loop ---
// ------------------------------------

void setup() {
  Serial.begin(115200);
  Wire.begin(); 

  EEPROM.begin(EEPROM_SIZE);
  assignedReceiverIndex = EEPROM.read(RECEIVER_NAME_ADDR);
  if (assignedReceiverIndex < 0 || assignedReceiverIndex >= NUM_RECEIVERS) {
    assignedReceiverIndex = 0; 
  }
  if (EEPROM.read(GAMMA_FLAG_ADDR) == 0xA5) {
      EEPROM.get(GAMMA_ADDR, GAMMA);
      if (isnan(GAMMA) || GAMMA < 0.5f || GAMMA > 6.0f) GAMMA = 2.2f;
  }
  
  // DRV2605 Initialization
  if (!drv.begin()) {
    Serial.println("DRV2605 not found. Check wiring.");
    while (1) delay(100);
  }
  
  drv.useLRA(); 
  drv.selectLibrary(LRA_LIBRARY); 
  // *** FIX: Use setAudioGain(0) ***
  drv.setAudioGain(0); 
  drv.setMode(0); // Start in Standby mode

  // Wi-Fi Connection
  Serial.printf("Connecting to %s...", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

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
}

void loop() {
  handleRouterOscInput(); 
  httpServer.handleClient();
  MDNS.update();
}
