// The battle's effects: drawn, never deciding anything -- but they do decide
// WHEN things are seen and heard, and that is what is held here.
//
// - A ball always plays its catch animation, and a catch is celebrated (the
//   victory music, the rocking ball) only at the click, not as the ball leaves.
// - A tap skips the animation straight to its end, keeping the narration, so a
//   child is never made to wait.
// - When the creature gets out, its answer is timed for after the ball, not
//   played while it is still inside -- and a skip pulls it forward again.
// - A move schedules an effect in the move's own type, and the second actor's
//   effect waits for the first to land.
#include "Arduino.h"
#include "Arduino_GFX_Library.h"
#include "Preferences.h"
#include "pet.h"
#include "battle.h"
#include "moves.h"
#include "audio.h"
#include <cstdio>
uint32_t g_seed = 0xBA11; FakeSerial Serial; FakeESP ESP; FakeWire Wire;
volatile int g_touchX = 0, g_touchY = 0; volatile bool g_touchDown = false;
void FakeESP::restart() { exit(0); }
int FakeSerial::available() { return 0; }
String FakeSerial::readStringUntil(char) { return String(""); }
void setup(); void loop(); void render();
void emuSetTimeScale(uint32_t s);
bool startWildBattle(int16_t dex, uint8_t lvl, uint8_t place);
void startBattle(int16_t dex, uint8_t lvl);
void battleTap(int16_t x, int16_t y);
struct BtlFx { uint32_t t0; uint8_t from, type, flags; int8_t stage; };   // as the sketch's
extern BtlFx btlFx[4];
extern Pet pet;
extern bool battleOpen, btlCaught, btlOver;
extern uint8_t btlMenu, btlMsgCount;
extern uint32_t btlCatchEnd, btlBallUntil, btlLungeUntil[2];
extern bool btlCatchOk;
extern Combatant btlYou, btlFoe;
extern uint8_t g_emuMusic;
extern uint32_t lastInteract;
static int bad = 0;
static void ck(bool ok, const char *w) { printf("%s  %s\n", ok ? "PASS" : "FAIL", w); if (!ok) bad++; }

#define BALL_X (237 + 60)
#define BALL_Y (274 + 22)

// Let the game clock run past `until` without touching anything.
static void waitPast(uint32_t until) {
  emuSetTimeScale(200);
  while ((int32_t)(millis() - until) < 50) { lastInteract = millis(); render(); }   // awake: an idle panel does not draw
  emuSetTimeScale(1);
}

static void fresh() {
  startWildBattle(16, 5, 0);
  btlMsgCount = 0;
}

// Throw until the ball does `want` (held or escaped); false if it never did.
static bool throwUntil(bool want) {
  for (int i = 0; i < 400; i++) {
    if (!battleOpen || btlOver) fresh();
    btlCatchEnd = 0; btlMsgCount = 0; btlMenu = 0;
    for (auto &f : btlFx) f.t0 = 0;           // only this throw's effects are looked at
    btlYou.hp = btlYou.maxHp;
    btlFoe.hp = want ? 1 : btlFoe.maxHp;     // full HP makes an escape likely
    battleTap(BALL_X, BALL_Y);
    if (btlCatchEnd && btlCatchOk == want) return true;
  }
  return false;
}

int main() {
  setup();
  if (pet.awaitingStarter()) pet.chooseStarter(4);
  if (pet.isEgg()) pet.dbgHatchAs(4, false);
  while (pet.hasLearnOffer()) pet.declineLearn();
  pet.ageMinutes = 60UL * 20;
  pet.sleeping = false;

  // --- a catch that holds
  fresh();
  ck(throwUntil(true), "a ball can hold");
  ck(btlCaught, "the catch itself is decided at once");
  ck(btlCatchEnd > millis(), "and its animation is running");
  ck(g_emuMusic != MUS_VICTORY && btlBallUntil == 0, "no fanfare and no rocking ball before the click");
  ck(btlMsgCount > 0, "the narration is queued behind it");
  waitPast(btlCatchEnd);
  ck(btlCatchEnd == 0, "the animation ends by itself");
  ck(g_emuMusic == MUS_VICTORY && btlBallUntil != 0, "the click brings the fanfare and the rocking ball");

  // --- a tap skips it
  fresh();
  ck(throwUntil(true), "a second ball holds");
  uint8_t msgs = btlMsgCount;
  g_emuMusic = MUS_NONE;                    // the first catch's fanfare must not count here
  battleTap(233, 233);
  ck(btlCatchEnd == 0 && g_emuMusic == MUS_VICTORY, "a tap skips straight to the click");
  ck(battleOpen && btlMsgCount == msgs, "and spends nothing else: the fight and its words are still there");

  // --- it gets out: the answer comes after the ball
  fresh();
  ck(throwUntil(false), "a ball can fail");
  bool answered = false, after = true;
  for (int tries = 0; tries < 100 && !answered; tries++) {
    if (tries) { if (!throwUntil(false)) break; }
    uint32_t end = btlCatchEnd;
    bool fx = false;
    for (auto &f : btlFx) if (f.t0 && f.from == 1) { fx = true; after = after && f.t0 >= end; }
    if (fx && btlLungeUntil[1] > millis()) { answered = true; after = after && btlLungeUntil[1] > end; }
  }
  ck(answered, "the creature answers a failed ball");
  ck(after, "and its answer is timed for after the ball, not while it is inside");
  uint32_t lunge = btlLungeUntil[1];
  battleTap(233, 233);
  ck(btlCatchEnd == 0 && btlLungeUntil[1] < lunge, "a skip pulls the answer forward");
  ck(btlMenu == 0 && battleOpen && !btlOver, "and the fight goes on");

  // --- a move: an effect in its type, the second after the first lands
  startBattle(7, 20);                      // SQUIRTLE: water moves, a different type from ours
  btlMsgCount = 0;
  for (auto &f : btlFx) f.t0 = 0;
  bool typed = false, ordered = false;
  for (int round = 0; round < 30 && !(typed && ordered); round++) {
    if (!battleOpen || btlOver) { startBattle(7, 20); btlMsgCount = 0; }
    for (auto &f : btlFx) f.t0 = 0;
    btlYou.hp = btlYou.maxHp; btlFoe.hp = btlFoe.maxHp; btlMsgCount = 0;
    btlMenu = 1;
    // a damaging move of a type that is not NORMAL (0), so a type left at its
    // default cannot pass for the right one
    uint8_t mv = 0;
    for (uint8_t m = 1; m < MOVE_COUNT && !mv; m++)
      if (MOVE_TBL[m].type == T_ELECTRIC && MOVE_TBL[m].power && MOVE_TBL[m].acc >= 100) mv = m;
    btlYou.moves[0] = mv;
    battleTap(69 + 40, 274 + 22);          // move cell 0, top-left of the grid
    uint32_t t[2] = { 0, 0 }; bool seen[2] = { false, false };
    for (auto &f : btlFx) if (f.t0) {
      if (f.from == 0 && f.type == MOVE_TBL[mv].type) typed = true;
      if (f.from < 2) { seen[f.from] = true; t[f.from] = f.t0; }
    }
    if (seen[0] && seen[1] && t[0] != t[1]) ordered = true;
  }
  ck(typed, "your move's effect is drawn in your move's type");
  ck(ordered, "the second actor's effect waits for the first");

  printf(bad ? "\n%d FAILED\n" : "\nall passed\n", bad);
  return bad ? 1 : 0;
}
