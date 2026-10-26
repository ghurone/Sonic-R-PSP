"""Assembles the Memory Stick folder of the PSP port: PSP/GAME/SonicR/ with the EBOOT, the game data and the music.

  - EBOOT.PBP                  build/EBOOT.PBP (build it first: make)
  - AI BIN CITY ... SOUND      the game's data folders, from your own Sonic R PC CD (default: sonic-r-cd-rip/)
  - MUSIC/track<N>.at3         the CD's audio tracks as ATRAC3 (tools/convert_music.py, default: build/MUSIC/)
  - SOUND/SFX/REPLAY<N><A-C>   the announcer's replay lines as the game plays them, cut from the CD's REPLAY<N>.WAV
  - SAVE/ GHOST/               the CD's empty folders (GHOST/NORMAL|REVERSE/<track>/, a README.TXT in each): the game
                               writes the saves and time-attack ghosts into them and never creates them itself
  - PATCHES below              small fixes to the CD's data, made in the copy (Egg Robo's gun texture)

The data folders and the replay lines go into one file, SONICR.PAK, which the game reads its data from: opening a file on
the Memory Stick costs ~22 ms, and a race opened 27. (--loose: as folders instead, as before; the game reads those
too, and any file the pack hasn't.) The music, the saves and the ghosts stay files of their own.

Files already there with the same size are left alone, so running it again only copies what changed. Nothing already
in SAVE/ or GHOST/ is ever replaced (your saves and ghosts).

usage: python tools/make_ms_folder.py [output folder] [--data DIR] [--music DIR] [--eboot FILE] [--loose]
       (output default: dist/PSP/GAME/SonicR; copy dist/PSP to the root of the Memory Stick)
"""
import argparse
import os
import re
import shutil
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DATA_DIRS = ['AI', 'BIN', 'CITY', 'EMERALD', 'FACTORY', 'GENERAL', 'ISLAND', 'RUIN', 'SOUND']
REPLAY_HEADER = os.path.join(ROOT, 'src', 'sdl', 'sound', 'replay_voice.h')

# Fixes to the CD's data, made in the copy: (file, [(offset, byte on the CD, fixed byte)]). A byte is only written
# where it still holds the CD's value, so a run over an already fixed copy changes nothing.
PATCHES = [
    # Egg Robo's gun showed part of Tails's eye: one face's UVs (InvisibleUp's EggRobo Texture Fix, from
    # CheatFreak's SRUpdater)
    (os.path.join('BIN', 'OBJECTS', 'MROBOT', 'MROBOTZ.BIN'),
     [(8876, 0x7F, 0x5F), (8878, 0x7F, 0x5F), (8898, 0x7F, 0x5F), (8900, 0x7F, 0x5F)]),
]


def find_ci(base, rel):
    """rel under base, its parts matched whatever their case (the CD's folders are in both); None if not there"""
    path = base
    for part in rel.split(os.sep):
        names = {n.upper(): n for n in os.listdir(path)} if os.path.isdir(path) else {}
        if part.upper() not in names:
            return None
        path = os.path.join(path, names[part.upper()])
    return path


def patch(out):
    n = 0
    for rel, edits in PATCHES:
        path = find_ci(out, rel)
        if path is None:
            print('  (not patched, not there: %s)' % rel)
            continue
        with open(path, 'r+b') as f:
            data = bytearray(f.read())
            changed = False
            for off, was, now in edits:
                if data[off] == was:
                    data[off] = now
                    changed = True
            if changed:
                f.seek(0)
                f.write(data)
                n += 1
    return n


def wav_chunks(path):
    """a WAV file's fmt chunk (as it is) and its samples"""
    with open(path, 'rb') as f:
        d = f.read()
    fmt = pcm = None
    i = 12
    while i + 8 <= len(d):
        cid, size = d[i:i + 4], struct.unpack('<I', d[i + 4:i + 8])[0]
        if cid == b'fmt ':
            fmt = d[i + 8:i + 8 + size]
        elif cid == b'data':
            pcm = d[i + 8:i + 8 + size]
        i += 8 + size + (size & 1)
    return fmt, pcm


def wav_bytes(fmt, pcm):
    body = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(pcm)) + pcm
    return b'RIFF' + struct.pack('<I', len(body)) + body


def replay_pieces(data):
    """The announcer's replay lines as the game plays them (src/sdl/sound/replay_voice.h): each of the CD's
    SOUND/SFX/REPLAY<N>.WAV cut into its pieces, from its start, in the samples the header gives (its silent tail
    left out) — the decompilation's own REPLAY<N><A-C>.WAV, made from the CD. {pack name: WAV bytes}"""
    with open(REPLAY_HEADER, encoding='utf-8') as f:
        rows = re.findall(r'\{ \{ ([^}]*) \}, \{ ([^}]*) \}', f.read())
    out = {}
    for n, (names, counts) in enumerate(rows, 1):
        src = find_ci(data, os.path.join('SOUND', 'SFX', 'REPLAY%d.WAV' % n))
        if src is None:
            continue
        fmt, pcm = wav_chunks(src)
        block = struct.unpack('<H', fmt[12:14])[0]
        at = 0
        for name, count in zip([x.strip().strip('"') for x in names.split(',')], [int(x) for x in counts.split(',')]):
            if name in ('0', '') or count == 0:
                continue
            out['SOUND/SFX/%s.WAV' % name] = wav_bytes(fmt, pcm[at * block:(at + count) * block])
            at += count
    return out


PACK_ALIGN = 512   # (a Memory Stick sector: each file's reads start on one)


def pack_name(rel):
    """the name the game looks a file up by (platform_psp.c's pack_key): upper case, '/' between the parts"""
    return rel.replace(os.sep, '/').upper()


def gather(src, files, skip=()):
    """every file under src, by its pack name"""
    for base, dirs, names in os.walk(src):
        rel = os.path.relpath(base, src)
        if any(rel == s or rel.startswith(s + os.sep) for s in skip):
            continue
        for f in names:
            files[pack_name(os.path.normpath(os.path.join(rel, f)))] = os.path.join(base, f)


def write_pack(path, files):
    """SONICR.PAK: 'SRPK', version 1, the file count and the names' size; for each file (sorted by name, for the
    game's binary search) its name's offset in the names, its data's offset in the pack and its size; the names,
    NUL-ended; then the files' data, each from a PACK_ALIGN boundary. PATCHES made in the data written."""
    names = sorted(files)
    blob = b''.join(n.encode('ascii') + b'\0' for n in names)
    head = 16 + 12 * len(names) + len(blob)
    at = (head + PACK_ALIGN - 1) // PACK_ALIGN * PACK_ALIGN
    edits = {pack_name(rel): e for rel, e in PATCHES}
    entries, datas, name_at = [], [], 0
    for n in names:
        if isinstance(files[n], bytes):
            data = bytearray(files[n])   # (made here: replay_pieces)
        else:
            with open(files[n], 'rb') as f:
                data = bytearray(f.read())
        for off, was, now in edits.get(n, []):
            if data[off] == was:
                data[off] = now
        entries.append(struct.pack('<III', name_at, at, len(data)))
        datas.append((at, data))
        name_at += len(n) + 1
        at = (at + len(data) + PACK_ALIGN - 1) // PACK_ALIGN * PACK_ALIGN
    tmp = path + '.tmp'
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(tmp, 'wb') as f:
        f.write(b'SRPK' + struct.pack('<III', 1, len(names), len(blob)))
        f.write(b''.join(entries))
        f.write(blob)
        for off, data in datas:
            f.write(b'\0' * (off - f.tell()))
            f.write(data)
    os.replace(tmp, path)
    return len(names), at


def copy(src, dst, keep_existing=False):
    if os.path.exists(dst) and (keep_existing or os.path.getsize(dst) == os.path.getsize(src)):
        return 0
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)
    return 1


def copy_tree(src, dst, skip=(), keep_existing=False):
    n = 0
    for base, dirs, files in os.walk(src):
        rel = os.path.relpath(base, src)
        if any(rel == s or rel.startswith(s + os.sep) for s in skip):
            continue
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in files:
            n += copy(os.path.join(base, f), os.path.join(dst, rel, f), keep_existing)
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', nargs='?', default=os.path.join(ROOT, 'dist', 'PSP', 'GAME', 'SonicR'))
    ap.add_argument('--data', default=os.path.join(ROOT, 'sonic-r-cd-rip'))
    ap.add_argument('--music', default=os.path.join(ROOT, 'build', 'MUSIC'))
    ap.add_argument('--eboot', default=os.path.join(ROOT, 'build', 'EBOOT.PBP'))
    ap.add_argument('--loose', action='store_true', help='the data as folders, not SONICR.PAK')
    a = ap.parse_args()

    for d in DATA_DIRS:
        if not os.path.isdir(os.path.join(a.data, d)):
            raise SystemExit('game data folder missing: %s' % os.path.join(a.data, d))
    if not os.path.exists(a.eboot):
        raise SystemExit('no EBOOT at %s (make)' % a.eboot)

    n = copy(a.eboot, os.path.join(a.out, 'EBOOT.PBP'))
    replay = replay_pieces(a.data)
    if a.loose:
        for d in DATA_DIRS:
            n += copy_tree(os.path.join(a.data, d), os.path.join(a.out, d))
        for name, wav in replay.items():
            dst = os.path.join(a.out, *name.split('/'))
            if not os.path.exists(dst) or os.path.getsize(dst) != len(wav):
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, 'wb') as f:
                    f.write(wav)
                n += 1
    else:
        files = {}
        for d in DATA_DIRS:
            sub = {}
            gather(os.path.join(a.data, d), sub)
            files.update({d + '/' + k: v for k, v in sub.items()})
        files.update(replay)
        count, size = write_pack(os.path.join(a.out, 'SONICR.PAK'), files)
        print('SONICR.PAK: %d files, %.1f MB' % (count, size / 1048576.0))
    tracks = 0
    for t in range(2, 22):
        src = os.path.join(a.music, 'track%d.at3' % t)
        if os.path.exists(src):
            n += copy(src, os.path.join(a.out, 'MUSIC', 'track%d.at3' % t))
            tracks += 1
    for d in ('SAVE', 'GHOST'):
        if os.path.isdir(os.path.join(a.data, d)):
            n += copy_tree(os.path.join(a.data, d), os.path.join(a.out, d), keep_existing=True)
        else:
            os.makedirs(os.path.join(a.out, d), exist_ok=True)

    fixed = patch(a.out) if a.loose else len(PATCHES)   # (in the pack: made as it is written)
    print('%s: %d files copied, %d fixed, %d/20 music tracks' % (a.out, n, fixed, tracks))
    if tracks < 20:
        print('  (music missing: python tools/convert_music.py)')


if __name__ == '__main__':
    main()
