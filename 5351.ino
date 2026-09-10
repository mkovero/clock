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
// Serial1 (pin 0 = RX1, pin 1 = TX1) carries the same status text as the USB
// console to the CM4, 115200 8N1:
//
//   Teensy pin 1 (TX1) -> CM4 40-pin header pin 33 (GPIO13, RXD5)
//   Teensy pin 0 (RX1) <- CM4 40-pin header pin 32 (GPIO12, TXD5)
//   Teensy GND         -- CM4 40-pin header pin 34
//
// Needs dtoverlay=uart5 in the CM4's config.txt; the port is /dev/ttyAMA5.
// Nothing is read from RX1 yet - it is wired for a future command channel.
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
// FIVE states, not two. Neither an I2C failure nor a corrupted reading is a
// reference failure, and neither may change the output state:
//
//   not configured     -> force CLK4 off, retry si5351Init() (see below)
//   comm failed        -> hold whatever CLK4 was doing, count the error
//   reading rejected   -> hold, count separately (see below)
//   reference bad      -> disable after BAD_READS_TO_DISABLE consecutive polls
//   reference good     -> enable after GOOD_READS_TO_ENABLE consecutive polls
//
// "Not configured" is its own state because it is not a reference fault and
// must never be treated as one. si5351Init() can fail - the device can be
// absent, or stuck asserting SYS_INIT - and if it does, PLLA, MS4, reg 15
// (CLKIN as PLL source) and reg 20 (drive) were never written. Enabling reg 3
// on such a part does not produce 54 MHz, it lets whatever the unprogrammed
// MultiSynth is doing out of the pin: the exact failure mode the gating exists
// to prevent, arrived at from the other direction. So the output is forced off
// and held off, unconditionally, until an init actually succeeds.
//
// Earlier versions leaned on SYS_INIT being part of the "reference good" test
// to cover this by accident. It no longer is (see readStatus), so the
// interlock is now explicit and does not depend on any status bit.
//
// The first version of this sketch returned 0xFF from a failed read, which is
// indistinguishable from a register with every fault bit set. Single bus
// glitches therefore killed the CM4's clock for a second at a time. The tell
// was the sticky register: reg 1 read 0xC0 throughout, i.e. LOL_A_STKY and
// LOS_CLKIN_STKY never latched, proving the reference had not actually
// dropped.
//
// -----------------------------------------------------------------------------
// I2C DATA INTEGRITY
//
// A transaction can succeed while the data it carried is wrong. One poll
// returned reg0 = 0xC1 with SYS_INIT apparently set, every other flag
// unchanged and the sticky byte unchanged - a bit error in transit, not an
// event. Wire.endTransmission() returned 0, so i2cErr did not count it and the
// true error rate was higher than the counter showed. Only the two-read
// debounce stopped it reaching the output.
//
// The sticky register makes such a reading detectable in software. A live
// fault bit in reg 0 cannot be set while its sticky counterpart in reg 1 reads
// clear: the sticky latches when the condition asserts, this sketch clears it
// at the end of every poll, and a condition that is still true re-latches it
// immediately. So
//
//   reg0 fault set  AND  matching reg1 sticky clear   ->  impossible
//
// and any sample showing that is discarded rather than acted on. Discarded
// samples do NOT clear the sticky byte, so the evidence accumulates for the
// next comparison. They are counted in dataErr, reported alongside i2cErr.
//
// Fail-safe: the check rests on the assumption that a persistent condition
// keeps its sticky bit set. If that assumption is ever wrong on this part, a
// genuine reference loss would be rejected forever and CLK4 would be left
// running at the free-VCO frequency - exactly the failure this sketch exists
// to prevent. So after CONTRADICTIONS_TO_TRUST consecutive rejections the
// check stands down, the reading is believed, and the bypass is logged.
//
// The stand-down LATCHES. It is released only by a sample that is not
// contradictory at all - not by the next poll, and not by the bypassed sample
// itself. Resetting the run counter on the bypassed poll re-armed the check
// immediately and produced a repeating 8-reject / 1-accept cycle: the sketch
// then acted on status one poll in nine and took ~2.5 s rather than ~0.5 s to
// notice a real reference loss, while the panel LEDs sat frozen in between.
//
// A second, independent integrity check, which needs no assumption about
// sticky behaviour at all: reg 0 bits 2:0 are REVID, a constant for the life
// of the part. si5351Init() latches the value it reads; any later sample whose
// REVID disagrees contains a bit error, by definition, and is discarded.
//
// REVID does NOT catch the 0xC1 event, and an earlier revision of this comment
// wrongly claimed it did. Measured on this board: reg 0 reads 0x41 steadily
// (LOL_B set, PLLB unconfigured; REVID = 001) and the glitch reads 0xC1. Those
// differ in bit 7 alone. REVID is identical in both. The check is still worth
// its cost - it catches errors in the low three bits and a swapped part - but
// bit 7 needs a rule of its own, below.
//
// -----------------------------------------------------------------------------
// WHAT THE STICKY BYTE MEASURED
//
// reg 1 reads 0xC0 on every poll, in every reference state observed so far.
// Decomposed, that settles three things:
//
//   LOL_A_STKY, LOS_CLKIN_STKY  clear on every poll
//     -> the siWrite(1, 0x00) at the end of each poll genuinely clears.
//
//   LOL_B_STKY                  set on every poll
//     -> PLLB is unconfigured and permanently unlocked, and its sticky
//        re-asserts after every clear. So this part LEVEL-latches: a condition
//        that is still true re-latches immediately. That is precisely the
//        assumption the reg0/reg1 cross-check rests on, now measured rather
//        than assumed, and it means CONTRADICTIONS_TO_TRUST should never fire.
//
//   SYS_INIT_STKY               set on every poll
//     -> but LOL_A_STKY stays clear across the same polls, and a device that
//        is genuinely re-running its power-up initialisation cannot hold PLLA
//        locked through it. So SYS_INIT is not actually asserting: bit 7's
//        sticky is stuck on this part and carries no information at all.
//        Excluding it from FAULT_MASK was correct.
//
// Which leaves bit 7 with no sticky counterpart that can ever contradict it,
// hence the live-bit rule:
//
//   SYS_INIT set AND LOL_A clear  ->  impossible
//
// The PLLs are not locked while the device is initialising. A sample showing
// both is discarded, exactly like a sticky contradiction. This is what catches
// reg0 = 0xC1 against a steady 0x41 with PLLA locked and CLKIN present
// throughout - one bit flipped in transit, every other bit unchanged.
//
// SYS_INIT is not gated on, so these samples were harmless to the output; they
// were simply invisible, and dataErr read 0 while the true bit-error rate did
// not. Counting them is the point.
//
// Both live-bit and sticky rejections stand down after their respective
// run limits, for the same fail-safe reason, and both latch until clean.
//
// If REVID disagrees for REVID_MISMATCHES_TO_REINIT consecutive polls, the
// conclusion is not "noisy bus" but "this is not the device that was
// configured" - a hot-swap, a brown-out, an undervolt reset. That drops
// g_initOk, which forces CLK4 off and re-runs si5351Init().
//
// Bus hardening on the hardware side: the poll rate is 4 Hz, so bus speed is
// irrelevant and SCL runs at 100 kHz rather than 400 kHz. The Teensy 4.0's
// internal pull-ups are weak; if these errors persist, fit external 2.2k-4.7k
// to 3V3 on SDA and SCL and keep the wires short.
// -----------------------------------------------------------------------------
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

// ---- logging ----------------------------------------------------------------
//
// Everything the sketch reports goes to BOTH the USB console and Serial1
// (pins 0 = RX1, 1 = TX1), which runs to the CM4's ttyAMA5 on GPIO12/13.
// USB is for the bench; Serial1 is the permanent link and is what the CM4
// logs. Writing to a USB port with no host attached is harmless on a
// Teensy 4.0 - the guard below just avoids the work.
//
// Defined here, above the first function, so the .ino preprocessor's
// generated prototypes land after the type rather than before it.

class DualPrint : public Print {
public:
  size_t write(uint8_t c) override {
    Serial1.write(c);
    if (Serial) Serial.write(c);
    return 1;
  }
  size_t write(const uint8_t *buf, size_t n) override {
    Serial1.write(buf, n);
    if (Serial) Serial.write(buf, n);
    return n;
  }
};

static DualPrint Log;

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

// Reg 0 / reg 1 bits cross-checked against each other. Only bits whose sticky
// counterpart carries information:
//   bit 5  LOL_A
//   bit 4  LOS_CLKIN
//
// Excluded, because their stickies are permanently asserted on this board and
// so can never contradict anything:
//   bit 7  SYS_INIT  - observed: sticky reads 0xC0 every poll, i.e. bit 7
//                      re-latches immediately after every clear
//   bit 6  LOL_B     - PLLB unconfigured, permanently unlocked
//   bit 3  LOS_XTAL  - no crystal fitted
//
// This was found the hard way: a glitched reg0 = 0xC1 sailed through the
// cross-check with dataErr = 0, because SYS_INIT_STKY was already set.
static const uint8_t  FAULT_MASK           = 0x30;

// Consecutive rejected samples after which the cross-check stands down and the
// reading is believed. ~2 s at the default poll interval. The stand-down then
// latches until a clean sample arrives. See the header note.
static const uint8_t  CONTRADICTIONS_TO_TRUST = 8;

// Reg 0 bits 2:0, device revision. Constant for the life of the part, so a
// sample that disagrees with the value latched at init is a transit error.
static const uint8_t  REVID_MASK           = 0x07;

// Live-bit consistency within reg 0: SYS_INIT (bit 7) cannot be set while
// LOL_A (bit 5) is clear. See the header note. Needs no sticky reasoning.
static const uint8_t  SYS_INIT_BIT         = 0x80;
static const uint8_t  LOL_A_BIT            = 0x20;

// Consecutive live-bit rejections after which the rule stands down and the
// reading is believed. Same fail-safe shape as CONTRADICTIONS_TO_TRUST: a rule
// that can reject forever would freeze the gating decision forever.
static const uint8_t  LIVE_REJECTS_TO_TRUST = 8;

// Consecutive REVID mismatches after which the part is assumed to have been
// reset or replaced rather than merely misread, forcing a re-init.
static const uint8_t  REVID_MISMATCHES_TO_REINIT = 4;

// How long to wait between attempts to re-run si5351Init() after a failure.
// Slow on purpose: a missing device should not flood the log or the bus.
static const uint32_t INIT_RETRY_MS        = 2000;

// Consecutive polls with no usable status before the REF LED says so. Two
// polls (~0.5 s) keeps a single rejected sample from flickering the panel.
static const uint8_t  BLIND_RUNS_TO_SHOW   = 2;

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

// Write a register and read it straight back. Returns false and logs on any
// mismatch or failed read. Used at init on the registers whose contents there
// is any reason to argue about later.
static bool siWriteVerify(uint8_t reg, uint8_t val, const char *what) {
  if (!siWrite(reg, val)) {
    Log.print("reg "); Log.print(reg);
    Log.print(" ("); Log.print(what); Log.println("): write failed");
    return false;
  }
  uint8_t rb;
  if (!siReadOk(reg, rb)) {
    Log.print("reg "); Log.print(reg);
    Log.print(" ("); Log.print(what); Log.println("): readback failed");
    return false;
  }
  if (rb != val) {
    Log.print("reg "); Log.print(reg);
    Log.print(" ("); Log.print(what); Log.print("): wrote 0x");
    Log.print(val, HEX); Log.print(" read 0x"); Log.println(rb, HEX);
    return false;
  }
  Log.print("reg "); Log.print(reg);
  Log.print(" ("); Log.print(what); Log.print(") verified 0x");
  Log.println(rb, HEX);
  return true;
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

// ---- device identity --------------------------------------------------------
//
// Declared above si5351Init() rather than with the rest of the status state so
// that init can latch REVID; the .ino preprocessor will not reorder these.

static uint8_t g_revid       = 0xFF;  // reg 0 bits 2:0, latched at init
static bool    g_revidKnown  = false;

// ---- init -------------------------------------------------------------------

static bool si5351Init() {
  // Wait for the device to finish its own power-up initialisation.
  uint32_t t0 = millis();
  while (siRead(0) & 0x80) {
    if (millis() - t0 > 100) return false;   // no device / stuck on SYS_INIT
  }

  // 0. Latch REVID before touching anything. A failed read here means there is
  //    no device to configure, so there is nothing to do but report it.
  uint8_t idReg;
  if (!siReadOk(0, idReg)) return false;
  g_revid      = idReg & REVID_MASK;
  g_revidKnown = true;

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

  // 9. CLK4 driver up, still gated off at reg 3. Read back: this is the
  //    register that decides drive strength (0x4F = 8 mA), and it is worth
  //    being able to say what it actually contains rather than what was sent.
  siWriteVerify(20, CLK4_CTRL, "CLK4 ctrl / 8 mA drive");

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
static bool     g_dataOk   = false;
static bool     g_bypassed = false;   // cross-check stood down this poll
static bool     g_bypassLatched  = false; // ...and stays down until a clean read
static uint32_t g_commErrs = 0;
static uint32_t g_dataErrs = 0;
static uint32_t g_revidErrs = 0;      // subset of dataErrs: REVID disagreed
static uint32_t g_liveErrs  = 0;      // subset of dataErrs: reg0 self-inconsistent
static bool     g_liveBypassed = false;   // live-bit rule stood down this poll
static bool     g_liveLatched  = false;   // ...and stays down until a clean read
static uint8_t  g_liveRejects  = 0;   // consecutive live-bit rejections
static uint8_t  g_contradictions = 0; // consecutive rejected samples
static uint8_t  g_revidMismatches = 0; // consecutive REVID disagreements

// Set by setup() and by the retry in loop(). False means PLLA, MS4, reg 15 and
// reg 20 are NOT known to hold the frequency plan, so the output must stay off
// whatever the status bits say.
static bool     g_initOk   = false;
static uint32_t g_initFails = 0;
static uint32_t g_lastInitAttempt = 0;

static void readStatus() {
  uint8_t r0, r1;
  g_bypassed     = false;
  g_liveBypassed = false;
  g_commOk = siReadOk(0, r0) && siReadOk(1, r1);
  if (!g_commOk) {
    g_commErrs++;
    return;                  // leave previous flags alone; caller holds state
  }

  // Identity check first: REVID is constant, so this needs no assumption about
  // how the part latches anything. A disagreement is a bit error, full stop.
  if (g_revidKnown && (r0 & REVID_MASK) != g_revid) {
    g_revidErrs++;
    g_dataErrs++;
    g_dataOk = false;
    g_reg0   = r0;
    g_sticky = r1;
    if (g_revidMismatches < REVID_MISMATCHES_TO_REINIT) g_revidMismatches++;
    if (g_revidMismatches >= REVID_MISMATCHES_TO_REINIT) {
      // Not a misread any more. Treat it as a different or reset device: drop
      // the configured flag so loop() forces CLK4 off and re-runs init.
      g_initOk = false;
    }
    return;                  // decision flags untouched; caller holds state
  }
  g_revidMismatches = 0;

  // Live-bit rule: the device cannot be running its power-up initialisation
  // while PLLA reports lock. Catches a bit error on SYS_INIT, which no sticky
  // comparison can see - bit 7's sticky is permanently asserted on this part,
  // so it can never contradict anything. See the header note.
  bool inconsistent = (r0 & SYS_INIT_BIT) && !(r0 & LOL_A_BIT);

  if (inconsistent) {
    if (!g_liveLatched && g_liveRejects < LIVE_REJECTS_TO_TRUST) {
      g_liveRejects++;
      g_liveErrs++;
      g_dataErrs++;
      g_dataOk = false;
      g_reg0   = r0;
      g_sticky = r1;
      // Not clearing reg 1, for the same reason as a sticky rejection.
      return;                // decision flags untouched; caller holds state
    }
    // Rejected too many times running. Believe it rather than freeze the
    // gating decision indefinitely, and keep believing it until a clean read.
    g_liveLatched  = true;
    g_liveBypassed = true;
  } else {
    g_liveLatched = false;
    g_liveRejects = 0;
  }

  // Cross-check: a live fault with a clear sticky counterpart cannot happen.
  bool contradictory = (r0 & FAULT_MASK & ~r1) != 0;

  if (contradictory) {
    if (!g_bypassLatched && g_contradictions < CONTRADICTIONS_TO_TRUST) {
      g_contradictions++;
      g_dataErrs++;
      g_dataOk = false;
      g_reg0   = r0;         // keep the raw bytes for the log
      g_sticky = r1;
      // Deliberately NOT clearing reg 1: the sticky evidence accumulates for
      // the next poll, which is what makes the next comparison stronger.
      return;                // decision flags untouched; caller holds state
    }
    // Rejected too many times in a row. Either the bus is badly broken or the
    // sticky assumption is wrong on this part. Believe the reading rather than
    // risk holding CLK4 on through a real reference loss - and keep believing
    // it, so the check cannot re-arm and start rejecting again next poll.
    g_bypassLatched = true;
    g_bypassed      = true;
  } else {
    // A clean sample is the only thing that re-arms the cross-check.
    g_bypassLatched  = false;
    g_contradictions = 0;
  }

  g_dataOk   = true;
  g_reg0     = r0;
  g_sticky   = r1;
  g_sysInit  = (g_reg0 >> 7) & 1;
  g_lolA     = (g_reg0 >> 5) & 1;
  g_losClkin = (g_reg0 >> 4) & 1;

  // SYS_INIT is reported but deliberately NOT gated on. Its sticky is
  // permanently asserted here, so it has no sticky counterpart to be checked
  // against, and gating on it would put a single flipped bit two polls away
  // from cutting the CM4's clock. The live-bit rule above now rejects such a
  // sample rather than acting on it, but the gating stays off bit 7 anyway:
  // startup is already covered, since si5351Init() waits for SYS_INIT to clear
  // and a genuine device reset would show up on LOL_A as well.
  g_refGood  = !g_lolA && !g_losClkin;
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
static uint8_t blindRuns    = 0;   // consecutive polls with no usable status

static void setClk4(bool on) {
  if (on == clk4Enabled) return;
  if (on) {
    // Re-lock cleanly before letting the output out: a PLL that has just
    // reacquired its reference benefits from a soft reset.
    siWrite(177, 0x20);
    delay(2);
    siWrite(3, OE_CLK4_ONLY);
    Log.print("CLK4: ENABLED  (54 MHz to CM4)  reg0=0x");
    Log.println(g_reg0, HEX);
  } else {
    siWrite(3, OE_ALL_OFF);
    Log.print("CLK4: DISABLED (reference lost - CM4 has no clock)  reg0=0x");
    Log.print(g_reg0, HEX);
    Log.print(" sticky=0x");
    Log.println(g_sticky, HEX);
  }
  clk4Enabled = on;
}

// Unconditional off, bypassing the clk4Enabled cache. Used when the device is
// not known to be configured: the cache describes what this sketch last wrote,
// which says nothing about a part that has just reset or was never programmed.
static void forceClk4Off() {
  siWrite(3, OE_ALL_OFF);
  if (clk4Enabled) {
    Log.println("CLK4: DISABLED (device not configured - holding output off)");
  }
  clk4Enabled = false;
  goodRuns    = 0;
  badRuns     = 0;
}

static void report(bool rbLocked) {
  Log.print("reg0=0x");     Log.print(g_reg0, HEX);
  Log.print(" SYS_INIT=");  Log.print(g_sysInit);
  Log.print(" LOL_A=");     Log.print(g_lolA);
  Log.print(" LOS_CLKIN="); Log.print(g_losClkin);
  Log.print(" sticky=0x");  Log.print(g_sticky, HEX);
  Log.print(" Rb=");        Log.print(rbLocked ? "LOCKED" : "UNLOCKED");
  Log.print(" CLK4=");      Log.print(clk4Enabled ? "on" : "off");
  Log.print(" i2cErr=");    Log.print(g_commErrs);
  Log.print(" dataErr=");   Log.print(g_dataErrs);
  Log.print(" revidErr=");  Log.print(g_revidErrs);
  Log.print(" liveErr=");   Log.print(g_liveErrs);
  Log.print(" init=");      Log.println(g_initOk ? "ok" : "FAILED");

  if (!g_initOk) {
    Log.print("  !! Si5351 not configured - CLK4 forced off, init attempts=");
    Log.println(g_initFails);
  }
  if (!g_commOk)  Log.println("  !! I2C read failed (output state held)");
  // A rejection has exactly one cause: the checks run in order and the first
  // to fire returns. Report whichever one it was.
  if (g_commOk && !g_dataOk && !g_revidMismatches && !g_liveRejects) {
    Log.print("  !! reading rejected: reg0 fault with clear sticky "
              "(output state held), run=");
    Log.println(g_contradictions);
  }
  if (g_commOk && !g_dataOk && g_liveRejects) {
    Log.print("  !! reading rejected: SYS_INIT set with PLLA locked "
              "(output state held), run=");
    Log.println(g_liveRejects);
  }
  if (g_liveBypassed) Log.println("  !! live-bit rule bypassed after repeated "
                                  "rejections - reading believed "
                                  "(latched until a clean sample)");
  if (g_bypassed) Log.println("  !! sticky cross-check bypassed after "
                              "repeated rejections - reading believed "
                              "(latched until a clean sample)");
  if (g_revidMismatches) {
    Log.print("  !! REVID mismatch: expected 0x"); Log.print(g_revid, HEX);
    Log.print(" run="); Log.println(g_revidMismatches);
  }
  if (g_losClkin) Log.println("  !! no signal on CLKIN");
  if (g_lolA)     Log.println("  !! PLLA not locked");
  if (g_sysInit)  Log.println("  !! SYS_INIT set (reported only, not gated on)");
  if (!rbLocked)  Log.println("  !! AR-40A unlocked or in holdover");
}

// -----------------------------------------------------------------------------

void setup() {
  Serial1.begin(115200);     // pins 0/1 -> CM4 ttyAMA5 (GPIO12/13)
  Serial.begin(115200);      // USB, bench only
  while (!Serial && millis() < 3000) { }

  pinMode(LOCK_BIT_PIN, INPUT_PULLUP);
  pinMode(LED_REF_PIN, OUTPUT);
  pinMode(LED_LOCK_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(LED_REF_PIN, LOW);
  digitalWrite(LED_LOCK_PIN, LOW);
  digitalWrite(STATUS_LED_PIN, LOW);

  // Wire on the Teensy 4.0 default pins: 18 = SDA, 19 = SCL.
  // 100 kHz, not 400 kHz: the poll rate is 4 Hz, so bus speed buys nothing,
  // and 400 kHz over wire on weak internal pull-ups is marginal. If dataErr
  // keeps climbing, the next step is external 2.2k-4.7k pull-ups to 3V3.
  Wire.begin();
  Wire.setClock(100000);
  delay(10);

  // A failed init is NOT fatal and must NOT return: loop() has to keep running
  // so that the output stays forced off, the LEDs keep saying so, and init is
  // retried. Returning here left loop() driving a chip nobody had configured.
  g_lastInitAttempt = millis();
  g_initOk = si5351Init();
  if (!g_initOk) {
    g_initFails++;
    Log.println("Si5351 not responding at 0x60 - CLK4 held off, will retry");
  }

  Log.println("Si5351C: CLKIN 10 MHz -> PLLA 864 MHz -> CLK4 54 MHz");
  Log.println("CLK4 gated on LOS_CLKIN / LOL_A, and on init having succeeded.");
  Log.println("BIT on pin 2 = indication only.");
  Log.println("I2C 100 kHz. i2cErr = failed transactions. dataErr = readings "
              "rejected by an integrity check, of which revidErr = REVID "
              "disagreed and liveErr = SYS_INIT set with PLLA locked.");
}

void loop() {
  // An unconfigured part is the one case where the output is driven rather
  // than held: reg 3 is written off every pass, because a device that has just
  // reset does not remember being disabled and its POR default is not 54 MHz.
  if (!g_initOk) {
    forceClk4Off();
    if (millis() - g_lastInitAttempt >= INIT_RETRY_MS) {
      g_lastInitAttempt = millis();
      g_initFails++;
      g_initOk = si5351Init();
      Log.print("Si5351 re-init ");
      Log.print(g_initOk ? "OK" : "FAILED");
      Log.print("  attempts="); Log.println(g_initFails);
      if (g_initOk) {
        // Nothing about the old status survives a reconfigure.
        g_contradictions = 0;
        g_bypassLatched  = false;
        g_liveRejects    = 0;
        g_liveLatched    = false;
      }
    }
  }

  readStatus();
  bool locked = rubidiumLocked();

  if (!g_initOk) {
    // Normally already forced off above - but readStatus() can clear g_initOk
    // mid-pass when REVID says the part reset, so force it again rather than
    // leave an unconfigured device driving CLK4 for a poll.
    forceClk4Off();
  } else if (!g_commOk || !g_dataOk) {
    // Neither a bus failure nor a corrupted reading tells us anything about
    // the reference. Hold the output where it is and do not touch the run
    // counters.
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
  //   REF  solid       - CLKIN present and PLLA locked, CLK4 live
  //        slow blink  - CLKIN present but PLLA not locked (settling, or a
  //                      1 Hz          reference the PLL cannot use)
  //        fast blink  - NO USABLE STATUS: init failed, the bus is down, or
  //        4 Hz          readings are being rejected. Says nothing about the
  //                      reference - it says the sketch cannot see it.
  //        off         - no signal on CLKIN: cable, DA or AR-40A
  //
  //   LOCK solid  - AR-40A reports locked
  //        off    - warm-up or holdover; the reference is running on the
  //                 free OCXO and is not yet rubidium-disciplined
  //
  // Together: REF off is a broken signal path. REF solid with LOCK off is a
  // working path on an undisciplined reference. REF fast is a Teensy-side or
  // I2C problem, and CLK4 is held wherever it was (or forced off, if the
  // device was never configured).
  //
  // The fast state exists because the status globals initialise fault-set and
  // are only updated by a good read. Without it, a dead I2C bus and a genuinely
  // absent reference both showed REF off and were indistinguishable on the
  // panel - which is what a bench indicator is for.
  bool slowBlink = (millis() / 500) & 1;
  bool fastBlink = (millis() / 125) & 1;

  if (!g_initOk || !g_commOk || !g_dataOk) {
    if (blindRuns < BLIND_RUNS_TO_SHOW) blindRuns++;
  } else {
    blindRuns = 0;
  }
  bool blind = blindRuns >= BLIND_RUNS_TO_SHOW;

  if (blind) {
    digitalWrite(LED_REF_PIN, fastBlink);
  } else if (g_losClkin) {
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
