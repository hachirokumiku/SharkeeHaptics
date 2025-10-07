# Quick Start Guide

## 5-Minute Setup

### Prerequisites
- ESP8266 with DRV2605L haptic driver (assembled and wired)
- Python 3.7+ installed on your PC
- VRChat with OSC enabled
- WiFi network (2.4GHz)

### Step 1: Flash ESP8266 (5 minutes per device)

1. Install Arduino IDE and ESP8266 board support
2. Install required libraries via Library Manager:
   - Adafruit DRV2605
   - OSC
   - ESP8266 core libraries (WiFi, WebServer, mDNS, OTA)

3. Open `Sharkee_Haptics.ino`

4. Update WiFi credentials (line 15-16):
   ```cpp
   const char* ssid = "YOUR_WIFI_NAME";
   const char* password = "YOUR_WIFI_PASSWORD";
   ```

5. Select your board and port in Arduino IDE

6. Click Upload (wait for compilation and upload ~2 minutes)

7. Open Serial Monitor (115200 baud) to see device IP address

### Step 2: Configure Device (2 minutes per device)

1. Open web browser and navigate to device IP (shown in serial monitor)
   - Example: `http://192.168.1.100`

2. Click "Set Receiver" dropdown

3. Select body location for this device (e.g., "chest", "upperarm_l")

4. Click "Set Receiver" button

5. Device will restart and advertise itself as `[location].local`

6. Repeat for each device with different body locations

### Step 3: Start Python Router (1 minute)

1. Open terminal/command prompt

2. Install dependencies:
   ```bash
   pip install -r requirements.txt
   ```

3. Run the router:
   ```bash
   python sharkee_gui.py
   ```

4. Click "Start Server" button in the GUI

5. Verify devices appear in the client status table

### Step 4: Configure VRChat (5 minutes)

1. Enable OSC in VRChat:
   - Launch VRChat with `--enable-debug-gui --enable-sdk-log-levels`
   - Or use Action Menu → Options → OSC → Enable

2. Configure your avatar with OSC parameters:
   - Add Float parameters to your avatar:
     - `SharkeeHead` (0.0 - 1.0)
     - `SharkeeChest` (0.0 - 1.0)
     - `SharkeeUpperArm_L` (0.0 - 1.0)
     - `SharkeeUpperArm_R` (0.0 - 1.0)
     - `SharkeeHips` (0.0 - 1.0)
     - `SharkeeUpperLeg_L` (0.0 - 1.0)
     - `SharkeeUpperLeg_R` (0.0 - 1.0)
     - `SharkeeLowerLeg_L` (0.0 - 1.0)
     - `SharkeeLowerLeg_R` (0.0 - 1.0)
     - `SharkeeFoot_L` (0.0 - 1.0)
     - `SharkeeFoot_R` (0.0 - 1.0)

3. Link parameters to your avatar's contact receivers or animations

4. Upload avatar to VRChat

5. In VRChat, load your avatar and test!

## Testing

### Test Individual Device
1. Open device web interface (http://[device-ip]/)
2. Click "Test Haptic" button
3. You should feel a distinct "click" sensation
4. If not, check hardware connections (see HARDWARE.md)

### Test Full System
1. In VRChat, trigger one of the haptic parameters (e.g., stand near a contact)
2. Check Python router GUI:
   - "Received" counter should increase
   - "Routed" counter should increase
   - Device status should show intensity value
3. You should feel haptic feedback on the corresponding body location

## Common Issues

### "Device not found" in router
- **Solution**: Check that ESP8266 and PC are on same WiFi network
- **Solution**: Try manually setting IP in router (right-click device → Set Manual IP)
- **Solution**: Check mDNS is working (ping `chest.local` from command line)

### No haptic feedback
- **Solution**: Test motor using web interface test button
- **Solution**: Check VRChat OSC is enabled
- **Solution**: Verify avatar parameters are correctly named
- **Solution**: Check battery level in web interface (low battery = weak haptics)

### Router shows "RESOLVING" status
- **Solution**: Wait a few seconds for mDNS resolution
- **Solution**: Check WiFi connectivity
- **Solution**: Restart router and/or ESP8266

### High latency/lag
- **Solution**: Reduce WiFi congestion (switch to less crowded channel)
- **Solution**: Adjust `INTENSITY_THRESHOLD` in `sharkee_gui.py` (higher = less frequent updates)
- **Solution**: Ensure strong WiFi signal (check RSSI in web interface)

## System Architecture

```
┌─────────────┐
│   VRChat    │  Sends OSC messages with intensity values
└──────┬──────┘  (localhost:9001)
       │
       │ OSC over UDP
       │
┌──────▼──────────────────────┐
│  Python Router (sharkee_gui)│  Routes messages to correct device
│  - mDNS discovery           │  Uses cached IPs for speed
│  - Rate limiting            │
│  - Manual IP override       │
└──────┬──────────────────────┘
       │
       │ OSC over WiFi UDP (port 8000)
       │
┌──────▼────────┐  ┌────────────┐  ┌────────────┐
│  ESP8266      │  │  ESP8266   │  │  ESP8266   │
│  (chest.local)│  │(head.local)│  │ (foot_l... │  ... etc
│               │  │            │  │            │
│  DRV2605L     │  │  DRV2605L  │  │  DRV2605L  │
│     ↓         │  │     ↓      │  │     ↓      │
│  [Motor]      │  │  [Motor]   │  │  [Motor]   │
└───────────────┘  └────────────┘  └────────────┘
```

## Performance Tips

1. **WiFi Optimization**:
   - Use 2.4GHz WiFi with good signal strength
   - Dedicate a WiFi network for haptics if possible
   - Position router centrally

2. **Rate Limiting**:
   - Adjust `INTENSITY_THRESHOLD` in Python code (default 0.03)
   - Higher threshold = fewer packets but more "jumpy" feedback
   - Lower threshold = smoother but more network traffic

3. **Battery Life**:
   - Use larger capacity LiPo batteries
   - Reduce intensity for non-critical feedback
   - Turn off devices when not in use

4. **Latency**:
   - Expected latency: 20-50ms
   - mDNS caching reduces resolution overhead
   - Manual IP override bypasses mDNS entirely

## Next Steps

- **Customize**: Edit intensity thresholds, motor effects, battery monitoring
- **Expand**: Add more body locations (supports up to 11 devices)
- **Optimize**: Tune parameters for your specific hardware and use case
- **Create**: Design custom haptic patterns in VRChat avatars

## Support

- Check README.md for detailed setup instructions
- See HARDWARE.md for wiring and assembly help
- Review code comments for implementation details
- Test each component individually before full system integration

Enjoy your haptic feedback system! 🎮✨
