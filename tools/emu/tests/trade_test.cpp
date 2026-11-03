// Trading over the link, with no radio: two Links cross-wired in one process,
// over a channel that drops and duplicates frames on purpose.
//
// What is being held: a deal goes through only when BOTH sides said OK to the
// SAME pair of offers; changing an offer voids the other side's OK; each side
// stores the other's creature before giving its own away, so a link cut at
// any frame of the exchange leaves a creature on both devices or neither
// changed -- never one lost; a quiet READY link stays alive while players
// choose; what arrives is clamped; the old deal's repeats never reopen it; and
// Kadabra arrives as Alakazam.
#include "Arduino.h"
#include "Preferences.h"
#include "pet.h"
#include "party.h"
#include "battle.h"
#include "link.h"
#include <cstdio>
#include <cstring>
uint32_t g_seed=41; FakeSerial Serial; FakeESP ESP; FakeWire Wire;
volatile int g_touchX=0,g_touchY=0; volatile bool g_touchDown=false; bool wasPressed=false;
uint32_t millis(){return 0;} void FakeESP::restart(){exit(0);}
int FakeSerial::available(){return 0;} String FakeSerial::readStringUntil(char){return String("");}
void sfxPlay(uint8_t){}
static int bad=0;
static void ck(bool ok,const char*w){printf("%s  %s\n",ok?"PASS":"FAIL",w); if(!ok)bad++;}

static Link A, B;
static int dropEvery = 0, dupEvery = 0, seen = 0;
static bool cut = false;              // the radio dies: nothing more gets through
static int allowLeft = -1;            // frames still allowed through; -1 = no limit
static void wire(Link &to, const uint8_t *b, uint8_t n) {
  seen++;
  if (cut) return;
  if (allowLeft == 0) return;           // the budget is spent: dead from this frame on
  if (allowLeft > 0) allowLeft--;
  if (dropEvery && (seen % dropEvery) == 0) return;
  uint8_t tmp[260]; if (n > sizeof(tmp)) return;
  memcpy(tmp, b, n);
  to.onPacket(tmp, n);
  if (dupEvery && (seen % dupEvery) == 0) to.onPacket(tmp, n);
}
static void toB(void*, const uint8_t*b, uint8_t n){ wire(B, b, n); }
static void toA(void*, const uint8_t*b, uint8_t n){ wire(A, b, n); }

static uint32_t T = 0;
static void run(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 100) { T += 100; A.tick(T); B.tick(T); } }

static PartyMon pm(int16_t dex, uint16_t lvl, const char *nick) {
  PartyMon m; makeMon(dex, lvl, false, m); strcpy(m.nick, nick); return m;
}
static void pair() {
  A.send = toB; B.send = toA;
  A.begin(true, "A"); B.begin(false, "B");
  A.id = 0x1111; B.id = 0x2222;
  LinkMon lm = {}; lm.dex = 25; lm.level = 10; lm.maxHp = 30;
  for (int i = 0; i < SI_COUNT; i++) lm.base[i] = 20;
  A.addMon(lm); B.addMon(lm);
  A.tick(T); B.tick(T);
  A.start();
  run(3000);
}

// The app's half: store theirs when it arrives, give ours once they stored it.
// Counts what each device ends up holding.
struct Side { int gotDex = 0; bool gave = false; };
static void app(Link &l, Side &s) {
  if (l.tArrived) { s.gotDex = l.tTheirs.dex; l.tArrived = false; }
  if (l.tPhase == 1 && l.tPeerDone && s.gotDex) { s.gave = true; l.tPhase = 2; }
}
static void runApp(uint32_t ms, Side &a, Side &b) {
  for (uint32_t t = 0; t < ms; t += 100) { T += 100; A.tick(T); B.tick(T); app(A, a); app(B, b); }
}

int main() {
  // --- a clean deal
  dropEvery = dupEvery = 0; cut = false;
  pair();
  ck(A.state == LINK_READY && B.state == LINK_READY, "two devices paired");
  run(20000);
  ck(A.state == LINK_READY && B.state == LINK_READY, "a READY link stays up through 20 s of quiet (pings)");
  {
    Side a, b;
    A.tradeOffer(pm(64, 30, "SPOON"));       // KADABRA
    B.tradeOffer(pm(25, 20, "ZAP"));
    runApp(1000, a, b);
    ck(A.tradeHasTheirs() && B.tradeHasTheirs() && A.tTheirs.dex == 25 && B.tTheirs.dex == 64,
       "each side sees the other's offer");
    A.tradeAccept();
    runApp(1500, a, b);
    ck(!a.gotDex && !b.gotDex, "one OK alone moves nothing");
    B.tradeAccept();
    runApp(2000, a, b);
    ck(a.gotDex == 25 && b.gotDex == 64 && a.gave && b.gave, "both OK: each gets the other's, each gives its own");
    ck(!strcmp(B.tTheirs.nick, "SPOON") && B.tTheirs.level == 30, "the creature arrives whole: name, level");
    ck(tradeEvolution(64) == 65 && tradeEvolution(25) == 0, "KADABRA evolves by trade into ALAKAZAM; PIKACHU does not");
  }

  // --- changing an offer voids the other side's OK
  {
    Side a, b;
    A.tradeReset(); B.tradeReset();
    A.tradeOffer(pm(1, 10, "LEAF"));
    B.tradeOffer(pm(4, 10, "EMBER"));
    runApp(1000, a, b);
    B.tradeAccept();                          // B says OK to LEAF...
    runApp(600, a, b);
    A.tradeOffer(pm(7, 10, "BUBBLE"));        // ...and A swaps it for SQUIRTLE
    runApp(1000, a, b);
    A.tradeAccept();
    runApp(2000, a, b);
    ck(!a.gotDex && !b.gotDex, "an OK given to the old offer does not carry over to the new one");
    ck(B.tTheirs.dex == 7, "the new offer is the one on show");
    B.tradeAccept();
    runApp(2000, a, b);
    ck(a.gotDex == 4 && b.gotDex == 7, "OK again on the new pair, and it goes through");
    // B is still repeating the finished deal while A starts a new one: those
    // repeats must not reopen it
    A.tradeReset();
    runApp(3000, a, b);
    ck(!A.tradeHasTheirs(), "repeats of the finished deal do not reopen it");
    B.tradeReset();
  }

  // --- a lossy, duplicating channel
  {
    dropEvery = 3; dupEvery = 4;
    Side a, b;
    A.tradeOffer(pm(133, 15, "EEVEE"));
    B.tradeOffer(pm(92, 25, "GHOST"));
    runApp(3000, a, b);
    A.tradeAccept(); B.tradeAccept();
    runApp(6000, a, b);
    ck(a.gotDex == 92 && b.gotDex == 133 && a.gave && b.gave,
       "with one frame in three dropped and duplicates, the deal still completes once");
    dropEvery = dupEvery = 0;
  }

  // --- the radio dies at every possible frame of the exchange: never a loss.
  // Both OKs are given while the radio is down, so they CROSS in flight; then
  // exactly k frames get through, in either direction, and it dies for good.
  {
    bool everLost = false, sawDup = false, sawDone = false;
    for (int k = 0; k < 48; k++) {
      bool bFirst = k >= 24;              // which side speaks first after the outage
      cut = false; dropEvery = dupEvery = 0;
      pair();
      Side a, b;
      A.tradeOffer(pm(16, 12, "BIRD"));
      B.tradeOffer(pm(19, 12, "RAT"));
      runApp(1000, a, b);
      cut = true;
      A.tradeAccept(); B.tradeAccept();
      cut = false;
      allowLeft = k % 24;                 // exactly this many frames, then nothing
      for (int t = 0; t < 60; t++) {
        T += 100;
        if (bFirst) { B.tick(T); A.tick(T); } else { A.tick(T); B.tick(T); }
        app(A, a); app(B, b);
      }
      cut = true; allowLeft = -1;
      runApp(20000, a, b);
      // each device: has its own unless it gave it; has theirs if it got it
      int birds = (a.gave ? 0 : 1) + (b.gotDex == 16 ? 1 : 0);
      int rats  = (b.gave ? 0 : 1) + (a.gotDex == 19 ? 1 : 0);
      if (birds < 1 || rats < 1) everLost = true;
      if (birds > 1 || rats > 1) sawDup = true;
      if (a.gave && b.gave) sawDone = true;
    }
    ck(!everLost, "a link cut at ANY frame of the exchange never loses a creature");
    ck(sawDone, "and the sweep does reach complete deals (the cut is not always too early)");
    printf("      (some cuts leave a duplicate: %s)\n", sawDup ? "yes" : "no");
  }

  // --- what arrives over the air is clamped
  {
    PartyMon m = pm(25, 10, "X");
    m.level = 500; m.ivAtk = 200; m.moves[0] = 250; m.shiny = 7;
    memset(m.nick, 'A', sizeof(m.nick));     // no terminator at all
    ck(tradeSanitize(m) && m.level == MAX_LEVEL && m.ivAtk == 31 && m.moves[0] == 0 && m.shiny == 1 &&
       strlen(m.nick) < sizeof(m.nick), "a mangled creature is clamped, its name terminated");
    PartyMon z; z.dex = 9999;
    ck(!tradeSanitize(z), "and a dex past the table is refused outright");
  }

  printf(bad ? "\n%d FAILED\n" : "\nALL PASS\n", bad);
  return bad ? 1 : 0;
}
