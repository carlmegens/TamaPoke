// The power policy (power.h), and the sketch actually following it.
//
// What is being held: the black blink on every touch and release is gone -- a
// touch stamped a few ms after loop() read its clock must leave the panel at
// full brightness, tested through the REAL updateBrightness(), the caller that
// had it wrong; with the screen off nothing is drawn; frames slow down the
// longer the creature is left alone and never stop while the screen is on;
// a minigame keeps its 85 ms; brightness is capped on a low battery; light
// sleep is never allowed on USB. Ported from socquique/TamaPoke#26's tests.
#include "Arduino.h"
#include "Arduino_GFX_Library.h"
#include "Preferences.h"
#include "pet.h"
#include "power.h"
#include <chrono>
#include <thread>
#include <cstdio>
uint32_t g_seed = 9; FakeSerial Serial; FakeESP ESP; FakeWire Wire;
volatile int g_touchX = 0, g_touchY = 0; volatile bool g_touchDown = false;
void FakeESP::restart() { exit(0); }
int FakeSerial::available() { return 0; }
String FakeSerial::readStringUntil(char) { return String(""); }
void setup(); void loop();
void updateBrightness(uint32_t now);
extern Pet pet;
extern Arduino_Canvas *gfx; extern Arduino_CO5300 *panel;
extern bool screenOff; extern uint32_t lastInteract; extern uint8_t dimStage;
static int bad = 0;
static void ck(bool ok, const char *w) { printf("%s  %s\n", ok ? "PASS" : "FAIL", w); if (!ok) bad++; }

static PowerCtx ctx(uint32_t idle) { PowerCtx c = {}; c.idleMs = idle; c.batPct = 80; return c; }

int main() {
  // --- the pure policy
  ck(idleSince(1000, 400) == 600 && idleSince(1000, 1002) == 0, "a touch a few ms after the clock was read is 'now', not 49 days");
  ck(idleSince(0x90000000u, 0) == 0x90000000u, "while weeks really untouched are still weeks");
  ck(dimStageFor(90000) == DIM_AWAKE && dimStageFor(90001) == DIM_LOW && dimStageFor(300001) == DIM_OFF,
     "the dim steps are the old 90 s and 5 min");
  {
    PowerCtx c = ctx(0);
    bool same = brightnessFor(c) == 145;
    c.usb = true; same &= brightnessFor(c) == 180;
    c = ctx(100000); same &= brightnessFor(c) == 60;
    c = ctx(400000); same &= brightnessFor(c) == 8;
    c = ctx(0); c.screenOff = true; same &= brightnessFor(c) == 0;
    ck(same, "brightness is unchanged on a healthy battery");
    c = ctx(0); c.batPct = 15; bool cap = brightnessFor(c) == 100;
    c.batPct = 5; cap &= brightnessFor(c) == 60;
    c.charging = true; cap &= brightnessFor(c) == 145;
    ck(cap, "and capped on a low one, but not while charging");
  }
  {
    uint32_t prev = 0; bool grows = true;
    for (uint32_t idle = 0; idle <= 600000; idle += 1000) {
      uint32_t ms = frameIntervalMs(ctx(idle));
      grows &= ms >= 100 && ms >= prev; prev = ms;
    }
    ck(grows && frameIntervalMs(ctx(0)) == 100, "frames slow down with idleness and never stop, 10 fps in use");
    PowerCtx g = ctx(200000); g.gameOpen = true; g.batPct = 5;
    PowerCtx off = ctx(0); off.screenOff = true;
    ck(frameIntervalMs(g) == 85 && frameIntervalMs(off) == 0, "a minigame keeps 85 ms; screen off draws nothing");
    PowerCtx s = ctx(400000); s.usb = true;
    PowerCtx t = ctx(0); t.screenOff = true;
    ck(!lightSleepAllowed(ctx(0)) && !lightSleepAllowed(s) && lightSleepAllowed(t),
       "light sleep only when nobody looks, and never on USB");
  }

  // --- the sketch following it
  setup();
  for (int i = 0; i < 3; i++) loop();
  if (pet.awaitingStarter()) pet.chooseStarter(4);
  if (pet.isEgg()) pet.dbgHatchAs(4, false);
  while (pet.hasLearnOffer()) pet.declineLearn();
  lastInteract = millis();
  updateBrightness(millis());
  uint8_t full = panel->brightness;
  // what loop() does on a touch: read the clock, then handleTouch() stamps a
  // LATER millis() into lastInteract, then updateBrightness(the earlier clock)
  uint32_t now = millis();
  lastInteract = now + 3;
  updateBrightness(now);
  ck(dimStage == 0 && panel->brightness == full && full > 8,
     "a touch stamped after the clock was read leaves the panel lit (the black blink)");

  // with the screen off, no frame is drawn at all
  screenOff = true;
  gfx->frameReady = false;
  for (int i = 0; i < 8; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(40)); loop(); }
  ck(!gfx->frameReady, "with the screen off, nothing is redrawn");
  screenOff = false;
  lastInteract = millis();
  for (int i = 0; i < 8 && !gfx->frameReady; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(40)); loop(); }
  ck(gfx->frameReady, "and the screen back on draws again (the guard is not stuck)");

  printf(bad ? "\n%d FAILED\n" : "\nALL PASS\n", bad);
  return bad ? 1 : 0;
}
