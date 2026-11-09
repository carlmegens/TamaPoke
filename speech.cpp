#include "speech.h"
#include "audio.h"
#include "i18n.h"
#include <Arduino.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(ESP_PLATFORM)
#include <SD_MMC.h>
extern bool sdReady;
static int32_t vSize(const char *p) {
  if (!sdReady || !SD_MMC.exists(p)) return -1;
  File f = SD_MMC.open(p);
  if (!f) return -1;
  int32_t n = (int32_t)f.size();
  f.close();
  return n;
}
static bool vRead(const char *p, uint32_t off, void *buf, uint32_t n) {
  if (!sdReady) return false;
  File f = SD_MMC.open(p);
  if (!f) return false;
  bool ok = f.seek(off) && f.read((uint8_t *)buf, n) == n;
  f.close();
  return ok;
}
#else  // desktop emulator: clips from $TAMAPOKE_VOICE (the folder holding voice/)
static FILE *vOpen(const char *p) {
  const char *base = getenv("TAMAPOKE_VOICE");
  if (!base) return nullptr;
  char full[512];
  snprintf(full, sizeof(full), "%s%s", base, p);
  return fopen(full, "rb");
}
static int32_t vSize(const char *p) {
  FILE *f = vOpen(p);
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  int32_t n = (int32_t)ftell(f);
  fclose(f);
  return n;
}
static bool vRead(const char *p, uint32_t off, void *buf, uint32_t n) {
  FILE *f = vOpen(p);
  if (!f) return false;
  bool ok = fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
  fclose(f);
  return ok;
}
#endif

// ---------------------------------------------------------------------------
// What is on screen
// ---------------------------------------------------------------------------
#define MAX_RUNS 112
#define RUN_LEN 48
#define MAX_BOXES 64
struct Run {
  int16_t x1, y1, x2, y2;   // the text itself
  int16_t hx1, hy1, hx2, hy2;   // where a touch counts: its button, else the text
  uint8_t sx, len;
  bool region;
  char t[RUN_LEN];
};
struct Box { int16_t x1, y1, x2, y2; };
static Run cur[MAX_RUNS], shown[MAX_RUNS];
static Box boxes[MAX_BOXES];
static int nCur, nShown, nBoxes;
static bool runOpen;
static char lastSaid[RUN_LEN];

static bool hasWord(const char *s) {
  for (; *s; s++)
    if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9')) return true;
  return false;
}

void speechGlyph(int16_t x, int16_t y, uint8_t sx, uint8_t sy, char c) {
  if (c == '\n' || c == '\r') { runOpen = false; return; }
  Run *r = (runOpen && nCur) ? &cur[nCur - 1] : nullptr;
  if (r && (r->region || r->y1 != y || r->sx != sx || r->x2 != x || r->len >= RUN_LEN - 1)) r = nullptr;
  if (!r) {
    if (c == ' ' || nCur >= MAX_RUNS) { runOpen = false; return; }
    r = &cur[nCur++];
    r->x1 = r->x2 = x;
    r->y1 = y;
    r->y2 = y + 8 * (sy ? sy : 1);
    r->sx = sx;
    r->len = 0;
    r->region = false;
  }
  r->t[r->len++] = c;
  r->t[r->len] = 0;
  r->x2 = x + 6 * (sx ? sx : 1);
  runOpen = true;
}

void speechBox(int16_t x, int16_t y, int16_t w, int16_t h) {
  // a panel the size of the screen is a background, not a button
  if (w <= 0 || h <= 0 || (int32_t)w * h > 300 * 200 || nBoxes >= MAX_BOXES) return;
  boxes[nBoxes++] = { x, y, (int16_t)(x + w), (int16_t)(y + h) };
}

void speechRegion(int16_t x, int16_t y, int16_t w, int16_t h, const char *text) {
  runOpen = false;
  if (nCur >= MAX_RUNS) return;
  Run &r = cur[nCur++];
  r.x1 = r.hx1 = x;
  r.y1 = r.hy1 = y;
  r.x2 = r.hx2 = x + w;
  r.y2 = r.hy2 = y + h;
  r.sx = 0;
  r.region = true;
  strncpy(r.t, text, RUN_LEN - 1);
  r.t[RUN_LEN - 1] = 0;
  r.len = strlen(r.t);
}

void speechFrame() {
  runOpen = false;
  // A flush with no text at all is an animation step that only redrew a sprite:
  // what was on screen is still there, so keep it.
  if (nCur == 0) { nBoxes = 0; return; }
  for (int i = 0; i < nCur; i++) {
    Run &r = cur[i];
    while (r.len && r.t[r.len - 1] == ' ') r.t[--r.len] = 0;
    if (r.region) continue;
    r.hx1 = r.x1; r.hy1 = r.y1; r.hx2 = r.x2; r.hy2 = r.y2;
    int cx = (r.x1 + r.x2) / 2, cy = (r.y1 + r.y2) / 2;
    int32_t best = INT32_MAX;
    for (int b = 0; b < nBoxes; b++) {
      const Box &k = boxes[b];
      if (cx < k.x1 || cx > k.x2 || cy < k.y1 || cy > k.y2) continue;
      int32_t a = (int32_t)(k.x2 - k.x1) * (k.y2 - k.y1);
      if (a < best) { best = a; r.hx1 = k.x1; r.hy1 = k.y1; r.hx2 = k.x2; r.hy2 = k.y2; }
    }
  }
  memcpy(shown, cur, sizeof(Run) * nCur);
  nShown = nCur;
  nCur = 0;
  nBoxes = 0;
}

void speechDump() {
  Serial.printf("SPEECH ready=%d runs=%d\n", speechReady(), nShown);
  for (int i = 0; i < nShown; i++) {
    const Run &r = shown[i];
    Serial.printf("  [%d,%d %d,%d] hit[%d,%d %d,%d]%s \"%s\"\n", r.x1, r.y1, r.x2, r.y2, r.hx1, r.hy1,
                  r.hx2, r.hy2, r.region ? " region" : "", r.t);
  }
  Serial.println("SPEECH_END");
}

// ---------------------------------------------------------------------------
// Tokens and the clip index
// ---------------------------------------------------------------------------
uint32_t speechHash(const char *s, int n) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
  return h;
}

static int cls(char c) {
  if (c >= 'A' && c <= 'Z') return 1;
  if (c >= 'a' && c <= 'z') return 1;
  if (c >= '0' && c <= '9') return 2;
  return 0;
}

int speechTokens(const char *text, char *out, int outLen, int16_t *starts, int maxTok) {
  int n = 0, o = 0;
  auto add = [&](const char *w, int len) {
    if (n >= maxTok || o + len + 1 >= outLen) return;
    if (n) out[o++] = ' ';
    starts[n++] = o;
    memcpy(out + o, w, len);
    o += len;
    out[o] = 0;
  };
  out[0] = 0;
  const char *p = text;
  while (*p) {
    int c = cls(*p);
    if (!c) {
      bool numBefore = p > text && cls(p[-1]) == 2;
      bool numAfter = cls(p[1]) == 2;
      if (*p == '/' && numBefore && numAfter) add("van", 3);
      else if (*p == '+' && numAfter) add("plus", 4);
      p++;
      continue;
    }
    const char *s = p;
    while (*p && cls(*p) == c) p++;
    char w[32];
    int len = 0;
    if (c == 2) {
      while (s < p - 1 && *s == '0') s++;   // "05" -> "5"
    }
    for (const char *q = s; q < p && len < 31; q++) w[len++] = (*q >= 'A' && *q <= 'Z') ? *q + 32 : *q;
    add(w, len);
  }
  return n;
}

struct VEntry { uint32_t hash; uint16_t chunk, pad; uint32_t off, len; };
static VEntry *gIdx;
static uint32_t gCount;
static uint8_t gMaxTok;
static uint32_t gLastTry;
static bool gTried;

static void loadIndex() {
  if (gIdx) return;
  if (gTried && millis() - gLastTry < 10000) return;
  gTried = true;
  gLastTry = millis();
  int32_t n = vSize("/voice/index.bin");
  if (n < 12) return;
  uint8_t hdr[12];
  if (!vRead("/voice/index.bin", 0, hdr, 12) || memcmp(hdr, "VIX1", 4)) return;
  uint32_t count;
  memcpy(&count, hdr + 4, 4);
  if ((int32_t)(12 + count * sizeof(VEntry)) > n) return;
  VEntry *idx = (VEntry *)ps_malloc(count * sizeof(VEntry));
  if (!idx) return;
  if (!vRead("/voice/index.bin", 12, idx, count * sizeof(VEntry))) { free(idx); return; }
  gIdx = idx;
  gCount = count;
  gMaxTok = hdr[8] ? hdr[8] : 1;
}

bool speechReady() {
  if (gLang != LANG_NL) return false;
  loadIndex();
  return gIdx != nullptr;
}

static const VEntry *find(const char *s, int n) {
  uint32_t h = speechHash(s, n);
  uint32_t lo = 0, hi = gCount;
  while (lo < hi) {
    uint32_t m = (lo + hi) / 2;
    if (gIdx[m].hash < h) lo = m + 1;
    else hi = m;
  }
  return (lo < gCount && gIdx[lo].hash == h) ? &gIdx[lo] : nullptr;
}

#define SAY_MAX (16000 * 10)   // 10 s of 8-bit 16 kHz
#define SAY_GAP 960            // 60 ms between words

bool speechSay(const char *text) {
  if (!speechReady() || !audioEnabled()) return false;
  char norm[160];
  int16_t st[24];
  int n = speechTokens(text, norm, sizeof(norm), st, 24);
  if (!n) return false;

  const VEntry *parts[32];
  int np = 0;
  for (int i = 0; i < n && np < 32;) {
    const VEntry *e = nullptr;
    int L = gMaxTok < n - i ? gMaxTok : n - i;
    for (; L >= 1; L--) {
      int end = (i + L < n) ? st[i + L] - 1 : (int)strlen(norm);
      if ((e = find(norm + st[i], end - st[i]))) break;
    }
    if (e) { parts[np++] = e; i += L; continue; }
    // an unknown number is read digit by digit; an unknown word is skipped
    int end = (i + 1 < n) ? st[i + 1] - 1 : (int)strlen(norm);
    for (int k = st[i]; k < end && np < 32; k++)
      if (norm[k] >= '0' && norm[k] <= '9')
        if (const VEntry *d = find(norm + k, 1)) parts[np++] = d;
    i++;
  }
  if (!np) return false;

  uint32_t total = 0;
  for (int i = 0; i < np; i++) total += parts[i]->len + (i ? SAY_GAP : 0);
  if (total > SAY_MAX) total = SAY_MAX;
  int8_t *pcm = (int8_t *)ps_malloc(total);
  if (!pcm) return false;
  uint32_t at = 0;
  for (int i = 0; i < np && at < total; i++) {
    if (i) {
      uint32_t g = SAY_GAP < total - at ? SAY_GAP : total - at;
      memset(pcm + at, 0, g);
      at += g;
    }
    uint32_t len = parts[i]->len < total - at ? parts[i]->len : total - at;
    char path[24];
    snprintf(path, sizeof(path), "/voice/v%02u.bin", parts[i]->chunk);
    if (!vRead(path, parts[i]->off, pcm + at, len)) memset(pcm + at, 0, len);
    at += len;
  }
#if !defined(ESP_PLATFORM)
  printf("speech: \"%s\" -> %s (%d clips)\n", text, norm, np);
#endif
  audioStopPcm();   // a new touch cuts off whatever was still being said
  audioPlayPcm(pcm, at);
  return true;
}

bool speechAt(int16_t x, int16_t y, bool fresh) {
  if (fresh) lastSaid[0] = 0;
  if (!nShown || !speechReady()) return false;
  int best = -1;
  int32_t bestD = INT32_MAX, bestA = INT32_MAX;
  for (int i = 0; i < nShown; i++) {
    const Run &r = shown[i];
    if (!hasWord(r.t)) continue;
    int dx = x < r.hx1 ? r.hx1 - x : (x > r.hx2 ? x - r.hx2 : 0);
    int dy = y < r.hy1 ? r.hy1 - y : (y > r.hy2 ? y - r.hy2 : 0);
    int32_t d = dx * dx + dy * dy;
    int32_t a = (int32_t)(r.hx2 - r.hx1) * (r.hy2 - r.hy1);
    if (d < bestD || (d == bestD && a < bestA)) { best = i; bestD = d; bestA = a; }
  }
  if (best < 0 || bestD > 28 * 28) return false;
  if (!strcmp(shown[best].t, lastSaid)) return false;
  strcpy(lastSaid, shown[best].t);
  return speechSay(shown[best].t);
}
