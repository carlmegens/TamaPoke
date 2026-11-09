// The menu: two pages of four tiles, CLOSE below them, page dots between.
//
// What is held here:
// - every tile opens what its picture says, on the page it is drawn on;
// - a horizontal swipe turns the page and NEVER closes the menu, even off
//   either end -- a child flinging at it must not land back on the pet;
// - the dots under the tiles pick a page by a tap;
// - a greyed tile refuses AND keeps the menu up (one predicate for both);
// - TRADE pairs with no squad picker and no fight, and lands on the trade
//   screen by itself once paired -- while a LAN opened from the gyms does not;
// - every tile and CLOSE sit fully on the round glass.
#include "Arduino.h"
#include "Arduino_GFX_Library.h"
#include "Preferences.h"
#include "pet.h"
#include "party.h"
#include "battle.h"
#include "link.h"
#include <cstdio>
#include <cstring>
uint32_t g_seed=31; FakeSerial Serial; FakeESP ESP; FakeWire Wire;
volatile int g_touchX=0,g_touchY=0; volatile bool g_touchDown=false;
void FakeESP::restart(){exit(0);}
int FakeSerial::available(){return 0;}
String FakeSerial::readStringUntil(char){return String("");}
void setup(); void loop(); void render();
void onTap(int16_t x, int16_t y);
void onSwipe(int dir); void onSwipeV(int dir);
int menuHit(int16_t x, int16_t y);
bool menuItemDisabled(uint8_t i);
void lanTap(int16_t x, int16_t y);
extern Pet pet;
extern Link lan;
extern bool menuOpen, galleryOpen, exploreOpen, gymOpen, gymPick, lanOpen, tradeOpen,
            partyOpen, cardOpen, clockOpen, pickOpen, lanForTrade;
extern uint8_t menuPage, cardPage, choiceKind;
static int bad=0;
static void ck(bool ok,const char*w){printf("%s  %s\n",ok?"PASS":"FAIL",w); if(!ok)bad++;}

// The firmware's own layout, asked rather than restated: a tile's centre is
// found by scanning for the point menuHit() says is that item.
static bool centreOf(int item, int16_t &cx, int16_t &cy) {
  long sx = 0, sy = 0, n = 0;
  for (int y = 0; y < 466; y += 3) for (int x = 0; x < 466; x += 3)
    if (menuHit(x, y) == item) { sx += x; sy += y; n++; }
  if (!n) return false;
  cx = (int16_t)(sx / n); cy = (int16_t)(sy / n);
  return true;
}
static void closeAll() {
  menuOpen = galleryOpen = exploreOpen = gymOpen = gymPick = lanOpen = tradeOpen = false;
  partyOpen = cardOpen = clockOpen = pickOpen = lanForTrade = false;
  choiceKind = 0;
}
static void openMenu(uint8_t page) { closeAll(); onTap(233, 60); menuPage = page; }
static bool tapItem(int item) {
  int16_t x, y;
  if (!centreOf(item, x, y)) return false;
  onTap(x, y);
  return true;
}

static Link P;
static void wireToP(void*, const uint8_t*b, uint8_t n){ uint8_t t[260]; memcpy(t,b,n); P.onPacket(t,n); }
static void wireToUs(void*, const uint8_t*b, uint8_t n){ uint8_t t[260]; memcpy(t,b,n); lan.onPacket(t,n); }
static uint32_t T = 1000;
static void pairWithPeer() {   // the sketch's lan already holds our squad
  P.begin(false, "GARY");
  lan.send = wireToP; P.send = wireToUs;
  lan.id = 1; P.id = 2;
  LinkMon lm = {}; lm.dex = 25; lm.level = 10; lm.maxHp = 30;
  for (int i = 0; i < SI_COUNT; i++) lm.base[i] = 20;
  P.addMon(lm);
  lan.tick(T); P.tick(T);
  lan.start();
  for (int i = 0; i < 6; i++) { T += 500; lan.tick(T); P.tick(T); }
}
static PartyMon mk(int16_t d, uint16_t lv){ PartyMon m; makeMon(d, lv, false, m); return m; }

int main(){
  setup();
  for (int i=0;i<3;i++) render();
  if (pet.awaitingStarter()) pet.chooseStarter(4);
  if (pet.isEgg()) pet.dbgHatchAs(4,false);
  while (pet.hasLearnOffer()) pet.declineLearn();
  pet.ageMinutes = 60UL * 20;
  for (int i=0;i<PARTY_SLOTS;i++) party.releaseAt(i);
  for (int i=0;i<BOX_SLOTS;i++) party.boxReleaseAt(i);
  party.add(mk(25, 20));

  // --- the name band opens it, on the first page
  closeAll(); menuPage = 1;
  onTap(233, 60);
  ck(menuOpen && menuPage == 0, "the name band opens the menu on its first page");
  render();

  // --- geometry: each tile and CLOSE on the glass, nothing overlapping
  bool onGlass = true;
  for (uint8_t p = 0; p < 2; p++) {
    menuPage = p;
    for (int y = 0; y < 466; y++) for (int x = 0; x < 466; x++) {
      int h = menuHit(x, y);
      if (h == -2 || h == -1) continue;                    // outside / nothing
      long dx = x - 233, dy = y - 233;
      if (dx * dx + dy * dy > 231L * 231L) onGlass = false;
    }
  }
  ck(onGlass, "every tile, the dots and CLOSE lie on the round glass");

  // --- page 1 tiles
  openMenu(0); ck(tapItem(0) && galleryOpen && !menuOpen, "POKEDEX opens the Pokedex");
  openMenu(0); ck(tapItem(1) && exploreOpen && !menuOpen, "EXPLORE opens the map");
  openMenu(0); ck(tapItem(2) && gymOpen && gymPick && !menuOpen, "BATTLE opens the gym chooser");
  openMenu(0); ck(tapItem(3) && lanOpen && lanForTrade && !menuOpen, "TRADE opens pairing, marked as a trade");
  // --- page 2 tiles
  openMenu(1); ck(tapItem(4) && partyOpen && !menuOpen, "TEAM opens the party");
  openMenu(1); ck(tapItem(5) && cardOpen && cardPage == 1 && !menuOpen, "STATS opens the stats page");
  openMenu(1); ck(tapItem(6) && clockOpen && !menuOpen, "SETTINGS opens the settings");
  openMenu(1); ck(tapItem(7) && choiceKind == 3 && !menuOpen, "FAREWELL asks to say goodbye");
  openMenu(0); ck(menuHit(305, 143) == 1, "page 1 draws EXPLORE top-right (the manual points there)");
  openMenu(1); ck(menuHit(305, 143) == 5, "page 2 draws STATS top-right");

  // --- swiping pages and never closes
  openMenu(0);
  onSwipe(-1);
  ck(menuOpen && menuPage == 1, "a swipe left turns to page 2");
  onSwipe(-1);
  ck(menuOpen && menuPage == 1, "another swipe left, off the end: still open, still page 2");
  onSwipe(+1);
  ck(menuOpen && menuPage == 0, "a swipe right comes back to page 1");
  onSwipe(+1);
  ck(menuOpen && menuPage == 0, "off the front: still open");

  // --- the dots
  openMenu(0);
  onTap(300, 324);
  ck(menuOpen && menuPage == 1, "tapping the right-hand dot turns to page 2");
  onTap(166, 324);
  ck(menuOpen && menuPage == 0, "tapping the left-hand dot comes back");

  // --- CLOSE and outside
  openMenu(0); onTap(233, 75 + 262 + 22);
  ck(!menuOpen, "CLOSE closes");
  openMenu(0); onTap(30, 233);
  ck(!menuOpen, "a tap on the dimmed area closes");

  // --- a greyed tile refuses and the menu stays
  party.releaseAt(0);
  ck(menuItemDisabled(3), "with nobody to offer, TRADE is greyed");
  openMenu(0); tapItem(3);
  ck(menuOpen && !lanOpen, "and a tap on it does nothing but leave the menu up");
  party.add(mk(25, 20));
  ck(!menuItemDisabled(3), "with someone to offer, TRADE works again");

  // --- TRADE: pairing with no picker, then the trade screen by itself
  openMenu(0); tapItem(3);
  render();
  lanTap(233, 148);                                   // HOST
  ck(!pickOpen && lanOpen, "HOST from TRADE skips the squad picker");
  ck(lan.mineN > 0, "and still says hello with a squad");
  pairWithPeer();
  ck(lan.ready(), "paired");
  loop();
  ck(tradeOpen && !lanForTrade, "once paired, the trade screen opens by itself");

  // --- the same pairing from the gyms stays on the LAN screen
  closeAll();
  gymOpen = true; gymPick = true;
  lan.state = LINK_OFF;
  lanForTrade = true;                                  // a stale flag must not leak
  render();
  onTap(233, 336 + 22);                                // the gym chooser's LAN button
  ck(lanOpen && !lanForTrade, "LAN battle from the gyms is not a trade");
  lan.state = LINK_READY;
  loop();
  ck(!tradeOpen, "so being paired does not jump to the trade screen");

  printf(bad ? "\n%d FAILED\n" : "\nall passed\n", bad);
  return bad ? 1 : 0;
}
