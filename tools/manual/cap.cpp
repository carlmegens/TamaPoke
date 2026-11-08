// Captures the screens for the kids' manual: cap <scene> <lang> <out.ppm>
#include "Arduino.h"
#include "Arduino_GFX_Library.h"
#include "Preferences.h"
#include "pet.h"
#include "party.h"
#include "i18n.h"
#include "battle.h"
#include "dexbox.h"

#include <cstdio>
#include <cstring>
uint32_t g_seed=7; FakeSerial Serial; FakeESP ESP; FakeWire Wire;
volatile int g_touchX=0,g_touchY=0; volatile bool g_touchDown=false;
void FakeESP::restart(){exit(0);}
int FakeSerial::available(){return 0;}
String FakeSerial::readStringUntil(char){return String("");}
void setup(); void loop(); void render();
void onTap(int16_t x, int16_t y);
void openClock(); void openQuiz();
void emuSetSpriteDir(const char *);
void startTrainerBattle(uint8_t idx, bool hard);
extern Arduino_Canvas *gfx; extern Pet pet;
extern bool cardOpen, galleryOpen, clockOpen, kbOpen, menuOpen, partyOpen, trainOpen, gymOpen, gymPick, galleryPick, playerOpen, quizOpen, galleryDirty;
extern uint8_t cardPage, settingsPage, playerPage, btlMenu, galleryRegion;
extern int galleryPage;
extern int16_t galleryDetail, quizAns, quizOpt[3];
extern int8_t quizPicked;
extern uint8_t quizGainShown;
extern uint32_t feedMenuUntil;
extern bool btlNewBadge; extern uint32_t btlWinUntil;
extern uint8_t btlTrainGain, btlTrainWhich;
void openExplore();
bool startWildBattle(int16_t dex, uint8_t lvl, uint8_t place);
void battleTap(int16_t x, int16_t y);
extern bool exploreOpen, expKeepAsk, battleOpen, btlCaught;
extern int8_t expPlace; extern uint8_t expActive, btlMsgCount, partyDetail;
extern uint32_t expActiveUntil, expNextAt, btlBallUntil, btlCatchEnd;
extern Combatant btlFoe;
extern PartyMon expCatch;
// a place's scene with spot 2 frozen mid-stir, so the picture shows what to tap
static void scene(int8_t pl){ openExplore(); expPlace = pl; expNextAt = millis() + 600000; expActive = 2; expActiveUntil = millis() + 600000; }
// a wild CATERPIE in the forest, its opening line already read
static void wild(){ scene(2); startWildBattle(10, 12, 2); while (btlMsgCount) battleTap(233, 233); }
static void caught(){ wild(); for (int k = 0; k < 40 && !btlCaught; k++) { if (btlCatchEnd) { battleTap(233,233); continue; } if (btlMsgCount) { battleTap(233,233); continue; } btlFoe.hp = 1; btlMenu = 0; battleTap(317, 296); } if (btlCatchEnd) battleTap(233, 233); btlBallUntil = 0; }   // a tap skips the catch to its click

static void dump(const char*n){ FILE*f=fopen(n,"wb"); fprintf(f,"P6 466 466 255\n");
  const uint16_t*b=gfx->buffer(); for(int i=0;i<466*466;i++){uint16_t p=b[i];
  unsigned char c[3]={(unsigned char)(((p>>11)&31)*255/31),(unsigned char)(((p>>5)&63)*255/63),(unsigned char)((p&31)*255/31)}; fwrite(c,1,3,f);} fclose(f);}
static void settle(int n=4){ for(int i=0;i<n;i++) loop(); }

int main(int argc, char **argv){
  const char *sc = argv[1]; int lang = atoi(argv[2]); const char *out = argv[3];
  emuSetSpriteDir(argv[4]);
  setup(); settle();
  setLang((Lang)lang);
  bool first = !strcmp(sc,"region") || !strcmp(sc,"starter");
  if (!first) {
    if (pet.awaitingStarter()) pet.chooseStarter(4);
    if (strcmp(sc,"egg") && pet.isEgg()) pet.dbgHatchAs(4,false);
    for (int d : {1, 7, 16, 19, 25, 39, 52, 54, 129, 133}) pet.dbgHatchAs(d,false);
    if (strcmp(sc,"egg")) { pet.dbgHatchAs(4,false); pet.ageMinutes = 11UL*60; pet.relearnFromLevel(); }
    while (pet.hasLearnOffer()) pet.declineLearn();
    pet.renameTrainer("SACHA");
    pet.fullness = 70; pet.joy = 85; pet.energy = 60; pet.hygiene = 90;
  }
  pet.setClock(20757u*86400u + 15*3600u + 1200);    // an afternoon, 31 Oct
  if (strcmp(sc,"halloween")) pet.setClock(20700u*86400u + 15*3600u + 1200);   // an ordinary day
  if (!strcmp(sc,"starter")) onTap(233, 108 + 30);
  if (!strcmp(sc,"egg")) { pet.newEgg(); }
  settle(6);

  if (!strcmp(sc,"feed")) onTap(134, 393);
  else if (!strcmp(sc,"sleep")) { pet.toggleLight(); settle(3); }
  else if (!strcmp(sc,"menu")) menuOpen = true;
  else if (!strcmp(sc,"card0")) { cardOpen = true; cardPage = 0; settle(8); }
  else if (!strcmp(sc,"card1")) { cardOpen = true; cardPage = 1; }
  else if (!strcmp(sc,"card2")) { cardOpen = true; cardPage = 2; }
  else if (!strcmp(sc,"train")) trainOpen = true;
  else if (!strcmp(sc,"player")) { pet.badges = 0x07; pet.streak = 4; playerOpen = true; playerPage = 0; }
  else if (!strcmp(sc,"stickers")) { pet.stickers = (1u<<STK_HALLOWEEN)|(1u<<STK_BIRTHDAY)|(1u<<STK_DEX10)|(1u<<STK_QUIZ10); playerOpen = true; playerPage = GYM_REGIONS + 1; }
  else if (!strcmp(sc,"party")) {
    static const int f[] = { 25, 7, 133 }; static const char *nk[] = { "ZAP", "", "" };
    for (int i = 0; i < 3; i++) { PartyMon m; m.dex = f[i]; m.level = 12 + i * 5; m.ivAtk=m.ivDef=m.ivSpe=m.ivHp=20; snprintf(m.nick,sizeof(m.nick),"%s",nk[i]); party.replaceAt(i,m); }
    partyOpen = true;
  }
  else if (!strcmp(sc,"gympick")) { gymOpen = true; gymPick = true; }
  else if (!strcmp(sc,"gyms")) { gymOpen = true; gymPick = false; }
  else if (!strcmp(sc,"fight")) { startTrainerBattle(0, false); settle(2); }
  else if (!strcmp(sc,"fightmoves")) { startTrainerBattle(0, false); settle(2); btlMenu = 1; }
  else if (!strcmp(sc,"win")) { startTrainerBattle(0, false); btlNewBadge = true; btlWinUntil = millis() + 60000; pet.badges = 0x01; btlTrainGain = 4; btlTrainWhich = 0; }
  else if (!strcmp(sc,"dexpick")) { galleryOpen = true; galleryPick = true; }
  else if (!strcmp(sc,"dex")) { galleryOpen = true; galleryPick = false; galleryRegion = 0; galleryPage = 0; galleryDirty = true; }
  else if (!strcmp(sc,"dexdetail")) { galleryOpen = true; galleryPick = false; galleryRegion = 0; galleryPage = 0; galleryDirty = true; onTap(73 + 3*80 + 40, 84 + 40); settle(2); }
  else if (!strcmp(sc,"quiz")) { galleryOpen = true; openQuiz(); }
  else if (!strcmp(sc,"quizright")) { galleryOpen = true; openQuiz(); for (int i=0;i<3;i++) if (quizOpt[i]==quizAns) onTap(233, 256 + i*52 + 23); }
  else if (!strcmp(sc,"quizdone")) { galleryOpen = true; for (int i=0;i<10;i++) pet.quizAnswered(i<8); openQuiz(); quizGainShown = 4; }
  else if (!strcmp(sc,"clock")) { openClock(); }
  else if (!strcmp(sc,"date")) { openClock(); settingsPage = 1; }
  else if (!strcmp(sc,"options")) { openClock(); settingsPage = 2; }
  else if (!strcmp(sc,"explmap")) { openExplore(); pet.expSpend(); }
  else if (!strcmp(sc,"explscene")) { scene(2); pet.expSpend(); }
  else if (!strcmp(sc,"explocean")) { scene(6); }
  else if (!strcmp(sc,"explvolcano")) { scene(3); }
  else if (!strcmp(sc,"wild")) { wild(); }
  else if (!strcmp(sc,"caught")) { caught(); }
  else if (!strcmp(sc,"keep")) { scene(2); makeMon(10, 14, false, expCatch); PartyMon o; makeMon(10, 9, false, o); dexBoxPut(o); expKeepAsk = true; }
  else if (!strcmp(sc,"dexbox")) { PartyMon o; makeMon(10, 12, false, o); dexBoxPut(o); pet.registerCaught(10, false); for (int i = 0; i < PARTY_SLOTS; i++) party.releaseAt(i); pet.newEgg(); galleryOpen = true; galleryPick = false; galleryDetail = 10; settle(2); }
  else if (!strcmp(sc,"sheetxp")) { PartyMon m; makeMon(25, 9, false, m); party.replaceAt(0, m); party.xp[0] = 51; partyOpen = true; partyDetail = 1; }
  else if (!strcmp(sc,"birthday")) { pet.setBirthday(10, 31); pet.setClock(20757u*86400u + 15*3600u); settle(3); }
  render(); render();
  dump(out);
  return 0;
}
