# Esp32_bw_dac
This Project focuses on transmitting digital audio to external DAC like PCM5102A which uses i2s communication by utilizing esp32 bluetooth or wifi wireless compatibility. Its kind of hybrid project and user can either connect from bluetooth or wifi at a time.



## Esp32 Gpio pin connection to PCM5102A
### 1. GPIO-22 : DIN (Data In)
### 2. GPIO-25:  LCK (Left/Right Clock)
### 3. GPIO-26:  BIN (Bit Clock)
### 4. GND : SCK and GND
### 5. 3.3v or 5v : VIN



## Important Note

### 1. Bluetooth Limitation
Bluetooth is config to transmit max of **16bit and 48khz**

### 2. One device Connectivity
Only one device can connect at time via either wifi or bluetooth after connecting to a device it will stop showing in bluetooth and wifi discoverable section.

### 3. Bluetooth issue with Windows
It might not work well with windows as it will disconnect automatically after some time try connecting via wifi.
