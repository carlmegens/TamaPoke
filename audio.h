#pragma once
#include <stdint.h>

// Efectos de sonido del juego (cola, no bloqueante). El orden coincide con la
// tabla SFX de audio.cpp.
enum Sfx : uint8_t {
  SFX_TAP = 0,  // tocar / boton
  SFX_EAT,      // comer
  SFX_PLAY,     // punto del minijuego / golpe
  SFX_HEART,    // le gusta / mimo
  SFX_HATCH,    // eclosion
  SFX_EVOLVE,   // evolucion
  SFX_MEDAL,    // medalla / hito
  SFX_DENY,     // accion no permitida
  SFX_BYE,      // despedida
  SFX_LEVEL,    // sube de nivel
  // battle cues: a fight in silence is what made it feel flat
  SFX_HIT,      // a physical move landing
  SFX_BEAM,     // a special move
  SFX_STATUS,   // a status move or an ailment taking hold
  SFX_SUPER,    // super effective
  SFX_FAINT,    // something goes down
  SFX_VICTORY,  // the fight is won
  SFX_COUNT
};

// Background music. One square-wave voice and a single blocking audio task, so
// this is not mixed with the effects -- the task plays the tune a note at a
// time and hands the voice to any effect that arrives, resuming after. An
// effect therefore always cuts through, which is what you want anyway.
enum Music : uint8_t { MUS_NONE = 0, MUS_BATTLE, MUS_VICTORY };
void audioMusic(uint8_t id);

// 0..10. Stored, and applied as the square wave's amplitude; 0 is silence
// without disabling the system, which is what a mute expects to do.
void audioSetVolume(uint8_t v);
uint8_t audioVolume();

void audioBegin();          // init ES8311 + I2S + amplificador + tarea de audio

// A species' cry, streamed from /cries/cNNN.raw on the SD card (pack_cries.py).
// Silent when the card has no cries pack -- they are optional, like every pack.
// Defined in sdmon.cpp on the board, since it reads the card, and in the
// emulator's host_impl.cpp. True when a cry was queued, so a caller whose
// cue stands in for the cry plays it only when there was none (cryOr()).
bool cryPlay(int16_t dex);
// Plays signed 8-bit mono PCM at the I2S rate and TAKES OWNERSHIP of `pcm`
// (heap or PSRAM): the audio task frees it once played, or at once if refused.
// Queued in order with the effects, so a jingle and then a cry play in turn.
void audioPlayPcm(int8_t *pcm, uint32_t len);
void sfxPlay(uint8_t id);   // encola un efecto (no bloquea el loop)
void audioSetEnabled(bool on);
bool audioEnabled();
bool audioActive();      // music, an effect or a cry still sounding: the CPU must not sleep
