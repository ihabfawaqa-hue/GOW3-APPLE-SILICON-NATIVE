# Contributing

Thanks for looking. This is a small, single-maintainer, personal-time project, so replies can be slow.
Focused contributions are the easiest to review and merge.

## Ground rules

- **No game files, ever.** Do not post, attach, link or request game dumps, keys, firmware, extracted
  assets, or captures of copyrighted assets in issues, pull requests, discussions or commits. Anything like
  that will be deleted.
- Keep the claims honest. State what you measured and how; say "unverified" when it is.
- By contributing you agree your work is licensed under the project's license, **GPL-2.0-or-later**
  (see [`LICENSE`](LICENSE)). Keep existing license headers and SPDX lines.

## Most useful contributions

1. **Test on other Apple Silicon** (M1, M3, M4, Pro/Max/Ultra) and report what you see.
2. **Benchmark results** from other scenes, following the rules below.
3. **Build and packaging fixes**, documentation, and portable scripts.
4. **Fixes for the known limitations** listed in the README, with a measurement showing the effect.

## Reporting a performance result

Performance depends on the scene, heat and background load, so please include:

- chip, memory, macOS version, display resolution and whether it is fanless;
- the scene (the draws-per-frame figure from `GOW3_FRAME_STATS=1`);
- the load average (`uptime`) before the run, and what else was open;
- the configuration (environment variables, runtime id from `MANIFEST.txt`);
- the frame-rate source (Metal HUD or the harness output).

Measure on an idle, cool machine, compare only inside the same scene, and alternate A and B runs.
`docs/handoff/bench/` has the scripts.

## Development notes

- Machine-specific settings go in `local.env` (see `local.env.example`); never commit it.
- Run `python3 -m unittest discover -s tests` before opening a pull request.
- Keep changes small. For a performance change, include before/after numbers and the method.
- Read [`docs/engineering/DECISIONS.md`](docs/engineering/DECISIONS.md) first: several obvious ideas were
  tried and rejected, with measurements.
- `third_party/gow3gpu` tracks upstream shadPS4; changes there are best sent upstream as well.
