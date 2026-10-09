#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <avr/sleep.h>
#include <avr/power.h>

// Change this to match your I2C scanner result (usually 0x01, 0x02, or 0x03)
#define AS3935_ADDR 0x03 
#define IRQ_PIN      2

#define LED_PIN    6     // Nano D6 -> (330 ohm) -> strip DIN
#define NUM_LEDS   7     // set to the real number of LEDs on your strip
#define BRIGHTNESS 50

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

volatile bool irqFlag = false;

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
  if (Wire.endTransmission(false) != 0) return 0xFF; // Connection error
  
  Wire.requestFrom(AS3935_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0xFF;
}

void wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(AS3935_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

void irqISR() {
  irqFlag = true;
}

// --------------------------------
// Apply parameters with safe masks
// --------------------------------
void applySettings() {
  // AFE Gain (Reg 0x00 bits [5:1])
  uint8_t r0 = rd(0x00);
  r0 &= ~0b00111110; 
  r0 |= ((AFE_GAIN & 0x1F) << 1);
  wr(0x00, r0);

  // Noise Floor (Reg 0x01 bits [6:4]) & Watchdog (bits [3:0])
  uint8_t r1 = rd(0x01);
  r1 &= ~0b01111111; // Clear both fields
  r1 |= ((NOISE_FLOOR & 0x07) << 4);
  r1 |= (WATCHDOG_THRESHOLD & 0x0F);
  wr(0x01, r1);

  // Spike rejection (Reg 0x02 bits [3:0]) & minimum lightning events (bits [5:4])
  uint8_t r2 = rd(0x02);
  r2 &= ~0b00111111; // Preserve reserved bit 7 and CL_STAT bit 6
  r2 |= ((MIN_LIGHTNING_EVENTS & 0x03) << 4);
  r2 |= (SPIKE_REJECTION & 0x0F);
  wr(0x02, r2);

  // Mask disturbers (Reg 0x03 bit 5)
  uint8_t r3 = rd(0x03);
  if (MASK_DISTURBERS) r3 |=  (1 << 5);
  else                 r3 &= ~(1 << 5);
  wr(0x03, r3);

  // Antenna Tuning (Reg 0x08 bits [3:0])
  uint8_t r8 = rd(0x08);
  r8 &= ~0b00001111;
  r8 |= (ANTENNA_TUNING & 0x0F);
  wr(0x08, r8);
}

void as3935_init() {
  wr(0x3C, 0x96); // Reset registers to default
  delay(10);      // Give it time to settle

  wr(0x3D, 0x96); // Calibrate internal oscillators
  delay(10);

  applySettings();
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

void printTimestamp() {
  unsigned long ms = millis();
  unsigned long seconds = ms / 1000;
  unsigned int days = seconds / 86400;
  byte hours = (seconds / 3600) % 24;
  byte minutes = (seconds / 60) % 60;
  byte secs = seconds % 60;
  unsigned int millisPart = ms % 1000;

  Serial.print("[uptime ");
  Serial.print(days);
  Serial.print("d ");
  if (hours < 10) Serial.print("0");
  Serial.print(hours);
  Serial.print(":");
  if (minutes < 10) Serial.print("0");
  Serial.print(minutes);
  Serial.print(":");
  if (secs < 10) Serial.print("0");
  Serial.print(secs);
  Serial.print(".");
  if (millisPart < 100) Serial.print("0");
  if (millisPart < 10) Serial.print("0");
  Serial.print(millisPart);
  Serial.print("] ");
}

void setAllLeds(uint32_t color) {
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, color);
  }
  strip.show();
}

void clearLeds() {
  setAllLeds(strip.Color(0, 0, 0));
}

void showStartupLeds() {
  uint32_t color = strip.Color(0, 80, 255);

  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    clearLeds();
    strip.setPixelColor(i, color);
    strip.show();
    delay(90);
  }

  for (int8_t i = NUM_LEDS - 2; i >= 0; i--) {
    clearLeds();
    strip.setPixelColor(i, color);
    strip.show();
    delay(90);
  }

  clearLeds();
}

void flashLeds(uint32_t color, uint8_t flashes, unsigned int onMs, unsigned int offMs) {
  for (uint8_t i = 0; i < flashes; i++) {
    setAllLeds(color);
    delay(onMs);
    clearLeds();
    delay(offMs);
  }
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

void showLightningLeds(uint8_t dist) {
  uint8_t count = ledCountForDistance(dist);
  uint32_t color = strip.Color(255, 0, 0);

  strip.setBrightness(100);
  for (int8_t remaining = count; remaining >= 0; remaining--) {
    clearLeds();
    for (uint8_t i = 0; i < remaining; i++) {
      strip.setPixelColor(i, color);
    }
    strip.show();
    delay(350);
  }

  strip.setBrightness(BRIGHTNESS);
  clearLeds();
}

void showNoiseLeds() {
  flashLeds(strip.Color(255, 0, 120), 2, 120, 120);
}

void showDisturberLeds() {
  strip.setBrightness(10);
  flashLeds(strip.Color(0, 0, 255), 2, 120, 120);
  strip.setBrightness(BRIGHTNESS);
  clearLeds();
}

void disableAlwaysUnusedPeripherals() {
  power_adc_disable();
  power_spi_disable();
  power_timer1_disable();
  power_timer2_disable();
}

void disableSleepOnlyPeripherals() {
  power_twi_disable();
  power_usart0_disable();
}

void enableSleepOnlyPeripherals() {
  power_usart0_enable();
  power_twi_enable();
}

void enterLowPowerMode() {
  clearLeds();
  Serial.flush();
  set_sleep_mode(SLEEP_MODE_IDLE);

  noInterrupts();
  if (!irqFlag) {
    sleep_enable();
    disableSleepOnlyPeripherals();
    interrupts();
    sleep_cpu();
    sleep_disable();
    enableSleepOnlyPeripherals();
  } else {
    interrupts();
  }
}

void setup() {
  Serial.begin(9600);
  Wire.begin();
  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  clearLeds();
  showStartupLeds();
  
  // Set I2C clock to 100kHz (AS3935 maximum stable speed for I2C)
  Wire.setClock(100000); 

  pinMode(IRQ_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(IRQ_PIN), irqISR, RISING);

  Serial.println("AS3935 startup...");
  as3935_init();
  disableAlwaysUnusedPeripherals();
  
  // Verify communication by reading back register 0x00
  uint8_t testRead = rd(0x00);
  if (testRead == 0xFF || testRead == 0x00) {
    Serial.println("ERROR: Cannot communicate with AS3935! Check wiring/I2C address.");
  } else {
    Serial.println("AS3935 Ready and Listening!");
    dumpSettings();
  }

  Serial.println("Entering low power mode; waiting for AS3935 IRQ...");
}

void loop() {
  if (!irqFlag) {
    enterLowPowerMode();
  }

  if (!irqFlag) {
    return;
  }

  // Crucial: Wait 2ms after IRQ goes high before reading I2C
  delay(2); 
  
  // Read interrupt source (Reg 0x03, bits [3:0])
  uint8_t src = rd(0x03) & 0x0F;
  
  // Reset flag *after* reading register 0x03, which clears the hardware interrupt
  irqFlag = false; 

  switch (src) {
    case 0x01:
      printTimestamp();
      Serial.println("[IRQ] Noise level too high.");
      showNoiseLeds();
      break;

    case 0x04:
      printTimestamp();
      Serial.println("[IRQ] Disturber detected (false alarm rejected).");
      showDisturberLeds();
      break;

    case 0x08: {
      uint8_t dist = rd(0x07) & 0x3F;
      printTimestamp();
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
        printTimestamp();
        Serial.print("Unknown interrupt code: 0x");
        Serial.println(src, HEX);
      }
      break;
  }
}
