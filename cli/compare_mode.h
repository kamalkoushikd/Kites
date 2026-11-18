#pragma once

namespace Kites
{
/**
 * @brief `kites-cli compare FILE [options]`: checks the scheduler's reordered program.
 *
 * Runs the original program on the single-cycle processor (the reference), then
 * the reordered program (scheduler::applyAll) on the single-cycle processor and
 * on each 5-stage pipeline, and compares the final registers and memory. Also
 * prints the cycles of the original and the reordered program on each pipeline.
 * Returns 0 when every run matches the reference.
 */
int runCompareMode(int argc, char *argv[]);
} // namespace Kites
