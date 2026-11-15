# V-Track

## Build and test output

- Build: `PYTHON=~/.venv-fw/bin/python3 ./build.sh 2>&1 | tail -3` (this machine's system `python3` has no Pillow; the venv has it).
- Full suite: ~30 s and ~80 KB of `ok` lines. Run it once, at the end of a ticket, filtered: `tests/run_tests.sh 2>&1 | grep -E "FAIL|OVER BUDGET|PASSED|build.sh first"`.
- While iterating: compile the one test with its line from `tests/run_tests.sh`, run it as `timeout 60 build/host/<test> | grep -v " ok$"`. A hung simulator test spins until the timeout; there is no gdb here.
- ISR cost after a build: `python3 tests/target_budget.py` (one line per function).

## Reading large files

- The header comment is each module's interface: `tests/vt_sim.h` (seam A) and every `firmware/src/*.c`. Read it (`sed -n 1,40p`) before the body.
- The parent spec #1 is ~30 KB. Read the section you need: `gh issue view 1 --json body -q .body | sed -n '/^### Modules/,/^### Control/p'`.
- `tools/build.py`, `firmware/src/main.c`, `tests/run_tests.sh`, `firmware/hal/smk_input*.h`: `grep -n` first, then read the line range.

## Agent skills

### Issue tracker

GitHub Issues on DEV-BLOCK-M27/V-Track (the fork, never upstream Felucca). See `docs/agents/issue-tracker.md`.

### Triage labels

The five default labels (needs-triage, needs-info, ready-for-agent, ready-for-human, wontfix). See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `GLOSSARY.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.
