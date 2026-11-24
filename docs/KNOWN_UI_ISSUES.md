# Known issues found while testing the suggestions feature

These were found while testing the instruction-reordering features (the
**Reorder** panel with its diff and **Suggestions** views). They are not caused by
those features and were **not** fixed, so that this branch only changes what
belongs to the new features. Screenshots were taken on a virtual screen
(Qt `offscreen` platform) with the Kites Dark and Kites Light themes.

## User interface

| Where | What happens |
|---|---|
| Compiler tab | Two labels still show Qt Designer's placeholder text **"TextLabel"** above the two editors. |
| Processor tab, light theme | The circuit diagram keeps a **black background** while the rest of the window is light; the gray block labels ("Control", "Reg File", ...) are hard to read on it. |
| Processor tab | The box above the diagram shows only the text **"CI"** before a program runs. |
| Memory tab | Below address `0x0` the table shows rows filled with **"-"** (rows that do not belong to any address). |
| Cache tab | The statistics fields (Size in Bits, Hits, Misses, Hit rate, Write backs) are empty boxes before a run instead of showing 0. |
| Main toolbar | `MainWindow::runFinishedSlot()` and `themeChangedSlot()` find toolbar buttons by their **text** (`"Run"`, `"Stop"`, ...). Any other toolbar action with one of those texts would be changed by mistake. (The new action is called "Reorder", so it is not affected.) |

## Simulator and tools

| Where | What happens |
|---|---|
| `kites-cli` (normal build) | Crashes on start: every processor creates its circuit diagram (`QGraphicsScene`), which needs a `QApplication`, but the CLI only creates a `QCoreApplication`. It works only when Kites is configured with `-DDISABLE_GUI=ON`. (`kites-cli compare` creates an invisible `QApplication`, so it works in both builds.) |
| `kites-cli` with `DISABLE_GUI=ON` | Runs on `rvss`, but **hangs on all four pipelines**, even for the hello example in `docs/CLI.md`. |
| No-hazard-unit, no-forwarding pipeline | **Any** `ecall` is treated as exit and stops the program at once, before the instructions in front of it have finished (`RV5StageProcessorNHNF::handle_syscall`). |
| Hazard detection unit (`rv5s_hdu.cpp`) | Reads bits [24:20] of every instruction as `rs2`, also for I-type instructions where those bits are part of the immediate, so some immediates cause stalls that should not happen. Kites' measured stall counts can therefore be higher than the analysis predicts. |
| Hazard detection unit (`rv5s_hdu.cpp`) | Does not tell integer and FP registers apart: `addi a0, a0, -1` (writes x10) followed by `fmul.s ft0, ft0, fa0` (reads f10) stalls although nothing is shared. In `main5.s` of the project's lab codes a reordered loop gets 1124 such stall cycles instead of 152. They are included in the measured count of the Reorder panel; `kites-cli compare --lines` lists them as stalls "without a real dependence". |
| Single-cycle vs pipelines | The pipelines NaN-box single-precision results in the FP registers (upper 32 bits all ones), the single-cycle processor does not. The same program therefore ends with different FP register values (same float). |
| Test suite | `test/test_main.cpp` includes `<windows.h>` unconditionally, so `kites_test` does not build on Linux or macOS. (The scheduler tests also build as `kites_scheduler_test`, which runs everywhere.) |
