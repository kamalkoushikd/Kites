# list_scheduler (vendored)

RISC-V basic-block list scheduler from the "Assembly Code Reordering with
Suggestions for KITES" project (P-1). These files are an **unmodified copy**
of `list_scheduler/src` from that project:

| file        | role                                                         |
|-------------|--------------------------------------------------------------|
| `common.h`  | all constants, structs and function declarations             |
| `isa.c`     | instruction table (RV64I, RV64M, pseudo-instructions)        |
| `parser.c`  | reads a `.s` file into a `Program`                           |
| `hazard.c`  | 5-stage pipeline timing (`issue`, `count_stalls`)            |
| `dag.c`     | dependence DAG of a window                                   |
| `sched.c`   | list scheduling and the loop over blocks and windows         |

The tool's own `main.c`, `show.c` and `verify.c` are not included: Kites uses
the scheduler as a library. `CMakeLists.txt` builds these files into the
`kites_list_scheduler` static library (C99), which `kites_core` links.

Kites-side code lives in `src/reorder/` (analysis, explanations, diff) and
`src/ui/reorder_panel/` (the "Instruction Reordering" dock). To update the
scheduler, copy the new files over these ones; nothing in Kites edits them.
