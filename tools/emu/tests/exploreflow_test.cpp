// Exploring, played the way a hand plays it: real touches and swipes through
// loop(), with real time passing -- no state poked between steps.
//
// It exists because explore_test drove the screens by calling their functions,
// and three bugs reached the board anyway: the victory music played on long
// after leaving, a catch could fail to reach the Pokedex, and leaving the map
// flashed the panel black. A test that skips the loop skips whatever the loop
// does between two taps.
#include "Arduino.h"
#include "Arduino_GFX_Library.h"
#include "Preferences.h"
#include "pet.h"
#include "party.h"
#include "battle.h"
#include "dexbox.h"
#include "backs.h"
#include <chrono>
#include <thread>
#include <cstdio>
uint32_t g_seed=777; FakeSerial Serial; FakeESP ESP; FakeWire Wire;
volatile int g_touchX=0,g_touchY=0; volatile bool g_touchDown=false;
void FakeESP::restart(){exit(0);}
int FakeSerial::available(){return 0;}
String FakeSerial::readStringUntil(char){return String("");}
void setup(); void loop();
void emuFireInterrupt();
void exploreSpotAt(uint8_t i, int *x, int *y);
uint8_t uiCurrentScreen();
extern const char *const SCREEN_NAME[];
extern Pet pet;
extern Arduino_Canvas *gfx;
extern bool exploreOpen, battleOpen, btlCaught, btlWild, menuOpen;
extern int8_t expPlace;
extern uint8_t expActive, btlMsgCount;
extern uint32_t expActiveUntil;
extern PartyMon btlWildMon;
extern uint8_t g_emuMusic;

static int bad=0;
static void ck(bool ok,const char*w){printf("%s  %s\n",ok?"PASS":"FAIL",w); if(!ok)bad++;}
static void pump(int n){ for(int i=0;i<n;i++){ std::this_thread::sleep_for(std::chrono::milliseconds(30)); loop(); } }
static void click(int x,int y){
  g_touchX=x; g_touchY=y; g_touchDown=true; emuFireInterrupt(); pump(4);
  g_touchDown=false; emuFireInterrupt(); pump(3);
}
static void swipe(int x0,int y0,int x1,int y1){
  g_touchX=x0; g_touchY=y0; g_touchDown=true; emuFireInterrupt(); pump(1);
  for (int k=1;k<=6;k++){ g_touchX=x0+(x1-x0)*k/6; g_touchY=y0+(y1-y0)*k/6; emuFireInterrupt(); pump(1); }
  g_touchDown=false; emuFireInterrupt(); pump(4);
}
static const char *screen(){ return SCREEN_NAME[uiCurrentScreen()]; }

int main(){
  setup();
  pump(4);
  if (pet.awaitingStarter()) pet.chooseStarter(4);
  if (pet.isEgg()) pet.dbgHatchAs(4,false);
  while (pet.hasLearnOffer()) pet.declineLearn();
  pet.ageMinutes = 60UL * 20;
  pump(3);
  printf("      on '%s'\n", screen());

  // menu -> EXPLORE -> the forest
  click(233, 60);
  click(233, 75 + 16 + 46 + 4 + 22);
  ck(exploreOpen, "the menu's EXPLORE row opens the map");
  click(MAP_PINS[PL_FOREST].x * 2, MAP_PINS[PL_FOREST].y * 2);
  ck(expPlace == PL_FOREST, "a pin opens its place");

  // wait for a spot to stir, tap it -- until a fight starts
  int16_t dex = 0;
  for (int tries = 0; tries < 40 && !battleOpen; tries++) {
    pump(10);
    if (expActive < 5 && millis() < expActiveUntil) {
      int x, y; exploreSpotAt(expActive, &x, &y);
      click(x, y);
    }
    if (!pet.expCharges()) pet.expGain(EXP_CHARGE_UNITS * EXP_MAX);
  }
  ck(battleOpen && btlWild, "tapping the spot that stirs starts a wild fight");
  dex = btlWildMon.dex;

  // BALL until it holds, reading every line like a player would
  for (int i = 0; i < 80 && battleOpen; i++) {
    if (btlMsgCount) { click(233, 296); continue; }
    extern Combatant btlFoe, btlYou;
    btlFoe.hp = 1; btlYou.hp = btlYou.maxHp;   // the only help: it always holds in the end
    click(237 + 80, 274 + 22);                 // BALL
  }
  pump(5);
  ck(!battleOpen && exploreOpen, "the fight closes back onto the place");
  ck(pet.isRegistered(dex), "the catch is in the Pokedex");
  ck(dexBoxHas(dex), "...and in the Pokedex box");

  // the fanfare ends by itself, and leaving stops whatever is playing
  printf("      music after the catch: %u\n", g_emuMusic);
  // A swipe no longer leaves (a child's drag at a spot was one): BACK and the
  // map's arrow are the ways out.
  swipe(380, 233, 80, 233);
  ck(exploreOpen && expPlace == PL_FOREST, "a swipe on a place stays there");
  click(233, 395);                             // BACK, under the scene
  ck(exploreOpen && expPlace < 0, "BACK on a place goes back to the map");
  click(233 - 120, 46);                        // the map's back arrow
  ck(!exploreOpen, "the arrow on the map leaves exploring");
  ck(g_emuMusic == 0, "no music is left playing once exploring is closed");

  // None of these screens may clear to black before drawing: on the board the
  // panel's DMA can still be reading the last frame, and a slow draw over
  // black flashes the AMOLED. The emulator has no DMA to show it, so the rule
  // itself is held: no fillScreen() while the map, a place or a fight is drawn.
  {
    extern void openExplore(); extern void render(); extern bool startWildBattle(int16_t, uint8_t, uint8_t);
    openExplore(); int c0 = gfx->fillScreens; render();
    ck(gfx->fillScreens == c0, "the map is drawn without clearing to black first");
    expPlace = PL_OCEAN; c0 = gfx->fillScreens; render();
    ck(gfx->fillScreens == c0, "so is a place");
    startWildBattle(10, 10, PL_OCEAN); c0 = gfx->fillScreens; render();
    ck(gfx->fillScreens == c0, "and a fight");
    extern bool battleOpen; battleOpen = false; extern void closeExplore(); closeExplore();
    pump(2);
  }

  // no frame of the way out is a black panel
  int blackFrames = 0;
  click(233, 60); click(233, 75 + 16 + 46 + 4 + 22);   // in again
  for (int k = 0; k < 6; k++) {
    g_touchX = 380 - k * 50; g_touchY = 233; g_touchDown = (k < 5); emuFireInterrupt();
    loop();
    const uint16_t *b = gfx->buffer();
    long lit = 0; for (int i = 0; i < 466 * 466; i += 97) lit += b[i] != 0;
    if (lit < 50) blackFrames++;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  pump(6);
  printf("      black frames while leaving the map: %d\n", blackFrames);
  ck(blackFrames == 0, "leaving the map never shows a black panel");

  printf(bad ? "\n%d FAILED\n" : "\nALL PASS\n", bad);
  return bad ? 1 : 0;
}
