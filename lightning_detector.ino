#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <avr/sleep.h>
#include <avr/power.h>
#include <math.h>

// Change this to match your I2C scanner result (usually 0x01, 0x02, or 0x03)
#define AS3935_ADDR 0x03 
#define IRQ_PIN      2
const uint32_t I2C_TIMEOUT_US = 25000;

#define LED_PIN    6     // Nano D6 -> (330 ohm) -> strip DIN
#define NUM_LEDS   7     // set to the real number of LEDs on your strip
#define BRIGHTNESS 50

// Source -> 20 kOhm -> A1 -> 10 kOhm -> GND; leave AREF unconnected.
const uint8_t BATTERY_PIN = A1;
const float ADC_REFERENCE = 2.048;
const float DIVIDER_RATIO = 3.0;
const unsigned long BATTERY_DISPLAY_MS = 5000;
const unsigned long BATTERY_SAMPLE_MS = 500;
const unsigned long BATTERY_BLINK_MS = 500;
const uint8_t BATTERY_BRIGHTNESS = 100;
// Approximate resting-voltage curve for a conventional 4.2 V Li-ion cell.
const float CHARGE_VOLTAGES[] = {
  3.00, 3.40, 3.60, 3.65, 3.70, 3.75, 3.80, 3.90, 4.00, 4.10, 4.20
};

const uint32_t LED_COLOR_OFF = Adafruit_NeoPixel::Color(0, 0, 0);
const uint32_t LED_COLOR_STARTUP = Adafruit_NeoPixel::Color(0, 80, 255);
const uint32_t LED_COLOR_NOISE = Adafruit_NeoPixel::Color(255, 0, 120);
const uint32_t LED_COLOR_DISTURBER = Adafruit_NeoPixel::Color(0, 255, 0);
const uint32_t LED_COLOR_LIGHTNING_START = Adafruit_NeoPixel::Color(173, 216, 230);
const uint32_t LED_COLOR_LIGHTNING_TRANSITION = Adafruit_NeoPixel::Color(0, 0, 255);
const unsigned int LIGHTNING_FRAME_MS = 35;
const uint8_t LIGHTNING_PHASE_FRAMES = 18;

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

volatile bool irqFlag = false;

enum LedIndication : uint8_t { LED_IDLE, LED_FLASH, LED_LIGHTNING };
struct LedFrame {
  unsigned long startedAt;
  uint32_t color;
  LedIndication indication;
  uint8_t brightness;
};
LedFrame ledFrames[NUM_LEDS] = {};
uint32_t flashColor = LED_COLOR_OFF;
unsigned int flashOnMs = 0;
unsigned int flashOffMs = 0;

// ---------------------------
// USER SETTINGS (Cleaned Up)
// ---------------------------
// AFE Gain field values: Indoor = 0x12 (0b10010), Outdoor = 0x0E (0b01110).
// These are shifted into register 0x00 bits [5:1] by applySettings().
uint8_t AFE_GAIN = 0x12;

uint8_t NOISE_FLOOR = 1;            // 0..7 (0 = most sensitive)
uint8_t WATCHDOG_THRESHOLD = 1;     // 0..15
uint8_t SPIKE_REJECTION = 0;        // 0..15
bool MASK_DISTURBERS = false;
uint8_t MIN_LIGHTNING_EVENTS = 0;   // 0 = 1 event, 1 = 5 events, 2 = 9 events, 3 = 16 events

// TUNING: Most GY-AS3935 boards need their internal capacitor tuned to match 500kHz.
// Value ranges from 0 to 15 (multiples of 8pF). Default to 0 if unknown, but 
// if you get zero detections, you may need to tweak this (often 0x05 or 0x07).
uint8_t ANTENNA_TUNING = 0x00;

// -----------------------------
// Low-level I2C helpers
// -----------------------------
uint8_t rd(uint8_t reg) {
  Wire.beginTransmission(AS3935_ADDR);
  Wire.write(reg);
  uint8_t status = Wire.endTransmission(false);
  if (status != 0) {
    Serial.print("ERROR: I2C read failed at register 0x");
    Serial.print(reg, HEX);
    Serial.print("; Wire status ");
    Serial.println(status);
    return 0xFF;
  }
  
  Wire.requestFrom((uint8_t)AS3935_ADDR, (uint8_t)1);
  if (!Wire.available()) {
    Serial.print("ERROR: No I2C data at register 0x");
    Serial.println(reg, HEX);
    return 0xFF;
  }
  return Wire.read();
}

bool wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(AS3935_ADDR);
  Wire.write(reg);
  Wire.write(val);
  uint8_t status = Wire.endTransmission();
  if (status != 0) {
    Serial.print("ERROR: I2C write failed at register 0x");
    Serial.print(reg, HEX);
    Serial.print("; Wire status ");
    Serial.println(status);
    return false;
  }
  return true;
}

void irqISR() {
  irqFlag = true;
}

// --------------------------------
// Apply parameters with safe masks
// --------------------------------
bool applySettings() {
  // AFE Gain (Reg 0x00 bits [5:1])
  uint8_t r0 = rd(0x00);
  if (r0 == 0xFF) return false;
  r0 &= ~0b00111110; 
  r0 |= ((AFE_GAIN & 0x1F) << 1);
  if (!wr(0x00, r0)) return false;

  // Noise Floor (Reg 0x01 bits [6:4]) & Watchdog (bits [3:0])
  uint8_t r1 = rd(0x01);
  if (r1 == 0xFF) return false;
  r1 &= ~0b01111111; // Clear both fields
  r1 |= ((NOISE_FLOOR & 0x07) << 4);
  r1 |= (WATCHDOG_THRESHOLD & 0x0F);
  if (!wr(0x01, r1)) return false;

  // Spike rejection (Reg 0x02 bits [3:0]) & minimum lightning events (bits [5:4])
  uint8_t r2 = rd(0x02);
  if (r2 == 0xFF) return false;
  r2 &= ~0b00111111; // Preserve reserved bit 7 and CL_STAT bit 6
  r2 |= ((MIN_LIGHTNING_EVENTS & 0x03) << 4);
  r2 |= (SPIKE_REJECTION & 0x0F);
  if (!wr(0x02, r2)) return false;

  // Mask disturbers (Reg 0x03 bit 5)
  uint8_t r3 = rd(0x03);
  if (r3 == 0xFF) return false;
  if (MASK_DISTURBERS) r3 |=  (1 << 5);
  else                 r3 &= ~(1 << 5);
  if (!wr(0x03, r3)) return false;

  // Antenna Tuning (Reg 0x08 bits [3:0])
  uint8_t r8 = rd(0x08);
  if (r8 == 0xFF) return false;
  r8 &= ~0b00001111;
  r8 |= (ANTENNA_TUNING & 0x0F);
  return wr(0x08, r8);
}

bool as3935_init() {
  if (!wr(0x3C, 0x96)) return false; // Reset registers to default
  delay(10);      // Give it time to settle

  if (!wr(0x3D, 0x96)) return false; // Calibrate internal oscillators
  delay(10);

  return applySettings();
}

void printRegisterHex(uint8_t reg, uint8_t val) {
  Serial.print("Reg 0x");
  if (reg < 0x10) Serial.print("0");
  Serial.print(reg, HEX);
  Serial.print(" = 0x");
  if (val < 0x10) Serial.print("0");
  Serial.print(val, HEX);
}

void dumpSettings() {
  Serial.println("AS3935 register check:");

  uint8_t r0 = rd(0x00);
  printRegisterHex(0x00, r0);
  Serial.print(" - AFE gain field: 0x");
  Serial.print((r0 >> 1) & 0x1F, HEX);
  Serial.println(" (0x12 indoor, 0x0E outdoor)");

  uint8_t r1 = rd(0x01);
  printRegisterHex(0x01, r1);
  Serial.print(" - noise floor: ");
  Serial.print((r1 >> 4) & 0x07);
  Serial.print(", watchdog threshold: ");
  Serial.println(r1 & 0x0F);

  uint8_t r2 = rd(0x02);
  printRegisterHex(0x02, r2);
  Serial.print(" - min lightning events code: ");
  Serial.print((r2 >> 4) & 0x03);
  Serial.print(" (0=1, 1=5, 2=9, 3=16), spike rejection: ");
  Serial.println(r2 & 0x0F);

  uint8_t r3 = rd(0x03);
  printRegisterHex(0x03, r3);
  Serial.print(" - mask disturbers: ");
  Serial.print((r3 & (1 << 5)) ? "yes" : "no");
  Serial.print(", interrupt source: 0x");
  Serial.println(r3 & 0x0F, HEX);

  uint8_t r8 = rd(0x08);
  printRegisterHex(0x08, r8);
  Serial.print(" - antenna tuning capacitors: ");
  Serial.print(r8 & 0x0F);
  Serial.println(" steps (8pF each)");
}

void setAllLeds(uint32_t color) {
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, color);
  }
  strip.show();
}

void clearLeds() {
  setAllLeds(LED_COLOR_OFF);
}

float readBatteryVoltage() {
  unsigned long total = 0;
  for (uint8_t i = 0; i < 16; i++) {
    total += analogRead(BATTERY_PIN);
  }
  return (total / 16.0) * (ADC_REFERENCE / 4095.0) * DIVIDER_RATIO;
}

float estimateCharge(float voltage) {
  if (voltage <= CHARGE_VOLTAGES[0]) return 0.0;
  for (uint8_t i = 1; i < 11; i++) {
    if (voltage < CHARGE_VOLTAGES[i]) {
      float fraction = (voltage - CHARGE_VOLTAGES[i - 1]) /
                       (CHARGE_VOLTAGES[i] - CHARGE_VOLTAGES[i - 1]);
      return (i - 1 + fraction) * 10.0;
    }
  }
  return 100.0;
}

void showBatteryCharge(float chargePercent, bool blinkOn) {
  strip.clear();
  if (chargePercent <= 0.0) {
    if (blinkOn) strip.fill(strip.Color(255, 0, 0));
  } else {
    uint8_t lit = (uint8_t)ceilf(chargePercent * NUM_LEDS / 100.0);
    for (uint8_t i = 0; i < lit && i < NUM_LEDS; i++) {
      strip.setPixelColor(i, strip.Color(0, 255, 0));
    }
  }
  strip.show();
}

void printBatteryStatus(float voltage, float chargePercent) {
  Serial.print(F("Voltage: "));
  Serial.print(voltage, 3);
  Serial.print(F(" V | Charge (estimated): "));
  Serial.print(chargePercent, 1);
  Serial.println(F("%"));
}

void showStartupBattery() {
  pinMode(BATTERY_PIN, INPUT);
  analogReadResolution(12);
  analogReference(INTERNAL2V048);
  delay(10);
  analogRead(BATTERY_PIN); // Discard the first conversion after changing reference.
  strip.setBrightness(BATTERY_BRIGHTNESS);

  float voltage = readBatteryVoltage();
  float chargePercent = estimateCharge(voltage);
  bool blinkOn = true;
  showBatteryCharge(chargePercent, blinkOn);
  printBatteryStatus(voltage, chargePercent);
  unsigned long startedAt = millis();
  unsigned long lastSample = startedAt;
  unsigned long lastBlink = startedAt;
  while (millis() - startedAt < BATTERY_DISPLAY_MS) {
    unsigned long now = millis();
    if (now - lastSample >= BATTERY_SAMPLE_MS) {
      lastSample = now;
      voltage += 0.25 * (readBatteryVoltage() - voltage);
      float previousCharge = chargePercent;
      chargePercent = estimateCharge(voltage);
      if (chargePercent <= 0.0 && previousCharge > 0.0) {
        blinkOn = true;
        lastBlink = now;
      }
      showBatteryCharge(chargePercent, blinkOn);
      printBatteryStatus(voltage, chargePercent);
    }
    if (chargePercent <= 0.0 && now - lastBlink >= BATTERY_BLINK_MS) {
      lastBlink = now;
      blinkOn = !blinkOn;
      showBatteryCharge(chargePercent, blinkOn);
    }
  }
  clearLeds();
  strip.setBrightness(BRIGHTNESS);
}

void showStartupLeds() {
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    clearLeds();
    strip.setPixelColor(i, LED_COLOR_STARTUP);
    strip.show();
    delay(90);
  }

  for (int8_t i = NUM_LEDS - 2; i >= 0; i--) {
    clearLeds();
    strip.setPixelColor(i, LED_COLOR_STARTUP);
    strip.show();
    delay(90);
  }

  clearLeds();
}

uint8_t ledCountForDistance(uint8_t dist) {
  if (dist == 0x3F) return 1; // Out of range
  if (dist <= 6) return 7;    // Storm overhead / very close
  if (dist <= 10) return 6;
  if (dist <= 17) return 5;
  if (dist <= 24) return 4;
  if (dist <= 31) return 3;
  return 2;                   // 34..40 km
}

uint32_t blendLedColors(uint32_t from, uint32_t to, uint8_t amount) {
  uint32_t color = 0;
  for (uint8_t shift = 0; shift <= 16; shift += 8) {
    uint32_t fromChannel = (from >> shift) & 0xFF;
    uint32_t toChannel = (to >> shift) & 0xFF;
    color |= ((fromChannel * (255 - amount) + toChannel * amount) / 255) << shift;
  }
  return color;
}

uint32_t scaleLedColor(uint32_t color, uint8_t brightness) {
  uint32_t scaled = 0;
  for (uint8_t shift = 0; shift <= 16; shift += 8) {
    uint32_t channel = (color >> shift) & 0xFF;
    scaled |= ((channel * (brightness + 1)) >> 8) << shift;
  }
  return scaled;
}

void updateLedIndication() {
  unsigned long now = millis();
  bool changed = false;
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    LedFrame &led = ledFrames[i];
    unsigned long elapsed = now - led.startedAt;
    uint32_t color = LED_COLOR_OFF;
    switch (led.indication) {
      case LED_FLASH:
        if (elapsed < flashOnMs) color = flashColor;
        else if (elapsed >= (unsigned long)flashOnMs + flashOffMs) led.indication = LED_IDLE;
        break;

      case LED_LIGHTNING: {
        unsigned long frame = elapsed / LIGHTNING_FRAME_MS;
        if (frame >= 2 * LIGHTNING_PHASE_FRAMES) {
          led.indication = LED_IDLE;
          break;
        }
        uint8_t value = 255 - (frame % LIGHTNING_PHASE_FRAMES) * 15;
        color = frame < LIGHTNING_PHASE_FRAMES
          ? blendLedColors(LED_COLOR_LIGHTNING_TRANSITION, LED_COLOR_LIGHTNING_START, value)
          : blendLedColors(LED_COLOR_OFF, LED_COLOR_LIGHTNING_TRANSITION, value);
        break;
      }

      case LED_IDLE:
        break;
    }
    color = scaleLedColor(color, led.brightness);
    if (color != led.color) {
      led.color = color;
      strip.setPixelColor(i, color);
      changed = true;
    }
  }
  if (changed) strip.show();
}

bool ledsAreActive() {
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    if (ledFrames[i].indication != LED_IDLE) return true;
  }
  return false;
}

void showLightningLeds(uint8_t dist) {
  unsigned long now = millis();
  uint8_t count = ledCountForDistance(dist);
  // Only replace the selected pixels; other pixels retain their own timelines.
  for (uint8_t i = 0; i < count; i++) {
    ledFrames[i].indication = LED_LIGHTNING;
    ledFrames[i].startedAt = now;
    ledFrames[i].brightness = 100;
  }
  updateLedIndication();
}

void flashLeds(uint32_t color, unsigned int onMs, unsigned int offMs, uint8_t brightness) {
  flashColor = color;
  flashOnMs = onMs;
  flashOffMs = offMs;
  unsigned long now = millis();
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    ledFrames[i].indication = LED_FLASH;
    ledFrames[i].startedAt = now;
    ledFrames[i].brightness = brightness;
  }
  updateLedIndication();
}

void showNoiseLeds() {
  flashLeds(LED_COLOR_NOISE, 120, 120, BRIGHTNESS);
}

void showDisturberLeds() {
  flashLeds(LED_COLOR_DISTURBER, 120, 120, 10);
}

void disableAlwaysUnusedPeripherals() {
  power_adc_disable();
  power_spi_disable();
  power_timer1_disable();
  power_timer2_disable();
#if defined(__LGT8FX8P__)
  PRR1 |= _BV(PRTC3); // LGT Timer3 is started by the core but unused here.
#endif
}

void disableSleepOnlyPeripherals() {
  power_timer0_disable();
  power_twi_disable();
  power_usart0_disable();
}

void enableSleepOnlyPeripherals() {
  power_timer0_enable(); // Required by delay() and NeoPixel latch timing.
  power_usart0_enable();
  power_twi_enable();
}

void enterLowPowerMode() {
  Serial.flush();
  set_sleep_mode(SLEEP_MODE_IDLE);

  noInterrupts();
  if (!irqFlag && !ledsAreActive()) {
    // Mask pending Timer0 interrupts as well as stopping its clock.
    uint8_t timer0InterruptMask = TIMSK0;
    TIMSK0 = 0;
    sleep_enable();
    disableSleepOnlyPeripherals();
    interrupts();
    sleep_cpu();
    sleep_disable();
    enableSleepOnlyPeripherals();
    TIMSK0 = timer0InterruptMask;
  } else {
    interrupts();
  }
}

void setup() {
  Serial.begin(9600);
  Wire.begin();
  // Bound stalled I2C transactions while Timer0 is running.
  Wire.setWireTimeout(I2C_TIMEOUT_US, true);
  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  clearLeds();
  showStartupBattery();
  showStartupLeds();
  // Event brightness is applied per pixel so overlapping events remain independent.
  strip.setBrightness(255);
  
  // Set I2C clock to 100kHz (AS3935 maximum stable speed for I2C)
  Wire.setClock(100000); 

  pinMode(IRQ_PIN, INPUT);

  Serial.println("AS3935 startup...");
  Serial.print(F("I2C idle levels: SDA="));
  Serial.print(digitalRead(SDA));
  Serial.print(F(", SCL="));
  Serial.println(digitalRead(SCL));
  Serial.println(F("Both should be 1 before an I2C transaction."));
  bool sensorReady = as3935_init();
  disableAlwaysUnusedPeripherals();
  
  // Verify communication by reading back register 0x00
  uint8_t testRead = sensorReady ? rd(0x00) : 0xFF;
  if (!sensorReady || testRead == 0xFF || testRead == 0x00) {
    Serial.println("ERROR: Cannot communicate with AS3935! Check wiring/I2C address.");
    Serial.println(F("Sensor unavailable; sleeping. Reset after fixing I2C."));
  } else {
    Serial.println("AS3935 Ready and Listening!");
    dumpSettings();
    attachInterrupt(digitalPinToInterrupt(IRQ_PIN), irqISR, RISING);
    // Handle an IRQ already asserted before the interrupt was attached.
    if (digitalRead(IRQ_PIN)) irqFlag = true;
    Serial.println("Entering low power mode; waiting for AS3935 IRQ...");
  }
}

void handleSensorInterrupt() {
  // Crucial: Wait 2ms after IRQ goes high before reading I2C
  delay(2); 
  
  // Read interrupt source (Reg 0x03, bits [3:0])
  // Clear before reading so an IRQ arriving during I2C is not lost.
  irqFlag = false;
  uint8_t irqRegister = rd(0x03);
  if (irqRegister == 0xFF) return;
  uint8_t src = irqRegister & 0x0F;

  switch (src) {
    case 0x01:
      Serial.println("[IRQ] Noise level too high.");
      showNoiseLeds();
      break;

    case 0x04:
      Serial.println("[IRQ] Disturber detected (false alarm rejected).");
      showDisturberLeds();
      break;

    case 0x08: {
      uint8_t distanceRegister = rd(0x07);
      if (distanceRegister == 0xFF) break;
      uint8_t dist = distanceRegister & 0x3F;
      Serial.print("⚡ LIGHTNING DETECTED! Approximate Distance: ");
      if (dist == 0x3F) {
        Serial.println("Out of range (>40km)");
      } else if (dist == 0x01) {
        Serial.println("Storm overhead");
      } else {
        Serial.print(dist);
        Serial.println(" km");
      }
      showLightningLeds(dist);
      break;
    }

    default:
      // If it returns 0, it means it was a transient pulse that didn't meet criteria
      if (src != 0) {
        Serial.print("Unknown interrupt code: 0x");
        Serial.println(src, HEX);
      }
      break;
  }
}

void loop() {
  if (irqFlag) handleSensorInterrupt();
  // A newer IRQ takes priority over advancing the previous animation.
  if (irqFlag) return;
  updateLedIndication();
  if (!ledsAreActive()) enterLowPowerMode();
}
