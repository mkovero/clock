// -----------------------------------------------------------------------------
// Si5351C-B (20-QFN) driven from an external 10 MHz reference
//
//   CLKIN  = 10.000 MHz  (AccuBeat AR-40A rubidium)
//   CLK4   = 54.000 MHz  (Raspberry Pi CM4 / BCM2711 reference)
//
//   PLLA = 10 MHz x 86.4      = 864.000 MHz   VCO, inside 600-900 MHz
//   MS4  = 864 MHz / 16       =  54.000 MHz   integer mode
//   R4   = 1
//
//   86.4 = 86 + 2/5  -> fractional feedback, denominator 5, spurs at 2 MHz offset
//
// I2C address 0x60. The C variant has no A0 pin, so the address is fixed.
// Teensy 4.0: SDA = pin 18, SCL = pin 19 (Wire default routing), 3.3 V logic.
//
// !! Teensy 4.0 pins are NOT 5 V tolerant. 3.3 V is the absolute maximum on
// !! any input. The AR-40A's BIT pin is open collector and only ever sinks, so
// !! it is safe on its own - but ONLY if nothing else is on that line. The old
// !! interlock had a relay coil between +V and BIT; if any of that wiring
// !! remains, an open BIT gets pulled up through the coil to the relay supply
// !! and will destroy the input. Verify the coil is out of circuit.
//
// CLK4 drive strength is 8 mA (0x4F), matching the Etherkit library default
// that this board has been running with. Use 0x4C for 2 mA if you ever want
// to reduce the swing at the BCM2711 crystal pin.
//
// Register usage follows AN619 (10-MSOP / 20-QFN devices).
//
// -----------------------------------------------------------------------------
// CLK4 OUTPUT GATING
//
// Measured behaviour with no CLKIN present: the PLL loop opens, the charge pump
// rails, the VCO parks around 186 MHz and MS4's /16 emits a stable ~11.6 MHz.
// The CM4 is then presented with a plausible-looking clock at a fifth of
// nominal; its internal PLLs never lock and it sits with power and green LEDs
// on, doing nothing. That is a bad failure mode to diagnose cold.
//
// So CLK4 is held disabled unless the Si5351's own status says the reference
// is good:
//
//   reg 0 bit 4  LOS_CLKIN   no signal on CLKIN
//   reg 0 bit 5  LOL_A       PLLA not locked
//
// THREE states, not two. An I2C read failure is NOT a reference failure and
// must not change the output state:
//
//   comm failed        -> hold whatever CLK4 was doing, count the error
//   reference bad      -> disable after BAD_READS_TO_DISABLE consecutive polls
//   reference good     -> enable after GOOD_READS_TO_ENABLE consecutive polls
//
// The first version of this sketch returned 0xFF from a failed read, which is
// indistinguishable from a register with every fault bit set. Single bus
// glitches therefore killed the CM4's clock for a second at a time. The tell
// was the sticky register: reg 1 read 0xC0 throughout, i.e. LOL_A_STKY and
// LOS_CLKIN_STKY never latched, proving the reference had not actually
// dropped.
//
// The AR-40A's BIT (lock) pin arrives on LOCK_BIT_PIN and is used for
// INDICATION ONLY, not for gating. Gating on BIT would reinstate the old
// relay's five-minute delayed boot, because during warm-up CLKIN is present
// (free-running OCXO) while BIT still reads unlocked.
//
// BIT is open collector, active low = locked. Confirmed empirically: the
// original interlock energised a relay coil from this pin and the rig powered
// up after lock. (The AR-40A manual contradicts itself on this: section 2.1.2
// is right, section 3.3 has its labels swapped.)
// -----------------------------------------------------------------------------

#include <Wire.h>

static const uint8_t SI_ADDR = 0x60;

// ---- gating configuration ---------------------------------------------------

static const uint8_t  LOCK_BIT_PIN         = 2;    // AR-40A BIT, active low
static const uint8_t  LED_REF_PIN          = 3;    // reference present / usable
static const uint8_t  LED_LOCK_PIN         = 4;    // AR-40A locked
static const uint8_t  STATUS_LED_PIN       = 13;   // Teensy onboard, heartbeat

// Teensy 4.0 pins want far less current than a 3.x. Use a high-efficiency LED
// with ~1k in series (about 1.5 mA at 3.3 V), anode to the pin, cathode to GND.
// Do not reuse 220R values from AVR-era designs.
static const uint32_t POLL_INTERVAL_MS     = 250;
static const uint8_t  GOOD_READS_TO_ENABLE = 4;    // ~1 s of clean status
static const uint8_t  BAD_READS_TO_DISABLE = 2;    // ignore single-poll blips

// Set false to make a genuine loss of CLKIN report only, leaving CLK4 running
// at whatever the free-running VCO produces. Default true: a stopped CM4 is
// more diagnosable than one running at the wrong frequency.
static const bool     DISABLE_ON_LOSS      = true;

// ---- frequency plan ---------------------------------------------------------

static const uint32_t PLLA_A = 86;
static const uint32_t PLLA_B = 2;
static const uint32_t PLLA_C = 5;

static const uint32_t MS4_A = 16;
static const uint32_t MS4_B = 0;
static const uint32_t MS4_C = 1;

// CLK4 control byte, register 20:
//   bit 7    CLK4_PDN  = 0   powered up
//   bit 6    MS4_INT   = 1   integer mode (MS4_B == 0)
//   bit 5    MS4_SRC   = 0   PLLA
//   bit 4    CLK4_INV  = 0
//   bit 3:2  CLK4_SRC  = 11  MultiSynth 4
//   bit 1:0  CLK4_IDRV = 11  8 mA (use 0x4C for 2 mA)
static const uint8_t CLK4_CTRL = 0x4F;

// Register 3, output enable. 0 = enabled, bit n = CLKn.
static const uint8_t OE_ALL_OFF   = 0xFF;
static const uint8_t OE_CLK4_ONLY = 0xEF;

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

// Returns false if the transaction itself failed, separately from the data.
// One retry, because a NACK is usually transient.
static bool siReadOk(uint8_t reg, uint8_t &val) {
  for (uint8_t attempt = 0; attempt < 2; attempt++) {
    Wire.beginTransmission(SI_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission() != 0) continue;
    Wire.requestFrom((int)SI_ADDR, 1);
    if (Wire.available()) { val = Wire.read(); return true; }
  }
  return false;
}

// Init-time convenience wrapper; failure is handled by surrounding timeouts.
static uint8_t siRead(uint8_t reg) {
  uint8_t v;
  return siReadOk(reg, v) ? v : 0xFF;
}

// ---- parameter encoding (AN619 section 3.2) ---------------------------------
//   P1 = 128*a + floor(128*b/c) - 512
//   P2 = 128*b - c*floor(128*b/c)
//   P3 = c

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
  r[3] = (p1 >> 8)  & 0xFF;
  r[4] =  p1        & 0xFF;
  r[5] = ((p3 >> 12) & 0xF0) | ((p2 >> 16) & 0x0F);
  r[6] = (p2 >> 8) & 0xFF;
  r[7] =  p2       & 0xFF;
  siWriteBurst(26, r, 8);
}

// Output MultiSynth 0..5, registers 42 + 8*index
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

  siWrite(3, OE_ALL_OFF);                    // 1. all outputs off
  for (uint8_t r = 16; r <= 23; r++) siWrite(r, 0x80);   // 2. drivers down

  // 3. Ignore the OEB pin for every output. Without this a floating OEB can
  //    hold the outputs disabled - the pin has no internal pull-up.
  siWrite(9, 0xFF);

  // 4. Input source: CLKIN divided by 1 (10 MHz is under the 30 MHz PLL input
  //    limit), both PLLs referenced to CLKIN rather than the crystal.
  siWrite(15, 0x0C);

  // 5. Crystal load capacitance. Harmless with no crystal fitted; the low six
  //    bits are reserved and must be written as 010010b.
  siWrite(183, 0xD2);

  // 6. Interrupt masks: watch LOS_CLKIN and LOL_A, ignore LOL_B (PLLB unused)
  //    and LOS_XTAL (no crystal in this frequency plan).
  siWrite(2, 0x48);

  // 7. Frequency plan.
  setPllA(PLLA_A, PLLA_B, PLLA_C);
  setMultiSynth(4, MS4_A, MS4_B, MS4_C, 0);

  siWrite(169, 0x00);        // 8. zero phase offset on CLK4 (reg 165 + n)
  siWrite(20, CLK4_CTRL);    // 9. CLK4 driver up, still gated off at reg 3
  siWrite(177, 0x20);        // 10. soft reset PLLA only
  delay(2);

  // 11. Outputs stay DISABLED here. The gating loop enables CLK4 once status
  //     has been clean for GOOD_READS_TO_ENABLE polls.

  siWrite(1, 0x00);          // clear sticky bits
  return true;
}

// ---- status -----------------------------------------------------------------
//
// Deliberately no struct. The Arduino .ino preprocessor inserts generated
// function prototypes above user type definitions, so a function returning a
// locally-declared struct fails with "does not name a type".

static uint8_t  g_reg0     = 0xFF;
static uint8_t  g_sticky   = 0x00;
static bool     g_sysInit  = true;
static bool     g_lolA     = true;
static bool     g_losClkin = true;
static bool     g_refGood  = false;
static bool     g_commOk   = false;
static uint32_t g_commErrs = 0;

static void readStatus() {
  uint8_t r0, r1;
  g_commOk = siReadOk(0, r0) && siReadOk(1, r1);
  if (!g_commOk) {
    g_commErrs++;
    return;                  // leave previous flags alone; caller holds state
  }
  g_reg0     = r0;
  g_sticky   = r1;
  g_sysInit  = (g_reg0 >> 7) & 1;
  g_lolA     = (g_reg0 >> 5) & 1;
  g_losClkin = (g_reg0 >> 4) & 1;
  g_refGood  = !g_sysInit && !g_lolA && !g_losClkin;
  siWrite(1, 0x00);          // clear sticky so the next read is fresh
}

static bool rubidiumLocked() {
  // BIT is open collector, pulled low when locked.
  return digitalRead(LOCK_BIT_PIN) == LOW;
}

// ---- gating state -----------------------------------------------------------

static bool    clk4Enabled  = false;
static uint8_t goodRuns     = 0;
static uint8_t badRuns      = 0;
static bool    lastRbLocked = false;
static bool    firstReport  = true;

static void setClk4(bool on) {
  if (on == clk4Enabled) return;
  if (on) {
    // Re-lock cleanly before letting the output out: a PLL that has just
    // reacquired its reference benefits from a soft reset.
    siWrite(177, 0x20);
    delay(2);
    siWrite(3, OE_CLK4_ONLY);
    Serial.print("CLK4: ENABLED  (54 MHz to CM4)  reg0=0x");
    Serial.println(g_reg0, HEX);
  } else {
    siWrite(3, OE_ALL_OFF);
    Serial.print("CLK4: DISABLED (reference lost - CM4 has no clock)  reg0=0x");
    Serial.print(g_reg0, HEX);
    Serial.print(" sticky=0x");
    Serial.println(g_sticky, HEX);
  }
  clk4Enabled = on;
}

static void report(bool rbLocked) {
  Serial.print("reg0=0x");     Serial.print(g_reg0, HEX);
  Serial.print(" LOL_A=");     Serial.print(g_lolA);
  Serial.print(" LOS_CLKIN="); Serial.print(g_losClkin);
  Serial.print(" sticky=0x");  Serial.print(g_sticky, HEX);
  Serial.print(" Rb=");        Serial.print(rbLocked ? "LOCKED" : "UNLOCKED");
  Serial.print(" CLK4=");      Serial.print(clk4Enabled ? "on" : "off");
  Serial.print(" i2cErr=");    Serial.println(g_commErrs);

  if (!g_commOk)  Serial.println("  !! I2C read failed (output state held)");
  if (g_losClkin) Serial.println("  !! no signal on CLKIN");
  if (g_lolA)     Serial.println("  !! PLLA not locked");
  if (!rbLocked)  Serial.println("  !! AR-40A unlocked or in holdover");
}

// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { }

  pinMode(LOCK_BIT_PIN, INPUT_PULLUP);
  pinMode(LED_REF_PIN, OUTPUT);
  pinMode(LED_LOCK_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(LED_REF_PIN, LOW);
  digitalWrite(LED_LOCK_PIN, LOW);
  digitalWrite(STATUS_LED_PIN, LOW);

  // Wire on the Teensy 4.0 default pins: 18 = SDA, 19 = SCL.
  Wire.begin();
  Wire.setClock(400000);
  delay(10);

  if (!si5351Init()) {
    Serial.println("Si5351 not responding at 0x60");
    return;
  }

  Serial.println("Si5351C: CLKIN 10 MHz -> PLLA 864 MHz -> CLK4 54 MHz");
  Serial.println("CLK4 gated on LOS_CLKIN / LOL_A. BIT on pin 2 = indication only.");
}

void loop() {
  readStatus();
  bool locked = rubidiumLocked();

  if (!g_commOk) {
    // Bus failure tells us nothing about the reference. Hold the output where
    // it is and do not touch the run counters.
  } else if (g_refGood) {
    badRuns = 0;
    if (goodRuns < GOOD_READS_TO_ENABLE) goodRuns++;
    if (goodRuns >= GOOD_READS_TO_ENABLE) setClk4(true);
  } else {
    goodRuns = 0;
    if (badRuns < BAD_READS_TO_DISABLE) badRuns++;
    if (badRuns >= BAD_READS_TO_DISABLE && DISABLE_ON_LOSS) setClk4(false);
  }

  // Panel indicators.
  //
  //   REF  solid  - CLKIN present and PLLA locked, CLK4 live
  //        blink  - CLKIN present but PLLA not locked (settling, or a
  //                 reference the PLL cannot use)
  //        off    - no signal on CLKIN: cable, DA or AR-40A
  //
  //   LOCK solid  - AR-40A reports locked
  //        off    - warm-up or holdover; the reference is running on the
  //                 free OCXO and is not yet rubidium-disciplined
  //
  // Together: REF off is a broken signal path. REF solid with LOCK off is a
  // working path on an undisciplined reference.
  bool slowBlink = (millis() / 500) & 1;

  if (g_losClkin) {
    digitalWrite(LED_REF_PIN, LOW);
  } else if (g_lolA) {
    digitalWrite(LED_REF_PIN, slowBlink);
  } else {
    digitalWrite(LED_REF_PIN, HIGH);
  }

  digitalWrite(LED_LOCK_PIN, locked ? HIGH : LOW);

  // Onboard LED is a heartbeat: proves the firmware is running rather than
  // hung, which neither panel LED would show.
  digitalWrite(STATUS_LED_PIN, (millis() / 1000) & 1);

  // Print on every BIT state change, and otherwise every 8th poll (~2 s).
  static uint8_t tick = 0;
  bool stateChanged = firstReport || (locked != lastRbLocked);
  if (stateChanged || (++tick >= 8)) {
    report(locked);
    tick = 0;
  }
  lastRbLocked = locked;
  firstReport  = false;

  delay(POLL_INTERVAL_MS);
}
