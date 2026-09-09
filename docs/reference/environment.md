# Environment contract (`TFORGE_*`)

**Status:** current reference
**Source of truth:** the executable — `grep -rn "getenv" src/`

Temporal Forge production code reads a large set of `TFORGE_*` environment
variables — over 300 `getenv` call sites (as of 2026-09-09) covering
experiment knobs for jitter,
dynamic resolution, chain passes, frame dumps, cache directories, Vulkan
validation, and more.

## Contract

1. **Experimental by default.** The variables are experiment knobs. They are
   not a stable user-facing configuration surface and may change between
   campaigns.
2. **Default-off / neutral.** With no `TFORGE_*` variables set, the player
   runs its shipped default policy (see
   [`../current/STATE.md`](../current/STATE.md)).
3. **Runtime-configurable by design.** Knobs are read at runtime so another
   strength, filter, or value can be tested without a source edit or
   recompile. This is a project working rule, not an accident.
4. **The authoritative inventory is the code.** No hand-maintained list in
   this document can stay complete. To enumerate what a given build actually
   reads, run:

   ```sh
   grep -rn "getenv" src/
   ```

## Notable named variables

These are called out because they alter shipped policy rather than a single
experiment parameter:

- `TFORGE_VK_VALIDATE=1` — enable the Vulkan validation layer.
- `TFORGE_QUALITY_LAB_CONFIG` — designates a **deliberate experiment
  override** of the checked-in quality-lab profile. When set, the requested
  composition is honored at every scale; when unset, the checked-in
  `config/quality_lab.json` profile is the shipped default playback policy
  (applied only at ≥3x scale — scale-aware — via `PlaybackEngine`).
- `TFORGE_FSR4_RE_ROOT` — overrides where the generic FSR4 RE weight blobs
  (`quality.bin`, `balanced.bin`, `performance.bin`, `ultraperf.bin`,
  `native.bin`, `drs.bin` — 131072 bytes each) are looked up. Point it at the
  root of the reverse-engineering tree; the engine reads
  `$TFORGE_FSR4_RE_ROOT/extracted/v410_initializers/<preset>.bin` (the
  `v410_initializers` directory contains the per-preset blobs listed above).
  Without it the
  blob is searched under the XDG data location:
  `$XDG_DATA_HOME/temporal-forge-player/fsr4/extracted/v410_initializers/`
  (or `$HOME/.local/share/...`). Native INT8 packs are resolved separately
  (see below) and are not affected by this variable.

## FSR4 asset lookup (host-independent)

Runtime asset resolution never uses paths baked in at build time. Native
INT8 packs (`resources/fsr4/native_i8/<pack>/`) resolve from
`<exe_dir>/../resources/fsr4` first (in-tree build layouts such as `build/`
or `build-fast/`), then `./resources/fsr4`. Generic weight blobs resolve via
`TFORGE_FSR4_RE_ROOT` (see above), then the XDG data location. Absence of an
asset logs the searched locations and falls down the backend chain — it is
never fatal.

## Settings file location

Settings persistence (`SettingsStore::defaultPath`) prefers
`$XDG_CONFIG_HOME/temporal-forge-player/settings.json`, then
`$HOME/.config/temporal-forge-player/settings.json`, and falls back to a
CWD-relative `temporal-forge-player-settings.json` only when neither
variable is set. The CWD fallback exists for environments without a home
directory; on a normal desktop the XDG path is used.
