# What this fork adds: the details

The README gives a short summary of what this fork added after Dylan Dao's
v3.11 (up to v3.32). This page has the rest of the detail. The exact numbers
(odds, thresholds, drain rates) are in the README's
[Game rules and numbers](../README.md#game-rules-and-numbers).

## Exploring and catching

- **EXPLORE** in the menu opens an island with eight places: forest, meadow,
  beach, ocean, cave, mountain, snow and volcano. Each place has its own
  creatures. Tap what moves in the scene and a wild one comes out.
- A wild fight ends with a **BALL**. The lower the creature's HP, the better
  the ball's chances. The wild creature's level follows whoever you send out
  first, so a fresh starter is never matched against your strongest banked
  creature.
- Wild fights have a **RUN** button. No swipe can throw you out of a fight by
  accident, because children kept leaving fights that way.

## Animated fights

The battle system itself (moves, types, gyms, Elite 4, LAN) is Dylan Dao's.
This fork adds the animations on top of it:

- **Each move is drawn in its type:** fireballs, a lightning bolt, leaves, ice
  shards or rocks.
- A **critical hit** bursts into stars, and a super-effective hit shakes the
  target.
- Stat changes show as yellow arrows (up) or blue arrows (down).
- **When you throw a ball**, it arcs over, opens in a flash that draws the
  creature in, drops and wobbles. Then it either clicks shut with stars or
  bursts open.
- The victory music waits for the click.
- **A tap skips any animation.** The narration is kept, so nothing is lost by
  skipping.

## Pokédex box, adoption, experience, trading

- **Every catch goes to the Pokédex box**, one creature per species.
  - The box is a file on the board's FAT partition, because 790 creatures do
    not fit in NVS.
  - From the box you can send a creature **TO TEAM**, or **ADOPT** it in place
    of the waiting egg. An adopted creature is raised from level 1, exactly
    like a hatched one.
- **Banked creatures gain experience** by fighting, and they level up and
  evolve. Their ceiling is the best level you raised yourself.
- **Trading** between two boards over the LAN link. Each side stores the other
  side's creature before it releases its own, so a cut link can duplicate a
  creature but never lose one. `trade_test` cuts the radio at every frame of
  the exchange to check this.
- **A finishable Pokédex.** The totals count only species that can be drawn:
  790, not 809.

## For kids

- **Kid mode** keeps an awake creature's needs above 20. The creature never
  runs away, the volume is capped at 5, and the ball gets +15 %.
- A **needs speed** setting slows the whole care routine down.
- **"Who's that Pokémon?"** shows ten silhouettes a day, and tapping the shadow
  plays its cry.
- **Calendar events** bring their own eggs. There is a **present on the
  player's birthday**, and a sticker book collects all of it.
- **Species and move names follow the UI language.** For example CHARMANDER is
  SALAMECHE in French and GLUMANDA in German.
- **Gentler touch:**
  - taps forgive a slow or shaky finger;
  - the egg's region pill has a guard band, so a miss cannot hatch the egg;
  - holding PWR for **2 s** (not 4) turns the board off.
- **The egg waits for you.** It hatches only on your third tap, never on its
  own.
- **Each touch makes one sound:** a cry replaces the beep it would have come
  with.
- A **date page** in SETTINGS. Without it, the calendar events and the
  birthday could not be reached on a board whose clock had never been set.

## Scenes drawn by code

- The ocean, the volcano, the eight gym arenas and the main screen's homes are
  drawn by `tools/gen_scenes.py` and `tools/gen_home.py`.
  - These scripts are pure code: no source image and no image generator.
  - Each home has four palettes (dawn, day, dusk and night) that follow the
    clock.
- A gym fight takes place in its leader's arena.
- Fights past Kanto keep their own region's team and badge.

## Saves

- The web installer can **download and restore the save** from the browser,
  over the same USB cable.
- An install never wipes a save. `check_installer.py` verifies this on every
  build.
- `EXPORT` / `IMPORT` now carry the Pokédex box as well.

Fixed:

- Saves stopped after the first fight, because a copy of the creature closed
  the NVS handle.
- Catches lost by that bug are now put back in the Pokédex at boot.
- The audio task could reboot the board through the watchdog after music
  stopped mid-note.

## From upstream

The battery and frame-rate work by **ViezeVingertjes** is ported in from the
original repository: the adaptive frame rate, light sleep, the LiPo gauge,
safe power-off, and sprites drawn as runs. See the README's
[Battery](../README.md#battery) section. The port also removed a black blink on
every touch.
