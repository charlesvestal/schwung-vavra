# The 64 KB contract ceiling, and what it costs this module

`chain_params` and `ui_hierarchy` are each served through one
`shadow_param_t.value` buffer, sized by `SHADOW_PARAM_VALUE_LEN` (65536) in
Schwung's `src/host/shadow_constants.h`. A module whose answer is longer is not
truncated -- the chain host **rejects the module** with "UI buffer overflow"
(`chain_host.c:518`), leaving a slot that loads and shows an error.

## Where this module sits

The ceiling is **131072** since schwung #444; it was 65536.

| string | bytes | share of the ceiling |
|---|---|---|
| `chain_params` (394 params) | 64,780 | 49% |
| `ui_hierarchy` (31 levels) | 26,478 | 20% |

It briefly reached 70,708 bytes -- 108% of the old ceiling -- while the 300
preset names were still riding in `chain_params` as enum options. Moving preset
selection to a browser page took 6,233 bytes back out, so the module now fits
the old ceiling again, at 99% of it. That is not a place to sit: the headroom
is what makes the current parameter set safe to keep.

## What is left out

The microQ publishes **449** single-sound parameters; this module exposes 386.
What remains out is now editorial rather than forced:

- **The 5.1 / surround delay parameters** (15), meaningless on a stereo device.
- **The Five FX composite** (8 per unit), and a handful of unreachable oddments.

Everything the old ceiling forced out is back: all eight slots of both
modulation matrices, the arpeggiator's 80 user-pattern parameters (as one
instance picker plus one grid, via `child_key_template`), and the four envelope
trigger modes.

## How the raise was done (schwung #444, merged)

`SHADOW_PARAM_VALUE_LEN` 65536 -> 131072, and it could **not** go on its own.

`shadow_param_t.value` lives in the `/schwung-param` SHM segment, so the
segment grows by 64 KB. That part is cheap and well-trodden: /dev/shm is tmpfs
and allocates by page, `shadow_shm_map()` fstats on attach and refuses a short
segment with a log line rather than a SIGBUS, and the shim and shadow_ui deploy
together. Two earlier resizes (#358, #361) used exactly that procedure.

The blocker is elsewhere. `chain_mod_refresh_target_param_cache`
(`src/modules/chain/dsp/chain_mod.c:561`) declares BOTH
`char buf[SHADOW_PARAM_VALUE_LEN]` and `chain_param_info_t parsed[MAX_CHAIN_PARAMS]`
as locals. `chain_param_info_t` is ~4.3 KB -- `options[128][32]` alone is 4 KB --
so that array is **~1.05 MB of stack**, and the function is called every
`MOD_PARAM_CACHE_REFRESH_MS` (250 ms). Doubling the constant adds another 64 KB
to that frame.

Both locals are `static` now, and `tests/host/test_param_buffers_not_on_stack.sh`
fails if either returns to the stack. `static` rather than heap because every
caller arrives on the callback thread, where `malloc` is a realtime violation.

**Sequencing.** A module larger than 64 KB is rejected by a host that has not
been upgraded, so this module requires schwung >= #444 on the device.

## The unfinished thread: re-planning fingerprints the whole contract

`planPages` computes `fingerprintOf([hierarchy, chainParams, mode])` on every
call -- a `JSON.stringify` of the entire contract followed by a per-character
FNV loop over every one of those characters. `replanIfCondition` calls
`planPages` on **every detent of a gating knob**, so turning an FX type hashes
94 KB per detent.

Measured in node, over the fleet's contracts:

| module | contract | fingerprint |
|---|---|---|
| vavra | 94 KB | 0.78 ms |
| minijv | 83 KB | 0.39 ms |
| surge | 64 KB | 0.30 ms |
| osirus | 38 KB | 0.19 ms |
| obxd | 9 KB | 0.04 ms |

Node is a JIT; the device runs QuickJS on an A72, where a 94,000-iteration
character loop is far slower. That is what stalls the screen while the type
knob turns.

The fingerprint exists to notice a contract that CHANGED -- osirus swaps
`rom_index`'s options from `["(loading)"]` to the real models -- but on a
re-plan driven by a VALUE change the objects are identical by reference, so
memoising it on `(hierarchy, chainParams, mode)` identity would skip it
entirely. Not attempted; the module was parked first.
