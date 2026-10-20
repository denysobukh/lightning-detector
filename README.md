# Lightning Detector

Arduino firmware for an AS3935 lightning sensor, an LGT8F328-32 MCU, and a seven-pixel NeoPixel strip. The sketch reports sensor events over serial and displays distance-dependent lightning indications.

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

Connect board, sensor, and strip grounds together. Configure the sensor module for I2C and use supply and signal voltages appropriate for your modules. Confirm the sensor address before changing `AS3935_ADDR`.

## Build and Upload

Install the LGT8F board core and Adafruit NeoPixel library using Arduino IDE's Boards Manager and Library Manager. The tested configuration is LGT8F328P-LQFP32 with the internal 32 MHz oscillator divided by two, giving a **16 MHz CPU clock**.

With Arduino CLI and the board core installed:

```sh
arduino-cli lib install "Adafruit NeoPixel"
BOARD_FQBN='lgt8fx:avr:328:variant=modelP,clock_source=internal,clock_div=2'
arduino-cli compile --fqbn "$BOARD_FQBN" .
arduino-cli upload --fqbn "$BOARD_FQBN" -p /dev/cu.usbmodem00001 .
arduino-cli monitor -p /dev/cu.usbmodem00001 -c baudrate=9600
```

Adjust the port for your machine. Tested dependencies are LGT8F core 2.0.7 and Adafruit NeoPixel 1.15.5. The installed NeoPixel library rejects 32 MHz CPU builds.

## LED Indications

- **Startup:** one blue pixel travels along the strip and back.
- **Battery after reset:** before the blue animation, shows estimated charge for five seconds using green pixels (rounded up). At 0% all pixels blink red every 500 ms. Voltage and estimated charge print at 9600 baud every 500 ms.
- **Noise:** one pink flash at brightness 50.
- **Disturber:** one green flash at brightness 10.
- **Lightning:** selected pixels start light blue, fade together through blue to black, at brightness 100.

Brightness values use the library's 0-255 scale.

The startup battery gauge matches `../measure_voltage`: 2.048 V internal reference, 12-bit ADC, 3.0 divider ratio (nominal 0–6.144 V range), 16-sample averaging, and smoothing. Battery display brightness is 100. Charge is an approximate resting-voltage estimate for a conventional 4.2 V Li-ion cell; compare voltage readings with a multimeter. Measurements stop after the startup display, and the existing ADC clock shutdown and IRQ/sleep behavior continue unchanged.

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

Startup should print `AS3935 Ready and Listening!`, decoded registers, and the low-power message. I2C transactions have a 25 ms timeout; Wire status 5 indicates a timeout. Check wiring, power, I2C mode, address, and pull-ups. Idle SDA/SCL readings should both be 1, but that alone does not prove communication works. Reset after fixing a failed startup.

Compilation and startup were verified at 16 MHz. IRQ wake-up, LED behavior, and current consumption still need hardware verification.
