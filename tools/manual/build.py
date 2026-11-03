# Builds the kids' manual, EN and FR, from the screenshots cap.cpp took.
#   python build.py <work dir holding shots/> <out dir>
# Run through build.sh, which takes the screenshots first. Needs reportlab and
# Pillow, and Comic Sans (Windows and macOS ship it; on Linux, the
# ttf-mscorefonts package).
import os, sys, math
from PIL import Image, ImageDraw, ImageFont
from reportlab.lib.pagesizes import A4
from reportlab.pdfgen import canvas
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.lib.utils import simpleSplit
from reportlab.lib.colors import HexColor, white, black

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'work')
OUT = sys.argv[2] if len(sys.argv) > 2 else WORK
SHOTS = os.path.join(WORK, 'shots')
TMP = os.path.join(WORK, 'ann')
os.makedirs(TMP, exist_ok=True)

def find_font(names):
    dirs = ['C:/Windows/Fonts', '/Library/Fonts', '/System/Library/Fonts/Supplementary',
            '/usr/share/fonts/truetype/msttcorefonts', os.path.expanduser('~/.fonts')]
    for d in dirs:
        for n in names:
            if os.path.exists(os.path.join(d, n)): return os.path.join(d, n)
    raise SystemExit('Comic Sans not found (%s): install it or add its folder to find_font()' % names[0])

REG = find_font(['comic.ttf', 'Comic Sans MS.ttf', 'Comic_Sans_MS.ttf'])
BOLD = find_font(['comicbd.ttf', 'Comic Sans MS Bold.ttf', 'Comic_Sans_MS_Bold.ttf'])
pdfmetrics.registerFont(TTFont('Kid', REG))
pdfmetrics.registerFont(TTFont('KidB', BOLD))
PILFONT = BOLD

W, H = A4
M = 40
INK = HexColor('#2b2b3a')
RED = HexColor('#e2453c'); BLUE = HexColor('#3b7dd8'); GREEN = HexColor('#3aa65a')
YEL = HexColor('#f6c544'); PURP = HexColor('#8a5cc7'); ORANGE = HexColor('#f08a2c')
PINK = HexColor('#e85d9a'); TEAL = HexColor('#2aa3a3')
SOFT = {RED: '#fde7e5', BLUE: '#e5effc', GREEN: '#e4f5e8', YEL: '#fdf4dc', PURP: '#efe7f9',
        ORANGE: '#fdeedd', PINK: '#fce6f0', TEAL: '#ddf3f3'}

# ---------- screenshots ----------
def shot(name, lang, marks=(), arrows=(), size=466):
    """A round device view of a capture, with numbered rings and arrows drawn
    in the panel's own 466 px coordinates."""
    src = Image.open(os.path.join(SHOTS, f'{name}_{lang}.png')).convert('RGB')
    S = 466; pad = 22
    im = Image.new('RGBA', (S + 2 * pad, S + 2 * pad), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse((0, 0, S + 2 * pad - 1, S + 2 * pad - 1), fill=(40, 40, 52, 255))      # the case
    mask = Image.new('L', (S, S), 0); ImageDraw.Draw(mask).ellipse((0, 0, S - 1, S - 1), fill=255)
    im.paste(src, (pad, pad), mask)
    d = ImageDraw.Draw(im)
    f = ImageFont.truetype(PILFONT, 26)
    for (x0, y0, x1, y1, lab) in arrows:
        x0 += pad; y0 += pad; x1 += pad; y1 += pad
        d.line((x0, y0, x1, y1), fill=(226, 69, 60, 255), width=10)
        a = math.atan2(y1 - y0, x1 - x0)
        p1 = (x1 + 24 * math.cos(a), y1 + 24 * math.sin(a))
        p2 = (x1 + 22 * math.cos(a + 2.4), y1 + 22 * math.sin(a + 2.4))
        p3 = (x1 + 22 * math.cos(a - 2.4), y1 + 22 * math.sin(a - 2.4))
        d.polygon([p1, p2, p3], fill=(226, 69, 60, 255))
        if lab:
            cx, cy = x0 - 0 * math.cos(a), y0
            badge(d, f, x0 - 26 * math.cos(a), y0 - 26 * math.sin(a), lab)
    for m in marks:
        n, x, y = m[0], m[1] + pad, m[2] + pad
        r = m[3] if len(m) > 3 else 34
        if r:
            d.ellipse((x - r, y - r, x + r, y + r), outline=(226, 69, 60, 255), width=6)
        bx = m[4] + pad if len(m) > 4 else x + r * 0.75
        by = m[5] + pad if len(m) > 5 else y - r * 0.75
        badge(d, f, bx, by, str(n))
    p = os.path.join(TMP, f'{name}_{lang}_{abs(hash((marks, arrows))) % 99999}.png')
    im.save(p)
    return p

def badge(d, f, x, y, txt):
    R = 21
    d.ellipse((x - R, y - R, x + R, y + R), fill=(246, 197, 68, 255), outline=(43, 43, 58, 255), width=4)
    tw = d.textlength(txt, font=f)
    d.text((x - tw / 2, y - 18), txt, font=f, fill=(43, 43, 58, 255))

# ---------- page pieces ----------
class Doc:
    def __init__(self, path, lang, T):
        self.c = canvas.Canvas(path, pagesize=A4)
        self.c.setTitle(T['doc_title']); self.c.setAuthor('TamaPoke')
        self.lang = lang; self.T = T; self.page = 0

    def new(self, title, color, subtitle=None):
        if self.page: self.footer(); self.c.showPage()
        self.page += 1
        c = self.c
        c.setFillColor(color); c.roundRect(M - 10, H - 92, W - 2 * M + 20, 62, 18, fill=1, stroke=0)
        for i, col in enumerate((YEL, white, YEL)):
            c.setFillColor(col); c.circle(W - M - 20 - i * 22, H - 61, 6, fill=1, stroke=0)
        c.setFillColor(white); c.setFont('KidB', 28); c.drawString(M + 8, H - 72, title)
        self.y = H - 112
        if subtitle:
            self.y = self.text(subtitle, M, self.y, W - 2 * M, 14, INK) - 6
        self.color = color

    def footer(self):
        c = self.c
        c.setFont('Kid', 10); c.setFillColor(HexColor('#888899'))
        c.drawCentredString(W / 2, 22, f"{self.T['footer']}  -  {self.page}")

    def text(self, s, x, y, w, size=14, color=INK, font='Kid', lead=1.3):
        """Wrapped text; **...** spans are bold. Returns the y below it."""
        c = self.c; c.setFillColor(color)
        sp = pdfmetrics.stringWidth(' ', font, size)
        for para in s.split(chr(10)):
            words = []
            prev = ''
            for k, (txt, b) in enumerate(self.rich(para)):
                # a chunk glues to the last word only when no space separates them
                joined = k > 0 and not txt.startswith(' ') and not prev.endswith(' ')
                prev = txt
                for j, wd in enumerate(txt.split(' ')):
                    if not wd: continue
                    glue = joined and j == 0 and words
                    words.append((wd, b, bool(glue)))
            line, lw = [], 0
            def flush(line, y):
                xx = x
                for i, (wd, b, g) in enumerate(line):
                    if i and g: xx -= sp
                    f = 'KidB' if b else font
                    c.setFont(f, size); c.drawString(xx, y, wd)
                    xx += pdfmetrics.stringWidth(wd, f, size) + sp
            for wd, b, g in words:
                ww = pdfmetrics.stringWidth(wd, 'KidB' if b else font, size)
                if line and not g and lw + ww > w:
                    flush(line, y); y -= size * lead; line, lw = [], 0
                line.append((wd, b, g)); lw += ww + (0 if g else sp)
            flush(line, y); y -= size * lead
        return y

    @staticmethod
    def rich(s):
        out, bold = [], False
        for i, chunk in enumerate(s.split('**')):
            if chunk: out.append((chunk, i % 2 == 1))
        return out

    def img(self, path, x, y, w):
        self.c.drawImage(path, x, y - w, w, w, mask='auto')
        return y - w

    def legend(self, items, x, y, w, size=13.5):
        c = self.c
        for n, s in items:
            c.setFillColor(YEL); c.setStrokeColor(INK); c.setLineWidth(1.5)
            c.circle(x + 11, y - 2, 11, fill=1, stroke=1)
            c.setFillColor(INK); c.setFont('KidB', 13); c.drawCentredString(x + 11, y - 6.5, str(n))
            y = self.text(s, x + 30, y - 6, w - 30, size) - 6
        return y

    def box(self, label, s, x, y, w, color, size=12.5):
        # measure first
        lines = 0
        for para in s.split('\n'):
            lines += max(1, len(simpleSplit(para.replace('**', ''), 'Kid', size, w - 28)))
        h = lines * size * 1.3 + 38
        c = self.c
        c.setFillColor(HexColor(SOFT.get(color, '#f2f2f2'))); c.setStrokeColor(color); c.setLineWidth(2)
        c.roundRect(x, y - h, w, h, 12, fill=1, stroke=1)
        c.setFillColor(color); c.setFont('KidB', 13); c.drawString(x + 14, y - 20, label)
        self.text(s, x + 14, y - 38, w - 28, size)
        return y - h - 12

    def caption(self, s, x, y, w):
        self.c.setFillColor(HexColor('#666677')); self.c.setFont('Kid', 11)
        self.c.drawCentredString(x + w / 2, y - 14, s)
        return y - 22

    def save(self):
        self.footer(); self.c.save()

# ---------- the manual ----------
def build(lang, T):
    L = {'en': 1, 'fr': 2, 'de': 3}[lang]     # the firmware's Lang: LANG_EN, LANG_FR, LANG_DE
    path = os.path.join(OUT, T['file'])
    d = Doc(path, lang, T)
    c = d.c
    CW = W - 2 * M

    # 1 -- cover
    d.page = 1
    c.setFillColor(HexColor('#fdf4dc')); c.rect(0, 0, W, H, fill=1, stroke=0)
    for i, (col, x, y, r) in enumerate([(RED, 80, 760, 40), (BLUE, 520, 790, 30), (GREEN, 60, 120, 34),
                                        (PURP, 530, 140, 44), (ORANGE, 500, 470, 18), (PINK, 90, 470, 22)]):
        c.setFillColor(col); c.circle(x, y, r, fill=1, stroke=0)
    c.setFillColor(RED); c.setFont('KidB', 60); c.drawCentredString(W / 2, H - 150, 'TamaPoke')
    c.setFillColor(INK); c.setFont('KidB', 22); c.drawCentredString(W / 2, H - 190, T['cover_sub'])
    d.img(shot('main', L), W / 2 - 150, H - 220, 300)
    c.setFont('Kid', 16); c.drawCentredString(W / 2, 250, T['cover_line'])
    c.setStrokeColor(INK); c.setLineWidth(1.5)
    c.setFont('KidB', 16); c.drawString(110, 190, T['belongs'])
    c.line(110 + pdfmetrics.stringWidth(T['belongs'], 'KidB', 16) + 8, 188, W - 110, 188)
    c.setFont('Kid', 11); c.setFillColor(HexColor('#777788'))
    c.drawCentredString(W / 2, 60, T['cover_small'])

    # 2 -- meet your device
    d.new(T['p_device'], BLUE, T['device_intro'])
    y = d.y - 4
    y = d.box(T['pwr_label'], T['pwr'], M, y, CW, BLUE)
    # gestures, drawn
    c.setFillColor(INK); c.setFont('KidB', 18); c.drawString(M, y - 8, T['fingers']); y -= 30
    gw = CW / 3
    for i, (key, kind) in enumerate([('tap', 0), ('swipe', 1), ('hold', 2)]):
        cx = M + gw * i + gw / 2; cy = y - 60
        c.setFillColor(HexColor('#e5effc')); c.circle(cx, cy, 52, fill=1, stroke=0)
        c.setFillColor(HexColor('#f2c9a5')); c.setStrokeColor(INK); c.setLineWidth(2)
        c.roundRect(cx - 9, cy - 46, 18, 46, 9, fill=1, stroke=1)          # a finger
        if kind == 0:
            c.setStrokeColor(RED); c.setLineWidth(3)
            for r in (12, 20): c.circle(cx, cy + 6, r, fill=0, stroke=1)
        elif kind == 1:
            c.setStrokeColor(RED); c.setLineWidth(4); c.line(cx - 40, cy + 22, cx + 30, cy + 22)
            c.setFillColor(RED); p = c.beginPath(); p.moveTo(cx + 42, cy + 22); p.lineTo(cx + 28, cy + 31); p.lineTo(cx + 28, cy + 13); p.close(); c.drawPath(p, fill=1, stroke=0)
        else:
            c.setFillColor(RED); c.setFont('KidB', 16); c.drawCentredString(cx, cy + 18, '3 s')
        c.setFillColor(INK); c.setFont('KidB', 15); c.drawCentredString(cx, cy - 74, T['g_' + key])
        d.text(T['g_' + key + '_t'], cx - gw / 2 + 10, cy - 92, gw - 20, 12)
    y = y - 222
    y = d.box(T['charge_label'], T['charge'], M, y, CW, GREEN)

    # 3 -- start
    d.new(T['p_start'], GREEN, T['start_intro'])
    y = d.y
    s = 250
    d.img(shot('region', L, marks=((1, 233, 140, 0, 36, 140),)), M, y, s)
    d.img(shot('starter', L, marks=((2, 233, 220, 0, 36, 220),)), W - M - s, y, s)
    y -= s + 10
    y = d.legend([(1, T['start_1']), (2, T['start_2'])], M, y, CW)
    y = d.box(T['tip'], T['start_tip'], M, y - 6, CW, YEL)

    # 4 -- the egg
    d.new(T['p_egg'], ORANGE, T['egg_intro'])
    y = d.y; s = 300
    d.img(shot('egg', L, marks=((1, 233, 200, 62), (2, 233, 390, 0, 120, 390))), M, y, s)
    d.legend([(1, T['egg_1']), (2, T['egg_2'])], M + s + 20, y - 30, CW - s - 20)
    y -= s + 10
    y = d.box(T['tip'], T['egg_tip'], M, y, CW, YEL)

    # 5 -- the main screen
    d.new(T['p_main'], RED, T['main_intro'])
    y = d.y; s = 290
    d.img(shot('main', L, marks=((1, 233, 66, 0, 70, 66), (2, 233, 200, 70), (3, 233, 338, 0, 40, 338),
                                 (4, 233, 396, 0, 400, 396))), M, y, s)
    d.legend([(1, T['main_1']), (2, T['main_2']), (3, T['main_3']), (4, T['main_4'])], M + s + 16, y - 20, CW - s - 16, 13)
    y -= s + 14
    # the four bars
    bars = [(T['bar_food'], ORANGE), (T['bar_joy'], PINK), (T['bar_ene'], YEL), (T['bar_hyg'], BLUE)]
    bw = (CW - 30) / 4
    for i, (s_, col) in enumerate(bars):
        x = M + i * (bw + 10)
        c.setFillColor(HexColor(SOFT[col])); c.setStrokeColor(col); c.setLineWidth(2)
        c.roundRect(x, y - 92, bw, 92, 10, fill=1, stroke=1)
        name, txt = s_.split('|')
        c.setFillColor(col); c.setFont('KidB', 15); c.drawString(x + 10, y - 22, name)
        d.text(txt, x + 10, y - 42, bw - 18, 11.5)
    y -= 106
    y = d.box(T['tip'], T['bars_tip'], M, y, CW, GREEN)

    # 6 -- taking care
    d.new(T['p_care'], PINK, T['care_intro'])
    y = d.y; s = 240
    d.img(shot('feed', L, marks=((1, 134, 318, 0, 134, 262), (2, 200, 318, 0, 200, 262), (3, 266, 318, 0, 266, 262),
                                 (4, 332, 318, 0, 332, 262))), M, y, s)
    d.legend([(1, T['berry_red']), (2, T['berry_blue']), (3, T['berry_green']), (4, T['candy'])],
             M + s + 16, y - 10, CW - s - 16, 12.5)
    y -= s + 8
    d.img(shot('sleep', L, marks=((5, 200, 405, 34),)), M, y, s)
    yy = d.legend([(5, T['light'])], M + s + 16, y - 10, CW - s - 16, 12.5)
    yy = d.legend([(6, T['bath']), (7, T['pet'])], M + s + 16, yy - 4, CW - s - 16, 12.5)
    y -= s + 8
    d.box(T['tip'], T['care_tip'], M, y, CW, YEL)

    # 7 -- training
    d.new(T['p_train'], PURP, T['train_intro'])
    y = d.y; s = 300
    d.img(shot('train', L, marks=((1, 233, 166, 0, 440, 166), (2, 233, 230, 0, 440, 230), (3, 233, 294, 0, 440, 294))), M, y, s)
    d.legend([(1, T['tr_str']), (2, T['tr_spd']), (3, T['tr_def'])], M + s + 16, y - 20, CW - s - 16, 13)
    y -= s + 14
    d.box(T['tip'], T['train_tip'], M, y, CW, YEL)

    # 8 -- swipes
    d.new(T['p_swipe'], TEAL, T['swipe_intro'])
    y = d.y - 40; s = 250
    cx, cy = W / 2, y - s / 2
    d.img(shot('main', L), cx - s / 2, y, s)
    def arrow(x0, y0, x1, y1, n, bx, by):
        c.setStrokeColor(RED); c.setLineWidth(6); c.line(x0, y0, x1, y1)
        a = math.atan2(y1 - y0, x1 - x0); p = c.beginPath()
        p.moveTo(x1 + 12 * math.cos(a), y1 + 12 * math.sin(a))
        p.lineTo(x1 + 12 * math.cos(a + 2.4), y1 + 12 * math.sin(a + 2.4))
        p.lineTo(x1 + 12 * math.cos(a - 2.4), y1 + 12 * math.sin(a - 2.4)); p.close()
        c.setFillColor(RED); c.drawPath(p, fill=1, stroke=0)
        c.setFillColor(YEL); c.setStrokeColor(INK); c.setLineWidth(1.5); c.circle(bx, by, 12, fill=1, stroke=1)
        c.setFillColor(INK); c.setFont('KidB', 14); c.drawCentredString(bx, by - 5, str(n))
    arrow(cx, cy + s / 2 + 2, cx, cy + s / 2 + 34, 1, cx + 26, cy + s / 2 + 18)
    arrow(cx, cy - s / 2 - 2, cx, cy - s / 2 - 34, 2, cx + 26, cy - s / 2 - 18)
    arrow(cx - s / 2 - 4, cy, cx - s / 2 - 50, cy, 3, cx - s / 2 - 28, cy + 24)
    arrow(cx + s / 2 + 4, cy, cx + s / 2 + 50, cy, 4, cx + s / 2 + 28, cy + 24)
    y -= s + 50
    y = d.legend([(1, T['sw_up']), (2, T['sw_down']), (3, T['sw_left']), (4, T['sw_right'])], M, y, CW)
    s2 = 165
    d.img(shot('card1', L), M + 40, y, s2); d.img(shot('card2', L), W - M - 40 - s2, y, s2)
    d.caption(T['card_cap1'], M + 40, y - s2, s2); d.caption(T['card_cap2'], W - M - 40 - s2, y - s2, s2)

    # 9 -- growing up
    d.new(T['p_grow'], GREEN, T['grow_intro'])
    y = d.y
    for lab, key, col in [(T['g1_l'], 'g1', BLUE), (T['g2_l'], 'g2', ORANGE), (T['g3_l'], 'g3', PURP),
                          (T['g4_l'], 'g4', PINK), (T['g5_l'], 'g5', RED)]:
        y = d.box(lab, T[key], M, y, CW, col, 13)

    # 10 -- party
    d.new(T['p_party'], ORANGE, T['party_intro'])
    y = d.y; s = 270
    d.img(shot('party', L, marks=((1, 160, 126, 48), (2, 233, 340, 0, 120, 340))), M, y, s)
    d.legend([(1, T['party_1']), (2, T['party_2'])], M + s + 16, y - 20, CW - s - 16, 13)
    y -= s + 14
    y = d.box(T['tip'], T['party_tip'], M, y, CW, YEL)
    s = 190
    d.img(shot('sheetxp', L, marks=((3, 233, 288, 0, 120, 288),)), M, y, s)
    d.box(T['xp_l'], T['xp'], M + s + 14, y - 10, CW - s - 14, GREEN, 12.5)

    # 11 -- battles
    d.new(T['p_battle'], RED, T['battle_intro'])
    y = d.y; s = 165
    xs = [M, M + (CW - s) / 2, W - M - s]
    d.img(shot('gympick', L, marks=((1, 233, 130, 0, 46, 130),)), xs[0], y, s)
    d.img(shot('gyms', L, marks=((2, 233, 126, 0, 46, 126),)), xs[1], y, s)
    d.img(shot('fight', L, marks=((3, 233, 294, 0, 100, 294),)), xs[2], y, s)
    y -= s + 6
    y = d.legend([(1, T['b1']), (2, T['b2']), (3, T['b3'])], M, y, CW, 13)
    s = 190
    d.img(shot('fightmoves', L, marks=((4, 150, 290, 0, 46, 290),)), M + 20, y, s)
    d.img(shot('win', L), W - M - 20 - s, y, s)
    y -= s + 6
    y = d.legend([(4, T['b4']), (5, T['b5'])], M, y, CW, 13)

    # 12 -- exploring
    d.new(T['p_explore'], GREEN, T['explore_intro'])
    y = d.y; s = 165
    d.img(shot('menu', L, marks=((1, 233, 164, 0, 80, 164),)), xs[0], y, s)
    d.img(shot('explmap', L, marks=((2, 233, 52, 0, 346, 52), (3, 128, 204, 34), (5, 113, 46, 30))), xs[1], y, s)
    d.img(shot('explscene', L, marks=((4, 276, 200, 44),)), xs[2], y, s)
    y -= s + 6
    y = d.legend([(1, T['e1']), (2, T['e2']), (3, T['e3']), (4, T['e4']), (5, T['e5'])], M, y, CW, 13)
    s = 150
    d.img(shot('explocean', L), M + 40, y, s); d.img(shot('explvolcano', L), W - M - 40 - s, y, s)
    d.caption(T['ocean_cap'], M + 40, y - s, s); d.caption(T['volcano_cap'], W - M - 40 - s, y - s, s)
    y -= s + 30
    d.box(T['tip'], T['explore_tip'], M, y, CW, YEL)

    # 13 -- catching
    d.new(T['p_catch'], RED, T['catch_intro'])
    y = d.y; s = 165
    d.img(shot('wild', L, marks=((1, 317, 296, 46),)), xs[0], y, s)
    d.img(shot('caught', L, marks=((2, 324, 100, 40),)), xs[1], y, s)
    d.img(shot('keep', L, marks=((3, 233, 216, 0, 60, 216),)), xs[2], y, s)
    y -= s + 6
    y = d.legend([(1, T['c1']), (2, T['c2']), (3, T['c3'])], M, y, CW, 13)
    s = 190
    d.img(shot('dexbox', L, marks=((4, 152, 367, 0, 40, 367), (5, 314, 367, 0, 426, 367))), M, y, s)
    d.legend([(4, T['c4']), (5, T['c5'])], M + s + 16, y - 20, CW - s - 16, 13)
    y -= s + 10
    d.box(T['tip'], T['catch_tip'], M, y, CW, YEL)

    # 14 -- pokedex
    d.new(T['p_dex'], BLUE, T['dex_intro'])
    y = d.y; s = 165
    d.img(shot('menu', L, marks=((1, 233, 214, 0, 80, 214),)), xs[0], y, s)
    d.img(shot('dexpick', L, marks=((2, 233, 130, 0, 46, 130),)), xs[1], y, s)
    d.img(shot('dex', L, marks=((3, 353, 124, 38),)), xs[2], y, s)
    y -= s + 6
    y = d.legend([(1, T['d1']), (2, T['d2']), (3, T['d3'])], M, y, CW, 13)
    s = 190
    d.img(shot('dexdetail', L, marks=((4, 233, 262, 60),)), W / 2 - s / 2, y, s)
    y -= s + 4
    y = d.legend([(4, T['d4'])], M, y, CW, 13)

    # 13 -- quiz
    d.new(T['p_quiz'], PURP, T['quiz_intro'])
    y = d.y; s = 165
    d.img(shot('dexpick', L, marks=((1, 233, 358, 0, 120, 358),)), xs[0], y, s)
    d.img(shot('quiz', L, marks=((2, 233, 150, 56), (3, 233, 330, 0, 60, 330))), xs[1], y, s)
    d.img(shot('quizright', L), xs[2], y, s)
    y -= s + 6
    y = d.legend([(1, T['q1']), (2, T['q2']), (3, T['q3'])], M, y, CW, 13)
    s = 180
    d.img(shot('quizdone', L), M, y, s)
    d.legend([(4, T['q4'])], M + s + 16, y - 30, CW - s - 16, 13)
    y -= s + 10
    d.box(T['tip'], T['quiz_tip'], M, y, CW, YEL)

    # 14 -- trainer card, stickers, special days
    d.new(T['p_card'], TEAL, T['card_intro'])
    y = d.y; s = 180
    d.img(shot('player', L), M + 30, y, s); d.img(shot('stickers', L), W - M - 30 - s, y, s)
    d.caption(T['player_cap'], M + 30, y - s, s); d.caption(T['stickers_cap'], W - M - 30 - s, y - s, s)
    y -= s + 44
    c.setFillColor(INK); c.setFont('KidB', 17); c.drawString(M, y, T['days_title']); y -= 22
    for i, (day, what) in enumerate(T['days']):
        c.setFillColor(ORANGE); c.setFont('KidB', 12.5); c.drawString(M + 10, y, day)
        c.setFillColor(INK); c.setFont('Kid', 12.5); c.drawString(M + 160, y, what)
        y -= 19
    y -= 8
    s = 150
    d.img(shot('birthday', L, marks=((1, 73, 270, 40),)), M, y, s)
    d.box(T['bday_label'], T['bday'], M + s + 14, y, CW - s - 14, PINK, 12.5)

    # 15 -- grown-ups: settings
    d.new(T['p_settings'], INK, T['settings_intro'])
    y = d.y; s = 165
    d.img(shot('clock', L, marks=((1, 233, 22, 18), (2, 233, 364, 0, 120, 364))), xs[0], y, s)
    d.img(shot('date', L), xs[1], y, s)
    d.img(shot('options', L), xs[2], y, s)
    for i, k in enumerate(('set_cap1', 'set_cap2', 'set_cap3')):
        d.caption(T[k], xs[i], y - s, s)
    y -= s + 26
    y = d.legend([(1, T['s1']), (2, T['s2'])], M, y, CW, 12.5)
    for lab, key, col in [(T['kid_l'], 'kid', GREEN), (T['spd_l'], 'spd', BLUE), (T['date_l'], 'datet', ORANGE)]:
        y = d.box(lab, T[key], M, y, CW, col, 12)

    # 16 -- help
    d.new(T['p_help'], GREEN)
    y = d.y
    for q, a in T['help']:
        c.setFillColor(RED); c.setFont('KidB', 14); c.drawString(M, y, q); y -= 18
        y = d.text(a, M + 14, y, CW - 14, 12.5) - 8
    y = d.box(T['grown_l'], T['grown'], M, y - 4, CW, BLUE, 11.5)
    d.save()
    print('wrote', path, d.page, 'pages')

from texts import EN, FR, DE
build('en', EN)
build('fr', FR)
build('de', DE)
