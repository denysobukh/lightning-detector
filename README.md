# Lightning Detector

Arduino firmware for an AS3935 lightning sensor, an LGT8F328-32 MCU, and a seven-pixel NeoPixel strip. After reset, it displays battery charge for five seconds, then reports sensor events over serial and displays distance-dependent lightning indications. The MCU sleeps between events once LED animations finish.

The main sketch is `lightning_detector.ino`, matching the `lightning_detector` folder name.

## Hardware and Wiring

| Connection | Board pin / setting |
| --- | --- |
| AS3935 SDA | A4 |
| AS3935 SCL | A5 |
| AS3935 IRQ | D2 |
| Strip DIN | D6 through a 330-ohm resistor |
| Battery voltage | Source → 20 kΩ → A1 → 10 kΩ → GND; AREF unconnected |
| I2C address | `0x03` |
| I2C clock | 100 kHz |
| Serial baud rate | 9600 |

Connect battery, board, sensor, and strip grounds together. Configure the sensor module for I2C and use supply and signal voltages appropriate for your modules. Confirm the sensor address before changing `AS3935_ADDR`.

## Build and Upload

Install the LGT8F board core and Adafruit NeoPixel library using Arduino IDE's Boards Manager and Library Manager. The tested configuration is LGT8F328P-LQFP32 with the internal 32 MHz oscillator divided by two, giving a **16 MHz CPU clock**.

Run from the `lightning_detector` folder with Arduino CLI and the board core installed:

```sh
arduino-cli lib install "Adafruit NeoPixel"
BOARD_FQBN='lgt8fx:avr:328:variant=modelP,clock_source=internal,clock_div=2'
arduino-cli compile --fqbn "$BOARD_FQBN" .
arduino-cli upload --fqbn "$BOARD_FQBN" -p /dev/cu.usbmodem00001 .
arduino-cli monitor -p /dev/cu.usbmodem00001 -c baudrate=9600
```

Adjust the port for your machine. Tested dependencies are LGT8F core 2.0.7 and Adafruit NeoPixel 1.15.5. The installed NeoPixel library rejects 32 MHz CPU builds.

If NeoPixel is installed outside the CLI's configured sketchbook, add `--libraries /path/to/Arduino/libraries` to the compile command. Close any serial monitor before uploading so it releases the port.

## Startup Battery Measurement

After each reset, the firmware measures the voltage on A1 and displays estimated charge for five seconds:

- Green pixels show charge, rounded up to the next pixel; each pixel represents approximately 14.3%.
- At 0% estimated charge (3.00 V or below), all seven pixels blink red, switching every 500 ms.
- Voltage and estimated charge print every 500 ms at 9600 baud, for example `Voltage: 3.800 V | Charge (estimated): 60.0%`.

The gauge uses a 2.048 V internal reference, 12-bit ADC, and 3.0 divider ratio, giving a nominal 0–6.144 V conversion range. Each reading averages 16 conversions; subsequent readings use a smoothing factor of 0.25. Battery display brightness is 100 on the 0–255 scale.

Charge follows an approximate resting-voltage curve for a conventional 4.2 V Li-ion cell, from 3.00 V at 0% to 4.20 V at 100%. Tune `CHARGE_VOLTAGES`, `ADC_REFERENCE`, and `DIVIDER_RATIO` for the actual battery and resistor divider, and compare readings with a multimeter.

After five seconds, the battery display clears, the blue startup animation runs, and the AS3935 initializes. Battery measurements stop until the next reset. The ADC clock is then disabled by the existing peripheral shutdown; IRQ handling and sleep between events continue as before.

## LED Indications

- **Battery after reset:** green charge gauge or blinking red at 0%, for five seconds at brightness 100.
- **Startup:** one blue pixel travels along the strip and back.
- **Noise:** one pink flash at brightness 50.
- **Disturber:** one green flash at brightness 10.
- **Lightning:** selected pixels start light blue, fade together through blue to black, at brightness 100.

Brightness values use the library's 0-255 scale.

Event animations run without blocking delays. Each LED has its own frame-buffer state, brightness, and animation start time. A new lightning event restarts only the LEDs selected by its distance; unselected LEDs continue their previous fades. Noise and disturber flashes replace all seven LEDs. The MCU sleeps only after every LED animation finishes.

| Estimated distance | Pixels lit |
| --- | --- |
| Overhead / up to 6 km | 7 |
| Over 6 to 10 km | 6 |
| Over 10 to 17 km | 5 |
| Over 17 to 24 km | 4 |
| Over 24 to 31 km | 3 |
| Over 31 to 40 km | 2 |
| Out of range | 1 |

## Configuration and Sleep

Edit pin definitions, `LED_COLOR_*` constants, and sensor tuning values near the top of `lightning_detector.ino`. Current settings use indoor gain, noise floor 1, watchdog threshold 1, spike rejection 0, and one lightning event per report. Disturbers are unmasked; antenna tuning is 0.

Between events, the MCU uses Idle sleep. ADC, SPI, Timers 1/2, and LGT Timer3 clocks are disabled. Timer0, TWI, and USART0 clocks stop during sleep and resume after wake. Timer0 remains available for active delays and LED timing. Serial output finishes before sleeping; timestamps are omitted.

## Verification and Troubleshooting

Startup should first print battery readings, followed by `AS3935 Ready and Listening!`, decoded registers, and the low-power message. I2C transactions have a 25 ms timeout; Wire status 5 indicates a timeout. Check wiring, power, I2C mode, address, and pull-ups. Idle SDA/SCL readings should both be 1, but that alone does not prove communication works. Reset after fixing a failed startup.

The current sketch compiles for LGT8F328P at 16 MHz: 14,286 bytes of flash and 1,383 bytes of static RAM, leaving 665 bytes for stack and local variables. The latest upload attempt on `/dev/cu.usbmodem00001` failed because the bootloader did not respond at either 57600 or 115200 baud. The port was initially held by a serial monitor; closing it cleared the busy-port error but did not resolve bootloader synchronization. Reconnect the board and check reset/bootloader behavior before retrying.

The updated firmware has not been verified on hardware. Check the five-second battery display and voltage accuracy, successful sensor startup, noise/disturber/lightning IRQ output, LED animations, IRQ wake-up, and current consumption between events.
