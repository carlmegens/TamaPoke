#pragma once
#include <stdint.h>

// Joins the neighbouring pixels of one colour in a sprite row: one fillRect per
// run instead of one per pixel, with identical pixels on screen. Ported from
// socquique/TamaPoke#25; blit_test paints both ways and compares.
// key(c) is pixel c's colour key, or -1 when transparent; emit(c0, len, key)
// receives each opaque run.
template <class Key, class Emit>
inline void forRuns(int w, Key key, Emit emit) {
  int c = 0, k = w > 0 ? key(0) : -1;
  while (c < w) {
    int c0 = c, kn = k;
    while (++c < w && (kn = key(c)) == k) {}
    if (k >= 0) emit(c0, c - c0, k);
    k = kn;  // the key that ended a run opens the next one: not read twice
  }
}

// an indexed pixel's key: -1 = transparent (0xFF); as a silhouette every opaque
// pixel shares one key, since they are all drawn in ink
inline int indexedKey(uint8_t idx, bool sil) { return idx == 0xFF ? -1 : (sil ? 0 : idx); }

// the same for char maps: '.' is transparent, anything else its own character,
// unsigned so that spriteColor() gets it back intact through (char)key
inline int charKey(char ch, bool sil) { return ch == '.' ? -1 : (sil ? 0 : (unsigned char)ch); }
