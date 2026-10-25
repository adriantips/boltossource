#!/usr/bin/env python3
"""Cut BoltOS TV's background films out of their 4K originals.

    mkmedia.py SRC_DIR [OUT_DIR]      (default OUT_DIR: tv/media)

SRC_DIR holds the originals, downloaded from Pexels under the Pexels licence
(free to use and modify, no attribution required -- they are credited anyway,
on screen and below). They are 3840x2160 H.264, which nothing in this kernel
can decode, and 150 to 640 MB each, which nothing in this repository should
carry. What comes out is what the kernel plays:

  NAME.m1v   MPEG-1 video elementary stream, 1920x1080, 24 fps, ~6 Mbit/s.
             MPEG-1 because a decoder for it is a few thousand lines
             (vendor/pl_mpeg) and a decoder for H.264 is a project. 1080p
             because that is what the TV shell composites at; a 4K panel gets
             it doubled, as every television's own menus are.
  NAME.jpg   480x270 still of the first frame, for the channel guide.

Every film is a seamless loop. The last LOOP_FADE seconds of the window are
cross-faded into its first LOOP_FADE seconds and those are then cut off the
front, so the final frame leads straight back into the first. A television
that is left on for an evening shows the same ten seconds a great many times,
and a jump cut every ten seconds is what you would notice first.

Re-run after changing the table; the outputs are committed, the originals are
not. To rebuild from scratch, download the 3840x2160 file of each Pexels id in
the table (pexels.com/video/ID) into SRC_DIR as NAME.mp4.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# name, original, start (s), window (s), quantiser, Pexels id, creator.
# The quantiser is per film because the cost of a frame is: a sunset over
# water is nearly free and an autumn forest seen from above is all edges.
FILMS = [
    ("lofoten", "lofoten.mp4",  1.0, 10.5, 8,  40303327, "Clément Proust"),
    ("lagoon",  "lagoon.mp4",   2.0, 10.5, 11, 29525837, "Kenan Turguç"),
    ("sunset",  "sunset.mp4",  12.0, 10.5, 12, 34228902, "Oskar Gross"),
    ("aurora",  "aurora.mp4",   2.0, 10.5, 7,  28492331, "T Honkamies"),
    ("kauai",   "kauai.mp4",    6.0, 10.5, 9,  28616126, "Logan Voss"),
    ("autumn",  "autumn.mp4",   0.1, 10.5, 12, 29143105, "AP Vibes"),
]

LOOP_FADE = 1.5
FPS = 24


def tool(name):
    """ffmpeg from the build toolchain if it is there, else from PATH."""
    local = os.path.join(ROOT, "build", ".tools", "env", "bin", name)
    return local if os.path.exists(local) else name


def run(args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        sys.exit("ffmpeg failed: " + " ".join(args[:6]) + " ...")


def cut(src, out_dir, name, path, start, window, q):
    ff = tool("ffmpeg")
    m1v = os.path.join(out_dir, name + ".m1v")
    jpg = os.path.join(out_dir, name + ".jpg")
    body = window - LOOP_FADE              # how long the loop runs for
    graph = (
        f"[0:v]fps={FPS},scale=1920:1080:flags=lanczos,split[a][b];"
        f"[a]trim=start={LOOP_FADE},setpts=PTS-STARTPTS[main];"
        f"[b]trim=0:{LOOP_FADE},setpts=PTS-STARTPTS[head];"
        f"[main][head]xfade=transition=fade:duration={LOOP_FADE}:"
        f"offset={body - LOOP_FADE},format=yuv420p[v]"
    )
    run([ff, "-v", "error", "-y", "-ss", str(start), "-t", str(window),
         "-i", os.path.join(src, path), "-filter_complex", graph,
         "-map", "[v]", "-an", "-c:v", "mpeg1video", "-q:v", str(q),
         "-bf", "2", "-g", str(FPS), "-mbd", "rd", "-trellis", "2",
         "-f", "mpeg1video", m1v])
    run([ff, "-v", "error", "-y", "-i", m1v, "-frames:v", "1",
         "-vf", "scale=480:270:flags=lanczos", "-q:v", "3", jpg])
    return os.path.getsize(m1v), os.path.getsize(jpg)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "tv", "media")
    os.makedirs(out_dir, exist_ok=True)
    total = 0
    for name, path, start, window, q, pid, who in FILMS:
        v, j = cut(src, out_dir, name, path, start, window, q)
        total += v + j
        print(f"  {name:10s} {v / 1e6:5.1f} MB  {window - LOOP_FADE:4.1f} s loop"
              f"  pexels.com/video/{pid}  ({who})")
    print(f"  total      {total / 1e6:5.1f} MB")


if __name__ == "__main__":
    main()
