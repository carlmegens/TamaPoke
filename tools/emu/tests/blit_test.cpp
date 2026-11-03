// Sprites drawn as runs (blit.h) leave exactly the pixels per-pixel drawing
// did: random sprites at scales 1-5, empty, full and one-pixel rows, and the
// silhouette joining different colours. Ported from socquique/TamaPoke#25.
#include "blit.h"
#include <cstdio>
#include <vector>
static int bad = 0;
static void ck(bool ok, const char *w) { printf("%s  %s\n", ok ? "PASS" : "FAIL", w); if (!ok) bad++; }

static std::vector<int> paint(const std::vector<uint8_t> &img, int w, int h, int s, bool runs, bool sil) {
  int W = w * s, H = h * s;
  std::vector<int> out(W * H, -1);
  auto rect = [&](int x, int y, int rw, int rh, int col) {
    for (int j = 0; j < rh; j++) for (int i = 0; i < rw; i++) out[(y + j) * W + x + i] = col;
  };
  for (int r = 0; r < h; r++) {
    const uint8_t *row = &img[r * w];
    if (runs) {
      forRuns(w, [&](int c) { return indexedKey(row[c], sil); },
              [&](int c0, int len, int k) { rect(c0 * s, r * s, len * s, s, sil ? 99 : k); });
    } else {
      for (int c = 0; c < w; c++) if (row[c] != 0xFF) rect(c * s, r * s, s, s, sil ? 99 : row[c]);
    }
  }
  return out;
}

int main() {
  uint32_t seed = 12345;
  bool same = true;
  for (int iter = 0; iter < 200; iter++) {
    int w = 1 + iter % 40, h = 1 + (iter / 3) % 30, s = 1 + iter % 5;
    std::vector<uint8_t> img(w * h);
    for (auto &p : img) {
      seed = seed * 1664525u + 1013904223u;
      uint32_t v = (seed >> 16) % 6;
      p = v == 0 ? 0xFF : v == 1 ? 7 : (uint8_t)(v % 3);
    }
    same &= paint(img, w, h, s, true, false) == paint(img, w, h, s, false, false);
    same &= paint(img, w, h, s, true, true) == paint(img, w, h, s, false, true);
  }
  ck(same, "200 random sprites, in colour and as silhouettes, paint the same pixels");
  std::vector<uint8_t> empty(8, 0xFF), full(8, 3), one = {5};
  ck(paint(empty, 8, 1, 2, true, false) == paint(empty, 8, 1, 2, false, false) &&
     paint(full, 8, 1, 2, true, false) == paint(full, 8, 1, 2, false, false) &&
     paint(one, 1, 1, 3, true, false) == paint(one, 1, 1, 3, false, false), "empty, full and one-pixel rows");
  int runs = 0;
  std::vector<uint8_t> row = { 1, 2, 3, 0xFF, 4, 4, 0xFF, 7 };
  forRuns((int)row.size(), [&](int c) { return indexedKey(row[c], true); }, [&](int, int, int) { runs++; });
  ck(runs == 3, "a silhouette joins different colours into one run");
  ck(charKey('.', false) == -1 && (char)charKey((char)0xE9, false) == (char)0xE9, "char maps: '.' is clear, high chars survive");
  printf(bad ? "\n%d FAILED\n" : "\nALL PASS\n", bad);
  return bad ? 1 : 0;
}
