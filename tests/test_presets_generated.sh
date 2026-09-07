#!/usr/bin/env bash
# The preset names exist in three places -- docs/presets-os223.tsv (the dump),
# src/dsp/presets_os223.h (compiled into the module) and src/module.json's
# enum options (parsed by the chain host) -- and only the first is authored.
# A stale copy does not fail loudly at runtime: it shows the WRONG NAME on a
# preset that still loads, or it makes the host's param table disagree with
# the plugin's chain_params, which is how a module ends up with an invented
# knob writing a float into an enum. So compare them by content.
set -euo pipefail
cd "$(dirname "$0")/.."

python3 - <<'PY'
import json, pathlib, re, sys

root = pathlib.Path(".")
rows = []
for line in (root / "docs/presets-os223.tsv").read_text().splitlines():
    if line.startswith("#") or not line.strip():
        continue
    slot, name = line.split("\t", 1)
    rows.append(re.sub(r"\s{2,}", " ", f"{slot} {name.rstrip()}"))

failures = []
if len(rows) != 300:
    failures.append(f"docs/presets-os223.tsv has {len(rows)} presets, expected 300")

header = (root / "src/dsp/presets_os223.h").read_text()
in_header = re.findall(r'^    "(.*)",$', header, re.M)
if in_header != [r.replace('"', "'") for r in rows]:
    failures.append(f"src/dsp/presets_os223.h is stale ({len(in_header)} names); "
                    "re-run tools/gen_presets_header.py")

module = json.loads((root / "src/module.json").read_text())
options = next((p.get("options") for p in module["chain_params"] if p["key"] == "preset"), None)
if options != rows:
    failures.append(f"src/module.json preset options are stale ({options and len(options)} names); "
                    "re-run tools/gen_presets_header.py")

# Both contract strings now live in the GENERATED header, so the drift that
# matters is between that header and the generator -- a stale vavra_ui.h ships
# a UI that no longer matches module.json or the tables it came from. Compare
# by content: regenerate into memory and check the checked-in header carries
# exactly that.
import sys
sys.path.insert(0, "tools")
import gen_ui

params, hierarchy, _, _ = gen_ui.build_contract()
header = (root / "src/dsp/vavra_ui.h").read_text()


def embedded(name):
    """Reassemble a C string literal the generator emitted."""
    body = header.split(f"static const char {name}[] =", 1)[1].split(";", 1)[0]
    chunks = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
    return "".join(chunks).replace('\\"', '"').replace("\\\\", "\\")


want_params = json.dumps(params, separators=(",", ":"))[1:-1]
want_hierarchy = json.dumps(hierarchy, separators=(",", ":"))
if embedded("g_chainParamsRest") != want_params:
    failures.append("src/dsp/vavra_ui.h chain_params is stale; re-run tools/gen_ui.py")
if embedded("g_uiHierarchy") != want_hierarchy:
    failures.append("src/dsp/vavra_ui.h ui_hierarchy is stale; re-run tools/gen_ui.py")

# module.json carries the WRAPPER params only, and deliberately not the 386
# synth ones: the chain host reads the plugin's own chain_params (it only falls
# back to module.json when the plugin does not answer), and a module.json over
# 65536 bytes parses to NOTHING AT ALL -- chain_params.c refuses the file, so
# every param would be lost, not just the extra ones. Subset, then, plus the
# size limit that makes it a subset.
in_header = {p["key"] for p in json.loads("[" + embedded("g_chainParamsRest") + "]")}
in_module = {p["key"] for p in module["chain_params"]}
orphans = in_module - in_header
if orphans:
    failures.append(f"module.json declares params the plugin does not serve: {sorted(orphans)}")
module_bytes = (root / "src/module.json").stat().st_size
if module_bytes > 65536:
    failures.append(f"src/module.json is {module_bytes} bytes; over 65536 it parses to nothing")

if failures:
    for f in failures:
        print("FAIL:", f)
    sys.exit(1)
print(f"PASS: {len(rows)} preset names agree across the dump, the header and module.json")
PY
