#!/usr/bin/env python3
"""BoltOS TV's AAC-LC decoder (tv/aac.c) against ffmpeg's, on the host.

Makes test sounds -- tones, a sweep, noise bursts and clicks, so the encoder
reaches for short blocks, TNS, noise substitution and both stereo tools --
encodes them with ffmpeg's AAC encoder (and Apple's, through afconvert, on a
Mac), decodes each with test/host/aactest.c and with ffmpeg, and prints the
signal-to-noise ratio between the two. Floating-point decoders differ in the
last bit or two, so anything above ~70 dB is the same sound.

    python3 test/host/aaccmp.py [EXTRA.mp4 ...]
"""
import os, shutil, struct, subprocess, sys, tempfile, math

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TOOLS = os.path.join(ROOT, "build", ".tools", "env", "bin")
FFMPEG = os.path.join(TOOLS, "ffmpeg") if os.path.exists(os.path.join(TOOLS, "ffmpeg")) else "ffmpeg"
tmp = tempfile.mkdtemp(prefix="aaccmp")
exe = os.path.join(tmp, "aactest")


def run(*a, **kw):
    return subprocess.run(list(a), check=True, capture_output=True, **kw)


run("cc", "-O2", "-DAAC_SELFTEST", "-I", os.path.join(ROOT, "test", "host", "shim"), "-o", exe,
    os.path.join(ROOT, "test", "host", "aactest.c"), os.path.join(ROOT, "tv", "aac.c"),
    os.path.join(ROOT, "tv", "aactab.c"), os.path.join(ROOT, "mini", "mp4.c"), "-lm")

SRC = os.path.join(tmp, "src.wav")
# Ten seconds: a chord, a sweep, a noise burst, clicks, a mono-ish passage.
expr = ("0.25*sin(2*PI*220*t)+0.15*sin(2*PI*277*t+1)+0.1*sin(2*PI*330*t)"
        "+0.2*sin(2*PI*(200+900*t)*t)*between(t,2,4)"
        "+0.3*(random(0)-0.5)*between(t,4,5.5)"
        "+0.8*lt(mod(t,0.37),0.002)*between(t,5.5,7.5)")
expr_r = expr.replace("220", "221").replace("random(0)", "random(1)")
run(FFMPEG, "-y", "-v", "error", "-f", "lavfi", "-i",
    "aevalsrc=exprs='%s|%s':s=44100:d=10" % (expr, expr_r), SRC)

files = []
for name, args in (("ffmpeg-128k", ["-c:a", "aac", "-b:a", "128k"]),
                   ("ffmpeg-96k", ["-c:a", "aac", "-b:a", "96k"]),
                   ("ffmpeg-48k", ["-c:a", "aac", "-b:a", "48k"]),
                   ("ffmpeg-mono", ["-c:a", "aac", "-b:a", "64k", "-ac", "1"])):
    out = os.path.join(tmp, name + ".mp4")
    run(FFMPEG, "-y", "-v", "error", "-i", SRC, *args, out)
    files.append(out)
if shutil.which("afconvert"):
    m4a = os.path.join(tmp, "apple-128k.m4a")
    run("afconvert", "-f", "m4af", "-d", "aac", "-b", "128000", SRC, m4a)
    files.append(m4a)
files += sys.argv[1:]

worst = 1e9
for f in files:
    ours, ref = os.path.join(tmp, "ours.raw"), os.path.join(tmp, "ref.raw")
    r = subprocess.run([exe, f, ours], capture_output=True, text=True)
    print("%s: %s" % (os.path.basename(f), r.stdout.strip().splitlines()[-1] if r.stdout else r.returncode))
    if r.returncode:
        print(r.stdout)
    # a mono stream on both channels at full level, as tv/aac.c puts it (-ac 2 would mix it 3 dB down)
    mono = "mono" in subprocess.run([FFMPEG, "-hide_banner", "-i", f], capture_output=True, text=True).stderr
    run(FFMPEG, "-y", "-v", "error", "-i", f, "-f", "s16le", *(["-af", "pan=stereo|c0=c0|c1=c0"] if mono else ["-ac", "2"]),
        "-c:a", "pcm_s16le", ref)
    A, B = open(ours, "rb").read(), open(ref, "rb").read()
    n = min(len(A), len(B)) // 2
    a, b = struct.unpack("<%dh" % n, A[:n * 2]), struct.unpack("<%dh" % n, B[:n * 2])
    # ffmpeg drops the encoder's priming samples (an edit list); find the shift.
    best = None
    for shift in (0, 1024, 2048, 2112, 1024 + 1088):
        s = shift * 2
        m = min(len(a) - s, len(b))
        if m <= 0:
            continue
        sig = sum(x * x for x in b[:m:7]) + 1
        err = sum((a[i + s] - b[i]) ** 2 for i in range(0, m, 7)) + 1
        snr = 10 * math.log10(sig / err)
        if best is None or snr > best[0]:
            best = (snr, shift)
    print("   %.1f dB against ffmpeg (ours shifted %d frames)" % best)
    worst = min(worst, best[0])
print("worst %.1f dB" % worst)
sys.exit(0 if worst > 60 else 1)
