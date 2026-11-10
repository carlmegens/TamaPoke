#pragma once
#include <Arduino.h>

// Idiomas soportados. La fuente del firmware no tiene acentos: ambos textos van
// sin tildes ni enes (igual que ya iba el espanol).
enum Lang : uint8_t { LANG_ES = 0, LANG_EN, LANG_FR, LANG_DE, LANG_IT, LANG_PT, LANG_NL, LANG_COUNT };
#define LANG_DEFAULT LANG_NL  // default language in this fork: Dutch

extern Lang gLang;  // idioma activo (definido en i18n.cpp)

// IDs de cadena. El orden debe coincidir con la tabla STRINGS de i18n.cpp.
enum StrId : uint8_t {
  // estado del bicho (statusMsg)
  S_EVOLVING, S_EATING, S_LIKES, S_HUNGRY, S_NEEDS_BATH,
  S_EXHAUSTED, S_SAD, S_CHUBBY, S_IS_SHINY, S_HAPPY,
  // ceremonias de despedida
  S_FAREWELL, S_RUNAWAY, S_GOODBYE,
  // huevo
  S_EGG_HDR, S_EGG_LEGEND, S_EGG_RARE, S_EGG_TOUCH, S_EGG_MOVES, S_EGG_ALMOST,
  // formatos compartidos
  S_POKEDEX_FMT,   // "POKEDEX %u/151"
  S_NAME_FMT,      // "%s%s Nv.%u"
  // dialogo soltar
  S_RELEASE_FMT, S_YES, S_NO,
  // minijuego y saco
  S_HITS_FMT, S_STR_GAIN_FMT, S_NEW_RECORD, S_RECORD_FMT, S_HIT_FAST,
  S_SCORE_FMT, S_GREAT_JOY, S_PLUS_JOY,
  // reloj / ajustes
  S_SET_TIME, S_HOUR, S_MIN, S_CLOCK_CANCEL, S_LANG_LABEL,
  // celebracion
  S_MEDAL_BANNER, S_GREAT, S_STREAK_DAYS_FMT,
  // ficha: perfil
  S_STREAK_FMT, S_VIN, S_BERRY_UNK, S_BERRY_RED, S_BERRY_BLUE, S_BERRY_GREEN,
  S_INFO_FMT, S_RENAME_HINT,
  // ficha: combate
  S_BATTLE, S_STAT_ATK, S_STAT_DEF, S_STAT_SPE, S_STAT_WGT, S_TRAIN_STR,
  // ficha: medallas
  S_MEDALS_FMT, S_BACK,
  // teclado y galeria
  S_NAME, S_DETAIL_BACK,
  // barras
  S_BAR_FOOD, S_BAR_JOY, S_BAR_ENE, S_BAR_HYG,
  // marcador en vivo del minijuego
  S_REC_FMT,
  // ficha: pagina de progreso
  S_PROGRESS, S_LVL_FMT, S_NEXT_LVL_FMT, S_EVO_LABEL, S_FINAL_FORM,
  S_EVO_READY, S_EVO_BLOCKED, S_EVO_IN_FMT, S_MISTAKES_FMT,
  // interruptor de sonido (ajustes)
  S_SND_ON, S_SND_OFF,
  S_EVO_TAP,        // texto del boton de evolucion
  S_FAREWELL_BTN,   // texto del boton de despedida (lleva el nombre: "%s ...")
  S_RUNAWAY_BTN,    // texto del boton de escapada por abandono (final triste)
  // dialogos de decision (evolucionar/mantener, despedirse/quedaros)
  S_EVO_Q, S_EVO_KEEP, S_FAR_Q, S_FAR_GO, S_FAR_STAY,
  S_CHOOSE_STARTER,  // titulo de la eleccion del inicial (primera vez)
  S_NO_SPRITES, S_LOAD_SPRITES,  // aviso cuando falta el sprite en la SD
  S_STAT_VIT,   // vitalidad (abreviatura de PS/HP en cada idioma)
  S_IV_FMT,     // valor individual junto a cada stat ("IV %u")
  // menu overlay (tap the name on the main screen) and the party
  S_MENU_TITLE, S_SETTINGS, S_CLOSE,
  S_PARTY_FMT,      // "PARTY %u/6"
  S_PARTY_EMPTY,    // empty slot marker
  S_PARTY_JOINED,   // "%s joined the party!"
  S_PARTY_FULL,     // prompt: pick someone to replace
  S_PARTY_LETGO,    // decline the swap
  // stats shortcut in the menu, and the training submenu behind the 5th icon
  S_STATS,          // menu row: jump straight to the stats card page
  S_TRAIN,          // training submenu title
  S_TR_ATK,         // row: train strength (the punching bag)
  S_TR_SPE,         // row: train speed (the ball game)
  S_TR_DEF,         // row: defence -- passive, shown for information only
  S_TR_DEF_HINT,    // why the defence row does nothing when tapped
  // moves card page + the move picker behind it
  S_MOVES,          // card page title
  S_MOVE_EMPTY,     // an unused move slot
  S_MOVE_PICK,      // picker title: choose what goes in this slot
  S_MOVE_TAP,       // hint on the moves page: tap a slot to change it
  S_MOVE_PWR,       // "PWR %u" next to an attacking move
  S_MOVE_STATUS,    // shown instead of power for a status move
  // level-up learn prompt
  S_LEARN_Q,        // "%s wants to learn" (name of the creature)
  S_LEARN_SKIP,     // decline the new move
  // battle
  S_BTL_USED,       // "%s used %s"
  S_BTL_SUPER, S_BTL_WEAK, S_BTL_IMMUNE,
  S_BTL_MISS, S_BTL_CRIT, S_BTL_FAINT,
  S_BTL_HURTSELF,   // confusion
  S_BTL_STATUS,     // "%s was %s" -- ailment name comes from S_AIL_*
  S_AIL_PARA, S_AIL_BURN, S_AIL_POISON, S_AIL_SLEEP, S_AIL_FREEZE, S_AIL_CONFUSE,
  S_BTL_WIN, S_BTL_LOSE,
  S_BTL_SENDS,      // "%s sends out %s"
  S_BTL_GO,         // "Go, %s!"
  S_GYMS,           // gym list title
  S_BADGES_FMT,     // "BADGES %u/8"
  S_TRAINER,        // player card title
  S_SPD_GAIN_FMT,   // "SPEED +%u" after the reaction test
  S_AVATAR_HINT,    // "tap: change avatar"
  S_MEDALS_TOTAL_FMT,  // "%u earned in all"
  S_EASY, S_HARD,   // gym ladder difficulty toggle
  S_PICK_FMT,       // "CHOSEN %u/%u"
  S_FIGHT,          // start the battle
  S_LOCKED,         // a gym not yet unlocked
  S_BTL_SWITCH,     // battle menu: bring on another creature
  S_BTL_BEAT,       // "%s defeated!" on the win screen
  S_BTL_NEWBADGE,   // a badge earned for the first time
  S_VOL_FMT,        // volume level, e.g. "VOL 7"
  S_BOX_FMT,        // "BOX %u/%u"
  S_BOX_SWAP,       // "swap with %s: pick a slot"
  S_BOX_BTN,        // the button on the party screen
  S_REVIVE,         // bring a banked creature back as a companion
  S_REVIVE_EGG,     // why it is unavailable right now
  // LAN battle
  S_LAN, S_LAN_HOST, S_LAN_JOIN, S_LAN_WAIT, S_LAN_READY, S_LAN_REFUSED,
  S_LAN_PICK, S_LAN_VS,
  S_LAN_GONE, S_LAN_WAITFOE, S_LAN_REMATCH,
  S_BTL_RUN,        // the third option in a fight: leave it
  S_EGG_REGION,     // hint under the egg's region pill
  S_WIN_TRAIN_FMT,  // "%s +%u" -- the training a gym win is worth
  S_WIN_MAXED,      // ... or that there is nowhere left to train
  S_CHOOSE_REGION,  // first boot: pick the region your starter comes from
  S_RETIRE,         // menu row: retire the creature on demand
  S_RETIRE_Q,       // ...and the confirmation
  S_RETIRE_COST,    // what retiring early costs the next creature
  S_EVO_SLOW,       // shown on the card while a creature carries that debt
  S_NEED_PACK,      // region chooser: this region's sprite pack is not on the SD
  // Letting a BANKED creature go for good, from the party or box detail sheet.
  // The confirmation itself reuses S_RELEASE_FMT / S_YES / S_NO, which the live
  // pet's own release dialog already owns -- the question is the same question.
  S_RELEASE_BTN,    // the button on the detail sheet
  S_RELEASE_GONE,   // ...and the warning under it: this one does not come back
  S_BOX_TAKE,       // box detail sheet: move this creature into the party
  S_RETIRE_GONE,    // retire confirm: an early retire is not banked either
  // OPTIONS, the second page of SETTINGS
  S_OPTIONS,        // its title
  S_KID_MODE,       // the kid mode switch
  S_KID_HINT,       // ...and what it does, in one line
  S_NEEDS_SPEED,    // how fast the needs drop
  S_NEEDS_HINT,     // ...said plainly
  S_ON, S_OFF,      // a switch's two states
  // the quiz, reached from the Pokedex chooser
  S_QUIZ,           // the button
  S_QUIZ_Q,         // "WHO'S THAT POKEMON?"
  S_QUIZ_RIGHT,     // a right answer
  S_QUIZ_WRONG,     // "IT WAS %s!" -- the answer, after a wrong one
  S_QUIZ_LEFT,      // "%u LEFT TODAY"
  S_QUIZ_DONE,      // the daily allowance is spent
  S_QUIZ_NEXT,      // after an answer: tap for the next one
  S_QUIZ_HEAR,      // tap the silhouette to hear the cry again
  // stickers, the birthday and its present
  S_STICKERS,       // the player card page
  S_NEW_STICKER,    // banner line 1 when one is earned
  S_BIRTHDAY,       // OPTIONS: the birthday row
  S_GIFT_TAP,       // status line while the present waits
  S_GIFT_FULL,      // ...and when party and box have no room for it
  S_HAPPY_BDAY,     // status line on the birthday once it is opened
  S_SET_DATE,       // SETTINGS: the date page's title
  S_DAY, S_MONTH, S_YEAR,   // ...and its three column labels
  S_QUIZ_TODAY_FMT, // "Today: %u/%u", the quiz's day score above its stars
  // exploring, catching, the Pokedex box, experience
  S_EXPLORE,  // menu row, and the map title
  S_PL_MEADOW,  // the eight places, PL_* order
  S_PL_BEACH,
  S_PL_FOREST,
  S_PL_VOLCANO,
  S_PL_MOUNTAIN,
  S_PL_SNOW,
  S_PL_OCEAN,
  S_PL_CAVE,
  S_EXP_HINT,  // the scene: what to do
  S_EXP_EMPTY,  // no outing left
  S_EXP_NEXT_FMT,  // "+1 in %uh%02u" until the next outing
  S_EXP_FLED_FMT,  // "%s ran away!"
  S_EXP_APPEARS_FMT,  // "A wild %s appeared!"
  S_EXP_NOBODY,  // a place with nobody in the installed packs
  S_EXP_NOTEAM,  // nobody to fight with yet
  S_BALL,  // the battle menu cell
  S_CATCH_OK_FMT,  // "Gotcha! %s was caught!"
  S_CATCH_FAIL,  // the ball broke open
  S_WILD_KO,  // the wild one fainted: a hint to use a ball
  S_KEEP_Q_FMT,  // "Keep which %s?" when one is already in the Pokedex
  S_KEEP_OLD_FMT,  // "OLD Lv.%u"
  S_KEEP_NEW_FMT,  // "NEW Lv.%u"
  S_DEXBOX_FMT,  // "In the Pokedex: Lv.%u"
  S_TO_TEAM,  // Pokedex page: move it to the team
  S_TO_DEX,  // Pokedex page: put one of the team here
  S_STORED_FMT,  // "%s is in the Pokedex"
  S_XP_FMT,  // "%s +%u XP"
  S_GREW_FMT,  // "%s grew to Lv.%u!"
  S_EVOLVED_FMT,  // "%s became %s!"
  S_ADOPT,  // Pokedex page, while an egg waits: raise this one instead
  S_ADOPT_SURE,  // the second tap that confirms it
  S_TRADE,  // the LAN screen's TRADE button and the trade screen title
  S_TRADE_GIVE,  // trade: what you put on the table
  S_TRADE_GET,  // trade: what the other trainer offers
  S_TRADE_PICK,  // trade: choose who to offer
  S_TRADE_WAIT,  // trade: you said OK, waiting on them
  S_TRADE_THEYOK,  // trade: they said OK
  S_TRADE_DONE_FMT,  // "%s is yours!"
  S_TRADE_NONE,  // trade: nobody in the party or box
  S_TRADE_CHANGE,  // trade: pick someone else
  S_MENU_TRADE,  // menu tile: go and trade (<= 10 letters, it sits in a tile)
  S_MENU_TEAM,  // menu tile: the party
  S_MENU_SETTINGS,  // menu tile: settings, short enough for a tile
  S_MENU_FAREWELL,  // menu tile: say goodbye to the creature (was RETIRE)
  STR_COUNT
};

const char *T(StrId id);       // texto en el idioma activo
const char *medalName(int i);  // banner de medalla (MED_COUNT)
const char *stickerName(int i); // a sticker's name (STK_COUNT), "" out of range
const char *medalLabel(int i); // etiqueta corta de medalla
const char *medalDesc(int i);  // descripcion larga de medalla

void loadLang();             // lee el idioma de NVS (llamar en setup)
void setLang(Lang l);        // cambia y persiste el idioma
