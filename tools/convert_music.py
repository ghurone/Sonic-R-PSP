"""Converts Sonic R's music (the PC CD's audio tracks 2-21, ripped to WAV) to ATRAC3 (.at3) for the PSP port.

The PSP decodes ATRAC3 on its Media Engine (sceAtrac), so the main CPU is left to the game and the tracks stream
from the Memory Stick instead of sitting in RAM. The engine looks for MUSIC/track<n>.at3 in the data folder, the
same names it uses for the other formats (track2 ... track21, by CD track number).

The CD's audio is already 44.1kHz 16-bit stereo, what atracdenc takes, so no ffmpeg pass is needed. Needs atracdenc
(https://github.com/dcherednik/atracdenc, Windows build 0.2.3; by default tools/atracdenc/atracdenc.exe, else the PATH's);
its raw output is used as is - rewriting its RIFF header made the PSP's decoder reject the files - so loop points
live in the engine, not in the .at3. It runs in a temp folder, away from the non-ASCII project path.

usage: python tools/convert_music.py [wav folder] [output MUSIC folder] [--atracdenc atracdenc.exe] [--jobs n]
"""
import argparse
import os
import shutil
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TRACKS = range(2, 22)


def convert(n, wav_dir, out_dir, atracdenc):
    name = 'track%d' % n
    target = os.path.join(out_dir, name + '.at3')
    if os.path.exists(target):
        return name, 'kept'
    with tempfile.TemporaryDirectory() as tmp:
        wav, at3 = os.path.join(tmp, 'in.wav'), os.path.join(tmp, 'out.at3')
        shutil.copyfile(os.path.join(wav_dir, name + '.wav'), wav)
        subprocess.run([atracdenc, '-e', 'atrac3', '-i', wav, '-o', at3], check=True, stdout=subprocess.DEVNULL)
        shutil.move(at3, target)
    return name, 'converted'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('wavs', nargs='?', default=os.path.join(ROOT, 'sonic-r-cd-rip', 'MUSIC'))
    ap.add_argument('out', nargs='?', default=os.path.join(ROOT, 'build', 'MUSIC'))
    ap.add_argument('--atracdenc', default=None)
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    bundled = os.path.join(HERE, 'atracdenc', 'atracdenc.exe')
    atracdenc = a.atracdenc or (bundled if os.path.exists(bundled) else shutil.which('atracdenc'))
    if not atracdenc or not os.path.exists(atracdenc):
        raise SystemExit('atracdenc not found (put it in tools/atracdenc/, on the PATH, or give --atracdenc)')
    atracdenc = os.path.abspath(atracdenc)
    missing = [n for n in TRACKS if not os.path.exists(os.path.join(a.wavs, 'track%d.wav' % n))]
    if missing:
        raise SystemExit('missing in %s: %s' % (a.wavs, ', '.join('track%d.wav' % n for n in missing)))
    os.makedirs(a.out, exist_ok=True)

    done = 0
    with ThreadPoolExecutor(a.jobs) as pool:
        for name, status in pool.map(lambda n: convert(n, a.wavs, a.out, atracdenc), TRACKS):
            done += 1
            print(f'[{done}/{len(TRACKS)}] {name}: {status}', flush=True)


if __name__ == '__main__':
    main()
