#!/usr/bin/env bash
# Every cell must draw a label somebody CHOSE.
#
# The renderer squeezes what it cannot fit, and on a word it does not know the
# squeeze drops vowels: "Sort Order" drew SORDER, "Phaser Center" drew PCENTE,
# "Direction" drew DIRCTN. A third of this module's cells were doing that, and
# none of it is visible without rendering -- the contract validates, the plan
# has no warnings, and the page looks fine until you read it.
#
# Needs the schwung checkout beside this one for the real renderer; skips
# rather than fails when it is absent, like the widget sheet does.
set -euo pipefail
cd "$(dirname "$0")/.."

SCHWUNG=${SCHWUNG_DIR:-../schwung}
if [ ! -f "$SCHWUNG/src/shared/param_pages/render_page_movy.mjs" ]; then
    echo "SKIP: no schwung checkout at $SCHWUNG"
    exit 0
fi

python3 - <<'PY'
import sys, json, datetime
sys.path.insert(0, "tools")
import gen_ui
params, hierarchy, _, _ = gen_ui.build_contract()
json.dump({"_source": "tests/test_labels.sh", "generated_at": datetime.datetime.now().isoformat(),
           "module_count": 1,
           "modules": [{"id": "vavra", "category": "sound_generator", "status": "ok",
                        "name": "Vavra", "ui_hierarchy": hierarchy,
                        "chain_params": params, "presets": []}]},
          open("/tmp/vavra-label-audit.json", "w"))
PY

node --input-type=module -e "
import fs from 'node:fs';
import * as R from '$SCHWUNG/src/shared/param_pages/render_page_movy.mjs';
import { planPages } from '$SCHWUNG/src/shared/param_pages/page_plan.mjs';
const m = JSON.parse(fs.readFileSync('/tmp/vavra-label-audit.json','utf8')).modules[0];
const meta = Object.fromEntries(m.chain_params.map(p => [p.key, p]));
const on = new Set();
for (const mode of ['play','edit','multi'])
  for (const p of planPages({ hierarchy: m.ui_hierarchy, chainParams: m.chain_params, mode }).pages)
    for (const k of (p.keys || [])) on.add(k);
const bad = [];
for (const k of [...on].sort()) {
  const p = meta[k]; if (!p) continue;
  const src = p.short_name || p.name;
  const drawn = R.labelForCell(src);
  const plain = String(src).toUpperCase().replace(/[^A-Z0-9]/g, '');
  // Only the SQUEEZE is a defect. The renderer also EXPANDS abbreviations it
  // knows -- labelForCell('AMT') is 'AMOUNT' -- and that is chosen vocabulary,
  // not an invention, so a longer result is fine.
  if (drawn.length < plain.length && !plain.startsWith(drawn))
    bad.push(k + ' \"' + src + '\" -> ' + drawn);
}
if (bad.length) {
  console.log('FAIL: ' + bad.length + ' cells draw a label the squeeze invented:');
  for (const b of bad.slice(0, 12)) console.log('   ' + b);
  process.exit(1);
}
console.log('PASS: all ' + on.size + ' cells draw a chosen label');
"
