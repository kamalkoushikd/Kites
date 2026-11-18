// Tests for src/scheduler: the examples of the P-1 report must give the same results.
#include <gtest/gtest.h>

#include "scheduler/scheduler.h"

#include <string>
#include <vector>

using namespace Kites::scheduler;

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

TEST(Scheduler, ReportsAssemblyErrors)
{
    const Result result = analyzeWith("addz x1, x2, x3\n", true);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
}
