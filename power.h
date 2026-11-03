#pragma once
#include <stdint.h>

// Power policy. Everything that decides how hard the board works lives here as
// pure functions with no hardware, so power_test can drive it on the PC.
// Ported from socquique/TamaPoke#26 (ViezeVingertjes), adapted to this fork.
//
// Where the battery goes: the AMOLED (brightness and content), the CPU while it
// draws and pushes the 434 KB canvas over QSPI (~70 ms of every 100 at 10 fps),
// and the main loop waking up for nothing.

// Idle steps (ms since the last touch or visible event)
#define POWER_CALM_MS 60000UL   // past this, draw a little less often
#define POWER_DIM1_MS 90000UL   // screen dimmed
#define POWER_DIM2_MS 300000UL  // screen almost off

enum DimStage : uint8_t { DIM_AWAKE = 0, DIM_LOW = 1, DIM_OFF = 2 };

struct PowerCtx {
  uint32_t idleMs;    // ms without interaction
  bool screenOff;     // switched off with the PWR button
  bool sleeping;      // the creature is asleep
  bool gameOpen;      // a minigame: its physics is tuned to 85 ms a frame
  bool usb;           // external power
  bool charging;
  int batPct;         // 0-100, -1 with no battery
};

// ms since `last`, never wrapping when `last` is a few ms AFTER `now`.
//
// That is the normal case, not an edge one: loop() reads the clock first and
// handleTouch() stamps lastInteract with a later millis(). `now - last` then
// wrapped to ~49 days of idleness, so for one pass the screen dropped to
// brightness 8 -- the black blink on every touch AND every release that was
// reported from the board. Only a lead of under a second counts as "now"; 25
// real days untouched are still 25 days.
inline uint32_t idleSince(uint32_t now, uint32_t last) {
  int32_t d = (int32_t)(now - last);
  return d < 0 && d > -1000 ? 0 : now - last;
}

inline uint8_t dimStageFor(uint32_t idleMs) {
  return idleMs > POWER_DIM2_MS ? DIM_OFF : idleMs > POWER_DIM1_MS ? DIM_LOW : DIM_AWAKE;
}

// low battery saves harder -- only when it really is running on the battery
inline bool batteryLow(const PowerCtx &c) { return c.batPct >= 0 && c.batPct < 20 && !c.charging && !c.usb; }
inline bool batteryCritical(const PowerCtx &c) { return c.batPct >= 0 && c.batPct < 10 && !c.charging && !c.usb; }

// panel brightness (0-255): the usual levels, capped when the battery is low
inline uint8_t brightnessFor(const PowerCtx &c) {
  if (c.screenOff) return 0;
  uint8_t stage = dimStageFor(c.idleMs);
  uint8_t t = c.sleeping ? 25 : (c.usb ? 180 : 145);
  if (stage == DIM_LOW) t = c.sleeping ? 10 : 60;
  else if (stage == DIM_OFF) t = 8;
  if (batteryCritical(c) && t > 60) t = 60;
  else if (batteryLow(c) && t > 100) t = 100;
  return t;
}

// ms between frames; 0 = draw nothing (screen off). Every frame is a full
// render plus the canvas over QSPI, and the panel only changes when redrawn.
inline uint32_t frameIntervalMs(const PowerCtx &c) {
  if (c.screenOff) return 0;
  if (c.gameOpen) return 85;
  uint32_t ms;
  uint8_t stage = dimStageFor(c.idleMs);
  if (stage == DIM_OFF) ms = 1000;
  else if (c.sleeping && c.idleMs > POWER_CALM_MS) ms = 500;   // asleep and untouched: little moves
  else if (stage == DIM_LOW) ms = 250;
  else ms = c.idleMs > POWER_CALM_MS ? 150 : 100;
  if (batteryLow(c)) ms *= 2;
  return ms;
}

// the main loop's pause on a pass that drew nothing
inline uint32_t idleDelayMs(const PowerCtx &c) { return c.screenOff ? 20 : 1; }

// With the screen off, or almost off, and no USB, the CPU light-sleeps between
// frames; never on USB, or the serial port the SD loader uses would drop.
// (TamaPoke.ino adds what this file cannot see: no radio link, no sound.)
inline bool lightSleepAllowed(const PowerCtx &c) {
  return !c.usb && (c.screenOff || dimStageFor(c.idleMs) == DIM_OFF);
}
#define POWER_SLEEP_MS 200  // each light sleep; a touch wakes it sooner

// how often the battery is read over I2C
inline uint32_t powerCacheMs(const PowerCtx &c) {
  return (c.screenOff || dimStageFor(c.idleMs) == DIM_OFF) ? 10000 : 2000;
}
