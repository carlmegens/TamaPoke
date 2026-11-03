// The battery gauge and the safe power-off (batcurve.h), ported from
// socquique/TamaPoke#27's tests: the curve is monotonic over its whole range,
// only a SUSTAINED low voltage powers off, a load dip that recovers does not,
// nothing powers off in the first 30 s, a failed read resets the count, and
// the percent only falls while discharging.
#include "batcurve.h"
#include <cstdio>
#include <cstdint>
static int bad = 0;
static void ck(bool ok, const char *w) { printf("%s  %s\n", ok ? "PASS" : "FAIL", w); if (!ok) bad++; }

// a reading every 2 s from t0 at one voltage; whether to power off at the end
static bool lowFor(BatMonitor &m, int mv, uint32_t t0, uint32_t ms) {
  bool r = false;
  for (uint32_t t = t0; t <= t0 + ms; t += 2000) { m.sample(mv, t); r = m.shutdownNeeded(t); }
  return r;
}

int main() {
  ck(batPercentFromMv(4300) == 100 && batPercentFromMv(4200) == 100 && batPercentFromMv(BAT_CUTOFF_MV) == 0 &&
     batPercentFromMv(3000) == 0 && batPercentFromMv(3840) == 50, "the ends and the middle of the curve");
  {
    int prev = 100; bool mono = true;
    for (int mv = 4200; mv >= BAT_CUTOFF_MV; mv--) { int p = batPercentFromMv(mv); mono &= p <= prev && p >= 0; prev = p; }
    ck(mono && batPercentFromMv(3660) == 8, "it never rises as the voltage falls");
  }
  { BatMonitor m; bool a = !lowFor(m, 3250, 100000, 20000), b = lowFor(m, 3250, 120000, 12000);
    ck(a && b, "20 s under the limit is not enough; 30 s straight is"); }
  {
    BatMonitor m;
    for (uint32_t t = 100000; t < 130000; t += 2000) m.sample(3700, t);
    bool dip = !lowFor(m, 3200, 130000, 20000);
    for (uint32_t t = 152000; t <= 158000; t += 2000) m.sample(3700, t);
    ck(dip && !m.shutdownNeeded(160000), "a 20 s dip under load that recovers does not power off");
  }
  {
    BatMonitor m;
    for (uint32_t t = 0; t <= 20000; t += 2000) m.sample(3200, t);
    bool early = !m.shutdownNeeded(20000) && !m.shutdownNeeded(BAT_BOOT_GRACE_MS - 1);
    for (uint32_t t = 22000; t <= 60000; t += 2000) m.sample(3200, t);
    ck(early && m.shutdownNeeded(60000), "nothing in the first 30 s after boot, then it does");
  }
  {
    BatMonitor m;
    lowFor(m, 3250, 100000, 24000);
    bool fail = m.sample(0, 126000) == -1 && !m.shutdownNeeded(128000);
    ck(fail && !lowFor(m, 3250, 128000, 20000), "a failed read starts the count again");
  }
  {
    BatMonitor m;
    m.seed(60);
    bool held = m.sample(4100, 1000) == 60;     // the charge-inflated voltage does not raise it
    int a = 60;
    for (uint32_t t = 3000; t < 23000; t += 2000) a = m.sample(3700, t);
    ck(held && a < 60 && a > 10 && m.sample(4000, 25000) == a, "the percent only falls while discharging");
  }
  printf(bad ? "\n%d FAILED\n" : "\nALL PASS\n", bad);
  return bad ? 1 : 0;
}
