// Trading as the sketch wires it: the LAN screen's TRADE button, the picker,
// OK, and tradePoll() -- the one place a trade touches the party. The other
// device is a second Link cross-wired to the sketch's `lan`, so no radio.
//
// What is being held: our creature leaves ONLY after the peer has stored it --
// with the peer's confirmation lost we briefly hold both, never neither; a
// KADABRA arrives as ALAKAZAM and both land in the Pokedex; a full party and
// box cannot say OK; leaving the trade screen keeps the pairing; and the
// picker pages on a swipe rather than closing.
#include "Arduino.h"
#include "Arduino_GFX_Library.h"
#include "Preferences.h"
#include "pet.h"
#include "party.h"
#include "battle.h"
#include "link.h"
#include <cstdio>
#include <cstring>
uint32_t g_seed=8; FakeSerial Serial; FakeESP ESP; FakeWire Wire;
volatile int g_touchX=0,g_touchY=0; volatile bool g_touchDown=false;
void FakeESP::restart(){exit(0);}
int FakeSerial::available(){return 0;}
String FakeSerial::readStringUntil(char){return String("");}
void setup(); void render();
void lanTap(int16_t x, int16_t y);
void onSwipe(int dir); void onSwipeV(int dir);
void tradePoll();
extern Pet pet;
extern Link lan;
extern bool lanOpen, tradeOpen, tradePicking;
extern uint8_t tradePage;
static int bad=0;
static void ck(bool ok,const char*w){printf("%s  %s\n",ok?"PASS":"FAIL",w); if(!ok)bad++;}

static Link P;                 // the other device
static bool toPeer = true;     // frames from us reach them
static void wireToP(void*, const uint8_t*b, uint8_t n){ if (!toPeer) return; uint8_t t[260]; memcpy(t,b,n); P.onPacket(t,n); }
static void wireToUs(void*, const uint8_t*b, uint8_t n){ uint8_t t[260]; memcpy(t,b,n); lan.onPacket(t,n); }
static uint32_t T = 1000;
static void step(int n = 1){ for (int i = 0; i < n; i++) { T += 500; lan.tick(T); P.tick(T); tradePoll();
  if (P.tArrived) { P.tArrived = false; }                       // the peer stores ours
  if (P.tPhase == 1 && P.tPeerDone) P.tPhase = 2; } }           // ...and gives theirs

static int countDex(int16_t d){ int n=0; for (auto &m : party.slots) n += m.dex==d; for (auto &m : party.box) n += m.dex==d; return n; }
static PartyMon mk(int16_t d, uint16_t lv){ PartyMon m; makeMon(d, lv, false, m); return m; }

int main(){
  setup();
  for (int i=0;i<3;i++) render();
  if (pet.awaitingStarter()) pet.chooseStarter(4);
  if (pet.isEgg()) pet.dbgHatchAs(4,false);
  for (int i=0;i<PARTY_SLOTS;i++) party.releaseAt(i);
  for (int i=0;i<BOX_SLOTS;i++) party.boxReleaseAt(i);
  party.add(mk(25, 20));       // PIKACHU, party slot 0: what we will offer
  party.add(mk(1, 15));

  // two paired devices
  lan.begin(true, "ASH"); P.begin(false, "GARY");
  lan.send = wireToP; P.send = wireToUs;
  lan.id = 1; P.id = 2;
  LinkMon lm = {}; lm.dex = 25; lm.level = 10; lm.maxHp = 30;
  for (int i = 0; i < SI_COUNT; i++) lm.base[i] = 20;
  lan.addMon(lm); P.addMon(lm);
  lan.tick(T); P.tick(T);
  lan.start();
  step(4);
  ck(lan.ready() && P.ready(), "two devices paired");
  lanOpen = true;
  render();

  lanTap(233, 318);                                   // TRADE
  ck(tradeOpen && tradePicking, "TRADE opens the picker");
  render();
  lanTap(73 + 50, 104 + 50);                          // the first creature: PIKACHU
  ck(!tradePicking && lan.tOffering && lan.tMine.dex == 25, "tapping a creature offers it");
  P.tradeOffer(mk(64, 30));                           // they offer KADABRA
  step(2);
  render();
  ck(lan.tradeHasTheirs() && lan.tTheirs.dex == 64, "their offer shows");
  P.tradeAccept();
  step(1);

  // our OK goes out, but their confirmation that they stored ours is lost
  toPeer = false;
  lanTap(320, 323);                                   // OK
  step(2);
  ck(countDex(65) == 1, "theirs arrives -- and KADABRA arrives as ALAKAZAM");
  ck(pet.isRegistered(64) && pet.isRegistered(65), "both KADABRA and ALAKAZAM are in the Pokedex");
  ck(countDex(25) == 1, "but ours is NOT given up before they say they stored it");

  toPeer = true;                                      // the radio recovers
  step(4);
  ck(countDex(25) == 0 && countDex(65) == 1, "once they confirm, ours is gone and theirs stays");
  ck(lan.tPhase == 2, "the deal is complete");
  render();

  // leaving the trade screen keeps the pairing
  onSwipeV(1);
  ck(!tradeOpen && lanOpen && lan.ready(), "a swipe out of the trade leaves both devices still paired");

  // a full party and box cannot take anything in
  for (int i = 0; i < PARTY_SLOTS; i++) if (party.slots[i].empty()) party.replaceAt(i, mk(16, 5));
  for (int i = 0; i < BOX_SLOTS; i++) party.boxAdd(mk(19, 5));
  lanTap(233, 318);
  render();
  ck(tradeOpen && tradePicking && tradePage == 0, "TRADE again");
  onSwipe(-1);
  ck(tradeOpen && tradePage == 1, "the picker pages on a swipe instead of closing");
  onSwipe(1);
  lanTap(73 + 50, 104 + 50);
  P.tradeReset(); P.tradeOffer(mk(7, 10));
  step(2);
  lanTap(320, 323);                                   // OK, with no room
  step(2);
  ck(!lan.tIAccepted && countDex(7) == 0, "with the party and box full, OK does nothing");

  printf(bad ? "\n%d FAILED\n" : "\nALL PASS\n", bad);
  return bad ? 1 : 0;
}
