// How strong a wild creature is, through the real exploring screens.
//
// What is being held: the wild creature's level follows whoever goes out FIRST
// -- the creature being raised, or with an egg waiting the first of the party
// -- and NOT the strongest member banked. A level-1 newborn backed by a
// level-70 party used to lead into level-70 wild creatures and be knocked out
// at once.
#include "Arduino.h"
#include "Arduino_GFX_Library.h"
#include "Preferences.h"
#include "pet.h"
#include "party.h"
#include "battle.h"
#include <cstdio>
uint32_t g_seed=77; FakeSerial Serial; FakeESP ESP; FakeWire Wire;
volatile int g_touchX=0,g_touchY=0; volatile bool g_touchDown=false;
void FakeESP::restart(){exit(0);}
int FakeSerial::available(){return 0;}
String FakeSerial::readStringUntil(char){return String("");}
void setup(); void render();
void openExplore(); void closeExplore();
void exploreTap(int16_t x, int16_t y);
void exploreSpotAt(uint8_t i, int *x, int *y);
extern Pet pet;
extern bool battleOpen, btlWild, exploreOpen;
extern int8_t expPlace;
extern uint8_t expActive;
extern uint32_t expActiveUntil;
extern Combatant btlYou, btlFoe;
static int bad=0;
static void ck(bool ok,const char*w){printf("%s  %s\n",ok?"PASS":"FAIL",w); if(!ok)bad++;}

// Meets wild creatures in the meadow through the real taps, and returns the
// lowest and highest level faced and the level of who led.
static void meetMany(int n, int *lo, int *hi, int *lead) {
  *lo = 999; *hi = 0; *lead = -1;
  for (int i = 0; i < n; i++) {
    pet.expGain(EXP_CHARGE_UNITS * EXP_MAX);
    openExplore(); expPlace = PL_MEADOW;
    expActive = 0; expActiveUntil = millis() + 1000;
    int x, y; exploreSpotAt(0, &x, &y);
    exploreTap(x, y);
    if (battleOpen && btlWild) {
      if (btlFoe.level < *lo) *lo = btlFoe.level;
      if (btlFoe.level > *hi) *hi = btlFoe.level;
      *lead = btlYou.level;
      battleOpen = false; btlWild = false;
    }
    closeExplore();
  }
}

static PartyMon mk(int16_t d, uint16_t lv) { PartyMon m; makeMon(d, lv, false, m); return m; }

int main(){
  setup();
  for (int i = 0; i < 3; i++) render();
  if (pet.awaitingStarter()) pet.chooseStarter(4);
  for (int i = 0; i < PARTY_SLOTS; i++) party.releaseAt(i);
  for (int i = 0; i < PARTY_SLOTS; i++) party.add(mk(16 + i, 70));   // a strong banked team
  int lo, hi, lead;

  // --- a newborn, backed by a level-70 party
  pet.dbgHatchAs(1, false);
  while (pet.hasLearnOffer()) pet.declineLearn();
  meetMany(40, &lo, &hi, &lead);
  char w[96];
  snprintf(w, sizeof(w), "a level-1 newborn meets levels %d..%d, not 70", lo, hi);
  ck(lead == 1 && lo >= 2 && hi <= 3, w);

  // --- a grown one
  pet.ageMinutes = 29UL * MINUTES_PER_LEVEL;          // level 30
  meetMany(40, &lo, &hi, &lead);
  snprintf(w, sizeof(w), "a level-30 creature meets levels %d..%d", lo, hi);
  ck(lead == 30 && lo >= 28 && hi <= 32 && hi > lo, w);

  // --- an egg waiting: the first of the party leads, not the strongest
  party.replaceAt(0, mk(25, 12));
  party.replaceAt(3, mk(6, 95));
  pet.newEgg();
  meetMany(40, &lo, &hi, &lead);
  snprintf(w, sizeof(w), "with an egg, the party's first (12) sets it: %d..%d", lo, hi);
  ck(lead == 12 && lo >= 10 && hi <= 14, w);

  printf(bad ? "\n%d FAILED\n" : "\nALL PASS\n", bad);
  return bad ? 1 : 0;
}
