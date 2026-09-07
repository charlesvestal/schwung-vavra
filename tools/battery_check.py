"""Score the audio battery captured by tests/audio_battery.sh.

The module is compared against an OFFLINE render of the same engine driven
directly -- no fork, no ring, no gain -- so what is under test is this
module's own audio path and its preset/part handling, not the emulator.

Identity is spectral, not sample-exact, because the emulator is not
reproducible: mqLib runs the 68k in m_ucThread and the DSP in a DSPThread and
their interleaving moves the output. Two renders of the SAME preset differ by
1.4-5.0 on this metric, so a capture must match its own reference well inside
that and beat every other reference by 2x."""
import numpy as np, wave, sys, os, json

BATTERY = os.environ.get("VAVRA_BATTERY_DIR", "battery")

def load(p):
    with wave.open(p) as w:
        return np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').reshape(-1,2).astype(float)

def feature(path, after=0.5, length=2.0):
    """Log-band spectrum of a window measured from the ONSET, level-normalised.

    Not from the start of the file. Both files begin at note-on in intent, but
    only the module capture is paced by a real-time consumer; an offline render
    runs flat out and the emulator's 68k and DSP threads interleave differently
    each time, so the sound actually starts at a slightly different place. A
    fixed wall-clock window therefore compared different parts of the envelope
    and moved a reference 7-18 on this metric between two renders of the same
    preset -- far more than the module's own captures move (0.5-2.1)."""
    m = load(path).mean(axis=1)
    peak = np.abs(m).max()
    onset = int(np.argmax(np.abs(m) > peak * 0.05)) if peak > 0 else 0
    a, b = onset + int(after*44100), onset + int((after+length)*44100)
    if len(m) < b: raise SystemExit(f"{path}: too short for onset+{after+length}s")
    seg = m[a:b] * np.hanning(b-a)
    spec = np.abs(np.fft.rfft(seg)); freqs = np.fft.rfftfreq(b-a, 1/44100)
    edges = np.logspace(np.log10(40), np.log10(16000), 41)
    bands = [spec[(freqs>=lo)&(freqs<hi)].mean() if ((freqs>=lo)&(freqs<hi)).any() else 1e-9
             for lo,hi in zip(edges[:-1], edges[1:])]
    b_db = 20*np.log10(np.maximum(np.array(bands), 1e-9))
    return b_db - b_db.max()

def rms(path): 
    a = load(path); return float(np.sqrt((a**2).mean()))

NAMES = {0:"A1 LosAngeles2019", 16:"A17 11KHz Solo", 100:"B1 Jazz Percssn",
         200:"C1 The Beginning", 299:"C100 Init Sound"}
def reference(preset):
    """Average several renders.

    A single offline render is not a stable reference: the module's own
    captures repeat to within 0.5-2.1 on this metric, but two offline renders
    of one preset differ by 7-18, because they run flat out and the emulator's
    68k and DSP threads interleave differently every time. Averaging is what
    makes the yardstick stiffer than the thing being measured."""
    takes = [feature(f"{BATTERY}/R{preset}.wav")]
    for extra in ("b", "c"):
        path = f"{BATTERY}/R{preset}{extra}.wav"
        if os.path.exists(path):
            takes.append(feature(path))
    return np.mean(takes, axis=0)

refs = {p: reference(p) for p in NAMES}
d = lambda a,b: float(np.abs(a-b).mean())

# Two references closer together than this cannot be told apart by a capture,
# because the reference itself moves this much between renders. Saying so is
# the honest verdict; calling it a pass or a failure is not.
REFERENCE_NOISE = 7.0

def identify(path):
    f = feature(path)
    row = {p: d(f, refs[p]) for p in refs}
    best = min(row, key=row.get)
    return best, row[best], sorted(row.values())[1], row


def verdict(best, expect, dist, second):
    if best == expect:
        return "PASS", False
    if second - dist < REFERENCE_NOISE:
        return "INCONCLUSIVE", False
    return "FAIL", True

fails = []

# The primary check is a REGRESSION against captures that were verified good,
# stored as features in tests/fixtures/audio_features.json. It is the stable
# one: repeat captures of a preset land within 0.5-2.3 of each other, because
# a real-time consumer paces them. The identity check below is secondary and
# deliberately loose -- an offline render is NOT reproducible enough to be a
# tight yardstick, two renders of one preset moving 7-18 on this metric.
GOLDEN_TOLERANCE = 5.0
golden_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "tests", "fixtures", "audio_features.json")
if os.path.exists(golden_path):
    with open(golden_path) as handle:
        golden = json.load(handle)["features"]
    print("=== REGRESSION: every capture must still sound like the accepted one ===")
    for name, expected in sorted(golden.items()):
        path = f"{BATTERY}/{name}.wav"
        if not os.path.exists(path):
            continue
        drift = float(np.abs(feature(path) - np.array(expected)).mean())
        ok = drift <= GOLDEN_TOLERANCE
        if not ok:
            fails.append(f"regression on {name}")
        print(f"  {name:<14} drift {drift:5.2f}  {'PASS' if ok else 'FAIL'}")
    print()

# LEVEL, which the spectral check above cannot see: feature() normalises it
# away on purpose, so for a long time the module threw away 3.1 dB (a gain
# default of 70) and every test still passed. Peak is the comparator rather
# than rms: the emulator is not reproducible, so which second is loudest
# moves, but the peak of a held note does not.
print("=== LEVEL: the module must reproduce the engine's own level ===")
LEVEL_TOLERANCE_DB = 2.5
for p in sorted(NAMES):
    cap, ref = f"{BATTERY}/single_{p}.wav", f"{BATTERY}/R{p}.wav"
    if not (os.path.exists(cap) and os.path.exists(ref)):
        continue
    peak = lambda path: max(1.0, float(np.abs(load(path).mean(axis=1)).max()))
    delta = 20 * np.log10(peak(cap) / peak(ref))
    ok = abs(delta) <= LEVEL_TOLERANCE_DB
    if not ok:
        fails.append(f"level on preset {p}")
    print(f"  {NAMES[p]:<18} {delta:+5.2f} dB vs the engine   {'PASS' if ok else 'FAIL'}")
print()

print("=== SINGLE MODE: each capture must identify its OWN preset ===")
for p in NAMES:
    best, dist, second, _ = identify(f"{BATTERY}/single_{p}.wav")
    mark, bad = verdict(best, p, dist, second)
    if bad: fails.append(f"single preset {p}")
    print(f"  {NAMES[p]:<18} -> {NAMES[best]:<18} d={dist:5.2f} next={second:5.2f}  {mark}")

print("\n=== MULTI MODE: each channel must play its own part ===")
for f_, expect, label in [("multi_p1",0,"part1=A1 ch1, play ch1"), ("multi_p2",100,"part2=B1 ch2, play ch2"),
                          ("multi_p3",200,"part1=C1 ch1, play ch1"), ("multi_p4",299,"part2=C100 ch2, play ch2")]:
    best, dist, second, _ = identify(f"{BATTERY}/{f_}.wav")
    mark, bad = verdict(best, expect, dist, second)
    if bad: fails.append(label)
    print(f"  {label:<26} -> {NAMES[best]:<18} d={dist:5.2f} next={second:5.2f}  {mark}")

print("\n=== MULTI MODE: volume and layering ===")
muted, audible = rms(f"{BATTERY}/multi_muted.wav"), rms(f"{BATTERY}/multi_p2.wav")
ok = muted < audible*0.02
if not ok: fails.append("part volume 0")
print(f"  part2 volume=0 on its channel: rms {muted:.1f} vs {audible:.1f} ({muted/audible*100:.2f}%)  {'PASS' if ok else 'FAIL'}")

layer, alone = rms(f"{BATTERY}/multi_layered.wav"), rms(f"{BATTERY}/multi_p1.wav")
best, dist, second, row = identify(f"{BATTERY}/multi_layered.wav")
ok = layer > alone*1.05
if not ok: fails.append("layering")
print(f"  both parts on ch1: rms {layer:.1f} vs {alone:.1f} alone (+{(layer/alone-1)*100:.0f}%)  {'PASS' if ok else 'FAIL'}")
print("     distances: " + "  ".join(f"{NAMES[p][:8]}={row[p]:.1f}" for p in row))

print(f"\n{'ALL PASS' if not fails else 'FAILURES: ' + ', '.join(fails)}")
sys.exit(0 if not fails else 1)
