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

Kites-side code lives in `src/reorder/` (analysis, explanations, diff, the
Suggestions data and the measured runs) and `src/ui/reorder_panel/` +
`src/ui/suggestions/` (the "Instruction Reordering" dock). It only calls the public
functions declared in `common.h` (`read_program`, `schedule_program`, `issue`,
`stall_producer`, `build_dag`, ...). To update the scheduler, copy the new files over
these ones; nothing in Kites edits them. If a function's signature changes, only
`src/reorder/reorder_analysis.cpp` has to follow.

Kites works around two cases in its own code (`prepare()` in reorder_analysis.cpp):
`lw rd, label` (the tool sees no destination register) and `auipc` (its value depends
on where it is) become fixed lines; `li` gets the size Kites' assembler gives it.
