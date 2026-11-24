/**
 * @file reorder_analysis.h
 * @brief Instruction reordering suggestions (list scheduling) with explanations.
 *
 * This is the Qt-free core of the "Reorder" feature. It runs the list scheduler
 * (external/list_scheduler, plain C, unchanged) on the editor text, checks that
 * the new order computes the same results, and turns every decision into
 * structured data for the UI: a diff (side by side and unified) where every
 * row carries an explanation of why it changed.
 *
 * All explanations are produced from fixed sentence templates filled with facts
 * computed from the scheduler's dependence graph and pipeline timing.
 */
#pragma once

#include <string>
#include <vector>

namespace Kites
{
namespace reorder
{

/**
 * @brief The four 5-stage pipelines of Kites, each with its own hazard rules.
 */
enum class PipelineVariant
{
    HazardUnitForwarding,     ///< RV5Stage_H_F
    HazardUnitNoForwarding,   ///< RV5Stage_H_NF
    NoHazardUnitForwarding,   ///< RV5Stage_NH_F
    NoHazardUnitNoForwarding, ///< RV5Stage_NH_NF
};

bool hasForwarding(PipelineVariant variant);
bool hasHazardUnit(PipelineVariant variant);
/// e.g. "5-stage, hazard unit, forwarding"
std::string variantDescription(PipelineVariant variant);

struct AnalysisOptions
{
    PipelineVariant variant = PipelineVariant::HazardUnitForwarding;
    int windowSize = 16;        ///< 1..64 instructions per scheduling window
    bool slidingWindows = true; ///< windows 1..W, 2..W+1, ... (false: 1..W, W+1..2W, ...)
    bool crossBlock = true;     ///< cross-block precaution: also count the stalls the block's end causes in the next block
};

enum class CellKind
{
    Text,        ///< label, directive, comment or blank line
    Instruction, ///< an instruction line
    Stall,       ///< a pipeline bubble inserted by the hazard unit (not a line of the file)
    MissingNop,  ///< a hazard the code does not cover (no-hazard-unit pipelines only)
    Nop,         ///< a nop line (no-hazard-unit pipelines only)
};

enum class CellChange
{
    None,
    MovedUp,
    MovedDown,
    Removed, ///< only on the left: the line is not in the new program
    Added,   ///< only on the right: the line is new
};

struct DiffCell
{
    CellKind kind = CellKind::Text;
    CellChange change = CellChange::None;
    int line = -1;        ///< 1-based line in this side's text, -1 for bubbles
    int sourceLine = -1;  ///< 1-based line of the original text the content comes from
    int instruction = -1; ///< instruction id, the same on both sides, -1 if none
    int cycle = -1;       ///< static estimate of the cycle it enters IF, -1 if none
    int distance = 0;     ///< how many instruction positions it moved
    std::string text;     ///< text to display
    std::string badge;    ///< short note shown at the end of the row
    std::string tooltipHtml;
    std::string detailHtml;
    std::vector<int> related; ///< instruction ids to highlight together with this row
};

enum class UnifiedSign
{
    Context,
    Removed,
    Added,
};

struct UnifiedRow
{
    UnifiedSign sign = UnifiedSign::Context;
    DiffCell cell;
    int oldLine = -1; ///< 1-based, -1 if the row is not in the original text
    int newLine = -1; ///< 1-based, -1 if the row is not in the new text
};

struct DiffSegment
{
    int block = -1; ///< basic block number (0-based), -1 for lines between blocks
    bool changed = false;
    int firstLine = 0; ///< 1-based first original line of the segment
    int lastLine = 0;  ///< 1-based last original line of the segment
    int stallsBefore = 0;
    int stallsAfter = 0;
    std::string title;
    std::string tooltipHtml;
    std::vector<DiffCell> left;  ///< the original program
    std::vector<DiffCell> right; ///< the suggested program
    std::vector<UnifiedRow> unified;
};

struct MoveSummary
{
    int instruction = -1;
    int fromLine = -1; ///< 1-based line in the original text
    int toLine = -1;   ///< 1-based line in the new text
    int distance = 0;  ///< negative = moved up, positive = moved down (instruction positions)
    std::string shortText;
    std::string detailHtml;
};

struct AnalysisResult
{
    bool ok = false;
    std::string error;              ///< why the analysis could not run (ok == false)
    std::vector<std::string> notes; ///< plain-text remarks, e.g. lines that were not analyzed
    AnalysisOptions options;

    int instructionCount = 0;
    int blockCount = 0;
    int stallsBefore = 0; ///< hazard-unit pipelines: bubbles; otherwise NOPs the code needs
    int stallsAfter = 0;
    int cyclesBefore = 0; ///< static estimate, every instruction counted once
    int cyclesAfter = 0;
    int nopsBefore = 0;  ///< no-hazard-unit pipelines: NOP lines in the original text
    int nopsAfter = 0;   ///< no-hazard-unit pipelines: NOP lines in the new text
    int missingNops = 0; ///< no-hazard-unit pipelines: hazards the original text leaves open
    int movedCount = 0;
    bool changed = false; ///< the new text differs from the original text

    std::string originalText;
    std::string newText;
    /// lineMap[oldLine - 1] = new 1-based line of that line's content (instructions follow
    /// the instruction), or -1 if the line is removed
    std::vector<int> lineMap;
    std::vector<DiffSegment> segments;
    std::vector<MoveSummary> moves;
    std::string overviewHtml;
};

/**
 * @brief Analyze a program and build the suggested reordering with explanations.
 * Never throws; problems are reported through AnalysisResult::ok / error.
 */
AnalysisResult analyzeProgram(const std::string &source, const AnalysisOptions &options);

/**
 * @brief A unified diff (git style) of the suggestion, usable with `git apply`.
 */
std::string makeUnifiedPatch(const AnalysisResult &result, const std::string &fileName);

} // namespace reorder
} // namespace Kites
