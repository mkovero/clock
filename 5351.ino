// -----------------------------------------------------------------------------
// Si5351C-B (20-QFN) driven from an external 10 MHz reference
//
//   CLKIN  = 10.000 MHz  (AccuBeat AR-40A rubidium, AC coupled + VDD/2 bias)
//   CLK4   = 54.000 MHz  (Raspberry Pi CM4 / BCM2711 reference)
//
//   PLLA = 10 MHz x 86.4      = 864.000 MHz   VCO, inside 600-900 MHz
//   MS4  = 864 MHz / 16       =  54.000 MHz   integer mode
//   R4   = 1
//
//   86.4 = 86 + 2/5  -> fractional feedback, denominator 5, spurs at 2 MHz offset
//
// I2C address 0x60. The C variant has no A0 pin, so the address is fixed.
// Teensy 3.2: SDA = pin 18, SCL = pin 19, native 3.3 V logic (default I2C0
//             routing, as used by the original Etherkit-based firmware).
//             Pins 16/17 are the alternate routing of the SAME peripheral;
//             if you ever move to them, call setSCL/setSDA before begin().
//
// CLK4 drive strength is 8 mA (0x4F), matching the Etherkit library default
// that this board has been running with. Use 0x4C for 2 mA if you ever want
// to reduce the swing at the BCM2711 crystal pin.
// Pull-ups: 1k - 2.2k to 3.3 V (check whether the breakout already fits them).
//
// Register usage follows AN619 (10-MSOP / 20-QFN devices).
// -----------------------------------------------------------------------------

#include <Wire.h>

static const uint8_t SI_ADDR = 0x60;

// ---- frequency plan ---------------------------------------------------------
// PLLA feedback multiplier: a + b/c
static const uint32_t PLLA_A = 86;
static const uint32_t PLLA_B = 2;
static const uint32_t PLLA_C = 5;

// MultiSynth 4 divider: a + b/c  (b = 0 -> integer mode)
static const uint32_t MS4_A = 16;
static const uint32_t MS4_B = 0;
static const uint32_t MS4_C = 1;

// CLK4 control byte, register 20:
//   bit 7   CLK4_PDN   = 0   powered up
//   bit 6   MS4_INT    = 1   integer mode (MS4_B == 0)
//   bit 5   MS4_SRC    = 0   PLLA
//   bit 4   CLK4_INV   = 0
//   bit 3:2 CLK4_SRC   = 11  MultiSynth 4
//   bit 1:0 CLK4_IDRV  = 11  8 mA  (use 0x4C for 2 mA)
static const uint8_t CLK4_CTRL = 0x4F;

// ---- low level --------------------------------------------------------------

static bool siWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(SI_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool siWriteBurst(uint8_t reg, const uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(SI_ADDR);
  Wire.write(reg);
  for (uint8_t i = 0; i < n; i++) Wire.write(buf[i]);
  return Wire.endTransmission() == 0;
}

static uint8_t siRead(uint8_t reg) {
  Wire.beginTransmission(SI_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return 0xFF;
  Wire.requestFrom((int)SI_ADDR, 1);
  return Wire.available() ? Wire.read() : 0xFF;
}

// ---- parameter encoding (AN619 section 3.2) ---------------------------------
// P1 = 128*a + floor(128*b/c) - 512
// P2 = 128*b - c*floor(128*b/c)
// P3 = c

static void encodeParams(uint32_t a, uint32_t b, uint32_t c,
                         uint32_t &p1, uint32_t &p2, uint32_t &p3) {
  uint32_t f = (128UL * b) / c;
  p1 = 128UL * a + f - 512UL;
  p2 = 128UL * b - c * f;
  p3 = c;
}

// PLLA feedback MultiSynth, registers 26..33
static void setPllA(uint32_t a, uint32_t b, uint32_t c) {
  uint32_t p1, p2, p3;
  encodeParams(a, b, c, p1, p2, p3);

  uint8_t r[8];
  r[0] = (p3 >> 8) & 0xFF;
  r[1] =  p3       & 0xFF;
  r[2] = (p1 >> 16) & 0x03;
  r[3] = (p1 >> 8) & 0xFF;
  r[4] =  p1       & 0xFF;
  r[5] = ((p3 >> 12) & 0xF0) | ((p2 >> 16) & 0x0F);
  r[6] = (p2 >> 8) & 0xFF;
  r[7] =  p2       & 0xFF;

  siWriteBurst(26, r, 8);
}

// Output MultiSynth 0..5, registers 42 + 8*index
// rdiv: 0 = /1, 1 = /2, 2 = /4 ... 7 = /128
static void setMultiSynth(uint8_t index, uint32_t a, uint32_t b, uint32_t c,
                          uint8_t rdiv) {
  uint32_t p1, p2, p3;
  encodeParams(a, b, c, p1, p2, p3);

  uint8_t r[8];
  r[0] = (p3 >> 8) & 0xFF;
  r[1] =  p3       & 0xFF;
  r[2] = ((rdiv & 0x07) << 4) | ((p1 >> 16) & 0x03);
  r[3] = (p1 >> 8) & 0xFF;
  r[4] =  p1       & 0xFF;
  r[5] = ((p3 >> 12) & 0xF0) | ((p2 >> 16) & 0x0F);
  r[6] = (p2 >> 8) & 0xFF;
  r[7] =  p2       & 0xFF;

  siWriteBurst(42 + 8 * index, r, 8);
}

// ---- init -------------------------------------------------------------------

static bool si5351Init() {
  // Wait for the device to finish its own power-up initialisation.
  uint32_t t0 = millis();
  while (siRead(0) & 0x80) {
    if (millis() - t0 > 100) return false;   // no device / stuck on SYS_INIT
  }

  // 1. Disable all outputs.
  siWrite(3, 0xFF);

  // 2. Power down all output drivers.
  for (uint8_t r = 16; r <= 23; r++) siWrite(r, 0x80);

  // 3. Ignore the OEB pin for every output. Without this a floating OEB
  //    can hold the outputs disabled - the pin has no internal pull-up.
  siWrite(9, 0xFF);

  // 4. Input source: CLKIN divided by 1 (10 MHz is under the 30 MHz PLL
  //    input limit), both PLLs referenced to CLKIN rather than the crystal.
  //      bit 7:6 CLKIN_DIV = 00  (/1)
  //      bit 3   PLLB_SRC  = 1   (CLKIN)
  //      bit 2   PLLA_SRC  = 1   (CLKIN)
  siWrite(15, 0x0C);

  // 5. Crystal load capacitance. Harmless if no crystal is fitted; the low
  //    six bits are reserved and must be written as 010010b.
  siWrite(183, 0xD2);

  // 6. Interrupt masks: watch LOS_CLKIN and LOL_A, ignore LOL_B (PLLB unused)
  //    and LOS_XTAL (no crystal in this frequency plan).
  siWrite(2, 0x48);

  // 7. Frequency plan.
  setPllA(PLLA_A, PLLA_B, PLLA_C);
  setMultiSynth(4, MS4_A, MS4_B, MS4_C, 0);

  // 8. Zero phase offset on CLK4 (register 165 + n).
  siWrite(169, 0x00);

  // 9. Bring up the CLK4 driver.
  siWrite(20, CLK4_CTRL);

  // 10. Soft reset PLLA only. Resetting both (0xAC) would glitch every
  //     output on PLLB too - relevant once you add more clocks.
  siWrite(177, 0x20);

  delay(2);

  // 11. Enable CLK4, leave the rest disabled (0 = enabled, bit n = CLKn).
  siWrite(3, 0xEF);

  // Clear sticky status bits so the first read reflects the present state.
  siWrite(1, 0x00);

  return true;
}

// ---- status -----------------------------------------------------------------

static void printStatus() {
  uint8_t s = siRead(0);
  uint8_t k = siRead(1);

  Serial.print("reg0=0x");
  Serial.print(s, HEX);
  Serial.print("  SYS_INIT=");
  Serial.print((s >> 7) & 1);
  Serial.print("  LOL_A=");
  Serial.print((s >> 5) & 1);
  Serial.print("  LOS_CLKIN=");
  Serial.print((s >> 4) & 1);
  Serial.print("  LOS_XTAL=");
  Serial.print((s >> 3) & 1);
  Serial.print("   sticky=0x");
  Serial.println(k, HEX);

  if ((s >> 4) & 1) Serial.println("  !! no signal on CLKIN");
  if ((s >> 5) & 1) Serial.println("  !! PLLA not locked");
}

// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { }

  // I2C0 on the Teensy 3.2 default pins: 18 = SDA, 19 = SCL.
  // This matches the original firmware, which called Wire.begin() via the
  // Etherkit library with no pin remapping.
  //
  // If the bus is ever moved to the alternate pads (16 = SCL, 17 = SDA),
  // uncomment the two lines below - they must come before begin(), and only
  // one pinset can be active since both route the same peripheral.
  //
  // Wire.setSCL(16);
  // Wire.setSDA(17);

  Wire.begin();
  Wire.setClock(400000);

  delay(10);

  if (!si5351Init()) {
    Serial.println("Si5351 not responding at 0x60");
    return;
  }

  Serial.println("Si5351C: CLKIN 10 MHz -> PLLA 864 MHz -> CLK4 54 MHz");
  printStatus();
}

void loop() {
  printStatus();
  delay(2000);
}
