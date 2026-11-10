// TamaPoke - tamagotchi pixel art inspirado en la gen 1
// para Waveshare ESP32-S3-Touch-AMOLED-1.75
//
// Librerias (Library Manager o repo de Waveshare):
//   - "GFX Library for Arduino" (moononournation), con soporte CO5300 QSPI
//   - "SensorLib" (Lewis He), driver tactil CST9217
//
// Placa: ESP32S3 Dev Module | Flash 16MB | PSRAM: OPI PSRAM | USB CDC On Boot: Enabled
//
// Los sprites y la tabla de especies se generan con tools/sprites.py (emit).

#include <Arduino.h>
#include <Wire.h>
#include "Arduino_GFX_Library.h"
#include "TouchDrvCSTXXX.hpp"
#include "pin_config.h"
#include "species.h"
#include "dex.h"
#include "types.h"
#include "moves.h"
#include "battle.h"
#include "trainers.h"
#include "link.h"
#include "linknow.h"
#include "backs.h"
#include "blit.h"
#include "power.h"
#include "batcurve.h"
#if defined(ESP32)
#include <esp_sleep.h>
#include <driver/gpio.h>
#endif
#include "badges.h"
#include "avatars.h"
#include <stdarg.h>
#include "party.h"
#include "save.h"
#include "pet.h"
#include "dexbox.h"
#include "sdmon.h"
#include "rtcbat.h"
#include "i18n.h"
#include "audio.h"
#include "speech.h"

// Version del firmware. Subir este numero en cada release (y manifest.json para
// el instalador web). Se muestra en la pantalla de ajustes y por serie al arrancar.
#define FW_VERSION "3.33"

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(
  bus, LCD_RESET, 0 /*rotation*/, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
// Framebuffer completo en PSRAM: dibujamos todo y hacemos flush() (sin parpadeo)
#if defined(ESP_PLATFORM)
// The canvas, plus a note of every glyph and button drawn, so speech.cpp knows
// what is on screen (see speech.h). Declared as SpeakCanvas so the non-virtual
// round-rect calls also land here.
class SpeakCanvas : public Arduino_Canvas {
 public:
  using Arduino_Canvas::Arduino_Canvas;
  size_t write(uint8_t c) override {
    speechGlyph(cursor_x, cursor_y, textsize_x, textsize_y, (char)c);
    return Arduino_Canvas::write(c);
  }
  void flush(bool force_flush = false) override {
    speechFrame();
    Arduino_Canvas::flush(force_flush);
  }
  void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t c) {
    speechBox(x, y, w, h);
    Arduino_Canvas::fillRoundRect(x, y, w, h, r, c);
  }
  void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t c) {
    speechBox(x, y, w, h);
    Arduino_Canvas::drawRoundRect(x, y, w, h, r, c);
  }
};
SpeakCanvas *gfx = new SpeakCanvas(LCD_WIDTH, LCD_HEIGHT, panel);
#else
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);  // the emulator's stub calls speech*() itself
#endif

TouchDrvCST92xx touch;
Pet pet;

// sprite animado de la SD para la especie actual (si existe el archivo)
SdMon mon;          // sprite B/N (respaldo y minijuego si no hay PMD)
PmdMon pmd;         // sprite PMD multi-accion (pantalla principal)
PmdMon evoPmd;      // forma anterior, solo durante el parpadeo de evolucion
int16_t monFor = -2;
bool monShinyFor = false;

// comportamiento del bicho en pantalla
struct {
  uint8_t mode = 0;     // 0 idle, 1 paseo, 2 gesto one-shot
  uint8_t act = PMD_IDLE;
  uint32_t t0 = 0;      // inicio de la animacion en curso
  uint32_t until = 0;   // fin del estado actual
  float x = 233, targetX = 233;
} beh;
#define PET_GROUND 304  // linea de suelo de la mascota
PmdMon galleryPmd;  // sprite grande de la vista detalle de la galeria (PMD/TPK2, legal)

// galeria pokedex
bool galleryOpen = false;
bool galleryDirty = false;
// 16 to a page, and the Pokedex is browsed ONE REGION AT A TIME. Three
// generations flat is 25 pages of swiping to reach Hoenn, which is not a
// Pokedex, it is a scroll. A vertical swipe changes region and a horizontal one
// pages within it, so nothing is ever more than ten pages from the front.
// ALL is deliberately not offered here -- it is the thing being replaced.
#define GAL_PER_PAGE 16
#define GAL_REGIONS (REGION_COUNT - 1)          // the real regions, not ALL
#define GAL_CELL 80                             // drawThumb centres in a box this size
#define GAL_LO (REGIONS[galleryRegion % GAL_REGIONS].lo)
#define GAL_HI (REGIONS[galleryRegion % GAL_REGIONS].hi)
#define GAL_SPAN (GAL_HI - GAL_LO + 1)
#define GAL_PAGES ((GAL_SPAN + GAL_PER_PAGE - 1) / GAL_PER_PAGE)
uint8_t galleryRegion = 0;
// Both the Pokedex and the gym ladder now open on a REGION CHOOSER rather than
// dropping you into whichever region was last viewed. The vertical swipe that
// changes region still works, but it is invisible, so on its own it meant the
// Johto and Hoenn content looked absent.
bool galleryPick = false;
bool gymPick = false;
uint8_t rpickPage = 0;      // the region chooser is paged; shared by all 3 modes
int galleryPage = 0;        // GAL_PAGES paginas de GAL_PER_PAGE
int16_t galleryDetail = 0;  // dex en vista detalle, 0 = rejilla

bool screenOff = false;       // pulsacion corta del boton PWR
bool cardOpen = false;        // ficha del bicho (deslizar vertical)
bool kbOpen = false;
enum : uint8_t { KB_PET = 0, KB_TRAINER };
uint8_t kbTarget = KB_PET;          // teclado para renombrar al bicho
char nameBuf[12] = "";
uint8_t nameLen = 0;
#define CARD_PAGES 4   // profile, stats, moves, progress -- medals moved to
                       // the player card, where the totals already live
uint8_t cardPage = 0;         // 0 perfil, 1 stats+medallas
// Menu overlay: opened by tapping the pet's name on the main screen. The
// horizontal swipe is already taken by the Pokedex (it pages through 10 pages
// internally), so a hub on that axis would be ambiguous; the header was inert
// and is the only free surface left.
bool menuOpen = false;
#define MENU_X 73
// Two pages of four square tiles, each an icon and a word, with CLOSE always
// on a row of its own below them and a page indicator between. Bigger targets
// than the old six rows, which matters for a child's finger, and room for the
// two places that used to hide behind other screens: BATTLE (the gym chooser,
// also a swipe left) and TRADE (straight to pairing, no squad, no fight).
// The panel keeps the old one's size: 320 wide, so 160 from the centre, and at
// MENU_Y 75 the round display gives a half-width of 171 against 160.
#define MENU_Y 75
#define MENU_W 320
#define MENU_H 316
#define MENU_TILE_W 135
#define MENU_TILE_H 108
#define MENU_TILE_X(c) (MENU_X + 16 + (c) * (MENU_TILE_W + 14))
#define MENU_TILE_Y(r) (MENU_Y + 14 + (r) * (MENU_TILE_H + 8))
#define MENU_DOTS_Y (MENU_Y + 249)          // centre line of the page dots
#define MENU_CLOSE_X (MENU_X + 16)
#define MENU_CLOSE_Y (MENU_Y + 262)
#define MENU_CLOSE_W (MENU_W - 32)
#define MENU_CLOSE_H 44                     // UI_TAP_MIN
// Each item's index IS its place: page = i / 4, then left-to-right, top-down.
enum : uint8_t { MENU_POKEDEX = 0, MENU_EXPLORE, MENU_BATTLE, MENU_TRADE,
                 MENU_TEAM, MENU_STATS, MENU_SETTINGS, MENU_FAREWELL, MENU_ITEMS };
#define MENU_PER_PAGE 4
#define MENU_PAGES ((MENU_ITEMS + MENU_PER_PAGE - 1) / MENU_PER_PAGE)
#define MENU_HIT_NONE -1      // inside the panel, on nothing
#define MENU_HIT_OUTSIDE -2   // on the dimmed area: closes
#define MENU_HIT_CLOSE -3
#define MENU_HIT_DOT0 -10     // page dot p is MENU_HIT_DOT0 - p
uint8_t menuPage = 0;
bool lanForTrade = false;   // the LAN screen was opened by TRADE: pair, then trade

// Party screen. partyPick != 0 means the newcomer needs a slot: the player
// either taps someone to replace or lets it go.
bool partyOpen = false;
bool partyPick = false;
uint8_t partyDetail = 0;   // 0 = the grid, else slot + 1
// The box, reached from the party screen. `boxSwapFrom` is the party slot
// waiting for something to trade with, 0 when nothing is pending.
bool boxOpen = false;
uint8_t boxPage = 0;
uint8_t boxSwapFrom = 0;   // party slot + 1, armed from the party side
uint8_t boxSel = 0;        // box slot + 1, armed from the box side
// The box gets the same detail sheet the party has. Tapping a box slot used to
// yank the creature into the party immediately, which is a surprising amount to
// happen from one tap -- and left nowhere to put a RELEASE button.
uint8_t boxDetail = 0;        // box slot + 1 whose sheet is open
// The RELEASE confirm, on whichever sheet is up. One flag rather than two,
// because only one sheet can be open at a time and partyDetail/boxDetail
// already say which.
bool releaseConfirm = false;
#define BOX_PER_PAGE 6
uint32_t partyBannerUntil = 0;   // "<name> joined the party!"
// The birthday present on the main screen (Pet::giftReady()), and how long to
// say there is no room for it when party and box are both full.
#define PRESENT_X 50
#define PRESENT_Y 250
#define PRESENT_W 46
#define PRESENT_H 40
uint32_t giftFullUntil = 0;
char partyBannerName[14] = "";
#define PARTY_CELL_W 150
#define PARTY_CELL_H 70
#define PARTY_GRID_X 78
#define PARTY_GRID_Y 88

bool clockOpen = false;       // pantalla de ajuste de hora (deslizar abajo)
// SETTINGS has three pages: the clock, sound and language; the DATE; and
// OPTIONS -- kid mode, the needs speed and the birthday. Paged by a horizontal
// swipe or a tap on the dots, like every other paged screen (and so it is in
// swipe_test). The time and the date are one edit: OK on either page applies
// both, so changing the hour and then the day loses neither.
uint8_t settingsPage = 0;
#define SET_PAGE_TIME 0
#define SET_PAGE_DATE 1
#define SET_PAGE_OPTIONS 2
#define SETTINGS_PAGES 3
#define DATE_YEAR_MIN 2024
#define DATE_YEAR_MAX 2099
uint16_t dateY = DATE_YEAR_MIN;   // the date being edited, read from the clock by openClock()
uint8_t dateM = 1, dateD = 1;
// Kid mode caps the volume here. The cap lives in the sketch, beside the only
// controls that change the volume, rather than in audio.cpp.
#define KID_VOL_MAX 5
uint8_t volMax() { return pet.kidMode ? KID_VOL_MAX : 10; }

// The quiz: a silhouette, its cry, three names. Reached from the Pokedex
// chooser, QUIZ_PER_DAY a day (Pet::quizLeft()).
bool quizOpen = false;
int16_t quizAns = 0;
int16_t quizOpt[3] = { 0, 0, 0 };
int8_t quizPicked = -1;            // -1 = not answered yet
uint32_t quizHearAt = 0;
uint8_t quizGainShown = 0;      // training the day's stars gave, shown until the quiz is left
#define QUIZ_HEAR_GAP_MS 3000UL    // replaying the cry: not faster than this
int clockH = 12, clockM = 0;  // hora en edicion

// escena de bano: espuma sobre el bicho y limpieza al reventar
uint32_t bathUntil = 0;
bool bathPending = false;
struct { int16_t x, y; uint8_t r, ph; } bubbles[14];
uint32_t feedMenuUntil = 0;   // selector de comida abierto hasta este millis

// minijuego "toques": mantener la pokeball en el aire
bool gameOpen = false;
uint32_t gameOverUntil = 0;
// The ball game used to run until you missed three times or walked away, so a
// session had no length and the reward no shape. The bag is 10 s and the
// reaction test 15 s; this sits between them, and whichever comes first --
// the clock or three misses -- ends it.
#define GAME_MS 20000UL
uint32_t gameUntil = 0;
float ballX, ballY, ballVX, ballVY, gamePetX;
uint8_t gameScore, gameMisses;
float hitX, hitY;             // ultimo golpe (anillo de impacto)
uint32_t hitTime = 0;
bool gameNewHi = false;

// saco de entrenamiento (entrena la fuerza)
bool sackOpen = false;
uint32_t sackUntil = 0, sackOverUntil = 0;
uint16_t sackHits = 0;
float sackShake = 0;
uint8_t sackGain = 0;
bool sackNewHi = false;

// training submenu (the 5th icon): routes to the trainer for each stat.
// DEF has no minigame -- it rises on its own from good wellbeing -- so its row
// is informational and does not respond to a tap.
bool trainOpen = false;

// move picker, opened from the MOVES card page. Most learnsets are level 0, so
// a level-up "you learned a move" prompt would almost never fire -- the moveset
// is edited on demand instead.
bool movePickOpen = false;
uint8_t movePickSlot = 0;   // which of the 4 slots is being replaced
uint8_t movePickParty = 0;  // 0 = the live pet, else the party slot + 1
uint8_t movePickPage = 0;
#define MOVE_ROW_Y(i) (96 + (i) * 58)
#define MOVE_PICK_PER_PAGE 5
#define MOVE_PICK_Y(i) (76 + (i) * 58)
// level-up learn prompt: modal, and deliberately without a timeout -- it
// decides what the creature is for the rest of its life, and once banked into
// the party, forever.
#define LEARN_ROW_Y(i) (104 + (i) * 56)
#define LEARN_SKIP_Y 334

// ---------- battle ----------
// The move menu is a 2x2 grid rather than four stacked rows: the round panel
// has to fit both creatures, both HP bars and the menu, and four full-width
// rows do not leave room for the sprites.
bool battleOpen = false;

enum : uint8_t {
  SCR_STARTER = 0, SCR_REGION, SCR_GALLERY, SCR_DEXPICK, SCR_MOVEPICK, SCR_BOX,
  SCR_PARTY, SCR_KEYBOARD, SCR_CARD, SCR_PLAYER, SCR_CLOCK, SCR_GYM, SCR_GYMPICK,
  SCR_LAN, SCR_PICK, SCR_BATTLE, SCR_WIN, SCR_LEARN, SCR_TRAIN, SCR_MENU,
  SCR_GAME, SCR_MAIN, SCR_QUIZ, SCR_EXPLORE, SCR_COUNT
};
extern const char *const SCREEN_NAME[SCR_COUNT];   // const is internal linkage in C++

// Declared HERE, above every use. tools/emu/build.sh generates a proto.h with
// every prototype at the top, so the emulator compiled these happily while
// arduino-cli -- which relies on the IDE's auto-prototyping and does not always
// produce one -- did not. A green emulator build is not proof the firmware
// builds; only arduino-cli is.
void bootReport();
uint8_t uiCurrentScreen();
void renderExplore();
void exploreTap(int16_t x, int16_t y);
void exploreAfterBattle();
void openExplore();
void closeExplore();
void drawBallIcon(int cx, int cy, int r);
bool startWildBattle(int16_t dex, uint8_t lvl, uint8_t place);
const BackScene &placeScene(uint8_t pl, bool night);
extern bool exploreOpen;
extern int8_t expPlace;
extern bool expKeepAsk;
// the quiz (near the top) and OPTIONS draw with these, defined far below
void drawThumb(const uint8_t *b, int x, int y, int s, bool sil);
void drawClockBtn(int x, int y, const char *l);
void drawEventDecor(uint8_t ev, uint32_t now);
void drawPresent(uint32_t now);
void drawSticker(uint8_t s, int cx, int cy, bool got);
void renderPlayerStickers();
// Declared here because renderMonSheet() and the sheet's tap handlers use both
// ~4000 lines above where they are defined. The emulator generates a proto.h and
// would compile it either way; arduino-cli would not, and that has shipped once.
void uiConfirmRects(int *b1Top, int *b1Bot, int *b2Top, int *b2Bot);
void renderParty();
void renderBox();
void drawConfirmPanel(const char *q, const char *sub1, const char *sub2,
                      uint16_t subCol, const char *o1, uint16_t c1, uint16_t t1,
                      const char *o2, uint16_t c2, uint16_t t2);
const char *const SCREEN_NAME[SCR_COUNT] = {
  "starter", "region", "gallery", "dexpick", "movepick", "box",
  "party", "keyboard", "card", "player", "clock", "gym", "gympick",
  "lan", "pick", "battle", "win", "learn", "train", "menu",
  "minigame", "main", "quiz", "explore"
};

// Badge art for a region, or nullptr when that region has none yet.
//
// NEVER `BADGES_ART[region % BADGE_REGIONS]`. The moment GYM_REGIONS outgrew
// BADGE_REGIONS -- which happened when Kalos landed and the upstream badge set
// stops at Unova -- 5 % 5 = 0 silently dressed Kalos in KANTO's badges, on both
// the win screen and the player card. A wrong badge is worse than an honest
// blank: it claims you won something you did not. The callers draw a
// placeholder instead, and static_assert below stops the array being indexed
// out of range whatever the two counts are.
static_assert(BADGE_REGIONS <= GYM_REGIONS,
              "more badge sheets than ladders -- BADGES_ART would be indexed by a region that does not exist");
static const BadgeArt *badgeArtFor(uint8_t region, uint8_t i) {
  if (region >= BADGE_REGIONS || i >= TRAINER_GYMS) return nullptr;
  return &BADGES_ART[region][i];
}

// Does this region have its own badge art? Split out with a primitive return
// type so the tests can ask: the emulator's genproto.py emits every prototype
// ABOVE the includes, where the BadgeArt struct does not exist yet.
bool badgeArtExists(uint8_t region, uint8_t i) { return badgeArtFor(region, i) != nullptr; }

Combatant btlYou, btlFoe;
bool btlOver = false;
bool btlWon = false;
bool btlNewBadge = false;
uint32_t btlWinUntil = 0;   // the win screen is up
// A trainer fight is a run of 1v1s: both sides queue their squad and the next
// one steps up when the current one faints. This is the whole difficulty curve
// -- no gating, just attrition, so one strong creature sweeps Brock and dies
// four deep into Lance.
// The ladder is now sequential: a leader opens once the previous one is beaten,
// tracked per difficulty so hard mode is its own run. This replaces the earlier
// "no gating, attrition is the gate" rule -- with both ladders level-capped,
// nothing stopped you opening with Lance and simply losing, which read as a
// dead end rather than a challenge.
// reaction test (trains SPEED)
bool spdOpen = false;
uint32_t spdUntil = 0, spdOverUntil = 0, spdBorn = 0;
int16_t spdX = 0, spdY = 0;
uint16_t spdHits = 0, spdMisses = 0;
uint8_t spdGain = 0;
bool spdNewHi = false;

bool gymOpen = false;
bool gymHard = false;   // which ladder the list is showing

// LAN battle. `lanOpen` is the pairing screen; once both squads are known the
// normal battle screen takes over with btlLink set.
bool lanOpen = false;
bool tradeOpen = false;          // the trade screen, inside the LAN screen
bool tradePicking = true;        // choosing who to offer, or looking at the deal
uint8_t tradePage = 0;
Link lan;
// What the shared region chooser is being used FOR. It only changes the
// subtitle and whether there is a way back: at first boot every count would
// read zero, which tells the player nothing, and there is nowhere to go back to.
#define RPICK_FOR_GYMS  0
#define RPICK_FOR_DEX   1
#define RPICK_FOR_START 2
// Three rows is what the round panel fits above the BACK label. The dex now
// lists four regions and will list more, so the chooser PAGES rather than
// growing -- and every paged screen in this sketch has to be driven by
// swipe_test, which is why that test exists at all.
#define RPICK_PER_PAGE  3
extern uint8_t rpickPage;
static void renderRegionPick(uint8_t mode);   // the region chooser, defined below
// Not static: swipe_test drives these so it asks the FIRMWARE for the page
// count instead of recomputing it from its own copy of RPICK_PER_PAGE, which
// would prove the transcription rather than the screen.
uint8_t rpickRegions(uint8_t mode);           // rows this mode lists (gyms: 3)
uint8_t rpickPageCount(uint8_t mode);
uint8_t rpickModeNow();                       // which chooser is up, or 0xFF
static bool rpickSwipe(int dir);              // true if it handled the gesture
static int regionPickTap(int16_t x, int16_t y, uint8_t mode);
static void drawEggRegion();          // defined with the egg screen helpers
static int eggRegionTap(int16_t x, int16_t y);
static void drawBtlBack();
static void btlLinkPoll();   // defined with the battle code, called from render()
static void btlSwitchTo(uint8_t i);
static void btlResolve(uint8_t yourMove);
static void btlAfterTurn();
static void btlBegin();
static void btlAwardXp();
// The peer's whole team, kept live. A trainer's replacements are built fresh
// from TRAINERS[] because they only ever arrive once; a linked opponent can
// switch OUT and back IN, so its creatures have to remember how battered they
// are. Host side only -- the guest takes absolute health off the wire.
Combatant btlFoeSquad[TRAINER_TEAM_MAX];
uint8_t btlFoeSquadN = 0;
uint8_t btlMyAct = 0;        // host: our own action, latched until theirs lands
// Which ladder the gym screen and the current fight belong to. The battle keeps
// its own copy so that leaving the gym list mid-fight cannot retarget the badge.
// The BOX button on the party screen. It was 150x32 with a pixel-exact hit
// test, which is a small target on a round panel -- reported as hard to press,
// same complaint as the battle grid. Now bigger AND padded: the drawn size grew
// too, so the button looks like the size it actually is rather than hiding a
// generous hit area behind a small graphic.
// The smallest a button may be. Three separate "hard to hit" reports -- the
// battle grid's bottom row, the party screen's BOX, and the LAN button -- were
// all the same mistake: a control sized to fit its label rather than a finger.
// 44 px is the usual guidance and roughly a fingertip on this 466 px panel.
#define UI_TAP_MIN 44

// The LAN battle button on the gym region chooser.
#define LANBTN_W 190
#define LANBTN_H UI_TAP_MIN
#define LANBTN_X (233 - LANBTN_W / 2)
#define LANBTN_Y 336

#define BOXBTN_X 146
#define BOXBTN_Y 320
#define BOXBTN_W 174
#define BOXBTN_H UI_TAP_MIN
#define BOXBTN_PAD 8
// CLOSE sits below BOX with a real gap between them. They used to touch at
// y=372, and BOX's padding then reached to 380 -- so the top of CLOSE was
// inside BOX's hit area and taps there opened the box instead of closing the
// screen. Padding one button into its neighbour just moves the problem along.
#define PARTYCLOSE_Y 376
#define PARTYCLOSE_H UI_TAP_MIN
#define PARTYCLOSE_X 133
#define PARTYCLOSE_W 200

// The confirm panel, in ONE place. Three callers draw it -- the evolve/farewell/
// retire dialog on the main screen, and letting a banked creature go from the
// party or box sheet -- and hit_test asserts against the same numbers through
// uiConfirmRects(). Copies of this geometry in the tap handler are exactly how
// a YES button ends up somewhere the drawing is not.
#define CONFIRM_X 73
#define CONFIRM_Y 156
#define CONFIRM_W 320
#define CONFIRM_H 188
#define CONFIRM_BTN_X 93
#define CONFIRM_BTN_W 280
#define CONFIRM_BTN_H 52
#define CONFIRM_B1_Y 206
#define CONFIRM_B2_Y 268

// The two buttons on the party/box detail sheet. A third full-width row would
// not fit above BACK, and BRING BACK was h=38 -- under UI_TAP_MIN, the shape
// section 4 of CLAUDE.md keeps warning about.
//
// THEY ARE NOT EQUAL HALVES, and that is the whole point. The first version
// centred the pair on 233 with a dead gap between them -- which put the gap at
// the CENTRE OF THE PANEL, the one place a thumb naturally lands, so BRING BACK
// "did not work" and was reported as broken. Worse, every build up to v3.5 drew
// BRING BACK as a FULL-WIDTH button centred exactly there, so muscle memory
// aimed straight at the dead zone.
//
// The primary action now owns the centre. RELEASE is narrower, offset right,
// and still over UI_TAP_MIN -- it is irreversible, so it should be reachable
// but never the thing you hit by aiming at the middle. The gap between them is
// wider than before, not narrower.
#define PDET_BTN_Y 336
#define PDET_BTN_H 48
#define PDET_L_X 70
#define PDET_L_W 180
#define PDET_R_X 266
#define PDET_R_W 120
static_assert(PDET_L_X < 233 && 233 < PDET_L_X + PDET_L_W,
              "the panel centre must land on the sheet's PRIMARY button, not between the two");
static_assert(PDET_R_X > PDET_L_X + PDET_L_W + 8,
              "the destructive button needs a real dead gap before it");

uint8_t gymRegion = 0;
uint8_t btlRegion = 0;
#define TRAINERS (TRAINER_SETS[gymRegion % GYM_REGIONS].list)
#define BTL_TRAINERS (TRAINER_SETS[btlRegion % GYM_REGIONS].list)
bool gShowAllAvatars = false;  // emulator screenshot aid, never set on hardware
bool btlPetIn = false;       // was the live pet in the squad?
uint8_t btlTrainGain = 0;    // what the win trained, for the win screen
uint8_t btlTrainWhich = 0;
bool btlLink = false;      // this fight is against another device
bool btlLinkHost = false;
static bool gymUnlocked(uint8_t idx, bool hard) {
  return idx == 0 || pet.hasBadge(gymRegion, idx - 1, hard);
}

// Team select. Candidate 0 is the live pet, 1..PARTY_SLOTS are the banked
// members, so one bitmask covers the whole pool.
bool pickOpen = false;
// The team picker serves the gym ladder and the LAN screen both. PICK_LAN is
// not a trainer index: squadCap() already returns an uncapped six for anything
// past the roster, which is what a LAN battle wants -- two players who know
// each other can bring what they like.
#define PICK_LAN 0xFF
static void lanOffer(bool host);
uint8_t pickTrainer = 0;
bool pickHard = false;
bool lanWantHost = true;   // which button opened the picker
uint16_t squadMask = 0xFFFF;   // everything, until the player says otherwise
uint8_t pickPage = 0;
#define PICK_PER_PAGE 6
#define PICK_CELL_W 150
#define PICK_CELL_H 74
#define PICK_X(i) (78 + ((i) % 2) * (PICK_CELL_W + 10))
#define PICK_Y(i) (86 + ((i) / 2) * (PICK_CELL_H + 6))
#define PICK_GO_Y 350
// BACK beside FIGHT on the team-select screen. There was no way out of it but a
// swipe, which is invisible -- the same complaint as everywhere else.
#define PICK_BTN_W 155
#define PICK_BTN_H UI_TAP_MIN
#define PICK_BACK_X (233 - PICK_BTN_W - 7)
#define PICK_GO_X (233 + 7)
bool playerOpen = false;
// One badge page per gym region, then the medals. Three ladders will not fit on
// one page, and the page you are on IS the region -- no extra control needed,
// and horizontal paging already works everywhere else.
uint8_t playerPage = 0;
#define PLAYER_PAGES (GYM_REGIONS + 2)   // badges per region, medals, stickers
#define playerBadgeRegion (playerPage % GYM_REGIONS)
uint8_t gymPage = 0;
#define GYM_ROWS 5
#define GYM_ROW_Y(i) (110 + (i) * 50)
int8_t btlTrainer = -1;      // index into TRAINERS, -1 = a one-off fight
// A wild fight met while exploring: drawn in its place, with a BALL beside
// FIGHT. btlWildMon is the creature itself, built whole before the fight, so
// the one that is caught is exactly the one that was fought.
bool btlWild = false;
uint8_t btlWildPlace = 0;
PartyMon btlWildMon;
bool btlCaught = false;      // set by the ball; the explore screen keeps it
uint32_t btlBallUntil = 0;   // the caught ball rocks until then
// Experience: which party slot each squad member came from (-1 = the live
// creature, which grows by age instead), who has been out at least once, and
// which foe has already paid out, so a knockout counts once.
int8_t btlSquadSlot[TRAINER_TEAM_MAX];
uint8_t btlFought = 0;
uint8_t btlXpPaid = 0;       // btlFoeAt + 1 of the last foe that paid
bool btlHard = false;
Combatant btlSquad[TRAINER_TEAM_MAX + 1];
uint8_t btlSquadN = 0, btlSquadAt = 0;
uint8_t btlFoeAt = 0;

// Animation. Deliberately built on the thumbnails the screen already draws
// rather than on PmdMon: three PmdMon blobs are live already, and the battle
// has to stay graceful on a board with no SD at all (S_NO_SPRITES). Index 0 is
// you, 1 is the foe.
uint32_t btlLungeUntil[2] = { 0, 0 };   // acted: leans toward the opponent
uint32_t btlHitUntil[2] = { 0, 0 };     // was hit: jitters and flashes
uint16_t btlHpShown[2] = { 0, 0 };      // bars ease toward the real value
// Two streamed sprites, so the creatures can actually swing and flinch. They
// cost ~135 KB of PSRAM each on average and are freed when the fight ends. The
// player's side is NOT the global `pmd`: the active creature may be a banked
// party member rather than the live pet.
PmdMon btlPmd[2];
int16_t btlPmdDex[2] = { 0, 0 };
// A faint used to swap the next creature in instantly, inside the same call
// that resolved the turn -- which is why it felt like a jump cut. The swap is
// now deferred: the fainted one drops out of frame, and the replacement slides
// in only once the player dismisses that message.
uint32_t btlFaintUntil[2] = { 0, 0 };
uint32_t btlEnterUntil[2] = { 0, 0 };
int8_t btlSwapWho = -1;        // 0 = your side, 1 = the foe's, -1 = nothing due
#define BTL_FAINT_MS 700
#define BTL_ENTER_MS 420
// battle menu: 0 = FIGHT/POKEMON, 1 = the moves, 2 = the switch list
uint8_t btlMenu = 0;
#define BTL_LUNGE_MS 260
#define BTL_HIT_MS 420
char btlMsg[6][40];
uint8_t btlMsgCount = 0;   // queued lines; a tap shows the next

// ---------- battle effects ----------
// Purely visual. A turn is resolved the moment it is chosen; these only SHOW
// what was decided -- a projectile in the move's type, an impact, stars on a
// critical hit, arrows for a stat change, and the catch: the ball thrown, the
// creature drawn in, the ball wobbling, then a click or a burst. Nothing here
// changes the fight, and a tap during the catch skips straight to its end, so
// an impatient child is never stuck watching.
#define FX_TRAVEL_MS 340
#define FX_IMPACT_MS 280
#define FX_SLOTS 4
enum : uint8_t { FX_HIT = 1, FX_CRIT = 2, FX_SUPER = 4, FX_STATUS = 8 };
struct BtlFx { uint32_t t0 = 0; uint8_t from = 0, type = 0, flags = 0; int8_t stage = 0; };
BtlFx btlFx[FX_SLOTS];
uint32_t btlShakeUntil = 0;     // a super-effective hit rattles its target
uint8_t btlShakeWho = 1;
// where each side's creature is, roughly its middle (see btlSide's anchors)
static const int16_t FX_X[2] = { 100, 324 }, FX_Y[2] = { 206, 78 };

// The catch: thrown, drawn in, dropped, wobbled 1-3 times, then a click (it
// holds) or a burst (it escapes). The outcome was rolled when the ball left.
#define CATCH_THROW_MS 450
#define CATCH_OPEN_MS 250
#define CATCH_DROP_MS 200
#define CATCH_SHAKE_MS 600
#define CATCH_END_MS 450
#define CATCH_BALL_X 324
#define CATCH_BALL_Y 100
uint32_t btlCatchT0 = 0, btlCatchEnd = 0;   // btlCatchEnd 0 = no catch animating
uint8_t btlCatchShakes = 0;
bool btlCatchOk = false;

static void btlFxClear() {
  for (auto &f : btlFx) f.t0 = 0;
  btlShakeUntil = 0;
  btlCatchEnd = 0;
}

// One move's effect, from side `from` toward the other, starting at `t0`.
static void btlFxMove(uint8_t from, const TurnLog &lg, uint32_t t0) {
  if (lg.skipped || lg.hurtSelf || lg.charged || !lg.move || lg.move >= MOVE_COUNT) return;
  BtlFx *slot = &btlFx[0];
  uint32_t now = millis();
  for (auto &f : btlFx)
    if (!f.t0 || now > f.t0 + FX_TRAVEL_MS + FX_IMPACT_MS) { slot = &f; break; }
  slot->t0 = t0;
  slot->from = from;
  slot->type = MOVE_TBL[lg.move].type;
  slot->stage = lg.stageDelta;
  bool landed = lg.damage && !lg.missed && !lg.immune;
  // a miss still flies, then puffs out; a move with no damage is a status show
  slot->flags = landed ? FX_HIT : (lg.missed || lg.immune) ? 0 : FX_STATUS;
  if (landed && lg.crit) slot->flags |= FX_CRIT;
  if (landed && lg.effPct > 100) {
    slot->flags |= FX_SUPER;
    btlShakeUntil = t0 + FX_TRAVEL_MS + 320;
    btlShakeWho = 1 - from;
  }
}

static void btlCatchStart(bool ok, uint8_t shakes) {
  btlCatchOk = ok;
  btlCatchShakes = ok ? 3 : shakes;
  btlCatchT0 = millis();
  btlCatchEnd = btlCatchT0 + CATCH_THROW_MS + CATCH_OPEN_MS + CATCH_DROP_MS +
                btlCatchShakes * CATCH_SHAKE_MS + CATCH_END_MS;
}

// The click (or the burst) has happened: only now does a catch sound like one.
// Skipped early, the foe's answer (timed for after the ball) is pulled forward
// by the time saved, so it plays now rather than over the narration later.
static void btlCatchFinish() {
  if (!btlCatchEnd) return;
  uint32_t now = millis();
  if (now < btlCatchEnd) {
    uint32_t early = btlCatchEnd - now;
    for (auto &f : btlFx) if (f.t0 > now) f.t0 -= early;
    for (int i = 0; i < 2; i++) {
      if (btlLungeUntil[i] > now) btlLungeUntil[i] -= early;
      if (btlHitUntil[i] > now) btlHitUntil[i] -= early;
    }
    if (btlShakeUntil > now) btlShakeUntil -= early;
  }
  btlCatchEnd = 0;
  if (btlCatchOk) {
    btlBallUntil = millis() + 1500;
    audioMusic(MUS_VICTORY);
    sfxPlay(SFX_VICTORY);
  }
}

// While it animates the creature is inside the ball: from the flash that
// draws it in until the end, unless it is bursting back out.
static bool btlCatchHidesFoe(uint32_t now) {
  if (!btlCatchEnd || now >= btlCatchEnd) return false;
  uint32_t e = now - btlCatchT0;
  if (e < CATCH_THROW_MS + CATCH_OPEN_MS / 2) return false;
  if (!btlCatchOk && now + CATCH_END_MS / 2 >= btlCatchEnd) return false;   // out again
  return true;
}

// two tones per type: the effect's body and its highlight
static const uint16_t FX_COL[18][2] = {
  { 0xFFFF, 0xC618 }, { 0xFC60, 0xFFE0 }, { 0x3D9F, 0xAEFF }, { 0xFFE0, 0xFFFF },
  { 0x4E89, 0xA7E8 }, { 0xAFFF, 0xFFFF }, { 0xFA60, 0xFFFF }, { 0xA11F, 0xD35F },
  { 0xC4C8, 0x8A85 }, { 0xDEFF, 0xFFFF }, { 0xFB1F, 0xFDDF }, { 0x9E64, 0xCF2A },
  { 0xAAA6, 0x7A04 }, { 0x7A1F, 0x3809 }, { 0x681F, 0xFC60 }, { 0x4208, 0x8410 },
  { 0xBDF7, 0xFFFF }, { 0xFDBA, 0xFFFF },
};

static void fxStar(int x, int y, int r, uint16_t c) {
  gfx->fillRect(x - r, y - 1, 2 * r + 1, 3, c);
  gfx->fillRect(x - 1, y - r, 3, 2 * r + 1, c);
  gfx->fillRect(x - r / 2, y - r / 2, r + 1, r + 1, c);
}

static void fxLeaf(int x, int y, float a, uint16_t c) {
  int dx = (int)(12 * cosf(a)), dy = (int)(12 * sinf(a));
  int px = (int)(5 * -sinf(a)), py = (int)(5 * cosf(a));
  gfx->fillTriangle(x - dx, y - dy, x + dx, y + dy, x + px, y + py, c);
  gfx->fillTriangle(x - dx, y - dy, x + dx, y + dy, x - px, y - py, c);
}

// Arrows rising off a creature whose stat went up, or sinking through one
// whose stat went down.
static void fxStageArrows(int side, bool up, float q) {
  uint16_t c = up ? 0xFFE0 : 0x3D9F;     // yellow rising, blue sinking: both read on any creature
  for (int i = 0; i < 3; i++) {
    int x = FX_X[side] - 34 + i * 34;
    int y = FX_Y[side] + (up ? 34 - (int)(q * 80) : -44 + (int)(q * 80)) + (i % 2) * 14;
    for (int o = 1; o >= 0; o--) {          // a white outline, then the arrow
      uint16_t k = o ? UI_WHITE : c;
      if (up) gfx->fillTriangle(x, y - 14 - o * 2, x - 11 - o * 2, y + o, x + 11 + o * 2, y + o, k);
      else gfx->fillTriangle(x, y + 14 + o * 2, x - 11 - o * 2, y - o, x + 11 + o * 2, y - o, k);
      gfx->fillRect(x - 4 - o, up ? y : y - 13, 9 + o * 2, 13 + o, k);
    }
  }
}

// By slot, not by reference: arduino-cli hoists a prototype of every function
// to the top of the sketch, above BtlFx, and a BtlFx in the signature fails there.
static void btlDrawMoveFx(uint8_t slot, uint32_t now) {
  const BtlFx &f = btlFx[slot];
  if (!f.t0 || now < f.t0) return;
  uint32_t e = now - f.t0;
  if (e >= FX_TRAVEL_MS + FX_IMPACT_MS) return;
  int to = 1 - f.from;
  int x0 = FX_X[f.from], y0 = FX_Y[f.from], x1 = FX_X[to], y1 = FX_Y[to];
  uint8_t t = f.type < 18 ? f.type : 0;
  uint16_t c1 = FX_COL[t][0], c2 = FX_COL[t][1];

  if (f.flags & FX_STATUS) {                // no damage: a stat change, or a sparkle
    float q = (float)e / (FX_TRAVEL_MS + FX_IMPACT_MS);
    if (f.stage > 0) fxStageArrows(f.from, true, q);
    else if (f.stage < 0) fxStageArrows(to, false, q);
    else
      for (int i = 0; i < 6; i++) {
        float a = q * 6.28f + i * 1.047f;
        fxStar(x1 + (int)(34 * cosf(a)), y1 + (int)(26 * sinf(a)), 3, c1);
      }
    return;
  }

  if (e < FX_TRAVEL_MS) {                   // on its way
    float p = (float)e / FX_TRAVEL_MS;
    float arc = (t == T_WATER || t == T_GRASS || t == T_BUG) ? 40.0f : 14.0f;
    int x = x0 + (int)((x1 - x0) * p), y = y0 + (int)((y1 - y0) * p) - (int)(arc * sinf(p * 3.1416f));
    switch (t) {
      case T_ELECTRIC: {                    // a bolt, re-struck every 40 ms
        int px = x0, py = y0;
        for (int s = 1; s <= 6; s++) {
          int nx = x0 + (x - x0) * s / 6, ny = y0 + (y - y0) * s / 6;
          if (s < 6) { nx += (int)((((e / 40) * 7 + s * 13) % 21) - 10); ny += (int)((((e / 40) * 5 + s * 11) % 21) - 10); }
          gfx->drawLine(px, py, nx, ny, c1);
          gfx->drawLine(px + 1, py, nx + 1, ny, c2);
          gfx->drawLine(px, py + 1, nx, ny + 1, c1);
          px = nx; py = ny;
        }
        break;
      }
      case T_FIRE: case T_DRAGON:           // a fireball with a trail
        for (int k = 3; k >= 1; k--) {
          float pk = p - k * 0.07f;
          if (pk < 0) continue;
          gfx->fillCircle(x0 + (int)((x1 - x0) * pk), y0 + (int)((y1 - y0) * pk) - (int)(arc * sinf(pk * 3.1416f)), 13 - k * 3, c2);
        }
        gfx->fillCircle(x, y, 16, c1);
        gfx->fillCircle(x - 3, y - 3, 7, c2);
        break;
      case T_WATER:                         // a stream of droplets
        for (int k = 0; k < 4; k++) {
          float pk = p - k * 0.08f;
          if (pk < 0) continue;
          int dx = x0 + (int)((x1 - x0) * pk), dy = y0 + (int)((y1 - y0) * pk) - (int)(arc * sinf(pk * 3.1416f));
          gfx->fillCircle(dx, dy, 10 - k * 2, c1);
          gfx->fillCircle(dx - 2, dy - 3, 3, c2);
        }
        break;
      case T_GRASS: case T_BUG:             // spinning leaves
        for (int k = 0; k < 3; k++)
          fxLeaf(x + (int)(18 * cosf(e * 0.02f + k * 2.1f)), y + (int)(18 * sinf(e * 0.02f + k * 2.1f)),
                 e * 0.03f + k, k ? c1 : c2);
        break;
      case T_ICE:                           // shards
        for (int k = 0; k < 3; k++) {
          int sx = x + (k - 1) * 18, sy = y + (k % 2) * 10;
          gfx->fillTriangle(sx, sy - 15, sx - 6, sy + 7, sx + 6, sy + 7, k == 1 ? c2 : c1);
        }
        break;
      case T_ROCK: case T_GROUND: {         // rocks drop onto the target
        int fall = (int)(p * 110);
        for (int k = 0; k < 3; k++)
          gfx->fillCircle(x1 - 30 + k * 30, y1 - 110 + fall - k * 10, 10 + (k % 2) * 3, k == 1 ? c2 : c1);
        break;
      }
      case T_FLYING:                        // gusts
        for (int k = 0; k < 3; k++) {
          int gy = y + (k - 1) * 14;
          gfx->fillRect(x - 24 + k * 6, gy, 36, 3, c1);
          gfx->drawFastHLine(x - 18 + k * 6, gy + 3, 24, c2);
        }
        break;
      case T_NORMAL: case T_FIGHTING: case T_STEEL:
        break;                              // the lunge IS the attack
      default:                              // psychic, ghost, dark, poison, fairy: an orb
        gfx->fillCircle(x, y, 12, c1);
        gfx->drawCircle(x, y, 17 + (int)(e / 30) % 5, c2);
        gfx->drawCircle(x, y, 18 + (int)(e / 30) % 5, c2);
        break;
    }
    return;
  }

  float q = (float)(e - FX_TRAVEL_MS) / FX_IMPACT_MS;   // the impact
  if (!(f.flags & FX_HIT)) {                // missed: a puff of nothing
    gfx->drawCircle(x1, y1, 6 + (int)(q * 14), 0x8410);
    return;
  }
  int r = 16 + (int)(q * 42);
  for (int i = 0; i < 8; i++) {
    float a = i * 0.785f;
    int ax = x1 + (int)(r * 0.5f * cosf(a)), ay = y1 + (int)(r * 0.5f * sinf(a));
    int bx = x1 + (int)(r * cosf(a)), by = y1 + (int)(r * sinf(a));
    gfx->drawLine(ax, ay, bx, by, c1);
    gfx->drawLine(ax + 1, ay, bx + 1, by, c1);
    gfx->drawLine(ax, ay + 1, bx, by + 1, c1);
  }
  gfx->drawCircle(x1, y1, (int)(q * 34), c2);
  gfx->drawCircle(x1, y1, (int)(q * 34) + 1, c2);
  if (t == T_POISON || t == T_GHOST)        // bubbles rising off the target
    for (int k = 0; k < 4; k++) gfx->drawCircle(x1 - 20 + k * 14, y1 - (int)(q * 40) - k * 6, 4, c1);
  if (f.flags & FX_CRIT)
    for (int i = 0; i < 5; i++) {
      float a = i * 1.2566f + q * 2;
      fxStar(x1 + (int)((24 + q * 36) * cosf(a)), y1 + (int)((24 + q * 36) * sinf(a)), 7, 0xFFE0);
    }
}

static void btlDrawCatch(uint32_t now) {
  if (!btlCatchEnd) return;
  uint32_t e = now - btlCatchT0;
  int bx = CATCH_BALL_X, by = CATCH_BALL_Y;
  if (e < CATCH_THROW_MS) {                 // thrown in an arc from your side
    float p = (float)e / CATCH_THROW_MS;
    int x = 110 + (int)((CATCH_BALL_X - 110) * p);
    int y = 300 + (int)((70 - 300) * p) - (int)(70 * sinf(p * 3.1416f));
    drawBallIcon(x, y, 10 + (int)(4 * p));
    return;
  }
  e -= CATCH_THROW_MS;
  if (e < CATCH_OPEN_MS) {                  // it opens: a flash draws the creature in
    float q = (float)e / CATCH_OPEN_MS;
    int r = 10 + (int)(q * 46);
    gfx->drawCircle(bx, 70, r, UI_WHITE);
    gfx->drawCircle(bx, 70, r - 2, 0xFFE0);
    for (int i = 0; i < 6; i++) {
      float a = i * 1.047f + q;
      gfx->drawLine(bx, 70, bx + (int)(r * cosf(a)), 70 + (int)(r * sinf(a)), UI_WHITE);
    }
    drawBallIcon(bx, 70, 14);
    return;
  }
  e -= CATCH_OPEN_MS;
  if (e < CATCH_DROP_MS) {                  // and drops to the ground
    drawBallIcon(bx, 70 + (int)((by - 70) * e / CATCH_DROP_MS), 14);
    return;
  }
  e -= CATCH_DROP_MS;
  if (e < (uint32_t)btlCatchShakes * CATCH_SHAKE_MS) {   // the wobbles
    uint32_t s = e % CATCH_SHAKE_MS;
    int off = s < 300 ? (int)(8 * sinf(s * 3.1416f / 150)) : 0;
    drawBallIcon(bx + off, by + (off ? 1 : 0), 14);
    return;
  }
  float q = (float)(now + CATCH_END_MS - btlCatchEnd) / CATCH_END_MS;
  if (q > 1) q = 1;
  if (btlCatchOk) {                         // click: it holds
    drawBallIcon(bx, by, 14);
    for (int i = 0; i < 4; i++) {
      float a = i * 1.5708f + q * 1.5f;
      fxStar(bx + (int)((18 + q * 20) * cosf(a)), by - 10 + (int)((10 + q * 14) * sinf(a)), 4, UI_BAR_WARN);
    }
  } else {                                  // burst: it got out
    gfx->fillCircle(bx, by - 20, 8 + (int)(q * 36), UI_WHITE);
  }
}

static void btlDrawFx(uint32_t now) {
  for (uint8_t i = 0; i < FX_SLOTS; i++) btlDrawMoveFx(i, now);
  btlDrawCatch(now);
}

#define BTL_CELL_W 160
#define BTL_CELL_H 44
#define BTL_GRID_X 69
#define BTL_GRID_Y 274
#define BTL_CELL_X(i) (BTL_GRID_X + ((i) % 2) * (BTL_CELL_W + 8))
#define BTL_CELL_Y(i) (BTL_GRID_Y + ((i) / 2) * (BTL_CELL_H + 8))

// A cell's HIT area is bigger than the cell that is drawn. On the board the two
// bottom buttons were much harder to hit than the top two: the drawn cells are
// only 44 px tall, there was an 8 px dead gap between the rows, and everything
// below the bottom row was dead too -- so a finger landing low, or a touch panel
// reading a few pixels high, missed entirely. Nothing else is tappable in this
// area while the grid is open, so the slop costs nothing.
//
// The gap between the two rows and the two columns is split down the middle, and
// the bottom row additionally claims the empty space beneath it.
#define BTL_HIT_PAD 4
// The bottom row keeps extra room downward, but not so much that it reaches the
// BACK bar below it -- the mistake made once already with BOX and CLOSE.
#define BTL_HIT_BOTTOM 6
// BACK, under the grid: the move and switch screens had no way out except
// choosing something.
// The gym list's EASY/HARD pill. It was 24 px tall, which is half a fingertip.
// Sits between the title and the first leader row. At 72 with a 44 px height it
// ran to 116 and overlapped the first row, which begins at 110 -- introduced
// when the pill was enlarged to a real tap target.
#define GYMDIF_Y 60
#define GYMDIF_H UI_TAP_MIN
#define BTL_BACK_W 190
#define BTL_BACK_H UI_TAP_MIN
#define BTL_BACK_X (233 - BTL_BACK_W / 2)
#define BTL_BACK_Y 384
#define BTL_HIT_X0(i) (BTL_CELL_X(i) - BTL_HIT_PAD)
// The far edges stop one pixel short so the four boxes TILE: the gap between
// two cells is split down the middle with no pixel left over and none shared.
#define BTL_HIT_X1(i) (BTL_CELL_X(i) + BTL_CELL_W + BTL_HIT_PAD - 1)
#define BTL_HIT_Y0(i) (BTL_CELL_Y(i) - BTL_HIT_PAD)
#define BTL_HIT_Y1(i) (BTL_CELL_Y(i) + BTL_CELL_H - 1 + \
                       ((i) / 2 ? BTL_HIT_BOTTOM : BTL_HIT_PAD))

// Which cell a point falls in, or -1. Exposed (not static) so a test can sweep
// the panel and prove there are no dead pixels between the cells -- the bug that
// made the bottom row hard to press was a gap, not a wrong rectangle.
int btlCellIndexAt(int16_t x, int16_t y);

// Used by BOTH the move grid and the switch grid. They had a copy each of the
// same rectangle test, which is exactly how two halves of one control drift.
static inline bool btlCellHit(int i, int16_t x, int16_t y) {
  return x >= BTL_HIT_X0(i) && x <= BTL_HIT_X1(i) &&
         y >= BTL_HIT_Y0(i) && y <= BTL_HIT_Y1(i);
}
#define TRAIN_X 73
#define TRAIN_Y 96
#define TRAIN_W 320
#define TRAIN_H 274
#define TRAIN_ROW_H 56
#define TRAIN_ROW_GAP 8
#define TRAIN_ROW_Y(i) (TRAIN_Y + 54 + (i) * (TRAIN_ROW_H + TRAIN_ROW_GAP))

// las 9 especies con sprite propio en flash (respaldo sin SD): dex -> indice
int flashIdxForDex(int16_t dex) {
  static const int8_t IDX[10] = { -1, 3, 4, 5, 0, 1, 2, 6, 7, 8 };
  return (dex >= 1 && dex <= 9) ? IDX[dex] : -1;
}

#define CX 233  // centro de la pantalla redonda
#define CY 233
#define PET_CY 202  // centro vertical del sprite

static const uint16_t INK_K = 0x18C4;  // spriteColor('k')

// botones de icono siguiendo el arco inferior de la pantalla redonda
// (los exteriores van mas altos para no salirse del circulo)
struct Btn {
  int16_t cx, cy;
  const char *const *icon;
};
// Five across the arc: spacing tightened 62 -> 54 so the outer pair stays far
// enough in to keep its old y. Lifting them instead would have run the row into
// the ENE/HYG bars, which end at y=361 -- the buttons are 52 tall, so any centre
// above 387 overlaps them.
// Four, not five. The ball had its own icon here until it became DEFENCE's
// trainer and moved into the training menu -- at which point tapping it just
// opened the same menu the dumbbell does, two icons for one destination.
// They sit on the panel's curve: y = 406 - dx^2/729.
#define BTN_COUNT 4
// Referred to by NAME, never by literal index. Removing the ball icon shifted
// every index by one and drawButtons() still had `i != 2` meaning LIGHT -- which
// silently made the BATH button the one that wakes the pet.
#define BTN_FOOD  0
#define BTN_LIGHT 1
#define BTN_BATH  2
#define BTN_TRAIN 3
Btn buttons[BTN_COUNT] = {
  { 134, 393, SPR_ICON_FOOD },   // comer
  { 200, 405, SPR_ICON_LIGHT },  // luz
  { 266, 405, SPR_ICON_CLEAN },  // bano
  { 332, 393, SPR_ICON_TRAIN },  // entrenar
};
#define BTN_HALF 30  // boton de 60x60 -- mas grande y mas separado que antes
#define BTN_HIT 40   // radio tactil (un poco mas generoso)

// grietas del huevo (pixeles 'k' sobre el sprite)
static const uint8_t CRACK1[][2] = { {15,8},{16,9},{15,10} };
static const uint8_t CRACK2[][2] = { {11,13},{12,14},{11,15},{20,12},{19,13},{20,14} };
// estrellas del modo noche
static const uint16_t STARS[][2] = { {120,140},{330,120},{370,210},{95,230},{280,90},{160,95} };

bool wasPressed = false;
// eleccion de inicial (primera partida): Bulbasaur / Charmander / Squirtle, 3 filas
// The first-boot starter list is the FRONT of each region's starter array in
// dex.h -- not a copy of it. That array is also the pool a region's first egg
// is drawn from (pet.cpp rollInRegion), where Kanto's five deliberately include
// Pikachu and Eevee; the choice screen shows the canonical three and leaves the
// rest to the egg. starter_test pins the first three of every region, so
// reordering that array cannot silently change the first screen anyone sees.
#define STARTER_SHOWN 3
int16_t starterOf(uint8_t region, uint8_t i) {
  const RegionInfo &rg = REGIONS[region % REGION_COUNT];
  if (i >= rg.starterCount) i = 0;
  return rg.starters[i];
}
uint8_t starterCountShown(uint8_t region) {
  uint8_t n = REGIONS[region % REGION_COUNT].starterCount;
  return n < STARTER_SHOWN ? n : STARTER_SHOWN;
}

// First boot runs region -> starter. This is NOT persisted: a reset between the
// two lands back on the region, which is the harmless direction to fail in --
// nothing has been chosen yet, and pet.setRegion() is idempotent.
static bool starterRegionDone = false;
#define STARTER_ROW_Y 110
#define STARTER_ROW_H 70
#define STARTER_ROW_GAP 8
// boton-CTA de evolucion (centrado, mitad de pantalla)
#define EVO_BTN_W 256
#define EVO_BTN_H 64
#define EVO_BTN_X (CX - EVO_BTN_W / 2)
#define EVO_BTN_Y 172
// boton-CTA de despedida (mas ancho: lleva el nombre + frase)
#define FAR_BTN_W 408
#define FAR_BTN_H 58
#define FAR_BTN_X (CX - FAR_BTN_W / 2)
#define FAR_BTN_Y 176
// el CST9217 avisa por el pin INT cuando hay datos tactiles; lo usamos para no
// leer el bus I2C mientras el chip esta dormido (esa lectura se colgaba ~1s)
volatile bool gTouchIrq = false;
void IRAM_ATTR touchIsr() { gTouchIrq = true; }
uint32_t lastRender = 0;
// proteccion del AMOLED: atenuado por inactividad
uint32_t lastInteract = 0;
uint8_t dimStage = 0;        // 0 despierto, 1 atenuado (90s), 2 casi apagado (5min)
bool swallowGesture = false; // el toque que despierta no acciona nada
uint32_t holdStart = 0;     // pulsacion larga sobre el bicho
uint32_t confirmUntil = 0;  // dialogo "soltar?" activo hasta este millis
uint8_t choiceKind = 0;     // dialogo de decision: 0 ninguno, 1 evolucion, 2 despedida
uint32_t choiceUntil = 0;   // se cierra solo a este millis
int16_t tX0, tY0, tXl, tYl; // gesto en curso (inicio y ultima posicion)
uint32_t tStart = 0;
bool holdFired = false;

void setup() {
  Serial.setRxBufferSize(8192);  // la transferencia a SD llega en bloques de 2 KB
  Serial.begin(115200);
  // CRITICO: sin esto, Serial.print BLOQUEA el juego cuando no hay un
  // monitor serie abierto en el host (el bufer TX del USB CDC se llena
  // y nadie lo vacia) -> con timeout 0 los mensajes se descartan
  Serial.setTxTimeoutMs(0);
  Serial.printf("TamaPoke fw v%s\n", FW_VERSION);
  bootReport();   // why the last run ended, and what it was doing
  loadLang();  // idioma guardado (ES por defecto)
  Wire.begin(IIC_SDA, IIC_SCL);
  // CST9217 (tactil), AXP2101 (PMU) y PCF85063 (RTC) comparten este bus I2C.
  // Red de seguridad para PMU/RTC (SensorLib NO respeta este timeout en el
  // tactil; el cuelgue del tactil dormido se resuelve gateando por INT, ver
  // handleTouch).
  Wire.setTimeOut(50);

  // CRITICO: encender la alimentacion del panel (BLDO1=OLED VDD 3.3V) ANTES de
  // inicializar el display. Si el PMU se reseteo (drenaje total), este rail
  // queda OFF y la pantalla se ve negra aunque el resto de la placa funcione.
  pmuEnablePanel();

  // QSPI a 80MHz (por defecto 40): el flush del framebuffer es el cuello de
  // botella del fps (~56ms a 40MHz). Si el panel mostrara basura, bajar a 40M.
  if (!gfx->begin(80000000)) Serial.println("gfx->begin() fallo");
  panel->setBrightness(180);

  touch.setPins(TP_RESET, TP_INT);
  bool touchOk = false;
  for (int i = 0; i < 3 && !touchOk; i++) {  // a veces falla al primer intento
    touchOk = touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL);
    if (!touchOk) delay(150);
  }
  if (!touchOk) Serial.println("CST9217 no detectado");
  // begin() deja el chip en modo comando (lee la identidad y no sale);
  // hace falta un reset por hardware para que vuelva a reportar toques
  touch.reset();
  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  touch.setMirrorXY(true, true);  // el panel esta montado girado 180 grados
  // INT activo-bajo: salta cuando hay datos. Gatea las lecturas I2C (ver loop)
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TP_INT), touchIsr, FALLING);

  party.begin();
  pet.begin();
  dexBoxRepair();
  sdBegin();
  thumbs.load();

  // reloj real: aplica el tiempo que estuvo apagado
  rtcBegin();
  batBegin();
  pwrSetup();
  uint32_t e = rtcEpoch();
  if (e == 0) {
    rtcSetEpoch(1767225600UL);  // RTC virgen: semilla (la hora absoluta da igual,
    e = rtcEpoch();             // solo importan las diferencias)
    Serial.println("RTC sin hora: sembrado, sin progresion offline esta vez");
  }
  pet.syncClock(e);

  audioBegin();  // ES8311 + I2S + amplificador (suena un jingle de arranque)

  lastInteract = millis();
}

// carga/descarga el sprite de SD cuando cambia la especie
void ensureMon() {
  if (pet.speciesId == monFor && monShinyFor == pet.shiny && !sdDirty) return;
  sdDirty = false;
  monFor = pet.speciesId;
  monShinyFor = pet.shiny;
  mon.unload();
  pmd.unload();
  beh.x = beh.targetX = 233;
  beh.mode = 0;
  beh.until = 0;
  if (pet.speciesId >= 1 && pet.speciesId <= DEX_COUNT) {
    pmd.load(pet.speciesId, pet.shiny);          // principal: PMD
    if (!pmd.loaded) mon.load(pet.speciesId, pet.shiny);  // respaldo: B/N
  }
}

// ---------- cries ----------
// When a creature makes itself heard. Watched from here, off what the pet's
// STATE did since the last frame, rather than called from inside pet.cpp:
// pet.cpp links into every test binary, and an edge seen here cannot disagree
// with what actually happened -- a hatch is the egg going away, however it went.
//
// Kept rare on purpose -- this is a toy a child will hold. The events cry once
// each; a caress only sometimes, and never twice inside CRY_CARESS_GAP_MS.
#define CRY_CARESS_ODDS 6            // one caress in six...
#define CRY_CARESS_GAP_MS 60000UL    // ...and at most once a minute
static uint32_t lastCaressCry = 0;

// A creature's own voice where the card has it, else the cue that stood in for
// it -- never both. A blip and then a cry on one tap is two sounds, and the
// blip was only ever there because there was no voice.
void cryOr(int16_t dex, uint8_t sfx) {
  if (!cryPlay(dex)) sfxPlay(sfx);
}

bool cryOnCaress(uint32_t now) {
  if (lastCaressCry && now - lastCaressCry < CRY_CARESS_GAP_MS) return false;
  if (random(CRY_CARESS_ODDS)) return false;
  lastCaressCry = now ? now : 1;     // 0 means "never"
  return true;
}

// ---------- calendar decorations, the present, stickers ----------
// Drawn from primitives -- there is no art for any of this, and none is
// needed. Particles are a fixed set placed by index and moved by the clock, so
// they animate without any state and the screenshot tests stay deterministic.
#define C_ORANGE 0xFC00
#define C_PINK 0xFB56
#define C_YELLOW 0xFFE0
#define C_RED 0xF800
#define C_GREEN 0x2589
#define C_PURPLE 0x801F
#define C_SKY 0x5D9F
#define C_BROWN 0xA285
static void drawHeart(int x, int y, int r, uint16_t c) {
  gfx->fillCircle(x - r / 2, y, r / 2 + 1, c);
  gfx->fillCircle(x + r / 2, y, r / 2 + 1, c);
  gfx->fillTriangle(x - r - 1, y + 1, x + r + 1, y + 1, x, y + r + 2, c);
}
static void drawPumpkin(int x, int y, int r) {
  gfx->fillCircle(x, y, r, C_ORANGE);
  gfx->fillRect(x - 1, y - r - 4, 3, 5, C_GREEN);
  gfx->fillTriangle(x - r / 2 - 2, y - 2, x - r / 2 + 2, y - 2, x - r / 2, y - 6, 0x0000);
  gfx->fillTriangle(x + r / 2 - 2, y - 2, x + r / 2 + 2, y - 2, x + r / 2, y - 6, 0x0000);
  gfx->drawFastHLine(x - r / 2, y + r / 3, r, 0x0000);
}
// a particle's position: x across the scene, y falling (dir 1) or rising (-1)
static void particleAt(int i, uint32_t now, int dir, int *px, int *py) {
  int speed = 30 + (i % 5) * 12;
  int span = 200;
  int t = (int)((now / speed + (uint32_t)i * 53) % span);
  *px = 40 + (i * 97 + 31) % 386 + (int)((t / 20) % 3) - 1;
  *py = dir > 0 ? 100 + t : 300 - t;
}

void drawEventDecor(uint8_t ev, uint32_t now) {
  int x, y;
  switch (ev) {
    case EV_NEWYEAR:            // fireworks, three bursts out of step
      for (int k = 0; k < 3; k++) {
        int cx = 110 + k * 125, cy = 130 + (k % 2) * 30;
        int r = (int)((now / 25 + k * 40) % 60);
        uint16_t c = k == 0 ? C_YELLOW : k == 1 ? C_PINK : C_SKY;
        for (int a = 0; a < 8; a++) {
          float an = a * 0.785f;
          gfx->fillCircle(cx + (int)(cosf(an) * r), cy + (int)(sinf(an) * r), 2, c);
        }
      }
      break;
    case EV_VALENTINE:          // hearts drifting up
      for (int i = 0; i < 8; i++) { particleAt(i, now, -1, &x, &y); drawHeart(x, y, 7, C_PINK); }
      break;
    case EV_POKEDAY:            // balloons on strings
      for (int i = 0; i < 6; i++) {
        particleAt(i, now, -1, &x, &y);
        gfx->drawFastVLine(x, y + 10, 16, 0x8410);
        gfx->fillCircle(x, y, 9, i % 2 ? C_RED : C_YELLOW);
      }
      break;
    case EV_SPRING:             // petals
      for (int i = 0; i < 14; i++) { particleAt(i, now, 1, &x, &y); gfx->fillCircle(x, y, 3, C_PINK); }
      break;
    case EV_SUMMER: {           // a big sun, top left of the scene
      int cx = 92, cy = 130;
      for (int a = 0; a < 12; a++) {
        float an = a * 0.5236f + (now % 6000) / 6000.0f * 0.5236f;
        gfx->drawLine(cx + (int)(cosf(an) * 24), cy + (int)(sinf(an) * 24),
                      cx + (int)(cosf(an) * 34), cy + (int)(sinf(an) * 34), C_YELLOW);
      }
      gfx->fillCircle(cx, cy, 20, C_YELLOW);
      break;
    }
    case EV_AUTUMN:             // leaves
      for (int i = 0; i < 12; i++) {
        particleAt(i, now, 1, &x, &y);
        gfx->fillTriangle(x - 4, y, x + 4, y, x, y + 7, i % 2 ? C_ORANGE : C_BROWN);
      }
      break;
    case EV_HALLOWEEN:          // pumpkins either side of the creature
      drawPumpkin(70, 282, 14);
      drawPumpkin(396, 282, 14);
      drawPumpkin(372, 290, 9);
      break;
    case EV_CHRISTMAS:          // a tree, then the snow
      gfx->fillTriangle(396, 236, 376, 270, 416, 270, C_GREEN);
      gfx->fillTriangle(396, 252, 372, 292, 420, 292, C_GREEN);
      gfx->fillRect(393, 292, 7, 8, C_BROWN);
      gfx->fillCircle(396, 236, 4, C_YELLOW);
      // fall through: Christmas has snow too
    case EV_WINTER:
      for (int i = 0; i < 16; i++) { particleAt(i, now, 1, &x, &y); gfx->fillCircle(x, y, 2, 0xFFFF); }
      break;
    case EV_BIRTHDAY:           // confetti and a cake
      for (int i = 0; i < 16; i++) {
        particleAt(i, now, 1, &x, &y);
        const uint16_t cc[4] = { C_PINK, C_YELLOW, C_SKY, C_GREEN };
        gfx->fillRect(x, y, 4, 4, cc[i % 4]);
      }
      gfx->fillRoundRect(374, 270, 44, 26, 5, 0xFFFF);
      gfx->fillRect(374, 278, 44, 5, C_PINK);
      gfx->fillRect(395, 258, 3, 12, C_RED);
      gfx->fillCircle(396, 254, 3, ((now / 200) % 2) ? C_YELLOW : C_ORANGE);
      break;
    default:
      break;
  }
}

// a wrapped box with a bow, bobbing so it reads as something to touch
void drawPresent(uint32_t now) {
  int x = PRESENT_X, y = PRESENT_Y + (int)((now / 300) % 2) * 2;
  gfx->fillRoundRect(x, y, PRESENT_W, PRESENT_H, 5, C_RED);
  gfx->drawRoundRect(x, y, PRESENT_W, PRESENT_H, 5, UI_INK);
  gfx->fillRect(x + PRESENT_W / 2 - 4, y, 8, PRESENT_H, C_YELLOW);
  gfx->fillRect(x, y + 10, PRESENT_W, 7, C_YELLOW);
  gfx->fillTriangle(x + PRESENT_W / 2, y + 2, x + PRESENT_W / 2 - 14, y - 10, x + PRESENT_W / 2 - 12, y + 4, C_YELLOW);
  gfx->fillTriangle(x + PRESENT_W / 2, y + 2, x + PRESENT_W / 2 + 14, y - 10, x + PRESENT_W / 2 + 12, y + 4, C_YELLOW);
}

// One sticker, centred on cx, cy. A missing one is a grey disc: the shape of
// the collection is visible from the start, which is half of wanting it.
void drawSticker(uint8_t s, int cx, int cy, bool got) {
  const int r = 22;
  if (!got) {
    gfx->fillCircle(cx, cy, r, UI_TRACK);
    gfx->drawCircle(cx, cy, r, 0x9492);
    return;
  }
  static const uint16_t BG[STK_COUNT] = {
    0x10A6, C_PINK, C_YELLOW, C_GREEN, C_SKY, C_BROWN, C_PURPLE, 0xAEDF, C_RED, 0xFE19,
    C_RED, C_RED, C_RED, C_RED, 0x4C98, 0x4C98, 0xFEA0,
  };
  gfx->fillCircle(cx, cy, r, BG[s]);
  gfx->drawCircle(cx, cy, r, UI_INK);
  switch (s) {
    case STK_NEWYEAR:
      for (int a = 0; a < 8; a++) {
        float an = a * 0.785f;
        gfx->drawLine(cx + (int)(cosf(an) * 5), cy + (int)(sinf(an) * 5),
                      cx + (int)(cosf(an) * 15), cy + (int)(sinf(an) * 15), C_YELLOW);
      }
      break;
    case STK_VALENTINE: drawHeart(cx, cy - 3, 12, 0xFFFF); break;
    case STK_POKEDAY:           // a lightning bolt
      gfx->fillTriangle(cx + 2, cy - 15, cx - 9, cy + 2, cx + 1, cy + 1, 0x0000);
      gfx->fillTriangle(cx - 2, cy - 1, cx + 8, cy - 2, cx - 3, cy + 15, 0x0000);
      break;
    case STK_SPRING:
      for (int a = 0; a < 5; a++) {
        float an = a * 1.2566f;
        gfx->fillCircle(cx + (int)(cosf(an) * 8), cy + (int)(sinf(an) * 8), 6, C_PINK);
      }
      gfx->fillCircle(cx, cy, 5, C_YELLOW);
      break;
    case STK_SUMMER:
      for (int a = 0; a < 8; a++) {
        float an = a * 0.785f;
        gfx->drawLine(cx + (int)(cosf(an) * 11), cy + (int)(sinf(an) * 11),
                      cx + (int)(cosf(an) * 17), cy + (int)(sinf(an) * 17), C_YELLOW);
      }
      gfx->fillCircle(cx, cy, 9, C_YELLOW);
      break;
    case STK_AUTUMN:
      gfx->fillTriangle(cx - 11, cy - 4, cx + 11, cy - 4, cx, cy + 14, C_ORANGE);
      gfx->fillTriangle(cx - 8, cy - 4, cx + 8, cy - 4, cx, cy - 15, C_ORANGE);
      gfx->drawLine(cx, cy - 10, cx, cy + 16, C_BROWN);
      break;
    case STK_HALLOWEEN: drawPumpkin(cx, cy + 2, 12); break;
    case STK_WINTER:
      for (int a = 0; a < 3; a++) {
        float an = a * 1.0472f;
        int dx = (int)(cosf(an) * 15), dy = (int)(sinf(an) * 15);
        gfx->drawLine(cx - dx, cy - dy, cx + dx, cy + dy, 0xFFFF);
      }
      break;
    case STK_CHRISTMAS:
      gfx->fillTriangle(cx, cy - 15, cx - 12, cy + 8, cx + 12, cy + 8, C_GREEN);
      gfx->fillRect(cx - 2, cy + 8, 5, 6, C_BROWN);
      gfx->fillCircle(cx, cy - 15, 3, C_YELLOW);
      break;
    case STK_BIRTHDAY:
      gfx->fillRoundRect(cx - 12, cy - 2, 24, 14, 3, 0xFFFF);
      gfx->fillRect(cx - 12, cy + 3, 24, 3, C_PINK);
      gfx->fillRect(cx - 1, cy - 11, 3, 9, C_RED);
      gfx->fillCircle(cx, cy - 13, 2, C_YELLOW);
      break;
    case STK_SHINY:             // a star
      gfx->fillTriangle(cx, cy - 15, cx - 13, cy + 8, cx + 13, cy + 8, 0xFFFF);
      gfx->fillTriangle(cx, cy + 14, cx - 13, cy - 5, cx + 13, cy - 5, 0xFFFF);
      break;
    default: {                  // the milestones: their number, or a "?" for the quiz
      const char *t = s == STK_DEX10 ? "10" : s == STK_DEX50 ? "50" : s == STK_DEX100 ? "100"
                    : s == STK_DEX200 ? "200" : "?";
      int ts = strlen(t) > 2 ? 1 : 2;
      if (s == STK_QUIZ10 || s == STK_QUIZ50) ts = 3;
      gfx->setTextColor(0xFFFF);
      gfx->setTextSize(ts);
      gfx->setCursor(cx - (int)strlen(t) * 3 * ts, cy - 4 * ts);
      gfx->print(t);
      break;
    }
  }
}

#define STK_COLS 6
#define STK_DX 62
#define STK_ROW_Y(r) (112 + (r) * 84)
void renderPlayerStickers() {
  char head[32];
  snprintf(head, sizeof(head), "%s %u/%u", T(S_STICKERS), pet.stickerCount(), STK_COUNT);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(head) * 6, 44);
  gfx->print(head);
  for (uint8_t s = 0; s < STK_COUNT; s++) {
    int row = s / STK_COLS, col = s % STK_COLS;
    int inRow = (row == (STK_COUNT - 1) / STK_COLS) ? (STK_COUNT - row * STK_COLS) : STK_COLS;
    int cx = CX - (inRow - 1) * STK_DX / 2 + col * STK_DX;
    int cy = STK_ROW_Y(row);
    bool got = pet.hasSticker(s);
    drawSticker(s, cx, cy, got);
    const char *nm = stickerName(s);
    gfx->setTextColor(got ? UI_INK : 0x9492);
    gfx->setTextSize(1);
    gfx->setCursor(cx - (int)strlen(nm) * 3, cy + 28);
    gfx->print(nm);
  }
}

// A five-pointed star, filled: five triangles from the centre.
void drawStar5(int cx, int cy, int r, uint16_t col) {
  const float ri = r * 0.42f;
  for (int k = 0; k < 5; k++) {
    float a = -1.5708f + k * 1.2566f;            // each outer point, from the top
    float b1 = a - 0.6283f, b2 = a + 0.6283f;    // the inner corners either side
    int ox = cx + (int)(r * cosf(a)), oy = cy + (int)(r * sinf(a));
    gfx->fillTriangle(ox, oy, cx + (int)(ri * cosf(b1)), cy + (int)(ri * sinf(b1)),
                      cx + (int)(ri * cosf(b2)), cy + (int)(ri * sinf(b2)), col);
    gfx->fillTriangle(cx, cy, cx + (int)(ri * cosf(b1)), cy + (int)(ri * sinf(b1)),
                      cx + (int)(ri * cosf(b2)), cy + (int)(ri * sinf(b2)), col);
  }
}

// ---------- quiz ----------
// A random species of `r` that can be drawn. Rejection sampling, bounded: every
// region is mostly art, and a miss falls back to the region's first species.
static int16_t quizSpecies(uint8_t r) {
  const RegionInfo &rg = REGIONS[r % GAL_REGIONS];
  for (int t = 0; t < 64; t++) {
    int16_t d = (int16_t)(rg.lo + random(rg.hi - rg.lo + 1));
    if (speciesHasArt(d)) return d;
  }
  return (int16_t)rg.lo;
}

// One question. All three names come from ONE region whose pack is on the
// card, so the options are of an era and every one of them can be drawn.
void quizNew() {
  uint8_t regs[GAL_REGIONS], nr = 0;
  for (uint8_t r = 0; r < GAL_REGIONS; r++)
    if (regionAvailable(r)) regs[nr++] = r;
  uint8_t r = nr ? regs[random(nr)] : 0;
  quizAns = quizSpecies(r);
  uint8_t at = (uint8_t)random(3);
  for (uint8_t i = 0; i < 3; i++) {
    if (i == at) { quizOpt[i] = quizAns; continue; }
    int16_t d = quizAns;
    for (int t = 0; t < 32; t++) {
      d = quizSpecies(r);
      bool dup = (d == quizAns);
      for (uint8_t j = 0; j < i; j++) dup = dup || (d == quizOpt[j]);
      if (!dup) break;
    }
    quizOpt[i] = d;
  }
  quizPicked = -1;
  cryOr(quizAns, SFX_TAP);
  quizHearAt = millis();
}

void openQuiz() {
  quizOpen = true;
  quizPicked = -1;
  quizGainShown = 0;
  if (pet.quizLeft()) quizNew();     // its cry is the tap's sound
  else sfxPlay(SFX_TAP);             // none left: the screen says come back tomorrow
}

#define QUIZ_SIL_Y 150                // centre of the silhouette
#define QUIZ_ROW_X 98
#define QUIZ_ROW_W 270
#define QUIZ_ROW_H 46
#define QUIZ_ROW_Y(i) (256 + (i) * 52)
void renderQuiz() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  if (!pet.quizLeft() && quizPicked < 0) {
    // the day's score, up to three stars, and what the stars trained
    char sc[32];
    snprintf(sc, sizeof(sc), T(S_QUIZ_TODAY_FMT), pet.quizRightToday(), (unsigned)QUIZ_PER_DAY);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(3);
    gfx->setCursor(CX - (int)strlen(sc) * 9, 110);
    gfx->print(sc);
    uint8_t stars = pet.quizStars();
    for (int i = 0; i < 3; i++)
      drawStar5(CX - 70 + i * 70, 190, 28, i < stars ? C_YELLOW : UI_TRACK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (int)strlen(T(S_QUIZ_DONE)) * 6, 256);
    gfx->print(T(S_QUIZ_DONE));
    if (quizGainShown) {
      static const StrId NAMES[3] = { S_TR_ATK, S_TR_DEF, S_TR_SPE };
      char l[32];
      snprintf(l, sizeof(l), T(S_WIN_TRAIN_FMT), T(NAMES[pet.quizGainWhich % 3]), quizGainShown);
      gfx->setTextColor(UI_BAR_OK);
      gfx->setCursor(CX - (int)strlen(l) * 6, 296);
      gfx->print(l);
    }
    gfx->flush();
    return;
  }
  char head[40];
  uint16_t hc = UI_INK;
  if (quizPicked < 0) snprintf(head, sizeof(head), "%s", T(S_QUIZ_Q));
  else if (quizOpt[quizPicked] == quizAns) { snprintf(head, sizeof(head), "%s", T(S_QUIZ_RIGHT)); hc = UI_BAR_OK; }
  else { snprintf(head, sizeof(head), T(S_QUIZ_WRONG), dexName(quizAns)); hc = UI_BAR_BAD; }
  gfx->setTextColor(hc);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(head) * 6, 40);
  gfx->print(head);

  char left[32];
  snprintf(left, sizeof(left), T(S_QUIZ_LEFT), pet.quizLeft());
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(1);
  gfx->setCursor(CX - (int)strlen(left) * 3, 64);
  gfx->print(left);

  // the shadow until it is answered, then the creature itself
  const uint8_t *th = thumbs.get(quizAns);
  if (th) drawThumb(th, CX - GAL_CELL / 2, QUIZ_SIL_Y - GAL_CELL / 2, 4, quizPicked < 0);
  else {
    gfx->setTextSize(6);
    gfx->setCursor(CX - 18, QUIZ_SIL_Y - 24);
    gfx->print("?");
  }
  const char *hint = quizPicked < 0 ? T(S_QUIZ_HEAR) : T(S_QUIZ_NEXT);
  gfx->setTextSize(1);
  gfx->setCursor(CX - (int)strlen(hint) * 3, 236);
  gfx->print(hint);

  for (uint8_t i = 0; i < 3; i++) {
    int y = QUIZ_ROW_Y(i);
    uint16_t bg = UI_WHITE;
    if (quizPicked >= 0) {
      if (quizOpt[i] == quizAns) bg = UI_BAR_OK;            // the right one, always shown
      else if (i == quizPicked) bg = UI_BAR_BAD;            // the wrong pick
      else bg = UI_TRACK;
    }
    gfx->fillRoundRect(QUIZ_ROW_X, y, QUIZ_ROW_W, QUIZ_ROW_H, 12, bg);
    gfx->drawRoundRect(QUIZ_ROW_X, y, QUIZ_ROW_W, QUIZ_ROW_H, 12, UI_INK);
    const char *nm = dexName(quizOpt[i]);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (int)strlen(nm) * 6, y + 15);
    gfx->print(nm);
  }
  gfx->flush();
}

void quizTap(int16_t x, int16_t y) {
  if (!pet.quizLeft() && quizPicked < 0) { quizOpen = false; return; }   // done for today
  if (quizPicked >= 0) {                 // answered: any tap moves on
    if (pet.quizLeft()) quizNew();       // the next cry is the tap's sound
    else { sfxPlay(SFX_TAP); quizPicked = -1; }   // that was the last one: the day's score
    return;
  }
  for (uint8_t i = 0; i < 3; i++) {
    int ry = QUIZ_ROW_Y(i);
    if (x < QUIZ_ROW_X || x > QUIZ_ROW_X + QUIZ_ROW_W || y < ry - 3 || y > ry + QUIZ_ROW_H + 3) continue;
    quizPicked = (int8_t)i;
    bool right = (quizOpt[i] == quizAns);
    pet.quizAnswered(right);             // spends one of today's, cheers the pet if right
    if (pet.quizGain) quizGainShown = pet.quizGain;   // the day's stars just paid out
    sfxPlay(right ? SFX_MEDAL : SFX_TAP);  // a wrong answer is not scolded
    return;
  }
  // the silhouette: hear it again, but not on every frantic tap
  if (y >= 80 && y <= 240 && millis() - quizHearAt > QUIZ_HEAR_GAP_MS) {
    cryPlay(quizAns);
    quizHearAt = millis();
  }
}

void cryWatch() {
  static bool seen = false, wasEgg = true, wasEvolving = false;
  static uint8_t wasCeremony = CER_NONE;
  bool egg = pet.isEgg(), evo = pet.evolving();
  uint8_t cer = pet.ceremony;
  // the first frame only records: booting is not a hatch. And nothing cries at
  // a dark screen -- an evolution finishing in a pocket at night stays quiet.
  if (seen && !screenOff) {
    if (wasEgg && !egg) cryPlay(pet.speciesId);               // hatched, or brought back
    else if (wasEvolving && !evo && !egg) cryPlay(pet.speciesId);  // the new form, once revealed
    if (cer != CER_NONE && wasCeremony == CER_NONE) cryPlay(pet.speciesId);  // leaving
  }
  seen = true;
  wasEgg = egg;
  wasEvolving = evo;
  wasCeremony = cer;
}

void loop() {
  uint32_t now = millis();
  pet.update(now);
  cryWatch();

  // The link is pumped here rather than from the LAN screen, because it has to
  // keep running through the battle too: linkNowPoll() drains what the radio
  // parked on the WiFi task, and tick() is what resends a lost packet and gives
  // up on a peer that has gone quiet.
  if (lan.live()) {
    linkNowPoll();
    lan.tick(now);
    tradePoll();       // a struck deal finishes whatever screen is up
  }
  // Opened by the menu's TRADE: the moment the pair is made, go to the trade
  // screen. Once only -- leaving it lands on the LAN screen, still paired.
  if (lanForTrade && lanOpen && !tradeOpen && lan.state == LINK_READY) {
    lanForTrade = false;
    tradeOpenScreen();
  }

  // avisa con un sonido cuando el bicho pasa a estar listo para evolucionar
  // (incluye el caso de cumplir al despertar). canEvolveNow es false durmiendo.
  static bool wasEvoReady = false;
  bool evoReady = pet.wantEvolveButton();
  if (evoReady && !wasEvoReady) sfxPlay(SFX_MEDAL);
  wasEvoReady = evoReady;
  // aviso sombrio cuando el bicho esta a punto de escaparse por abandono
  static bool wasRunReady = false;
  bool runReady = pet.canRunawayNow();
  if (runReady && !wasRunReady) sfxPlay(SFX_DENY);
  wasRunReady = runReady;

  handleTouch();
  handleSerial();
  ensureMon();

  // A sprite pack can arrive long after the card was mounted: the web installer
  // streams it over PUT into the FIRMWARE THAT IS ALREADY RUNNING. gRegionArt was
  // computed once in sdBegin(), so without this the region a player just spent ten
  // minutes downloading stayed greyed out reading NEEDS PACK until they rebooted --
  // indistinguishable, from the player's side, from the download having failed.
  // Quietly, because the host is still parsing this serial stream.
  if (sdArtDirty) {
    sdArtDirty = false;
    sdScanRegionArt(false);
  }

  // A farewell or release just finished: the creature is waiting for a slot.
  // With room it simply joins; with a full party the player is taken straight
  // to the party screen to choose who it replaces, or to let it go.
  if (pet.endedKind != CER_NONE && !partyPick) {
    // party first, then the box; only a full box makes it your choice
    if (party.add(pet.endedMon) || party.boxAdd(pet.endedMon)) {
      snprintf(partyBannerName, sizeof(partyBannerName), "%s",
               pet.endedMon.nick[0] ? pet.endedMon.nick : dexName(pet.endedMon.dex));
      partyBannerUntil = now + 3500;
      pet.endedKind = CER_NONE;
      sfxPlay(SFX_MEDAL);
    } else {
      partyPick = true;
      partyOpen = true;
      menuOpen = false;
    }
  }

  // The PWR button: a short press toggles the screen; held 2 s it switches the
  // TamaPoke off, the game saved first. The PMU's own cut needs 4 s at least
  // (4/6/8/10 is all the AXP2101 offers), which is long for a child, so the
  // firmware does it at 2 s and the PMU's 4 s stays as the backstop.
  static uint32_t lastPwr = 0;
  if (now - lastPwr > 250) {
    lastPwr = now;
    uint8_t pwr = pwrPoll();
    if (pwr == PWR_SHORT) {
      screenOff = !screenOff;
      pet.setScreenOff(screenOff);   // asleep only if it is also night
      if (!screenOff) wakeUI();
    } else if (pwr == PWR_LONG) {
      Serial.println("PWR held: saving and powering off");
      pet.saveNow();
      panel->setBrightness(0);
      batPowerOff();
    }
  }

  updateBrightness(now);

  // The battery at its safe limit, with no USB: save the game and power the PMU
  // off before the cell goes lower. The RTC keeps the time. (batcurve.h)
  if (batShouldShutdown() && !usbPresent()) {
    pet.flushSave();
    panel->setBrightness(0);
    batPowerOff();
  }

  // vuelca el autoguardado periodico SOLO con la pantalla atenuada/apagada o
  // durmiendo: la escritura a NVS congela ~1s ambos cores (caché de flash off),
  // y aqui no hay animacion que se corte ni dedo esperando respuesta. Con 90s
  // de inactividad la pantalla ya atenua, asi que se vuelca enseguida; el uso
  // activo persiste igual por los guardados de cada accion (comer/jugar/...).
  if (pet.savePending() && (screenOff || dimStage >= 1 || pet.sleeping)) {
    pet.flushSave();
  }

  // anota la hora real cada 30 s (se persiste en cada save del juego)
  static uint32_t lastClock = 0;
  if (now - lastClock > 30000) {
    lastClock = now;
    uint32_t e = rtcEpoch();
    if (e) pet.lastSeenEpoch = e;
  }

  // latido de salud cada 5 min (para el soak test; se descarta si no hay monitor)
  static uint32_t lastHealth = 0;
  if (now - lastHealth > 300000) {
    lastHealth = now;
    Serial.printf("HEALTH up=%lus heap=%u min=%u\n", (unsigned long)(now / 1000),
                  ESP.getFreeHeap(), ESP.getMinFreeHeap());
  }

  // How often to draw comes from power.h: 10 fps in use, slower the longer it
  // is left alone, 85 ms in a minigame, and NOTHING with the screen off -- at
  // brightness 0 nothing shows, yet the full redraw and the 434 KB push over
  // QSPI used to carry on at 10 fps, the largest avoidable load on the board.
  // lastRender stays put, so the first frame after waking comes at once.
  PowerCtx pc = powerCtx(now);
  batSetPollMs(powerCacheMs(pc));
  uint32_t interval = frameIntervalMs(pc);
  bool drewFrame = false;
  if (interval && now - lastRender >= interval) {
    lastRender = now;
    render();
    drewFrame = true;
  }
  // yield on passes that draw nothing: the loop used to spin flat out between
  // frames. Touch is read every 20 ms and sound has its own task.
  if (!drewFrame) idleWait(pc);
}

// What the power policy needs to know right now (power.h).
PowerCtx powerCtx(uint32_t now) {
  PowerCtx c;
  c.idleMs = idleSince(now, lastInteract);
  c.screenOff = screenOff;
  c.sleeping = pet.sleeping;
  c.gameOpen = gameOpen || sackOpen || spdOpen;
  c.usb = usbPresent();
  c.charging = batCharging();
  c.batPct = batPercent();
  return c;
}

// The player did something: back to the normal frame rate at once, rather
// than after the next frame of a slow one. A forced frame still keeps 85 ms
// from the last.
void wakeUI() {
  uint32_t now = millis();
  uint32_t iv = frameIntervalMs(powerCtx(now));
  bool slow = !iv || iv > 100;
  lastInteract = now;
  if (slow) lastRender = (now - lastRender >= 85) ? 0 : lastRender - 15;
}

// The pause on a pass that drew nothing. With the screen off, or almost off,
// and no USB, the CPU light-sleeps until the next frame or 200 ms; a touch
// (the touch INT going low) wakes it. Never with the radio link up -- ESP-NOW
// needs the radio awake -- and never while a sound plays. USB is read uncached
// first, in case it was just plugged in.
void idleWait(PowerCtx c) {
#if defined(ESP32)
  if (lightSleepAllowed(c) && !lan.live() && !audioActive() && millis() - lastRender >= 100) {
    c.usb = usbPresentNow();
    if (!lightSleepAllowed(c)) { delay(idleDelayMs(c)); return; }
    if (digitalRead(TP_INT) == LOW) { gTouchIrq = true; delay(1); return; }   // a touch pending
    uint32_t ms = POWER_SLEEP_MS;
    uint32_t iv = frameIntervalMs(c);
    if (iv) {      // screen on: sleep until the next frame is due
      uint32_t due = lastRender + iv - millis();
      if ((int32_t)due < (int32_t)ms) ms = (int32_t)due > 1 ? due : 1;
    }
    esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000);
    gpio_wakeup_enable((gpio_num_t)TP_INT, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();
    // the touch interrupt goes back to an edge, as attachInterrupt left it; a
    // touch that already pulled the line low is recorded by hand
    gpio_wakeup_disable((gpio_num_t)TP_INT);
    gpio_set_intr_type((gpio_num_t)TP_INT, GPIO_INTR_NEGEDGE);
    if (digitalRead(TP_INT) == LOW) gTouchIrq = true;
    return;
  }
#endif
  delay(idleDelayMs(c));
}

// brillo segun sueno + inactividad (proteccion del AMOLED)
void updateBrightness(uint32_t now) {
  // los eventos visibles despiertan la pantalla solos
  if (!screenOff && (pet.evolving() || pet.ceremony || pet.eating() || pet.showHeart())) {
    if (dimStageFor(idleSince(now, lastInteract)) != DIM_AWAKE) wakeUI();
    else lastInteract = now;
  }
  // idleSince(), not `now - lastInteract`: a touch stamped after `now` was read
  // made that wrap to ~49 days, and the panel blinked to 8 on every touch
  PowerCtx c = powerCtx(now);
  dimStage = dimStageFor(c.idleMs);
  uint8_t target = brightnessFor(c);
  static uint8_t current = 255;
  if (target != current) {
    current = target;
    panel->setBrightness(target);
  }
}

// ---------- consola serie (provision de SD + depuracion) ----------

// Hex text -> bytes. Returns the count, or -1 for an odd length, a non-hex
// digit or more than `cap`.
static int hexToBytes(const String &hex, uint8_t *out, size_t cap) {
  size_t len = hex.length();
  if (len & 1) return -1;
  if (len / 2 > cap) return -1;
  const char *hs = hex.c_str();
  auto nyb = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  for (size_t i = 0; i < len; i += 2) {
    int hi = nyb(hs[i]), lo = nyb(hs[i + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i / 2] = (uint8_t)((hi << 4) | lo);
  }
  return (int)(len / 2);
}

// The Pokedex box's half of a restore, held until the commit.
struct BoxImport {
  PartyMon *recs = nullptr;
  uint16_t n = 0;
  bool seen = false, bad = false;
  void start() { n = 0; bad = false; seen = true; }
  // A record is checked whole: length, checksum, a real species, and a
  // record that says it is that species. A shorter record (an older build's)
  // is read into the front of the current one, as the box file itself does.
  bool add(const uint8_t *b, int len) {
    if (!seen || len < 2 + 2 + 2) return false;
    size_t body = (size_t)len - 2;
    uint16_t want = (uint16_t)(b[body] | (b[body + 1] << 8));
    if (saveCrc16(b, body) != want) return false;
    int16_t d = (int16_t)(b[0] | (b[1] << 8));
    if (d < 1 || d > DEX_COUNT) return false;
    if (!recs) recs = (PartyMon *)malloc(sizeof(PartyMon) * DEX_COUNT);
    if (!recs || n >= DEX_COUNT) return false;
    PartyMon m;
    size_t k = body - 2 < sizeof(PartyMon) ? body - 2 : sizeof(PartyMon);
    memcpy(&m, b + 2, k);
    if (m.dex != d) return false;
    recs[n++] = m;
    return true;
  }
};
static BoxImport boxIn;

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  if (sdSerialCommand(line)) return;

  if (line.startsWith("SAY ")) {        // speak a text: SAY Een wilde PIKACHU!
    Serial.println(speechSay(line.c_str() + 4) ? "SAID" : "NOT SAID");
    return;
  } else if (line == "SPK") {           // the texts on screen and where they can be touched
    speechDump();
    return;
  } else if (line.startsWith("TOUCH ")) {   // speak what is at x y, as a finger would
    int tx = 0, ty = 0;
    sscanf(line.c_str() + 6, "%d %d", &tx, &ty);
    bool ok = speechAt(tx, ty, true);
    Serial.println(ok ? "SAID" : "NOT SAID");
    return;
  }

  if (line == "HATCH") {
    pet.eggTap(); pet.eggTap(); pet.eggTap();
    Serial.println("DONE");
  } else if (line.startsWith("SPEC ")) {
    int n = line.substring(5).toInt();
    if (n >= 1 && n <= DEX_COUNT) {
      pet.prevSpeciesId = pet.speciesId;
      pet.speciesId = n;
      Serial.printf("especie #%d %s\n", n, DEX_TBL[n].name);
    }
    Serial.println("DONE");
  } else if (line.startsWith("LVL ")) {
    // level() is 1 + age/rate, so the age for level N is (N-1) rates -- this
    // used to set N and hand back N+1, which is a poor thing for a command
    // named LVL to do when it is what every balance check is anchored on.
    long want = line.substring(4).toInt();
    if (want < 1) want = 1;
    if (want > MAX_LEVEL) want = MAX_LEVEL;
    pet.ageMinutes = (uint32_t)(want - 1) * MINUTES_PER_LEVEL;
    pet.saveNow();
    Serial.printf("lvl=%u\n", pet.level());
  } else if (line.startsWith("MISS ")) {
    // MISS <n>: sets the care mistakes -- "descuidos", the desc= on STATS.
    // Each one pushes every evolution threshold up a level, so a creature that
    // was neglected early evolves late; MISS 0 forgives that. Its sibling
    // commands are IV, TR and LVL.
    long m = line.substring(5).toInt();
    if (m < 0) m = 0;
    if (m > 255) m = 255;
    pet.careMistakes = (uint8_t)m;
    pet.saveNow();
    Serial.printf("desc=%u\n", pet.careMistakes);
  } else if (line.startsWith("TR ")) {
    // TR <atk> <def> <spe>: sets the TRAINING (this game's EVs), for testing a
    // fully-raised creature without playing the minigames for an hour. Each is
    // clamped to trMaxFor(iv), the same IV-bound ceiling the games enforce, so
    // this cannot produce a creature the player could not have raised.
    int v[3] = { 0, 0, 0 };
    int n = sscanf(line.c_str() + 3, "%d %d %d", &v[0], &v[1], &v[2]);
    if (n >= 1) {
      int a = v[0], d = (n >= 2) ? v[1] : v[0], e = (n >= 3) ? v[2] : v[0];
      pet.trAtk = (uint8_t)(a < 0 ? 0 : (a > pet.trMaxAtk() ? pet.trMaxAtk() : a));
      pet.trDef = (uint8_t)(d < 0 ? 0 : (d > pet.trMaxDef() ? pet.trMaxDef() : d));
      pet.trSpe = (uint8_t)(e < 0 ? 0 : (e > pet.trMaxSpe() ? pet.trMaxSpe() : e));
      pet.saveNow();
    }
    Serial.printf("tr=%u/%u/%u topes=%u/%u/%u\n", pet.trAtk, pet.trDef, pet.trSpe,
                  pet.trMaxAtk(), pet.trMaxDef(), pet.trMaxSpe());
  } else if (line.startsWith("IV ")) {
    // IV <fue> <def> <vel> <vit>: fija los valores individuales (pruebas).
    // Con "IV 31 31 31 31" se ve el techo; con "IV 8 8 8 8" el suelo.
    int v[4] = { 16, 16, 16, 16 };
    int n = sscanf(line.c_str() + 3, "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]);
    if (n >= 1) {
      for (int i = 0; i < 4; i++) v[i] = v[i] < 0 ? 0 : (v[i] > 31 ? 31 : v[i]);
      pet.ivAtk = v[0];
      pet.ivDef = (n >= 2) ? v[1] : v[0];
      pet.ivSpe = (n >= 3) ? v[2] : v[0];
      pet.ivHp = (n >= 4) ? v[3] : v[0];
      if (pet.trAtk > pet.trMaxAtk()) pet.trAtk = pet.trMaxAtk();
      if (pet.trDef > pet.trMaxDef()) pet.trDef = pet.trMaxDef();
      if (pet.trSpe > pet.trMaxSpe()) pet.trSpe = pet.trMaxSpe();
    }
    pet.saveNow();   // IV used to change RAM only and never write
    Serial.printf("iv=%u/%u/%u/%u topes=%u/%u/%u\n", pet.ivAtk, pet.ivDef,
                  pet.ivSpe, pet.ivHp, pet.trMaxAtk(), pet.trMaxDef(), pet.trMaxSpe());
    Serial.println("DONE");
  } else if (line.startsWith("TIME ")) {
    uint32_t e = (uint32_t)line.substring(5).toInt();
    rtcSetEpoch(e);
    pet.setClock(e);
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line.startsWith("RTCSET ")) {  // solo RTC (simular apagados en pruebas)
    rtcSetEpoch((uint32_t)line.substring(7).toInt());
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line == "TIME") {
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line == "GAL") {
    galleryOpen = !galleryOpen;
    galleryDetail = 0;
    galleryDirty = true;
    if (!galleryOpen) galleryPmd.unload();
    Serial.println("DONE");
  } else if (line == "EGGS") {
    // simula 20 tiradas de huevo (no cambia el estado del juego)
    for (int i = 0; i < 20; i++) {
      int16_t d = pet.pickEggSpecies();
      Serial.printf("%d:%s(r%u) ", d, DEX_TBL[d].name, DEX_TBL[d].rarity);
    }
    Serial.println();
    Serial.println("DONE");
  } else if (line.startsWith("EGG ")) {
    // EGG <dex> [shiny]: hatch a chosen species right now. The legendary and
    // shiny IV guarantees only apply at hatch time, so this is the only way to
    // exercise them on hardware (SHINY below just toggles the flag afterwards).
    int dex = 0, sh = 0;
    int n = sscanf(line.c_str() + 4, "%d %d", &dex, &sh);
    if (n >= 1 && dex >= 1 && dex <= DEX_COUNT) {
      pet.dbgHatchAs(dex, sh != 0);
      Serial.printf("%s%s iv=%u/%u/%u/%u\n", DEX_TBL[dex].name,
                    pet.shiny ? " *SHINY*" : "", pet.ivAtk, pet.ivDef,
                    pet.ivSpe, pet.ivHp);
    }
    Serial.println("DONE");
  } else if (line.startsWith("BATTLE")) {
    // BATTLE <dex> [level] -- the only way in until the trainer roster exists
    int dex = 0, lvl = 0;
    int n = sscanf(line.c_str() + 6, "%d %d", &dex, &lvl);
    if (n >= 1 && dex >= 1 && dex <= DEX_COUNT) {
      startBattle(dex, lvl > 0 ? (uint8_t)lvl : pet.level());
      Serial.printf("battle vs %s Lv.%u\n", DEX_TBL[dex].name, btlFoe.level);
    } else {
      Serial.println("uso: BATTLE <dex> [nivel]");
    }
    Serial.println("DONE");
  } else if (line == "SHINY") {  // alterna shiny del actual (pruebas)
    pet.shiny = !pet.shiny;
    Serial.printf("shiny=%d\n", pet.shiny);
    Serial.println("DONE");
  } else if (line.startsWith("NICK ")) {
    pet.rename(line.substring(5).c_str());
    Serial.printf("nick=%s\n", pet.nick);
    Serial.println("DONE");
  } else if (line == "CAREDAY") {  // simula un dia nuevo cuidado (pruebas)
    pet.setClock(pet.lastSeenEpoch + 86400);
    pet.caress();
    Serial.printf("streak=%u bond=%u medals=0x%X\n", pet.streak, pet.bond, pet.medals);
    Serial.println("DONE");
  } else if (line == "BYE") {
    pet.startFarewell();
    Serial.println("DONE");
  } else if (line == "RUN") {
    pet.startRunaway();
    Serial.println("DONE");
  } else if (line == "BEEP") {
    sfxPlay(SFX_HATCH);  // prueba de audio
    Serial.println("DONE");
  } else if (line == "ABANDON") {
    pet.dbgRunawayReady();  // fuerza el estado "lista para escaparse" (test del boton)
    Serial.println("DONE");
  } else if (line == "EXPORT") {
    // Prints the whole save as a block of IMPORT commands. Pasting that block
    // back is the restore -- there is no separate format to get wrong, and no
    // single 2000-character line for a terminal to mangle.
    static uint8_t buf[2048];
    size_t n = saveExport(buf, sizeof(buf));
    if (!n) { Serial.println("EXPORT FAIL"); return; }
    Serial.printf("# TamaPoke save, %u bytes. Paste this whole block back.\n",
                  (unsigned)n);
    for (size_t i = 0; i < n; i += 48) {
      Serial.print("IMPORT ");
      for (size_t j = i; j < i + 48 && j < n; j++) Serial.printf("%02X", buf[j]);
      Serial.println();
    }
    // The Pokedex box is a file, not NVS, so it rides as its own lines: one
    // creature each, with its own checksum. START says "this backup has a
    // box", so restoring an empty one empties it, while a backup from before
    // the box existed leaves the box alone.
    Serial.println("IMPORTBOX START");
    for (int16_t d = 1; d <= DEX_COUNT; d++) {
      PartyMon m;
      if (!dexBoxGet(d, m)) continue;
      uint8_t rec[2 + sizeof(PartyMon) + 2];
      rec[0] = (uint8_t)d; rec[1] = (uint8_t)(d >> 8);
      memcpy(rec + 2, &m, sizeof(PartyMon));
      uint16_t c = saveCrc16(rec, 2 + sizeof(PartyMon));
      rec[2 + sizeof(PartyMon)] = (uint8_t)c; rec[3 + sizeof(PartyMon)] = (uint8_t)(c >> 8);
      Serial.print("IMPORTBOX ");
      for (size_t j = 0; j < sizeof(rec); j++) Serial.printf("%02X", rec[j]);
      Serial.println();
    }
    Serial.println("IMPORT");        // the empty one commits
  } else if (line.startsWith("IMPORTBOX")) {
    // IMPORTBOX START   a box follows (even an empty one)
    // IMPORTBOX <hex>   one creature: dex (2) + its record + crc16 (2)
    // Held until the commit, which applies the save AND the box or neither.
    String hex = line.substring(9);
    hex.trim();
    if (hex == "START") { boxIn.start(); return; }
    uint8_t rec[2 + sizeof(PartyMon) + 2 + 8];
    int n = hexToBytes(hex, rec, sizeof(rec));
    if (!boxIn.add(rec, n)) { Serial.println("IMPORTBOX BAD"); boxIn.bad = true; }
  } else if (line.startsWith("IMPORT")) {
    // IMPORT <hex>   append a chunk
    // IMPORT         commit what has been appended
    static uint8_t in[2048];
    static size_t inN = 0;
    String hex = line.substring(6);
    hex.trim();
    if (hex.length()) {
      if (hex.length() & 1) { Serial.println("IMPORT ODD"); inN = 0; return; }
      for (size_t i = 0; i + 1 < (size_t)hex.length(); i += 2) {
        if (inN >= sizeof(in)) { Serial.println("IMPORT FULL"); inN = 0; return; }
        auto nyb = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          return -1;
        };
        const char *hs = hex.c_str();
        int hi = nyb(hs[i]), lo = nyb(hs[i + 1]);
        if (hi < 0 || lo < 0) { Serial.println("IMPORT BAD"); inN = 0; return; }
        in[inN++] = (uint8_t)((hi << 4) | lo);
      }
      return;                        // silent while collecting
    }
    if (!inN) { Serial.println("IMPORT EMPTY"); return; }
    bool ok = !boxIn.bad && saveImport(in, inN);
    Serial.println(ok ? "IMPORT OK" : "IMPORT REJECTED");
    inN = 0;
    if (ok && boxIn.seen) {          // the save went in: now the box, replaced whole
      dexBoxWipe();
      for (uint16_t i = 0; i < boxIn.n; i++) dexBoxPut(boxIn.recs[i]);
      Serial.printf("IMPORTBOX %u\n", (unsigned)boxIn.n);
    }
    boxIn.start();
    boxIn.seen = false;
    if (ok) { Serial.println("DONE"); delay(100); ESP.restart(); }
  } else if (line == "WIPE") {
    pet.factoryReset();     // borra NVS y reinicia -> partida nueva (eleccion de inicial)
    dexBoxWipe();           // the Pokedex box is a file, not NVS: take it too
    Serial.println("DONE");
    delay(100);
    ESP.restart();
  } else if (line.startsWith("PARTY")) {
    // PARTY          list the party
    // PARTY <dex>    bank a level-50 specimen (fills slots up for testing)
    // PARTY CLEAR    empty it
    String arg = line.substring(5);
    arg.trim();
    if (arg == "CLEAR") {
      for (int i = 0; i < PARTY_SLOTS; i++) party.releaseAt(i);
    } else if (arg.length()) {
      int d = arg.toInt();
      if (d >= 1 && d <= DEX_COUNT) {
        PartyMon m;
        m.dex = d;
        m.level = 50;
        m.ivAtk = m.ivDef = m.ivSpe = m.ivHp = 20;
        m.trAtk = m.trDef = m.trSpe = 50;
        Serial.println(party.add(m) ? "added" : "party full");
      }
    }
    Serial.printf("party %u/%u:", party.count(), PARTY_SLOTS);
    for (int i = 0; i < PARTY_SLOTS; i++) {
      const PartyMon &m = party.slots[i];
      if (m.empty()) Serial.print(" -");
      else Serial.printf(" %s%s(nv%u)", DEX_TBL[m.dex].name, m.shiny ? "*" : "", m.level);
    }
    Serial.println();
    Serial.println("DONE");
  } else if (line == "OUTINGS") {   // refill the explore bar (testing)
    pet.expGain(EXP_CHARGE_UNITS * EXP_MAX);
    pet.saveNow();
    Serial.printf("outings %u/%u\n", pet.expCharges(), EXP_MAX);
    Serial.println("DONE");
  } else if (line == "FULL") {      // every need to 100: food, joy, energy, hygiene (testing)
    pet.fullness = pet.joy = pet.energy = pet.hygiene = 100;
    pet.saveNow();
    Serial.println("DONE");
  } else if (line == "DEXBOX") {    // what the Pokedex box holds, on the board's flash
    if (!dexBoxReady()) Serial.println("dexbox: NOT MOUNTED");
    Serial.printf("dexbox %u:", dexBoxCount());
    for (int16_t d = 1; d <= DEX_COUNT; d++)
      if (dexBoxHas(d)) Serial.printf(" %d%s", d, pet.isRegistered(d) ? "" : "(NOT IN POKEDEX)");
    Serial.println();
    Serial.println("DONE");
  } else if (line == "REG") {
    Serial.printf("pokedex %u/%u:", pet.registeredCount(), dexObtainable());
    for (int i = 1; i <= DEX_COUNT; i++)
      if (pet.isRegistered(i)) Serial.printf(" %d", i);
    Serial.println();
    Serial.println("DONE");
  } else if (line == "HEALTH") {
    Serial.printf("up=%lus heap=%u min=%u sd=%d mon=%d\n",
                  (unsigned long)(millis() / 1000), ESP.getFreeHeap(),
                  ESP.getMinFreeHeap(), sdReady, pmd.loaded || mon.loaded);
    // PSRAM is where the sprites and the framebuffer live, so a memory problem
    // shows up here long before it shows up in the heap figure above.
    Serial.printf("psram=%u screen=%s btlspr=%d/%d\n", (unsigned)ESP.getFreePsram(),
                  SCREEN_NAME[uiCurrentScreen() % SCR_COUNT],
                  btlPmd[0].loaded ? 1 : 0, btlPmd[1].loaded ? 1 : 0);
    Serial.println("DONE");
  } else if (line == "STATS") {
    Serial.printf("spec=%d nv=%u com=%u fel=%u ene=%u lim=%u desc=%u sd=%d mon=%d bat=%d usb=%d rtc=%u\n",
                  pet.speciesId, pet.level(), pet.fullness, pet.joy, pet.energy,
                  pet.hygiene, pet.careMistakes, sdReady, mon.loaded,
                  batPercent(), usbPresent(), rtcEpoch());
    Serial.printf("peso=%u fue=%u def=%u vel=%u vit=%u baya=%d\n",
                  pet.weight, pet.atkStat(), pet.defStat(), pet.speStat(),
                  pet.vitStat(), pet.berryKnown);
    Serial.printf("iv=%u/%u/%u/%u tr=%u/%u/%u topes=%u/%u/%u\n",
                  pet.ivAtk, pet.ivDef, pet.ivSpe, pet.ivHp,
                  pet.trAtk, pet.trDef, pet.trSpe,
                  pet.trMaxAtk(), pet.trMaxDef(), pet.trMaxSpe());
    Serial.printf("shiny=%d streak=%u/%u bond=%u medals=0x%X(%u) nick=%s\n",
                  pet.shiny, pet.streak, pet.bestStreak, pet.bond, pet.medals,
                  pet.totalMedals, pet.nick);
    Serial.println("DONE");
  }
}

// ---------- entrada tactil ----------

bool inPetZone(int16_t x, int16_t y) {
  return x > 110 && x < 356 && y > 95 && y < 310;
}

// el toque se resuelve al LEVANTAR el dedo para distinguir tap de deslizar
#define TAP_SLOP 60       // px a finger may drift and still tap (a swipe is > 80)
#define TAP_MAX_MS 2500   // ms a tap may last (the hold on the pet is 3000)

void handleTouch() {
  static uint32_t lastPoll = 0;
  if (millis() - lastPoll < 20) return;  // 50 Hz le sobra a un dedo
  lastPoll = millis();
  // solo tocamos el bus si el chip aviso por INT o si el dedo sigue abajo (hay
  // que detectar el levantamiento). Leer el CST9217 dormido se colgaba ~1s y
  // congelaba el loop entero; SensorLib no respeta el timeout de Wire.
  if (!gTouchIrq && !wasPressed) return;
  gTouchIrq = false;
  int16_t x, y;
  bool pressed = touch.getPoint(&x, &y, 1) > 0;

  // saco de entrenamiento: cada toque cuenta al instante (aporrear rapido)
  if (sackOpen) {
    if (pressed && !wasPressed) {
      wakeUI();
      if (y < 72) leaveSack();       // tocar arriba = salir, conservando lo ganado
      else sackTap();
    }
    wasPressed = pressed;
    return;
  }

  if (pressed && !wasPressed) {  // empieza el gesto
    tX0 = tXl = x;
    tY0 = tYl = y;
    tStart = millis();
    holdFired = false;
    swallowGesture = (dimStage > 0) || screenOff;  // si estaba a oscuras, solo despierta
    if (screenOff) pet.setScreenOff(false);        // waking the screen wakes it
    screenOff = false;
    wakeUI();
    if (!swallowGesture) speechAt(x, y, true);     // read out what the finger is on
  } else if (pressed) {  // sigue apoyado
    // sliding the finger onto other text reads that one too
    if (!swallowGesture && (abs(x - tXl) > 8 || abs(y - tYl) > 8)) speechAt(x, y, false);
    tXl = x;
    tYl = y;
    // pulsacion larga sin moverse sobre el bicho -> dialogo de soltar
    //
    // Gated on the MAIN screen, not on a hand-maintained list of screens to
    // exclude. That list had gallery/card/keyboard/clock on it and nothing
    // else, so the hold still fired on the party, box, gym, battle, player and
    // menu screens -- every one of which draws something inside inPetZone.
    // On the party screen it was genuinely dangerous: the grid overlaps the
    // zone, so holding a party slot opened "release the live pet?", and that
    // dialog's YES box sits on top of party slot 4. Hold a slot, tap where you
    // think a creature is, lose the creature you are actually raising.
    // One question with one answer, so a new screen cannot be forgotten.
    if (!holdFired && !swallowGesture && uiCurrentScreen() == SCR_MAIN && millis() - tStart > 3000 &&
        abs(tXl - tX0) < 30 && abs(tYl - tY0) < 30 && inPetZone(tX0, tY0) &&
        !pet.isEgg() && !confirmUntil && !pet.ceremony) {
      confirmUntil = millis() + 10000;
      holdFired = true;
    }
  } else if (wasPressed) {  // levanta el dedo: resolver gesto
    wakeUI();
    int dx = tXl - tX0, dy = tYl - tY0;
    uint32_t dt = millis() - tStart;
    if (!holdFired && !swallowGesture) {
      if (abs(dx) > 80 && abs(dy) < 70 && dt < 800) onSwipe(dx > 0 ? 1 : -1);
      else if (abs(dy) > 80 && abs(dx) < 70 && dt < 800) onSwipeV(dy > 0 ? 1 : -1);
      // A tap forgives a finger that wobbles or lingers: small hands slide
      // while pressing, and 40 px / 1.5 s lost a child's taps to nothing at all
      // (too far for a tap, too short for an 80 px swipe). Still under the 3 s
      // hold, and still well clear of a swipe.
      else if (dt < TAP_MAX_MS && abs(dx) < TAP_SLOP && abs(dy) < TAP_SLOP) onTap(tX0, tY0);
    }
  }
  wasPressed = pressed;
}

// deslizar vertical: abre/cierra la ficha del bicho

void openClock();  // prototipo

void onSwipeV(int dir) {
  if (pet.awaitingStarter()) return;  // bloqueado durante la eleccion de inicial
  if (quizOpen) { quizOpen = false; return; }   // any swipe leaves the quiz
  if (uiCurrentScreen() == SCR_DEXPICK || uiCurrentScreen() == SCR_GYMPICK)
    return;                 // on the chooser, vertical does nothing: pick a row
  if (menuOpen) { menuOpen = false; return; }   // any swipe closes the menu
  if (battleOpen) return;   // no swiping out of a fight
  if (exploreOpen) return;  // nor out of exploring: see onSwipe()
  if (pickOpen) { pickOpen = false; return; }
  if (lanOpen && tradeOpen) { tradeClose(); return; }   // back to the LAN screen, still paired
  if (lanOpen) { lanLeave(); lanOpen = false; return; }
  if (gymOpen) {
    // Same gesture as the Pokedex: vertical changes region, horizontal pages.
    gymRegion = (uint8_t)((gymRegion + (dir > 0 ? 1 : GYM_REGIONS - 1)) % GYM_REGIONS);
    gymPage = 0;
    sfxPlay(SFX_TAP);
    return;
  }
  if (playerOpen) { playerOpen = false; return; }
  if (trainOpen) { trainOpen = false; return; }
  if (movePickOpen) { movePickOpen = false; return; }
  if (boxOpen) { boxOpen = false; boxSel = 0; return; }   // vertical backs out
  if (partyOpen) {
    if (partyDetail) { partyDetail = 0; return; }
    if (partyPick) { partyPick = false; pet.endedKind = CER_NONE; }
    partyOpen = false;
    return;
  }
  // Either minigame exits on a swipe. A swipe cannot be confused with a ball
  // hit -- the gesture resolver separates them -- which the header tap no
  // longer can now that the ball is hittable up there.
  if (gameOpen) { leaveGame(); return; }
  if (sackOpen) { leaveSack(); return; }
  if (spdOpen) { leaveSpeed(); return; }
  if (galleryOpen) {
    if (galleryDetail) { galleryDetail = 0; galleryPmd.unload(); galleryDirty = true; return; }
    galleryRegion = (uint8_t)((galleryRegion + (dir > 0 ? 1 : GAL_REGIONS - 1)) % GAL_REGIONS);
    galleryPage = 0;
    galleryDirty = true;
    sfxPlay(SFX_TAP);
    return;
  }
  if (kbOpen || pet.ceremony) return;
  if (clockOpen) { clockOpen = false; return; }
  if (cardOpen) {
    if (dir < 0) cardOpen = false;  // arriba cierra la ficha
    return;
  }
  // Swipe down is the PLAYER card, up is the creature's. The clock lost this
  // gesture on purpose -- the menu's SETTINGS row already opens it, and the
  // player card is the thing you reach for far more often.
  if (dir > 0) {
    if (!confirmUntil && !feedMenuUntil) playerOpen = true;
  } else if (!pet.isEgg() && !confirmUntil && !feedMenuUntil) {
    cardOpen = true;                // deslizar arriba: ficha
    cardPage = 0;
  }
}

// party screen: pick a slot (when a newcomer is waiting) or just leave
// A banked creature's sheet: its moves above all, since typing alone does not
// tell you whether that Lapras still has ICE BEAM -- and in hard mode that is
// what decides the fight.
// The detail sheet, shared by the party and the box so the two cannot drift.
// `fromBox` picks which action the LEFT button offers; the right one is always
// RELEASE, which is irreversible and therefore always asks first.
void renderMonSheet(const PartyMon &m, bool fromBox, uint16_t xp) {
  const DexEntry &d = DEX_TBL[m.dex];
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  char head[36];
  snprintf(head, sizeof(head), "%s%s Lv.%u", m.shiny ? "*" : "",
           m.nick[0] ? m.nick : dexName(m.dex), (unsigned)m.level);
  gfx->setTextColor(d.accent);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(head) * 6, 40);
  gfx->print(head);
  char ty[24];
  if (d.type2 == T_NONE) snprintf(ty, sizeof(ty), "%s", typeName(d.type1));
  else snprintf(ty, sizeof(ty), "%s/%s", typeName(d.type1), typeName(d.type2));
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(1);
  gfx->setCursor(CX - (int)strlen(ty) * 3, 64);
  gfx->print(ty);

  for (int i = 0; i < MOVE_SLOTS; i++)
    drawMoveRow(78 + i * 52, m.moves[i], false, m.dex);

  char st[40];
  snprintf(st, sizeof(st), "ATK %u  DEF %u  SPD %u  HP %u",
           party.atkOf(m), party.defOf(m), party.speOf(m), party.vitOf(m));
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(1);
  gfx->setCursor(CX - (int)strlen(st) * 3, 300);
  gfx->print(st);
  // Experience toward the next level, or MAX at the player's own ceiling.
  {
    char xl[24];
    if (m.level >= pet.xpCap()) snprintf(xl, sizeof(xl), "XP MAX");
    else snprintf(xl, sizeof(xl), "XP %u/%u", (unsigned)xp, (unsigned)Party::xpToNext(m.level));
    gfx->setTextColor(0x8410);
    gfx->setCursor(CX - (int)strlen(xl) * 3, 288);
    gfx->print(xl);
  }

  // Bringing one back is only offered while an egg is waiting. Otherwise it
  // would silently destroy whatever creature is currently alive, and a rule the
  // player cannot see is worse than a button they cannot press. A box creature
  // goes to the party instead, which is always allowed if there is room.
  bool leftOk = fromBox ? (party.firstFree() >= 0)
                        : (pet.isEgg() && !pet.awaitingStarter());
  const char *leftLbl = fromBox ? T(S_BOX_TAKE) : T(S_REVIVE);
  gfx->fillRoundRect(PDET_L_X, PDET_BTN_Y, PDET_L_W, PDET_BTN_H, 10,
                     leftOk ? UI_BAR_OK : UI_TRACK);
  gfx->drawRoundRect(PDET_L_X, PDET_BTN_Y, PDET_L_W, PDET_BTN_H, 10, UI_INK);
  gfx->setTextColor(leftOk ? UI_BG_DAY : 0x8410);
  gfx->setTextSize(2);
  gfx->setCursor(PDET_L_X + PDET_L_W / 2 - (int)strlen(leftLbl) * 6,
                 PDET_BTN_Y + PDET_BTN_H / 2 - 8);
  gfx->print(leftLbl);

  gfx->fillRoundRect(PDET_R_X, PDET_BTN_Y, PDET_R_W, PDET_BTN_H, 10, UI_BAR_BAD);
  gfx->drawRoundRect(PDET_R_X, PDET_BTN_Y, PDET_R_W, PDET_BTN_H, 10, UI_INK);
  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(PDET_R_X + PDET_R_W / 2 - (int)strlen(T(S_RELEASE_BTN)) * 3,
                 PDET_BTN_Y + PDET_BTN_H / 2 - 4);
  gfx->print(T(S_RELEASE_BTN));

  // WHY the button is dead, ABOVE it and in a colour that can be read.
  //
  // This used to sit BELOW the buttons in UI_TRACK -- pale beige on a pale
  // background, at the smallest text size, eight pixels above "tap: back". It
  // was reported as "every time I hit bring back it just buzzes": the refusal
  // was correct (reviving would destroy the creature you are raising) but the
  // reason was invisible, so the button read as broken rather than disabled.
  // Above the button there is clear space between it and the stat line, and
  // that is also where the eye is already travelling.
  if (!leftOk) {
    const char *why = fromBox ? T(S_PARTY_FULL) : T(S_REVIVE_EGG);
    gfx->setTextColor(UI_BAR_WARN);
    gfx->setTextSize(1);
    gfx->setCursor(CX - (int)strlen(why) * 3, PDET_BTN_Y - 14);
    gfx->print(why);
  }
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_BACK)) * 6, 404);
  gfx->print(T(S_BACK));

  // Asked before it happens, because nothing gets this creature back: it is not
  // a farewell, it does not join anything, and there is no undo.
  if (releaseConfirm) {
    char q[40];
    snprintf(q, sizeof(q), T(S_RELEASE_FMT), m.nick[0] ? m.nick : dexName(m.dex));
    drawConfirmPanel(q, T(S_RELEASE_GONE), nullptr, UI_BAR_BAD,
                     T(S_YES), UI_BAR_BAD, UI_WHITE, T(S_NO), UI_TRACK, UI_INK);
  }
  gfx->flush();
}

void renderPartyDetail() {
  // An empty slot means the creature was just let go. Fall through to the grid
  // rather than returning: a bare return draws nothing AND never flushes, which
  // leaves the previous frame frozen on the panel -- the exact failure
  // flush_test exists to catch, and invisible to any screenshot.
  if (party.slots[partyDetail - 1].empty()) {
    partyDetail = 0;
    releaseConfirm = false;
    renderParty();
    return;
  }
  renderMonSheet(party.slots[partyDetail - 1], false, party.xp[partyDetail - 1]);
}

void renderBoxDetail() {
  if (party.box[boxDetail - 1].empty()) {
    boxDetail = 0;
    releaseConfirm = false;
    renderBox();
    return;
  }
  renderMonSheet(party.box[boxDetail - 1], true, party.boxXp[boxDetail - 1]);
}

// The two buttons' hit areas, so a test can prove they do not overlap without
// copying the geometry -- a test that restates the numbers drifts from them.
// Every primary button's height, so a test can hold them all to UI_TAP_MIN
// instead of waiting for somebody to report the next one by hand.
void uiButtonHeights(int *out, int max, int *n) {
  const int h[] = { BOXBTN_H, PARTYCLOSE_H, LANBTN_H, BTL_CELL_H + BTL_HIT_PAD * 2 };
  int c = (int)(sizeof(h) / sizeof(h[0]));
  if (c > max) c = max;
  for (int i = 0; i < c; i++) out[i] = h[i];
  if (n) *n = c;
}

// The gym list's difficulty pill against its first leader row. Enlarging the
// pill to a real tap target once pushed it straight over that row -- the third
// overlap of this kind, after BOX/CLOSE and the battle grid against BACK.
// Which home icon is the only one live while the pet sleeps, and where it is.
// Exposed so a test can prove it is the LIGHT: removing an icon once shifted
// every index and quietly made the BATH button the wake-up button.
// Asleep, the LIGHT is the only live icon -- it is what wakes the pet. Both the
// draw path and the tap path ask THIS, so a greyed button can never still be
// tappable and the greying can never point at the wrong icon.
bool uiButtonDisabled(int i) { return pet.sleeping && i != BTN_LIGHT; }

int uiSleepButton(int *cx, int *cy) {
  if (cx) *cx = buttons[BTN_LIGHT].cx;
  if (cy) *cy = buttons[BTN_LIGHT].cy;
  return BTN_LIGHT;
}

void uiButtonAt(int i, int *cx, int *cy, int *half) {
  if (i < 0 || i >= BTN_COUNT) return;
  if (cx) *cx = buttons[i].cx;
  if (cy) *cy = buttons[i].cy;
  if (half) *half = BTN_HALF;
}

void gymHeaderRects(int *pillTop, int *pillBot, int *rowTop) {
  if (pillTop) *pillTop = GYMDIF_Y;
  if (pillBot) *pillBot = GYMDIF_Y + GYMDIF_H;
  if (rowTop) *rowTop = GYM_ROW_Y(0);
}

void partyButtonRects(int *boxTop, int *boxBot, int *closeTop, int *closeBot) {
  if (boxTop) *boxTop = BOXBTN_Y - BOXBTN_PAD;
  if (boxBot) *boxBot = BOXBTN_Y + BOXBTN_H + BOXBTN_PAD;
  if (closeTop) *closeTop = PARTYCLOSE_Y;
  if (closeBot) *closeBot = PARTYCLOSE_Y + PARTYCLOSE_H;
}

// Which of the detail sheet's two buttons a tap landed on. One answer for the
// party and the box, off the same PDET_* constants the sheet is DRAWN with, so
// the graphic and the hit area cannot drift apart.
bool monSheetBtn(int16_t x, int16_t y, bool left) {
  if (y < PDET_BTN_Y || y > PDET_BTN_Y + PDET_BTN_H) return false;
  int x0 = left ? PDET_L_X : PDET_R_X;
  int w = left ? PDET_L_W : PDET_R_W;
  return x >= x0 && x <= x0 + w;
}

// YES / NO on the release confirm. Returns true when the tap was consumed. The
// caller swallows everything else while it is up: a modal that leaks a miss
// through to what is drawn underneath is precisely how an irreversible action
// gets triggered by a fumbled tap, which is the shape CLAUDE.md section 4 keeps
// warning about.
bool monSheetConfirmTap(int16_t x, int16_t y, bool fromBox) {
  int c1t, c1b, c2t, c2b;
  uiConfirmRects(&c1t, &c1b, &c2t, &c2b);
  if (x < CONFIRM_BTN_X || x > CONFIRM_BTN_X + CONFIRM_BTN_W) return false;
  if (y >= c1t && y <= c1b) {            // YES -- and it does not come back
    cryPlay(fromBox ? party.box[boxDetail - 1].dex : party.slots[partyDetail - 1].dex);
    if (fromBox) { party.boxReleaseAt(boxDetail - 1); boxDetail = 0; }
    else { party.releaseAt(partyDetail - 1); partyDetail = 0; }
    // A half-finished swap named a slot that may now be empty, so disarm both
    // sides rather than leaving one pointing at a creature that is gone.
    boxSwapFrom = 0;
    boxSel = 0;
    releaseConfirm = false;
    sfxPlay(SFX_BYE);
    return true;
  }
  if (y >= c2t && y <= c2b) {            // NO
    releaseConfirm = false;
    sfxPlay(SFX_TAP);
    return true;
  }
  return false;
}

void partyTap(int16_t x, int16_t y) {
  if (boxOpen) { boxTap(x, y); return; }
  // The SHEET IS CHECKED FIRST, and that is not cosmetic ordering. It covers the
  // whole screen, and its RELEASE button (x240..370, y336..384) lands inside the
  // BOX button's hit area (x138..328, y312..372) below -- so with BOX tested
  // first, tapping RELEASE opened the box instead. Nothing behind a full-screen
  // sheet may answer a tap.
  if (partyDetail) {
    // The confirm is modal: while it is up nothing else on the sheet responds,
    // or a miss on YES would fall through to the move rows underneath it.
    if (releaseConfirm) { monSheetConfirmTap(x, y, false); return; }
    if (monSheetBtn(x, y, true)) {           // BRING BACK
      if (!pet.isEgg() || pet.awaitingStarter()) { sfxPlay(SFX_DENY); return; }
      pet.reviveFrom(party.slots[partyDetail - 1]);
      party.releaseAt(partyDetail - 1);      // it is alive now, not banked
      partyDetail = 0;
      boxSwapFrom = 0;
      partyOpen = false;
      sfxPlay(SFX_HATCH);
      return;
    }
    if (monSheetBtn(x, y, false)) {          // RELEASE -- ask first, always
      releaseConfirm = true;
      sfxPlay(SFX_TAP);
      return;
    }
    for (int i = 0; i < MOVE_SLOTS; i++) {   // tap a move to change it
      int ry = 78 + i * 52;
      if (x < 70 || x > 396 || y < ry || y > ry + 50) continue;
      movePickParty = partyDetail;
      movePickSlot = i;
      movePickPage = 0;
      movePickOpen = true;
      sfxPlay(SFX_TAP);
      return;
    }
    partyDetail = 0;
    releaseConfirm = false;
    sfxPlay(SFX_TAP);
    return;
  }
  if (!partyPick && y >= BOXBTN_Y - BOXBTN_PAD && y <= BOXBTN_Y + BOXBTN_H + BOXBTN_PAD &&
      x >= BOXBTN_X - BOXBTN_PAD && x <= BOXBTN_X + BOXBTN_W + BOXBTN_PAD) {
    boxOpen = true;                  // open the box, nothing picked yet
    boxPage = 0;
    boxSwapFrom = 0;
    sfxPlay(SFX_TAP);
    return;
  }
  // exit button, and the top band, both always work
  if ((y >= 372 && y <= 416 && x >= 133 && x <= 333) || y < 34) {
    if (partyPick) {                 // declined the swap: the pet is let go
      partyPick = false;
      pet.endedKind = CER_NONE;
    }
    partyOpen = false;
    sfxPlay(SFX_TAP);
    return;
  }
  for (int i = 0; i < PARTY_SLOTS; i++) {
    int cx0 = PARTY_GRID_X + (i % 2) * (PARTY_CELL_W + 10);
    int cy0 = PARTY_GRID_Y + (i / 2) * (PARTY_CELL_H + 8);
    if (x < cx0 || x > cx0 + PARTY_CELL_W || y < cy0 || y > cy0 + PARTY_CELL_H) continue;
    if (boxSel) {                    // a box creature is waiting for a slot
      party.swapPartyBox(i, boxSel - 1);
      boxSel = 0;
      sfxPlay(SFX_MEDAL);
      return;
    }
    if (!partyPick) {
      if (boxSwapFrom == i + 1) {    // tapped again: take it to the box
        boxOpen = true;
        boxPage = 0;
        sfxPlay(SFX_TAP);
        return;
      }
      if (party.slots[i].empty()) { boxSwapFrom = i + 1; sfxPlay(SFX_TAP); return; }
      partyDetail = i + 1;
      boxSwapFrom = i + 1;           // armed, in case the box is opened next
      sfxPlay(SFX_TAP);
      return;
    }
    party.replaceAt(i, pet.endedMon);
    snprintf(partyBannerName, sizeof(partyBannerName), "%s",
             pet.endedMon.nick[0] ? pet.endedMon.nick : dexName(pet.endedMon.dex));
    partyBannerUntil = millis() + 3500;
    pet.endedKind = CER_NONE;
    partyPick = false;
    partyOpen = false;
    sfxPlay(SFX_MEDAL);
    return;
  }
}

// deslizar: dir +1 = hacia la derecha
void onSwipe(int dir) {
  // The region chooser pages, and it is checked before everything else because
  // it sits on TOP of the starter/gallery/gym screens -- each of which has its
  // own horizontal handler that would otherwise swallow the gesture. Paging a
  // screen by closing it is the bug this project shipped four times.
  if (rpickSwipe(dir)) return;
  if (pet.awaitingStarter()) return;  // bloqueado durante la eleccion de inicial
  if (quizOpen) { quizOpen = false; return; }   // any swipe leaves the quiz
  if (menuOpen) {   // pages the menu; off either end it stays put, never closes
    int p = (int)menuPage + (dir > 0 ? -1 : 1);       // left advances, as on the card
    if (p >= 0 && p < MENU_PAGES) { menuPage = (uint8_t)p; sfxPlay(SFX_TAP); }
    return;
  }
  if (battleOpen) return;   // no swiping out of a fight
  // Nor out of exploring. A child reaching for a stirring spot drags as much as
  // taps, and the drag read as a swipe and dropped them back on the map -- or
  // off it. The BACK button and the map's arrow are the ways out.
  if (exploreOpen) return;
  if (lanOpen && tradeOpen) { tradeSwipe(dir); return; }   // pages who can be offered
  if (pickOpen) {   // horizontal pages the candidates, as everywhere else
    uint8_t pages = (pickCandidates() + PICK_PER_PAGE - 1) / PICK_PER_PAGE;
    if (!pages) pages = 1;
    int p = (int)pickPage + (dir > 0 ? -1 : 1);
    if (p < 0 || p >= pages) pickOpen = false;
    else pickPage = (uint8_t)p;
    return;
  }
  if (gymOpen) {   // horizontal pages the ladder; vertical backs out
    uint8_t pages = (TRAINER_COUNT + GYM_ROWS - 1) / GYM_ROWS;
    int p = (int)gymPage + (dir > 0 ? -1 : 1);
    if (p < 0 || p >= pages) { gymPick = true; rpickPage = 0; }  // back to the chooser
    else gymPage = (uint8_t)p;
    return;
  }
  if (playerOpen) {   // horizontal pages it, like the card and the gallery
    int p = (int)playerPage + (dir > 0 ? -1 : 1);
    if (p < 0 || p >= PLAYER_PAGES) playerOpen = false;
    else playerPage = (uint8_t)p;
    return;
  }
  if (trainOpen) { trainOpen = false; return; }
  if (movePickOpen) {   // the picker is paged; without this its later pages
    uint8_t all[64];    // were simply unreachable
    uint8_t n = learnableList(all, sizeof(all));
    uint8_t pages = n ? (n + MOVE_PICK_PER_PAGE - 1) / MOVE_PICK_PER_PAGE : 1;
    int p = (int)movePickPage + (dir > 0 ? -1 : 1);
    if (p < 0 || p >= pages) movePickOpen = false;
    else movePickPage = (uint8_t)p;
    return;
  }
  if (boxOpen) {   // horizontal pages the box, as every other paged screen
    uint8_t pages = BOX_SLOTS / BOX_PER_PAGE;
    int p = (int)boxPage + (dir > 0 ? -1 : 1);
    if (p < 0 || p >= pages) { boxOpen = false; boxSel = 0; }
    else boxPage = (uint8_t)p;
    return;
  }
  if (partyOpen) {
    if (partyDetail) { partyDetail = 0; return; }
    if (partyPick) { partyPick = false; pet.endedKind = CER_NONE; }
    partyOpen = false;
    return;
  }
  if (gameOpen) { leaveGame(); return; }   // swipe out, keeping what you earned
  if (spdOpen) { leaveSpeed(); return; }
  if (kbOpen) return;
  if (clockOpen) {   // pages SETTINGS; swipe up is still the way out
    int p = (int)settingsPage + (dir > 0 ? -1 : 1);   // left advances, as on the card
    settingsPage = (uint8_t)(p < 0 ? 0 : (p > SETTINGS_PAGES - 1 ? SETTINGS_PAGES - 1 : p));
    sfxPlay(SFX_TAP);
    return;
  }
  if (cardOpen) {  // dentro de la ficha: cambiar entre las 4 paginas
    int p = (int)cardPage + (dir > 0 ? -1 : 1);  // izquierda avanza
    cardPage = p < 0 ? 0 : (p > CARD_PAGES - 1 ? CARD_PAGES - 1 : p);
    return;
  }
  if (!galleryOpen) {
    // Swipe LEFT is the gym ladder, RIGHT is the party. The Pokedex lost this
    // gesture: it has a menu row, and gestures are worth more spent on screens
    // without one.
    if (!pet.ceremony && !confirmUntil) {
      if (dir < 0) { gymOpen = true; gymPick = true; gymPage = 0; rpickPage = 0; }
      else partyOpen = true;
    }
    return;
  }
  if (galleryDetail) {  // en detalle: volver a la rejilla
    galleryDetail = 0;
    galleryPmd.unload();
    galleryDirty = true;
    return;
  }
  int np = galleryPage - dir;  // deslizar a la izquierda avanza pagina
  if (np < 0) {                // back past the first page = the region chooser
    galleryPick = true;
    rpickPage = 0;
    galleryPmd.unload();
    return;
  }
  if (np > GAL_PAGES - 1) np = GAL_PAGES - 1;
  if (np != galleryPage) {
    galleryPage = np;
    galleryDirty = true;
  }
}

void onTap(int16_t x, int16_t y) {
  if (pet.awaitingStarter()) {  // primera partida: region y luego inicial
    if (!starterRegionDone) {
      int r = regionPickTap(x, y, RPICK_FOR_START);
      if (r >= 0) {
        pet.setRegion((uint8_t)r);   // the region picked here is where eggs come from too
        starterRegionDone = true;
        sfxPlay(SFX_TAP);
      }
      return;
    }
    for (int i = 0; i < starterCountShown(pet.region); i++) {
      int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
      if (x >= 70 && x <= 396 && y >= ry && y <= ry + STARTER_ROW_H) {
        pet.chooseStarter(starterOf(pet.region, (uint8_t)i));
        sfxPlay(SFX_TAP);
        break;
      }
    }
    return;
  }
  if (quizOpen) {
    quizTap(x, y);
    return;
  }
  if (battleOpen) {
    battleTap(x, y);
    return;
  }
  if (exploreOpen) {
    exploreTap(x, y);
    return;
  }
  if (playerOpen) {
    if (playerPage == 0 && y >= 32 && y < 68) {   // the name: rename yourself
      openKeyboardFor(KB_TRAINER);
      sfxPlay(SFX_TAP);
      return;
    }
    // the avatar is the only other live target; everything else backs out
    if (playerPage == 0 && x > CX - 40 && x < CX + 40 && y > 70 && y < 146) {
      pet.avatar = (uint8_t)((pet.avatar + 1) % AVATAR_COUNT);
      pet.flushSave();
      sfxPlay(SFX_TAP);
      return;
    }
    playerOpen = false;
    return;
  }
  if (pickOpen) {
    pickTap(x, y);
    return;
  }
  if (lanOpen) {
    lanTap(x, y);
    return;
  }
  if (gymOpen && gymPick) {
    int r = regionPickTap(x, y, RPICK_FOR_GYMS);
    if (r >= 0) {
      gymRegion = (uint8_t)r;
      gymPage = 0;
      gymPick = false;
      sfxPlay(SFX_TAP);
      return;
    }
    if (y >= LANBTN_Y && y <= LANBTN_Y + LANBTN_H &&
        x >= LANBTN_X && x <= LANBTN_X + LANBTN_W) {   // LAN battle
      gymOpen = false; gymPick = false;
      lanForTrade = false;
      lan.state = LINK_OFF;
      lanOpen = true;
      sfxPlay(SFX_TAP);
      return;
    }
    if (y > 380) { gymOpen = false; gymPick = false; }
    return;
  }
  if (gymOpen) {
    if (y >= GYMDIF_Y && y <= GYMDIF_Y + GYMDIF_H) {   // the difficulty pill
      gymHard = !gymHard;
      sfxPlay(SFX_TAP);
      return;
    }
    if (y >= 380 && y <= 412 && x >= 148 && x <= 318) {   // LAN battle
      gymOpen = false;
      lanForTrade = false;
      lan.state = LINK_OFF;
      lanOpen = true;
      sfxPlay(SFX_TAP);
      return;
    }
    for (int i = 0; i < GYM_ROWS; i++) {
      uint8_t idx = gymPage * GYM_ROWS + i;
      if (idx >= TRAINER_COUNT) break;
      int ry = GYM_ROW_Y(i);
      if (x < 70 || x > 396 || y < ry || y > ry + 44) continue;
      if (!gymUnlocked(idx, gymHard)) { sfxPlay(SFX_DENY); return; }
      sfxPlay(SFX_TAP);
      gymOpen = false;
      pickTrainer = idx;
      pickHard = gymHard;
      pickPage = 0;
      pickDefault(squadCap(idx, gymHard));
      pickOpen = true;
      return;
    }
    gymOpen = false;
    return;
  }
  // Only when the prompt is what is DRAWN. It used to be `if (hasLearnOffer())`,
  // which answered taps meant for the party, the Pokedex, SETTINGS... -- every
  // screen render() puts on top of a pending prompt. See uiCurrentScreen().
  if (uiCurrentScreen() == SCR_LEARN) {
    for (int i = 0; i < MOVE_SLOTS; i++) {
      int ry = LEARN_ROW_Y(i);
      if (x < 70 || x > 396 || y < ry || y > ry + 50) continue;
      sfxPlay(SFX_TAP);
      pet.acceptLearn(i);
      return;
    }
    if (x >= 70 && x <= 396 && y >= LEARN_SKIP_Y && y <= LEARN_SKIP_Y + 44) {
      sfxPlay(SFX_TAP);
      pet.declineLearn();
    }
    return;   // modal: nothing else on screen responds until it is answered
  }
  if (trainOpen) {
    bool inPanel = (x >= TRAIN_X && x <= TRAIN_X + TRAIN_W &&
                    y >= TRAIN_Y && y <= TRAIN_Y + TRAIN_H);
    if (!inPanel) { trainOpen = false; return; }   // tap outside = back to the pet
    for (int i = 0; i < 3; i++) {   // all three train something now
      int ry = TRAIN_ROW_Y(i);
      if (x < TRAIN_X + 18 || x > TRAIN_X + TRAIN_W - 18) continue;
      if (y < ry || y > ry + TRAIN_ROW_H) continue;
      sfxPlay(SFX_TAP);
      trainOpen = false;
      if (i == 0) startSack();
      else if (i == 1) startSpeedGame();
      else startGame();          // the ball game trains DEF
      return;
    }
    return;
  }
  // The menu is modal and has two ways out: the CLOSE row and a tap anywhere
  // on the dimmed area outside the panel. A horizontal swipe turns the page
  // (see onSwipe). Deliberately no timeout: a menu that vanishes while you
  // read it is worse than one that lingers.
  if (menuOpen) {
    int h = menuHit(x, y);
    if (h == MENU_HIT_OUTSIDE || h == MENU_HIT_CLOSE) { menuOpen = false; return; }
    if (h <= MENU_HIT_DOT0) { menuPage = (uint8_t)(MENU_HIT_DOT0 - h); sfxPlay(SFX_TAP); return; }
    if (h >= 0) menuDo((uint8_t)h);
    return;
  }
  if (movePickOpen) {
    uint8_t all[64];
    uint8_t n = learnableList(all, sizeof(all));
    for (uint8_t i = 0; i < MOVE_PICK_PER_PAGE; i++) {
      uint8_t idx = movePickPage * MOVE_PICK_PER_PAGE + i;
      if (idx >= n) break;
      int ry = MOVE_PICK_Y(i);
      if (x < 70 || x > 396 || y < ry || y > ry + 50) continue;
      sfxPlay(SFX_TAP);
      // Swapping for a move already in another slot would silently duplicate
      // it, so trade the two slots instead of overwriting.
      uint8_t *tgt = pickTargetMoves();
      for (int s = 0; s < MOVE_SLOTS; s++)
        if (tgt[s] == all[idx] && s != movePickSlot) tgt[s] = tgt[movePickSlot];
      tgt[movePickSlot] = all[idx];
      if (movePickParty) party.save(); else pet.flushSave();
      movePickOpen = false;
      return;
    }
    movePickOpen = false;   // tap anywhere else = back to the moves page
    return;
  }
  if (partyOpen) {
    partyTap(x, y);
    return;
  }
  if (galleryOpen) {
    if (galleryPick) {
      int r = regionPickTap(x, y, RPICK_FOR_DEX);
      if (r >= 0) {
        galleryRegion = (uint8_t)r;
        galleryPage = 0;
        galleryDetail = 0;
        galleryDirty = true;
        galleryPick = false;
        sfxPlay(SFX_TAP);
      } else if (x >= LANBTN_X && x <= LANBTN_X + LANBTN_W &&
                 y >= LANBTN_Y && y <= LANBTN_Y + LANBTN_H) {
        openQuiz();             // the QUIZ button, where the gym chooser has LAN
      } else if (y > 380) {
        galleryOpen = false;
      }
      return;
    }
    galleryTap(x, y);
    return;
  }
  if (kbOpen) {
    keyboardTap(x, y);
    return;
  }
  if (clockOpen) {
    clockTap(x, y);
    return;
  }
  if (pet.ceremony) return;  // durante la despedida no hay botones
  if (cardOpen) {
    if (cardPage == 0 && y < 84) openKeyboard();  // tocar el nombre = renombrar
    else if (cardPage == 2) {
      for (int i = 0; i < MOVE_SLOTS; i++) {   // tap a slot to change it
        int ry = MOVE_ROW_Y(i);
        if (x < 70 || x > 396 || y < ry || y > ry + 50) continue;
        sfxPlay(SFX_TAP);
        movePickParty = 0;      // the live pet
        movePickSlot = i;
        movePickPage = 0;
        movePickOpen = true;
        return;
      }
      cardOpen = false;            // anywhere else on the page still exits
    } else {
      cardOpen = false;
    }
    return;
  }
  if (spdOpen) {
    spdTap(x, y);
    return;
  }
  if (gameOpen) {
    gameTap(x, y);
    return;
  }
  if (choiceKind) {          // dialogo de decision: boton accion (arriba) / mantener (abajo)
    int c1t, c1b, c2t, c2b;
    uiConfirmRects(&c1t, &c1b, &c2t, &c2b);
    const bool inX = (x >= CONFIRM_BTN_X && x <= CONFIRM_BTN_X + CONFIRM_BTN_W);
    bool b1 = inX && y >= c1t && y <= c1b;   // accion
    bool b2 = inX && y >= c2t && y <= c2b;   // mantener / quedaros
    if (choiceKind == 1) {                 // evolucion
      if (b1) { int16_t old = pet.speciesId; pet.evolve(); evoPmd.load(old, pet.shiny); }
      else if (b2) pet.declineEvolve();
    } else if (choiceKind == 3) {          // retirada a peticion
      if (b1) pet.startRetire();
      // b2 is simply "no": nothing to decline, the row is always there
    } else if (choiceKind == 2) {          // despedida
      if (b1) pet.startFarewell();
      else if (b2) pet.declineFarewell();
    }
    choiceKind = 0;
    return;
  }
  if (confirmUntil) {        // dialogo "soltar?": SI / NO
    if (millis() < confirmUntil && x >= 118 && x <= 218 && y >= 252 && y <= 304) {
      pet.release();
    }
    confirmUntil = 0;
    return;
  }
  if (feedMenuUntil) {       // selector de comida
    if (millis() < feedMenuUntil && y >= 288 && y <= 352 && x >= 101 && x <= 365) {
      int item = (x - 101) / 66;
      if (item == 3) pet.feedCandy();
      else pet.feedBerry(item);
      sfxPlay(SFX_EAT);
    }
    feedMenuUntil = 0;
    return;
  }
  // The birthday present, bottom-left of the scene and clear of inPetZone
  // (x > 110), so reaching for it can never be a caress or an egg tap.
  if (pet.giftReady() && x >= PRESENT_X - 14 && x <= PRESENT_X + PRESENT_W + 14 &&
      y >= PRESENT_Y - 14 && y <= PRESENT_Y + PRESENT_H + 14) {
    int16_t d = pet.openGift();
    if (d) {
      snprintf(partyBannerName, sizeof(partyBannerName), "%s", dexName(d));
      partyBannerUntil = millis() + 4000;     // "<name> joined the party!"
      cryPlay(d);
    } else {
      giftFullUntil = millis() + 3000;        // party and box are both full
      sfxPlay(SFX_DENY);
    }
    return;
  }
  if (pet.isEgg()) {
    // the region pill first, or choosing a region would also crack the egg --
    // and a near miss is swallowed rather than counted, since three taps hatch
    if (eggRegionTap(x, y)) return;
    pet.eggTap();
    sfxPlay(SFX_TAP);
    return;
  }
  // boton de evolucion: abre el dialogo evolucionar/mantener
  if (pet.wantEvolveButton() && x >= EVO_BTN_X && x <= EVO_BTN_X + EVO_BTN_W &&
      y >= EVO_BTN_Y && y <= EVO_BTN_Y + EVO_BTN_H) {
    choiceKind = 1; choiceUntil = millis() + 12000;
    return;
  }
  // botones de final (mismo recuadro): escapada directa; despedida abre dialogo.
  //
  // The runaway does NOT ask, deliberately. A pet you have to authorise to
  // leave is not really at stake, and neglect having teeth is the whole premise.
  // What was actually wrong is that a night's sleep could reach this state at
  // all -- fixed where it belongs, in the drain, by having the creature put
  // itself to bed (Pet::tick, autoSleep).
  if (x >= FAR_BTN_X && x <= FAR_BTN_X + FAR_BTN_W &&
      y >= FAR_BTN_Y && y <= FAR_BTN_Y + FAR_BTN_H) {
    if (pet.canRunawayNow()) { pet.startRunaway(); return; }
    if (pet.wantFarewellButton()) { choiceKind = 2; choiceUntil = millis() + 12000; return; }
  }
  for (int i = 0; i < BTN_COUNT; i++) {
    int dx = x - buttons[i].cx, dy = y - buttons[i].cy;
    if (dx * dx + dy * dy <= BTN_HIT * BTN_HIT) {
      if (uiButtonDisabled(i)) { sfxPlay(SFX_DENY); return; }
      sfxPlay(SFX_TAP);
      if (i == BTN_FOOD) feedMenuUntil = millis() + 6000;
      else if (i == BTN_LIGHT) pet.toggleLight();
      else if (i == BTN_BATH) startBath();
      else trainOpen = true;
      return;
    }
  }
  // tapping the name/status band opens the menu. This band was inert before,
  // and it sits clear of inPetZone (which starts at y 95).
  if (y >= 28 && y < 94) {
    menuOpen = true;
    menuPage = 0;
    sfxPlay(SFX_TAP);
    return;
  }
  // tocar al bicho = caricia
  if (inPetZone(x, y)) {
    pet.caress();
    if (!pet.sleeping) {
      if (!pet.isEgg() && cryOnCaress(millis())) cryOr(pet.speciesId, SFX_HEART);
      else sfxPlay(SFX_HEART);
    }
  }
}

// ---------- render ----------

bool gNight = false;  // noche real (por hora) o durmiendo: lo fija render()
uint16_t inkColor() { return gNight ? UI_INK_NIGHT : UI_INK; }

// ---------- escena de fondo: bioma del tipo + hora real del RTC ----------

#define C565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
uint16_t lerp565(uint16_t a, uint16_t b, int i, int n) {
  if (n <= 0) return a;
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)((((ar + (br - ar) * i / n) << 11)) |
                    (((ag + (bg - ag) * i / n) << 5)) | (ab + (bb - ab) * i / n));
}

// hora del dia 0-23 (de la hora real cacheada cada 30s; 13 si no hay reloj)
int sceneHour() {
  uint32_t e = pet.lastSeenEpoch;
  return e ? (int)((e / 3600) % 24) : 13;
}

// suelo de cada bioma de dia (de noche se mezcla hacia el azul nocturno)
static const uint16_t BIOME_SOIL[6] = {
  C565(0x7e, 0xc0, 0x7f),  // 0 pradera
  C565(0xdc, 0xca, 0x94),  // 1 playa (arena)
  C565(0x4f, 0x8a, 0x55),  // 2 bosque
  C565(0x8a, 0x55, 0x44),  // 3 volcan
  C565(0xa8, 0x90, 0x6a),  // 4 montana
  C565(0xe6, 0xee, 0xf5),  // 5 nieve
};

// An 8bpp indexed picture at 2x, centred, its top row at y0. The art is flat
// pixel art with long runs of one colour, so a run is one fillRect.
void drawIndexed(const uint16_t *pal, const uint8_t *idx, int w, int h, int y0) {
  const int SC = 2;
  int x0 = CX - (w * SC) / 2;
  for (int r = 0; r < h; r++) {
    const uint8_t *row = idx + (uint32_t)r * w;
    int c = 0;
    while (c < w) {
      uint8_t v = row[c];
      int run = 1;
      while (c + run < w && row[c + run] == v) run++;
      gfx->fillRect(x0 + c * SC, y0 + r * SC, run * SC, SC, pal[v]);
      c += run;
    }
  }
}

// Which of the home scene's four lights the hour calls for.
uint8_t homePhase(bool night) {
  int h = sceneHour();
  return night ? HP_NIGHT : h < 8 ? HP_DAWN : h < 18 ? HP_DAY : HP_DUSK;
}

// The main screen: the biome's picture (tools/gen_home.py, packed by
// gen_backs.py) in the hour's light, and a few things that move on top of it.
// The picture stops at y=312; below it is the status panel, painted here too
// so that every path through render() starts from a whole frame.
void drawScene(uint8_t biome, uint32_t now, bool night) {
  const BackScene &b = HOMES[biome < HOME_COUNT ? biome : 0];
  drawIndexed(b.pal + homePhase(night) * b.palCount, b.idx, b.w, b.h, 0);
  gfx->fillRect(0, b.h * 2, 466, 466 - b.h * 2, night ? UI_BG_NIGHT : UI_BG_DAY);
  if (night) {                       // a few stars twinkle
    for (int i = 0; i < 6; i++) {
      if ((now / 250 + i * 5) % 11 > 1) continue;
      int x = STARS[i][0], y = STARS[i][1] - 70;
      gfx->fillRect(x, y - 2, 2, 6, UI_WHITE);
      gfx->fillRect(x - 2, y, 6, 2, UI_WHITE);
    }
  }
  if (biome == 5) {                  // snow: flakes falling over everything
    for (int f = 0; f < 14; f++) {
      int fx = ((f * 67 + now / 45 + (f & 1) * (now / 90)) % 466) & ~1;
      int fy = ((f * 83 + now / 16) % 312) & ~1;
      gfx->fillRect(fx, fy, 4, 4, UI_WHITE);
    }
  } else if (biome == 3) {           // volcano: embers rising from the lava
    for (int e = 0; e < 8; e++) {
      uint32_t t = (now / 30 + e * 37) % 120;
      int bx = e < 4 ? 52 : 148, by = e < 4 ? 268 : 76;
      int x = (bx + (e % 4) * 12 - 18 + (int)(6 * sinf(t * 0.12f + e))) & ~1;
      int y = (by - (int)t) & ~1;
      if (y > 0) gfx->fillRect(x, y, 2, 2, t < 60 ? C565(0xff, 0xc8, 0x50) : C565(0xff, 0x7a, 0x30));
    }
  } else if (biome == 1) {           // beach: the sea glints
    for (int g = 0; g < 7; g++) {
      if ((now / 180 + g * 3) % 7 > 1) continue;
      int x = (40 + g * 61) & ~1, y = 164 + (g * 13) % 40;
      gfx->fillRect(x, y, 6, 2, night ? C565(0xb0, 0xc8, 0xff) : UI_WHITE);
    }
  } else if (night && (biome == 0 || biome == 2)) {   // fireflies
    for (int f = 0; f < 5; f++) {
      if ((now / 220 + f * 4) % 9 > 4) continue;
      int x = (60 + f * 80 + (int)(18 * sinf(now * 0.0011f + f))) & ~1;
      int y = (220 + (f * 23) % 70 + (int)(10 * sinf(now * 0.0017f + f * 2))) & ~1;
      gfx->fillRect(x, y, 4, 4, C565(0xe8, 0xff, 0x70));
    }
  } else if (!night && biome == 0) {  // meadow: two butterflies
    for (int k = 0; k < 2; k++) {
      int x = (int)(k ? 380 : 70) + (int)(26 * sinf(now * 0.0009f + k * 3));
      int y = (int)(k ? 236 : 250) + (int)(14 * sinf(now * 0.0023f + k));
      x &= ~1; y &= ~1;
      uint16_t c = k ? C565(0xff, 0xe0, 0x60) : C565(0xff, 0xff, 0xff);
      bool open = (now / 140 + k) % 2;
      gfx->fillRect(x - (open ? 4 : 2), y, open ? 4 : 2, 4, c);
      gfx->fillRect(x + 2, y, open ? 4 : 2, 4, c);
      gfx->fillRect(x, y, 2, 4, RGB565_BLACK);
    }
  } else if (!night && biome == 4) {  // mountain: a bird gliding past
    int x = ((now / 40) % 560) - 50, y = 120 + (int)(8 * sinf(now * 0.002f));
    x &= ~1; y &= ~1;
    bool up = (now / 300) % 2;
    gfx->fillRect(x - 6, y - (up ? 2 : 0), 6, 2, INK_K);
    gfx->fillRect(x, y, 2, 2, INK_K);
    gfx->fillRect(x + 2, y - (up ? 2 : 0), 6, 2, INK_K);
  }
}

// primera partida: elige inicial entre Bulbasaur / Charmander / Squirtle
void renderStarterSelect() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  const char *t = T(S_CHOOSE_STARTER);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(t) * 6, 68);
  gfx->print(t);
  for (int i = 0; i < starterCountShown(pet.region); i++) {
    int16_t d = starterOf(pet.region, i);
    const DexEntry &de = DEX_TBL[d];
    int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
    gfx->fillRoundRect(70, ry, 326, STARTER_ROW_H, 14, lerp565(de.accent, UI_WHITE, 6, 8));
    gfx->drawRoundRect(70, ry, 326, STARTER_ROW_H, 14, de.accent);
    const uint8_t *th = thumbs.get(d);     // miniatura del inicial (si la SD esta lista)
    if (th) drawThumb(th, 76, ry - 5, 3, false);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(3);
    gfx->setCursor(178, ry + 24);
    gfx->print(dexName(d));
  }
  gfx->flush();
}

// ---------- crash breadcrumbs ----------
//
// "It has a mini crash" is not a bug report you can act on, because the board
// never says WHY it restarted. These three live in RTC memory, which survives
// a panic, a watchdog and a software reset -- everything except pulling the
// power -- so the next boot can say what the firmware was doing when it died.
//
// Written every frame. That is one word to RTC RAM per render, which costs
// nothing next to a full-screen redraw, and it means the crumb always names
// the screen that was actually on the panel rather than the last one opened.
#define CRUMB_MAGIC 0x7AABE10C
RTC_NOINIT_ATTR uint32_t gCrumbMagic;
RTC_NOINIT_ATTR uint32_t gCrumbScreen;
RTC_NOINIT_ATTR uint32_t gCrumbHeap;


// Which screen is on the panel RIGHT NOW, in EXACTLY the order render() tests.
//
// It said so before and was not true: the learn prompt, the minigames, the
// training menu and the card sat in different places here than in render().
// With a "wants to learn" prompt pending, a swipe right drew the PARTY over
// it -- render() draws the party first -- while onTap() still sent every tap
// to the hidden prompt, so tapping a party slot taught or skipped a move. The
// tap handler now asks this function, so the order here is load-bearing.
uint8_t uiCurrentScreen() {
  if (pet.awaitingStarter()) return starterRegionDone ? SCR_STARTER : SCR_REGION;
  if (quizOpen) return SCR_QUIZ;     // drawn over the Pokedex chooser it came from
  if (galleryOpen) return galleryPick ? SCR_DEXPICK : SCR_GALLERY;
  if (movePickOpen) return SCR_MOVEPICK;
  if (partyOpen) return boxOpen ? SCR_BOX : SCR_PARTY;
  if (gameOpen || sackOpen || spdOpen) return SCR_GAME;
  if (trainOpen) return SCR_TRAIN;
  if (kbOpen) return SCR_KEYBOARD;
  if (clockOpen) return SCR_CLOCK;
  if (battleOpen) return btlWinUntil ? SCR_WIN : SCR_BATTLE;
  if (exploreOpen) return SCR_EXPLORE;    // under its own wild fights
  if (pickOpen) return SCR_PICK;
  if (lanOpen) return SCR_LAN;
  if (gymOpen) return gymPick ? SCR_GYMPICK : SCR_GYM;
  if (playerOpen) return SCR_PLAYER;
  if (pet.hasLearnOffer()) return SCR_LEARN;
  if (cardOpen) return SCR_CARD;
  if (menuOpen) return SCR_MENU;
  return SCR_MAIN;
}

static void crumbDrop() {
  gCrumbMagic = CRUMB_MAGIC;
  gCrumbScreen = uiCurrentScreen();
  gCrumbHeap = ESP.getFreeHeap();
}

static const char *resetReasonName(int r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_EXT:      return "reset pin";
    case ESP_RST_SW:       return "software (our own restart)";
    case ESP_RST_PANIC:    return "PANIC -- a crash";
    case ESP_RST_INT_WDT:  return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK WATCHDOG -- something blocked too long";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT -- the supply sagged";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default: return "unknown";
  }
}

// Printed once at boot. On a clean start it is one line; after a crash it says
// which screen was up and how much heap was left, which is the whole point.
void bootReport() {
  int r = (int)esp_reset_reason();
  bool bad = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
              r == ESP_RST_TASK_WDT || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT);
  Serial.printf("boot: reset=%s heap=%u psram=%u\n", resetReasonName(r),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  if (bad && gCrumbMagic == CRUMB_MAGIC) {
    Serial.printf("CRASH: it died on the '%s' screen with heap=%u\n",
                  gCrumbScreen < SCR_COUNT ? SCREEN_NAME[gCrumbScreen] : "?",
                  (unsigned)gCrumbHeap);
  } else if (bad) {
    Serial.println("CRASH: no breadcrumb (RTC memory was lost too)");
  }
  gCrumbMagic = 0;   // one report per crash, not on every later boot
}

void render() {
  crumbDrop();   // so a crash can name the screen it happened on
  if (pet.awaitingStarter()) {  // primera partida: region y luego inicial
    if (!starterRegionDone) renderRegionPick(RPICK_FOR_START);
    else renderStarterSelect();
    return;
  }
  if (quizOpen) {
    renderQuiz();
    return;
  }
  if (galleryOpen) {
    if (galleryPick) { renderRegionPick(RPICK_FOR_DEX); return; }
    renderGallery();
    return;
  }
  if (movePickOpen) {
    renderMovePick();
    return;
  }
  if (partyOpen) {
    if (boxOpen) {
      if (boxDetail) renderBoxDetail();
      else renderBox();
    } else if (partyDetail) renderPartyDetail();
    else renderParty();
    return;
  }
  if (gameOpen) {
    renderGame();
    return;
  }
  if (sackOpen) {
    renderSack();
    return;
  }
  if (spdOpen) {
    renderSpeed();
    return;
  }
  if (trainOpen) {
    renderTrain();
    return;
  }
  if (kbOpen) {
    renderKeyboard();
    return;
  }
  if (clockOpen) {
    renderClock();
    return;
  }
  if (battleOpen) {
    btlLinkPoll();
    renderBattle();
    return;
  }
  if (exploreOpen) {
    exploreAfterBattle();
    renderExplore();
    return;
  }
  if (pickOpen) {
    renderPick();
    return;
  }
  if (lanOpen) {
    renderLan();
    return;
  }
  if (gymOpen) {
    if (gymPick) renderRegionPick(RPICK_FOR_GYMS);
    else renderGyms();
    return;
  }
  if (playerOpen) {
    renderPlayer();
    return;
  }
  if (pet.hasLearnOffer()) {
    renderLearn();
    return;
  }
  if (cardOpen) {
    renderCard();
    return;
  }
  int h = sceneHour();
  gNight = pet.sleeping || h < 6 || h >= 20;
  // drawScene cubre los 466x466 completos: sin fillScreen(NEGRO) previo para
  // que un flush DMA solapado nunca capture negro a medias (anti-parpadeo)
  drawScene(pet.isEgg() ? 0 : DEX_TBL[pet.speciesId].biome, millis(), gNight);
  if (!pet.ceremony) {
    drawEventDecor(pet.eventToday(), millis());
    if (pet.giftReady()) drawPresent(millis());
  }

  if (pet.ceremony) {
    const DexEntry &d = DEX_TBL[pet.speciesId];
    const char *msg = (pet.ceremony == CER_FAREWELL) ? T(S_FAREWELL)
                      : (pet.ceremony == CER_RUNAWAY) ? T(S_RUNAWAY)
                                                      : T(S_GOODBYE);
    drawHeader(dexName(pet.speciesId), d.accent, msg);
    drawCeremony();
    gfx->flush();
    return;
  }

  if (pet.isEgg()) {
    drawHeader(T(S_EGG_HDR), inkColor(), eggMsg());
    int s = 5, x = CX - 16 * s, y = PET_CY - 16 * s;
    drawMap(SPR_EGG, SPRITE_H, x, y, s, false);
    if (pet.eggCracks() >= 1)
      for (auto &c : CRACK1) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
    if (pet.eggCracks() >= 2)
      for (auto &c : CRACK2) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
    if (pet.eggRarity() >= R_RARO) {
      const char *rar = (pet.eggRarity() == R_LEGENDARIO) ? T(S_EGG_LEGEND) : T(S_EGG_RARE);
      gfx->setTextColor(pet.eggRarity() == R_LEGENDARIO ? UI_BAR_WARN : 0x4C98);
      gfx->setTextSize(2);
      gfx->setCursor(CX - strlen(rar) * 6, 316);
      gfx->print(rar);
    }
    char reg[24];
    snprintf(reg, sizeof(reg), T(S_POKEDEX_FMT), pet.registeredCount(), dexObtainable());
    gfx->fillRect(0, 312, 466, 154, gNight ? UI_BG_NIGHT : UI_BG_DAY);
    gfx->setTextColor(inkColor());
    gfx->setTextSize(2);
    gfx->setCursor(CX - strlen(reg) * 6, 344);
    gfx->print(reg);

    // Which generation this egg comes from. It lives HERE rather than in the
    // settings screen because this is the only moment it does anything: the
    // species is decided when the egg appears, so choosing the region is
    // something you do to the egg in front of you.
    drawEggRegion();
  } else {
    const DexEntry &d = DEX_TBL[pet.speciesId];
    char name[28];
    const char *base = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
    snprintf(name, sizeof(name), T(S_NAME_FMT), pet.shiny ? "*" : "", base, pet.level());
    drawHeader(name, gNight ? UI_INK_NIGHT : d.accent, statusMsg());
    drawStreakBadge();
    drawPet();
    drawBath();
    drawPoops();
    // panel inferior: base limpia para barras y botones sobre el paisaje
    gfx->fillRect(0, 312, 466, 154, gNight ? UI_BG_NIGHT : UI_BG_DAY);
    drawBars();
    drawButtons();
    drawCelebration();
    if (pet.wantEvolveButton()) drawEvolveButton();        // CTA rojo: evolucionar
    else if (pet.canRunawayNow()) drawRunawayButton();     // CTA sombrio: escapada (abandono)
    else if (pet.wantFarewellButton()) drawFarewellButton();  // CTA dorado: despedida
  }

  if (pet.sleeping) {
    gfx->setTextColor(UI_INK_NIGHT);
    gfx->setTextSize(3);
    gfx->setCursor(320, 130);
    gfx->print("Zz");
  }

  // selector de comida
  if (feedMenuUntil) {
    if (millis() > feedMenuUntil) {
      feedMenuUntil = 0;
    } else {
      gfx->fillRoundRect(101, 288, 264, 64, 14, UI_WHITE);
      gfx->drawRoundRect(101, 288, 264, 64, 14, inkColor());
      drawMap(SPR_ICON_FOOD, 16, 110, 296, 3, false);
      drawMap(SPR_ICON_BERRY_B, 16, 176, 296, 3, false);
      drawMap(SPR_ICON_BERRY_G, 16, 242, 296, 3, false);
      drawMap(SPR_ICON_CANDY, 16, 308, 296, 3, false);
    }
  }

  // dialogo "soltar?" (pulsacion larga sobre el bicho)
  if (confirmUntil) {
    if (millis() > confirmUntil) {
      confirmUntil = 0;
    } else {
      gfx->fillRoundRect(94, 168, 278, 152, 16, UI_WHITE);
      gfx->drawRoundRect(94, 168, 278, 152, 16, UI_INK);
      char q[28];
      snprintf(q, sizeof(q), T(S_RELEASE_FMT), dexName(pet.speciesId));
      gfx->setTextColor(UI_INK);
      gfx->setTextSize(2);
      gfx->setCursor(CX - strlen(q) * 6, 196);
      gfx->print(q);
      gfx->fillRoundRect(118, 252, 100, 52, 12, UI_BAR_OK);
      gfx->setTextColor(UI_WHITE);
      gfx->setCursor(118 + (100 - (int)strlen(T(S_YES)) * 12) / 2, 270);
      gfx->print(T(S_YES));
      gfx->fillRoundRect(248, 252, 100, 52, 12, UI_BAR_BAD);
      gfx->setCursor(248 + (100 - (int)strlen(T(S_NO)) * 12) / 2, 270);
      gfx->print(T(S_NO));
    }
  }

  // dialogo de decision (evolucionar/mantener, despedirse/quedaros)
  if (choiceKind) {
    if (millis() > choiceUntil) choiceKind = 0;
    else drawChoiceDialog();
  }

  // "<name> joined the party!" after a farewell or release
  if (partyBannerUntil) {
    if (millis() > partyBannerUntil) {
      partyBannerUntil = 0;
    } else {
      char b[40];
      snprintf(b, sizeof(b), T(S_PARTY_JOINED), partyBannerName);
      gfx->fillRoundRect(53, 176, 360, 74, 16, UI_BAR_OK);
      gfx->drawRoundRect(53, 176, 360, 74, 16, UI_INK);
      gfx->setTextColor(UI_WHITE);
      gfx->setTextSize(2);
      gfx->setCursor(CX - (int)strlen(b) * 6, 206);
      gfx->print(b);
    }
  }

  if (menuOpen) drawMenu();

  gfx->flush();
}

// ---------- minijuego: toques con la pokeball ----------

void startGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameOpen = true;
  gameOverUntil = 0;
  gameUntil = millis() + GAME_MS;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  hitTime = 0;
  gamePetX = 233;
  respawnBall();
}

void respawnBall() {
  ballX = 150 + random(166);
  ballY = 96;
  float sp = 1.6f + gameScore * 0.05f;  // mas viva segun avanzas
  if (sp > 4.0f) sp = 4.0f;
  ballVX = random(2) ? sp : -sp;
  ballVY = 0;
}

// Leaving a minigame early banks what was actually earned rather than voiding
// it. Quitting used to forfeit everything, which mattered little when the ball
// game trained a stat you could grind back -- but it is now purely about
// happiness, and a pet that just played should be happier for it. The
// gameOver/over guards stop a swipe during the results screen paying twice.
void leaveGame() {
  if (!gameOverUntil) pet.playResult(gameScore);
  gameOpen = false;
}
void leaveSack() {
  if (!sackOverUntil) pet.trainStrength(sackHits);
  sackOpen = false;
}
void leaveSpeed() {
  if (!spdOverUntil) pet.trainSpeed(spdHits);
  spdOpen = false;
}

void gameTap(int16_t x, int16_t y) {
  if (gameOverUntil) return;
  // The ball is checked BEFORE the quit strip. The ball bounces inside a circle
  // of radius 205 about the centre, so it reaches y=28 -- well inside the y<72
  // header. Reaching up to hit a high ball used to abandon the game instead,
  // silently forfeiting the score, the speed training and the record.
  float dx = ballX - x, dy = ballY - y;
  bool onBall = (dx * dx + dy * dy < 74 * 74);
  if (!onBall && y < 72) {  // tocar la cabecera = salir, conservando lo ganado
    leaveGame();
    return;
  }
  if (onBall) {  // toque a la bola!
    gameScore++;
    sfxPlay(SFX_PLAY);
    // golpe mas suave: impulso moderado que crece poco a poco con la puntuacion
    float lift = 6.6f + (gameScore > 16 ? 3.5f : gameScore * 0.22f);
    ballVY = -lift;
    ballVX += dx * 0.12f;
    if (ballVX > 6.5f) ballVX = 6.5f;
    if (ballVX < -6.5f) ballVX = -6.5f;
    hitX = ballX;
    hitY = ballY;
    hitTime = millis();
  }
}

void stepGame() {
  float grav = 0.40f + gameScore * 0.013f;  // cae un poco mas rapido cada vez
  if (grav > 0.80f) grav = 0.80f;
  ballVY += grav;
  ballX += ballVX;
  ballY += ballVY;
  // rebote en la pared circular
  float dx = ballX - CX, dy = ballY - CY;
  float d = sqrtf(dx * dx + dy * dy);
  if (d > 205) {
    float nx = dx / d, ny = dy / d;
    float dot = ballVX * nx + ballVY * ny;
    if (dot > 0) {
      ballVX = (ballVX - 2 * dot * nx) * 0.85f;
      ballVY = (ballVY - 2 * dot * ny) * 0.85f;
    }
    ballX = CX + nx * 205;
    ballY = CY + ny * 205;
  }
  // the clock, or three misses, whichever lands first
  if (gameUntil && millis() >= gameUntil && !gameOverUntil) {
    gameNewHi = (gameScore > pet.gameHi);
    pet.playResult(gameScore);
    sfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
    gameOverUntil = millis() + 4000;
    return;
  }
  if (ballY > 384) {  // al suelo
    if (++gameMisses >= 3) {
      gameNewHi = (gameScore > pet.gameHi);
      pet.playResult(gameScore);  // actualiza el record y da felicidad
      sfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
      gameOverUntil = millis() + 4000;
    } else {
      respawnBall();
    }
  }
  // el bicho la sigue por abajo
  float chase = (ballX - gamePetX) * 0.12f;
  if (chase > 7) chase = 7;
  if (chase < -7) chase = -7;
  gamePetX += chase;
}

// ---------- saco de entrenamiento (entrena la fuerza) ----------

void startSack() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  sackOpen = true;
  sackUntil = millis() + 10000;
  sackOverUntil = 0;
  sackHits = 0;
  sackShake = 0;
  sackNewHi = false;
}

void sackTap() {
  if (millis() >= sackUntil) return;  // ya termino el tiempo
  sackHits++;
  sackShake = 16;  // sacude el saco
}

void drawGameScene();  // prototipo (definida mas abajo)

void renderSack() {
  uint32_t now = millis();
  drawGameScene();  // fondo del habitat
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  // pantalla de resultado
  if (sackOverUntil) {
    if (now > sackOverUntil) { sackOpen = false; return; }
    char b[20];
    snprintf(b, sizeof(b), T(S_HITS_FMT), sackHits);
    gfx->setTextColor(ink);
    gfx->setTextSize(4);
    gfx->setCursor(CX - strlen(b) * 12, 150);
    gfx->print(b);
    char g[18];
    snprintf(g, sizeof(g), T(S_STR_GAIN_FMT), sackGain);
    gfx->setTextColor(UI_BAR_BAD);
    gfx->setTextSize(3);
    gfx->setCursor(CX - strlen(g) * 9, 210);
    gfx->print(g);
    gfx->setTextSize(2);
    if (sackNewHi && sackHits > 0) {
      gfx->setTextColor(UI_BAR_WARN);
      gfx->setCursor(CX - strlen(T(S_NEW_RECORD)) * 6, 256);
      gfx->print(T(S_NEW_RECORD));
    } else {
      char r[18];
      snprintf(r, sizeof(r), T(S_RECORD_FMT), pet.strHi);
      gfx->setTextColor(ink);
      gfx->setCursor(CX - strlen(r) * 6, 256);
      gfx->print(r);
    }
    gfx->flush();
    return;
  }

  // se acabaron los 10 s: aplicar entrenamiento
  if (now >= sackUntil) {
    sackNewHi = (sackHits > pet.strHi);
    sackGain = pet.trainStrength(sackHits);
    sfxPlay(sackNewHi ? SFX_MEDAL : SFX_PLAY);
    sackOverUntil = now + 3500;
    gfx->flush();
    return;
  }

  // aporreo activo
  sackShake *= 0.84f;
  int off = (int)(sackShake * sinf(now * 0.05f));
  int sx = CX + off, top = 86, sy = 150;
  gfx->fillRect(CX - 3, 56, 6, top - 56, ink);          // gancho/cuerda
  gfx->fillRect(sx - 4, top - 30, 8, 34, ink);          // cadena
  gfx->fillRoundRect(sx - 42, top, 84, 150, 26, C565(0xb5, 0x3a, 0x3a));  // saco
  gfx->fillRoundRect(sx - 42, top, 84, 22, 18, C565(0x7e, 0x28, 0x28));   // tapa
  gfx->drawRoundRect(sx - 42, top, 84, 150, 26, ink);
  gfx->fillRect(sx - 42, top + 70, 84, 4, C565(0x7e, 0x28, 0x28));        // costura

  // contador de golpes
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", sackHits);
  gfx->setTextColor(ink);
  gfx->setTextSize(6);
  gfx->setCursor(CX - strlen(buf) * 18, 268);
  gfx->print(buf);

  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_HIT_FAST)) * 6, 322);
  gfx->print(T(S_HIT_FAST));

  // barra de tiempo
  uint32_t left = sackUntil - now;
  int bw = 280, fw = (int)((uint32_t)bw * left / 10000);
  gfx->fillRoundRect(CX - bw / 2, 350, bw, 16, 5, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, 350, fw, 16, 5, UI_BAR_OK);

  gfx->flush();
}

// fondo del minijuego: hatibat del bicho (cielo por hora + suelo del bioma)
void drawGameScene() {
  int hh = sceneHour();
  bool night = hh < 6 || hh >= 20;
  uint16_t top, bot;
  if (night)       { top = C565(0x0c, 0x12, 0x24); bot = C565(0x1e, 0x26, 0x46); }
  else if (hh < 8) { top = C565(0xd1, 0x6a, 0x86); bot = C565(0xf3, 0xb8, 0x7c); }
  else if (hh < 18){ top = C565(0x8f, 0xc8, 0xea); bot = C565(0xdc, 0xee, 0xe6); }
  else             { top = C565(0xc7, 0x5a, 0x4a); bot = C565(0xf0, 0xae, 0x64); }
  int hor = 376;
  for (int y = 0; y < hor; y += 8)
    gfx->fillRect(0, y, 466, 8, lerp565(top, bot, y, hor));
  if (night)
    for (auto &st : STARS) gfx->fillRect(st[0], st[1], 4, 4, UI_WHITE);
  uint8_t bio = pet.isEgg() ? 0 : DEX_TBL[pet.speciesId].biome;
  uint16_t soil = BIOME_SOIL[bio < 6 ? bio : 0];
  if (night) soil = lerp565(soil, C565(0x16, 0x1c, 0x30), 9, 16);
  gfx->fillRect(0, hor, 466, 466 - hor, soil);
}

void renderGame() {
  // sin fillScreen(NEGRO): drawGameScene cubre los 466x466 completos. Si el
  // DMA del flush anterior aun lee el buffer, vera contenido valido (no negro
  // a medio pintar), que era el parpadeo a 25 fps.
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  if (gameOverUntil) {
    drawGameScene();
    if (millis() > gameOverUntil) {
      gameOpen = false;
      return;
    }
    char buf[22];
    snprintf(buf, sizeof(buf), T(S_SCORE_FMT), gameScore);
    gfx->setTextColor(ink);
    gfx->setTextSize(4);
    gfx->setCursor(CX - strlen(buf) * 12, 160);
    gfx->print(buf);
    gfx->setTextSize(2);
    if (gameNewHi && gameScore > 0) {
      gfx->setTextColor(UI_BAR_WARN);
      gfx->setCursor(CX - strlen(T(S_NEW_RECORD)) * 6, 214);
      gfx->print(T(S_NEW_RECORD));
    } else {
      char rec[20];
      snprintf(rec, sizeof(rec), T(S_RECORD_FMT), pet.gameHi);
      gfx->setTextColor(ink);
      gfx->setCursor(CX - strlen(rec) * 6, 214);
      gfx->print(rec);
    }
    const char *msg = gameScore >= 10 ? T(S_GREAT_JOY) : T(S_PLUS_JOY);
    gfx->setTextColor(ink);
    gfx->setCursor(CX - strlen(msg) * 6, 250);
    gfx->print(msg);
    gfx->flush();
    return;
  }

  drawGameScene();
  stepGame();

  // marcador, record y vidas
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", gameScore);
  gfx->setTextColor(ink);
  gfx->setTextSize(4);
  gfx->setCursor(CX - strlen(buf) * 12, 30);
  gfx->print(buf);
  char rec[12];
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.gameHi);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(rec) * 6, 76);
  gfx->print(rec);
  for (int i = 0; i < 3; i++) {
    if (i < 3 - gameMisses) gfx->fillCircle(180 + i * 28, 104, 6, UI_BAR_BAD);
    else gfx->drawCircle(180 + i * 28, 104, 6, UI_TRACK);
  }
  // The clock, drawn like the bag's and the reaction test's so all three games
  // read the same way. Thin and near the rim: the middle belongs to the ball.
  if (!gameOverUntil) {
    uint32_t now2 = millis();
    uint32_t left = (gameUntil > now2) ? gameUntil - now2 : 0;
    int bw = 200, fw = (int)((uint32_t)bw * left / GAME_MS);
    gfx->fillRoundRect(CX - bw / 2, 124, bw, 10, 4, UI_TRACK);
    if (fw > 2)
      gfx->fillRoundRect(CX - bw / 2, 124, fw, 10, 4,
                         left < 5000 ? UI_BAR_WARN : UI_BAR_OK);
  }

  if (pmd.loaded) {
    uint8_t act = (ballX > gamePetX + 4) ? PMD_WALKR : (ballX < gamePetX - 4) ? PMD_WALKL : PMD_IDLE;
    if (!pmd.has(act)) act = PMD_IDLE;
    drawPmdAct(act, (int)gamePetX, 394, millis(), true, false, 3);
  } else if (mon.loaded) {
    int s = (mon.h * 2 > 130) ? 1 : 2;
    int w = mon.w * s, h = mon.h * s;
    uint16_t fm = mon.frameMs ? mon.frameMs : 100;
    uint16_t fi = (millis() / fm) % mon.frames;
    const uint8_t *fr = mon.data + (uint32_t)fi * mon.w * mon.h;
    int px = (int)gamePetX - w / 2, py = 394 - h;
    for (int r = 0; r < mon.h; r++) {
      const uint8_t *row = fr + r * mon.w;
      forRuns(mon.w, [&](int c) { return indexedKey(row[c], false); },
              [&](int c0, int len, int k) { gfx->fillRect(px + c0 * s, py + r * s, len * s, s, mon.pal[k]); });
    }
  }

  // anillo de impacto que se expande y desvanece (feedback suave del golpe)
  uint32_t ht = millis() - hitTime;
  if (hitTime && ht < 260) {
    int rad = 22 + (int)(ht / 6);
    gfx->drawCircle((int)hitX, (int)hitY, rad, C565(0xff, 0xe7, 0x9f));
    gfx->drawCircle((int)hitX, (int)hitY, rad - 2, C565(0xff, 0xd9, 0x8a));
  }

  // la pokeball
  drawMap(SPR_ICON_PLAY, 16, (int)ballX - 24, (int)ballY - 24, 3, false);

  gfx->flush();
}

// ---------- ficha del bicho (deslizar vertical) ----------

// una fila de la ficha: etiqueta, barra, valor y (si iv != IV_NONE) el valor
// individual que fija el techo de ese stat
// (sin argumento por defecto: el generador de prototipos de Arduino los
// descarta y las llamadas que lo omitan no compilarian)
#define IV_NONE 0xFF
void drawCardStat(int y, const char *label, uint16_t val, uint16_t maxBar,
                  uint16_t color, uint8_t iv) {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(70, y);
  gfx->print(label);
  // The bar used to start at 112, which leaves 42px for a label drawn at size 2
  // -- three characters. BOND (EN), LIEN (FR) and LACO (PT) are four, so the
  // label ran under the bar. 132 fits five, with the bar narrowed to keep the
  // number clear of it.
  int bw = 130;
  int fw = (int)val * bw / maxBar;
  if (fw > bw) fw = bw;
  gfx->fillRoundRect(132, y + 2, bw, 11, 3, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(132, y + 2, fw, 11, 3, color);
  char num[8];
  snprintf(num, sizeof(num), "%u", val);
  gfx->setCursor(272, y);
  gfx->print(num);
  if (iv != IV_NONE) {
    char b[10];
    snprintf(b, sizeof(b), T(S_IV_FMT), iv);
    // un IV perfecto se resalta: es el golpe de suerte que el jugador busca
    gfx->setTextColor(iv >= 31 ? UI_BAR_WARN : UI_TRACK);
    gfx->setCursor(344, y);
    gfx->print(b);
  }
}

// ---------- ajuste de hora en pantalla (deslizar abajo) ----------
// El usuario pone su hora LOCAL a ojo; el firmware la usa tal cual, asi que
// no hay que gestionar zona horaria. Preserva el dia (no rompe racha/edad).

void openClock() {
  uint32_t e = pet.lastSeenEpoch ? pet.lastSeenEpoch : rtcEpoch();
  clockH = (e / 3600) % 24;
  clockM = (e / 60) % 60;
  CalDate c = calFromDays(e / 86400);
  dateY = c.year < DATE_YEAR_MIN ? DATE_YEAR_MIN : (c.year > DATE_YEAR_MAX ? DATE_YEAR_MAX : c.year);
  dateM = c.month;
  dateD = c.day;
  settingsPage = SET_PAGE_TIME;
  clockOpen = true;
}

// Which page of SETTINGS this is, as two dots above the title. Tapping them
// turns the page too, for anyone who does not think to swipe.
#define SET_DOTS_Y 22
static void drawSettingsDots() {
  for (uint8_t d = 0; d < SETTINGS_PAGES; d++) {
    int cx = CX - (SETTINGS_PAGES - 1) * 8 + d * 16;
    if (d == settingsPage) gfx->fillCircle(cx, SET_DOTS_Y, 5, UI_INK);
    else gfx->drawCircle(cx, SET_DOTS_Y, 5, UI_TRACK);
  }
}

// OPTIONS. Both settings are the player's, saved with the game, and apply at
// once -- there is no OK to forget.
#define OPT_KID_X 296
#define OPT_KID_Y 104
#define OPT_KID_W 92
#define OPT_KID_H UI_TAP_MIN
#define OPT_SPD_Y 226                 // the - / + boxes, drawn by drawClockBtn
#define OPT_MINUS_X 112
#define OPT_PLUS_X 296
#define OPT_BD_Y 350                  // the birthday's four buttons
#define OPT_BD_W 40
#define OPT_BD_H 40
#define OPT_BD_DM_X 92                // day - / +
#define OPT_BD_DP_X 172
#define OPT_BD_MM_X 254               // month - / +
#define OPT_BD_MP_X 334
void renderOptions() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  drawSettingsDots();
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_OPTIONS)) * 9, 44);
  gfx->print(T(S_OPTIONS));

  // kid mode: a switch
  gfx->setTextSize(2);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(80, OPT_KID_Y + 14);
  gfx->print(T(S_KID_MODE));
  bool kid = pet.kidMode;
  const char *kl = kid ? T(S_ON) : T(S_OFF);
  gfx->fillRoundRect(OPT_KID_X, OPT_KID_Y, OPT_KID_W, OPT_KID_H, 10, kid ? UI_BAR_OK : UI_WHITE);
  gfx->drawRoundRect(OPT_KID_X, OPT_KID_Y, OPT_KID_W, OPT_KID_H, 10, UI_INK);
  gfx->setTextColor(kid ? UI_BG_DAY : UI_INK);
  gfx->setCursor(OPT_KID_X + (OPT_KID_W - (int)strlen(kl) * 12) / 2, OPT_KID_Y + 14);
  gfx->print(kl);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(1);
  gfx->setCursor(CX - (int)strlen(T(S_KID_HINT)) * 3, OPT_KID_Y + OPT_KID_H + 12);
  gfx->print(T(S_KID_HINT));

  // the needs speed: - 50% +
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(T(S_NEEDS_SPEED)) * 6, OPT_SPD_Y - 30);
  gfx->print(T(S_NEEDS_SPEED));
  drawClockBtn(OPT_MINUS_X, OPT_SPD_Y, "-");
  drawClockBtn(OPT_PLUS_X, OPT_SPD_Y, "+");
  char pct[8];
  snprintf(pct, sizeof(pct), "%u%%", pet.drainPct);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - (int)strlen(pct) * 9, OPT_SPD_Y + 18);
  gfx->print(pct);
  gfx->setTextSize(1);
  gfx->setCursor(CX - (int)strlen(T(S_NEEDS_HINT)) * 3, OPT_SPD_Y + 74);
  gfx->print(T(S_NEEDS_HINT));

  // the birthday: day and month, each with its own - and +. "--" = not set.
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(T(S_BIRTHDAY)) * 6, OPT_BD_Y - 26);
  gfx->print(T(S_BIRTHDAY));
  const int bx[4] = { OPT_BD_DM_X, OPT_BD_DP_X, OPT_BD_MM_X, OPT_BD_MP_X };
  for (int i = 0; i < 4; i++) {
    gfx->fillRoundRect(bx[i], OPT_BD_Y, OPT_BD_W, OPT_BD_H, 8, UI_WHITE);
    gfx->drawRoundRect(bx[i], OPT_BD_Y, OPT_BD_W, OPT_BD_H, 8, UI_INK);
    gfx->setCursor(bx[i] + OPT_BD_W / 2 - 6, OPT_BD_Y + 12);
    gfx->print(i % 2 ? "+" : "-");
  }
  char dd[4], mm[4];
  if (pet.bdayMonth) { snprintf(dd, sizeof(dd), "%u", pet.bdayDay); snprintf(mm, sizeof(mm), "%u", pet.bdayMonth); }
  else { snprintf(dd, sizeof(dd), "--"); snprintf(mm, sizeof(mm), "--"); }
  gfx->setTextSize(2);
  int dcx = (OPT_BD_DM_X + OPT_BD_W + OPT_BD_DP_X) / 2, mcx = (OPT_BD_MM_X + OPT_BD_W + OPT_BD_MP_X) / 2;
  gfx->setCursor(dcx - (int)strlen(dd) * 6, OPT_BD_Y + 12);
  gfx->print(dd);
  gfx->setCursor(mcx - (int)strlen(mm) * 6, OPT_BD_Y + 12);
  gfx->print(mm);
  gfx->setCursor(CX - 6, OPT_BD_Y + 12);
  gfx->print("/");

  char ver[20];
  snprintf(ver, sizeof(ver), "TamaPoke v%s", FW_VERSION);
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(1);
  gfx->setCursor(CX - (int)strlen(ver) * 3, 446);
  gfx->print(ver);
  gfx->flush();
}

static bool optBdHit(int16_t x, int16_t y, int bx) {
  return y >= OPT_BD_Y - 6 && y <= OPT_BD_Y + OPT_BD_H + 6 && x >= bx - 3 && x <= bx + OPT_BD_W + 3;
}

void optionsTap(int16_t x, int16_t y) {
  // the birthday: the day's - below 1 clears it; anything else starts it at 1/1
  uint8_t m = pet.bdayMonth, d = pet.bdayDay;
  if (optBdHit(x, y, OPT_BD_DM_X)) { pet.setBirthday(d > 1 ? m : 0, d > 1 ? d - 1 : 0); sfxPlay(SFX_TAP); return; }
  if (optBdHit(x, y, OPT_BD_DP_X)) {
    uint8_t mo = m ? m : 1;
    pet.setBirthday(mo, d >= calMonthDays(mo, 2024) ? 1 : d + 1);   // 2024: 29 Feb is a real birthday
    sfxPlay(SFX_TAP); return;
  }
  if (optBdHit(x, y, OPT_BD_MM_X)) { pet.setBirthday(m > 1 ? m - 1 : 12, d ? d : 1); sfxPlay(SFX_TAP); return; }
  if (optBdHit(x, y, OPT_BD_MP_X)) { pet.setBirthday(m < 12 ? m + 1 : 1, d ? d : 1); sfxPlay(SFX_TAP); return; }
  // generous: these are tapped by a parent, but on a 1.75" round panel
  if (y >= OPT_KID_Y - 8 && y <= OPT_KID_Y + OPT_KID_H + 8 &&
      x >= OPT_KID_X - 10 && x <= OPT_KID_X + OPT_KID_W + 10) {
    bool on = !pet.kidMode;
    pet.setKidMode(on);
    if (on && audioVolume() > KID_VOL_MAX) audioSetVolume(KID_VOL_MAX);
    sfxPlay(SFX_TAP);
    return;
  }
  if (y >= OPT_SPD_Y - 6 && y <= OPT_SPD_Y + 64) {
    if (x >= OPT_MINUS_X - 6 && x <= OPT_MINUS_X + 64) {
      pet.setDrainPct(pet.drainPct - DRAIN_STEP);
      sfxPlay(SFX_TAP);
    } else if (x >= OPT_PLUS_X - 6 && x <= OPT_PLUS_X + 64) {
      pet.setDrainPct(pet.drainPct + DRAIN_STEP);
      sfxPlay(SFX_TAP);
    }
  }
}

// Sets the RTC to the date and time being edited. setClock() moves the clock
// WITHOUT applying the gap as time away, so jumping the date months ahead on a
// board that never had one does not starve the creature.
void applyClock() {
  uint32_t e = calToDays(dateY, dateM, dateD) * 86400UL + (uint32_t)clockH * 3600 + (uint32_t)clockM * 60;
  rtcSetEpoch(e);
  pet.setClock(e);
  clockOpen = false;
}

void drawClockBtn(int x, int y, const char *l) {
  gfx->fillRoundRect(x, y, 58, 58, 12, UI_WHITE);
  gfx->drawRoundRect(x, y, 58, 58, 12, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(4);
  gfx->setCursor(x + 17, y + 15);
  gfx->print(l);
}

// pildoras de idioma centradas en y; rellena la activa
#define LANG_PILL_Y 296
#define LANG_PILL_H 30
#define LANG_PILL_X 336          // pildora de idioma (cicla los 6 al tocar)
#define LANG_PILL_W 96
// the volume mixer sits in the gap between the sound switch and the language
// pill: minus, the level, plus
#define VOL_MINUS_X 146
#define VOL_PLUS_X 276
#define VOL_BTN_W 48
static const char *const LANG_CODES[LANG_COUNT] = { "ES", "EN", "FR", "DE", "IT", "PT", "NL" };

// OK and the cancel hint, shared by the time and date pages; OK applies both
#define CLOCK_OK_X 133
#define CLOCK_OK_Y 340
#define CLOCK_OK_W 200
#define CLOCK_OK_H 48
static void drawClockOk() {
  gfx->fillRoundRect(CLOCK_OK_X, CLOCK_OK_Y, CLOCK_OK_W, CLOCK_OK_H, 14, UI_BAR_OK);
  gfx->setTextColor(UI_BG_DAY);
  gfx->setTextSize(3);
  gfx->setCursor(CX - 18, CLOCK_OK_Y + 12);
  gfx->print("OK");
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_CLOCK_CANCEL)) * 6, 410);
  gfx->print(T(S_CLOCK_CANCEL));
}
static bool clockOkHit(int16_t x, int16_t y) {
  return y >= CLOCK_OK_Y && y <= CLOCK_OK_Y + CLOCK_OK_H && x >= CLOCK_OK_X && x <= CLOCK_OK_X + CLOCK_OK_W;
}

// The DATE page: day, month and year, each a column with + above and - below.
#define DATE_PLUS_Y 110
#define DATE_VAL_Y 186
#define DATE_MINUS_Y 232
#define DATE_BTN 58
static const int DATE_COL_X[3] = { 113, 233, 353 };   // centres: day, month, year
void renderDate() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  drawSettingsDots();
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_SET_DATE)) * 9, 44);
  gfx->print(T(S_SET_DATE));

  char v[3][6];
  snprintf(v[0], sizeof(v[0]), "%02u", dateD);
  snprintf(v[1], sizeof(v[1]), "%02u", dateM);
  snprintf(v[2], sizeof(v[2]), "%u", dateY);
  const StrId lbl[3] = { S_DAY, S_MONTH, S_YEAR };
  for (int i = 0; i < 3; i++) {
    int cx = DATE_COL_X[i];
    drawClockBtn(cx - DATE_BTN / 2, DATE_PLUS_Y, "+");
    drawClockBtn(cx - DATE_BTN / 2, DATE_MINUS_Y, "-");
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(4);
    gfx->setCursor(cx - (int)strlen(v[i]) * 12, DATE_VAL_Y);
    gfx->print(v[i]);
    gfx->setTextColor(UI_TRACK);
    gfx->setTextSize(2);
    gfx->setCursor(cx - (int)strlen(T(lbl[i])) * 6, DATE_MINUS_Y + DATE_BTN + 10);
    gfx->print(T(lbl[i]));
  }
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(4);
  for (int i = 0; i < 2; i++) {   // each slash centred in the gap, not between centres
    int gap = (DATE_COL_X[i] + (int)strlen(v[i]) * 12 + DATE_COL_X[i + 1] - (int)strlen(v[i + 1]) * 12) / 2;
    gfx->setCursor(gap - 12, DATE_VAL_Y);
    gfx->print("/");
  }
  drawClockOk();
  gfx->flush();
}

// One step of a column. Day wraps within the month; changing the month or the
// year pulls the day back inside it (31 Jan -> Feb is 28 or 29, never 3 Mar).
void dateStep(int col, int dir) {
  if (col == 0) {
    uint8_t n = calMonthDays(dateM, dateY);
    dateD = dir > 0 ? (dateD >= n ? 1 : dateD + 1) : (dateD <= 1 ? n : dateD - 1);
    return;
  }
  if (col == 1) dateM = dir > 0 ? (dateM >= 12 ? 1 : dateM + 1) : (dateM <= 1 ? 12 : dateM - 1);
  else if (dir > 0 ? dateY < DATE_YEAR_MAX : dateY > DATE_YEAR_MIN) dateY += dir > 0 ? 1 : -1;
  uint8_t n = calMonthDays(dateM, dateY);
  if (dateD > n) dateD = n;
}

void dateTap(int16_t x, int16_t y) {
  int dir = (y >= DATE_PLUS_Y - 6 && y <= DATE_PLUS_Y + DATE_BTN + 6) ? 1
          : (y >= DATE_MINUS_Y - 6 && y <= DATE_MINUS_Y + DATE_BTN + 6) ? -1 : 0;
  if (dir) {
    for (int i = 0; i < 3; i++)
      if (x >= DATE_COL_X[i] - DATE_BTN / 2 - 6 && x <= DATE_COL_X[i] + DATE_BTN / 2 + 6) {
        dateStep(i, dir);
        sfxPlay(SFX_TAP);
        return;
      }
    return;
  }
  if (clockOkHit(x, y)) applyClock();
}

void renderClock() {
  if (settingsPage == SET_PAGE_OPTIONS) { renderOptions(); return; }
  if (settingsPage == SET_PAGE_DATE) { renderDate(); return; }
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  drawSettingsDots();
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_SET_TIME)) * 9, 44);
  gfx->print(T(S_SET_TIME));

  char t[8];
  snprintf(t, sizeof(t), "%02d:%02d", clockH, clockM);
  gfx->setTextSize(7);
  gfx->setCursor(CX - 105, 108);
  gfx->print(t);

  drawClockBtn(104, 190, "-");  // hora -
  drawClockBtn(170, 190, "+");  // hora +
  drawClockBtn(252, 190, "-");  // min -
  drawClockBtn(318, 190, "+");  // min +
  gfx->setTextSize(2);
  gfx->setTextColor(UI_TRACK);
  gfx->setCursor(120, 256);
  gfx->print(T(S_HOUR));
  gfx->setCursor(276, 256);
  gfx->print(T(S_MIN));

  // interruptor de sonido (izquierda de la fila de idioma)
  bool snd = audioEnabled();
  const char *sl = snd ? T(S_SND_ON) : T(S_SND_OFF);
  gfx->fillRoundRect(34, LANG_PILL_Y, 96, LANG_PILL_H, 8, snd ? UI_BAR_OK : UI_WHITE);
  gfx->drawRoundRect(34, LANG_PILL_Y, 96, LANG_PILL_H, 8, UI_INK);
  gfx->setTextColor(snd ? UI_BG_DAY : UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(34 + (96 - (int)strlen(sl) * 12) / 2, LANG_PILL_Y + 8);
  gfx->print(sl);

  // volume: a level, not a toggle. The sound switch beside it is still the
  // master -- this is how loud it is when it is on, and 0 is silent.
  {
    uint8_t v = audioVolume();
    for (int i = 0; i < 2; i++) {
      int bx = i ? VOL_PLUS_X : VOL_MINUS_X;
      bool live = i ? (v < volMax()) : (v > 0);   // kid mode stops + at KID_VOL_MAX
      gfx->fillRoundRect(bx, LANG_PILL_Y, VOL_BTN_W, LANG_PILL_H, 8,
                         live ? UI_WHITE : UI_TRACK);
      gfx->drawRoundRect(bx, LANG_PILL_Y, VOL_BTN_W, LANG_PILL_H, 8, UI_INK);
      gfx->setTextColor(live ? UI_INK : 0x8410);
      gfx->setTextSize(2);
      gfx->setCursor(bx + VOL_BTN_W / 2 - 6, LANG_PILL_Y + 8);
      gfx->print(i ? "+" : "-");
    }
    char vl[12];
    snprintf(vl, sizeof(vl), T(S_VOL_FMT), v);
    gfx->setTextColor(v ? UI_INK : UI_TRACK);
    gfx->setTextSize(1);
    gfx->setCursor(210 + (56 - (int)strlen(vl) * 6) / 2, LANG_PILL_Y + 4);
    gfx->print(vl);
    // a small bar under the number, so the level reads at a glance
    gfx->fillRoundRect(210, LANG_PILL_Y + 18, 56, 8, 3, UI_TRACK);
    if (v) gfx->fillRoundRect(210, LANG_PILL_Y + 18, 56 * v / 10, 8, 3, UI_BAR_OK);
  }

  // selector de idioma: una pildora que cicla los 6 idiomas al tocar
  gfx->fillRoundRect(LANG_PILL_X, LANG_PILL_Y, LANG_PILL_W, LANG_PILL_H, 8, UI_WHITE);
  gfx->drawRoundRect(LANG_PILL_X, LANG_PILL_Y, LANG_PILL_W, LANG_PILL_H, 8, UI_INK);
  char lp[10];
  snprintf(lp, sizeof(lp), "%s >", LANG_CODES[gLang]);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(LANG_PILL_X + (LANG_PILL_W - (int)strlen(lp) * 12) / 2, LANG_PILL_Y + 8);
  gfx->print(lp);

  drawClockOk();

  // version del firmware (discreta, abajo del todo)
  char ver[20];
  snprintf(ver, sizeof(ver), "TamaPoke v%s", FW_VERSION);
  gfx->setTextSize(1);
  gfx->setCursor(CX - (int)strlen(ver) * 3, 436);
  gfx->print(ver);
  gfx->flush();
}

void clockTap(int16_t x, int16_t y) {
  if (y < SET_DOTS_Y + 16 && x > CX - 40 && x < CX + 40) {   // the page dots
    settingsPage = (uint8_t)((settingsPage + 1) % SETTINGS_PAGES);
    sfxPlay(SFX_TAP);
    return;
  }
  if (settingsPage == SET_PAGE_OPTIONS) { optionsTap(x, y); return; }
  if (settingsPage == SET_PAGE_DATE) { dateTap(x, y); return; }
  if (y >= 190 && y <= 248) {  // fila de botones +/-
    if (x >= 104 && x < 162) clockH = (clockH + 23) % 24;
    else if (x >= 170 && x < 228) clockH = (clockH + 1) % 24;
    else if (x >= 252 && x < 310) clockM = (clockM + 59) % 60;
    else if (x >= 318 && x < 376) clockM = (clockM + 1) % 60;
    return;
  }
  if (y >= LANG_PILL_Y && y <= LANG_PILL_Y + LANG_PILL_H) {
    if (x >= 34 && x < 130) {                  // interruptor de sonido
      audioSetEnabled(!audioEnabled());
      if (audioEnabled()) sfxPlay(SFX_TAP);    // confirma al encender
      return;
    }
    if (x >= VOL_MINUS_X && x < VOL_MINUS_X + VOL_BTN_W) {
      if (audioVolume() > 0) audioSetVolume(audioVolume() - 1);
      sfxPlay(SFX_TAP);                        // so the new level is audible
      return;
    }
    if (x >= VOL_PLUS_X && x < VOL_PLUS_X + VOL_BTN_W) {
      if (audioVolume() < volMax()) audioSetVolume(audioVolume() + 1);
      sfxPlay(SFX_TAP);
      return;
    }
    if (x >= LANG_PILL_X && x < LANG_PILL_X + LANG_PILL_W) {  // cicla idioma
      setLang((Lang)((gLang + 1) % LANG_COUNT));
      sfxPlay(SFX_TAP);
      return;
    }
  }
  if (clockOkHit(x, y)) { applyClock(); return; }
}

// llama + numero de racha arriba a la izquierda
void drawStreakBadge() {
  if (pet.streak < 1) return;
  int x = 26, y = 16;
  gfx->fillTriangle(x + 8, y, x + 1, y + 17, x + 15, y + 17, UI_BAR_BAD);
  gfx->fillTriangle(x + 8, y + 7, x + 4, y + 17, x + 12, y + 17, UI_BAR_WARN);
  char s[6];
  snprintf(s, sizeof(s), "%u", pet.streak);
  gfx->setTextColor(inkColor());
  gfx->setTextSize(2);
  gfx->setCursor(x + 22, y + 2);
  gfx->print(s);
}

// banner temporal: medalla nueva o hito de racha
void drawCelebration() {
  const char *l1 = nullptr, *l2 = nullptr;
  char buf[20];
  if (pet.showMedal()) {
    for (int i = 0; i < MED_COUNT; i++)
      if (pet.newMedal & (1 << i)) { l2 = medalName(i); break; }
    l1 = T(S_MEDAL_BANNER);
  } else if (pet.showMilestone()) {
    snprintf(buf, sizeof(buf), T(S_STREAK_DAYS_FMT), pet.streak);
    l1 = T(S_GREAT);
    l2 = buf;
  } else if (pet.showSticker()) {
    l1 = T(S_NEW_STICKER);
    l2 = stickerName(pet.newSticker);
  }
  if (!l1) return;
  gfx->fillRoundRect(73, 150, 320, 96, 16, UI_BAR_WARN);
  gfx->drawRoundRect(73, 150, 320, 96, 16, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(l1) * 9, 176);
  gfx->print(l1);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(l2) * 6, 212);
  gfx->print(l2);
}

// medallas en la ficha: badge con etiqueta, color si conseguida
void drawMedalBadge(int x, int y, int i) {
  bool got = pet.hasMedal(1 << i);
  gfx->fillRoundRect(x, y, 100, 24, 6, got ? UI_BAR_OK : UI_TRACK);
  if (!got) gfx->drawRoundRect(x, y, 100, 24, 6, UI_TRACK);
  gfx->setTextColor(got ? UI_BG_DAY : 0x9492);
  gfx->setTextSize(2);
  gfx->setCursor(x + (100 - (int)strlen(medalLabel(i)) * 12) / 2, y + 5);
  gfx->print(medalLabel(i));
}

// pagina 0: perfil (retrato grande, identidad, racha, vinculo, baya)
void renderCardProfile() {
  const DexEntry &d = DEX_TBL[pet.speciesId];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  char head[26];
  snprintf(head, sizeof(head), T(S_NAME_FMT), pet.shiny ? "*" : "", nm, pet.level());
  gfx->setTextColor(d.accent);
  // auto-encoge: a tamano 3 los nombres largos no caben en la franja estrecha de
  // arriba de la pantalla redonda, asi que se cortaban por el borde
  int hlen = strlen(head);
  int hts = (hlen <= 11) ? 3 : 2;
  gfx->setTextSize(hts);
  gfx->setCursor(CX - hlen * (hts == 3 ? 9 : 6), hts == 3 ? 34 : 40);
  gfx->print(head);
  if (pet.nick[0]) {  // especie real bajo el apodo
    gfx->setTextColor(UI_TRACK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (strlen(dexName(pet.speciesId)) + 2) * 6, 64);
    gfx->printf("(%s)", dexName(pet.speciesId));
  }

  // retrato grande animado
  if (pmd.loaded) drawPmdAct(PMD_IDLE, CX, 206, millis(), true, false, 4);

  // racha con llama
  int sx = 138, sy = 224;
  gfx->fillTriangle(sx + 8, sy, sx + 1, sy + 18, sx + 15, sy + 18, UI_BAR_BAD);
  gfx->fillTriangle(sx + 8, sy + 7, sx + 4, sy + 18, sx + 12, sy + 18, UI_BAR_WARN);
  char rl[30];
  snprintf(rl, sizeof(rl), T(S_STREAK_FMT), pet.streak, pet.bestStreak);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(sx + 24, sy + 2);
  gfx->print(rl);

  drawCardStat(258, T(S_VIN), pet.bond, 100, C565(0xd4, 0x52, 0x7e), IV_NONE);

  const char *berry = !pet.berryKnown ? T(S_BERRY_UNK)
                      : pet.lovesBerry(0) ? T(S_BERRY_RED)
                      : pet.lovesBerry(1) ? T(S_BERRY_BLUE)
                                          : T(S_BERRY_GREEN);
  char info[40];
  snprintf(info, sizeof(info), T(S_INFO_FMT), berry,
           (unsigned long)(pet.ageMinutes / 1440));
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(info) * 6, 296);
  gfx->print(info);

  gfx->setTextColor(UI_TRACK);
  gfx->setCursor(CX - strlen(T(S_RENAME_HINT)) * 6, 332);
  gfx->print(T(S_RENAME_HINT));
}

// pagina 1: combate (4 barras + boton de entrenar)
void renderCardStats() {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_BATTLE)) * 9, 44);
  gfx->print(T(S_BATTLE));

  // typing, in the accent colour of the species (English in every language,
  // same as the species names themselves)
  const DexEntry &de = DEX_TBL[pet.speciesId];
  char ty[24];
  if (de.type2 == T_NONE) snprintf(ty, sizeof(ty), "%s", typeName(de.type1));
  else snprintf(ty, sizeof(ty), "%s/%s", typeName(de.type1), typeName(de.type2));
  gfx->setTextColor(de.accent);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(ty) * 6, 76);
  gfx->print(ty);

  // 360 de tope de barra: a nivel 73 (fin de ciclo) el stat mas alto de toda
  // la dex es la vitalidad de CHANSEY (355). El 260 anterior ya se desbordaba.
  drawCardStat(104, T(S_STAT_ATK), pet.atkStat(), 360, UI_BAR_BAD, pet.ivAtk);
  drawCardStat(144, T(S_STAT_DEF), pet.defStat(), 360, 0x4C98, pet.ivDef);
  drawCardStat(184, T(S_STAT_SPE), pet.speStat(), 360, UI_BAR_WARN, pet.ivSpe);
  drawCardStat(224, T(S_STAT_VIT), pet.vitStat(), 360, UI_BAR_OK, pet.ivHp);
  drawCardStat(264, T(S_STAT_WGT), pet.weight, 100, 0xB3C8, IV_NONE);

}

// Draws one move as a row: name, its type in the type's own colour, and either
// power or a STATUS marker. Shared by the moves page and the picker so a move
// looks the same wherever you meet it.
// A filled chip in the type's own colour, label in whichever of black/white
// reads on it. Returns its width so a caller can lay out beside it.
int drawTypeChip(int x, int y, uint8_t type) {
  const char *nm = typeName(type);
  int w = (int)strlen(nm) * 6 + 10;
  gfx->fillRoundRect(x, y, w, 15, 4, typeColor(type));
  gfx->setTextSize(1);
  gfx->setTextColor(typeColorIsLight(type) ? UI_INK : UI_WHITE);
  gfx->setCursor(x + 5, y + 4);
  gfx->print(nm);
  return w;
}

void drawMoveRow(int y, uint8_t mv, bool highlight, int16_t dex) {
  gfx->fillRoundRect(70, y, 326, 50, 12, highlight ? UI_BAR_WARN : UI_BG_DAY);
  gfx->drawRoundRect(70, y, 326, 50, 12, UI_INK);
  if (!mv) {
    gfx->setTextColor(UI_TRACK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (int)strlen(T(S_MOVE_EMPTY)) * 6, y + 17);
    gfx->print(T(S_MOVE_EMPTY));
    return;
  }
  const MoveEntry &m = MOVE_TBL[mv];
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(82, y + 8);
  gfx->print(moveName(mv));
  // There is no per-type palette (DexEntry.accent is per species), and inventing
  // one by hand would duplicate what gen_dex.py generates. Colouring same-type
  // moves in the species accent is more useful anyway: STAB is a 1.5x damage
  // bonus, so this marks the moves that actually hit hardest for this creature.
  // The chip carries the TYPE; STAB moved onto the power figure, where it
  // belongs -- STAB is a damage bonus, so saying it next to the damage reads
  // straight, and it leaves the type free to be its own colour.
  bool stab = hasStab(dex, m.type) && m.cat != MC_STATUS;
  int cw = drawTypeChip(82, y + 29, m.type);
  if (stab) {
    gfx->setTextColor(DEX_TBL[dex].accent);
    gfx->setTextSize(1);
    gfx->setCursor(82 + cw + 6, y + 33);
    gfx->print("STAB");
  }
  char pw[16];
  if (m.cat == MC_STATUS) snprintf(pw, sizeof(pw), "%s", T(S_MOVE_STATUS));
  else snprintf(pw, sizeof(pw), T(S_MOVE_PWR), m.power);
  gfx->setTextColor(stab ? DEX_TBL[dex].accent : UI_INK);
  gfx->setTextSize(1);
  gfx->setCursor(384 - (int)strlen(pw) * 6, y + 33);
  gfx->print(pw);
}

// card page 4: the four known moves. Tapping a slot opens the picker.
void renderCardMoves() {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_MOVES)) * 9, 44);
  gfx->print(T(S_MOVES));
  for (int i = 0; i < MOVE_SLOTS; i++) drawMoveRow(MOVE_ROW_Y(i), pet.moves[i], false, pet.speciesId);
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(1);
  gfx->setCursor(CX - (int)strlen(T(S_MOVE_TAP)) * 3, 340);
  gfx->print(T(S_MOVE_TAP));
}

// Every move the species can learn by this level, so a slot can be swapped for
// anything legal -- not just the handful a level-up would have offered.
uint8_t learnableFor(int16_t dex, uint8_t lvl, uint8_t *out, uint8_t max) {
  if (dex < 1 || dex > DEX_COUNT) return 0;
  uint8_t n = learnCount(dex), w = 0;
  for (uint8_t i = 0; i < n && w < max; i++) {
    // moveUnlockLevel(), NOT learnLevel(): a TM is stored as level 0 and would
    // otherwise clear this check at level 1. That is how a level 22 Charmeleon
    // came to be offered FIRE BLAST -- the same class of bug as the level 1
    // Squirtle with SURF, in the one path that fix did not reach.
    if (moveUnlockLevel(dex, i) > lvl) continue;
    uint8_t mv = learnMove(dex, i);
    if (!mv || mv >= MOVE_COUNT) continue;
    bool dup = false;
    for (uint8_t j = 0; j < w; j++)
      if (out[j] == mv) { dup = true; break; }
    if (!dup) out[w++] = mv;
  }
  return w;
}

// The picker targets either the live pet or a banked member. A banked one keeps
// its frozen level, so it can only relearn what it could have known back then.
uint8_t learnableList(uint8_t *out, uint8_t max) {
  if (movePickParty) {
    const PartyMon &m = party.slots[movePickParty - 1];
    return learnableFor(m.dex, (uint8_t)m.level, out, max);
  }
  return pet.isEgg() ? 0 : learnableFor(pet.speciesId, pet.level(), out, max);
}

uint8_t *pickTargetMoves() {
  return movePickParty ? party.slots[movePickParty - 1].moves : pet.moves;
}
int16_t pickTargetDex() {
  return movePickParty ? party.slots[movePickParty - 1].dex : pet.speciesId;
}

void renderMovePick() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_MOVE_PICK)) * 6, 40);
  gfx->print(T(S_MOVE_PICK));

  uint8_t all[64];
  uint8_t n = learnableList(all, sizeof(all));
  uint8_t pages = n ? (n + MOVE_PICK_PER_PAGE - 1) / MOVE_PICK_PER_PAGE : 1;
  if (movePickPage >= pages) movePickPage = 0;
  for (uint8_t i = 0; i < MOVE_PICK_PER_PAGE; i++) {
    uint8_t idx = movePickPage * MOVE_PICK_PER_PAGE + i;
    if (idx >= n) break;
    // the move already in this slot is highlighted, so replacing like for like
    // is obvious rather than a guess
    drawMoveRow(MOVE_PICK_Y(i), all[idx], all[idx] == pickTargetMoves()[movePickSlot], pickTargetDex());
  }
  for (uint8_t i = 0; i < pages && pages > 1; i++) {
    if (i == movePickPage) gfx->fillCircle(CX - (pages - 1) * 13 + i * 26, 380, 5, UI_INK);
    else gfx->drawCircle(CX - (pages - 1) * 13 + i * 26, 380, 4, UI_INK);
  }
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_BACK)) * 6, 402);
  gfx->print(T(S_BACK));
  gfx->flush();
}

// ---------- battle ----------

// Draws a battle backdrop scaled 2x. Emitted as runs of identical indices
// rather than a write per pixel: this is flat pixel art with long horizontal
// runs, and 240x112 at 2x would otherwise be 26,880 fillRect calls a frame.
// The round bezel crops the overhang physically, so nothing is clipped here.
// A ball: red over white, a black band and a button. Used by the catch and
// by the explore map's pins.
void drawBallIcon(int cx, int cy, int r) {
  gfx->fillCircle(cx, cy, r, UI_WHITE);
  for (int y = -r; y < 0; y++) {             // the red top half, inside the circle
    int w = (int)sqrtf((float)(r * r - y * y));
    gfx->drawFastHLine(cx - w, cy + y, 2 * w + 1, 0xE8A4);
  }
  gfx->drawCircle(cx, cy, r, RGB565_BLACK);
  gfx->fillRect(cx - r, cy - 1, 2 * r + 1, 3, RGB565_BLACK);
  gfx->fillCircle(cx, cy, r / 3 + 1, RGB565_BLACK);
  gfx->fillCircle(cx, cy, r / 3 - 1, UI_WHITE);
}

static void drawBack(const BackScene &b, int y0) {
  drawIndexed(b.pal, b.idx, b.w, b.h, y0);
}

// The backdrops' day/night split, the main screen's own hours.
static bool sceneIsNight() { return sceneHour() < 6 || sceneHour() >= 20; }

// Where each place is drawn. Six of them are the battle biomes, which already
// have art; the ocean and the volcano have their own (XBACKS), and the cave
// gets the supplied CAVE scene, which the volcano BIOME borrows for its fights.
const BackScene &placeScene(uint8_t pl, bool night) {
  if (pl == PL_OCEAN) return XBACKS[XB_OCEAN][night ? 1 : 0];
  if (pl == PL_VOLCANO) return XBACKS[XB_VOLCANO][night ? 1 : 0];
  if (pl == PL_CAVE) return BACKS[PL_VOLCANO][night ? 1 : 0];
  return BACKS[pl < BACK_BIOMES ? pl : 0][night ? 1 : 0];
}

// A gym fight happens in the leader's arena, picked by the type they
// specialise in; every Champion gets the grand hall. Indexed by PkType.
static const uint8_t ARENA_OF_TYPE[TYPE_COUNT] = {
  ARENA_ROCK,      // normal
  ARENA_FIRE,      // fire
  ARENA_WATER,     // water
  ARENA_ELECTRIC,  // electric
  ARENA_GRASS,     // grass
  ARENA_ICE,       // ice
  ARENA_ROCK,      // fighting
  ARENA_GHOST,     // poison
  ARENA_ROCK,      // ground
  ARENA_GRASS,     // flying
  ARENA_GHOST,     // psychic
  ARENA_GRASS,     // bug
  ARENA_ROCK,      // rock
  ARENA_GHOST,     // ghost
  ARENA_DRAGON,    // dragon
  ARENA_GHOST,     // dark
  ARENA_ELECTRIC,  // steel
  ARENA_GHOST,     // fairy
};

// The arena for the running fight, or ARENA_COUNT when it is not a gym fight
// (LAN, the BATTLE console command) and should stay where the foe lives.
uint8_t battleArena() {
  if (btlTrainer < 0 || btlTrainer >= TRAINER_COUNT) return ARENA_COUNT;
  if (btlTrainer == TRAINER_COUNT - 1) return ARENA_DRAGON;
  uint8_t t = BTL_TRAINERS[btlTrainer].type;
  return t < TYPE_COUNT ? ARENA_OF_TYPE[t] : ARENA_ROCK;
}

// Which scene: the leader's arena in a gym, otherwise the FOE's biome, since a
// wild battle happens where it lives, with the main screen's day/night split.
static void drawBattleBack() {
  if (btlWild) { drawBack(placeScene(btlWildPlace, sceneIsNight()), 30); return; }
  uint8_t a = battleArena();
  if (a < ARENA_COUNT) { drawBack(ARENAS[a], 30); return; }
  int16_t dex = btlFoe.dex;
  if (dex < 1 || dex > DEX_COUNT) { gfx->fillRect(0, 30, 466, 224, UI_BG_DAY); return; }
  uint8_t bi = DEX_TBL[dex].biome;
  if (bi >= BACK_BIOMES) bi = 0;
  drawBack(BACKS[bi][sceneIsNight() ? 1 : 0], 30);
}

// Streams a side's sprite if it is not already the one loaded. Called whenever
// a creature steps in, never per frame.
static void btlSyncSprite(uint8_t who, const Combatant &c) {
  cryPlay(c.dex);   // every step-in comes through here, so every one is heard
  int16_t key = c.dex * (c.shiny ? -1 : 1);
  if (btlPmdDex[who] == key && btlPmd[who].loaded) return;
  btlPmd[who].unload();
  btlPmdDex[who] = 0;
  if (c.dex < 1 || c.dex > DEX_COUNT) return;
  if (btlPmd[who].load(c.dex, c.shiny)) btlPmdDex[who] = key;   // NOT (uint8_t): Hoenn runs past 255
}

static void btlFreeSprites() {
  for (int i = 0; i < 2; i++) { btlPmd[i].unload(); btlPmdDex[i] = 0; }
}

static void btlSay(const char *fmt, ...) {
  if (btlMsgCount >= 6) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(btlMsg[btlMsgCount], sizeof(btlMsg[0]), fmt, ap);
  va_end(ap);
  btlMsgCount++;
}

// Turns a TurnLog into narration. Everything here was already decided by the
// engine -- nothing is recomputed, so the text can never disagree with the maths.
// Picks the cue for an action from the TurnLog, so the sound can never
// disagree with what actually happened.
static void btlSfxFor(const TurnLog &lg) {
  if (lg.targetFainted) { sfxPlay(SFX_FAINT); return; }
  if (lg.inflicted) { sfxPlay(SFX_STATUS); return; }
  if (lg.damage && lg.effPct > 100) { sfxPlay(SFX_SUPER); return; }
  if (lg.damage) {
    sfxPlay(lg.move && MOVE_TBL[lg.move].cat == MC_SPEC ? SFX_BEAM : SFX_HIT);
    return;
  }
  if (lg.move && MOVE_TBL[lg.move].cat == MC_STATUS && !lg.missed) sfxPlay(SFX_STATUS);
}

static void btlNarrate(const Combatant &actor, const Combatant &target, const TurnLog &lg) {
  if (lg.skipped) return;
  btlSfxFor(lg);
  if (lg.hurtSelf) { btlSay(T(S_BTL_HURTSELF)); return; }
  if (lg.charged) { btlSay(T(S_BTL_USED), actor.name, moveName(lg.move)); return; }
  if (lg.move) btlSay(T(S_BTL_USED), actor.name, moveName(lg.move));
  if (lg.missed) { btlSay(T(S_BTL_MISS), actor.name); return; }
  if (lg.immune) { btlSay(T(S_BTL_IMMUNE)); return; }
  if (lg.crit) btlSay(T(S_BTL_CRIT));
  if (lg.damage && lg.effPct > 100) btlSay(T(S_BTL_SUPER));
  else if (lg.damage && lg.effPct < 100) btlSay(T(S_BTL_WEAK));
  if (lg.inflicted) {
    static const StrId AIL_STR[] = { S_AIL_PARA, S_AIL_PARA, S_AIL_BURN, S_AIL_POISON,
                                     S_AIL_SLEEP, S_AIL_FREEZE, S_AIL_CONFUSE };
    if (lg.inflicted < 7) btlSay(T(S_BTL_STATUS), target.name, T(AIL_STR[lg.inflicted]));
  }
  if (lg.targetFainted) btlSay(T(S_BTL_FAINT), target.name);
}

// Builds one opponent through Pet, so it gets the same stat formula and the
// same learnset-driven moveset the player's creatures do.
static void foeFromSpecies(Combatant &c, int16_t dex, uint8_t lvl, uint8_t iv) {
  Pet foe;
  foe.dbgHatchAs(dex, false);
  foe.ivAtk = foe.ivDef = foe.ivSpe = foe.ivHp = iv;
  foe.ageMinutes = (uint32_t)(lvl ? lvl - 1 : 0) * MINUTES_PER_LEVEL;
  foe.relearnFromLevel();
  combatantFromPet(c, foe);
}

// Your side: the live pet first, then the banked party.
//
// Both ladders cap your LEVEL to the leader's best, so a gym is always fought
// on its own terms and grinding is never the answer -- the type chart, the
// movesets and the choices are. Hard additionally caps your team SIZE to the
// leader's, so Brock is two-on-two. The caps are applied while BUILDING the
// combatants, so nothing is ever written back to the stored creature, exactly
// like ailments.
static void buildSquad(uint8_t maxLvl, uint8_t maxCount, uint16_t mask) {
  btlSquadN = 0;
  btlSquadAt = 0;
  btlPetIn = false;
  btlFought = 0;
  btlXpPaid = 0;
  if (maxCount > TRAINER_TEAM_MAX) maxCount = TRAINER_TEAM_MAX;
  if (!pet.isEgg() && btlSquadN < maxCount && (mask & 1)) {
    Pet tmp = pet;   // a copy: it cannot save, nor close the real pet's store (ownprefs.h)
    if (maxLvl && tmp.level() > maxLvl)
      tmp.ageMinutes = (uint32_t)(maxLvl - 1) * MINUTES_PER_LEVEL;
    btlSquadSlot[btlSquadN] = -1;
    combatantFromPet(btlSquad[btlSquadN++], tmp);
    btlPetIn = true;      // the training reward goes to whoever fought for it
  }
  for (int i = 0; i < PARTY_SLOTS && btlSquadN < maxCount; i++) {
    if (party.slots[i].empty() || !(mask & (1 << (i + 1)))) continue;
    PartyMon m = party.slots[i];
    if (maxLvl && m.level > maxLvl) m.level = maxLvl;
    btlSquadSlot[btlSquadN] = (int8_t)i;
    combatantFromParty(btlSquad[btlSquadN++], m);
  }
  if (btlSquadN) btlYou = btlSquad[0];
}

// How many you may bring: the leader's own count in hard mode, six otherwise.
uint8_t squadCap(uint8_t idx, bool hard) {
  if (idx >= TRAINER_COUNT) return TRAINER_TEAM_MAX;
  return hard ? TRAINERS[idx].count : TRAINER_TEAM_MAX;
}

// A fight against another device. The squads are already exchanged; the host
// owns resolution and the guest renders what it is sent.
void startLinkBattle() {
  if (!lan.mineN || !lan.theirsN) return;
  // Rebuilt from lan.mine, NOT from squadMask. What we fight with has to be
  // exactly what the peer was told we have -- rebuilding from the party would
  // silently diverge if anything changed between offering and starting.
  btlSquadN = 0;
  btlSquadAt = 0;
  for (uint8_t i = 0; i < lan.mineN && i < TRAINER_TEAM_MAX; i++)
    linkMonTo(btlSquad[btlSquadN++], lan.mine[i]);
  if (!btlSquadN) return;
  btlYou = btlSquad[0];
  btlLink = true;
  btlLinkHost = lan.isHost;
  btlTrainer = -1;
  btlWild = false;
  btlHard = false;
  btlFoeAt = 0;
  btlFoeSquadN = 0;
  for (uint8_t i = 0; i < lan.theirsN && i < TRAINER_TEAM_MAX; i++)
    linkMonTo(btlFoeSquad[btlFoeSquadN++], lan.theirs[i]);
  btlFoe = btlFoeSquad[0];
  btlMyAct = 0;
  btlMsgCount = 0;
  btlOver = false;
  btlWon = false;
  btlMenu = 0;
  btlWinUntil = 0;
  btlSwapWho = -1;
  btlFaintUntil[0] = btlFaintUntil[1] = 0;
  btlEnterUntil[0] = btlEnterUntil[1] = 0;
  btlHpShown[0] = btlYou.maxHp;
  btlHpShown[1] = btlFoe.maxHp;
  btlSyncSprite(0, btlYou);
  btlSyncSprite(1, btlFoe);
  audioMusic(MUS_BATTLE);
  battleOpen = true;
}

void startTrainerBattle(uint8_t idx, bool hard) {
  if (idx >= TRAINER_COUNT || pet.isEgg() || pet.ceremony != CER_NONE) return;
  const Trainer &tr = TRAINERS[idx];
  uint8_t top = 0;
  for (int k = 0; k < tr.count; k++)
    if (tr.team[k].level > top) top = tr.team[k].level;
  // BOTH ladders cap your level to the leader's best. Without it a L73 team
  // walks every trainer at 100% and the type chart never matters. Hard adds the
  // size cap on top, plus a smarter AI and better opposing IVs.
  buildSquad(top, hard ? tr.count : TRAINER_TEAM_MAX, squadMask);
  if (!btlSquadN) return;
  // The fight remembers its own region: the replacements, the badge, the win
  // screen and the arena all read BTL_TRAINERS. It was declared for this and
  // never set, so every ladder past Kanto chained into KANTO's team after its
  // first creature and paid out a Kanto badge.
  btlRegion = gymRegion;
  btlTrainer = (int8_t)idx;
  btlWild = false;
  btlHard = hard;
  btlFoeAt = 0;
  const Trainer &t = TRAINERS[idx];
  foeFromSpecies(btlFoe, t.team[0].dex, t.team[0].level, hard ? HARD_IV : EASY_IV);
  btlMsgCount = 0;
  btlOver = false;
  btlWon = false;
  btlMenu = 0;
  btlWinUntil = 0;
  btlSwapWho = -1;
  btlFaintUntil[0] = btlFaintUntil[1] = 0;
  btlEnterUntil[0] = btlEnterUntil[1] = 0;
  btlHpShown[0] = btlYou.maxHp;
  btlHpShown[1] = btlFoe.maxHp;
  btlSyncSprite(0, btlYou);
  btlSyncSprite(1, btlFoe);
  audioMusic(MUS_BATTLE);
  btlLungeUntil[0] = btlLungeUntil[1] = 0;
  btlHitUntil[0] = btlHitUntil[1] = 0;
  battleOpen = true;
}

void startBattle(int16_t dex, uint8_t lvl) {
  if (pet.isEgg() || pet.ceremony != CER_NONE) return;
  if (dex < 1 || dex > DEX_COUNT) return;
  buildSquad(0, TRAINER_TEAM_MAX, 0xFFFF);
  if (!btlSquadN) return;
  // The opponent is built through Pet so it gets the same stat formula and the
  // same learnset-driven moveset the player's creature does -- no special-cased
  // "enemy" maths that could quietly diverge.
  Pet foe;
  foe.dbgHatchAs(dex, false);
  foe.ivAtk = foe.ivDef = foe.ivSpe = foe.ivHp = 20;
  foe.ageMinutes = (uint32_t)(lvl ? lvl - 1 : 0) * MINUTES_PER_LEVEL;
  foe.relearnFromLevel();
  combatantFromPet(btlFoe, foe);
  btlTrainer = -1;      // a console fight is nobody's gym: no badge, no arena
  btlWild = false;
  btlBegin();
}

// A wild creature met while exploring. It is built whole (makeMon) BEFORE the
// fight and fought as that record, so the creature caught is the one fought --
// same IVs, same moves -- rather than a fresh roll made after the ball lands.
bool startWildBattle(int16_t dex, uint8_t lvl, uint8_t place) {
  if (pet.ceremony != CER_NONE || dex < 1 || dex > DEX_COUNT) return false;
  buildSquad(0, TRAINER_TEAM_MAX, 0xFFFF);
  if (!btlSquadN) return false;
  if (!makeMon(dex, lvl, random(WILD_SHINY_ONE_IN) == 0, btlWildMon)) return false;
  combatantFromParty(btlFoe, btlWildMon);
  btlTrainer = -1;
  btlLink = false;
  btlHard = false;
  btlWild = true;
  btlWildPlace = place;
  btlCaught = false;
  btlBegin();
  btlSay(T(S_EXP_APPEARS_FMT), dexName(dex));
  return true;
}

// What every one-off fight starts with once both sides are set.
static void btlBegin() {
  btlFxClear();
  btlMsgCount = 0;
  btlOver = false;
  btlWon = false;
  btlMenu = 0;
  btlWinUntil = 0;
  btlSwapWho = -1;
  btlFaintUntil[0] = btlFaintUntil[1] = 0;
  btlEnterUntil[0] = btlEnterUntil[1] = 0;
  btlHpShown[0] = btlYou.maxHp;
  btlHpShown[1] = btlFoe.maxHp;
  btlSyncSprite(0, btlYou);
  btlSyncSprite(1, btlFoe);
  audioMusic(MUS_BATTLE);
  btlLungeUntil[0] = btlLungeUntil[1] = 0;
  btlHitUntil[0] = btlHitUntil[1] = 0;
  battleOpen = true;
}

// The guest's whole turn: copy in what the host resolved and play the same
// animations the host is playing. It runs no battle logic at all -- that is the
// entire point of one side being authoritative (see link.h).
static void btlApplyResult() {
  if (lan.resultN < sizeof(LinkResult)) { lan.resultNew = false; return; }
  LinkResult r;
  memcpy(&r, lan.result, sizeof(r));
  lan.resultNew = false;

  // The wire says "host"/"guest"; here we are always the guest, so their fields
  // are the foe's and ours are ours.
  uint32_t now = millis();
  if (r.guestIdx < btlSquadN && r.guestIdx != btlSquadAt) {
    btlSquad[btlSquadAt] = btlYou;
    btlSquadAt = r.guestIdx;
    btlYou = btlSquad[btlSquadAt];
    btlSyncSprite(0, btlYou);
    btlLungeUntil[0] = btlHitUntil[0] = btlFaintUntil[0] = 0;
    btlEnterUntil[0] = now + BTL_ENTER_MS;
  }
  if (r.hostIdx != btlFoeAt && r.hostIdx < lan.theirsN) {
    btlFoeAt = r.hostIdx;
    linkMonTo(btlFoe, lan.theirs[btlFoeAt]);
    btlHpShown[1] = btlFoe.maxHp;
    btlSyncSprite(1, btlFoe);
    btlLungeUntil[1] = btlHitUntil[1] = btlFaintUntil[1] = 0;
    btlEnterUntil[1] = now + BTL_ENTER_MS;
  }
  btlYou.hp = r.guestHp > btlYou.maxHp ? btlYou.maxHp : r.guestHp;
  btlFoe.hp = r.hostHp > btlFoe.maxHp ? btlFoe.maxHp : r.hostHp;
  btlYou.ailment = r.guestAil;
  btlFoe.ailment = r.hostAil;

  btlMsgCount = 0;
  if (r.hostMove) btlSay(T(S_BTL_USED), btlFoe.name, moveName(r.hostMove));
  if (r.guestMove) btlSay(T(S_BTL_USED), btlYou.name, moveName(r.guestMove));
  if (r.guestDmg) { btlHitUntil[0] = now + BTL_HIT_MS; sfxPlay(SFX_HIT); }
  if (r.hostDmg) { btlHitUntil[1] = now + BTL_HIT_MS; sfxPlay(SFX_HIT); }
  if (btlYou.fainted()) {
    btlFaintUntil[0] = now + BTL_FAINT_MS;
    btlSay(T(S_BTL_FAINT), btlYou.name);
  }
  if (btlFoe.fainted()) {
    btlFaintUntil[1] = now + BTL_FAINT_MS;
    btlSay(T(S_BTL_FAINT), btlFoe.name);
  }
}

// Radio packets land on another task, so the guest picks them up here, once a
// frame, rather than rendering from inside an interrupt.
static void btlLinkPoll() {
  if (!btlLink) return;

  // A peer that stopped answering. Ending the fight is the only honest thing to
  // do -- there is no result coming, and pretending otherwise is the hang this
  // whole layer exists to remove.
  if (!lan.live() && !btlOver) {
    btlOver = true;
    btlWon = false;
    audioMusic(MUS_NONE);
    btlMsgCount = 0;
    btlSay("%s", T(S_LAN_GONE));
    return;
  }

  if (btlLinkHost) {
    // Our own action was latched when it was tapped; theirs arrives whenever
    // the radio manages it. Whichever is second sets the turn going.
    if (btlMyAct && lan.hasPeerAct() && !btlOver && !btlMsgCount &&
        btlSwapWho < 0) {
      uint8_t act = btlMyAct;
      btlMyAct = 0;
      if (LINK_ACT_IS_SWITCH(act)) btlSwitchTo(LINK_ACT_SLOT(act));
      else btlResolve(btlYou.moves[LINK_ACT_SLOT(act) % MOVE_SLOTS]);
    }
    return;
  }

  if (lan.resultNew) btlApplyResult();
  if (lan.state == LINK_DONE && !btlOver) {
    btlOver = true;
    btlWon = lan.youWon;
    audioMusic(btlWon ? MUS_VICTORY : MUS_NONE);
    if (btlWon) sfxPlay(SFX_VICTORY);
    btlSay("%s", btlWon ? T(S_BTL_WIN) : T(S_BTL_LOSE));
  }
}

// Packs the outcome for the guest. Only the host ever calls this.
static void btlShipResult(uint8_t yourMove, uint8_t theirMove,
                          uint16_t hp0You, uint16_t hp0Foe) {
  LinkResult r = {};
  r.hostHp = btlYou.hp;   r.guestHp = btlFoe.hp;
  r.hostAil = btlYou.ailment; r.guestAil = btlFoe.ailment;
  r.hostMove = yourMove;  r.guestMove = theirMove;
  r.hostDmg = (hp0You > btlYou.hp) ? hp0You - btlYou.hp : 0;
  r.guestDmg = (hp0Foe > btlFoe.hp) ? hp0Foe - btlFoe.hp : 0;
  r.hostIdx = btlSquadAt; r.guestIdx = btlFoeAt;
  if (btlYou.fainted() || btlFoe.fainted()) r.flags |= 0x04;
  lan.sendResult((const uint8_t *)&r, (uint8_t)sizeof(r));
}

// One exchange: both sides act in speed order, then burn/poison chip.
static void btlResolve(uint8_t yourMove) {
  TurnLog lg;
  // Against another device the opponent's move comes off the wire, never from
  // the AI -- and the host is the only side that runs this at all.
  uint8_t foeMove;
  bool foeSwitched = false;
  uint32_t now = millis();
  if (btlLink) {
    // Off the wire, never from the AI. A switch is carried in the same message
    // as a move, and like our own switch it costs the turn: they change, we act.
    uint8_t act = lan.pendingAct;
    if (LINK_ACT_IS_SWITCH(act)) {
      uint8_t to = LINK_ACT_SLOT(act);
      if (to < btlFoeSquadN && to != btlFoeAt && !btlFoeSquad[to].fainted()) {
        btlFoeSquad[btlFoeAt] = btlFoe;     // remember how battered it was
        btlFoeAt = to;
        btlFoe = btlFoeSquad[to];
        btlHpShown[1] = btlFoe.hp;
        btlSyncSprite(1, btlFoe);
        btlLungeUntil[1] = btlHitUntil[1] = btlFaintUntil[1] = 0;
        btlEnterUntil[1] = now + BTL_ENTER_MS;
        btlSay(T(S_BTL_SENDS), lan.peerName, btlFoe.name);
        foeSwitched = true;
      }
      foeMove = 0;
    } else {
      foeMove = btlFoe.moves[LINK_ACT_SLOT(act) % MOVE_SLOTS];
    }
    lan.pendingAct = 0;
  } else {
    foeMove = aiChooseMove(btlFoe, btlYou, btlHard);
  }
  (void)foeSwitched;

  bool youFirst = battleMovesFirst(btlYou, yourMove, btlFoe, foeMove);
  Combatant *a = youFirst ? &btlYou : &btlFoe;
  Combatant *b = youFirst ? &btlFoe : &btlYou;
  uint8_t ma = youFirst ? yourMove : foeMove;
  uint8_t mb = youFirst ? foeMove : yourMove;

  uint16_t hp0You = btlYou.hp, hp0Foe = btlFoe.hp;
  battleAct(*a, *b, ma, lg);
  btlNarrate(*a, *b, lg);
  btlFxMove(a == &btlYou ? 0 : 1, lg, now);
  if (lg.damage && !lg.hurtSelf) btlLungeUntil[a == &btlYou ? 0 : 1] = now + BTL_LUNGE_MS;
  if (!b->fainted()) {
    battleAct(*b, *a, mb, lg);
    btlNarrate(*b, *a, lg);
    btlFxMove(b == &btlYou ? 0 : 1, lg, now + FX_TRAVEL_MS + FX_IMPACT_MS);
    if (lg.damage && !lg.hurtSelf)
      btlLungeUntil[b == &btlYou ? 0 : 1] = now + BTL_LUNGE_MS + BTL_LUNGE_MS;
  }
  // whoever actually lost health flinches, whichever side dealt it
  if (btlYou.hp < hp0You) btlHitUntil[0] = now + BTL_HIT_MS;
  if (btlFoe.hp < hp0Foe) btlHitUntil[1] = now + BTL_HIT_MS;
  if (btlLink && btlLinkHost) btlShipResult(yourMove, foeMove, hp0You, hp0Foe);
  btlAfterTurn();
}

// The end of a turn, however it was spent -- two moves, or a ball that broke
// open and handed the foe a free hit: ailments tick, then whoever went down is
// replaced or the fight is over. One copy, so a ball turn cannot skip a rule
// a move turn obeys.
static void btlAfterTurn() {
  TurnLog lg;
  btlFought |= (uint8_t)(1u << btlSquadAt);   // whoever is out has fought
  if (!btlYou.fainted() && !btlFoe.fainted()) {
    battleEndTurn(btlYou, lg);
    if (lg.damage) btlNarrate(btlYou, btlYou, lg);
    battleEndTurn(btlFoe, lg);
    if (lg.damage) btlNarrate(btlFoe, btlFoe, lg);
  }
  if (btlFoe.fainted()) btlAwardXp();
  // Someone went down. The replacement is NOT swapped in here -- that made the
  // change instant and read as a jump cut. Flag it, let the sprite drop out of
  // frame, and swap when the player dismisses the message.
  if (btlFoe.fainted() && btlLink && btlFoeAt + 1 < btlFoeSquadN) {
    btlFaintUntil[1] = millis() + BTL_FAINT_MS;
    btlSwapWho = 1;
    return;
  }
  if (btlFoe.fainted() && btlTrainer >= 0 && btlFoeAt + 1 < BTL_TRAINERS[btlTrainer].count) {
    btlFaintUntil[1] = millis() + BTL_FAINT_MS;
    btlSwapWho = 1;
    return;
  }
  if (btlYou.fainted() && btlSquadAt + 1 < btlSquadN) {
    btlFaintUntil[0] = millis() + BTL_FAINT_MS;
    btlSwapWho = 0;
    return;
  }
  if (btlFoe.fainted() || btlYou.fainted()) {
    btlOver = true;
    btlWon = btlFoe.fainted();
    btlNewBadge = false;
    btlTrainGain = 0;
    if (btlWon && btlTrainer >= 0 && !pet.hasBadge(btlRegion, btlTrainer, btlHard)) {
      pet.winBadge(btlRegion, btlTrainer, btlHard);
      btlNewBadge = true;
    }
    // A badge and nothing else made the ladder a one-way checklist. A win now
    // trains the creature that fought for it -- so a leader you can already
    // beat is worth returning to. It goes to the LIVE pet only: banked members
    // are frozen at the level and training they were banked with, and battling
    // already costs the live pet energy, which is what rate-limits the grind
    // without needing a cooldown.
    if (btlWon && btlTrainer >= 0 && btlPetIn) {
      // Later leaders are worth more, and hard mode is worth roughly double.
      uint8_t amt = (btlHard ? 6 + random(5) : 3 + random(3)) + btlTrainer / 3;
      btlTrainGain = pet.rewardTraining(amt, btlTrainWhich);
    }
    audioMusic(btlWon ? MUS_VICTORY : MUS_NONE);
    if (btlWon) sfxPlay(SFX_VICTORY);
    // Tell the peer before anything else: if we stop here without sending, the
    // other device sits on a battle that will never take another turn.
    if (btlLink && btlLinkHost) lan.sendEnd(btlWon);
    if (btlLink) { btlSay("%s", btlWon ? T(S_BTL_WIN) : T(S_BTL_LOSE)); return; }
    if (btlWon && btlTrainer >= 0) { btlWinUntil = millis() + 60000; return; }
    // A wild creature knocked out is gone; the line says what would have kept it.
    if (btlWild && btlWon) { btlSay("%s", T(S_WILD_KO)); return; }
    btlSay("%s", T(S_BTL_LOSE));
  }
}

static void btlHpBar(int x, int y, int w, const Combatant &c, uint16_t shown) {
  int fw = c.maxHp ? (w - 4) * shown / c.maxHp : 0;
  uint16_t col = (shown * 2 > c.maxHp) ? UI_BAR_OK
                 : (shown * 4 > c.maxHp) ? UI_BAR_WARN : UI_BAR_BAD;
  gfx->fillRoundRect(x, y, w, 14, 4, UI_TRACK);
  if (fw > 0) gfx->fillRoundRect(x + 2, y + 2, fw, 10, 3, col);
  gfx->drawRoundRect(x, y, w, 14, 4, UI_INK);
}

static void btlSide(int tx, int ty, int sx, int sy, const Combatant &c, uint8_t who) {
  // the scenes are busy, so the name and bar sit on their own plate rather
  // than fighting the artwork for contrast
  int ph = (who == 0) ? 54 : 40;
  gfx->fillRoundRect(tx - 8, ty - 8, 158, ph, 8, UI_BG_DAY);
  gfx->drawRoundRect(tx - 8, ty - 8, 158, ph, 8, UI_INK);
  char l[28];
  snprintf(l, sizeof(l), "%s Lv.%u", c.name, c.level);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(1);
  gfx->setCursor(tx, ty);
  gfx->print(l);
  gfx->setTextColor(UI_BAR_WARN);
  gfx->setCursor(tx, ty + 14);
  gfx->print("HP");
  btlHpBar(tx + 18, ty + 12, 122, c, btlHpShown[who]);
  if (who == 0) {                 // your own numbers, as the games do
    char hp[16];
    snprintf(hp, sizeof(hp), "%u/%u", btlHpShown[who], c.maxHp);
    gfx->setTextColor(UI_INK);
    gfx->setCursor(tx + 140 - (int)strlen(hp) * 6, ty + 28);
    gfx->print(hp);
  }
  if (c.ailment != AIL_NONE) {   // a status is the thing you most need to see
    static const StrId AIL_STR[] = { S_AIL_PARA, S_AIL_PARA, S_AIL_BURN, S_AIL_POISON,
                                     S_AIL_SLEEP, S_AIL_FREEZE, S_AIL_CONFUSE };
    gfx->setTextColor(UI_BAR_BAD);
    gfx->setCursor(tx + 18, ty + 28);
    gfx->print(T(AIL_STR[c.ailment]));
  }
  // Caught: the ball sits where the creature stood, rocking for a moment.
  uint32_t now = millis();
  if (who == 1 && btlCatchHidesFoe(now)) return;     // inside the ball
  if (who == 1 && btlCaught && !btlCatchEnd) {
    int rock = now < btlBallUntil ? (((btlBallUntil - now) / 120) % 2 ? 4 : -4) : 0;
    drawBallIcon(sx + 24 + rock, sy + 60, 16);
    return;
  }
  // a platform under each creature, so they stand in the scene rather than
  // floating over it
  int ox = 0, oy = 0;
  bool flash = false;
  if (now < btlFaintUntil[who]) {          // sinks out of frame as it faints
    uint32_t left = btlFaintUntil[who] - now;
    oy += (int)((BTL_FAINT_MS - left) * 70 / BTL_FAINT_MS);
  } else if (c.fainted() && btlSwapWho == (int8_t)who) {
    return;                                // gone, waiting to be replaced
  }
  if (now < btlEnterUntil[who]) {          // and the next one rises into place
    uint32_t left = btlEnterUntil[who] - now;
    oy += (int)(left * 70 / BTL_ENTER_MS);
  }
  if (now < btlLungeUntil[who]) {          // lean in, then back out
    uint32_t left = btlLungeUntil[who] - now;
    int amt = (int)(left > BTL_LUNGE_MS / 2 ? BTL_LUNGE_MS - left : left) * 22 / (BTL_LUNGE_MS / 2);
    ox = who == 0 ? amt : -amt;            // you lunge right, the foe lunges left
    oy = who == 0 ? -amt / 2 : amt / 2;
  }
  if (now < btlHitUntil[who]) {
    uint32_t left = btlHitUntil[who] - now;
    ox += ((left / 50) % 2) ? 5 : -5;      // jitter
  }
  if (now < btlShakeUntil && who == btlShakeWho && now + 320 >= btlShakeUntil) {
    oy += ((now / 40) % 2) ? 6 : -6;       // super effective: it rattles
    ox += ((now / 30) % 2) ? 4 : -4;
  }

  // Real PMD playback when the sprite streamed: attack while lunging, hurt
  // while flinching, idle otherwise. `has()` guards every one, because not
  // every species ships every action -- falling through to idle, and to the
  // flat thumbnail if the sprite is missing entirely (no SD).
  if (btlPmd[who].loaded) {
    uint8_t act = PMD_IDLE;
    bool loop = true;
    uint32_t t = now;
    if (now < btlHitUntil[who] && btlPmd[who].has(PMD_HURT)) {
      act = PMD_HURT; loop = false; t = now - (btlHitUntil[who] - BTL_HIT_MS);
    } else if (now < btlLungeUntil[who] && btlPmd[who].has(PMD_ATTACK)) {
      act = PMD_ATTACK; loop = false; t = now - (btlLungeUntil[who] - BTL_LUNGE_MS);
    }
    drawPmdActM(btlPmd[who], act, sx + 24 + ox, sy + 78 + oy, t, loop, false, 4);
    return;
  }
  const uint8_t *th = thumbs.get(c.dex);
  if (!th) return;
  if (now < btlHitUntil[who]) flash = ((btlHitUntil[who] - now) / 60) % 2 == 0;
  drawThumb(th, sx + ox, sy + oy, 3, flash);
}

// Bars drain rather than snap: a hit that removes half your health should be
// visible as it happens, not as a value that was already different.
static void btlEaseBars() {
  if (btlCatchEnd) return;          // nothing has happened yet, as far as anyone can see
  const uint16_t real[2] = { btlYou.hp, btlFoe.hp };
  for (int i = 0; i < 2; i++) {
    int diff = (int)real[i] - (int)btlHpShown[i];
    if (!diff) continue;
    int step = diff / 5;
    if (!step) step = diff > 0 ? 1 : -1;
    btlHpShown[i] = (uint16_t)((int)btlHpShown[i] + step);
  }
}

// The moment the ladder builds toward. It used to be one more line in the same
// message box as "It's super effective!", with the badge awarded silently.
void renderWin() {
  const Trainer &t = BTL_TRAINERS[btlTrainer];
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);

  gfx->setTextColor(UI_BAR_WARN);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_BTL_WIN)) * 9, 54);
  gfx->print(T(S_BTL_WIN));

  char l[40];
  snprintf(l, sizeof(l), T(S_BTL_BEAT), t.name);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(l) * 6, 96);
  gfx->print(l);

  // the badge, large, with the hard-mode halo if that is how it was won
  if (btlTrainer < TRAINER_GYMS) {
    int by = 190;
    if (btlHard) {
      for (int r = 62; r >= 56; r--) gfx->drawCircle(CX, by, r, r % 2 ? 0xFEA0 : 0xFF60);
    }
    const BadgeArt *a = badgeArtFor(btlRegion, btlTrainer);
    if (a) {
      for (int r = 0; r < BADGE_PX; r++)
        for (int c = 0; c < BADGE_PX; c++) {
          uint8_t v = a->idx[r * BADGE_PX + c];
          if (v == 0xFF) continue;
          // 3x, so it reads as a prize rather than a list entry
          gfx->fillRect(CX - BADGE_PX * 3 / 2 + c * 3, by - BADGE_PX * 3 / 2 + r * 3,
                        3, 3, a->pal[v]);
        }
    } else {
      // This region has no badge art. Say so with a plain medal in the leader's
      // type colour rather than borrowing another region's badge.
      const Trainer &tr = TRAINER_SETS[btlRegion % GYM_REGIONS].list[btlTrainer];
      gfx->fillCircle(CX, by, 26, typeColor(tr.type));
      gfx->drawCircle(CX, by, 26, UI_INK);
      char n[4];
      snprintf(n, sizeof(n), "%u", (unsigned)(btlTrainer + 1));
      gfx->setTextSize(3);
      gfx->setTextColor(typeColorIsLight(tr.type) ? UI_INK : UI_WHITE);
      gfx->setCursor(CX - (int)strlen(n) * 9, by - 11);
      gfx->print(n);
    }
    if (btlNewBadge) {
      gfx->setTextColor(UI_BAR_OK);
      gfx->setTextSize(2);
      gfx->setCursor(CX - strlen(T(S_BTL_NEWBADGE)) * 6, 286);
      gfx->print(T(S_BTL_NEWBADGE));
    }
  }
  snprintf(l, sizeof(l), T(S_BADGES_FMT), pet.badgeCountIn(btlRegion, btlHard));
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(l) * 6, 316);
  gfx->print(l);

  // what the win was worth beyond the badge
  if (btlTrainGain) {
    static const StrId NAMES[3] = { S_TR_ATK, S_TR_DEF, S_TR_SPE };
    snprintf(l, sizeof(l), T(S_WIN_TRAIN_FMT),
             T(NAMES[btlTrainWhich % 3]), btlTrainGain);
    gfx->setTextColor(UI_BAR_OK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (int)strlen(l) * 6, 344);
    gfx->print(l);
  } else if (btlPetIn && btlTrainer >= 0) {
    gfx->setTextColor(UI_TRACK);
    gfx->setTextSize(1);
    gfx->setCursor(CX - (int)strlen(T(S_WIN_MAXED)) * 3, 348);
    gfx->print(T(S_WIN_MAXED));
  }

  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_BACK)) * 6, 380);
  gfx->print(T(S_BACK));
  gfx->flush();
}

void renderBattle() {
  if (btlWinUntil) { renderWin(); return; }
  if (btlCatchEnd && millis() >= btlCatchEnd) btlCatchFinish();
  btlEaseBars();
  gfx->fillRect(0, 0, 466, 30, RGB565_BLACK);   // not fillScreen: see renderExploreMap()
  drawBattleBack();
  // the lower band stays flat so the move grid and the HP text keep their
  // contrast against it
  gfx->fillRect(0, 254, 466, 212, UI_BG_DAY);

  // x=82 not 58: at y=60 the round bezel starts around x=77, and a longer
  // name like BLASTOISE was losing its first characters off the edge
  btlSide(82, 82, 300, 40, btlFoe, 1);    // foe reads top-left, sprite top-right
  btlSide(250, 190, 76, 168, btlYou, 0);  // you read bottom-right, sprite bottom-left
  btlDrawFx(millis());
  if (btlCatchEnd) { gfx->flush(); return; }   // the ball has the stage: no menu, no words yet

  // Waiting on the other device. Without this the screen is identical to the
  // one where it is your turn, so a tap that has been sent and a tap that was
  // never registered look exactly the same.
  bool lanWait = btlLink && !btlOver &&
                 (btlLinkHost ? (btlMyAct && !lan.hasPeerAct())
                              : (lan.state == LINK_WAITING));
  if (lanWait && !btlMsgCount) {
    gfx->fillRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H * 2 + 8, 12, UI_WHITE);
    gfx->drawRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H * 2 + 8, 12, UI_INK);
    gfx->setTextColor(UI_TRACK);
    gfx->setTextSize(1);
    const char *w = T(S_LAN_WAITFOE);
    gfx->setCursor(CX - (int)strlen(w) * 3, BTL_GRID_Y + 40);
    gfx->print(w);
  } else if (btlMsgCount) {            // narration takes over the menu area
    gfx->fillRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H * 2 + 8, 12, UI_WHITE);
    gfx->drawRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H * 2 + 8, 12, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(1);
    for (uint8_t i = 0; i < btlMsgCount && i < 4; i++) {
      gfx->setCursor(CX - (int)strlen(btlMsg[i]) * 3, BTL_GRID_Y + 14 + i * 18);
      gfx->print(btlMsg[i]);
    }
    gfx->setTextColor(UI_TRACK);
    gfx->setCursor(CX - 30, BTL_GRID_Y + 84);
    gfx->print("tap...");
  } else if (btlMenu == 0) {
    // FIGHT across the top, then POKEMON and RUN side by side. Three full-width
    // rows do not fit: the panel is round, and at that depth the chord is only
    // ~250 px. The lower two reuse the move grid's cells, so they inherit its
    // padded hit areas -- which is what made POKEMON hard to press before.
    if (btlWild) {
      // In the wild the top row splits: FIGHT, and the BALL beside it. Both
      // reuse the move grid's cells and so its padded hit areas.
      const char *top[2] = { T(S_FIGHT), T(S_BALL) };
      for (int i = 0; i < 2; i++) {
        int x = BTL_CELL_X(i), y = BTL_CELL_Y(i);
        gfx->fillRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, i ? UI_BAR_WARN : UI_BG_DAY);
        gfx->drawRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, UI_INK);
        gfx->setTextColor(UI_INK);
        gfx->setTextSize(2);
        gfx->setCursor(x + (BTL_CELL_W - (int)strlen(top[i]) * 12) / 2, y + 14);
        gfx->print(top[i]);
      }
    } else {
      gfx->fillRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H, 10, UI_BG_DAY);
      gfx->drawRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H, 10, UI_INK);
      gfx->setTextColor(UI_INK);
      gfx->setTextSize(2);
      gfx->setCursor(CX - (int)strlen(T(S_FIGHT)) * 6, BTL_GRID_Y + 14);
      gfx->print(T(S_FIGHT));
    }
    const char *low[2] = { T(S_BTL_SWITCH), T(S_BTL_RUN) };
    for (int i = 0; i < 2; i++) {
      int x = BTL_CELL_X(i + 2), y = BTL_CELL_Y(i + 2);
      gfx->fillRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, UI_TRACK);
      gfx->drawRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, UI_INK);
      gfx->setTextColor(UI_INK);
      gfx->setTextSize(2);
      gfx->setCursor(x + (BTL_CELL_W - (int)strlen(low[i]) * 12) / 2, y + 14);
      gfx->print(low[i]);
    }
  } else if (btlMenu == 2) {
    drawBtlBack();
    // who to bring on instead; the current one and anything fainted is inert
    for (uint8_t i = 0; i < btlSquadN && i < 4; i++) {
      int x = BTL_CELL_X(i), y = BTL_CELL_Y(i);
      const Combatant &m = (i == btlSquadAt) ? btlYou : btlSquad[i];
      bool usable = (i != btlSquadAt) && !m.fainted();
      gfx->fillRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, usable ? UI_BG_DAY : UI_TRACK);
      gfx->drawRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, usable ? UI_INK : 0x8410);
      gfx->setTextColor(usable ? UI_INK : 0x8410);
      gfx->setTextSize(1);
      gfx->setCursor(x + 10, y + 10);
      gfx->print(m.name);
      char hp[20];
      snprintf(hp, sizeof(hp), "%u/%u", m.hp, m.maxHp);
      gfx->setCursor(x + 10, y + 28);
      gfx->print(hp);
    }
  } else {
    drawBtlBack();
    for (int i = 0; i < MOVE_SLOTS; i++) {
      int x = BTL_CELL_X(i), y = BTL_CELL_Y(i);
      uint8_t mv = btlYou.moves[i];
      gfx->fillRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, mv ? UI_BG_DAY : UI_TRACK);
      gfx->drawRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, UI_INK);
      if (!mv) continue;
      gfx->setTextColor(UI_INK);
      gfx->setTextSize(1);
      gfx->setCursor(x + 10, y + 12);
      gfx->print(moveName(mv));
      // Same chip as the move list: in a fight the type IS the decision, and
      // grey 6px text was the least visible thing on the busiest screen.
      int cw = drawTypeChip(x + 10, y + 26, MOVE_TBL[mv].type);
      if (hasStab(btlYou.dex, MOVE_TBL[mv].type) &&
          MOVE_TBL[mv].cat != MC_STATUS) {
        gfx->setTextSize(1);
        gfx->setTextColor(DEX_TBL[btlYou.dex].accent);
        gfx->setCursor(x + 10 + cw + 4, y + 30);
        gfx->print("+");
      }
    }
  }
  gfx->flush();
}

// Brings on the flagged replacement and starts its entrance.
static void btlDoSwap() {
  uint32_t now = millis();
  if (btlSwapWho == 1 && btlLink) {
    btlFoeSquad[btlFoeAt] = btlFoe;
    // the next one still standing, not simply the next index
    uint8_t nxt = btlFoeAt;
    while (++nxt < btlFoeSquadN && btlFoeSquad[nxt].fainted()) {}
    if (nxt >= btlFoeSquadN) { btlSwapWho = -1; return; }
    btlFoeAt = nxt;
    btlFoe = btlFoeSquad[btlFoeAt];
    btlHpShown[1] = btlFoe.hp;
    btlSyncSprite(1, btlFoe);
    btlLungeUntil[1] = btlHitUntil[1] = btlFaintUntil[1] = 0;
    btlEnterUntil[1] = now + BTL_ENTER_MS;
    btlSay(T(S_BTL_SENDS), lan.peerName, btlFoe.name);
  } else if (btlSwapWho == 1) {
    const Trainer &t = BTL_TRAINERS[btlTrainer];
    btlFoeAt++;
    foeFromSpecies(btlFoe, t.team[btlFoeAt].dex, t.team[btlFoeAt].level,
                   btlHard ? HARD_IV : EASY_IV);
    btlHpShown[1] = btlFoe.maxHp;
    btlSyncSprite(1, btlFoe);
    btlLungeUntil[1] = btlHitUntil[1] = btlFaintUntil[1] = 0;
    btlEnterUntil[1] = now + BTL_ENTER_MS;
    btlSay(T(S_BTL_SENDS), t.name, btlFoe.name);
  } else if (btlSwapWho == 0) {
    btlSquad[btlSquadAt] = btlYou;     // remember how battered it was
    btlSquadAt++;
    btlYou = btlSquad[btlSquadAt];
    btlHpShown[0] = btlYou.hp;
    btlSyncSprite(0, btlYou);
    btlLungeUntil[0] = btlHitUntil[0] = btlFaintUntil[0] = 0;
    btlEnterUntil[0] = now + BTL_ENTER_MS;
    btlSay(T(S_BTL_GO), btlYou.name);
  }
  btlSwapWho = -1;
}

// Switching spends your turn: the opponent still acts. That is what stops it
// being a free look at the matchup every round.
static void btlSwitchTo(uint8_t i) {
  if (i >= btlSquadN || i == btlSquadAt) return;
  btlSquad[btlSquadAt] = btlYou;
  btlSquadAt = i;
  btlYou = btlSquad[i];
  btlHpShown[0] = btlYou.hp;
  btlSyncSprite(0, btlYou);
  btlLungeUntil[0] = btlHitUntil[0] = btlFaintUntil[0] = 0;
  btlEnterUntil[0] = millis() + BTL_ENTER_MS;
  btlMenu = 0;
  btlSay(T(S_BTL_GO), btlYou.name);
  btlResolve(0);          // move 0 = no attack, so only the foe acts
}

int btlCellIndexAt(int16_t x, int16_t y) {
  for (int i = 0; i < 4; i++)
    if (btlCellHit(i, x, y)) return i;
  return -1;
}

// The way out of the move and switch screens. Without it the only exits were
// choosing something or leaving the fight entirely.
static void drawBtlBack() {
  gfx->fillRoundRect(BTL_BACK_X, BTL_BACK_Y, BTL_BACK_W, BTL_BACK_H, 11, UI_TRACK);
  gfx->drawRoundRect(BTL_BACK_X, BTL_BACK_Y, BTL_BACK_W, BTL_BACK_H, 11, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(T(S_BACK)) * 6, BTL_BACK_Y + 14);
  gfx->print(T(S_BACK));
}

static bool btlBackTap(int16_t x, int16_t y) {
  if (x < BTL_BACK_X || x > BTL_BACK_X + BTL_BACK_W ||
      y < BTL_BACK_Y || y > BTL_BACK_Y + BTL_BACK_H) return false;
  btlMenu = 0;
  sfxPlay(SFX_TAP);
  return true;
}

// Running. A gym leader keeps their badge and the fight simply ends; against
// another device the peer is told, so it does not sit waiting for a move that
// will never come.
// Experience for a foe that went down (or into a ball), once per foe. All six
// of the party take a share -- the full one if they were out at any point in
// this fight, XP_BENCH_PCT of it if not -- up to the level the player has
// raised a creature to. The live creature is skipped: its level is its age.
// Nothing over the radio: a LAN fight is between friends, not a farm.
static void btlAwardXp() {
  if (btlLink || btlXpPaid == btlFoeAt + 1) return;
  btlXpPaid = btlFoeAt + 1;
  uint16_t full = Party::xpForFoe(btlFoe.level, btlTrainer >= 0);
  uint8_t cap = pet.xpCap();
  for (uint8_t s = 0; s < PARTY_SLOTS; s++) {
    if (party.slots[s].empty()) continue;
    bool fought = false;
    for (uint8_t k = 0; k < btlSquadN; k++)
      if (btlSquadSlot[k] == (int8_t)s && ((btlFought >> k) & 1)) fought = true;
    uint16_t amt = fought ? full : (uint16_t)(full * XP_BENCH_PCT / 100);
    const char *nm = party.slots[s].nick[0] ? party.slots[s].nick : nullptr;
    XpResult r = party.gainXp(s, amt, cap);
    const PartyMon &m = party.slots[s];
    if (r.levels) btlSay(T(S_GREW_FMT), nm ? nm : dexName(r.evolvedFrom ? r.evolvedFrom : m.dex), (unsigned)m.level);
    if (r.evolvedFrom) btlSay(T(S_EVOLVED_FMT), nm ? nm : dexName(r.evolvedFrom), dexName(m.dex));
  }
}

// The ball. It holds or it does not (catchChancePct: rarer and healthier is
// harder); if it does not, the throw WAS the turn and the foe acts alone.
static void btlThrowBall() {
  uint32_t now = millis();
  uint8_t pct = catchChancePct(btlFoe.dex, btlFoe.hp, btlFoe.maxHp, pet.kidMode);
  if ((uint8_t)random(100) < pct) {
    btlCaught = true;
    btlOver = true;
    btlWon = true;
    btlFought |= (uint8_t)(1u << btlSquadAt);
    btlAwardXp();                      // a catch is worth what a knockout is
    btlCatchStart(true, 3);            // music and the rocking ball wait for the click
    sfxPlay(SFX_TAP);
    btlSay(T(S_CATCH_OK_FMT), dexName(btlFoe.dex));
    return;
  }
  // It gets out: after 0-2 wobbles, then the foe answers -- its attack shown
  // once it is out of the ball, not while it is still inside.
  btlCatchStart(false, (uint8_t)random(3));
  sfxPlay(SFX_TAP);
  btlSay("%s", T(S_CATCH_FAIL));
  TurnLog lg;
  uint16_t hp0 = btlYou.hp;
  uint8_t fm = aiChooseMove(btlFoe, btlYou, false);
  battleAct(btlFoe, btlYou, fm, lg);
  btlNarrate(btlFoe, btlYou, lg);
  btlFxMove(1, lg, btlCatchEnd);
  if (lg.damage && !lg.hurtSelf) btlLungeUntil[1] = btlCatchEnd + BTL_LUNGE_MS;
  if (btlYou.hp < hp0) btlHitUntil[0] = btlCatchEnd + FX_TRAVEL_MS + BTL_HIT_MS;
  btlAfterTurn();
}

static void btlRun() {
  sfxPlay(SFX_DENY);
  btlFreeSprites();
  audioMusic(MUS_NONE);
  if (btlLink) { lanLeave(); btlLink = false; lanOpen = true; }
  battleOpen = false;
  btlMenu = 0;
}

void battleTap(int16_t x, int16_t y) {
  if (btlCatchEnd) { btlCatchFinish(); return; }   // skip the catch to its end
  if (btlWinUntil) {          // dismiss the win screen and leave the fight
    btlWinUntil = 0;
    btlFreeSprites();
    audioMusic(MUS_NONE);
    battleOpen = false;
    if (btlLink) { btlLink = false; lanOpen = true; }
    return;
  }
  if (btlMsgCount) {          // a tap clears the narration and returns the menu
    btlMsgCount = 0;
    if (btlOver) {
      btlFreeSprites();
      battleOpen = false;
      // Back to the LAN screen rather than all the way out: that is where a
      // rematch is offered, and re-pairing for every fight would be tedious.
      if (btlLink) { btlLink = false; lanOpen = true; }
      return;
    }
    if (btlSwapWho >= 0) btlDoSwap();   // the replacement arrives on this beat
    return;
  }
  if (btlMenu == 0) {
    if (btlWild) {
      if (btlCellHit(0, x, y)) { sfxPlay(SFX_TAP); btlMenu = 1; return; }
      if (btlCellHit(1, x, y)) { btlThrowBall(); return; }
    } else if (x >= BTL_GRID_X - BTL_HIT_PAD && x <= BTL_GRID_X + 328 + BTL_HIT_PAD &&
        y >= BTL_HIT_Y0(0) && y <= BTL_HIT_Y1(0)) {
      sfxPlay(SFX_TAP);
      btlMenu = 1;                       // FIGHT
      return;
    }
    if (btlCellHit(2, x, y)) { sfxPlay(SFX_TAP); btlMenu = 2; return; }
    if (btlCellHit(3, x, y)) { btlRun(); return; }
    return;
  }
  if (btlMenu == 2) {
    if (btlBackTap(x, y)) return;
    for (uint8_t i = 0; i < btlSquadN && i < 4; i++) {
      if (!btlCellHit(i, x, y)) continue;
      const Combatant &m = (i == btlSquadAt) ? btlYou : btlSquad[i];
      if (i == btlSquadAt || m.fainted()) { sfxPlay(SFX_DENY); return; }
      sfxPlay(SFX_TAP);
      if (btlLink && !btlLinkHost) {
        // The guest asks; it never switches on its own. A switch rides the same
        // message as a move, so the host spends the turn on it exactly as it
        // would for us.
        lan.sendAct(LINK_ACT_SWITCH_TO(i));
        btlMenu = 0;
        return;
      }
      if (btlLink) {            // host: latched like a move, see btlLinkPoll
        btlMyAct = LINK_ACT_SWITCH_TO(i);
        btlMenu = 0;
        if (!lan.hasPeerAct()) return;
        btlMyAct = 0;
      }
      btlSwitchTo(i);
      return;
    }
    btlMenu = 0;      // anywhere else backs out
    return;
  }
  if (btlBackTap(x, y)) return;
  for (int i = 0; i < MOVE_SLOTS; i++) {
    if (!btlYou.moves[i]) continue;
    if (!btlCellHit(i, x, y)) continue;
    sfxPlay(SFX_TAP);
    btlMenu = 0;
    if (btlLink && !btlLinkHost) {
      lan.sendAct(LINK_ACT_MOVE(i));   // the guest asks; the host decides
      return;
    }
    if (btlLink) {
      // Latched, not discarded. The host used to throw the tap away when the
      // rival had not chosen yet, so you had to keep jabbing at the move until
      // the timing happened to line up.
      btlMyAct = LINK_ACT_MOVE(i);
      if (!lan.hasPeerAct()) return;   // resolved by btlLinkPoll when it lands
      btlMyAct = 0;
    }
    btlResolve(btlYou.moves[i]);
    return;
  }
  btlMenu = 0;        // a tap off the grid goes back to FIGHT/POKEMON
}

// ---------- player card (swipe down) ----------
// Everything here is player-wide and outlives the creature: badges, the daily
// streak, the Pokedex and the party. No player sprite yet -- the SD carries
// PMD creature sprites only, so a trainer portrait needs new art.
// Page 1: who you are -- avatar, badges, totals. Tap the avatar to cycle it;
// the four sprites are hand-drawn (see species.h) because SpriteCollab has no
// trainer art and ripped sprites would be unlicensed.
static void renderPlayerBadges() {
  // the player's name if they have set one, the generic title if not; either
  // way tapping it opens the keyboard
  const char *tn = pet.trainerName[0] ? pet.trainerName : T(S_TRAINER);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - (int)strlen(tn) * 9, 40);
  gfx->print(tn);
  // A pencil after the name: the tap that renames was there all along, and
  // nothing on screen said so. Only on page 0, the one the tap is wired to.
  if (playerBadgeRegion == 0) {
    int px = CX + (int)strlen(tn) * 9 + 10, py = 40;
    for (int k = 0; k < 4; k++) gfx->drawLine(px + 3 + k, py + 18, px + 15 + k, py + 4, UI_BAR_WARN);
    gfx->fillTriangle(px, py + 22, px + 3, py + 15, px + 7, py + 19, UI_INK);   // the point
    gfx->fillRect(px + 15, py + 1, 4, 4, UI_BAR_BAD);                           // the rubber
  }

  // Pages 1 and 2 are the other regions' ladders: name them, and drop the
  // avatar so the badges have the room. Only page 0 is "you".
  if (playerBadgeRegion != 0) {
    const char *rn = TRAINER_SETS[playerBadgeRegion].region;
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (int)strlen(rn) * 6, 120);
    gfx->print(rn);
  } else if (gShowAllAvatars) {          // emulator only: every avatar at once
    for (uint8_t i = 0; i < AVATAR_COUNT; i++)
      drawAvatar(i, 60 + (i % 4) * 88, 60 + (i / 4) * 60, 3);
  } else {
    drawAvatar(pet.avatar, CX - AVATAR_PX * 2, 72, 4);
    // the sprite alone is not always obvious at 16x16, so it is named
    const char *an = AVATARS[pet.avatar % AVATAR_COUNT].name;
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(1);
    gfx->setCursor(CX - (int)strlen(an) * 3, 143);
    gfx->print(an);
    gfx->setTextColor(UI_TRACK);
    gfx->setCursor(CX - (int)strlen(T(S_AVATAR_HINT)) * 3, 154);
    gfx->print(T(S_AVATAR_HINT));
  }

  // The real badges, 2x4. Unearned ones draw as a faint outline so the shape
  // of what is missing is still visible.
  for (int i = 0; i < TRAINER_GYMS; i++) {
    int bx = 140 + (i % 4) * 62, by = 188 + (i / 4) * 62;
    bool got = pet.hasBadge(playerBadgeRegion, i, false);
    bool hard = pet.hasBadge(playerBadgeRegion, i, true);
    if (hard) {
      // Beaten on hard: a golden halo. Concentric rings, not a filled disc --
      // a disc sat behind the art and read as a gold coin rather than a glow.
      gfx->drawCircle(bx, by, 25, 0xFDE0);
      gfx->drawCircle(bx, by, 24, 0xFEA0);
      gfx->drawCircle(bx, by, 23, 0xFF60);
      gfx->drawCircle(bx, by, 22, 0xFEA0);
      gfx->drawCircle(bx, by, 21, 0xFDE0);
    }
    if (!got) {
      gfx->drawCircle(bx, by, 20, UI_TRACK);
      continue;
    }
    const BadgeArt *a = badgeArtFor(playerBadgeRegion, i);
    if (!a) {                 // no art for this region: a filled disc, won is won
      gfx->fillCircle(bx, by, 14, typeColor(TRAINER_SETS[playerBadgeRegion % GYM_REGIONS].list[i].type));
      gfx->drawCircle(bx, by, 14, UI_INK);
      continue;
    }
    for (int r = 0; r < BADGE_PX; r++)
      for (int c = 0; c < BADGE_PX; c++) {
        uint8_t v = a->idx[r * BADGE_PX + c];
        if (v == 0xFF) continue;
        gfx->fillRect(bx - BADGE_PX / 2 + c, by - BADGE_PX / 2 + r, 1, 1, a->pal[v]);
      }
  }
  char l[32];
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  snprintf(l, sizeof(l), T(S_STREAK_FMT), pet.streak, pet.bestStreak);
  gfx->setCursor(CX - (int)strlen(l) * 6, 286);
  gfx->print(l);
  snprintf(l, sizeof(l), T(S_POKEDEX_FMT), pet.registeredCount(), dexObtainable());
  gfx->setCursor(CX - (int)strlen(l) * 6, 312);
  gfx->print(l);
  snprintf(l, sizeof(l), T(S_PARTY_FMT), party.count());
  gfx->setCursor(CX - (int)strlen(l) * 6, 338);
  gfx->print(l);
}

// Page 2: the medals. They used to sit on the creature's card; they belong with
// the player, since totalMedals accumulates across every pet you raise.
static void renderPlayerMedals() {
  int got = 0;
  for (int i = 0; i < MED_COUNT; i++)
    if (pet.hasMedal(1 << i)) got++;
  char head[24];
  snprintf(head, sizeof(head), T(S_MEDALS_FMT), got, MED_COUNT);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(head) * 9, 44);
  gfx->print(head);

  for (int i = 0; i < MED_COUNT; i++) {
    int x = 46 + (i % 2) * 190, y = 96 + (i / 2) * 58;
    bool g = pet.hasMedal(1 << i);
    gfx->fillRoundRect(x, y, 180, 48, 10, g ? UI_BAR_OK : UI_TRACK);
    gfx->drawRoundRect(x, y, 180, 48, 10, UI_INK);
    gfx->setTextColor(g ? UI_BG_DAY : UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(x + (180 - (int)strlen(medalLabel(i)) * 12) / 2, y + 6);
    gfx->print(medalLabel(i));
    gfx->setTextSize(1);
    gfx->setCursor(x + (180 - (int)strlen(medalDesc(i)) * 6) / 2, y + 30);
    gfx->print(medalDesc(i));
  }
  char tot[28];
  snprintf(tot, sizeof(tot), T(S_MEDALS_TOTAL_FMT), pet.totalMedals);
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(1);
  gfx->setCursor(CX - (int)strlen(tot) * 3, 344);
  gfx->print(tot);
}

void renderPlayer() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  if (playerPage < GYM_REGIONS) renderPlayerBadges();
  else if (playerPage == GYM_REGIONS) renderPlayerMedals();
  else renderPlayerStickers();

  for (uint8_t i = 0; i < PLAYER_PAGES; i++) {
    int dx = CX - (PLAYER_PAGES - 1) * 13 + i * 26;
    if (i == playerPage) gfx->fillCircle(dx, 366, 5, UI_INK);
    else gfx->drawCircle(dx, 366, 4, UI_INK);
  }
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_BACK)) * 6, 392);
  gfx->print(T(S_BACK));
  gfx->flush();
}

// ---------- reaction test (trains SPEED) ----------
// The third training verb. The bag is a masher and the ball is a juggler, so
// this one is a reaction: a target appears somewhere on the panel and you tap
// it before it expires. The window shrinks as you go, which is what makes it
// read as speed rather than as endurance.
#define SPD_MS 15000UL       // session length
#define SPD_LIFE0 1100       // first target's window, ms
#define SPD_LIFE_MIN 380
#define SPD_R 46             // target radius

void spdSpawn() {
  // Keep the whole target inside the bezel: pick an angle and a radius that
  // leave SPD_R of margin, rather than a square that clips at the corners.
  int ang = random(360);
  int rad = random(150);
  float a = ang * 3.14159f / 180.0f;
  spdX = CX + (int)(cosf(a) * rad);
  spdY = CY + (int)(sinf(a) * rad);
  spdBorn = millis();
}

void startSpeedGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  spdOpen = true;
  spdUntil = millis() + SPD_MS;
  spdOverUntil = 0;
  spdHits = 0;
  spdMisses = 0;
  spdGain = 0;
  spdNewHi = false;
  spdSpawn();
}

static uint16_t spdLife() {
  int life = SPD_LIFE0 - spdHits * 32;
  return life < SPD_LIFE_MIN ? SPD_LIFE_MIN : life;
}

void spdTap(int16_t x, int16_t y) {
  if (spdOverUntil) return;
  int dx = x - spdX, dy = y - spdY;
  if (dx * dx + dy * dy <= (SPD_R + 14) * (SPD_R + 14)) {
    spdHits++;
    sfxPlay(SFX_TAP);
    spdSpawn();
  }
}

void renderSpeed() {
  uint32_t now = millis();
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  if (spdOverUntil) {
    if (now > spdOverUntil) { spdOpen = false; return; }
    char b[24];
    snprintf(b, sizeof(b), T(S_SCORE_FMT), spdHits);
    gfx->setTextColor(ink);
    gfx->setTextSize(4);
    gfx->setCursor(CX - strlen(b) * 12, 150);
    gfx->print(b);
    char g[20];
    snprintf(g, sizeof(g), T(S_SPD_GAIN_FMT), spdGain);
    gfx->setTextColor(UI_BAR_WARN);
    gfx->setTextSize(3);
    gfx->setCursor(CX - strlen(g) * 9, 210);
    gfx->print(g);
    gfx->setTextSize(2);
    if (spdNewHi && spdHits > 0) {
      gfx->setTextColor(UI_BAR_WARN);
      gfx->setCursor(CX - strlen(T(S_NEW_RECORD)) * 6, 256);
      gfx->print(T(S_NEW_RECORD));
    } else {
      char r[20];
      snprintf(r, sizeof(r), T(S_RECORD_FMT), pet.spdHi);
      gfx->setTextColor(ink);
      gfx->setCursor(CX - strlen(r) * 6, 256);
      gfx->print(r);
    }
    gfx->flush();
    return;
  }

  // session over?
  if (now >= spdUntil) {
    spdNewHi = (spdHits > pet.spdHi);
    spdGain = pet.trainSpeed(spdHits);
    sfxPlay(spdNewHi ? SFX_MEDAL : SFX_PLAY);
    spdOverUntil = now + 3500;
    gfx->flush();
    return;
  }
  // target expired?
  if (now - spdBorn > spdLife()) {
    spdMisses++;
    spdSpawn();
  }

  // the target, shrinking as its window runs out so the urgency is visible
  uint32_t age = now - spdBorn;
  int life = spdLife();
  int r = SPD_R - (int)((uint32_t)SPD_R * age / (life ? life : 1) / 2);
  if (r < 8) r = 8;
  gfx->fillCircle(spdX, spdY, r, UI_BAR_BAD);
  gfx->fillCircle(spdX, spdY, r * 2 / 3, UI_WHITE);
  gfx->fillCircle(spdX, spdY, r / 3, UI_BAR_BAD);

  char b[12];
  snprintf(b, sizeof(b), "%u", spdHits);
  gfx->setTextColor(ink);
  gfx->setTextSize(4);
  gfx->setCursor(CX - strlen(b) * 12, 30);
  gfx->print(b);
  // seconds left
  uint32_t left = (spdUntil > now) ? (spdUntil - now + 999) / 1000 : 0;
  snprintf(b, sizeof(b), "%us", (unsigned)left);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(b) * 6, 76);
  gfx->print(b);
  gfx->flush();
}

// ---------- team select ----------
// Which creatures come to this fight. It exists because hard mode caps your
// team to the leader's size, so the difference between a sweep and a wipe is
// bringing the right type -- and the squad used to be simply whoever sat first
// in the party.

// candidate n: 0 = the live pet, 1..PARTY_SLOTS = banked members
bool pickExists(uint8_t n) {
  if (n == 0) return !pet.isEgg();
  return n <= PARTY_SLOTS && !party.slots[n - 1].empty();
}
uint8_t pickChosen() {
  uint8_t c = 0;
  for (uint8_t n = 0; n <= PARTY_SLOTS; n++)
    if (pickExists(n) && (squadMask & (1 << n))) c++;
  return c;
}
uint8_t pickCandidates() {
  uint8_t c = 0;
  for (uint8_t n = 0; n <= PARTY_SLOTS; n++)
    if (pickExists(n)) c++;
  return c;
}
// Trims the selection to the first `cap` candidates. The default used to be
// "everything", which with a live pet plus six banked is seven against a cap of
// six -- so the screen opened already invalid.
void pickDefault(uint8_t cap) {
  squadMask = 0;
  uint8_t taken = 0;
  for (uint8_t n = 0; n <= PARTY_SLOTS && taken < cap; n++)
    if (pickExists(n)) { squadMask |= (1 << n); taken++; }
}

static void drawPickCell(uint8_t n, int x, int y, uint8_t capLvl) {
  bool on = (squadMask & (1 << n)) != 0;
  int16_t dex; uint16_t lvl; const char *nm; bool shiny;
  if (n == 0) {
    dex = pet.speciesId; lvl = pet.level(); shiny = pet.shiny;
    nm = pet.nick[0] ? pet.nick : dexName(dex);
  } else {
    const PartyMon &m = party.slots[n - 1];
    dex = m.dex; lvl = m.level; shiny = m.shiny;
    nm = m.nick[0] ? m.nick : dexName(dex);
  }
  if (capLvl && lvl > capLvl) lvl = capLvl;   // show the level it will FIGHT at
  gfx->fillRoundRect(x, y, PICK_CELL_W, PICK_CELL_H, 10, on ? UI_BG_DAY : UI_TRACK);
  gfx->drawRoundRect(x, y, PICK_CELL_W, PICK_CELL_H, 10, on ? UI_INK : 0x8410);
  const uint8_t *th = thumbs.get(dex);
  if (th) drawThumb(th, x - 12, y - 6, 2, !on);
  gfx->setTextColor(on ? UI_INK : 0x8410);
  gfx->setTextSize(1);
  gfx->setCursor(x + 54, y + 14);
  gfx->print(nm);
  char l[16];
  snprintf(l, sizeof(l), "Lv.%u%s", (unsigned)lvl, shiny ? " *" : "");
  gfx->setCursor(x + 54, y + 30);
  gfx->print(l);
  // its typing is the whole reason you are on this screen
  const DexEntry &d = DEX_TBL[dex];
  gfx->setTextColor(on ? d.accent : 0x8410);
  gfx->setCursor(x + 54, y + 48);
  gfx->print(typeName(d.type1));
  if (d.type2 != T_NONE) {
    gfx->setCursor(x + 54, y + 60);
    gfx->print(typeName(d.type2));
  }
  if (on) {
    gfx->fillCircle(x + PICK_CELL_W - 16, y + 16, 9, UI_BAR_OK);
    gfx->setTextColor(UI_BG_DAY);
    gfx->setCursor(x + PICK_CELL_W - 19, y + 13);
    gfx->print("*");
  }
}

void renderPick() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  uint8_t cap = squadCap(pickTrainer, pickHard);
  uint8_t top = 0;          // the level cap shown on each cell; 0 = uncapped
  char head[40];
  if (pickTrainer == PICK_LAN) {
    snprintf(head, sizeof(head), "%s: %s", T(S_LAN),
             lanWantHost ? T(S_LAN_HOST) : T(S_LAN_JOIN));
  } else {
    const Trainer &t = TRAINERS[pickTrainer];
    for (int k = 0; k < t.count; k++)
      if (t.team[k].level > top) top = t.team[k].level;
    snprintf(head, sizeof(head), "%s  Lv.%u x%u", t.name, top, t.count);
  }
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(head) * 6, 44);
  gfx->print(head);
  char sub[28];
  snprintf(sub, sizeof(sub), T(S_PICK_FMT), pickChosen(), cap);
  gfx->setTextSize(1);
  gfx->setTextColor(pickChosen() > cap ? UI_BAR_BAD : UI_TRACK);
  gfx->setCursor(CX - (int)strlen(sub) * 3, 68);
  gfx->print(sub);

  uint8_t seen = 0, drawn = 0;
  for (uint8_t n = 0; n <= PARTY_SLOTS; n++) {
    if (!pickExists(n)) continue;
    if (seen++ < pickPage * PICK_PER_PAGE) continue;
    if (drawn >= PICK_PER_PAGE) break;
    drawPickCell(n, PICK_X(drawn), PICK_Y(drawn), top);
    drawn++;
  }
  uint8_t pages = (pickCandidates() + PICK_PER_PAGE - 1) / PICK_PER_PAGE;
  if (!pages) pages = 1;
  for (uint8_t i = 0; i < pages && pages > 1; i++) {
    int dx = CX - (pages - 1) * 13 + i * 26;
    if (i == pickPage) gfx->fillCircle(dx, 332, 5, UI_INK);
    else gfx->drawCircle(dx, 332, 4, UI_INK);
  }

  bool ok = pickChosen() > 0 && pickChosen() <= cap;
  gfx->fillRoundRect(PICK_BACK_X, PICK_GO_Y, PICK_BTN_W, PICK_BTN_H, 12, UI_TRACK);
  gfx->drawRoundRect(PICK_BACK_X, PICK_GO_Y, PICK_BTN_W, PICK_BTN_H, 12, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(PICK_BACK_X + (PICK_BTN_W - (int)strlen(T(S_BACK)) * 12) / 2,
                 PICK_GO_Y + 14);
  gfx->print(T(S_BACK));
  gfx->fillRoundRect(PICK_GO_X, PICK_GO_Y, PICK_BTN_W, PICK_BTN_H, 12,
                     ok ? UI_BAR_OK : UI_TRACK);
  gfx->drawRoundRect(PICK_GO_X, PICK_GO_Y, PICK_BTN_W, PICK_BTN_H, 12, UI_INK);
  gfx->setTextColor(ok ? UI_BG_DAY : 0x8410);
  gfx->setTextSize(2);
  gfx->setCursor(PICK_GO_X + (PICK_BTN_W - (int)strlen(T(S_FIGHT)) * 12) / 2,
                 PICK_GO_Y + 14);
  gfx->print(T(S_FIGHT));
  gfx->flush();
}

void pickTap(int16_t x, int16_t y) {
  if (y >= PICK_GO_Y && y <= PICK_GO_Y + PICK_BTN_H &&
      x >= PICK_BACK_X && x <= PICK_BACK_X + PICK_BTN_W) {   // BACK
    sfxPlay(SFX_TAP);
    pickOpen = false;
    if (pickTrainer == PICK_LAN) { lanOpen = true; }
    else { gymOpen = true; }
    return;
  }
  if (y >= PICK_GO_Y && y <= PICK_GO_Y + PICK_BTN_H &&
      x >= PICK_GO_X && x <= PICK_GO_X + PICK_BTN_W) {
    uint8_t cap = squadCap(pickTrainer, pickHard);
    if (pickChosen() == 0 || pickChosen() > cap) return;   // GO stays inert
    sfxPlay(SFX_TAP);
    pickOpen = false;
    if (pickTrainer == PICK_LAN) {
      // The squad is chosen BEFORE the radio comes up, so what gets offered to
      // the peer is what the player picked -- lanOffer() builds lan.mine from
      // squadMask, and the fight is then rebuilt from lan.mine rather than from
      // the party (see startLinkBattle).
      lanOffer(lanWantHost);
      lanOpen = true;
      return;
    }
    startTrainerBattle(pickTrainer, pickHard);
    return;
  }
  uint8_t seen = 0, drawn = 0;
  for (uint8_t n = 0; n <= PARTY_SLOTS; n++) {
    if (!pickExists(n)) continue;
    if (seen++ < pickPage * PICK_PER_PAGE) continue;
    if (drawn >= PICK_PER_PAGE) break;
    int cx0 = PICK_X(drawn), cy0 = PICK_Y(drawn);
    drawn++;
    if (x < cx0 || x > cx0 + PICK_CELL_W || y < cy0 || y > cy0 + PICK_CELL_H) continue;
    squadMask ^= (1 << n);
    sfxPlay(SFX_TAP);
    return;
  }
}

// ---------- LAN battle ----------
// Pairing on a touch-only screen: one device hosts, the other joins, and the
// protocol does the rest. There is no MAC entry because there is no keyboard
// worth typing one on.
void renderLan() {
  if (tradeOpen) {
    if (!lan.ready()) tradeOpen = false;      // the link went: back to say so
    else { renderTrade(); return; }
  }
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  const char *title = lanForTrade ? T(S_TRADE) : T(S_LAN);
  gfx->setCursor(CX - strlen(title) * 6, 44);
  gfx->print(title);

  const char *msg = T(S_LAN_PICK);
  switch (lan.state) {
    case LINK_HANDSHAKE:
    case LINK_LISTENING: msg = T(S_LAN_WAIT); break;
    case LINK_SQUADS:    msg = T(S_LAN_WAIT); break;
    case LINK_READY:     msg = T(S_LAN_READY); break;
    case LINK_REFUSED:   msg = T(S_LAN_REFUSED); break;
    case LINK_LOST:      msg = T(S_LAN_GONE); break;
    case LINK_DONE:      msg = lan.youWon ? T(S_BTL_WIN) : T(S_BTL_LOSE); break;
    default: break;
  }
  gfx->setTextColor((lan.state == LINK_REFUSED || lan.state == LINK_LOST)
                      ? UI_BAR_BAD : UI_TRACK);
  gfx->setTextSize(1);
  gfx->setCursor(CX - (int)strlen(msg) * 3, 76);
  gfx->print(msg);

  if (lan.state == LINK_OFF || lan.state == LINK_REFUSED ||
      lan.state == LINK_LOST) {
    const char *lab[2] = { T(S_LAN_HOST), T(S_LAN_JOIN) };
    for (int i = 0; i < 2; i++) {
      int y = 120 + i * 70;
      gfx->fillRoundRect(90, y, 286, 56, 12, UI_BG_DAY);
      gfx->drawRoundRect(90, y, 286, 56, 12, UI_INK);
      gfx->setTextColor(UI_INK);
      gfx->setTextSize(2);
      gfx->setCursor(CX - (int)strlen(lab[i]) * 6, y + 20);
      gfx->print(lab[i]);
    }
  } else if (lan.state == LINK_READY) {
    char l[40];
    if (lan.peerName[0]) {
      gfx->setTextColor(UI_INK);
      gfx->setTextSize(2);
      gfx->setCursor(CX - (int)strlen(lan.peerName) * 6, 130);
      gfx->print(lan.peerName);
    }
    snprintf(l, sizeof(l), T(S_LAN_VS), lan.theirsN);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (int)strlen(l) * 6, 150);
    gfx->print(l);
    gfx->fillRoundRect(120, 220, 226, 56, 12, UI_BAR_OK);
    gfx->drawRoundRect(120, 220, 226, 56, 12, UI_INK);
    gfx->setTextColor(UI_BG_DAY);
    gfx->setCursor(CX - strlen(T(S_FIGHT)) * 6, 240);
    gfx->print(T(S_FIGHT));
    gfx->fillRoundRect(120, 290, 226, 56, 12, UI_BAR_WARN);
    gfx->drawRoundRect(120, 290, 226, 56, 12, UI_INK);
    gfx->setTextColor(UI_WHITE);
    gfx->setCursor(CX - strlen(T(S_TRADE)) * 6, 310);
    gfx->print(T(S_TRADE));
  } else if (lan.state == LINK_DONE) {
    // Both squads are still in hand on both devices, so going again costs one
    // packet -- there is nothing to re-exchange.
    if (lan.peerName[0]) {
      gfx->setTextColor(UI_INK);
      gfx->setTextSize(2);
      gfx->setCursor(CX - (int)strlen(lan.peerName) * 6, 140);
      gfx->print(lan.peerName);
    }
    gfx->fillRoundRect(120, 220, 226, 56, 12, UI_BG_DAY);
    gfx->drawRoundRect(120, 220, 226, 56, 12, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - strlen(T(S_LAN_REMATCH)) * 6, 240);
    gfx->print(T(S_LAN_REMATCH));
  }
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_BACK)) * 6, 392);
  gfx->print(T(S_BACK));
  gfx->flush();
}

// ---------- trading ----------
// One creature for one, over a READY link, from the LAN screen's TRADE button.
// The protocol's half (who holds what, when) is in link.h; this is the
// screen, and the one place a trade touches the party: tradePoll().

int8_t tradeSrc = -1;            // what we offered: 0..5 party, 6.. box
bool tradeStored = false;        // theirs is in our party or box
char tradeMsg[48] = "";
#define TRADE_PER_PAGE 6
#define TRADE_CELL_X(i) (73 + ((i) % 3) * 110)
#define TRADE_CELL_Y(i) (104 + ((i) / 3) * 112)
#define TRADE_BTN_Y 300
#define TRADE_BTN_H 46

// Who can be offered: everyone in the party, then the box. Never the live
// creature -- it is alive, not a record.
uint8_t tradeCands(int8_t *out) {
  uint8_t n = 0;
  for (int i = 0; i < PARTY_SLOTS; i++) if (!party.slots[i].empty()) out[n++] = (int8_t)i;
  for (int i = 0; i < BOX_SLOTS; i++) if (!party.box[i].empty()) out[n++] = (int8_t)(PARTY_SLOTS + i);
  return n;
}
static PartyMon *tradeMonAt(int8_t src) {
  if (src < 0) return nullptr;
  return src < PARTY_SLOTS ? &party.slots[src] : &party.box[src - PARTY_SLOTS];
}
uint8_t tradePages() {
  int8_t c[PARTY_SLOTS + BOX_SLOTS];
  uint8_t n = tradeCands(c);
  return n ? (uint8_t)((n + TRADE_PER_PAGE - 1) / TRADE_PER_PAGE) : 1;
}
static bool tradeRoom() { return party.firstFree() >= 0 || party.boxFirstFree() >= 0; }

void tradeOpenScreen() {
  lan.tradeReset();
  tradeOpen = true;
  tradePicking = true;
  tradePage = 0;
  tradeSrc = -1;
  tradeStored = false;
  tradeMsg[0] = 0;
}

// Leaving the trade screen. Before the deal is struck it simply withdraws; once
// theirs is stored here (phase 1) there is nothing left to undo, and tradePoll()
// still finishes the job from loop() whatever screen is up.
void tradeClose() {
  if (!lan.tPhase) lan.tradeWithdraw();
  tradeOpen = false;
}

// Runs every pass the link is up, not only while the trade screen shows, so a
// deal finishes even if the player has walked away from it.
//   1. theirs arrived: store it (evolving if it trade-evolves), into the
//      Pokedex, the party or else the box;
//   2. they have stored ours: only now is ours given up.
// That order is the "never lose a creature" rule -- see link.h.
void tradePoll() {
  if (lan.tArrived) {
    lan.tArrived = false;
    PartyMon m = lan.tTheirs;
    int16_t from = m.dex, evo = tradeEvolution(m.dex);
    pet.registerCaught(from, m.shiny);
    if (evo) { m.dex = evo; pet.registerCaught(evo, m.shiny); }
    tradeStored = party.add(m) || party.boxAdd(m);
    if (evo) snprintf(tradeMsg, sizeof(tradeMsg), T(S_EVOLVED_FMT), dexName(from), dexName(evo));
    else snprintf(tradeMsg, sizeof(tradeMsg), T(S_TRADE_DONE_FMT), m.nick[0] ? m.nick : dexName(m.dex));
    Serial.printf("trade: got %s (dex %d)%s -- %s\n", DEX_TBL[m.dex].name, m.dex,
                  evo ? " evolved on arrival" : "", tradeStored ? "stored" : "NO ROOM");
    cryPlay(m.dex);
  }
  if (lan.tPhase == 1 && lan.tPeerDone && tradeStored) {
    PartyMon *mine = tradeMonAt(tradeSrc);
    // the record we offered, still where it was: released, and the deal is done
    if (mine && mine->dex == lan.tMine.dex) {
      if (tradeSrc < PARTY_SLOTS) party.releaseAt((uint8_t)tradeSrc);
      else party.boxReleaseAt((uint8_t)(tradeSrc - PARTY_SLOTS));
    }
    Serial.printf("trade: gave %s (dex %d)\n", DEX_TBL[lan.tMine.dex].name, lan.tMine.dex);
    lan.tPhase = 2;
    tradeSrc = -1;
  }
}

static void tradeCard(const PartyMon &m, int cx, int y) {
  const uint8_t *t = thumbs.get(m.dex);
  if (t) drawThumb(t, cx - GAL_CELL / 2, y, 2, false);
  const char *nm = m.nick[0] ? m.nick : dexName(m.dex);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(1);
  gfx->setCursor(cx - (int)strlen(nm) * 3, y + 84);
  gfx->print(nm);
  char lv[24];
  snprintf(lv, sizeof(lv), T(S_NAME_FMT), m.shiny ? "*" : "", "", (unsigned)m.level);
  gfx->setCursor(cx - (int)strlen(lv) * 3, y + 96);
  gfx->print(lv);
}

static void tradeButton(int x, int w, const char *l, bool live, uint16_t col) {
  gfx->fillRoundRect(x, TRADE_BTN_Y, w, TRADE_BTN_H, 12, live ? col : UI_TRACK);
  gfx->drawRoundRect(x, TRADE_BTN_Y, w, TRADE_BTN_H, 12, UI_INK);
  gfx->setTextColor(live ? UI_WHITE : 0x8410);
  gfx->setTextSize(2);
  gfx->setCursor(x + w / 2 - (int)strlen(l) * 6, TRADE_BTN_Y + 16);
  gfx->print(l);
}

// The two OK/CHANGE buttons, in one place for the draw and the tap.
static bool tradeCanChange() { return !lan.tPhase && !lan.tIAccepted; }
static bool tradeCanOk() {
  if (lan.tPhase == 2) return true;                 // "done": back to choosing
  return !lan.tPhase && lan.tradeHasTheirs() && !lan.tIAccepted && tradeRoom();
}

void renderTrade() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(T(S_TRADE)) * 6, 40);
  gfx->print(T(S_TRADE));
  if (lan.peerName[0]) {
    gfx->setTextColor(UI_TRACK);
    gfx->setTextSize(1);
    gfx->setCursor(CX - (int)strlen(lan.peerName) * 3, 64);
    gfx->print(lan.peerName);
  }

  if (tradePicking) {
    int8_t c[PARTY_SLOTS + BOX_SLOTS];
    uint8_t n = tradeCands(c);
    const char *hint = n ? T(S_TRADE_PICK) : T(S_TRADE_NONE);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(1);
    gfx->setCursor(CX - (int)strlen(hint) * 3, 82);
    gfx->print(hint);
    for (uint8_t i = 0; i < TRADE_PER_PAGE; i++) {
      uint8_t k = tradePage * TRADE_PER_PAGE + i;
      if (k >= n) break;
      int x = TRADE_CELL_X(i), y = TRADE_CELL_Y(i);
      gfx->drawRoundRect(x, y, 100, 106, 10, UI_TRACK);
      tradeCard(*tradeMonAt(c[k]), x + 50, y + 2);
    }
    uint8_t pages = tradePages();
    if (pages > 1) {
      char pg[12];
      snprintf(pg, sizeof(pg), "%u/%u", (unsigned)tradePage + 1, (unsigned)pages);
      gfx->setTextColor(UI_TRACK);
      gfx->setTextSize(2);
      gfx->setCursor(CX - (int)strlen(pg) * 6, 336);
      gfx->print(pg);
    }
  } else {
    // the deal: ours on the left, theirs on the right
    const char *hd[2] = { T(S_TRADE_GIVE), T(S_TRADE_GET) };
    for (int s = 0; s < 2; s++) {
      int x = s ? 246 : 50;          // inside the round bezel at y 90
      gfx->drawRoundRect(x, 90, 170, 132, 14, s ? UI_BAR_OK : UI_INK);
      gfx->setTextColor(UI_INK);
      gfx->setTextSize(1);
      gfx->setCursor(x + 85 - (int)strlen(hd[s]) * 3, 96);
      gfx->print(hd[s]);
    }
    tradeCard(lan.tMine, 135, 106);
    if (lan.tradeHasTheirs()) tradeCard(lan.tTheirs, 331, 106);
    else {
      gfx->setTextColor(UI_TRACK);
      gfx->setTextSize(4);
      gfx->setCursor(331 - 12, 140);
      gfx->print("?");
    }
    const char *st = "";
    uint16_t sc = UI_INK;
    if (lan.tPhase) st = tradeMsg;
    else if (!tradeRoom()) { st = T(S_PARTY_FULL); sc = UI_BAR_BAD; }
    else if (lan.tIAccepted) st = T(S_TRADE_WAIT);
    else if (lan.tPeerAccepted) { st = T(S_TRADE_THEYOK); sc = UI_BAR_OK; }
    gfx->setTextColor(sc);
    gfx->setTextSize(1);
    gfx->setCursor(CX - (int)strlen(st) * 3, 268);
    gfx->print(st);
    if (lan.tPhase == 1 && !lan.tPeerDone) {
      gfx->setCursor(CX - (int)strlen(T(S_TRADE_WAIT)) * 3, 282);
      gfx->print(T(S_TRADE_WAIT));
    }
    tradeButton(70, 150, T(S_TRADE_CHANGE), tradeCanChange(), UI_BAR_WARN);
    tradeButton(246, 150, "OK", tradeCanOk(), UI_BAR_OK);
  }
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_BACK)) * 6, 392);
  gfx->print(T(S_BACK));
  gfx->flush();
}

void tradeTap(int16_t x, int16_t y) {
  if (y > 370) { sfxPlay(SFX_TAP); tradeClose(); return; }      // back
  if (tradePicking) {
    int8_t c[PARTY_SLOTS + BOX_SLOTS];
    uint8_t n = tradeCands(c);
    for (uint8_t i = 0; i < TRADE_PER_PAGE; i++) {
      uint8_t k = tradePage * TRADE_PER_PAGE + i;
      if (k >= n) break;
      int cx = TRADE_CELL_X(i), cy = TRADE_CELL_Y(i);
      if (x < cx || x > cx + 100 || y < cy || y > cy + 106) continue;
      sfxPlay(SFX_TAP);
      tradeSrc = c[k];
      lan.tradeOffer(*tradeMonAt(c[k]));
      tradePicking = false;
      return;
    }
    return;
  }
  if (y < TRADE_BTN_Y - 6 || y > TRADE_BTN_Y + TRADE_BTN_H + 6) return;
  if (x >= 64 && x <= 226) {                                     // CHANGE
    if (!tradeCanChange()) { sfxPlay(SFX_DENY); return; }
    sfxPlay(SFX_TAP);
    lan.tradeWithdraw();
    tradeSrc = -1;
    tradePicking = true;
  } else if (x >= 240 && x <= 402) {                             // OK
    if (!tradeCanOk()) { sfxPlay(SFX_DENY); return; }
    sfxPlay(SFX_TAP);
    if (lan.tPhase == 2) tradeOpenScreen();                      // done: another?
    else lan.tradeAccept();
  }
}

// a horizontal swipe pages the candidates, and never closes the screen
bool tradeSwipe(int dir) {
  if (!tradePicking) return true;
  int p = (int)tradePage + (dir > 0 ? -1 : 1);
  uint8_t pages = tradePages();
  if (p < 0) p = pages - 1;
  if (p >= pages) p = 0;
  tradePage = (uint8_t)p;
  return true;
}

// Hands our chosen squad to the link, then announces.
// Leaving deliberately: tell the peer so it reports at once instead of sitting
// out the timeout, then free the radio -- it costs real current.
void lanLeave() {
  lanForTrade = false;
  if (lan.live()) lan.sendBye();
  linkNowEnd();
  lan.state = LINK_OFF;
}

static void lanOffer(bool host) {
  // The squad is built BEFORE the radio is touched. What we advertise has to be
  // exactly what the player just chose in the picker, and that does not depend
  // on whether the radio comes up -- doing it the other way round meant a
  // failed radio skipped the squad entirely and left nothing to inspect.
  lan.begin(host, pet.trainerName);
  snprintf(lan.peerName, sizeof(lan.peerName), "%s", pet.trainerName);
  buildSquad(0, TRAINER_TEAM_MAX, squadMask);
  for (uint8_t i = 0; i < btlSquadN; i++) {
    LinkMon m;
    linkMonFrom(m, btlSquad[i]);
    lan.addMon(m);
  }
  if (!linkNowBegin(&lan)) {          // no radio: say so rather than hanging
    lan.state = LINK_REFUSED;
    return;
  }
  // BOTH sides announce. Which of them ends up hosting is settled by id inside
  // the hello, so the buttons are only a preference -- two players who both tap
  // HOST still get a working fight instead of two authorities, and two who both
  // tap JOIN still get one instead of mutual silence.
  lan.start();
}

void lanTap(int16_t x, int16_t y) {
  if (tradeOpen) { tradeTap(x, y); return; }
  if (lan.state == LINK_OFF || lan.state == LINK_REFUSED ||
      lan.state == LINK_LOST) {
    for (int i = 0; i < 2; i++) {
      int by = 120 + i * 70;
      if (x < 90 || x > 376 || y < by || y > by + 56) continue;
      lanWantHost = (i == 0);
      if (lanForTrade) {
        // Pairing to trade needs no squad choice: the link still wants one to
        // say hello with, so it takes the default -- nobody fights with it.
        pickTrainer = PICK_LAN;
        pickDefault(squadCap(PICK_LAN, false));
        if (!pickChosen()) { sfxPlay(SFX_DENY); return; }   // an egg and an empty party
        sfxPlay(SFX_TAP);
        lanOffer(lanWantHost);
        return;
      }
      sfxPlay(SFX_TAP);
      lanOpen = false;
      pickTrainer = PICK_LAN;
      pickHard = false;
      pickPage = 0;
      pickDefault(squadCap(PICK_LAN, false));
      pickOpen = true;
      return;
    }
  } else if (lan.state == LINK_READY) {
    if (x >= 120 && x <= 346 && y >= 220 && y <= 276) {
      sfxPlay(SFX_TAP);
      lanOpen = false;
      startLinkBattle();
      return;
    }
    if (x >= 120 && x <= 346 && y >= 290 && y <= 346) {
      sfxPlay(SFX_TAP);
      tradeOpenScreen();
      return;
    }
  } else if (lan.state == LINK_DONE) {
    if (x >= 120 && x <= 346 && y >= 220 && y <= 276) {
      sfxPlay(SFX_TAP);
      lan.sendRematch();      // both sides go back to READY and tap FIGHT
      return;
    }
  }
  if (y > 370) { lanLeave(); lanOpen = false; }   // back
}

// ---------- exploring ----------
// The menu's EXPLORE row opens a map of the island; a place on it opens that
// place's scene, where hiding spots stir one at a time. Tapping the one that
// moves spends an outing (pet.expSpend) and meets whoever lives there
// (pet.rollWild): usually a wild fight, sometimes it takes fright and is gone.
// A caught creature goes to the POKEDEX BOX (dexbox.h), one per species; a
// species already there asks which of the two to keep.

bool exploreOpen = false;
int8_t expPlace = -1;            // -1 = the map, else PL_*
uint8_t expActive = 0xFF;        // the spot that is stirring
uint32_t expActiveUntil = 0, expNextAt = 0;
char expMsg[48] = "";
uint32_t expMsgUntil = 0;
bool expKeepAsk = false;         // a catch of a species the box already holds
PartyMon expCatch;

#define EXP_SPOTS 5
// On the ground of every scene (the backdrop spans y 30..254 at 2x), far enough
// apart that a finger cannot be on two at once.
static const int16_t EXP_SPOT_X[EXP_SPOTS] = { 112, 192, 276, 356, 232 };
static const int16_t EXP_SPOT_Y[EXP_SPOTS] = { 208, 170, 214, 176, 244 };
#define EXP_SPOT_R 38              // hit radius: generous, these are for children
#define EXP_STIR_MS 1900           // how long a spot stirs
#define EXP_GRACE_MS 350           // ...and how late a tap still counts
#define EXP_BACK_Y 372             // the BACK button under the scene
#define EXP_BACK_H 46
#define EXP_PIN_R 30               // map pin hit radius
#define EXP_TOP_H 70               // the map's title plate: tap to leave
// ...and the round back arrow beside it, which says so. Asked for from a child's
// point of view: a plate that silently leaves when tapped is not a way out
// anyone finds, and the swipe is invisible.
#define EXP_EXIT_X (CX - 120)
#define EXP_EXIT_Y 46
#define EXP_EXIT_R 22

enum : uint8_t { SPOT_GRASS, SPOT_SAND, SPOT_ROCK, SPOT_WATER, SPOT_SNOW, SPOT_LAVA };
static uint8_t spotKind(uint8_t pl) {
  switch (pl) {
    case PL_MEADOW: case PL_FOREST: return SPOT_GRASS;
    case PL_BEACH: return SPOT_SAND;
    case PL_OCEAN: return SPOT_WATER;
    case PL_SNOW: return SPOT_SNOW;
    case PL_VOLCANO: return SPOT_LAVA;
    default: return SPOT_ROCK;     // mountain, cave
  }
}

static const StrId PLACE_STR[PLACE_COUNT] = {
  S_PL_MEADOW, S_PL_BEACH, S_PL_FOREST, S_PL_VOLCANO, S_PL_MOUNTAIN, S_PL_SNOW,
  S_PL_OCEAN, S_PL_CAVE,
};
static_assert(MAP_PIN_COUNT == PLACE_COUNT, "the map needs a pin for every place");

// Someone to fight with: the live creature once hatched, or anyone banked.
static bool expHasTeam() { return !pet.isEgg() || party.count() > 0; }

static void expSay(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(expMsg, sizeof(expMsg), fmt, ap);
  va_end(ap);
  expMsgUntil = millis() + 2200;
}

void openExplore() {
  exploreOpen = true;
  expPlace = -1;
  expKeepAsk = false;
  expMsgUntil = 0;
}

// Every way out of exploring comes through here. The fanfare after a catch is
// welcome on the map; it is not welcome following the player home.
void closeExplore() {
  exploreOpen = false;
  expPlace = -1;
  audioMusic(MUS_NONE);
}

// One hiding spot. `stir` is how far through its stirring it is (0 = still).
static void drawSpot(uint8_t kind, int x, int y, bool stir, uint32_t now) {
  int dx = stir ? (int)(sinf(now * 0.025f) * 4) : 0;
  switch (kind) {
    case SPOT_GRASS: {
      // a tuft of blades; stirring, they sway and a leaf flies off
      for (int i = -4; i <= 4; i++) {
        int bx = x + i * 5, top = y - 26 + abs(i) * 3;
        int sway = stir ? dx * (y - top) / 26 : 0;
        uint16_t c = (i & 1) ? 0x2C64 : 0x3E86;
        gfx->fillTriangle(bx - 4, y, bx + 4, y, bx + sway, top, c);
      }
      gfx->drawFastHLine(x - 24, y, 49, 0x1A62);
      if (stir) gfx->fillCircle(x + 16 + (int)((now / 40) % 20), y - 30 - (int)((now / 60) % 12), 3, 0x5F28);
      break;
    }
    case SPOT_SAND:
      gfx->fillEllipse(x + dx, y - 6, 26, 10, 0xE6B2);
      gfx->fillEllipse(x + dx - 4, y - 9, 16, 5, 0xF738);
      gfx->fillTriangle(x + dx + 8, y - 12, x + dx + 18, y - 12, x + dx + 13, y - 20, 0xFB2C);  // a shell
      if (stir) for (int k = 0; k < 3; k++) gfx->fillCircle(x - 20 + k * 20, y - 18 - (int)((now / 50 + k * 7) % 14), 2, 0xE6B2);
      break;
    case SPOT_ROCK:
      gfx->fillEllipse(x + dx, y - 12, 26, 16, 0x7BCF);
      gfx->fillEllipse(x + dx - 6, y - 18, 14, 7, 0xA534);
      gfx->drawEllipse(x + dx, y - 12, 26, 16, 0x3186);
      if (stir) for (int k = 0; k < 4; k++) gfx->fillCircle(x - 28 + k * 18, y - 2 - (int)((now / 70 + k * 5) % 8), 2, 0xBDD7);
      break;
    case SPOT_WATER: {
      int r = stir ? 26 + (int)((now / 60) % 6) : 24;
      gfx->drawEllipse(x, y - 4, r, 7, 0xDF7F);
      gfx->drawEllipse(x, y - 4, r - 10, 4, 0xAEDF);
      if (stir) for (int k = 0; k < 3; k++)
        gfx->drawCircle(x - 8 + k * 8, y - 12 - (int)((now / 45 + k * 9) % 22), 2 + k % 2, 0xF7FF);
      break;
    }
    case SPOT_SNOW:
      gfx->fillEllipse(x + dx, y - 10, 26, 14, 0xFFFF);
      gfx->fillEllipse(x + dx + 6, y - 6, 18, 6, 0xC6FF);
      if (stir) for (int k = 0; k < 4; k++) gfx->fillCircle(x - 24 + k * 16, y - 22 - (int)((now / 55 + k * 6) % 16), 2, 0xFFFF);
      break;
    default:   // lava
      gfx->fillEllipse(x, y - 4, 26, 9, 0x4000);
      gfx->fillEllipse(x, y - 5, 21, 6, stir ? 0xFD20 : 0xE200);
      if (stir) {
        int b = (int)((now / 50) % 18);
        gfx->fillCircle(x - 6, y - 8 - b, 4 - b / 6, 0xFEA0);
        gfx->fillCircle(x + 9, y - 6 - (b + 9) % 18, 3, 0xFD20);
      }
      break;
  }
}

static void drawExploreCharges(int cy) {
  for (int i = 0; i < EXP_MAX; i++) {
    int cx = CX + (i - (EXP_MAX - 1) / 2) * 30 - ((EXP_MAX % 2) ? 0 : 15);
    if (i < pet.expCharges()) drawBallIcon(cx, cy, 10);
    else { gfx->fillCircle(cx, cy, 10, UI_TRACK); gfx->drawCircle(cx, cy, 10, 0x8410); }
  }
}

static void drawExploreNext(int y) {
  char t[32];
  if (pet.expCharges() >= EXP_MAX) return;
  uint16_t m = pet.expMinutesToNext();
  snprintf(t, sizeof(t), T(S_EXP_NEXT_FMT), m / 60, m % 60);
  gfx->setTextSize(1);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(CX - (int)strlen(t) * 3, y);
  gfx->print(t);
}

// NO fillScreen(BLACK) before drawing, on any of these: the panel's DMA can
// still be sending the previous frame from this buffer, and a slow draw over
// black flashes the AMOLED black -- the reason drawScene() and renderGame()
// never clear first. The map covers all 466x466; the scenes cover all but the
// top band, which is filled on its own.
static void renderExploreMap() {
  drawBack(MAP_ART, 0);
  // the title plate: what this is, and the outings in hand
  gfx->fillRoundRect(CX - 96, 12, 192, 58, 14, UI_WHITE);
  gfx->drawRoundRect(CX - 96, 12, 192, 58, 14, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(T(S_EXPLORE)) * 6, 20);
  gfx->print(T(S_EXPLORE));
  drawExploreCharges(52);
  gfx->fillCircle(EXP_EXIT_X, EXP_EXIT_Y, EXP_EXIT_R, UI_WHITE);
  gfx->drawCircle(EXP_EXIT_X, EXP_EXIT_Y, EXP_EXIT_R, UI_INK);
  gfx->fillTriangle(EXP_EXIT_X - 10, EXP_EXIT_Y, EXP_EXIT_X + 6, EXP_EXIT_Y - 11,
                    EXP_EXIT_X + 6, EXP_EXIT_Y + 11, UI_INK);
  for (uint8_t p = 0; p < PLACE_COUNT; p++) {
    int x = MAP_PINS[p].x * 2, y = MAP_PINS[p].y * 2;
    bool open = placeCount(p) > 0;
    if (open) drawBallIcon(x, y, 11);
    else { gfx->fillCircle(x, y, 11, UI_TRACK); gfx->drawCircle(x, y, 11, RGB565_BLACK); }
    const char *nm = T(PLACE_STR[p]);
    int w = (int)strlen(nm) * 6 + 8;
    gfx->fillRoundRect(x - w / 2, y + 14, w, 13, 4, open ? UI_INK : 0x8410);
    gfx->setTextSize(1);
    gfx->setTextColor(UI_WHITE);
    gfx->setCursor(x - w / 2 + 4, y + 17);
    gfx->print(nm);
  }
  // the bottom plate: the next outing, a reason, or nothing
  const char *line = nullptr;
  if (!expHasTeam()) line = T(S_EXP_NOTEAM);
  else if (millis() < expMsgUntil) line = expMsg;
  if (line || pet.expCharges() < EXP_MAX) {
    gfx->fillRoundRect(CX - 120, 412, 240, 30, 10, UI_WHITE);
    gfx->drawRoundRect(CX - 120, 412, 240, 30, 10, UI_INK);
    if (line) {
      gfx->setTextSize(1);
      gfx->setTextColor(UI_BAR_BAD);
      gfx->setCursor(CX - (int)strlen(line) * 3, 423);
      gfx->print(line);
    } else drawExploreNext(423);
  }
  gfx->flush();
}

static void renderExploreScene() {
  uint32_t now = millis();
  // the spots take turns: one stirs, then a pause, then another
  if (now >= expNextAt) {
    uint8_t s;
    do { s = (uint8_t)random(EXP_SPOTS); } while (s == expActive && EXP_SPOTS > 1);
    expActive = s;
    expActiveUntil = now + EXP_STIR_MS;
    expNextAt = expActiveUntil + 700 + random(1400);
  }
  gfx->fillRect(0, 0, 466, 30, RGB565_BLACK);
  drawBack(placeScene((uint8_t)expPlace, sceneIsNight()), 30);
  gfx->fillRect(0, 254, 466, 212, UI_BG_DAY);
  uint8_t kind = spotKind((uint8_t)expPlace);
  for (uint8_t i = 0; i < EXP_SPOTS; i++)
    drawSpot(kind, EXP_SPOT_X[i], EXP_SPOT_Y[i], i == expActive && now < expActiveUntil, now);

  // the place's name on a plate, top centre
  const char *nm = T(PLACE_STR[expPlace]);
  int w = (int)strlen(nm) * 12 + 20;
  gfx->fillRoundRect(CX - w / 2, 40, w, 28, 8, UI_WHITE);
  gfx->drawRoundRect(CX - w / 2, 40, w, 28, 8, UI_INK);
  gfx->setTextSize(2);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(CX - (int)strlen(nm) * 6, 47);
  gfx->print(nm);

  drawExploreCharges(282);
  const char *line = (now < expMsgUntil) ? expMsg
                   : (pet.expCharges() ? T(S_EXP_HINT) : T(S_EXP_EMPTY));
  gfx->setTextSize(2);
  gfx->setTextColor(now < expMsgUntil ? UI_BAR_BAD : UI_INK);
  int tw = (int)strlen(line) * 12;
  if (tw > 380) gfx->setTextSize(1), tw = (int)strlen(line) * 6;
  gfx->setCursor(CX - tw / 2, 310);
  gfx->print(line);
  if (!pet.expCharges()) drawExploreNext(340);

  gfx->fillRoundRect(CX - 90, EXP_BACK_Y, 180, EXP_BACK_H, 12, UI_TRACK);
  gfx->drawRoundRect(CX - 90, EXP_BACK_Y, 180, EXP_BACK_H, 12, UI_INK);
  gfx->setTextSize(2);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(CX - (int)strlen(T(S_BACK)) * 6, EXP_BACK_Y + 15);
  gfx->print(T(S_BACK));

  if (expKeepAsk) {
    char q[40], o[20], n[20];
    snprintf(q, sizeof(q), T(S_KEEP_Q_FMT), dexName(expCatch.dex));
    PartyMon old;
    dexBoxGet(expCatch.dex, old);
    snprintf(o, sizeof(o), T(S_KEEP_OLD_FMT), (unsigned)old.level);
    snprintf(n, sizeof(n), T(S_KEEP_NEW_FMT), (unsigned)expCatch.level);
    drawConfirmPanel(q, nullptr, nullptr, UI_INK, o, UI_TRACK, UI_INK, n, UI_BAR_OK, UI_WHITE);
  }
  gfx->flush();
}

void renderExplore() {
  if (expPlace < 0) renderExploreMap();
  else renderExploreScene();
}

// After a wild fight closes, however it closed: a catch is kept, a draw or a
// loss is simply over. Runs from render() so no close path can skip it.
void exploreAfterBattle() {
  if (!btlWild || battleOpen) return;
  btlWild = false;
  if (!btlCaught) return;
  btlCaught = false;
  expCatch = btlWildMon;
  pet.registerCaught(expCatch.dex, expCatch.shiny);
  // Said on the console every time, because "it was not in the Pokedex" is not
  // something anyone can act on without knowing which of these happened.
  if (!dexBoxReady()) {
    // no storage: the old way, the team then the box, or it cannot be kept
    bool kept = party.add(expCatch) || party.boxAdd(expCatch);
    if (!kept) expSay("%s", T(S_GIFT_FULL));
    const char *where = kept ? "went to the party/box" : "NOT KEPT";
    Serial.printf("catch: %s (dex %d) -- Pokedex box NOT mounted, %s\n", DEX_TBL[expCatch.dex].name, expCatch.dex, where);
    return;
  }
  if (dexBoxHas(expCatch.dex)) {
    expKeepAsk = true;
    Serial.printf("catch: %s (dex %d) -- already in the box, asking\n", DEX_TBL[expCatch.dex].name, expCatch.dex);
    return;
  }
  bool put = dexBoxPut(expCatch);
  unsigned lv = expCatch.level, n = dexBoxCount();
  Serial.printf("catch: %s (dex %d) lv %u -- Pokedex box %s (%u stored)\n", DEX_TBL[expCatch.dex].name, expCatch.dex, lv, put ? "ok" : "WRITE FAILED", n);
  if (!put && (party.add(expCatch) || party.boxAdd(expCatch))) return;   // never lose a catch
  expSay(T(S_STORED_FMT), dexName(expCatch.dex));
}

static void exploreEncounter() {
  if (!pet.expCharges()) { sfxPlay(SFX_DENY); expSay("%s", T(S_EXP_EMPTY)); return; }
  int16_t d = pet.rollWild((uint8_t)expPlace);
  if (!d) { sfxPlay(SFX_DENY); expSay("%s", T(S_EXP_NOBODY)); return; }
  pet.expSpend();
  pet.markSeen(d);
  pet.saveNow();
  expActiveUntil = 0;
  expNextAt = millis() + 1200;
  if ((int)random(100) < EXP_FLEE_PCT) {
    cryOr(d, SFX_DENY);
    expSay(T(S_EXP_FLED_FMT), dexName(d));
    return;
  }
  if (!startWildBattle(d, pet.wildLevel(), (uint8_t)expPlace)) expSay("%s", T(S_EXP_NOTEAM));
}

void exploreTap(int16_t x, int16_t y) {
  if (expKeepAsk) {               // modal: nothing behind it answers
    int c1t, c1b, c2t, c2b;
    uiConfirmRects(&c1t, &c1b, &c2t, &c2b);
    if (x < CONFIRM_BTN_X || x > CONFIRM_BTN_X + CONFIRM_BTN_W) return;
    if (y >= c2t && y <= c2b) {   // the NEW one replaces the old
      dexBoxPut(expCatch);
      expKeepAsk = false;
      sfxPlay(SFX_TAP);
      expSay(T(S_STORED_FMT), dexName(expCatch.dex));
    } else if (y >= c1t && y <= c1b) {   // keep the old; the new one goes free
      expKeepAsk = false;
      sfxPlay(SFX_TAP);
    }
    return;
  }
  if (expPlace < 0) {             // the map
    int ex = x - EXP_EXIT_X, ey = y - EXP_EXIT_Y, er = EXP_EXIT_R + 12;   // a finger, not a pixel
    if ((y < EXP_TOP_H && x > CX - 100 && x < CX + 100) || ex * ex + ey * ey <= er * er) {
      closeExplore(); sfxPlay(SFX_TAP); return;
    }
    int best = -1, bd = EXP_PIN_R * EXP_PIN_R + 1;
    for (uint8_t p = 0; p < PLACE_COUNT; p++) {
      int dx = x - MAP_PINS[p].x * 2, dy = y - (MAP_PINS[p].y * 2 + 8);   // pin + its label
      int d = dx * dx + dy * dy;
      if (d < bd) { bd = d; best = p; }
    }
    if (best < 0) return;
    if (!expHasTeam()) { sfxPlay(SFX_DENY); return; }
    if (!placeCount((uint8_t)best)) { sfxPlay(SFX_DENY); expSay("%s", T(S_EXP_NOBODY)); return; }
    sfxPlay(SFX_TAP);
    expPlace = (int8_t)best;
    expActive = 0xFF;
    expNextAt = millis() + 600;
    expMsgUntil = 0;
    return;
  }
  if (y >= EXP_BACK_Y - 4 && y <= EXP_BACK_Y + EXP_BACK_H + 4 && x >= CX - 94 && x <= CX + 94) {
    expPlace = -1;
    sfxPlay(SFX_TAP);
    return;
  }
  uint32_t now = millis();
  for (uint8_t i = 0; i < EXP_SPOTS; i++) {
    int dx = x - EXP_SPOT_X[i], dy = y - (EXP_SPOT_Y[i] - 12);
    if (dx * dx + dy * dy > EXP_SPOT_R * EXP_SPOT_R) continue;
    if (i == expActive && now < expActiveUntil + EXP_GRACE_MS) exploreEncounter();
    else sfxPlay(SFX_TAP);         // a still spot: nothing there this time
    return;
  }
}

// The test's view of the scene, so it never restates the geometry.
void exploreSpotAt(uint8_t i, int *x, int *y) {
  if (i >= EXP_SPOTS) return;
  if (x) *x = EXP_SPOT_X[i];
  if (y) *y = EXP_SPOT_Y[i] - 12;
}

// ---------- gym list ----------
void renderGyms() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  // The ladder's own region in the title, since a vertical swipe moves between
  // three of them and "GYMS" alone would not say which you are looking at.
  char title[28];
  snprintf(title, sizeof(title), "%s %s", TRAINER_SETS[gymRegion % GYM_REGIONS].region,
           T(S_GYMS));
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(title) * 6, 42);
  gfx->print(title);
  // The badge count used to sit here, directly behind the difficulty pill. The
  // region chooser already shows it per region, which is where you are choosing
  // from, so it was both redundant and in the way.
  // difficulty pill: hard caps YOUR team to the leader's size and level, so it
  // is a different ladder with its own badges rather than a damage multiplier
  const char *dif = T(gymHard ? S_HARD : S_EASY);
  int dw = (int)strlen(dif) * 12 + 48;      // wider as well as taller
  if (dw < 120) dw = 120;
  gfx->fillRoundRect(CX - dw / 2, GYMDIF_Y, dw, GYMDIF_H, 12,
                     gymHard ? UI_BAR_BAD : UI_TRACK);
  gfx->setTextColor(gymHard ? UI_BG_DAY : UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(dif) * 6, GYMDIF_Y + 14);
  gfx->print(dif);

  for (int i = 0; i < GYM_ROWS; i++) {
    uint8_t idx = gymPage * GYM_ROWS + i;
    if (idx >= TRAINER_COUNT) break;
    const Trainer &t = TRAINERS[idx];
    int y = GYM_ROW_Y(i);
    bool done = pet.hasBadge(gymRegion, idx, gymHard);
    bool open_ = gymUnlocked(idx, gymHard);
    gfx->fillRoundRect(70, y, 326, 44, 10, done ? UI_TRACK : UI_BG_DAY);
    gfx->drawRoundRect(70, y, 326, 44, 10, open_ ? UI_INK : UI_TRACK);
    gfx->setTextColor(open_ ? UI_INK : UI_TRACK);
    gfx->setTextSize(2);
    gfx->setCursor(84, y + 8);
    gfx->print(t.name);
    gfx->setTextSize(1);
    gfx->setTextColor(UI_TRACK);
    gfx->setCursor(84, y + 28);
    gfx->print(open_ ? t.place : T(S_LOCKED));
    // the level of the strongest creature: the honest measure of the wall
    uint8_t top = 0;
    for (int k = 0; k < t.count; k++)
      if (t.team[k].level > top) top = t.team[k].level;
    char lv[16];
    snprintf(lv, sizeof(lv), "Lv.%u x%u", top, t.count);
    gfx->setTextColor(done ? UI_BAR_OK : (open_ ? UI_INK : UI_TRACK));
    gfx->setCursor(384 - (int)strlen(lv) * 6, y + 28);
    gfx->print(lv);
    if (done) {
      gfx->setTextColor(UI_BAR_OK);
      gfx->setCursor(370, y + 8);
      gfx->print("*");
    }
  }
  uint8_t pages = (TRAINER_COUNT + GYM_ROWS - 1) / GYM_ROWS;
  for (uint8_t i = 0; i < pages; i++) {
    int dx = CX - (pages - 1) * 13 + i * 26;
    if (i == gymPage) gfx->fillCircle(dx, 366, 5, UI_INK);
    else gfx->drawCircle(dx, 366, 4, UI_INK);
  }
  // the other kind of battle lives here too
  gfx->fillRoundRect(148, 380, 170, 32, 9, UI_BG_DAY);
  gfx->drawRoundRect(148, 380, 170, 32, 9, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_LAN)) * 6, 388);
  gfx->print(T(S_LAN));
  gfx->flush();
}

// The region pill under a waiting egg. Tapping it cycles; Pet::setRegion swaps
// the egg to that region's creature, keeping the rarity it was granted and
// remembering each region's answer so flipping back and forth is not a re-roll.
#define EGGREG_X 133
#define EGGREG_Y 374
#define EGGREG_W 200
#define EGGREG_H 34
// The hit area is BIGGER than the pill, like the BOX button and the battle
// grid, and for the same reason: a 34 px target is under UI_TAP_MIN and a
// finger is not a stylus.
//
// The guard band matters more than the padding. Missing this pill fell through
// to pet.eggTap(), and THREE taps hatch the egg -- so fumbling at the region
// selector hatched the very egg you were trying to re-aim. A near miss now
// does nothing at all, which is the correct answer for a control whose
// neighbour is irreversible.
#define EGGREG_PAD 16
#define EGGREG_GUARD 14

static void drawEggRegion() {
  char l[24];
  snprintf(l, sizeof(l), "%s >", pet.regionName());
  gfx->fillRoundRect(EGGREG_X, EGGREG_Y, EGGREG_W, EGGREG_H, 10, UI_WHITE);
  gfx->drawRoundRect(EGGREG_X, EGGREG_Y, EGGREG_W, EGGREG_H, 10, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(EGGREG_X + (EGGREG_W - (int)strlen(l) * 12) / 2, EGGREG_Y + 9);
  gfx->print(l);
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(1);
  gfx->setCursor(CX - (int)strlen(T(S_EGG_REGION)) * 3, EGGREG_Y + EGGREG_H + 6);
  gfx->print(T(S_EGG_REGION));
}

// True if the tap was on the region pill, so the egg does not also get cracked.
// The egg's region pill: its graphic, and the area that actually accepts a tap.
// Exposed so a test can prove the second is bigger than the first and that a
// near miss does not reach pet.eggTap().
void uiEggPillRect(int *x, int *y, int *w, int *h, bool hitArea) {
  int pad = hitArea ? EGGREG_PAD : 0;
  if (x) *x = EGGREG_X - pad;
  if (y) *y = EGGREG_Y - pad;
  if (w) *w = EGGREG_W + 2 * pad;
  if (h) *h = EGGREG_H + 2 * pad;
}

// 1 = cycled the region, -1 = a near miss that must NOT reach the egg, 0 = not
// ours at all.
static int eggRegionTap(int16_t x, int16_t y) {
  if (!pet.isEgg()) return 0;
  int inset = EGGREG_PAD, guard = EGGREG_PAD + EGGREG_GUARD;
  bool hit = x >= EGGREG_X - inset && x <= EGGREG_X + EGGREG_W + inset &&
             y >= EGGREG_Y - inset && y <= EGGREG_Y + EGGREG_H + inset;
  if (hit) {
    pet.setRegion(nextAvailableRegion(pet.region));
    sfxPlay(SFX_TAP);
    return 1;
  }
  bool near = x >= EGGREG_X - guard && x <= EGGREG_X + EGGREG_W + guard &&
              y >= EGGREG_Y - guard && y <= EGGREG_Y + EGGREG_H + guard;
  if (near) { sfxPlay(SFX_DENY); return -1; }
  return 0;
}

// The region chooser used by the Pokedex and the gym ladder. Each row carries
// its own progress, so the screen answers "where am I up to" as well as "where
// do I want to go".
#define RPICK_X 74
#define RPICK_W 318
#define RPICK_H 62
#define RPICK_Y(i) (108 + (i) * 72)
// The page dots sit BETWEEN the last row and the LAN button, not over it.
// They used to be at y=366, which is inside the LAN button (336..380) and on
// top of its label -- invisible until a region chooser had more than one page,
// so it appeared the moment Sinnoh made GYM_REGIONS 4 and went unnoticed
// because swipe_test drives the paging and never looks at the pixels.
#define RPICK_DOTS_Y 325
#define RPICK_DOT_R 5
// Guards, not decoration: both of these are the collision that shipped, and a
// static_assert is the only check that cannot be forgotten when somebody moves
// a button. The dots must clear the LAN button below and the last row above.
static_assert(RPICK_DOTS_Y + RPICK_DOT_R < LANBTN_Y,
              "the region-chooser page dots overlap the LAN button");
static_assert(RPICK_DOTS_Y - RPICK_DOT_R > RPICK_Y(RPICK_PER_PAGE - 1) + RPICK_H,
              "the region-chooser page dots overlap the last region row");

// How many regions this mode lists. Gyms genuinely only exist for three
// (GYM_REGIONS); the Pokedex and the starter screen list every REAL region,
// which is GAL_REGIONS -- REGION_COUNT minus the ALL pseudo-region.
//
// This used to be GYM_REGIONS for ALL THREE MODES, with the names read out of
// TRAINER_SETS -- a trainer table driving the Pokedex chooser. So the moment
// Sinnoh landed it had no row, while the gallery's vertical swipe cycled it
// happily: built, reachable, and looking absent. That is the exact failure the
// chooser was added to prevent.
uint8_t rpickRegions(uint8_t mode) {
  return (mode == RPICK_FOR_GYMS) ? (uint8_t)GYM_REGIONS : (uint8_t)GAL_REGIONS;
}

uint8_t rpickPageCount(uint8_t mode) {
  uint8_t n = rpickRegions(mode);
  uint8_t p = (uint8_t)((n + RPICK_PER_PAGE - 1) / RPICK_PER_PAGE);
  return p ? p : 1;
}

// The chooser WRAPS rather than closing off the end, unlike the other paged
// screens. It is the root of its own screen, and at first boot there is
// nowhere to go back to at all -- exiting would strand the player before they
// have chosen anything.
// Which chooser is on the panel, or 0xFF for none. THE single answer: the
// swipe handler and swipe_test both ask this rather than each deciding.
uint8_t rpickModeNow() {
  switch (uiCurrentScreen()) {
    case SCR_REGION:  return RPICK_FOR_START;
    case SCR_DEXPICK: return RPICK_FOR_DEX;
    case SCR_GYMPICK: return RPICK_FOR_GYMS;
    default: return 0xFF;
  }
}

static bool rpickSwipe(int dir) {
  uint8_t mode = rpickModeNow();
  if (mode == 0xFF) return false;
  uint8_t pages = rpickPageCount(mode);
  int p = (int)rpickPage + (dir > 0 ? -1 : 1);
  if (p < 0) p = pages - 1;
  if (p >= pages) p = 0;
  rpickPage = (uint8_t)p;
  sfxPlay(SFX_TAP);
  return true;
}

static void renderRegionPick(uint8_t mode) {
  bool forGyms = (mode == RPICK_FOR_GYMS);
  uint8_t nreg = rpickRegions(mode);
  uint8_t pages = rpickPageCount(mode);
  if (rpickPage >= pages) rpickPage = 0;
  uint8_t first = (uint8_t)(rpickPage * RPICK_PER_PAGE);
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  char ttl[40];
  if (mode == RPICK_FOR_START) snprintf(ttl, sizeof(ttl), "%s", T(S_CHOOSE_REGION));
  else if (forGyms) snprintf(ttl, sizeof(ttl), "%s", T(S_GYMS));
  else snprintf(ttl, sizeof(ttl), T(S_POKEDEX_FMT), pet.registeredCount(), dexObtainable());
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(ttl) * 6, 48);
  gfx->print(ttl);

  for (uint8_t row = 0; row < RPICK_PER_PAGE; row++) {
    uint8_t i = (uint8_t)(first + row);
    if (i >= nreg) break;
    int y = RPICK_Y(row);
    // The sprite pack is the gate. A region without it is drawn GREYED with a
    // reason rather than dropped from the list: hiding it would say "this
    // region does not exist" when what we mean is "download its pack".
    bool open = forGyms || regionAvailable(i);
    gfx->fillRoundRect(RPICK_X, y, RPICK_W, RPICK_H, 12, open ? UI_WHITE : UI_BG_DAY);
    gfx->drawRoundRect(RPICK_X, y, RPICK_W, RPICK_H, 12, open ? UI_INK : UI_TRACK);
    const char *nm = forGyms ? TRAINER_SETS[i].region : REGIONS[i].name;
    gfx->setTextColor(open ? UI_INK : UI_TRACK);
    gfx->setTextSize(3);
    gfx->setCursor(RPICK_X + 18, y + 12);
    gfx->print(nm);
    // At first boot there is no subtitle: naming the starter here would give
    // away the next screen, and the counts the other two modes show would all
    // read zero on a new save anyway.
    char sub[28];
    sub[0] = 0;
    if (!open)
      snprintf(sub, sizeof(sub), "%s", T(S_NEED_PACK));
    else if (mode == RPICK_FOR_GYMS)
      snprintf(sub, sizeof(sub), T(S_BADGES_FMT), pet.badgeCountIn(i, gymHard));
    else if (mode == RPICK_FOR_DEX)
      snprintf(sub, sizeof(sub), "%u/%u",
               pet.registeredCountIn(REGIONS[i].lo, REGIONS[i].hi),
               (unsigned)dexObtainableIn(REGIONS[i].lo, REGIONS[i].hi));
    if (sub[0]) {
      gfx->setTextColor(UI_TRACK);
      gfx->setTextSize(2);
      gfx->setCursor(RPICK_X + RPICK_W - 18 - (int)strlen(sub) * 12, y + 22);
      gfx->print(sub);
    }
  }
  if (forGyms) {
    gfx->fillRoundRect(LANBTN_X, LANBTN_Y, LANBTN_W, LANBTN_H, 11, UI_BG_DAY);
    gfx->drawRoundRect(LANBTN_X, LANBTN_Y, LANBTN_W, LANBTN_H, 11, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - strlen(T(S_LAN)) * 6, LANBTN_Y + 14);
    gfx->print(T(S_LAN));
  } else if (mode == RPICK_FOR_DEX) {
    // the same spot on the Pokedex chooser is the quiz, with today's count
    uint8_t left = pet.quizLeft();
    char q[20];
    snprintf(q, sizeof(q), "%s %u/%u", T(S_QUIZ), left, QUIZ_PER_DAY);
    gfx->fillRoundRect(LANBTN_X, LANBTN_Y, LANBTN_W, LANBTN_H, 11, left ? UI_BG_DAY : UI_TRACK);
    gfx->drawRoundRect(LANBTN_X, LANBTN_Y, LANBTN_W, LANBTN_H, 11, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (int)strlen(q) * 6, LANBTN_Y + 14);
    gfx->print(q);
  }
  if (pages > 1) {                        // dots: which page of regions this is
    int total = pages * 16 - 8;
    for (uint8_t d = 0; d < pages; d++) {
      int cx = CX - total / 2 + d * 16;
      if (d == rpickPage) gfx->fillCircle(cx, RPICK_DOTS_Y, RPICK_DOT_R, UI_INK);
      else gfx->drawCircle(cx, RPICK_DOTS_Y, RPICK_DOT_R, UI_TRACK);
    }
  }
  if (mode != RPICK_FOR_START) {          // first boot has nowhere to go back to
    gfx->setTextColor(UI_TRACK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - strlen(T(S_BACK)) * 6, 392);
    gfx->print(T(S_BACK));
  }
  gfx->flush();
}

// Returns the region tapped, or -1. Takes the mode because the row on screen is
// an offset into the current page, not the region index -- and because a region
// whose sprite pack is missing must not be selectable, which is the whole point
// of the gate. The chooser still SHOWS it, greyed, saying why.
static int regionPickTap(int16_t x, int16_t y, uint8_t mode) {
  if (x < RPICK_X || x > RPICK_X + RPICK_W) return -1;
  uint8_t nreg = rpickRegions(mode);
  for (uint8_t row = 0; row < RPICK_PER_PAGE; row++) {
    uint8_t i = (uint8_t)(rpickPage * RPICK_PER_PAGE + row);
    if (i >= nreg) break;
    if (y >= RPICK_Y(row) && y <= RPICK_Y(row) + RPICK_H) {
      if (mode != RPICK_FOR_GYMS && !regionAvailable(i)) {
        sfxPlay(SFX_DENY);      // locked: say no out loud rather than do nothing
        return -1;
      }
      return i;
    }
  }
  return -1;
}

// ---------- level-up learn prompt ----------
void renderLearn() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  uint8_t mv = pet.learnOffer();
  char head[40];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  snprintf(head, sizeof(head), T(S_LEARN_Q), nm);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(1);
  gfx->setCursor(CX - (int)strlen(head) * 3, 48);
  gfx->print(head);
  gfx->setTextColor(DEX_TBL[pet.speciesId].accent);
  gfx->setTextSize(3);
  gfx->setCursor(CX - (int)strlen(moveName(mv)) * 9, 66);
  gfx->print(moveName(mv));

  for (int i = 0; i < MOVE_SLOTS; i++) drawMoveRow(LEARN_ROW_Y(i), pet.moves[i], false, pet.speciesId);

  gfx->fillRoundRect(70, LEARN_SKIP_Y, 326, 44, 12, UI_TRACK);
  gfx->drawRoundRect(70, LEARN_SKIP_Y, 326, 44, 12, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_LEARN_SKIP)) * 6, LEARN_SKIP_Y + 14);
  gfx->print(T(S_LEARN_SKIP));
  gfx->flush();
}

// pagina 2: medallas con etiqueta descriptiva
void renderCardMedals() {
  int got = 0;
  for (int i = 0; i < MED_COUNT; i++)
    if (pet.hasMedal(1 << i)) got++;
  char head[20];
  snprintf(head, sizeof(head), T(S_MEDALS_FMT), got, MED_COUNT);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(head) * 9, 48);
  gfx->print(head);

  for (int i = 0; i < MED_COUNT; i++) {
    int x = 28 + (i % 2) * 206, y = 104 + (i / 2) * 54;
    bool g = pet.hasMedal(1 << i);
    gfx->fillRoundRect(x, y, 196, 44, 10, g ? UI_BAR_OK : UI_TRACK);
    if (g) {  // marca de conseguida
      gfx->fillCircle(x + 22, y + 22, 11, UI_BG_DAY);
      gfx->setTextColor(UI_BAR_OK);
      gfx->setTextSize(2);
      gfx->setCursor(x + 16, y + 13);
      gfx->print("v");
    }
    gfx->setTextColor(g ? UI_BG_DAY : 0x8410);
    gfx->setTextSize(2);
    gfx->setCursor(x + 44, y + 14);
    gfx->print(medalDesc(i));
  }
}

// pagina 3: progreso (nivel, evolucion, descuidos) — saca a la luz mecanicas
// que antes eran invisibles (cuanto falta para subir/evolucionar y por que)
void renderCardProgress() {
  const DexEntry &d = DEX_TBL[pet.speciesId];
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_PROGRESS)) * 9, 44);
  gfx->print(T(S_PROGRESS));

  // nivel grande
  char lv[10];
  snprintf(lv, sizeof(lv), T(S_LVL_FMT), pet.level());
  gfx->setTextSize(5);
  gfx->setCursor(CX - strlen(lv) * 15, 86);
  gfx->print(lv);

  // barra de progreso al siguiente nivel (1 nivel = 60 min de juego)
  uint8_t into = pet.ageMinutes % MINUTES_PER_LEVEL;
  int bx = 93, bw = 280, by = 158, bh = 22;
  gfx->fillRoundRect(bx, by, bw, bh, 6, UI_TRACK);
  int fw = (bw - 4) * into / MINUTES_PER_LEVEL;
  if (fw > 0) gfx->fillRoundRect(bx + 2, by + 2, fw, bh - 4, 5, UI_BAR_OK);
  char nx[26];
  snprintf(nx, sizeof(nx), T(S_NEXT_LVL_FMT), MINUTES_PER_LEVEL - into, pet.level() + 1);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(nx) * 6, by + 32);
  gfx->print(nx);

  // estado de evolucion
  gfx->setTextColor(UI_TRACK);
  gfx->setCursor(CX - strlen(T(S_EVO_LABEL)) * 6, 230);
  gfx->print(T(S_EVO_LABEL));
  char evoBuf[28];
  const char *evo;
  uint16_t evoCol = UI_INK;
  if (d.evolvesTo == 0) {
    evo = T(S_FINAL_FORM);
  } else {
    // The SAME sum canEvolveNow() uses -- including the day owed for retiring
    // the previous creature early. A card that left evoPenalty() out would
    // promise an evolution that then does not happen.
    int needed = d.evolveLevel + pet.careMistakes + pet.evoPenalty();
    if (pet.level() >= needed) {
      if (pet.lowestStat() >= 40) { evo = T(S_EVO_READY); evoCol = UI_BAR_OK; }
      else { evo = T(S_EVO_BLOCKED); evoCol = UI_BAR_BAD; }
    } else {
      snprintf(evoBuf, sizeof(evoBuf), T(S_EVO_IN_FMT), needed - pet.level());
      evo = evoBuf;
    }
  }
  gfx->setTextColor(evoCol);
  gfx->setCursor(CX - strlen(evo) * 6, 256);
  gfx->print(evo);

  // the day inherited from an early retire, said out loud -- otherwise this
  // creature simply evolves late and the player has no way to know why
  if (pet.evoPenalty()) {
    gfx->setTextSize(1);
    gfx->setTextColor(UI_BAR_WARN);
    gfx->setCursor(CX - (int)strlen(T(S_EVO_SLOW)) * 3, 286);
    gfx->print(T(S_EVO_SLOW));
    gfx->setTextSize(2);
  }

  // descuidos (retrasan la evolucion)
  char ms[24];
  snprintf(ms, sizeof(ms), T(S_MISTAKES_FMT), pet.careMistakes);
  gfx->setTextColor(pet.careMistakes > 0 ? UI_BAR_BAD : UI_INK);
  gfx->setCursor(CX - strlen(ms) * 6, 312);
  gfx->print(ms);
}

void renderCard() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  if (cardPage == 0) renderCardProfile();
  else if (cardPage == 1) renderCardStats();
  else if (cardPage == 2) renderCardMoves();
  else renderCardProgress();

  // indicador de paginas + ayuda
  for (int i = 0; i < CARD_PAGES; i++) {
    int dx = CX - (CARD_PAGES - 1) * 13 + i * 26;
    if (i == cardPage) gfx->fillCircle(dx, 374, 5, UI_INK);
    else gfx->drawCircle(dx, 374, 4, UI_INK);
  }
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_BACK)) * 6, 398);
  gfx->print(T(S_BACK));
  gfx->flush();
}

// ---------- menu overlay ----------

// The tiles: what each item is called and what it looks like. The Pokedex tile
// also shows the live count under its name.
static const char *const *menuIcon(uint8_t i) {
  switch (i) {
    case MENU_POKEDEX: return SPR_MENU_DEX;
    case MENU_EXPLORE: return SPR_MENU_EXPLORE;
    case MENU_BATTLE: return SPR_MENU_BATTLE;
    case MENU_TRADE: return SPR_MENU_TRADE;
    case MENU_TEAM: return SPR_MENU_TEAM;
    case MENU_STATS: return SPR_MENU_STATS;
    case MENU_SETTINGS: return SPR_MENU_SETTINGS;
    default: return SPR_MENU_FAREWELL;
  }
}
static const char *menuLabel(uint8_t i) {
  switch (i) {
    case MENU_POKEDEX: return "POKEDEX";      // the same word in all six languages
    case MENU_EXPLORE: return T(S_EXPLORE);
    case MENU_BATTLE: return T(S_BATTLE);
    case MENU_TRADE: return T(S_MENU_TRADE);
    case MENU_TEAM: return T(S_MENU_TEAM);
    case MENU_STATS: return T(S_STATS);
    case MENU_SETTINGS: return T(S_MENU_SETTINGS);
    default: return T(S_MENU_FAREWELL);
  }
}

// ONE answer to "can this tile be used now", asked by the drawing (greyed) and
// by the tap (refused) alike, so a grey tile can never still work.
bool menuItemDisabled(uint8_t i) {
  if (i == MENU_FAREWELL) return !pet.canRetireNow();   // an egg or a companion
  if (i == MENU_TRADE) {                                 // nobody to offer
    int8_t c[PARTY_SLOTS + BOX_SLOTS];
    return tradeCands(c) == 0;
  }
  return false;
}

// Where a tap on the open menu landed. Drawing and tapping share the geometry
// through the MENU_* macros; the tests ask this rather than restating it.
int menuHit(int16_t x, int16_t y) {
  if (x < MENU_X || x > MENU_X + MENU_W || y < MENU_Y || y > MENU_Y + MENU_H)
    return MENU_HIT_OUTSIDE;
  if (y >= MENU_CLOSE_Y && y <= MENU_CLOSE_Y + MENU_CLOSE_H &&
      x >= MENU_CLOSE_X && x <= MENU_CLOSE_X + MENU_CLOSE_W) return MENU_HIT_CLOSE;
  // the strip between the tiles and CLOSE holds the page dots; a tap there
  // picks the page under the finger, for a child who cannot swipe yet
  if (y > MENU_TILE_Y(1) + MENU_TILE_H && y < MENU_CLOSE_Y) {
    int p = (x - MENU_X) * MENU_PAGES / MENU_W;
    if (p < 0) p = 0;
    if (p > MENU_PAGES - 1) p = MENU_PAGES - 1;
    return MENU_HIT_DOT0 - p;
  }
  for (uint8_t k = 0; k < MENU_PER_PAGE; k++) {
    uint8_t i = menuPage * MENU_PER_PAGE + k;
    if (i >= MENU_ITEMS) break;
    int tx = MENU_TILE_X(k % 2), ty = MENU_TILE_Y(k / 2);
    if (x >= tx && x <= tx + MENU_TILE_W && y >= ty && y <= ty + MENU_TILE_H) return i;
  }
  return MENU_HIT_NONE;
}

void menuDo(uint8_t i) {
  if (menuItemDisabled(i)) { sfxPlay(SFX_DENY); return; }   // the menu stays up
  sfxPlay(SFX_TAP);
  menuOpen = false;
  switch (i) {
    case MENU_POKEDEX:
      galleryOpen = true; galleryPick = true; galleryPage = 0; rpickPage = 0;
      galleryDetail = 0; galleryDirty = true;
      break;
    case MENU_EXPLORE: openExplore(); break;
    case MENU_BATTLE:                         // the same place as a swipe left
      gymOpen = true; gymPick = true; gymPage = 0; rpickPage = 0;
      break;
    case MENU_TRADE:                          // pairing, then the trade screen
      lanForTrade = true;
      lan.state = LINK_OFF;
      lanOpen = true;
      break;
    case MENU_TEAM: partyOpen = true; break;  // the same place as a swipe right
    case MENU_STATS: cardOpen = true; cardPage = 1; break;   // straight to the stats page
    case MENU_SETTINGS: openClock(); break;
    case MENU_FAREWELL: choiceKind = 3; choiceUntil = millis() + 12000; break;
  }
}

// Darkens the whole frame behind a modal panel: every pixel at half (a quarter
// at night). It used to be a black line on every other row, and on the board
// the panel blanked for an instant each time a finger touched or left the glass
// while that pattern was up. A real dim reads the same and has no stripes.
void dimFrame() {
  uint16_t *b = gfx->getFramebuffer();
  const uint16_t mask = gNight ? 0x39E7 : 0x7BEF;
  const int sh = gNight ? 2 : 1;
  for (uint32_t i = 0; i < 466UL * 466UL; i++) b[i] = (b[i] >> sh) & mask;
}

void drawMenu() {
  // dim the game behind the panel so the overlay reads as modal, and so it is
  // obvious that tapping the darkened area is a way out
  dimFrame();

  gfx->fillRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 18, UI_WHITE);
  gfx->drawRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 18, UI_INK);

  for (uint8_t k = 0; k < MENU_PER_PAGE; k++) {
    uint8_t i = menuPage * MENU_PER_PAGE + k;
    int tx = MENU_TILE_X(k % 2), ty = MENU_TILE_Y(k / 2);
    if (i >= MENU_ITEMS) continue;              // an empty place stays empty
    bool dead = menuItemDisabled(i);
    gfx->fillRoundRect(tx, ty, MENU_TILE_W, MENU_TILE_H, 14, dead ? UI_TRACK : UI_BG_DAY);
    gfx->drawRoundRect(tx, ty, MENU_TILE_W, MENU_TILE_H, 14, UI_INK);
    drawMap(menuIcon(i), 16, tx + MENU_TILE_W / 2 - 24, ty + 12, 3, dead);
    const char *lbl = menuLabel(i);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(tx + MENU_TILE_W / 2 - (int)strlen(lbl) * 6, ty + 68);
    gfx->print(lbl);
    if (i == MENU_POKEDEX) {
      char cnt[16];
      snprintf(cnt, sizeof(cnt), "%u/%u", (unsigned)pet.registeredCount(), (unsigned)dexObtainable());
      gfx->setTextSize(1);
      gfx->setCursor(tx + MENU_TILE_W / 2 - (int)strlen(cnt) * 3, ty + 90);
      gfx->print(cnt);
    }
  }

  for (uint8_t p = 0; p < MENU_PAGES; p++) {    // which page this is
    int dx = CX - (MENU_PAGES - 1) * 10 + p * 20;
    gfx->fillCircle(dx, MENU_DOTS_Y, 5, p == menuPage ? UI_INK : UI_TRACK);
  }

  gfx->fillRoundRect(MENU_CLOSE_X, MENU_CLOSE_Y, MENU_CLOSE_W, MENU_CLOSE_H, 12, UI_TRACK);
  gfx->drawRoundRect(MENU_CLOSE_X, MENU_CLOSE_Y, MENU_CLOSE_W, MENU_CLOSE_H, 12, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(T(S_CLOSE)) * 6, MENU_CLOSE_Y + MENU_CLOSE_H / 2 - 8);
  gfx->print(T(S_CLOSE));
}

// ---------- training submenu (5th icon) ----------

// Bars here show progress toward the IV-capped ceiling, not a raw stat: 100%
// means this individual cannot train the stat any higher, which is the whole
// point of trMaxFor() gating training by IV.
static uint8_t trainPct(uint8_t cur, uint8_t cap) {
  return cap ? (uint8_t)((uint16_t)cur * 100 / cap) : 0;
}

void renderTrain() {
  // Nothing redraws the scene under this panel, so the frame is darkened once,
  // when the panel opens; darkening it every frame would fade it to black.
  static uint32_t lastFrame = 0;
  if (millis() - lastFrame > 400) dimFrame();
  lastFrame = millis();

  gfx->fillRoundRect(TRAIN_X, TRAIN_Y, TRAIN_W, TRAIN_H, 18, UI_WHITE);
  gfx->drawRoundRect(TRAIN_X, TRAIN_Y, TRAIN_W, TRAIN_H, 18, UI_INK);

  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(T(S_TRAIN)) * 6, TRAIN_Y + 20);
  gfx->print(T(S_TRAIN));

  const char *lbl[3] = { T(S_TR_ATK), T(S_TR_SPE), T(S_TR_DEF) };
  uint8_t cur[3] = { pet.trAtk, pet.trSpe, pet.trDef };
  uint8_t cap[3] = { pet.trMaxAtk(), pet.trMaxSpe(), pet.trMaxDef() };

  for (int i = 0; i < 3; i++) {
    int y = TRAIN_ROW_Y(i);
    bool passive = false;      // every row opens a game now, DEF included
    gfx->fillRoundRect(TRAIN_X + 18, y, TRAIN_W - 36, TRAIN_ROW_H, 12,
                       passive ? UI_TRACK : UI_BG_DAY);
    gfx->drawRoundRect(TRAIN_X + 18, y, TRAIN_W - 36, TRAIN_ROW_H, 12, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(TRAIN_X + 32, y + 10);
    gfx->print(lbl[i]);

    uint8_t pct = trainPct(cur[i], cap[i]);
    int bx = TRAIN_X + 32, bw = TRAIN_W - 64, bh = 12, by = y + 34;
    gfx->fillRoundRect(bx, by, bw, bh, 4, UI_TRACK);
    int fw = (bw - 4) * pct / 100;
    if (fw > 0)
      gfx->fillRoundRect(bx + 2, by + 2, fw, bh - 4, 3, pct >= 100 ? UI_BAR_OK : UI_BAR_WARN);
  }

  gfx->setTextColor(UI_INK);
  gfx->setTextSize(1);
  gfx->setCursor(CX - (int)strlen(T(S_TR_DEF_HINT)) * 3, TRAIN_Y + TRAIN_H - 22);
  gfx->print(T(S_TR_DEF_HINT));
  gfx->flush();   // without this the panel never updates and the screen freezes
}

// ---------- the box ----------
// Storage past the six that fight. A creature is moved by picking a party slot
// and then a box slot, which swaps them -- so one gesture covers deposit,
// withdraw and exchange rather than needing three.
void renderBox() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  char head[32];
  snprintf(head, sizeof(head), T(S_BOX_FMT), party.boxCount(), BOX_SLOTS);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(head) * 6, 40);
  gfx->print(head);
  if (boxSwapFrom) {
    const PartyMon &p = party.slots[boxSwapFrom - 1];
    char sub[40];
    snprintf(sub, sizeof(sub), T(S_BOX_SWAP),
             p.empty() ? "-" : (p.nick[0] ? p.nick : dexName(p.dex)));
    gfx->setTextColor(UI_BAR_WARN);
    gfx->setTextSize(1);
    gfx->setCursor(CX - (int)strlen(sub) * 3, 64);
    gfx->print(sub);
  }
  for (uint8_t i = 0; i < BOX_PER_PAGE; i++) {
    uint8_t idx = boxPage * BOX_PER_PAGE + i;
    if (idx >= BOX_SLOTS) break;
    const PartyMon &m = party.box[idx];
    int x = PARTY_GRID_X + (i % 2) * (PARTY_CELL_W + 10);
    int y = 88 + (i / 2) * (PARTY_CELL_H + 8);
    gfx->fillRoundRect(x, y, PARTY_CELL_W, PARTY_CELL_H, 10,
                       m.empty() ? UI_TRACK : UI_WHITE);
    gfx->drawRoundRect(x, y, PARTY_CELL_W, PARTY_CELL_H, 10, UI_INK);
    if (m.empty()) {
      gfx->setTextColor(0x8410);
      gfx->setTextSize(1);
      gfx->setCursor(x + PARTY_CELL_W / 2 - 18, y + PARTY_CELL_H / 2 - 4);
      gfx->print(T(S_PARTY_EMPTY));
      continue;
    }
    const uint8_t *th = thumbs.get(m.dex);
    if (th) drawThumb(th, x - 14, y - 4, 2, false);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(1);
    gfx->setCursor(x + 52, y + 16);
    gfx->print(m.nick[0] ? m.nick : dexName(m.dex));
    char l[16];
    snprintf(l, sizeof(l), "Lv.%u%s", (unsigned)m.level, m.shiny ? " *" : "");
    gfx->setCursor(x + 52, y + 34);
    gfx->print(l);
  }
  uint8_t pages = BOX_SLOTS / BOX_PER_PAGE;
  for (uint8_t i = 0; i < pages; i++) {
    int dx = CX - (pages - 1) * 13 + i * 26;
    if (i == boxPage) gfx->fillCircle(dx, 366, 5, UI_INK);
    else gfx->drawCircle(dx, 366, 4, UI_INK);
  }
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_BACK)) * 6, 392);
  gfx->print(T(S_BACK));
  gfx->flush();
}

void boxTap(int16_t x, int16_t y) {
  // The sheet is checked first and is modal in the same way the party's is.
  if (boxDetail) {
    if (releaseConfirm) { monSheetConfirmTap(x, y, true); return; }
    if (monSheetBtn(x, y, true)) {          // TO PARTY
      int free = party.firstFree();
      if (free < 0) { sfxPlay(SFX_DENY); return; }
      party.swapPartyBox((uint8_t)free, boxDetail - 1);
      boxDetail = 0;
      boxSel = 0;
      sfxPlay(SFX_MEDAL);
      return;
    }
    if (monSheetBtn(x, y, false)) {         // RELEASE -- ask first, always
      releaseConfirm = true;
      sfxPlay(SFX_TAP);
      return;
    }
    boxDetail = 0;                          // anywhere else backs out
    releaseConfirm = false;
    sfxPlay(SFX_TAP);
    return;
  }
  for (uint8_t i = 0; i < BOX_PER_PAGE; i++) {
    uint8_t idx = boxPage * BOX_PER_PAGE + i;
    if (idx >= BOX_SLOTS) break;
    int cx0 = PARTY_GRID_X + (i % 2) * (PARTY_CELL_W + 10);
    int cy0 = 88 + (i / 2) * (PARTY_CELL_H + 8);
    if (x < cx0 || x > cx0 + PARTY_CELL_W || y < cy0 || y > cy0 + PARTY_CELL_H) continue;
    if (boxSwapFrom) {           // a party slot is waiting: complete the trade
      if (party.slots[boxSwapFrom - 1].empty() && party.box[idx].empty()) {
        sfxPlay(SFX_DENY);
        return;
      }
      party.swapPartyBox(boxSwapFrom - 1, idx);
      boxSwapFrom = 0;
      boxSel = 0;
      sfxPlay(SFX_MEDAL);
      return;
    }
    // Otherwise open its SHEET. It used to go straight to the party, which is a
    // lot to happen from one tap and left nowhere to put RELEASE; the sheet
    // offers TO PARTY explicitly and shows what you are about to move.
    if (party.box[idx].empty()) { sfxPlay(SFX_DENY); return; }
    if (party.firstFree() < 0) {
      boxSel = idx + 1;          // party is full: go choose who steps out
      boxOpen = false;
      sfxPlay(SFX_TAP);
      return;
    }
    boxDetail = idx + 1;
    releaseConfirm = false;
    sfxPlay(SFX_TAP);
    return;
  }
  boxOpen = false;               // anywhere else backs out
  boxSwapFrom = 0;
  boxDetail = 0;
  releaseConfirm = false;
}

// ---------- party ----------

void drawPartySlot(int i, int x, int y) {
  const PartyMon &m = party.slots[i];
  gfx->fillRoundRect(x, y, PARTY_CELL_W, PARTY_CELL_H, 10,
                     m.empty() ? UI_TRACK : UI_WHITE);
  gfx->drawRoundRect(x, y, PARTY_CELL_W, PARTY_CELL_H, 10, UI_INK);
  if (m.empty()) {
    gfx->setTextColor(0x8410);
    gfx->setTextSize(2);
    gfx->setCursor(x + (PARTY_CELL_W - (int)strlen(T(S_PARTY_EMPTY)) * 12) / 2,
                   y + PARTY_CELL_H / 2 - 8);
    gfx->print(T(S_PARTY_EMPTY));
    return;
  }
  const uint8_t *th = thumbs.get(m.dex);
  if (th) drawThumb(th, x - 6, y - 3, 1, false);
  const DexEntry &d = DEX_TBL[m.dex];
  const char *nm = m.nick[0] ? m.nick : dexName(m.dex);
  gfx->setTextColor(d.accent);
  gfx->setTextSize(1);
  gfx->setCursor(x + 62, y + 18);
  gfx->print(nm);
  if (m.shiny) {
    gfx->setTextColor(UI_BAR_WARN);
    gfx->setCursor(x + 62 + (int)strlen(nm) * 6 + 3, y + 18);
    gfx->print("*");
  }
  char lv[12];
  snprintf(lv, sizeof(lv), T(S_LVL_FMT), (unsigned)m.level);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(x + 62, y + 36);
  gfx->print(lv);
}

void renderParty() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);

  char head[24];
  snprintf(head, sizeof(head), T(S_PARTY_FMT), party.count());
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - (int)strlen(head) * 9, 42);
  gfx->print(head);

  // the box lives behind this button; it also shows how full it is, so the
  // player knows there is anything in there without opening it
  if (!partyPick) {
    char bl[24];
    snprintf(bl, sizeof(bl), T(S_BOX_FMT), party.boxCount(), BOX_SLOTS);
    bool armed = boxSwapFrom != 0;
    gfx->fillRoundRect(BOXBTN_X, BOXBTN_Y, BOXBTN_W, BOXBTN_H, 10,
                       armed ? UI_BAR_WARN : UI_BG_DAY);
    gfx->drawRoundRect(BOXBTN_X, BOXBTN_Y, BOXBTN_W, BOXBTN_H, 10, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(233 - (int)strlen(bl) * 6, BOXBTN_Y + 12);
    gfx->print(bl);
  }

  if (boxSel) {
    const PartyMon &b = party.box[boxSel - 1];
    char sw[44];
    snprintf(sw, sizeof(sw), T(S_BOX_SWAP),
             b.nick[0] ? b.nick : dexName(b.dex));
    gfx->setTextColor(UI_BAR_WARN);
    gfx->setTextSize(1);
    gfx->setCursor(CX - (int)strlen(sw) * 3, 72);
    gfx->print(sw);
  }

  // when a newcomer is waiting, say so instead of the usual hint
  if (partyPick) {
    gfx->setTextColor(UI_BAR_BAD);
    gfx->setTextSize(1);
    gfx->setCursor(CX - (int)strlen(T(S_PARTY_FULL)) * 3, 74);
    gfx->print(T(S_PARTY_FULL));
  }

  for (int i = 0; i < PARTY_SLOTS; i++) {
    int x = PARTY_GRID_X + (i % 2) * (PARTY_CELL_W + 10);
    int y = PARTY_GRID_Y + (i / 2) * (PARTY_CELL_H + 8);
    drawPartySlot(i, x, y);
  }

  // exit: an explicit button, always in the same place
  const char *ex = partyPick ? T(S_PARTY_LETGO) : T(S_CLOSE);
  gfx->fillRoundRect(PARTYCLOSE_X, PARTYCLOSE_Y, PARTYCLOSE_W, PARTYCLOSE_H, 12,
                     partyPick ? UI_BAR_BAD : UI_TRACK);
  gfx->drawRoundRect(PARTYCLOSE_X, PARTYCLOSE_Y, PARTYCLOSE_W, PARTYCLOSE_H, 12, UI_INK);
  gfx->setTextColor(partyPick ? UI_WHITE : UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(ex) * 6, PARTYCLOSE_Y + 14);
  gfx->print(ex);
  gfx->flush();
}

// ---------- teclado para renombrar ----------

static const char KB_KEYS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-";  // 28 + DEL + OK = 30
#define KB_COLS 6
#define KB_X 40
#define KB_Y 150
#define KB_W 64
#define KB_H 52

// The keyboard is shared, so it has to be told what it is naming. It used to
// hardcode pet.rename() on commit, which is why a second caller needed this.
void openKeyboardFor(uint8_t target) {
  kbTarget = target;
  kbOpen = true;
  const char *cur = (target == KB_TRAINER) ? pet.trainerName : pet.nick;
  strncpy(nameBuf, cur, sizeof(nameBuf) - 1);
  nameBuf[sizeof(nameBuf) - 1] = 0;
  nameLen = strlen(nameBuf);
}
void openKeyboard() { openKeyboardFor(KB_PET); }

void renderKeyboard() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_NAME)) * 6, 56);
  gfx->print(T(S_NAME));
  // buffer actual
  gfx->fillRoundRect(83, 84, 300, 40, 8, UI_WHITE);
  gfx->drawRoundRect(83, 84, 300, 40, 8, UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(95, 94);
  gfx->print(nameLen ? nameBuf : "_");

  for (int i = 0; i < 30; i++) {
    int x = KB_X + (i % KB_COLS) * KB_W, y = KB_Y + (i / KB_COLS) * KB_H;
    bool special = (i >= 28);
    gfx->fillRoundRect(x, y, KB_W - 6, KB_H - 6, 6, special ? UI_BAR_WARN : UI_WHITE);
    gfx->drawRoundRect(x, y, KB_W - 6, KB_H - 6, 6, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    if (i < 28) {
      gfx->setCursor(x + KB_W / 2 - 9, y + KB_H / 2 - 10);
      gfx->print(KB_KEYS[i]);
    } else {
      const char *lab = (i == 28) ? "<-" : "OK";
      gfx->setCursor(x + KB_W / 2 - 15, y + KB_H / 2 - 10);
      gfx->print(lab);
    }
  }
  gfx->flush();
}

void keyboardTap(int16_t x, int16_t y) {
  int col = (x - KB_X) / KB_W, row = (y - KB_Y) / KB_H;
  if (col < 0 || col >= KB_COLS || row < 0 || row >= 5) return;
  int i = row * KB_COLS + col;
  if (i >= 30) return;
  if (i == 28) {  // borrar
    if (nameLen) nameBuf[--nameLen] = 0;
  } else if (i == 29) {  // OK
    if (kbTarget == KB_TRAINER) pet.renameTrainer(nameBuf);
    else pet.rename(nameBuf);
    kbOpen = false;
  } else if (nameLen < sizeof(nameBuf) - 1) {
    nameBuf[nameLen++] = KB_KEYS[i];
    nameBuf[nameLen] = 0;
  }
}

// Every creature in the Pokedex box was caught, so its species belongs in the
// Pokedex. Until 3.25 a fight closed the save's NVS handle (ownprefs.h) and
// catches after it reached the box -- a file -- but not the Pokedex, so a
// Pokeball sat on species still drawn in black. Run at every boot: it costs a
// pass over a bitmap, and it is the one way those catches come back.
void dexBoxRepair() {
  if (!dexBoxReady()) return;
  uint16_t fixed = 0;
  for (int16_t d = 1; d <= DEX_COUNT; d++) {
    if (!dexBoxHas(d) || pet.isRegistered(d)) continue;
    PartyMon m;
    pet.registerCaught(d, dexBoxGet(d, m) && m.shiny);
    fixed++;
  }
  if (fixed) Serial.printf("dexbox: %u caught species put back in the Pokedex\n", fixed);
}

// ---------- galeria pokedex ----------

// The Pokedex page's one button, which moves a creature between the Pokedex
// box and the team. ONE answer for what it offers (dexPageAction) and one for
// doing it (dexPageDo), asked by the draw, the tap and the tests alike.
#define DEXBTN_Y 344
#define DEXBTN_H 46
#define DEXBTN_W 240
enum : uint8_t { DEXACT_NONE = 0, DEXACT_TO_TEAM, DEXACT_TEAM_FULL, DEXACT_STORE };

// A banked creature of this species, box first so the fighting six are the
// last to be touched: 1..BOX_SLOTS for the box, -1..-PARTY_SLOTS for the party.
static int teamMemberOf(int16_t dex) {
  for (int i = 0; i < BOX_SLOTS; i++) if (party.box[i].dex == dex) return i + 1;
  for (int i = 0; i < PARTY_SLOTS; i++) if (party.slots[i].dex == dex) return -(i + 1);
  return 0;
}

uint8_t dexPageAction(int16_t dex) {
  if (!pet.isRegistered(dex) || !dexBoxReady()) return DEXACT_NONE;
  if (dexBoxHas(dex))
    return (party.firstFree() >= 0 || party.boxFirstFree() >= 0) ? DEXACT_TO_TEAM : DEXACT_TEAM_FULL;
  return teamMemberOf(dex) ? DEXACT_STORE : DEXACT_NONE;
}

// While an egg waits, a creature in the Pokedex box can be raised instead of
// it. The page then shows two buttons side by side, the team one on the left.
bool dexPageCanAdopt(int16_t dex) {
  return dexBoxHas(dex) && pet.isEgg() && !pet.awaitingStarter();
}

#define ADOPT_W 150
#define ADOPT_GAP 12
static int dexBtnW(int16_t dex) { return dexPageCanAdopt(dex) ? ADOPT_W : DEXBTN_W; }
static int dexBtnX(int16_t dex) {
  return dexPageCanAdopt(dex) ? CX - ADOPT_W - ADOPT_GAP / 2 : CX - DEXBTN_W / 2;
}

bool dexPageButtonHit(int16_t x, int16_t y) {
  int bx = dexBtnX(galleryDetail);
  return x >= bx && x <= bx + dexBtnW(galleryDetail) &&
         y >= DEXBTN_Y - 4 && y <= DEXBTN_Y + DEXBTN_H + 4;
}

bool dexAdoptHit(int16_t x, int16_t y) {
  if (!dexPageCanAdopt(galleryDetail)) return false;
  int bx = CX + ADOPT_GAP / 2;
  return x >= bx && x <= bx + ADOPT_W && y >= DEXBTN_Y - 4 && y <= DEXBTN_Y + DEXBTN_H + 4;
}

// Giving up the egg cannot be undone, so the first tap only arms the button.
uint32_t adoptArmUntil = 0;

// Raises the Pokedex box's creature of this species in place of the egg.
bool dexAdopt(int16_t dex) {
  PartyMon m;
  if (!dexPageCanAdopt(dex) || !dexBoxGet(dex, m)) return false;
  if (!pet.adoptFrom(m)) return false;
  dexBoxTake(dex);
  Serial.printf("adopt: %s (dex %d) replaces the egg\n", DEX_TBL[dex].name, dex);
  return true;
}

// Moves it and returns true, or returns false having changed nothing.
bool dexPageDo(int16_t dex) {
  uint8_t act = dexPageAction(dex);
  PartyMon m;
  if (act == DEXACT_TO_TEAM) {
    if (!dexBoxGet(dex, m)) return false;
    if (!party.add(m) && !party.boxAdd(m)) return false;
    dexBoxTake(dex);
    return true;
  }
  if (act == DEXACT_STORE) {
    int at = teamMemberOf(dex);
    m = at > 0 ? party.box[at - 1] : party.slots[-at - 1];
    if (!dexBoxPut(m)) return false;       // stored FIRST: never in neither place
    if (at > 0) party.boxReleaseAt(at - 1); else party.releaseAt(-at - 1);
    return true;
  }
  return false;
}

#define GAL_X 73
#define GAL_Y 84

// dibuja una miniatura centrada en su celda; sil=true la pinta en tinta
void drawThumb(const uint8_t *b, int x, int y, int s, bool sil) {
  uint8_t w = b[0], h = b[1], n = b[2];
  const uint8_t *pal = b + 3;
  const uint8_t *d = pal + n * 2;
  int ox = x + (GAL_CELL - w * s) / 2;
  int oy = y + (GAL_CELL - h * s) / 2;
  for (int r = 0; r < h; r++) {
    const uint8_t *row = d + r * w;
    forRuns(w, [&](int c) { return indexedKey(row[c], sil); },
            [&](int c0, int len, int k) {
              uint16_t col = sil ? INK_K : (uint16_t)(pal[k * 2] | (pal[k * 2 + 1] << 8));
              gfx->fillRect(ox + c0 * s, oy + r * s, len * s, s, col);
            });
  }
}

void renderGallery() {
  if (galleryDetail) {  // vista detalle: se redibuja siempre (animada)
    gfx->fillScreen(RGB565_BLACK);
    gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
    const DexEntry &d = DEX_TBL[galleryDetail];
    bool reg = pet.isRegistered(galleryDetail);
    char head[24];
    snprintf(head, sizeof(head), "N.%03d %s%s", galleryDetail,
             pet.isShinyRegistered(galleryDetail) ? "*" : "", reg ? dexName(galleryDetail) : "???");
    gfx->setTextColor(reg ? d.accent : UI_INK);
    int glen = strlen(head);
    int gts = (glen <= 13) ? 3 : 2;  // auto-encoge nombres largos (no caben a t3)
    gfx->setTextSize(gts);
    gfx->setCursor(CX - glen * (gts == 3 ? 9 : 6), gts == 3 ? 56 : 60);
    gfx->print(head);
    if (galleryPmd.loaded) {
      // animado y a color si esta registrado; silueta estatica si no (estilo "?")
      drawPmdActM(galleryPmd, PMD_IDLE, CX, 300, reg ? millis() : 0, true, !reg, 6);
    } else {
      const uint8_t *t = thumbs.get(galleryDetail);
      if (t) drawThumb(t, CX - GAL_CELL, 135, 4, !reg);
    }
    // The Pokedex box: this species' creature, or a place to put one.
    uint8_t act = dexPageAction(galleryDetail);
    if (act != DEXACT_NONE) {
      if (act == DEXACT_TO_TEAM || act == DEXACT_TEAM_FULL) {
        PartyMon m;
        dexBoxGet(galleryDetail, m);
        char l[32];
        snprintf(l, sizeof(l), T(S_DEXBOX_FMT), (unsigned)m.level);
        gfx->setTextColor(UI_INK);
        gfx->setTextSize(1);
        gfx->setCursor(CX - (int)strlen(l) * 3, DEXBTN_Y - 16);
        gfx->print(l);
      }
      bool live = (act != DEXACT_TEAM_FULL);
      const char *lbl = act == DEXACT_STORE ? T(S_TO_DEX)
                      : (live ? T(S_TO_TEAM) : T(S_PARTY_FULL));
      int bx = dexBtnX(galleryDetail), bw = dexBtnW(galleryDetail);
      gfx->fillRoundRect(bx, DEXBTN_Y, bw, DEXBTN_H, 12, live ? UI_BAR_OK : UI_TRACK);
      gfx->drawRoundRect(bx, DEXBTN_Y, bw, DEXBTN_H, 12, UI_INK);
      gfx->setTextColor(live ? UI_WHITE : 0x8410);
      int ts = strlen(lbl) * 12 > bw - 16 ? 1 : 2;
      gfx->setTextSize(ts);
      gfx->setCursor(bx + bw / 2 - (int)strlen(lbl) * 3 * ts, DEXBTN_Y + DEXBTN_H / 2 - 4 * ts);
      gfx->print(lbl);
    }
    if (dexPageCanAdopt(galleryDetail)) {
      bool armed = millis() < adoptArmUntil;
      const char *lbl = armed ? T(S_ADOPT_SURE) : T(S_ADOPT);
      int bx = CX + ADOPT_GAP / 2;
      gfx->fillRoundRect(bx, DEXBTN_Y, ADOPT_W, DEXBTN_H, 12, armed ? UI_BAR_BAD : UI_BAR_WARN);
      gfx->drawRoundRect(bx, DEXBTN_Y, ADOPT_W, DEXBTN_H, 12, UI_INK);
      gfx->setTextColor(UI_WHITE);
      int ts = strlen(lbl) * 12 > ADOPT_W - 16 ? 1 : 2;
      gfx->setTextSize(ts);
      gfx->setCursor(bx + ADOPT_W / 2 - (int)strlen(lbl) * 3 * ts, DEXBTN_Y + DEXBTN_H / 2 - 4 * ts);
      gfx->print(lbl);
    }
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - strlen(T(S_DETAIL_BACK)) * 6, 408);
    gfx->print(T(S_DETAIL_BACK));
    gfx->flush();
    return;
  }

  if (!galleryDirty) return;  // la rejilla es estatica
  galleryDirty = false;

  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  // the region's own name and its own tally: "how much of Johto have I seen"
  // is the question you are actually asking here
  char head[32];
  const RegionInfo &grg = REGIONS[galleryRegion % GAL_REGIONS];
  snprintf(head, sizeof(head), "%s %u/%u", grg.name,
           pet.registeredCountIn(grg.lo, grg.hi), (unsigned)dexObtainableIn(grg.lo, grg.hi));
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(head) * 9, 36);
  gfx->print(head);

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      int16_t dex = GAL_LO + galleryPage * GAL_PER_PAGE + r * 4 + c;
      if (dex > GAL_HI || dex > DEX_COUNT) break;
      int x = GAL_X + c * GAL_CELL, y = GAL_Y + r * GAL_CELL;
      const uint8_t *t = thumbs.get(dex);
      if (t) {
        drawThumb(t, x, y, 2, !pet.isRegistered(dex));
        if (dexBoxHas(dex)) drawBallIcon(x + 10, y + 10, 7);   // one waits in the Pokedex box
        if (pet.isShinyRegistered(dex)) {
          gfx->setTextColor(UI_BAR_WARN);
          gfx->setTextSize(2);
          gfx->setCursor(x + 62, y + 4);
          gfx->print("*");
        }
      } else {
        // No thumbnail: either the pack is not on the card yet, or the species
        // has no art anywhere (noart.h) -- it can then never hatch, be met or be
        // evolved into. A framed number says "there is one here" instead of a
        // gap a child reads as a bug.
        char num[6];
        snprintf(num, sizeof(num), "%d", dex);
        gfx->drawRoundRect(x + 8, y + 8, GAL_CELL - 16, GAL_CELL - 16, 10, 0x8410);
        gfx->setTextColor(0x8410);
        gfx->setTextSize(2);
        gfx->setCursor(x + GAL_CELL / 2 - (int)strlen(num) * 6, y + 32);
        gfx->print(num);
      }
    }
  }
  // A page number, not a row of dots. 25 dots do not fit across the bottom of
  // a round panel -- the chord at that height is only ~228 px -- and counting
  // them to find where you are is worse than reading the number.
  char pg[12];
  snprintf(pg, sizeof(pg), "%d/%d", galleryPage + 1, (int)GAL_PAGES);
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(pg) * 6, 428);
  gfx->print(pg);
  gfx->flush();
}

void galleryTap(int16_t x, int16_t y) {
  if (galleryDetail && dexAdoptHit(x, y)) {
    if (millis() >= adoptArmUntil) { adoptArmUntil = millis() + 3000; sfxPlay(SFX_TAP); return; }
    adoptArmUntil = 0;
    if (!dexAdopt(galleryDetail)) { sfxPlay(SFX_DENY); return; }
    galleryDetail = 0;              // straight back to the new creature
    galleryPmd.unload();
    galleryOpen = false;
    menuOpen = false;
    return;
  }
  if (galleryDetail && dexPageButtonHit(x, y)) {
    if (!dexPageDo(galleryDetail)) sfxPlay(SFX_DENY);
    else sfxPlay(SFX_TAP);
    return;
  }
  if (galleryDetail) {  // volver a la rejilla
    galleryDetail = 0;
    galleryPmd.unload();
    galleryDirty = true;
    return;
  }
  if (y < 72) {  // tocar la cabecera = salir
    galleryOpen = false;
    galleryPmd.unload();
    return;
  }
  int c = (x - GAL_X) / GAL_CELL, r = (y - GAL_Y) / GAL_CELL;
  if (c < 0 || c > 3 || r < 0 || r > 3) return;
  int16_t dex = GAL_LO + galleryPage * GAL_PER_PAGE + r * 4 + c;
  if (dex > GAL_HI || dex > DEX_COUNT) return;
  galleryDetail = dex;
  galleryPmd.load(dex, pet.isShinyRegistered(dex));
  // only one you have met: an unknown one stays a silhouette, silent too, and
  // the quiz keeps the job of putting a cry to a shadow
  if (pet.isRegistered(dex)) cryOr(dex, SFX_TAP);
}

void drawBattery() {
  int pc = batPercent();
  if (pc < 0) return;  // sin bateria conectada
  int x = CX - 14, y = 12, w = 24, h = 11;
  bool charging = batCharging();
  uint16_t col = charging ? UI_BAR_OK
                 : (pc >= 40) ? inkColor()
                 : (pc >= 15) ? UI_BAR_WARN
                              : UI_BAR_BAD;
  gfx->drawRoundRect(x, y, w, h, 2, col);
  gfx->fillRect(x + w, y + 3, 3, 5, col);  // borne
  if (charging) {
    // rayo de carga (zigzag) en vez de la barra de nivel
    uint16_t bolt = C565(0xff, 0xd9, 0x4a);
    int bx = x + w / 2;
    gfx->fillTriangle(bx + 3, y + 1, bx - 4, y + 6, bx + 1, y + 6, bolt);
    gfx->fillTriangle(bx - 1, y + 5, bx + 4, y + 5, bx - 3, y + 10, bolt);
  } else {
    int fw = (w - 4) * pc / 100;
    if (fw > 0) gfx->fillRect(x + 2, y + 2, fw, h - 4, col);
  }
}

// Text over a picture: ringed in whichever of white or ink it stands out
// from, so it reads on a dawn sky as well as a noon one.
void printRinged(int x, int y, const char *t, uint8_t size, uint16_t col) {
  int r = (col >> 11) * 8, g = ((col >> 5) & 63) * 4, b = (col & 31) * 8;
  uint16_t ring = (r * 3 + g * 6 + b) / 10 > 140 ? (uint16_t)0x1083 : UI_WHITE;
  gfx->setTextSize(size);
  gfx->setTextColor(ring);
  for (int dy = -2; dy <= 2; dy += 2)
    for (int dx = -2; dx <= 2; dx += 2) {
      if (!dx && !dy) continue;
      gfx->setCursor(x + dx, y + dy);
      gfx->print(t);
    }
  gfx->setTextColor(col);
  gfx->setCursor(x, y);
  gfx->print(t);
}

void drawHeader(const char *name, uint16_t nameColor, const char *msg) {
  drawBattery();
  printRinged(CX - strlen(name) * 9, 52, name, 3, nameColor);
  printRinged(CX - strlen(msg) * 6, 90, msg, 2, inkColor());
}

// animacion de la ceremonia (10s): despedida = reverencia con corazones y se
// aleja caminando; escapada = se asusta y sale corriendo. Sustituye al idle.
void drawCeremony() {
  if (!pmd.loaded) { drawPet(); return; }  // respaldo si no hay sprite PMD
  uint32_t now = millis();
  float t = pet.ceremonyT();               // 0..1 a lo largo de los 10s
  bool panic = (pet.ceremony == CER_RUNAWAY);
  int x = CX, y = PET_GROUND;
  uint8_t act = PMD_IDLE;

  if (panic) {
    // final triste: penumbra azulada + lluvia
    for (int i = 0; i < 46; i++) {
      int rx = (i * 47 + now / 3) % 466;
      int ry = (i * 91 + now / 2) % 470;
      gfx->drawLine(rx, ry, rx - 3, ry + 12, C565(0x6a, 0x84, 0xb0));
    }
    bool fade = false;
    if (t < 0.30f) {                       // cabizbajo, temblando
      act = pmd.has(PMD_HURT) ? PMD_HURT : PMD_IDLE;
      x = CX + (int)(4 * sinf(now * 0.04f));
    } else {                               // se aleja despacio y se desvanece
      act = pmd.has(PMD_WALKL) ? PMD_WALKL : PMD_IDLE;
      x = CX - (int)(((t - 0.30f) / 0.70f) * (CX + 120));
      fade = (t > 0.6f) && ((now / 160) % 2 == 0);  // parpadea hacia la silueta
    }
    drawPmdAct(act, x, y, now, true, fade, 5);  // fade=silueta: se difumina al irse
    // lagrima cayendo del bicho
    if (t < 0.55f) {
      int ty = y - 150 + (int)((now / 6) % 40);
      gfx->fillRect(x + 6, ty, 3, 6, C565(0x9a, 0xc4, 0xe8));
    }
    return;
  }

  // despedida epica: halo dorado pulsante + chispas y corazones que ascienden
  int gcy = PET_GROUND - 96;
  for (int k = 0; k < 4; k++) {
    int r = 60 + k * 34 + (int)(10 * sinf(now * 0.02f));
    gfx->drawCircle(CX, gcy, r, C565(0xff, 0xdf, 0x8a));
  }
  for (int i = 0; i < 16; i++) {
    int px = (i * 71 + 28) % 466;
    int py = 410 - (int)((now / 8 + i * 70) % 360);   // suben y reaparecen abajo
    if (py < 30) continue;
    if (i % 4 == 0) drawMap(SPR_HEART, 32, px - 8, py - 8, 1, false);  // corazoncito
    else gfx->fillRect(px, py, 4, 4, (i % 2) ? C565(0xff, 0xe7, 0x9f) : C565(0xff, 0x9a, 0xc0));
  }

  if (t < 0.45f) {                         // reverencia / pose de despedida
    act = pmd.has(PMD_POSE) ? PMD_POSE : (pmd.has(PMD_NOD) ? PMD_NOD : PMD_IDLE);
  } else {                                 // se aleja por la derecha
    act = pmd.has(PMD_WALKR) ? PMD_WALKR : PMD_IDLE;
    x = CX + (int)(((t - 0.45f) / 0.55f) * (CX + 140));
  }
  drawPmdAct(act, x, y, now, true, false, 5);
  if (pet.showHeart())                     // corazon grande siguiendo al bicho
    drawMap(SPR_HEART, 32, x + 50, y - 190, 2, false);
}

// dialogo de decision (2 botones apilados): evolucionar/mantener o despedirse/quedaros
// The two confirm buttons' hit areas, so a test can hold them to UI_TAP_MIN and
// prove they do not overlap without restating the numbers.
void uiConfirmRects(int *b1Top, int *b1Bot, int *b2Top, int *b2Bot) {
  *b1Top = CONFIRM_B1_Y;
  *b1Bot = CONFIRM_B1_Y + CONFIRM_BTN_H;
  *b2Top = CONFIRM_B2_Y;
  *b2Bot = CONFIRM_B2_Y + CONFIRM_BTN_H;
}

// Draws the panel and its two buttons. sub1/sub2 are optional lines between the
// question and the buttons -- what the choice COSTS, which for anything
// irreversible has to be on screen before the tap, not after it.
void drawConfirmPanel(const char *q, const char *sub1, const char *sub2,
                      uint16_t subCol, const char *o1, uint16_t c1, uint16_t t1,
                      const char *o2, uint16_t c2, uint16_t t2) {
  gfx->fillRoundRect(CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H, 16, UI_WHITE);
  gfx->drawRoundRect(CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H, 16, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(q) * 6, 176);
  gfx->print(q);
  if (sub1 || sub2) {
    gfx->setTextSize(1);
    gfx->setTextColor(subCol);
    if (sub1) {
      gfx->setCursor(CX - (int)strlen(sub1) * 3, sub2 ? 188 : 194);
      gfx->print(sub1);
    }
    if (sub2) {
      gfx->setCursor(CX - (int)strlen(sub2) * 3, 197);
      gfx->print(sub2);
    }
    gfx->setTextSize(2);
    gfx->setTextColor(UI_INK);
  }
  gfx->fillRoundRect(CONFIRM_BTN_X, CONFIRM_B1_Y, CONFIRM_BTN_W, CONFIRM_BTN_H, 12, c1);
  gfx->setTextColor(t1);
  gfx->setCursor(CX - (int)strlen(o1) * 6, CONFIRM_B1_Y + 18);
  gfx->print(o1);
  gfx->fillRoundRect(CONFIRM_BTN_X, CONFIRM_B2_Y, CONFIRM_BTN_W, CONFIRM_BTN_H, 12, c2);
  gfx->setTextColor(t2);
  gfx->setCursor(CX - (int)strlen(o2) * 6, CONFIRM_B2_Y + 18);
  gfx->print(o2);
}

void drawChoiceDialog() {
  const char *q, *o1, *o2;
  const char *sub1 = nullptr, *sub2 = nullptr;
  uint16_t c1, c2, t1, t2;
  if (choiceKind == 1) {  // evolucion
    q = T(S_EVO_Q); o1 = T(S_EVO_TAP); o2 = T(S_EVO_KEEP);
    c1 = UI_BAR_BAD; t1 = UI_WHITE; c2 = UI_TRACK; t2 = UI_INK;
  } else if (choiceKind == 3) {   // retirada a peticion
    q = T(S_RETIRE_Q); o1 = T(S_FAR_GO); o2 = T(S_FAR_STAY);
    c1 = UI_BAR_WARN; t1 = UI_INK; c2 = UI_BAR_OK; t2 = UI_WHITE;
    // The price, spelled out, and only when there is one: retiring a creature
    // that has already earned its farewell costs nothing and must not claim to.
    // An EARLY one now costs TWO things and says both -- the creature is not
    // banked at all, which is the half a player would not otherwise discover
    // until the party screen came up empty.
    if (!pet.retireIsFree()) { sub1 = T(S_RETIRE_COST); sub2 = T(S_RETIRE_GONE); }
  } else {                // despedida
    q = T(S_FAR_Q); o1 = T(S_FAR_GO); o2 = T(S_FAR_STAY);
    c1 = UI_BAR_WARN; t1 = UI_INK; c2 = UI_BAR_OK; t2 = UI_WHITE;
  }
  drawConfirmPanel(q, sub1, sub2, UI_BAR_BAD, o1, c1, t1, o2, c2, t2);
}

// boton-CTA rojo y grande para evolucionar (pulsa para llamar la atencion)
void drawEvolveButton() {
  uint32_t now = millis();
  int p = (int)(5 * sinf(now * 0.006f));  // late: -5..5
  int x = EVO_BTN_X - p, y = EVO_BTN_Y - p, w = EVO_BTN_W + 2 * p, h = EVO_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 18, UI_BAR_BAD);
  gfx->drawRoundRect(x, y, w, h, 18, UI_WHITE);
  gfx->drawRoundRect(x + 2, y + 2, w - 4, h - 4, 16, UI_WHITE);
  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(3);
  const char *t = T(S_EVO_TAP);
  gfx->setCursor(CX - (int)strlen(t) * 9, y + h / 2 - 11);
  gfx->print(t);
}

// boton-CTA dorado de despedida: "<nombre> quiere decirte algo..."
void drawFarewellButton() {
  uint32_t now = millis();
  int p = (int)(4 * sinf(now * 0.005f));
  int x = FAR_BTN_X - p, y = FAR_BTN_Y - p, w = FAR_BTN_W + 2 * p, h = FAR_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 16, UI_BAR_WARN);
  gfx->drawRoundRect(x, y, w, h, 16, UI_INK);
  char buf[52];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  snprintf(buf, sizeof(buf), T(S_FAREWELL_BTN), nm);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(buf) * 6, y + h / 2 - 8);
  gfx->print(buf);
}

// boton-CTA sombrio de escapada por abandono: "<nombre> se siente abandonado..."
// (final triste: azul-gris oscuro, latido lento y apagado)
void drawRunawayButton() {
  uint32_t now = millis();
  int p = (int)(3 * sinf(now * 0.003f));
  int x = FAR_BTN_X - p, y = FAR_BTN_Y - p, w = FAR_BTN_W + 2 * p, h = FAR_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 16, C565(0x3a, 0x44, 0x5a));
  gfx->drawRoundRect(x, y, w, h, 16, C565(0x70, 0x80, 0x98));
  char buf[52];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  snprintf(buf, sizeof(buf), T(S_RUNAWAY_BTN), nm);
  gfx->setTextColor(C565(0xc8, 0xd2, 0xe0));
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(buf) * 6, y + h / 2 - 8);
  gfx->print(buf);
}

// animacion epica de evolucion: halo radial + rayos giratorios + parpadeo del
// sprite acelerando + chispas que salen disparadas + fogonazo final
void drawEvolveFX(uint32_t now) {
  float t = pet.evolveT();          // 0..1
  int cx = CX, cy = PET_GROUND - 96;

  // halo radial que crece y pulsa
  int halo = 36 + (int)(t * 150) + (int)(8 * sinf(now * 0.02f));
  for (int k = 0; k < 4; k++) {
    int r = halo - k * 7;
    if (r > 0) gfx->drawCircle(cx, cy, r, UI_WHITE);
  }
  // rayos giratorios desde el centro del bicho
  float base = now * 0.004f;
  for (int i = 0; i < 12; i++) {
    float a = base + i * (float)(PI / 6);
    int len = 90 + (int)(70 * (0.5f + 0.5f * sinf(now * 0.012f + i)));
    gfx->drawLine(cx, cy, cx + (int)(cosf(a) * len), cy + (int)(sinf(a) * len), UI_WHITE);
  }
  // parpadeo entre la forma ANTERIOR y la NUEVA (siluetas), acelerando; al
  // final (t>0.9) se queda fija en la nueva para el fogonazo de revelado
  int period = 60 + (int)(220 * (1.0f - t));
  bool showOld = t < 0.9f && evoPmd.loaded && ((now / period) % 2) == 0;
  if (showOld) drawPmdActM(evoPmd, PMD_IDLE, cx, PET_GROUND, 0, true, true, 5);
  else drawPmdAct(PMD_IDLE, cx, PET_GROUND, 0, true, true, 5);
  // chispas que salen disparadas
  for (int i = 0; i < 10; i++) {
    float a = i * (float)(PI / 5) + t * 4.0f;
    int d = (int)((now / 14 + i * 33) % 200);
    int sx = cx + (int)(cosf(a) * d), sy = cy + (int)(sinf(a) * d);
    gfx->fillRect(sx - 2, sy - 2, 5, 5, (i & 1) ? C565(0xff, 0xe0, 0x70) : UI_WHITE);
  }
  // fogonazo final antes de revelar la forma nueva
  if (t > 0.9f) gfx->fillCircle(cx, cy, (int)(300 * (t - 0.9f) / 0.1f), UI_WHITE);
}

void drawPet() {
  if (pmd.loaded) {
    drawPetPMD();
    return;
  }
  if (mon.loaded) {
    drawPetSD();
    return;
  }
  int fi = flashIdxForDex(pet.speciesId);
  if (fi < 0) {
    // sin SD y sin sprite de flash: aviso claro de que faltan sprites
    gfx->setTextColor(inkColor());
    gfx->setTextSize(6);
    gfx->setCursor(CX - 18, PET_CY - 80);
    gfx->print("?");
    gfx->setTextSize(2);
    const char *l1 = T(S_NO_SPRITES);
    gfx->setCursor(CX - (int)strlen(l1) * 6, PET_CY - 4);
    gfx->print(l1);
    const char *l2 = T(S_LOAD_SPRITES);
    gfx->setCursor(CX - (int)strlen(l2) * 6, PET_CY + 20);
    gfx->print(l2);
    return;
  }
  const Species &sp = SPECIES[fi];
  int s = sp.scale;
  int x = CX - 16 * s;
  int y = PET_CY - 16 * s;

  // animacion de evolucion: alterna la silueta de la forma anterior y la nueva
  if (pet.evolving()) {
    bool flash = (millis() / 300) % 2;
    int16_t showDex = (flash && pet.prevSpeciesId >= 0) ? pet.prevSpeciesId : pet.speciesId;
    int sfi = flashIdxForDex(showDex);
    if (sfi >= 0) {
      const Species &show = SPECIES[sfi];
      drawMap(show.sprite, SPRITE_H, CX - 16 * show.scale, PET_CY - 16 * show.scale, show.scale, flash);
    }
    return;
  }

  PetMood m = pet.mood();
  if (m == MOOD_HAPPY && (millis() / 500) % 2) y -= 6;  // saltito

  drawMap(sp.sprite, SPRITE_H, x, y, s, false);

  // expresiones superpuestas usando las anclas de la especie
  bool blink = (millis() % 3500 < 300);
  if (m == MOOD_SLEEPING || blink) {
    overlayEye(sp, x, y, s, sp.eyeColL);
    overlayEye(sp, x, y, s, sp.eyeColR);
  }
  if (m == MOOD_EATING) overlayMouth(sp, x, y, s, true);
  else if (m == MOOD_SAD) overlayMouth(sp, x, y, s, false);

  if (pet.showHeart()) drawMap(SPR_HEART, 32, x + 20 * s, y - 2 * s, 2, false);
}

// ---------- escena de bano ----------

void startBath() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony || bathUntil) return;
  bathUntil = millis() + 3000;
  bathPending = true;
  int cx = (int)beh.x;
  for (auto &b : bubbles) {
    b.x = cx - 70 + random(140);
    b.y = PET_GROUND - random(150);
    b.r = 8 + random(16);
    b.ph = random(64);
  }
}

void drawBath() {
  uint32_t now = millis();
  if (now > bathUntil) {
    bathUntil = 0;
    if (bathPending) {
      bathPending = false;
      pet.clean();
      // pose de alegria al quedar limpio
      if (pmd.has(PMD_POSE)) {
        beh.mode = 2;
        beh.act = PMD_POSE;
        beh.t0 = now;
        beh.until = now + pmdActTotalMs(pmd.acts[PMD_POSE]) * 2;
      }
    }
    return;
  }
  uint32_t left = bathUntil - now;
  if (left > 800) {
    // espuma: pompas meciendose y subiendo poco a poco
    float t = now / 220.0f;
    for (auto &b : bubbles) {
      int bx = b.x + (int)(sinf(t + b.ph) * 6);
      int by = b.y - (int)((3000 - left) / 90);
      gfx->fillCircle(bx, by, b.r, UI_WHITE);
      gfx->drawCircle(bx, by, b.r, 0x7E3D);
      gfx->fillCircle(bx - b.r / 3, by - b.r / 3, b.r / 4, UI_BG_DAY);
    }
  } else {
    // las pompas revientan: destellos
    for (int i = 0; i < 8; i++) {
      auto &b = bubbles[i];
      int sx = b.x + (i % 3) * 6 - 6, sy = b.y - 18;
      uint16_t col = (i % 2) ? UI_BAR_WARN : UI_WHITE;
      gfx->fillRect(sx - 6, sy - 1, 13, 3, col);
      gfx->fillRect(sx - 1, sy - 6, 3, 13, col);
    }
  }
}

// ---------- mascota PMD: comportamiento ----------

uint32_t pmdActTotalMs(const PmdAct &a) {
  uint32_t t = 0;
  for (uint8_t i = 0; i < a.frames; i++) t += a.ms[i];
  return t ? t : 100;
}

uint8_t pmdFrameAt(const PmdAct &a, uint32_t t, bool loop) {
  uint32_t total = pmdActTotalMs(a);
  if (!loop && t >= total) return a.frames - 1;
  t %= total;
  uint8_t i = 0;
  while (t >= a.ms[i]) {
    t -= a.ms[i];
    i = (i + 1) % a.frames;
  }
  return i;
}

// dibuja una accion anclada por la base (centro-x, suelo) y devuelve su escala
// dibuja una accion de un PmdMon concreto (m); drawPmdAct usa el global pmd
void drawPmdActM(PmdMon &m, uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS) {
  const PmdAct &a = m.acts[actId];
  if (!a.frames) return;
  uint8_t sBase = m.acts[PMD_IDLE].h ? 170 / m.acts[PMD_IDLE].h : 5;
  if (sBase < 2) sBase = 2;
  if (sBase > maxS) sBase = maxS;
  uint8_t s = sBase;
  while (s > 2 && a.h * s > 250) s--;  // acciones con frame grande (ataque)
  uint8_t fi = pmdFrameAt(a, t, loop);
  const uint8_t *fr = a.data + (uint32_t)fi * a.w * a.h;
  // anclar por los pies (a.base), no por el alto del lienzo: asi las acciones
  // con padding distinto (Hurt, Eat...) quedan todas a la misma altura de suelo
  int x0 = cx - a.w * s / 2, y0 = groundY - (a.base ? a.base : a.h) * s;
  for (int r = 0; r < a.h; r++) {
    const uint8_t *row = fr + r * a.w;
    forRuns(a.w, [&](int c) { return indexedKey(row[c], sil); },
            [&](int c0, int len, int k) { gfx->fillRect(x0 + c0 * s, y0 + r * s, len * s, s, sil ? INK_K : m.pal[k]); });
  }
}
void drawPmdAct(uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS) {
  drawPmdActM(pmd, actId, cx, groundY, t, loop, sil, maxS);
}

// elige el siguiente capricho del bicho cuando esta contento
void behNext() {
  uint32_t now = millis();
  beh.t0 = now;
  int r = random(100);
  if (r < 35 && (pmd.has(PMD_WALKL) || pmd.has(PMD_WALKR))) {
    beh.mode = 1;  // paseo
    beh.targetX = 150 + random(176);
    beh.until = now + 15000;
  } else if (r < 60) {
    // gesto aleatorio entre los disponibles
    // (Hop fuera: salta demasiado alto; Sit fuera: mira hacia atras)
    static const uint8_t flair[] = { PMD_POSE, PMD_NOD, PMD_BREATH };
    uint8_t pick[3], n = 0;
    for (uint8_t f : flair)
      if (pmd.has(f)) pick[n++] = f;
    if (n) {
      beh.mode = 2;
      beh.act = pick[random(n)];
      beh.until = now + pmdActTotalMs(pmd.acts[beh.act]);
      return;
    }
    beh.mode = 0;
    beh.until = now + 2000 + random(3000);
  } else {
    beh.mode = 0;  // mirar al frente
    beh.until = now + 2000 + random(3000);
  }
}

void drawPetPMD() {
  uint32_t now = millis();

  if (pet.evolving()) {
    drawEvolveFX(now);
    return;
  }
  if (evoPmd.loaded) evoPmd.unload();  // termino la evolucion: libera la forma anterior

  PetMood m = pet.mood();
  uint8_t act;
  bool loop = true;
  if (m == MOOD_SLEEPING && pmd.has(PMD_SLEEP)) {
    act = PMD_SLEEP;
    beh.mode = 0;
  } else if (m == MOOD_EATING && pmd.has(PMD_EAT)) {
    act = PMD_EAT;
    beh.t0 = 0;
  } else if (m == MOOD_SAD && pmd.has(PMD_HURT)) {
    act = PMD_HURT;
  } else {
    // contento: el planificador decide (idle / paseo / gesto)
    if (now > beh.until) behNext();
    static uint32_t walkLast = 0;          // the last walking step (0 = none yet)
    if (beh.mode != 1) walkLast = 0;       // standing still is not counted as walking
    if (beh.mode == 1) {
      float d = beh.targetX - beh.x;
      if (fabsf(d) < 4) {
        behNext();
        act = PMD_IDLE;
      } else {
        // 30 px/s (3 px a frame at 10 fps) measured in real time, so it walks
        // as fast at the slower idle frame rates; a long gap (another screen
        // in between) is not one long stride
        uint32_t dt = walkLast ? now - walkLast : 100;
        if (dt > 1100) dt = 1100;
        walkLast = now;
        float step = 30.0f * dt / 1000.0f;
        if (step > fabsf(d)) step = fabsf(d);
        beh.x += d > 0 ? step : -step;
        act = (d > 0) ? PMD_WALKR : PMD_WALKL;
      }
    } else {
      act = (beh.mode == 2) ? beh.act : PMD_IDLE;
      loop = false;
    }
    if (!pmd.has(act)) act = PMD_IDLE;
  }

  drawPmdAct(act, (int)beh.x, PET_GROUND, now - beh.t0, loop || act == PMD_IDLE, false, 5);

  if (pet.showHeart()) drawMap(SPR_HEART, 32, (int)beh.x + 50, PET_GROUND - 190, 2, false);
}

// sprite animado desde la SD: zoom entero por pixel, frames a su ritmo
void drawPetSD() {
  int s = mon.scale;
  int w = mon.w * s, h = mon.h * s;
  int x = CX - w / 2;
  int y = PET_CY - h / 2;

  bool sil = false;
  if (pet.evolving()) {
    sil = (millis() / 300) % 2;
  } else if (pet.mood() == MOOD_HAPPY && (millis() / 500) % 2) {
    y -= 6;  // saltito
  }

  uint16_t fm = mon.frameMs ? mon.frameMs : 100;
  uint16_t fi = pet.sleeping ? 0 : (millis() / fm) % mon.frames;
  const uint8_t *fr = mon.data + (uint32_t)fi * mon.w * mon.h;
  for (int r = 0; r < mon.h; r++) {
    const uint8_t *row = fr + r * mon.w;
    forRuns(mon.w, [&](int c) { return indexedKey(row[c], sil); },
            [&](int c0, int len, int k) { gfx->fillRect(x + c0 * s, y + r * s, len * s, s, sil ? INK_K : mon.pal[k]); });
  }

  // emotes en vez de expresiones (los sprites importados no tienen anclas)
  if (pet.showHeart()) drawMap(SPR_HEART, 32, x + w - 30, y - 50, 2, false);
}

// ojo cerrado: borra el ojo 3x4 y dibuja el parpado
void overlayEye(const Species &sp, int x, int y, int s, int col) {
  gfx->fillRect(x + col * s, y + sp.eyeRow * s, 3 * s, 4 * s, sp.bodyColor);
  gfx->fillRect(x + col * s, y + (sp.eyeRow + 2) * s, 3 * s, s, INK_K);
}

// borra la sonrisa base y pinta boca abierta (comer) o ceno (triste)
void overlayMouth(const Species &sp, int x, int y, int s, bool open) {
  int mc = sp.mouthCol, mr = sp.mouthRow;
  gfx->fillRect(x + (mc - 3) * s, y + mr * s, 7 * s, 2 * s, sp.bodyColor);
  if (open) {
    gfx->fillRect(x + (mc - 2) * s, y + mr * s, 5 * s, 2 * s, INK_K);
  } else {
    gfx->fillRect(x + (mc - 2) * s, y + mr * s, 5 * s, s, INK_K);
    gfx->fillRect(x + (mc - 3) * s, y + (mr + 1) * s, s, s, INK_K);
    gfx->fillRect(x + (mc + 3) * s, y + (mr + 1) * s, s, s, INK_K);
  }
}

void drawPoops() {
  for (int i = 0; i < pet.poops; i++) {
    drawMap(SPR_POOP, 32, 36 + i * 46, 244, 2, false);
  }
}

void drawBars() {
  drawBar(78, 318, T(S_BAR_FOOD), pet.fullness);
  drawBar(244, 318, T(S_BAR_JOY), pet.joy);
  drawBar(78, 346, T(S_BAR_ENE), pet.energy);
  drawBar(244, 346, T(S_BAR_HYG), pet.hygiene);
}

void drawBar(int x, int y, const char *label, uint8_t val) {
  gfx->setTextColor(inkColor());
  gfx->setTextSize(2);
  gfx->setCursor(x, y);
  gfx->print(label);
  int bx = x + 48, bw = 100, bh = 15;  // +48: deja sitio a etiquetas de 4 letras (EN)
  uint16_t fill = (val >= 50) ? UI_BAR_OK : (val >= 25) ? UI_BAR_WARN : UI_BAR_BAD;
  gfx->fillRoundRect(bx, y, bw, bh, 4, UI_TRACK);
  int fw = (bw - 4) * val / 100;
  if (fw > 0) gfx->fillRoundRect(bx + 2, y + 2, fw, bh - 4, 3, fill);
}

void drawButtons() {
  for (int i = 0; i < BTN_COUNT; i++) {
    bool off = uiButtonDisabled(i);   // durmiendo solo funciona LUZ
    int bx = buttons[i].cx - BTN_HALF, by = buttons[i].cy - BTN_HALF;
    if (!pet.sleeping) gfx->fillRoundRect(bx, by, 2 * BTN_HALF, 2 * BTN_HALF, 14, UI_WHITE);
    gfx->drawRoundRect(bx, by, 2 * BTN_HALF, 2 * BTN_HALF, 14, inkColor());
    if (!off) drawMap(buttons[i].icon, 16, buttons[i].cx - 16, buttons[i].cy - 16, 2, false);
    // icons have no text to read, so give them a spoken name (tools/gen_voice.py harvests these)
    static const char *const SAY[BTN_COUNT] = { "Eten geven", "Licht", "In bad", "Trainen" };
    speechRegion(bx, by, 2 * BTN_HALF, 2 * BTN_HALF, SAY[i]);
  }
}

const char *eggMsg() {
  switch (pet.eggCracks()) {
    case 0: return T(S_EGG_TOUCH);
    case 1: return T(S_EGG_MOVES);
    default: return T(S_EGG_ALMOST);
  }
}

const char *statusMsg() {
  if (pet.evolving()) return T(S_EVOLVING);
  if (bathUntil) return "Splish splash!";  // onomatopeya universal
  if (pet.sleeping) return "Zzz...";
  if (pet.eating()) return T(S_EATING);
  if (pet.showHeart()) return T(S_LIKES);
  if (pet.fullness < 25) return T(S_HUNGRY);
  if (pet.hygiene < 25) return T(S_NEEDS_BATH);
  if (pet.energy < 25) return T(S_EXHAUSTED);
  if (pet.joy < 25) return T(S_SAD);
  if (pet.weight > 60) return T(S_CHUBBY);
  if (pet.shiny && pet.ageMinutes < 15) return T(S_IS_SHINY);
  if (millis() < giftFullUntil) return T(S_GIFT_FULL);
  uint8_t ev = pet.eventToday();
  if (ev == EV_BIRTHDAY) return pet.giftReady() ? T(S_GIFT_TAP) : T(S_HAPPY_BDAY);
  if (ev != EV_NONE) {           // "HALLOWEEN!" -- the sticker's own name
    static char greet[16];
    snprintf(greet, sizeof(greet), "%s!", stickerName(ev - 1));
    return greet;
  }
  return T(S_HAPPY);
}

// dibuja un mapa de n x n pixeles escalado; silhouette=true lo pinta en tinta
// An 8bpp indexed avatar, same shape as the badge art: 0xFF is transparent.
void drawAvatar(uint8_t which, int x, int y, int s) {
  const AvatarArt &a = AVATARS[which % AVATAR_COUNT];
  for (int r = 0; r < AVATAR_PX; r++)
    for (int c = 0; c < AVATAR_PX; c++) {
      uint8_t v = a.idx[r * AVATAR_PX + c];
      if (v == 0xFF) continue;
      gfx->fillRect(x + c * s, y + r * s, s, s, a.pal[v]);
    }
}

void drawMap(const char *const *map, int n, int x, int y, int s, bool silhouette) {
  for (int r = 0; r < n; r++) {
    const char *row = map[r];
    forRuns(n, [&](int c) { return charKey(row[c], silhouette); },
            [&](int c0, int len, int k) { gfx->fillRect(x + c0 * s, y + r * s, len * s, s, silhouette ? INK_K : spriteColor((char)k)); });
  }
}
