/**
 * @file suggestions.h
 * @brief The "Suggestions" view of the Reorder panel: one move at a time.
 *
 * The data comes from the same analysis as the diff view (reorder_analysis.h), so
 * there is one scheduler for both views: the list scheduler in
 * external/list_scheduler (src/reorder/reorder_suggestions.cpp turns its result
 * into these structures).
 *
 * Every suggestion is reported in three parts:
 *   Moved  - the instruction that moves
 *   Why    - the hazard it removes: producer -> consumer, register, slots needed vs present
 *   Effect - "-N stall cycles" (hazard unit on) or "-N NOPs" (hazard unit off)
 * and every block carries the issue cycle of each instruction before and after,
 * which is what the pipeline timeline in the UI is drawn from.
 */
#pragma once

#include <string>
#include <vector>

namespace Kites
{
namespace suggestions
{

struct Options
{
    bool forwarding = true; ///< which latency table: forwarding on or off
    bool hazardUnit = true; ///< false: the code needs NOPs (they are recomputed for the new order)
    int window = 16;        ///< instructions per window (1..64)
    bool slidingWindows = true; ///< windows 1..W, 2..W+1, ... (false: 1..W, W+1..2W, ...)
};

/// One row of a pipeline timeline: an instruction and when it goes through the pipeline.
struct TimelineRow
{
    int line = 0;          ///< 1-based line in the editor text the analysis was made from
    std::string text;      ///< the instruction, without comment
    int fetchCycle = 0;    ///< cycle of IF (0 = first cycle of the block)
    int stalls = 0;        ///< cycles it waits between IF and ID
    int instructions = 1;  ///< machine instructions (2 for la, call, ...)
    bool moved = false;    ///< after-timeline: this instruction moved
    bool fillsStall = false;
};

/// A basic block whose order the scheduler would change.
struct BlockChange
{
    int index = 0;      ///< block number (0-based)
    int firstLine = 0;  ///< 1-based: first editor line Apply replaces
    int lastLine = 0;   ///< 1-based: last editor line Apply replaces (inclusive)
    std::vector<std::string> newLines; ///< the lines that replace [firstLine, lastLine]
    int stallsBefore = 0;
    int stallsAfter = 0;
    std::vector<TimelineRow> before; ///< original order (on no-hazard-unit pipelines: student NOPs taken out)
    std::vector<TimelineRow> after;  ///< suggested order
};

/// One move, explained as Moved / Why / Effect.
struct Suggestion
{
    int block = 0;           ///< index into Result::blocks
    int movedLine = 0;       ///< 1-based line of the instruction that moves
    std::string movedText;   ///< the instruction that moves (code only)
    int producerLine = 0;
    std::string producerText;
    int consumerLine = 0;    ///< the waiting line (where the editor shows the squiggle)
    std::string consumerText;
    std::string reg;         ///< register the consumer waits for, as written
    int slotsNeeded = 0;     ///< issue slots needed between producer and consumer
    int slotsPresent = 0;    ///< issue slots between them in the student's order
    int saved = 0;           ///< stall cycles (or NOPs) this removes
    bool wrongWithoutNops = false; ///< hazard unit off and the student's NOPs do not cover it
    std::string why;         ///< one line
    std::string effect;      ///< one line
};

struct Result
{
    bool ok = false;
    std::string error;
    Options options;
    std::vector<BlockChange> blocks;      ///< only blocks that change
    std::vector<Suggestion> suggestions;  ///< in line order
    int stallsBefore = 0;  ///< whole program (no hazard unit: NOPs the code needs, student NOPs taken out)
    int stallsAfter = 0;
    int studentNops = 0;   ///< NOP lines in the editor text
    std::vector<std::string> notes;
};

/// Analyze editor text. Never throws.
Result analyze(const std::string &source, const Options &options);

/// Replace the lines of one block change in `source` (what the Apply button does).
std::string applyBlock(const std::string &source, const BlockChange &change);

/// Apply every suggestion: apply one block, analyze again, until nothing changes.
std::string applyAll(const std::string &source, const Options &options);

} // namespace suggestions
} // namespace Kites
