# Hardware Setup Guide

## Bill of Materials (Per Device)

### Required Components
- **ESP8266 Module** (NodeMCU, Wemos D1 Mini, or similar)
  - Must support WiFi 802.11 b/g/n
  - Minimum 4MB flash recommended
  
- **Adafruit DRV2605L Haptic Motor Driver**
  - I2C interface
  - Supports LRA and ERM motors
  - Built-in effects library
  
- **Haptic Motor**
  - LRA (Linear Resonant Actuator) - recommended
  - Or ERM (Eccentric Rotating Mass)
  - Voltage: 3.3V - 5V compatible
  
- **Power Supply**
  - 3.7V LiPo battery (recommended for wireless operation)
  - Or 5V via USB (for testing/development)
  - Ensure adequate current capacity (500mA+ recommended)
  
### Optional Components
- Voltage divider resistors for battery monitoring (2x resistors)
- JST connector for battery
- On/off switch
- Enclosure/housing

## Wiring Connections

### ESP8266 to DRV2605L
```
ESP8266          DRV2605L
---------        ---------
3.3V      <--->  VIN
GND       <--->  GND
D1 (GPIO5)<--->  SCL (I2C Clock)
D2 (GPIO4)<--->  SDA (I2C Data)
```

### DRV2605L to Motor
```
DRV2605L         Motor
---------        ------
OUT+      <--->  Motor + (Red wire typically)
OUT-      <--->  Motor - (Black wire typically)
```

### Battery Voltage Monitor (Optional)
```
Battery+  ---[R1: 100kΩ]---+--- A0 (ESP8266)
                           |
                        [R2: 47kΩ]
                           |
                          GND

Note: Adjust R1/R2 values based on battery voltage range.
Formula: Vout = Vin × (R2 / (R1 + R2))
ESP8266 A0 max input: 1.0V (3.3V with voltage divider on some boards)
```

## Assembly Steps

1. **Connect I2C Pins**
   - Solder or connect SCL and SDA between ESP8266 and DRV2605L
   - Use short wires (< 10cm) for stable I2C communication
   
2. **Power Connections**
   - Connect VIN and GND from ESP8266 to DRV2605L
   - Ensure solid connections - poor connections cause instability
   
3. **Attach Motor**
   - Connect motor to OUT+ and OUT- on DRV2605L
   - Polarity matters for LRA motors (affects phase)
   
4. **Battery (Optional)**
   - Connect LiPo battery to ESP8266 VIN and GND
   - Add voltage divider to A0 for battery monitoring
   - Include protection circuit if not integrated in battery
   
5. **Housing**
   - Secure components in a compact enclosure
   - Ensure motor has good contact with body/clothing
   - Consider vibration isolation between motor and electronics

## Hardware Configuration

### DRV2605L Mode
The firmware configures the DRV2605L for:
- **Library**: 1 (ERM motors) or 6 (LRA motors)
- **Mode**: Real-Time Playback (for smooth continuous control)
- **Waveform**: Direct intensity control (0-127)

### Motor Selection
- **LRA (recommended)**: 
  - Faster response time
  - More precise control
  - Typically 150-200Hz resonant frequency
  - Examples: Adafruit #1201, Precision Microdrives C10-100
  
- **ERM**:
  - Wider availability
  - Lower cost
  - Slower response time
  - Examples: Standard vibration motors 3V-5V

## Testing Hardware

1. **Power On**: ESP8266 should boot and connect to WiFi
2. **Serial Monitor**: Check for "DRV2605 initialization...done"
3. **Web Interface**: Navigate to device IP to access config page
4. **Test Button**: Use web interface to trigger test haptic effect
5. **Motor Response**: Should feel distinct "click" sensation

## Troubleshooting

### Motor Doesn't Respond
- Check I2C connections (SCL/SDA)
- Verify motor connections (OUT+/OUT-)
- Check serial monitor for DRV2605 initialization errors
- Try different I2C pins if using custom board

### Weak Haptic Feedback
- Check battery voltage (should be > 3.3V)
- Verify motor is rated for operating voltage
- Ensure motor has good mechanical coupling
- Try increasing intensity values in code

### WiFi Connection Issues
- Verify SSID and password in code
- Check WiFi signal strength (RSSI in web interface)
- Ensure 2.4GHz WiFi (ESP8266 doesn't support 5GHz)
- Try moving closer to router

### I2C Communication Errors
- Reduce wire length between ESP8266 and DRV2605L
- Add pull-up resistors (4.7kΩ) to SCL and SDA if needed
- Check for loose connections
- Verify correct I2C address (0x5A for DRV2605L)

## Power Consumption

Typical current draw:
- **Idle**: 80-100mA (WiFi connected)
- **Active haptic**: 150-300mA (depending on intensity)
- **Peak**: Up to 500mA

Battery life estimate (1000mAh LiPo):
- Continuous use: 3-5 hours
- Moderate use: 6-10 hours
- Standby: 10-15 hours

## Safety Notes

- **Never** short circuit LiPo batteries
- Use batteries with built-in protection circuits
- Don't exceed 1.0V input on ESP8266 A0 pin
- Keep motors away from moisture
- Ensure enclosures have adequate ventilation
- Monitor battery temperature during charging

## Advanced: PCB Design

For permanent installations, consider designing a custom PCB:
- Integrate ESP8266 module, DRV2605L, and power circuit
- Add battery charging circuit (TP4056 or similar)
- Include voltage regulation (if using higher voltage batteries)
- Add status LEDs for power/WiFi/charging
- Keep I2C traces short and away from noisy signals
- Ground planes for stable operation

## References

- [Adafruit DRV2605L Guide](https://learn.adafruit.com/adafruit-drv2605-haptic-controller-breakout)
- [ESP8266 Arduino Core Documentation](https://arduino-esp8266.readthedocs.io/)
- [DRV2605L Datasheet](https://www.ti.com/product/DRV2605L)
