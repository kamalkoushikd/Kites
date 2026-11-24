// Tests for src/reorder: the examples of the P-1 report and of the scheduler's own test files
// (Kites-Instruction-Reordering/list_scheduler/tests) must give the same results as the tool.
#include <gtest/gtest.h>

#include "reorder/reorder_analysis.h"
#include "reorder/suggestions.h"

#include <string>
#include <vector>

using namespace Kites::suggestions;

namespace
{
Result analyzeWith(const std::string &source, bool forwarding, bool hazardUnit = true, int window = 16)
{
    Options options;
    options.forwarding = forwarding;
    options.hazardUnit = hazardUnit;
    options.window = window;
    return analyze(source, options);
}

/// The instruction lines of a block change, NOP lines left out.
std::vector<std::string> instructions(const BlockChange &change)
{
    std::vector<std::string> lines;
    for (const std::string &line : change.newLines)
    {
        if (line.find("nop") == std::string::npos)
        {
            lines.push_back(line);
        }
    }
    return lines;
}

// Report, Section 5.4 (worked example, forwarding)
const std::string kSection54 = "addi x12, x11, 1\n"
                               "ld   x5, 0(x10)\n"
                               "add  x6, x5, x5\n"
                               "add  x7, x6, x6\n"
                               "add  x13, x12, x12\n";

// Report, Section 7.2 (class example: a = b + e; c = b + f)
const std::string kClassExample = "ld  x1, 0(x31)\n"
                                  "ld  x2, 8(x31)\n"
                                  "add x3, x1, x2\n"
                                  "sd  x3, 24(x31)\n"
                                  "ld  x4, 16(x31)\n"
                                  "add x5, x1, x4\n"
                                  "sd  x5, 32(x31)\n";
const std::vector<std::string> kClassReordered = {"ld  x1, 0(x31)",  "ld  x2, 8(x31)",  "ld  x4, 16(x31)",
                                                  "add x3, x1, x2",  "add x5, x1, x4", "sd  x3, 24(x31)",
                                                  "sd  x5, 32(x31)"};
} // namespace

TEST(SchedulerReport, Section54GoesFromOneStallToNone)
{
    const Result result = analyzeWith(kSection54, true);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.stallsBefore, 1);
    EXPECT_EQ(result.stallsAfter, 0);
    ASSERT_EQ(result.blocks.size(), 1u);
    EXPECT_EQ(result.blocks[0].newLines.front(), "ld   x5, 0(x10)"); // the load goes first (largest height)
    ASSERT_FALSE(result.suggestions.empty());
    EXPECT_EQ(result.suggestions[0].movedLine, 1);  // addi x12 fills the load-use gap
    EXPECT_EQ(result.suggestions[0].consumerLine, 3);
    EXPECT_EQ(result.suggestions[0].slotsNeeded, 2);
    EXPECT_EQ(result.suggestions[0].slotsPresent, 1);
}

TEST(SchedulerReport, ClassExampleWithForwarding)
{
    const Result result = analyzeWith(kClassExample, true);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.stallsBefore, 2);
    EXPECT_EQ(result.stallsAfter, 0);
    ASSERT_EQ(result.blocks.size(), 1u);
    EXPECT_EQ(instructions(result.blocks[0]), kClassReordered);
}

TEST(SchedulerReport, ClassExampleWithoutForwarding)
{
    const Result result = analyzeWith(kClassExample, false);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.stallsBefore, 8);
    EXPECT_EQ(result.stallsAfter, 2);
    ASSERT_EQ(result.blocks.size(), 1u);
    EXPECT_EQ(instructions(result.blocks[0]), kClassReordered); // the same order in both modes
}

TEST(Scheduler, ExplainsEachMoveInThreeParts)
{
    const Result result = analyzeWith(kClassExample, true);
    ASSERT_FALSE(result.suggestions.empty());
    const Suggestion &first = result.suggestions[0];
    EXPECT_EQ(first.movedText, "ld  x4, 16(x31)");
    EXPECT_NE(first.why.find("on x2: needs 2 slots, has 1"), std::string::npos) << first.why;
    EXPECT_EQ(first.effect, "−1 stall cycle");
    EXPECT_EQ(analyzeWith(kClassExample, true, false).suggestions[0].effect, "−1 NOP");
}

TEST(Scheduler, OnlyNeededNopsAreWrittenBack)
{
    // two NOPs after the load are more than needed with forwarding (1 is enough)
    const std::string source = "ld  x5, 0(x10)\nnop\nnop\nadd x6, x5, x5\n";
    const Result result = analyzeWith(source, true, false);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.studentNops, 2);
    ASSERT_EQ(result.blocks.size(), 1u);
    int nops = 0;
    for (const std::string &line : result.blocks[0].newLines)
    {
        nops += line.find("nop") != std::string::npos ? 1 : 0;
    }
    EXPECT_EQ(nops, 1);
}

TEST(Scheduler, ApplyAllReachesAFixedPoint)
{
    const std::string once = applyAll(kClassExample, Options{});
    EXPECT_EQ(applyAll(once, Options{}), once);
    EXPECT_EQ(analyze(once, Options{}).stallsBefore, 0);
}

TEST(Scheduler, StallAcrossABlockEdgeIsCounted)
{
    // Report, Section 5.5 case 4: the load is in the block before the label, its use after it
    const std::string source = "addi x13, x13, 1\nld   x5, 0(x10)\nNext:\nadd  x6, x5, x5\n";
    const Result result = analyzeWith(source, true);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stallsBefore, 1);
}

TEST(Scheduler, UnknownInstructionStaysInPlace)
{
    // the tool leaves an instruction it does not know where it is, and moves nothing across it
    const Result result = analyzeWith("ld x5, 0(x10)\naddz x1, x2, x3\nadd x6, x5, x5\n", true);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.blocks.empty());
}

// ---- the same totals as the tool (reorder -q) on its test files ---------------------------------------

namespace
{
using Kites::reorder::PipelineVariant;

/// Stalls before -> after, as the tool's "TOTAL stalls: a -> b" line.
std::pair<int, int> totals(const std::string &source, bool forwarding, int window, bool sliding,
                           bool crossBlock = true)
{
    Kites::reorder::AnalysisOptions options;
    options.variant = forwarding ? PipelineVariant::HazardUnitForwarding : PipelineVariant::HazardUnitNoForwarding;
    options.windowSize = window;
    options.slidingWindows = sliding;
    options.crossBlock = crossBlock;
    const Kites::reorder::AnalysisResult result = Kites::reorder::analyzeProgram(source, options);
    EXPECT_TRUE(result.ok) << result.error;
    return {result.stallsBefore, result.stallsAfter};
}

// tests/sliding_window.s
const std::string kSlidingWindow = "addi x11, x0, 1\naddi x12, x0, 2\naddi x13, x0, 3\nld   x5, 0(x10)\n"
                                   "add  x6, x5, x5\nadd  x7, x6, x6\nsd   x7, 8(x10)\n";
// tests/cross_block.s
const std::string kCrossBlock = "    addi x8, x3, 512\n    addi x5, x0, 5\n    lw   x9, 0(x3)\nNext:\n"
                                "    add  x10, x9, x9\n    sub  x11, x10, x9\n    sw   x11, 4(x3)\n";
// tests/edge_cases/08_pseudo_inner_stalls.s
const std::string kInnerStalls = ".data\nvalue: .word 5\n.text\n    la   t0, value\n    li   t1, 100000\n"
                                 "    lw   t2, 0(t0)\n    call func\nfunc:\n    ret\n";
// tests/edge_cases/12_fp_int_crossing.s
const std::string kFpIntCrossing = "    addi     t1, t1, 1\n    fcvt.s.w ft0, t1\n    fcvt.w.s a0, ft0, rtz\n"
                                   "    flt.s    t2, ft0, ft1\n    add      t3, t2, a0\n";
} // namespace

TEST(SchedulerTool, SlidingWindowsSeeAcrossWindowEdges)
{
    EXPECT_EQ(totals(kSlidingWindow, true, 4, true), std::make_pair(1, 0));
    EXPECT_EQ(totals(kSlidingWindow, true, 4, false), std::make_pair(1, 1));
    EXPECT_EQ(totals(kSlidingWindow, false, 4, true), std::make_pair(6, 4));
    EXPECT_EQ(totals(kSlidingWindow, false, 4, false), std::make_pair(6, 6));
}

TEST(SchedulerTool, CrossBlockPrecautionMovesTheLoadUp)
{
    EXPECT_EQ(totals(kCrossBlock, true, 16, true), std::make_pair(1, 0));
    EXPECT_EQ(totals(kCrossBlock, false, 16, true), std::make_pair(6, 4));
}

// reorder -c off: block 1 can not help itself, so nothing changes
TEST(SchedulerTool, WithoutCrossBlockPrecautionTheLoadStays)
{
    EXPECT_EQ(totals(kCrossBlock, true, 16, true, false), std::make_pair(1, 1));
    EXPECT_EQ(totals(kCrossBlock, false, 16, true, false), std::make_pair(6, 6));
}

TEST(SchedulerTool, StallsInsidePseudoInstructionsAreCounted)
{
    EXPECT_EQ(totals(kInnerStalls, true, 16, true), std::make_pair(0, 0));
    EXPECT_EQ(totals(kInnerStalls, false, 16, true), std::make_pair(8, 8));
}

TEST(SchedulerTool, FloatingPointRegistersAreTracked)
{
    EXPECT_EQ(totals(kFpIntCrossing, true, 16, true), std::make_pair(0, 0));
    EXPECT_EQ(totals(kFpIntCrossing, false, 16, true), std::make_pair(6, 6));
}
