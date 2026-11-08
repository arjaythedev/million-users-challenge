# Admission-fix benchmark evidence

See [BENCHMARKS.md](../../BENCHMARKS.md) for every run, settings, limits, and commands.

- `results/` contains all eight full runs, per-generator outputs, full histograms,
  server logs, warm-up logs, environment commands and counters, and VM snapshots.
- `comparison.json` and `measurements.csv` contain derived metrics for those runs.
- `manifest.json` records the tested base commit, local patch fingerprint, runtime
  bundle hash, image identity, execution order, and before/after source hashes.
- `diagnostics/` contains the instrumented old-policy replay, shorter controls,
  and the preceding comparison. None contributes to the final eight-run averages.
- `diagnosis-summary.json` sums the old-policy admission counters.
- `validation/` contains the failing old-policy regression, passing fixed suite,
  upstream correctness, and lifecycle output.
- `verification.json` records checks before publication. Its remote PR head is
  the pre-correction commit, not the head after this evidence is published.

Only local workspace paths and JSON whitespace were normalized in copied files.
Counts, timings, histograms, environment settings, source hashes, and error events
are unchanged. Recorded commands can contain `<checkout>`, `<benchmark-work>`, and
`<workspace>` placeholders. These are provenance records, not commands to paste.
The portable runner is `tools/compare-local.py` in the submission folder.
