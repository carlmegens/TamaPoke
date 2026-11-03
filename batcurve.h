#pragma once
#include <stdint.h>

// A single-cell LiPo (charged to 4.2 V, as Waveshare's example sets it): from
// resting voltage to percent, and when to switch off before it is drained too
// far. Pure logic, driven by batcurve_test. Ported from socquique/TamaPoke#27.

#define BAT_CUTOFF_MV 3300     // below this, discharging, the firmware saves and powers off
#define BAT_CUTOFF_READS 3     // consecutive readings below it (as well as the time, see BatMonitor)
#define BAT_HW_CUTOFF_MV 3100  // the PMU's own backstop (AXP2101: 2600..3300 in steps of 100)

// a typical LiPo discharge curve, (mV, %) from high to low
struct BatPoint { int16_t mv; uint8_t pct; };
static const BatPoint BAT_CURVE[] = {
  { 4200, 100 }, { 4150, 95 }, { 4110, 90 }, { 4080, 85 }, { 4020, 80 }, { 3980, 75 }, { 3950, 70 },
  { 3910, 65 }, { 3870, 60 }, { 3850, 55 }, { 3840, 50 }, { 3820, 45 }, { 3800, 40 }, { 3790, 35 },
  { 3770, 30 }, { 3750, 25 }, { 3730, 20 }, { 3710, 15 }, { 3690, 10 }, { 3610, 5 }, { BAT_CUTOFF_MV, 0 },
};

// linear between points; 100 above 4.2 V and 0 at the cut-off
inline int batPercentFromMv(int mv) {
  const int n = sizeof(BAT_CURVE) / sizeof(BAT_CURVE[0]);
  if (mv >= BAT_CURVE[0].mv) return 100;
  if (mv <= BAT_CURVE[n - 1].mv) return 0;
  for (int i = 1; i < n; i++) {
    if (mv >= BAT_CURVE[i].mv) {
      int dmv = BAT_CURVE[i - 1].mv - BAT_CURVE[i].mv, dp = BAT_CURVE[i - 1].pct - BAT_CURVE[i].pct;
      return BAT_CURVE[i].pct + (mv - BAT_CURVE[i].mv) * dp / dmv;
    }
  }
  return 0;
}

// Smooths the voltage (exponential, 1/4 per reading), lets the shown percent
// only fall while discharging (the charge-inflated voltage just after
// unplugging, or a dip under load, neither raise it nor make it dance), and
// decides the shutdown: the smoothed voltage must stay under the limit for
// BAT_CUTOFF_MS straight (and at least BAT_CUTOFF_READS readings).
#define BAT_CUTOFF_MS 30000UL
#define BAT_BOOT_GRACE_MS 30000UL  // never right after boot: the first reading means little

struct BatMonitor {
  int smoothMv = 0;
  int pct = -1;            // last percent shown (-1 = no reading yet)
  uint32_t lowSince = 0;
  uint8_t lowReads = 0;

  void reset() { smoothMv = 0; pct = -1; lowSince = 0; lowReads = 0; }
  // the ceiling at the start of a discharge (what the PMU was showing)
  void seed(int p) { if (pct < 0 && p >= 0) pct = p; }
  // One reading while discharging (no USB, not charging). The percent, or -1.
  int sample(int mv, uint32_t now) {
    if (mv <= 0) { lowReads = 0; lowSince = 0; return -1; }
    smoothMv = smoothMv ? smoothMv + (mv - smoothMv) / 4 : mv;
    if (smoothMv < BAT_CUTOFF_MV) {
      if (!lowReads) lowSince = now ? now : 1;
      if (lowReads < 255) lowReads++;
    } else {
      lowReads = 0;
      lowSince = 0;
    }
    int c = batPercentFromMv(smoothMv);
    if (pct < 0 || c < pct) pct = c;
    return pct;
  }
  bool shutdownNeeded(uint32_t now) const {
    return now >= BAT_BOOT_GRACE_MS && lowReads >= BAT_CUTOFF_READS && now - lowSince >= BAT_CUTOFF_MS;
  }
};
