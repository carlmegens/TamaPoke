#!/usr/bin/env python3
"""Dutch speech clips for the spoken UI (speech.cpp).

    python tools/gen_voice.py <voice.onnx>

Needs piper-tts and numpy. Writes tools/sdcard/voice/{index.bin,vNN.bin} and
web/voice-nl.pak (load it like a sprite pack: same PUT protocol, ends up in /voice).

What gets a clip:
  - every Dutch UI string, split at its %-placeholders (i18n.cpp, NL rows)
  - every Pokemon, move and type name, and short capitalised UI literals in the code
  - the numbers 0..1100
  - every single word of all of the above, so text that only partly matches
    still gets read word by word
The board looks phrases up by an FNV-1a hash of the normalised words, greedy
longest match first, so speechTokens() in speech.cpp and tokens() here must agree.
"""
import glob, hashlib, os, re, struct, sys
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'tools', 'sdcard', 'voice')
CACHE = os.path.join(ROOT, 'tools', 'voice_cache')
CHUNK = 3_900_000          # PUT accepts files up to 4 MB
MAX_TOK = 8                # longest phrase, in words
RATE = 16000

# How an on-screen word is said, where the text abbreviates.
SPOKEN = {
    'lv': 'level', 'kra': 'kracht', 'aanv': 'aanval', 'ver': 'verdediging', 'sne': 'snelheid', 'snlhd': 'snelheid', 'gew': 'gewicht',
    'ene': 'energie', 'rec': 'record', 'min': 'minuten', 'vol': 'volume', 'snd': 'geluid',
    'h': 'uur', 'hp': 'HP', 'iv': 'IV', 'xp': 'XP', 'sd': 'SD', 'lan': 'LAN', 'ok': 'oké',
    'pokedex': 'pokédex', 'pokemon': 'pokémon', 'dex': 'dex', 'nv': 'level',
}


def cls(c):
    if c.isascii() and c.isalpha():
        return 1
    if c.isascii() and c.isdigit():
        return 2
    return 0


def tokens(text):
    """Mirror of speechTokens() in speech.cpp."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = cls(text[i])
        if not c:
            num_before = i > 0 and cls(text[i - 1]) == 2
            num_after = i + 1 < n and cls(text[i + 1]) == 2
            if text[i] == '/' and num_before and num_after:
                out.append('van')
            elif text[i] == '+' and num_after:
                out.append('plus')
            i += 1
            continue
        j = i
        while j < n and cls(text[j]) == c:
            j += 1
        w = text[i:j]
        if c == 2:
            w = w.lstrip('0') or '0'
        out.append(w.lower()[:31])
        i = j
    return out[:24]


def fnv(s):
    h = 2166136261
    for b in s.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def spoken(piece):
    """What piper is asked to say for a piece of on-screen text."""
    def word(m):
        w = m.group(0).lower()
        return SPOKEN.get(w, w)
    s = re.sub(r'[A-Za-z]+', word, piece)
    s = re.sub(r'(?<=\d)/(?=\d)', ' van ', s)
    s = re.sub(r'\+(?=\d)', 'plus ', s)
    s = re.sub(r'[_*>=<+\-]+', ' ', s)
    return re.sub(r'\s+', ' ', s).strip()


def lits(src):
    return re.findall(r'"((?:[^"\\]|\\.)*)"', src)


def nl_rows():
    c = open(os.path.join(ROOT, 'i18n.cpp')).read()
    a = c.index('// ---------------- NL ')
    ui = lits(c[a:c.index('};', a)])
    rest = []
    for name in ('MED_NAME', 'MED_LBL', 'MED_DSC', 'STK_NAME'):
        s = c.index(name + '[LANG_COUNT]')
        block = c[s:c.index('};', s)]
        rows = re.findall(r'\{([^{}]*)\}', block)
        rest += lits(rows[-1])          # NL is the last row
    return ui + rest


def code_literals():
    """Short UI words written straight into the code: region names, OK, BALL, trainer names..."""
    found = []
    skip = {'dex_names.h', 'move_names.h', 'backs.h', 'badges.h', 'music.h', 'avatars.h', 'i18n.cpp'}
    for f in glob.glob(os.path.join(ROOT, '*.ino')) + glob.glob(os.path.join(ROOT, '*.cpp')) + \
            glob.glob(os.path.join(ROOT, '*.h')):
        if os.path.basename(f) in skip:
            continue
        for line in open(f, errors='replace'):
            if 'Serial' in line or 'printf(' in line and 'snprintf' not in line:
                continue
            for s in lits(line):
                if '%' in s or '/' in s or '\\' in s or not (2 <= len(s) <= 30):
                    continue
                if not re.search(r'[A-Z]', s) or not re.search(r'[A-Za-z]{2}', s):
                    continue
                found.append(s)
    return found


def names(path, table):
    c = open(os.path.join(ROOT, path)).read()
    s = c.index(table)
    return re.findall(r'\{\s*"([^"]+)"', c[s:c.index('};', s)])


def collect():
    phrases = {}   # key -> spoken text

    def add(piece, say=None):
        t = tokens(piece)
        if not t or len(t) > MAX_TOK:
            return
        k = ' '.join(t)
        if k not in phrases:
            phrases[k] = say or spoken(piece)

    ui = nl_rows()
    for s in ui:
        for piece in re.split(r'%[-0-9.]*l?[sudxc]', s):
            add(piece)
    for s in code_literals():
        add(s)
    for n in names('dex.h', 'DEX_TBL[') + names('moves.h', 'MOVE_TBL['):
        if n not in ('?', '-'):
            add(n, n.title())
    for n in range(0, 1101):
        add(str(n), str(n))
    # single words, for text that only partly matches a phrase
    for k in list(phrases):
        for w in k.split():
            if not w.isdigit() and w not in phrases:
                phrases[w] = SPOKEN.get(w, w)
    return phrases


def resample(x, src, dst):
    n = int(round(len(x) * dst / src))
    X = np.fft.rfft(x)
    keep = n // 2 + 1
    Y = np.zeros(keep, dtype=complex)
    m = min(keep, len(X))
    Y[:m] = X[:m]
    return np.fft.irfft(Y, n) * (n / len(x))


def to_clip(pcm16, rate):
    x = pcm16.astype(np.float64) / 32768.0
    if rate != RATE:
        x = resample(x, rate, RATE)
    peak = np.max(np.abs(x)) if len(x) else 0
    if peak < 1e-4:
        return b''
    on = np.where(np.abs(x) > peak * 0.02)[0]
    pad = int(RATE * 0.015)
    x = x[max(0, on[0] - pad):on[-1] + pad]
    x = x / peak * 0.95
    return np.clip(np.round(x * 127), -127, 127).astype(np.int8).tobytes()


def synth_all(phrases, model):
    from piper import PiperVoice
    try:
        from piper import SynthesisConfig
        cfg = SynthesisConfig(length_scale=1.05)
    except ImportError:
        cfg = None
    voice = PiperVoice.load(model)
    os.makedirs(CACHE, exist_ok=True)
    clips, texts = {}, sorted(set(phrases.values()))
    for i, t in enumerate(texts):
        cp = os.path.join(CACHE, hashlib.sha1(t.encode()).hexdigest()[:16] + '.i8')
        if os.path.exists(cp):
            clips[t] = open(cp, 'rb').read()
            continue
        chunks = list(voice.synthesize(t, syn_config=cfg) if cfg else voice.synthesize(t))
        rate = chunks[0].sample_rate if chunks else 22050
        pcm = np.concatenate([np.frombuffer(c.audio_int16_bytes, dtype=np.int16) for c in chunks]) \
            if chunks else np.zeros(0, np.int16)
        clips[t] = to_clip(pcm, rate)
        open(cp, 'wb').write(clips[t])
        if i % 250 == 0:
            print(f'  {i}/{len(texts)} {t!r}', flush=True)
    return clips


def write(phrases, clips):
    os.makedirs(OUT, exist_ok=True)
    for f in glob.glob(os.path.join(OUT, '*')):
        os.remove(f)
    entries, chunks, cur, where = [], [], bytearray(), {}
    for k in sorted(phrases):
        data = clips[phrases[k]]
        if not data:
            continue
        if data not in where:      # identical clips stored once
            if len(cur) + len(data) > CHUNK:
                chunks.append(bytes(cur)); cur = bytearray()
            where[data] = (len(chunks), len(cur))
            cur += data
        ch, off = where[data]
        entries.append((fnv(k), ch, off, len(data), k))
    chunks.append(bytes(cur))
    entries.sort()
    for a, b in zip(entries, entries[1:]):
        assert a[0] != b[0], f'hash collision: {a[4]!r} / {b[4]!r}'
    files = {}
    idx = b'VIX1' + struct.pack('<IB3x', len(entries), MAX_TOK)
    idx += b''.join(struct.pack('<IHHII', h, ch, 0, off, ln) for h, ch, off, ln, _ in entries)
    files['voice/index.bin'] = idx
    for i, c in enumerate(chunks):
        files[f'voice/v{i:02d}.bin'] = c
    for name, data in files.items():
        open(os.path.join(OUT, os.path.basename(name)), 'wb').write(data)
    # TPAK bundle, same format as the sprite packs (web/index.html parsePak)
    pak = b'TPAK' + struct.pack('<H', len(files))
    for name, data in files.items():
        pak += bytes([len(name)]) + name.encode() + struct.pack('<I', len(data))
    pak += b''.join(files.values())
    open(os.path.join(ROOT, 'web', 'voice-nl.pak'), 'wb').write(pak)
    total = sum(len(c) for c in chunks)
    print(f'{len(entries)} phrases, {len(where)} clips, {total / 1e6:.1f} MB in {len(chunks)} chunks')


if __name__ == '__main__':
    ph = collect()
    print(f'{len(ph)} phrases, {len(set(ph.values()))} texts to synthesise')
    if len(sys.argv) > 1 and sys.argv[1] == '--list':
        for k, v in sorted(ph.items()):
            print(f'{k}\t{v}')
        sys.exit()
    write(ph, synth_all(ph, sys.argv[1]))
