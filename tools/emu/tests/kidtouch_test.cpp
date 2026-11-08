// Touch the way a seven-year-old touches, driven through the real handleTouch().
//
// What is being held: a drag inside exploring -- a child reaching for a
// stirring spot -- never leaves the scene or the map; a tap that wobbles 55 px
// or lasts 2 s still counts as a tap; and a real swipe still works on the main
// screen, so the first two cannot pass by gestures being dead altogether.
#include "Arduino.h"
#include "Arduino_GFX_Library.h"
#include "Preferences.h"
#include "pet.h"
#include <chrono>
#include <thread>
#include <cstdio>
uint32_t g_seed = 0xC0FFEE; FakeSerial Serial; FakeESP ESP; FakeWire Wire;
volatile int g_touchX = 0, g_touchY = 0; volatile bool g_touchDown = false;
void FakeESP::restart() { exit(0); }
int FakeSerial::available() { return 0; }
String FakeSerial::readStringUntil(char) { return String(""); }
void setup(); void loop();
void emuFireInterrupt();
void openExplore();
extern Pet pet;
extern bool exploreOpen, menuOpen, cardOpen;
extern int8_t expPlace;
extern uint32_t lastInteract;
static int bad = 0;
static void ck(bool ok, const char *w) { printf("%s  %s\n", ok ? "PASS" : "FAIL", w); if (!ok) bad++; }

static void pump(int n) { for (int i = 0; i < n; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(30)); loop(); } }
// press at (x0,y0), slide to (x1,y1) over `ms`, lift
static void gesture(int x0, int y0, int x1, int y1, int ms) {
  lastInteract = millis();                  // awake: a dimmed panel swallows the first touch
  g_touchX = x0; g_touchY = y0; g_touchDown = true; emuFireInterrupt(); pump(2);
  int steps = ms / 30; if (steps < 1) steps = 1;
  for (int i = 1; i <= steps; i++) {
    g_touchX = x0 + (x1 - x0) * i / steps; g_touchY = y0 + (y1 - y0) * i / steps;
    std::this_thread::sleep_for(std::chrono::milliseconds(30)); loop();
  }
  g_touchDown = false; emuFireInterrupt(); pump(3);
}

int main() {
  setup(); pump(3);
  if (pet.awaitingStarter()) pet.chooseStarter(4);
  if (pet.isEgg()) pet.dbgHatchAs(4, false);
  while (pet.hasLearnOffer()) pet.declineLearn();
  pump(3);

  // --- exploring: a drag is not a way out
  openExplore(); expPlace = PL_MEADOW; pump(2);
  gesture(150, 210, 330, 220, 200);         // fast and long: a real swipe's shape
  ck(exploreOpen && expPlace == PL_MEADOW, "a drag across the scene stays in the scene");
  gesture(233, 150, 240, 330, 200);
  ck(exploreOpen && expPlace == PL_MEADOW, "and so does a vertical one");
  expPlace = -1; pump(2);
  gesture(330, 300, 120, 300, 200);
  ck(exploreOpen && expPlace < 0, "a drag on the map stays on the map");
  exploreOpen = false; pump(3);

  // --- a wobbly, slow tap on the name opens the menu
  gesture(233, 60, 233 + 50, 60 + 20, 300);
  ck(menuOpen, "a tap that drifts 55 px still taps (the menu opens)");
  menuOpen = false; pump(3);
  gesture(233, 60, 238, 64, 2000);
  ck(menuOpen, "a tap held for 2 s still taps");
  menuOpen = false; pump(3);

  // --- and a real swipe still works where it should
  gesture(233, 300, 233, 120, 200);         // swipe up from the main screen
  ck(cardOpen, "a real swipe up still opens the card");

  printf(bad ? "\n%d FAILED\n" : "\nALL PASS\n", bad);
  return bad ? 1 : 0;
}
