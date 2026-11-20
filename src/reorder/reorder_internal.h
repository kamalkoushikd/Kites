/**
 * @file reorder_internal.h
 * @brief Internal data shared by the reorder_*.cpp files. Not part of the public API.
 *
 * This is the only header that includes the C list scheduler. Its macros
 * (MAX_TEXT, CLASS_LOAD, FMT_R, ...) and global types (Program, Instr, Dag, ...)
 * must not leak into other Kites code, so include it only from the src/reorder sources.
 */
#pragma once

#include "reorder_analysis.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern "C"
{
#include "list_scheduler/src/common.h"

// Defined in the scheduler sources but not declared in common.h.
void find_blocks(Program *p);
int split_words(char code[], char words[][MAX_WORD]);
}

namespace Kites
{
namespace reorder
{
namespace detail
{

enum class Barrier
{
    None,
    UnknownMnemonic, ///< not in the scheduler's instruction table (F/D, CSR, custom, ...)
    UnclearOperands, ///< known mnemonic, but the operands are not in a form it understands
    PcRelative,      ///< auipc: its value depends on where it is
    NearBarrier,     ///< no-hazard-unit pipelines: next to a barrier, so it keeps its place too
};

/// Why an instruction waits before it can enter the pipeline (from issue() and stall_producer()).
struct Wait
{
    int stalls = 0;     ///< cycles (or NOPs) needed before it
    int inner = 0;      ///< stalls inside it (la, big li, call without forwarding): no NOP can help
    int producer = -1;  ///< instruction whose result it waits for
    int reg = -1;       ///< register it waits for
    int startSlot = 0;  ///< issue slot in which it enters IF
};

enum class MoveReason
{
    FillsGap,           ///< it now sits between a producer and the instruction waiting for it
    Separates,          ///< it waited itself, and moved away from its producer
    FollowsDependency,  ///< it had to move because an instruction it depends on moved
    Indirect,           ///< putting it back adds stalls, but not through a gap it fills
    Neutral,            ///< putting it back gives the same number of stalls
    KeepsRelativePlace, ///< it only looks moved: other lines moved past it
};

struct MoveInfo
{
    int instruction = -1;
    int block = -1;
    int fromPos = 0; ///< position inside the block in the original order
    int toPos = 0;   ///< position inside the block in the new order
    MoveReason reason = MoveReason::Neutral;
    int saving = 0;  ///< stalls added if it alone is put back (can be <= 0)
    std::vector<int> fixedConsumers; ///< instructions that wait longer if it is put back
    int gapProducer = -1;
    int gapConsumer = -1;
    int pinnedBy = -1;         ///< FollowsDependency: the instruction it must stay next to
    bool pinnedBefore = false; ///< true: it must stay before pinnedBy, false: after
    std::vector<int> passed;   ///< instructions whose order with it changed
    int limit = -1;            ///< nearest dependence in the direction it moved
    bool limitAbove = true;    ///< limit is above it (it moved up) or below it
};

/// No-hazard-unit pipelines: the NOPs in front of one instruction slot.
struct GapNops
{
    std::vector<int> studentLines; ///< 0-based lines of the NOPs written in the original text
    int kept = 0;                  ///< how many of those stay
    int needed = 0;                ///< NOPs needed there in the new order
    int insertBeforeLine = -1;     ///< 0-based line before which added NOPs go
    int insertAfterLine = -1;      ///< or: 0-based line after which added NOPs go
    int added() const { return needed > kept ? needed - kept : 0; }
    int total() const { return kept + added(); } ///< NOP lines in front of the slot in the new text
};

struct LineInfo
{
    std::string text;       ///< original line without '\n'
    int instruction = -1;   ///< instruction (original slot) on this line, -1 if none
    int labelColon = -1;    ///< position of the label ':' in text, -1 if no label
    bool labelOnly = false; ///< "loop:" with nothing else (comments allowed)
    bool blankOrComment = false;
    bool strippedNop = false; ///< no-hazard-unit pipelines: a nop line, not given to the scheduler
    int nopGap = -1;          ///< for stripped NOPs: the slot they stand in front of
};

struct Context
{
    AnalysisOptions options;
    bool forwarding = true;
    bool hazardUnit = true;
    HazardModel model{};
    std::unique_ptr<Program> program;
    std::vector<LineInfo> lines;
    bool endsWithNewline = false;
    int instructionCount = 0;

    std::vector<Barrier> barrier; ///< per instruction
    std::vector<int> order;       ///< final order: order[slot] = instruction
    std::vector<int> slotOf;      ///< slotOf[instruction] = slot in the final order
    std::vector<int> blockOf;     ///< per instruction
    /// per block: deps[b][u][v] = 1 if instruction start+v must stay after start+u (u < v),
    /// from the scheduler's own build_dag()
    std::vector<std::vector<std::vector<unsigned char>>> deps;

    std::vector<Wait> waitBefore;  ///< original order (NOPs of the original text ignored)
    std::vector<Wait> waitAfter;   ///< new order
    std::vector<Wait> waitStudent; ///< no-hazard-unit only: original text with its own NOPs
    int finalSlotBefore = 0;       ///< issue slots used by the original order (with stalls)
    int finalSlotAfter = 0;
    int finalSlotStudent = 0;

    std::vector<GapNops> gaps; ///< no-hazard-unit only: size instructionCount + 1
    std::vector<int> rightStart; ///< no-hazard-unit only: issue slot of each instruction in the new text
    int finalSlotRight = 0;
    std::vector<MoveInfo> moves;
    std::vector<int> moveOf; ///< per instruction, index into moves or -1

    std::vector<std::string> notes;
};

// reorder_analysis.cpp
/// The whole analysis; ctx keeps the scheduler's data for callers that need more than the result.
bool analyze(Context &ctx, const std::string &source, const AnalysisOptions &options, AnalysisResult &result);
bool isNoHazardUnit(const Context &ctx);
/// true if `later` must stay after `earlier` (same block, earlier < later in the original order)
bool dependsOn(const Context &ctx, int earlier, int later);
int blockStart(const Context &ctx, int block);
int blockEnd(const Context &ctx, int block);
int originalLineOf(const Context &ctx, int instruction); ///< 1-based
bool nopIsKept(const Context &ctx, int line);          ///< 0-based line of a stripped NOP

// reorder_explain.cpp
std::string escapeHtml(const std::string &text);
std::string instructionCode(const Context &ctx, int instruction);
std::string waitTooltip(const Context &ctx, int consumer, bool newOrder);
std::string missingNopTooltip(const Context &ctx, int consumer);
std::string innerStallTooltip(const Context &ctx, int instruction); ///< la / big li / call without forwarding
std::string fixedNote(const Context &ctx, int consumer);
std::string remainingNote(const Context &ctx, int consumer);
std::string moveTooltip(const Context &ctx, const MoveInfo &move);
std::string moveShortText(const Context &ctx, const MoveInfo &move);
std::string nopTooltip(const Context &ctx, int slot, bool kept, bool added);
std::string blockDecisionHtml(const Context &ctx, int block);
std::string stallBadge(const Context &ctx, int consumer, bool newOrder);
std::string overviewHtml(const Context &ctx, const AnalysisResult &result);
std::string registerName(const Context &ctx, int instruction, int reg);

// reorder_diff.cpp
void buildDiff(Context &ctx, AnalysisResult &result);

} // namespace detail
} // namespace reorder
} // namespace Kites
