/**
 * @file run_profile.h
 * @brief Measured (dynamic) stalls: run a program on a Kites 5-stage pipeline and
 *        count, per source line, how often it ran, how many cycles it waited and how
 *        many waits it caused.
 *
 * The scheduler counts stalls statically (every instruction once, loops once).
 * This runs the real Kites processor cycle by cycle, so a stall inside a loop that
 * runs 100 times counts 100 times. Nothing in the processors is changed: a small
 * subclass reads their pipeline registers after every cycle.
 *
 *   stall cycle  = a cycle in which the instruction in IF/ID stays there (the hazard
 *                  unit holds it back); counted for that instruction's line
 *   caused       = the same cycle, counted for the line of the instruction ahead of it
 *                  in the pipeline that writes a register it reads
 *   NOPs         = nop instructions that reached WB (pipelines without a hazard unit
 *                  never stall: the program's NOPs take the place of the stalls)
 *
 * A run ends like a Kites run (exit ecall, end of the code), and also where a lab
 * program without an exit call would run forever: a return (jalr) to address 0 (ra is
 * still 0: the program would start again) or an instruction that jumps to itself.
 */
#pragma once

#include "reorder_analysis.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Kites
{
namespace reorder
{

struct LineRun
{
    int executed = 0; ///< times the line's instruction reached WB
    int stalls = 0;   ///< cycles it waited in ID
    int caused = 0;   ///< stall cycles of other lines that waited for this line's result
    int unexplained = 0; ///< its stall cycles with no instruction ahead that writes what it reads
};

struct RunProfile
{
    bool ok = false;
    std::string error;
    bool finished = false;     ///< false: stopped at the cycle limit
    std::string endReason;     ///< how the run ended, e.g. "returned to address 0"
    unsigned cycles = 0;
    unsigned instructions = 0; ///< instructions that reached WB
    unsigned stalls = 0;       ///< stall cycles (hazard unit)
    unsigned nops = 0;         ///< nop instructions that reached WB
    unsigned unexplained = 0;  ///< stall cycles with no instruction ahead that writes what it reads
    std::map<int, LineRun> lines;    ///< by 1-based source line
    std::vector<uint64_t> registers; ///< x1..x31, f0..f31 at the end (FP NaN-boxing removed)
};

/**
 * @brief One run, advanced in steps (so a GUI can keep painting in between).
 *
 * Create it in the GUI thread: the processors build their circuit diagram when they
 * are created. Call step() until it returns false, then read profile().
 */
class ProfiledRun
{
  public:
    ProfiledRun(const std::string &source, PipelineVariant variant, unsigned maxCycles);
    ~ProfiledRun();
    ProfiledRun(const ProfiledRun &) = delete;
    ProfiledRun &operator=(const ProfiledRun &) = delete;

    /// Runs up to `cycles` more cycles. Returns false when the run is over.
    bool step(unsigned cycles);
    const RunProfile &profile() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// The whole run at once (kites-cli).
RunProfile profileRun(const std::string &source, PipelineVariant variant, unsigned maxCycles = 5000000);

} // namespace reorder
} // namespace Kites
