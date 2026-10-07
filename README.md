# schwung-vavra

Waldorf microQ for Schwung/Move, on gearmulator's `mqLib`.

**Status: parked, 2026-09-07.** It works -- boots, plays, 386 of the microQ's
449 parameters across Play/Edit/Multi, per-part multitimbral editing, all
verified on hardware. It is parked because the instrument is not enjoyable on
this device, which the measurements agree with: its factory patches sit at
-27 to -35 dBFS rms where a Schwung module wants about -20, it takes 13-20 s
to boot, it costs ~1.5 cores, and its depth needs 41 pages of knob grid.

Everything here is resumable: the UI is generated from gearmulator's own
parameter descriptions by `tools/gen_ui.py`, so regenerating after an upstream
change is one command, and `tests/audio_battery.sh` still scores the audio
against an offline render of the same engine.

One thing was left unfinished: changing an FX type still stalls the screen.
That is a HOST cost, not this module's -- `planPages` fingerprints the whole
contract on every re-plan (`JSON.stringify` plus a per-character hash), and a
re-plan follows every detent of a gating knob. At 94 KB this module is the
largest contract in the fleet, so it hurts here first, but minijv (83 KB) and
surge (64 KB) pay it too. See the note in docs/CONTRACT_SIZE.md.

## Level

**Gain 100 is the engine's own level**, and the range runs to 400 because the
instrument is quiet. Measured against an offline render of the same engine, at
unity the factory patches sit at **-27 to -35 dBFS rms** -- against the ~-20
dBFS that is healthy for a Schwung module -- and only the loudest, A1, reaches
it. Above 100 the int16 conversion clamps, so a boost clips rather than wraps.

The default used to be 70, throwing away another 3.1 dB for no reason, and
**the battery could not see it**: `feature()` normalises level away so that an
identity test is not secretly a gain test. It now checks level separately, by
PEAK against the engine -- the emulator is not reproducible, so which second is
loudest moves, but the peak of a held note does not.

## Verifying the audio

`tests/audio_battery.sh` captures what `render_block` actually hands the host
and scores it against an offline render of the same engine driven directly --
no fork, no ring, no gain. Every capture must match its OWN reference and beat
every other by 2x. Current result: 5/5 presets in Single, 4/4 parts in Multi,
plus part-volume and layering.

| check | self | nearest other |
|---|---|---|
| Single, five presets | 1.26 - 2.92 | 15.5 - 33.2 |
| Multi, four part/channel pairs | 2.92 - 4.31 | 12.1 - 33.3 |
| part volume 0 | 0.05% of its audible level | |
| two parts on one channel | +63% rms, both sounds present | |

**Identity is spectral, not sample-exact, and that is forced.** `mqLib` runs the
68k in `m_ucThread` and the DSP in a `DSPThread`, so the emulator is not
reproducible against itself: two renders of one preset differ by 1.4-5.0 on this
metric. Any comparison needs that same-preset control, or it is measuring
thread scheduling.

Two ways this test lied before it was right, both worth keeping in mind:

- **Compare the same material.** Scoring the "loudest 1 s" of a 10 s capture
  against the "loudest 1 s" of a 3 s render compares different phases of an
  evolving patch and calls them different sounds -- 2 of 5 presets failed that
  way. Both files start at note-on, so a FIXED window at the same offset is the
  fair comparison.
- **A retriggering capture is not a held one.** Note-off/on clicks every second
  spread broadband low-frequency energy, which swamps the quiet bands of a
  spectrally clean patch: the init sound then matched the wrong reference by
  30+ dB in bands where its own reference sits at -75 dB.

## Multi mode

**The part is chosen from a list, not a knob.** As a knob it was an enum
sitting on the same page as the controls it silently re-pointed -- you could
not see which part you were editing without reading the cell you had just
turned. `Parts` is its own level so it can be titled (a mode's entry level
cannot name its own list page), it writes nothing until a row is clicked, and
each row carries the part's channel and the sound it holds:

```
1 ch1 A1 LosAngeles2019 T
2 ch2 B1 Jazz Percssn WMF
3 ch3 A3 Chor 2.0 S
```

**Selecting a part points the whole editor at it.** A Single parameter change
carries a part byte, so with Part 2 selected, `amp_volume=0` silences part 2
and leaves part 1 playing -- measured: channel 1 at peak 14000, channel 2 at 4.

**Two parts holding the SAME sound share its edit buffer**, so editing one
edits both. That is the instrument, not the module: the first run of this test
gave both parts the same preset and both went silent, which looked like the
part byte being ignored.


The firmware boots in **SINGLE mode on omni**: one sound, every channel, which
is why a slot's forward channel appears to do nothing. `mode` switches it to
Multi, where the 16 parts each have their own sound, MIDI channel and volume --
part n on channel n by default. Point several parts at the SAME channel to
layer them, or use the key ranges for a split.

Verified by playing one channel at a time:

| | ch1 | ch2 | ch3 | ch4 |
|---|---|---|---|---|
| Single | rms 3679 | 3479 | 3410 | 3484 |
| Multi | 3696 | 480 | 684 | 666 |

`part` selects which part the `part_*` keys address; it writes nothing to the
firmware. In Multi mode `preset` sets that part's sound.

**A part is assigned by sending the WHOLE Multi, not by writing its fields.**
`MultiParameterChange` on `Inst<n>SoundBank`/`SoundNumber` updates the Multi's
data -- reading it back confirms the bytes land exactly where intended -- and
the parts still do not load those sounds: measured, MIDI channel 1 then played
instrument 4's patch. mqLib's own `createInitState` does not use parameter
changes either. So the module fetches the Multi at boot, edits it, and sends it
back as one dump, **debounced 300 ms** -- sending it in the same block as the
Single/Multi mode change leaves every part edit with no effect.

Two more things that bite in a chain slot: a slot forwards ONE MIDI channel, so
with the default layout (part n on channel n) only part 1 sounds -- set several
parts to the same `part_channel` to layer them. And `preset_name` reports the
SELECTED name from the table rather than the front panel, because in Multi the
panel's second row shows the MULTI's name ("From TUS with <3") and freezes
while the sound changes, which reads as "presets do nothing".

## The editor

394 parameters across 31 levels -- oscillators, mixer, both filters, amplifier,
four envelopes, three LFOs, both modulation matrices, modifiers, arpeggiator,
both effects units, and the wrapper's settings. Generated by
`tools/gen_ui.py` from gearmulator's own parameter descriptions into
`src/dsp/vavra_ui.h`; only the level tables in that script are authored.

Design rules it follows, each from `docs/PARAM_PAGES.md` or the fleet:

- one level per block, sized to whole pages of eight knobs;
- an envelope's A/D/S/R in cells 1-4, so the envelope graphic gets its row --
  split across the row break it is not drawn at all;
- cutoff and resonance adjacent;
- `short_name` on 154 params, because the renderer's squeeze pass devowels an
  unknown word (`Rotation` drew `ROTATN`);
- knobs read musically: an octave is -4..4, a bipolar amount -64..63, converted
  to the wire's 0..127 per parameter;
- each LFO declares nine keys and shows eight -- Speed and Sync Speed are gated
  on Clocked; each FX unit shows only the selected effect's controls.

**A modulation slot is never cut in half.** A slot is three parameters and a page
holds eight, so four slots in one level page as 8 + 4 -- which puts slot 3's
Source and Dest at the end of one page and its Amount at the start of the next.
Each matrix is two levels of two slots, six knobs, one page each.

**The parameter lookup is a binary search**, not a scan. `get_param` IS the SPI
audio callback, and a page repaint asks for eight values: 278 `strcmp`s each
made ~2,200 string compares per repaint. `tests/runtime_test.cpp` pins the
table's ordering, because an out-of-order entry does not fail loudly -- it
makes a key unfindable and that knob quietly stops working.

Seven assertions run at generation time, because none of these fail visibly on
the device: a key that does not exist, a parameter on no page, a page listing
an undeclared key, non-ASCII text, two cells drawing the same abbreviation, and
a split envelope. They caught seven bad keys and twelve duplicate labels on
their first run.

**386 of the microQ's 449 parameters, in 49% of what the host allows** -- the
contract ceiling went from 64 KB to 128 KB in schwung #444, which this module
is what motivated. At 70,708 bytes it is 108% of the old ceiling, so it
**requires a host with that change**; an older one rejects it outright. See
`docs/CONTRACT_SIZE.md`.

The arpeggiator's 16-step user pattern is a child level -- one instance picker
plus one grid, rather than the ten pages five flat levels would have cost --
and the filter and envelope graphics are declared rather than left to a
detector to infer.

## Presets

**Page 00 is the factory preset browser**, not a knob. `preset` is an index and
the level declares `list_param` / `count_param` / `name_param`, the shape
osirus, surge and minijv all use.

It was a 300-option enum, which was wrong twice over: the 300 names rode inside
`chain_params` on every contract read (6,233 bytes), and selection became a knob
with roughly 1,200 detents of travel. The browser reads three scalars -- count,
index, name -- **one per tick**, so 300 entries cost what three would, and it is
a *door*: paging past it selects nothing, and only a click enters. Once inside,
the jog auditions on every detent.

The module serves all three; only `preset` is declared in `chain_params` (as an
int), which is what the fleet does -- `preset_count` and `preset_name` are read
straight through `get_param`.

`preset` is one enum of 300 options, `A1 LosAngeles2019 T` .. `C100 Init Sound
2.0`, in Program Change order. Three things about it are not obvious:

- **The names are not in the ROM.** Scanning all 524288 bytes for a 16-char
  name table at any plausible stride finds nothing. The only authority is the
  running firmware, which streams its 2x20 front panel as `SysexCommand::EmuLCD`
  -- so `tools/dump_presets.cpp` boots the device once and reads the names off
  the screen, one Program Change at a time, exactly as a person would.
- **Bank select is CC 32, not CC 0.** CC 0 is accepted and ignored: a full dump
  taken with it came back reading `A001-A100` for all three banks, which looks
  like a correct dump of a machine that only has one bank.
- That dump is committed as `docs/presets-os223.tsv` and compiled in by
  `tools/gen_presets_header.py`, so browsing costs no round trip and nothing
  reads a file on the audio callback. `tests/test_presets_generated.sh`
  compares the dump, the header and `module.json`'s options by content --
  a stale copy shows the wrong name on a preset that still loads, which is
  invisible at runtime. The *current* name (`preset_name`) is still read live
  off the LCD, so a different ROM cannot make the module lie.

**The module selects A1 at boot.** Left alone the firmware comes up on A17
`11KHz Solo`, which is arbitrary and sounds like a kick with reverb -- for a
while that was the only sound this port could make.

## Why the microQ and not one of the others

Measured on a Pi 4B @ 1.8 GHz (an A72, like Move's CM4), JIT, four voices, CPU
seconds per second of audio, boot excluded:

| synth | DSPs | CPU-s / audio-s | vs Virus A |
|---|---|---|---|
| Virus A (Osirus, already ships on Move) | 1 | 0.580 | 1.00x |
| **microQ (this)** | **1** | **0.813** | **1.40x** |
| Microwave II | 1 | 0.944 | 1.63x |
| Microwave II + voice expansion | 3 | 4.111 | 7.09x |
| Virus TI2 (OsTIrus) | 2 | 2.435 | 4.20x |
| Nord Lead 2x | 2 | 2.763 | 4.76x |

The microQ is the only DSP56300 synth in the tree besides the Virus A that fits
on Move: one DSP, roughly one core, and it answers `canModifyDspClock()` so it
has the same polyphony-for-CPU dial Osirus already uses. Everything below it in
that table wants two cores or more, which Move does not have spare -- Ableton
pins itself to cores 0-2.

**Rank by CPU-seconds, not by real-time factor.** By wall-clock RT the microQ
(1.88x) looks competitive with the Virus A (2.22x); it gets there by spending
nearly two cores. And boot must be excluded or it reads four times worse still:
the microQ burns ~16 CPU-seconds booting before it renders a sample.

## Measured on Move

`tests/module_smoke.cpp`, 3446 blocks (10 s) of held-and-retriggered chord per
run, two runs a cell, underruns counted only inside the note window:

| dsp_clock | 4 voices | 8 voices | Multi, 2 parts, 4 / 6 / 8 total |
|---|---|---|---|
| 100% | 0, 2 | **137, 141, 142, 159** | -- |
| 75% | 0, 0, 0 | 0, 0, 0 | **15 / 227 / 272** |
| 50% (default) | 0, 0, 0 | 0, 0, 0 | **0 / 0 / 0** |

**The budget is total sounding voices, not parts.** Multi mode itself costs
about 21% (13.9 -> 16.9 CPU-s for the same four voices); after that a voice
costs the same wherever it lives. 75% is clean single-timbrally and breaks in
Multi past four voices, so the default is 50%.

**50% does not change the sound.** Rendered offline at 50, 75 and 100 and
compared, the differences sit inside the emulator's own run-to-run variation:

| comparison | rel. diff | correlation | spectral |
|---|---|---|---|
| same clock, two runs | 0.280 | 0.9632 | 3.39 dB |
| 75% vs 100% | 0.096 | 0.9966 | 4.33 dB |
| 50% vs 100% | 0.275 | 0.9642 | 2.84 dB |

That non-determinism is structural: `mqLib` runs the 68k in `m_ucThread` and
the DSP in a `DSPThread`, so their interleaving moves the output more than the
clock does. **No A/B of this emulator can be bit-exact**, upstream harness
included -- a comparison without that same-clock control measures nothing.

**Voices cannot be split across cores.** The microQ has one DSP56300 and its
voices are one instruction stream. (The emulator is already multi-threaded --
that uc thread and DSP thread are why it costs ~1.3 CPU-seconds per second of
audio while staying real time.)

100% cannot sustain eight voices on Move -- 4% of blocks drop -- so the default
is 75%: clean at four voices, and 0.3% at eight. 50% was clean in every run.
The microQ's polyphony is DSP-bound, so this dial buys voices rather than
quality, which is why it exists and why Osirus does not default to 100 either.
**Anything measured at eight voices needs repeats**: MoveOriginal's own load
swings between 50% and 60% of a core and moves this cell with it. The 75%/8
cell read 0-10 before the queue boost below and 0, 0, 0 after.

**A placeholder is not a default.** The harness sets `dsp_clock` from argv, and
`atoi("")` is 0, which the module clamps to its MINIMUM -- so passing an empty
string to mean "leave it alone" quietly measured 25% and reported it as the
default. Pass `-`.

**A preset change brings its own cold JIT.** The boot warmup can only compile
the patch that is loaded, so the first notes on a newly selected sound stalled
5-8 blocks -- at every DSP clock, which is what rules out capacity. The child
runs the queue four times deep for 4 s after a change, paid for out of the
~20% steady-state headroom, and latency returns to the user's `buffer_ms`
once the JIT is quiet. Measured 0 underruns on four presets, two runs each. Boot is **12.6-20.6 s** wall, during which
`create_instance` has already returned (in ~0.5 ms) and the module renders
silence. Cost is ~1.3 CPU-seconds per second of audio; worst-case
`render_block` on the host callback is 26-77 us.

**Run the harness as root or it measures itself.** It only pins to core 3 at
FIFO 70 -- what the real SPI callback is -- when it can. Run as `ableton` it is
SCHED_OTHER, competing with the emulator's own FIFO 20 threads on cores 0-2; it
then gets descheduled and *bursts* its catch-up `render_block` calls, draining
the queue faster than real time. That reported 13-65 underruns a run, with a
spread so wide (0-52 within one configuration) that a 2x2 scheduler/buffer
matrix looked like pure noise -- because it was measuring the instrument. The
same builds measured correctly report 0-2.

## Two things that will bite

**Boot is long: 12.6-20.6 s.** ~16 CPU-s against the Virus A's 3.0. The host
never blocks on it -- `create_instance` returns immediately and the child boots
on its own thread -- but it is load latency the user waits through.

**ROM images are commonly byte-swapped.** `mqLib::ROM::verifyRom()` requires the
image to begin with the ASCII `2.23`; the widely circulated `microQ223.BIN`
begins `2e 32 33 32`, i.e. the same bytes swapped within each 16-bit word. Swap
pairwise and it boots. The loader reports only "no ROM found", which is a long
way from "your dump has the wrong endianness".

## Layout

Modelled on `schwung-jp8000`: `scripts/build.sh` cross-compiles for ARM64 in
Docker, `scripts/install.sh` deploys to `move.local`, `libs/gearmulator` is a
submodule pinned to the commit these measurements were taken with.

## Next

1. Play it through the Schwung chain on hardware. Everything above was measured
   by a standalone harness against the same `dsp.so` the host loads; the module
   has never been driven by the chain host itself.
2. ~~State save/restore~~ -- wired up, **unverified on hardware**. `get_param("state")`
   returns one JSON object (module settings plus hex dumps of the Single edit
   buffer and the Multi, ~1.7 KB) and `set_param("state", json)` queues it for
   the child, which applies it after boot: mode first, then the Multi through
   the usual 300 ms debounce, then the Single edit buffer. Limits: in Multi mode
   the per-part PARAMETER edits are not captured (each part's sound comes back
   from the Multi's bank/number, i.e. as a factory patch); and the key name
   `state` is the Osirus convention -- confirm it against the host.

3. Editable patch parameters and a Remote UI. Today the module publishes
   preset, mode, part, part channel, part volume, gain, DSP clock and buffer.
   The front panel arrives over sysex already (`EmuLCD`, `EmuLEDs`,
   `EmuButtons`), which is a route to a real editor.
4. More of the Multi: key ranges and transpose per part are one `ParamWrite`
   each -- the mechanism is in place, they just need keys.
