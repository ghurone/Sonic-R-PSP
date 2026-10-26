"""Extracts the Sonic R PC CD from a .bin/.cue dump: the data track's files, and the audio tracks (the music) as WAV.

  - <out>/AI BIN CITY ... SONICR.EXE   every file of the data track (track 1, ISO 9660), with its date
  - <out>/MUSIC/track<N>.wav           audio tracks 2-21 as 44.1kHz 16-bit stereo WAV, the pregap left out

What it makes is what tools/convert_music.py (the music) and tools/make_ms_folder.py (the rest) read; <out> defaults
to sonic-r-cd-rip/, where both look. Any dump with a cue sheet will do: one .bin per track (as Redump's) or a single
.bin with every track, the data track in raw 2352-byte sectors or plain 2048-byte ones.

usage: python tools/extract_cd.py "Sonic R (USA).cue" [output folder]
"""
import argparse
import datetime
import os
import re
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RAW_SECTOR = 2352
MUSIC_TRACKS = range(2, 22)


def parse_cue(path):
    """[{'number', 'file', 'mode', 'index': {n: sector in its file}}] in the sheet's order"""
    tracks, current_file = [], None
    with open(path, encoding='latin1') as f:
        for line in f:
            words = line.strip()
            m = re.match(r'FILE\s+"(.*)"\s+\S+$', words) or re.match(r'FILE\s+(\S+)\s+\S+$', words)
            if m:
                current_file = os.path.join(os.path.dirname(path), m.group(1))
                continue
            m = re.match(r'TRACK\s+(\d+)\s+(\S+)$', words)
            if m:
                tracks.append({'number': int(m.group(1)), 'file': current_file, 'mode': m.group(2).upper(), 'index': {}})
                continue
            m = re.match(r'INDEX\s+(\d+)\s+(\d+):(\d+):(\d+)$', words)
            if m and tracks:
                mm, ss, ff = int(m.group(2)), int(m.group(3)), int(m.group(4))
                tracks[-1]['index'][int(m.group(1))] = (mm * 60 + ss) * 75 + ff
    return tracks


class DataTrack:
    """the data track's 2048-byte user sectors, from raw 2352-byte (MODE1/2352) or cooked (MODE1/2048) ones"""

    def __init__(self, track):
        self.f = open(track['file'], 'rb')
        self.sector, self.skip = (2048, 0) if track['mode'].endswith('/2048') else (RAW_SECTOR, 16)
        self.start = track['index'].get(1, 0)

    def read(self, lba, size):
        out = bytearray()
        while len(out) < size:
            self.f.seek((self.start + lba) * self.sector + self.skip)
            out += self.f.read(2048)
            lba += 1
        return bytes(out[:size])


def record_time(rec):
    y, mo, d, h, mi, s, tz = struct.unpack('6Bb', rec[18:25])
    try:
        when = datetime.datetime(1900 + y, mo, d, h, mi, s) - datetime.timedelta(minutes=15 * tz)
    except ValueError:
        return None
    return when.replace(tzinfo=datetime.timezone.utc).timestamp()


def extract_files(disc, lba, size, dest, count):
    raw = disc.read(lba, size)
    i = 0
    while i < len(raw):
        length = raw[i]
        if length == 0:   # (the rest of this sector is padding)
            i = (i // 2048 + 1) * 2048
            continue
        rec = raw[i:i + length]
        i += length
        name = rec[33:33 + rec[32]]
        if name in (b'\x00', b'\x01'):   # (. and ..)
            continue
        name = name.decode('latin1').split(';')[0].rstrip('.')
        at, length = struct.unpack('<I', rec[2:6])[0], struct.unpack('<I', rec[10:14])[0]
        path = os.path.join(dest, name)
        if rec[25] & 2:
            os.makedirs(path, exist_ok=True)
            extract_files(disc, at, length, path, count)
        else:
            with open(path, 'wb') as f:
                f.write(disc.read(at, length))
            count[0] += 1
            count[1] += length
        t = record_time(rec)
        if t:
            os.utime(path, (t, t))


def audio_bytes(tracks, k):
    """track k's audio from its INDEX 01 to where the next track in the same file starts (its pregap included in
    neither), or to the end of its file"""
    t = tracks[k]
    start = t['index'][1] * RAW_SECTOR
    end = None
    if k + 1 < len(tracks) and tracks[k + 1]['file'] == t['file']:
        nxt = tracks[k + 1]['index']
        end = nxt.get(0, nxt.get(1)) * RAW_SECTOR
    with open(t['file'], 'rb') as f:
        f.seek(start)
        return f.read() if end is None else f.read(end - start)


def write_wav(path, pcm):
    head = b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVE'
    head += b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 2, 44100, 44100 * 4, 4, 16)
    head += b'data' + struct.pack('<I', len(pcm))
    with open(path, 'wb') as f:
        f.write(head)
        f.write(pcm)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cue')
    ap.add_argument('out', nargs='?', default=os.path.join(ROOT, 'sonic-r-cd-rip'))
    a = ap.parse_args()

    tracks = parse_cue(a.cue)
    for t in tracks:
        if not os.path.exists(t['file'] or ''):
            raise SystemExit('track %d: file not found: %s' % (t['number'], t['file']))
    data = [t for t in tracks if t['mode'].startswith('MODE1')]
    if not data:
        raise SystemExit('no data track (MODE1) in ' + a.cue)

    disc = DataTrack(data[0])
    pvd = disc.read(16, 2048)
    if pvd[1:6] != b'CD001':
        raise SystemExit('the data track has no ISO 9660 volume')
    root = pvd[156:190]
    os.makedirs(a.out, exist_ok=True)
    count = [0, 0]
    extract_files(disc, struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0], a.out, count)
    print('data: %d files, %.1f MB' % (count[0], count[1] / 1e6))
    if not os.path.exists(os.path.join(a.out, 'SONICR.EXE')):
        print('  (no SONICR.EXE: is this the Sonic R PC CD?)')

    music = os.path.join(a.out, 'MUSIC')
    os.makedirs(music, exist_ok=True)
    by_number = {t['number']: k for k, t in enumerate(tracks) if t['mode'] == 'AUDIO'}
    missing = [n for n in MUSIC_TRACKS if n not in by_number]
    for n in MUSIC_TRACKS:
        if n in by_number:
            pcm = audio_bytes(tracks, by_number[n])
            write_wav(os.path.join(music, 'track%d.wav' % n), pcm)
            secs = len(pcm) / (44100 * 4)
            print('track%d.wav  %d:%05.2f' % (n, secs // 60, secs % 60))
    if missing:
        print('  (audio tracks missing: %s)' % ', '.join(str(n) for n in missing))


if __name__ == '__main__':
    main()
