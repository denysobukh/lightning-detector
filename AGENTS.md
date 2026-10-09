# Repository Guidelines

## Project Structure & Module Organization

This repository contains a single Arduino sketch for an AS3935 lightning sensor running on an LGT8F328-32 MCU:

- `lightning_2026_06.ino`: main sketch with I2C helpers, AS3935 setup, interrupt handling, and serial output.

Keep sensor constants, tuning values, and pin definitions near the top of the sketch so hardware changes are easy to review. If the project grows, place reusable helpers in `.h`/`.cpp` files beside the sketch and keep the main `.ino` focused on `setup()` and `loop()`.

## Build, Test, and Development Commands

Use the Arduino IDE or Arduino CLI with the installed LGT8F board core. Select the board and clock configuration matching the LGT8F328-32 hardware. Replace `YOUR_LGT8F_BOARD_FQBN` and `/dev/cu.usbmodemXXXX` with the appropriate board FQBN and serial port; use `arduino-cli board listall` to find installed board identifiers.

```sh
arduino-cli compile --fqbn YOUR_LGT8F_BOARD_FQBN .
arduino-cli upload -p /dev/cu.usbmodemXXXX --fqbn YOUR_LGT8F_BOARD_FQBN .
arduino-cli monitor -p /dev/cu.usbmodemXXXX -c baudrate=9600
```

`compile` verifies the sketch, `upload` flashes it to the board, and `monitor` opens the serial log used by the sketch.

## Coding Style & Naming Conventions

Use two-space indentation, Arduino/C++ style braces, and descriptive names for hardware behavior. Constants and tunable settings should remain uppercase, such as `IRQ_PIN`, `NOISE_FLOOR`, and `ANTENNA_TUNING`. Keep low-level register helpers small and predictable; for example, `rd()` and `wr()` should only perform I2C register access.

Prefer comments that explain hardware timing, register masks, or calibration choices. Avoid broad rewrites when changing sensor tuning.

## Testing Guidelines

There is no automated test suite in this repository. Before submitting changes, run a compile check and perform a hardware smoke test:

- Confirm `AS3935 Ready and Listening!` appears at `9600` baud.
- Trigger or simulate interrupts and verify noise, disturber, and lightning messages still print correctly.
- Recheck I2C address changes against an I2C scanner result.

## Commit & Pull Request Guidelines

This directory is not currently a git repository, so no commit history is available to infer conventions. Use concise, imperative commit messages when version control is added, for example `Tune AS3935 noise floor`.

Pull requests should describe the hardware tested, board type, AS3935 module variant, I2C address, and any tuning changes. Include serial monitor excerpts when behavior changes.

## Hardware & Configuration Notes

The sketch assumes I2C address `0x03`, IRQ pin `2`, and a `100000` Hz I2C clock. Document any wiring or antenna-tuning changes in the pull request so future contributors can reproduce results.

The target MCU is LGT8F328-32. Verify sleep modes, interrupt wake sources, peripheral power controls, and timer behavior against the LGT8F documentation and installed core before changing power management. Do not assume ATmega328P-specific behavior applies. An Arduino Uno compile alone does not validate this hardware.
