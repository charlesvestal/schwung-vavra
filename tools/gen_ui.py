#!/usr/bin/env python3
"""Generate src/dsp/vavra_ui.h -- the microQ's chain_params and ui_hierarchy.

Source of truth is gearmulator's parameterDescriptions_mq.json: index, range,
step, discrete/bipolar, and the value list an enum's option text comes from.
Nothing here is hand-typed twice; the level tables below say which parameters
belong on which page, and every key is checked against the descriptions.

Modelled on schwung-jp8000's gen_params.py. The generation-time assertions are
the point of doing it this way: a key that does not exist, a parameter that
reaches no page, two cells on one page drawing the same abbreviation, and an
envelope graphic split across the row break are all invisible at runtime.
"""
import json, pathlib, re, sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from mq_descriptions import load, with_defaults

ROOT = pathlib.Path(__file__).resolve().parent.parent

# --- naming ----------------------------------------------------------------
# JUCE's parameter names are dense CamelCase with a positional prefix; the key
# is snake_case and the page LABEL drops what the page already says.
PREFIX = [
    (r"^O([123])",            r"osc\1_"),
    (r"^F([12])",             r"flt\1_"),
    (r"^Lfo([123])",          r"lfo\1_"),
    (r"^FilterEnv",           "fenv_"),
    (r"^AmpEnv",              "aenv_"),
    (r"^Env([34])",           r"env\1_"),
    (r"^Slot([1-8])F",        r"fmod\1_"),
    (r"^Slot([1-8])S",        r"smod\1_"),
    (r"^Mod([1-4])",          r"modif\1_"),
    (r"^Fx([12])",            r"fx\1_"),
    (r"^FiveFX([12])",        r"fx\1_five_"),
    (r"^Arp",                 "arp_"),
    (r"^Amp",                 "amp_"),
    (r"^Unisono",             "unison_"),
    (r"^Glide",               "glide_"),
    (r"^PitchMod",            "pitchmod_"),
    (r"^Noise",               "noise_"),
    (r"^RingMod",             "ringmod_"),
]

def snake(text):
    text = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", text)
    text = re.sub(r"(?<=[A-Z])(?=[A-Z][a-z])", "_", text)
    return text.lower()

def key_for(name):
    for pattern, replacement in PREFIX:
        if re.match(pattern, name):
            head = re.sub(pattern, replacement, name, count=1)
            prefix, rest = head.split("_", 1) if "_" in head else (head, "")
            # fx1_five_... keeps two segments
            if prefix in ("fx1", "fx2") and rest.startswith("five_"):
                return f"{prefix}_five_{snake(rest[5:])}"
            return f"{prefix}_{snake(rest)}" if rest else snake(head)
    return snake(name)

# Words that must not be devowelled by the renderer's squeeze pass, and page
# labels that repeat what the page title already says.
LABEL_FIX = {
    "Fm Source": "FM Source", "Fm Amount": "FM Amt", "Pwm Source": "PWM Source",
    "Pwm": "PWM", "Pulse Width": "Pulse Width", "Sub Freq Div": "Sub Div",
    "Key Track": "Keytrack", "Env Mod": "Env Amt", "Vel Mod": "Velocity",
    "Cutoff Mod": "Cut Mod", "Mod Source": "Mod Src", "Pan Mod Source": "Pan Src",
    "Bend Range": "Bend", "Attack Level": "Atk Level", "Trigger Mode": "Trigger",
    "Speed Clocked": "Sync Speed", "Start Phase": "Phase", "Same Note Overlap": "Overlap",
    "Pattern Reset": "Pat Reset", "Pattern Length": "Pat Length", "Octave Range": "Octaves",
    "Max Notes": "Max Notes", "T Factor": "Timing", "Velo Mode": "Velocity",
}

SHORT = {
    "octave": "OCT", "semitone": "SEMI", "semi": "SEMI", "detune": "DTUNE",
    "bend": "BEND", "keytrack": "KTRK", "fm_source": "FMSRC", "fm_amt": "FMAMT",
    "shape": "SHAPE", "pulse_width": "PW", "pwm_source": "PWSRC", "pwm": "PWM",
    "sub_div": "SUBDV", "sub_volume": "SUBVL", "level": "LEVEL", "balance": "BAL",
    "cutoff": "CUT", "resonance": "RES", "drive": "DRIVE", "env_amt": "ENV",
    "velocity": "VEL", "mod_src": "MSRC", "cut_mod": "CMOD", "pan": "PAN",
    "pan_src": "PNSRC", "pan_mod": "PNMOD", "attack": "ATK", "decay": "DEC",
    "sustain": "SUS", "release": "REL", "decay2": "DEC2", "sustain2": "SUS2",
    "atk_level": "ATKLV", "trigger": "TRIG", "mode": "MODE", "speed": "SPEED",
    "sync_speed": "SYNSP", "phase": "PHASE", "delay": "DELAY", "fade": "FADE",
    "sync": "SYNC", "clocked": "CLOCK", "volume": "VOL", "amount": "AMT",
    "source": "SRC", "destination": "DEST", "type": "TYPE", "mix": "MIX",
    "feedback": "FDBCK", "depth": "DEPTH", "speed2": "SPD2", "polarity": "POL",
}

def label_for(name, key, level_prefix):
    """Page label: strip the prefix the page already carries."""
    text = name
    for pattern, _ in PREFIX:
        if re.match(pattern, text):
            text = re.sub(pattern, "", text, count=1)
            break
    text = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", text)
    text = re.sub(r"(?<=[A-Z])(?=[A-Z][a-z])", " ", text).strip()
    text = LABEL_FIX.get(text, text)
    return text or name

def short_for(key):
    tail = key.split("_", 1)[1] if "_" in key else key
    return SHORT.get(tail)

# --- the design ------------------------------------------------------------
# One level per functional block, each sized so its knobs fill whole pages of
# eight. Envelope A/D/S/R sit in cells 1-4 so the envelope graphic gets its
# contiguous row; filter cutoff and resonance are adjacent for the same reason.
#
# Deliberate omissions, so the pages stay whole: each envelope's TriggerMode
# and the 5.1/surround delay parameters, which are meaningless on a stereo
# device. Each LFO declares nine keys and shows eight -- Speed and Sync Speed
# are gated on Clocked, so exactly one of them is ever on the page.

def L(level_id, label, keys, knobs=None, extra=None, hidden=None, relabel=None, child=None):
    return dict(id=level_id, label=label, keys=keys, knobs=knobs, extra=extra or [],
                hidden=hidden or {}, relabel=relabel or {}, child=child)

def osc(n, sub=True):
    keys = [f"osc{n}_shape", f"osc{n}_octave", f"osc{n}_semi", f"osc{n}_detune",
            f"osc{n}_pulse_width", f"osc{n}_pwm", f"osc{n}_pwm_source", f"osc{n}_key_track",
            f"osc{n}_bend_range", f"osc{n}_fm_source", f"osc{n}_fm_amount"]
    if sub:
        keys += [f"osc{n}_sub_freq_div", f"osc{n}_sub_volume"]
    return L(f"osc{n}", f"Osc {n}", keys, knobs=keys[:8])

def env(prefix, label):
    keys = [f"{prefix}_attack", f"{prefix}_decay", f"{prefix}_sustain", f"{prefix}_release",
            f"{prefix}_attack_level", f"{prefix}_decay2", f"{prefix}_sustain2", f"{prefix}_mode"]
    return L(prefix, label, keys, knobs=keys)

def lfo(n):
    keys = [f"lfo{n}_shape", f"lfo{n}_speed", f"lfo{n}_speed_clocked", f"lfo{n}_clocked",
            f"lfo{n}_sync", f"lfo{n}_start_phase", f"lfo{n}_delay", f"lfo{n}_fade",
            f"lfo{n}_key_track"]
    # Exactly one speed control is ever visible, so nine keys draw eight cells.
    return L(f"lfo{n}", f"LFO {n}", keys, knobs=keys,
             hidden={f"lfo{n}_speed": (f"lfo{n}_clocked", 0),
                     f"lfo{n}_speed_clocked": (f"lfo{n}_clocked", 1)})

def flt(n):
    keys = [f"flt{n}_type", f"flt{n}_cutoff", f"flt{n}_resonance", f"flt{n}_drive",
            f"flt{n}_env_mod", f"flt{n}_key_track", f"flt{n}_vel_mod", f"flt{n}_pan",
            f"flt{n}_mod_source", f"flt{n}_cutoff_mod", f"flt{n}_fm_source", f"flt{n}_fm_amount",
            f"flt{n}_pan_mod_source", f"flt{n}_pan_mod"]
    return L(f"flt{n}", f"Filter {n}", keys, knobs=keys[:8])

def mods(prefix, label, count):
    """One level per PAIR of slots -- six knobs, one page, no slot cut in half.

    A slot is three parameters, and eight knobs to a page divides badly: four
    slots in one level pages as 8 + 4, which puts slot 3's Source and Dest at
    the end of page one and its Amount at the start of page two. Six keys make
    one page of two whole slots."""
    levels = []
    for first in range(1, count + 1, 2):
        keys = []
        for slot in (first, first + 1):
            if slot > count:
                continue
            keys += [f"{prefix}{slot}_source", f"{prefix}{slot}_destination",
                     f"{prefix}{slot}_amount"]
        span = f"{first}-{first + 1}" if first + 1 <= count else f"{first}"
        levels.append(L(f"{prefix}{first}", f"{label} {span}", keys, knobs=keys))
    return levels

LEVELS = [
    osc(1), osc(2), osc(3, sub=False),
    # Every cell here is a level or a balance, so the SOURCE has to be in the
    # label -- six cells drawing LEVEL is not a page.
    L("mixer", "Mixer",
      ["osc1_level", "osc1_balance", "osc2_level", "osc2_balance",
       "osc3_level", "osc3_balance", "noise_level", "ringmod_level",
       "noise_balance", "ringmod_balance", "noise_mode_f1", "noise_mode_f2"],
      knobs=None,
      relabel={"osc1_level": ("Osc 1 Level", "O1LVL"), "osc1_balance": ("Osc 1 Balance", "O1BAL"),
               "osc2_level": ("Osc 2 Level", "O2LVL"), "osc2_balance": ("Osc 2 Balance", "O2BAL"),
               "osc3_level": ("Osc 3 Level", "O3LVL"), "osc3_balance": ("Osc 3 Balance", "O3BAL"),
               "noise_level": ("Noise Level", "NSLVL"), "noise_balance": ("Noise Balance", "NSBAL"),
               "ringmod_level": ("Ring Mod Level", "RMLVL"),
               "ringmod_balance": ("Ring Mod Balance", "RMBAL"),
               "noise_mode_f1": ("Noise Mode F1", "NMDF1"),
               "noise_mode_f2": ("Noise Mode F2", "NMDF2")}),
    flt(1), flt(2),
    L("amp", "Amplifier", ["amp_volume", "amp_velocity", "amp_mod_source", "amp_mod_amount"]),
    env("fenv", "Filter Env"), env("aenv", "Amp Env"),
    env("env3", "Env 3"), env("env4", "Env 4"),
    lfo(1), lfo(2), lfo(3),
    # All eight slots of each matrix. They cost ~18 KB between them, because
    # every slot repeats its own source and destination option lists verbatim;
    # that did not fit under the old 64 KB contract ceiling and does under the
    # 128 KB one (schwung #444). See docs/CONTRACT_SIZE.md.
    *mods("fmod", "Fast Mod", 8),
    *mods("smod", "Mod Slot", 8),
    # The four envelope trigger modes on one page rather than a ninth cell on
    # each envelope, which would have made four pages holding one knob each.
    L("envtrig", "Env Trigger",
      ["fenv_trigger_mode", "aenv_trigger_mode", "env3_trigger_mode", "env4_trigger_mode"],
      relabel={"fenv_trigger_mode": ("Filter Env", "FILT"),
               "aenv_trigger_mode": ("Amp Env", "AMP"),
               "env3_trigger_mode": ("Env 3", "ENV3"),
               "env4_trigger_mode": ("Env 4", "ENV4")}),
    L("modif", "Modifiers",
      [f"modif{n}_{field}" for n in range(1, 5)
       for field in ("source1", "source2", "operator", "constant")]),
    L("arp", "Arpeggiator",
      ["arp_mode", "arp_pattern", "arp_clock", "arp_length", "arp_octave_range",
       "arp_direction", "arp_sort_order", "arp_velo_mode", "arp_max_notes",
       "arp_t_factor", "arp_same_note_overlap", "arp_pattern_reset",
       "arp_pattern_length", "tempo"]),
    # The arpeggiator's user pattern is 16 steps x 5 attributes. As five flat
    # levels that is ten pages; as a child level it is one Step picker and one
    # grid. The level lists TEMPLATE keys and the host resolves them through
    # child_key_template, so each template carries its metadata inline -- a
    # listed key with no metadata gets a guessed float 0..1 knob.
    L("arpsteps", "Arp Pattern",
      ["step", "glide", "accent", "length", "timing"],
      child=dict(count=16, label="Step", template="arp_{index}_{key}", base=0, digits=2,
                 sample="arp_00_{key}")),
    L("voice", "Voice",
      ["voice_mode", "unison_count", "unison_detune", "sync",
       "glide_mode", "glide_rate", "pitchmod_src", "pitchmod_amount"],
      # Two cells would otherwise both draw MODE.
      relabel={"voice_mode": ("Voice Mode", "VOICE"), "glide_mode": ("Glide Mode", "GLIDE")}),
]

# --- emission --------------------------------------------------------------
# Wrapper-owned parameters: not the synth's, so they are declared here rather
# than derived from the descriptions.
WRAPPER = [
    # An INDEX, not an enum. The 300 names used to ride in chain_params as
    # options -- 6,233 bytes on every contract read, and a knob with 300
    # detents' worth of travel. A preset page reads three scalars (count,
    # index, name) one per tick instead, so the list size costs nothing, and
    # it is a door: paging past it does not audition anything.
    dict(key="preset", name="Preset", type="int", min=0, max=299, default=0),
    dict(key="mode", name="Mode", type="enum", options=["Single", "Multi"], default=0),
    dict(key="part", name="Part", type="int", min=1, max=16, default=1,
         visible_if={"key": "mode", "equals": 1}),
    dict(key="part_channel", name="Part Channel", short_name="PCHAN", type="int", min=0, max=16,
         default=0, visible_if={"key": "mode", "equals": 1}),
    dict(key="part_volume", name="Part Volume", short_name="PVOL", type="int", min=0, max=127,
         default=127, visible_if={"key": "mode", "equals": 1}),
    dict(key="dsp_clock", name="DSP Clock", short_name="CLOCK", type="int", min=25, max=100,
         default=50, unit="%"),
    dict(key="gain", name="Gain", type="int", min=0, max=100, default=70),
    dict(key="buffer_ms", name="Buffer", short_name="BUF", type="int", min=11, max=139,
         default=17, unit="ms"),
]

def build():
    descriptions, valuelists, defaults, _ = load()
    by_key, by_name = {}, {}
    for raw in descriptions:
        if not raw.get("section") or raw["section"].startswith("Multi"):
            continue
        merged = with_defaults(raw, defaults)
        if not merged.get("isPublic", True):
            continue
        key = key_for(merged["name"])
        merged["key"] = key
        by_key[key] = merged
        by_name[merged["name"]] = merged

    problems = []

    def K(keys):
        for key in keys:
            if key not in by_key:
                problems.append(f"no such parameter: {key}")
        return keys

    for level in LEVELS:
        if level["child"]:
            # The keys are templates; check the instance they are sampled from.
            K([level["child"]["sample"].format(key=k) for k in level["keys"]])
            continue
        K(level["keys"])
        K(list(level["hidden"]))
    if problems:
        raise SystemExit("\n".join(problems))
    return by_key, valuelists


FX_TYPES = {
    "fx1": ["Bypass", "Chorus", "Flanger", "Phaser", "Overdrive", "Five FX", "Vocoder"],
    "fx2": ["Bypass", "Chorus", "Flanger", "Phaser", "Overdrive", "Five FX", "Vocoder",
            "Delay", "Reverb"],
}
# Which parameter group belongs to which type index, per unit. The surround
# ("51") and Five FX groups are left out: 5.1 is meaningless on a stereo
# device and would cost 15 cells behind a type nobody can hear.
FX_GROUPS = {"chorus": "Chorus", "flanger": "Flanger", "phaser": "Phaser",
             "overdrive": "Overdrive", "delay": "Delay", "reverb": "Reverb",
             "vocoder": "Vocoder"}

def fx_level(unit, by_key):
    types = FX_TYPES[unit]
    keys = [f"{unit}_type", f"{unit}_mix"]
    hidden = {}
    for group, label in FX_GROUPS.items():
        if label not in types:
            continue
        members = sorted(k for k in by_key
                         if k.startswith(f"{unit}_{group}_") or k == f"{unit}_{group}")
        for key in members:
            hidden[key] = (f"{unit}_type", types.index(label))
        keys += members
    return L(unit, f"FX {unit[-1]}", keys, knobs=keys, hidden=hidden)


def ui_range(param):
    """(min, max, offset, scale) -- what the knob shows vs what the wire carries.

    raw = shown * scale + offset. The microQ stores bipolar values centred on
    64 and octaves in steps of 12; showing 0..127 for a detune, or 16..112 for
    an octave, is honest about the wire and useless to a player."""
    low, high, step = param["min"], param["max"], param.get("step", 0) or 1
    if param.get("toText") == "octaves" and step == 12:
        return (low - 64) // 12, (high - 64) // 12, 64, 12
    if param.get("isBipolar") and not param.get("isDiscrete") and low == 0 and high == 127:
        return -64, 63, 64, 1
    if param.get("isBipolar") and param.get("toText") == "signed":
        return low - 64, high - 64, 64, 1
    return low, high, 0, 1


# A slot's cells are told apart by their slot number, so the number has to be
# IN the label: eight cells all drawing "SRC" is the documented obxd defect
# (two cells both reading OCTAVE), and the page becomes unreadable.
SLOT_LABEL = [
    (r"^fmod([1-8])_(source|destination|amount)$", "F{n} {what}", "F{n}{ab}"),
    (r"^smod([1-8])_(source|destination|amount)$", "M{n} {what}", "M{n}{ab}"),
    (r"^modif([1-4])_(source1|source2|operator|constant)$", "M{n} {what}", "M{n}{ab}"),
]
SLOT_ABBREV = {"source": ("Src", "SRC"), "destination": ("Dest", "DST"),
               "amount": ("Amt", "AMT"), "source1": ("Src1", "S1"),
               "source2": ("Src2", "S2"), "operator": ("Op", "OP"),
               "constant": ("Const", "CN")}


def slot_label(key):
    for pattern, label_form, short_form in SLOT_LABEL:
        match = re.match(pattern, key)
        if not match:
            continue
        number, field = match.group(1), match.group(2)
        long_form, abbrev = SLOT_ABBREV[field]
        return (label_form.format(n=number, what=long_form),
                short_form.format(n=number, ab=abbrev))
    return None, None


# Graphics the detectors would otherwise infer. Declaring them makes the
# picture intent rather than a guess, and pins it if a detector changes.
# Both groups sit inside one row of four by construction -- an envelope's
# A/D/S/R are cells 1-4 of their level, cutoff and resonance are adjacent --
# and a group split across the row break is not drawn at all.
VIZ = {}
for _prefix in ("fenv", "aenv", "env3", "env4"):
    for _role in ("attack", "decay", "sustain", "release"):
        VIZ[f"{_prefix}_{_role}"] = {"group": _prefix, "role": _role}
for _n in (1, 2):
    VIZ[f"flt{_n}_cutoff"] = {"group": f"flt{_n}", "role": "cutoff"}
    VIZ[f"flt{_n}_resonance"] = {"group": f"flt{_n}", "role": "resonance"}


def param_entry(key, param, valuelists, level_id=None):
    entry = {"key": key, "name": label_for(param["name"], key, level_id or "")}
    short = short_for(key)
    slot_name, slot_short = slot_label(key)
    if slot_name:
        entry["name"], short = slot_name, slot_short
    if short:
        entry["short_name"] = short
    if key in VIZ:
        entry["viz"] = VIZ[key]
    options = None
    if param.get("isDiscrete") and param.get("toText") in valuelists:
        options = [str(o) for o in valuelists[param["toText"]]]
        options = options[: param["max"] - param["min"] + 1]
    if options and len(options) > 1 and all(o for o in options):
        entry["type"] = "enum"
        entry["options"] = options
        # Only carry a default that differs from the first option: every byte
        # of chain_params counts against a hard host ceiling.
        initial = max(0, param.get("default", param["min"]) - param["min"])
        if initial:
            entry["default"] = initial
    else:
        low, high, offset, scale = ui_range(param)
        entry["type"] = "int"
        entry["min"], entry["max"] = low, high
        default = param.get("default", param["min"])
        shown = (default - offset) // scale if scale else default
        if shown != low:
            entry["default"] = shown
    return entry


def build_contract():
    by_key, valuelists = build()
    levels = list(LEVELS) + [fx_level("fx1", by_key), fx_level("fx2", by_key)]

    params, seen = [], set()
    for wrapper in WRAPPER:
        entry = {k: v for k, v in wrapper.items() if v is not None}
        params.append(entry); seen.add(entry["key"])

    hierarchy_levels = {}
    root_links = []
    child_templates = []
    for level in levels:
        if level["child"]:
            child = level["child"]
            entries, knob_keys = [], []
            for template_key in level["keys"]:
                sample = child["sample"].format(key=template_key)
                param = by_key[sample]
                meta = param_entry(sample, param, valuelists, level["id"])
                # Every instance is declared, so nothing is addressable only by
                # a template the host might resolve differently than we expect.
                for index in range(child["count"]):
                    concrete = child["template"].format(
                        index=str(index + child["base"]).zfill(child["digits"]), key=template_key)
                    if concrete not in seen:
                        instance = dict(meta); instance["key"] = concrete
                        params.append(instance); seen.add(concrete)
                # The listed key is the TEMPLATE, and it carries its metadata
                # inline because it is not itself a chain_params entry.
                item = {k: v for k, v in meta.items() if k != "key"}
                item["key"] = template_key
                item["label"] = item.pop("name")
                entries.append(item); knob_keys.append(template_key)
            hierarchy_levels[level["id"]] = {
                "label": level["label"], "children": None,
                "child_count": child["count"], "child_label": child["label"],
                "child_key_template": child["template"],
                "child_index_base": child["base"], "child_index_digits": child["digits"],
                "knobs": knob_keys, "params": entries,
            }
            root_links.append({"level": level["id"], "label": level["label"]})
            child_templates.append((child, level["keys"]))
            continue
        knob_keys = level["knobs"] if level["knobs"] is not None else level["keys"][:8]
        entries = []
        for key in level["keys"]:
            param = by_key[key]
            entry = param_entry(key, param, valuelists, level["id"])
            if key in level["relabel"]:
                entry["name"], entry["short_name"] = level["relabel"][key]
            if key in level["hidden"]:
                gate_key, gate_value = level["hidden"][key]
                entry["visible_if"] = {"key": gate_key, "equals": gate_value}
            if key not in seen:
                params.append(entry); seen.add(key)
            item = {"key": key, "label": entry["name"]}
            if "short_name" in entry:
                item["short_name"] = entry["short_name"]
            entries.append(item)
        hierarchy_levels[level["id"]] = {
            "label": level["label"], "children": None,
            "knobs": knob_keys, "params": entries,
        }
        root_links.append({"level": level["id"], "label": level["label"]})

    hierarchy_levels["settings"] = {
        "label": "Settings", "children": None,
        "knobs": ["dsp_clock", "gain", "buffer_ms"],
        "params": [{"key": "preset", "label": "Preset"}, {"key": "mode", "label": "Mode"},
                   {"key": "part", "label": "Part"},
                   {"key": "part_channel", "label": "Part Channel"},
                   {"key": "part_volume", "label": "Part Volume"},
                   {"key": "dsp_clock", "label": "DSP Clock"}, {"key": "gain", "label": "Gain"},
                   {"key": "buffer_ms", "label": "Buffer"}],
    }
    root_links.append({"level": "settings", "label": "Settings"})

    # The landing page: the eight a player reaches for first. Cutoff and
    # resonance adjacent; the filter envelope's A/D/S/R in cells 5-8 so it is
    # one row and the envelope graphic can be drawn across it.
    # Root is the factory preset browser AND its own knob grid, the shape
    # osirus/surge/minijv all use. The three selector keys are dropped from
    # knob pages by the planner, so the eight knobs are all real controls:
    # filter across the top row, the filter envelope's A/D/S/R across the
    # second so its graphic gets a contiguous row.
    hierarchy_levels["root"] = {
        "label": "microQ",
        "list_param": "preset", "count_param": "preset_count",
        "name_param": "preset_name",
        "children": None,
        "knobs": ["flt1_cutoff", "flt1_resonance", "flt1_env_mod", "amp_volume",
                  "fenv_attack", "fenv_decay", "fenv_sustain", "fenv_release"],
        "params": [{"key": "preset", "label": "Preset"}] + root_links,
    }
    hierarchy = {"pad_layout": "chromatic", "levels": hierarchy_levels}
    return params, hierarchy, by_key, levels



def check(params, hierarchy, by_key, levels):
    """The assertions exist because none of these fail visibly at runtime."""
    problems = []
    declared = {p["key"] for p in params}
    wrapper = {w["key"] for w in WRAPPER}

    # 1. Totality: a param declared in chain_params and listed on no page is
    #    unreachable from any UI -- the fleet's largest source of dead params.
    placed = set()
    for level in hierarchy["levels"].values():
        placed.update(level["knobs"])
        placed.update(item["key"] for item in level["params"] if "key" in item)
    for level in hierarchy["levels"].values():
        template = level.get("child_key_template")
        if not template:
            continue
        for index in range(level["child_count"]):
            stamp = str(index + level["child_index_base"]).zfill(level["child_index_digits"])
            for item in level["params"]:
                placed.add(template.format(index=stamp, key=item["key"]))
    for key in declared - placed:
        problems.append(f"unreachable: {key} is declared but on no page")

    # 2. Every key a page lists must be declared, or the grid invents a
    #    float 0..1 knob and writes 0.058750 into it.
    for name, level in hierarchy["levels"].items():
        if level.get("child_key_template"):
            continue   # lists templates, each carrying its metadata inline
        for key in set(level["knobs"]) | {i["key"] for i in level["params"] if "key" in i}:
            if key not in declared:
                problems.append(f"undeclared: {name} lists {key}")

    # 3. The 5x7 font draws nothing outside printable ASCII.
    def ascii_only(text):
        return all(0x20 <= ord(c) <= 0x7e for c in text)
    for p in params:
        for field in ("name", "short_name", "unit"):
            if field in p and not ascii_only(str(p[field])):
                problems.append(f"non-ascii {field} on {p['key']}: {p[field]!r}")
        for option in p.get("options", []):
            if not ascii_only(option):
                problems.append(f"non-ascii option on {p['key']}: {option!r}")

    # 4. A knob cannot move an empty range.
    for p in params:
        if p.get("type") == "int" and p.get("max", 1) <= p.get("min", 0):
            problems.append(f"empty range on {p['key']}: {p.get('min')}..{p.get('max')}")
        # `preset` gets its 300 options at runtime, from the compiled-in table.
        if p.get("type") == "enum" and p["key"] != "preset" and len(p.get("options", [])) < 2:
            problems.append(f"enum with fewer than two options: {p['key']}")

    # 5. Two cells on one page must not draw the same label.
    for name, level in hierarchy["levels"].items():
        knobs = level["knobs"]
        labels = {item["key"]: item.get("short_name") or item["label"]
                  for item in level["params"] if "key" in item}
        for page in range(0, len(knobs), 8):
            seen = {}
            for key in knobs[page:page + 8]:
                label = labels.get(key, key)
                if label in seen:
                    problems.append(f"{name} page {page//8 + 1}: {seen[label]} and {key} both draw {label!r}")
                seen[label] = key

    # 6b. A modulation slot's three cells must not straddle a page break.
    for name, level in hierarchy["levels"].items():
        knobs = level["knobs"]
        for page in range(0, len(knobs), 8):
            window = knobs[page:page + 8]
            slots = {}
            for key in window:
                match = re.match(r"^([fs]mod\d+)_", key)
                if match:
                    slots.setdefault(match.group(1), 0)
                    slots[match.group(1)] += 1
            for slot, seen in slots.items():
                total = sum(1 for k in knobs if k.startswith(slot + "_"))
                if seen != total:
                    problems.append(f"{name} page {page//8 + 1}: slot {slot} split "
                                    f"({seen} of {total} cells)")

    # 6. An envelope's A/D/S/R must sit inside ONE row of four cells or the
    #    graphic is not drawn at all and the members fall back to dials.
    for name, level in hierarchy["levels"].items():
        knobs = level["knobs"]
        quartet = [i for i, k in enumerate(knobs)
                   if k.endswith(("_attack", "_decay", "_sustain", "_release"))
                   and not k.endswith("_attack_level")]
        if len(quartet) == 4:
            rows = {i // 4 for i in quartet}
            if len(rows) > 1 or quartet != list(range(quartet[0], quartet[0] + 4)):
                problems.append(f"{name}: A/D/S/R at {quartet} straddles the row break")
    return problems


if __name__ == "__main__":
    params, hierarchy, by_key, levels = build_contract()
    issues = check(params, hierarchy, by_key, levels)
    if issues:
        print("\n".join(f"FAIL: {i}" for i in issues))
        raise SystemExit(1)

    # chain_params is emitted in two halves so the 300 preset options can be
    # spliced in at runtime from the compiled-in name table, instead of being
    # written twice.
    rest = json.dumps(params, separators=(",", ":"))[1:-1]
    hierarchy_json = json.dumps(hierarchy, separators=(",", ":"))

    def c_string(text, name):
        chunks = []
        for i in range(0, len(text), 100):
            chunks.append('    "' + text[i:i + 100].replace("\\", "\\\\").replace('"', '\\"') + '"')
        return f"static const char {name}[] =\n" + "\n".join(chunks) + ";\n"

    table = []
    previous = ""
    for key, param in sorted(by_key.items()):
        assert key > previous, f"table not sorted at {key}"
        previous = key
        if key not in {p["key"] for p in params}:
            continue
        low, high, offset, scale = ui_range(param)
        table.append(f'    {{"{key}", {param["index"]}, {offset}, {scale}, {param["min"]}, {param["max"]}}},')

    out = [
        "// GENERATED by tools/gen_ui.py from gearmulator's parameterDescriptions_mq.json.",
        "// Do not edit; change the level tables in that script and regenerate.",
        "#pragma once",
        "",
        "namespace vavra {",
        "",
        "// One synth parameter: the sysex index it is written at, and how the",
        "// value shown on the knob maps to the byte on the wire",
        "// (raw = shown * scale + offset).",
        "struct MqParam { const char* key; uint16_t index; int8_t offset; uint8_t scale;",
        "                 uint8_t rawMin, rawMax; };",
        "static const MqParam g_mqParams[] = {",
        *table,
        "};",
        "",
        c_string(rest, "g_chainParamsRest"),
        c_string(hierarchy_json, "g_uiHierarchy"),
        "}",
        "",
    ]
    (ROOT / "src/dsp/vavra_ui.h").write_text("\n".join(out))
    print(f"wrote src/dsp/vavra_ui.h: {len(table)} synth params, "
          f"chain_params {len(rest)} bytes, ui_hierarchy {len(hierarchy_json)} bytes")
    cp = json.dumps(params, separators=(",", ":"))
    uh = json.dumps(hierarchy, separators=(",", ":"))
    print(f"{len(params)} chain_params, {len(hierarchy['levels'])} levels")
    print(f"chain_params  {len(cp):>7} bytes")
    print(f"ui_hierarchy  {len(uh):>7} bytes")
    print("(the 300 preset names no longer travel in the contract; the preset "
          "page reads one name at a time)")
