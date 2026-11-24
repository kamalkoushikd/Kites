/**
 * @file reorder_suggestions.cpp
 * @brief The Suggestions view's data (suggestions.h), built from the diff analysis.
 *
 * Nothing is scheduled here: detail::analyze() runs the list scheduler once, and
 * this file only regroups its result per changed block:
 *   - the block's new lines (what Apply writes),
 *   - a before/after timeline (issue cycle and stalls of every instruction),
 *   - one Moved / Why / Effect suggestion per wait that got shorter, also a wait at the
 *     start of the next block that this block's new order shortens (cross-block).
 */
#include "reorder_internal.h"
#include "suggestions.h"

#include <algorithm>
#include <cstdlib>

namespace Kites
{
namespace suggestions
{

namespace
{

using reorder::detail::Context;
using reorder::detail::Wait;

std::string plural(int count, const std::string &word)
{
    return std::to_string(count) + " " + word + (count == 1 ? "" : "s");
}

std::string codeOf(const Context &ctx, int instruction)
{
    return ctx.program->ins[instruction].code;
}

int lineOf(const Context &ctx, int instruction)
{
    return reorder::detail::originalLineOf(ctx, instruction);
}

/// One timeline row; cycle 0 = the block's first IF.
TimelineRow timelineRow(const Context &ctx, int instruction, const Wait &wait, int base)
{
    TimelineRow row;
    row.line = lineOf(ctx, instruction);
    row.text = codeOf(ctx, instruction);
    row.fetchCycle = wait.startSlot - wait.stalls - base; // fetched, then held for the stall cycles
    row.stalls = wait.stalls;
    row.instructions = ctx.program->ins[instruction].size;
    return row;
}

/// Issue slots the consumer must be behind the producer (the scheduler's latency table).
int slotsNeeded(const Context &ctx, int producer)
{
    Program *p = ctx.program.get();
    HazardModel *model = const_cast<HazardModel *>(&ctx.model);
    return slots_used(p, model, producer) - 1 + required_gap(p, model, producer);
}

/// The new text lines of one diff segment (bubbles are not lines of the file).
void appendLines(const reorder::DiffSegment &segment, std::vector<std::string> &lines)
{
    for (const reorder::DiffCell &cell : segment.right)
    {
        if (cell.kind != reorder::CellKind::Stall && cell.kind != reorder::CellKind::MissingNop)
        {
            lines.push_back(cell.text);
        }
    }
}

std::vector<std::string> splitLines(const std::string &source, bool &endsWithNewline)
{
    std::vector<std::string> lines;
    std::string current;
    for (char c : source)
    {
        if (c == '\n')
        {
            lines.push_back(current);
            current.clear();
        }
        else
        {
            current += c;
        }
    }
    endsWithNewline = !source.empty() && source.back() == '\n';
    if (!endsWithNewline)
    {
        lines.push_back(current);
    }
    return lines;
}

reorder::AnalysisOptions analysisOptionsFor(const Options &options)
{
    reorder::AnalysisOptions result;
    if (options.hazardUnit)
    {
        result.variant = options.forwarding ? reorder::PipelineVariant::HazardUnitForwarding
                                            : reorder::PipelineVariant::HazardUnitNoForwarding;
    }
    else
    {
        result.variant = options.forwarding ? reorder::PipelineVariant::NoHazardUnitForwarding
                                            : reorder::PipelineVariant::NoHazardUnitNoForwarding;
    }
    result.windowSize = options.window;
    result.slidingWindows = options.slidingWindows;
    result.crossBlock = options.crossBlock;
    return result;
}

} // namespace

Result analyze(const std::string &source, const Options &requested)
{
    Result result;
    result.options = requested;
    result.options.window = std::clamp(requested.window, 1, MAX_WIN);
    const Options &options = result.options;

    const reorder::AnalysisOptions analysisOptions = analysisOptionsFor(options);

    Context ctx;
    reorder::AnalysisResult full;
    if (!reorder::detail::analyze(ctx, source, analysisOptions, full))
    {
        result.error = full.error;
        return result;
    }
    result.notes = full.notes;
    result.stallsBefore = full.stallsBefore;
    result.stallsAfter = full.stallsAfter;
    for (const reorder::detail::LineInfo &info : ctx.lines)
    {
        result.studentNops += info.strippedNop ? 1 : 0;
    }
    if (ctx.hazardUnit)
    {
        // nop lines are ordinary instructions on these pipelines
        for (int i = 0; i < ctx.instructionCount; ++i)
        {
            result.studentNops += std::string(ctx.program->ins[i].mnem) == "nop" ? 1 : 0;
        }
    }
    const std::string unitWord = options.hazardUnit ? "stall cycle" : "NOP";

    const std::vector<reorder::DiffSegment> &segments = full.segments;
    for (size_t s = 0; s < segments.size(); ++s)
    {
        const reorder::DiffSegment &segment = segments[s];
        if (segment.block < 0 || !segment.changed)
        {
            continue;
        }
        const int start = reorder::detail::blockStart(ctx, segment.block);
        const int end = reorder::detail::blockEnd(ctx, segment.block);

        // Apply rewrites the block and the label/comment lines just above it: NOPs for the
        // block's first instruction are placed in front of its labels.
        BlockChange change;
        change.index = segment.block;
        change.firstLine = segment.firstLine;
        change.lastLine = segment.lastLine;
        if (s > 0 && segments[s - 1].block < 0)
        {
            change.firstLine = segments[s - 1].firstLine;
            appendLines(segments[s - 1], change.newLines);
        }
        appendLines(segment, change.newLines);
        change.stallsBefore = segment.stallsBefore;
        change.stallsAfter = segment.stallsAfter;

        // Timelines (rows are added below).
        const Wait &firstBefore = ctx.waitBefore[start];
        const int baseBefore = firstBefore.startSlot - firstBefore.stalls;
        const Wait &firstAfter = ctx.waitAfter[ctx.order[start]];
        const int baseAfter = firstAfter.startSlot - firstAfter.stalls;

        // Suggestions: every wait that got shorter, with the instruction that made it shorter.
        auto newPos = [&](int instruction) { return ctx.slotOf[instruction] - start; };
        std::vector<int> fillers;
        const int blockIndex = static_cast<int>(result.blocks.size());
        auto addSuggestion = [&](int p, int c, int moved, const Wait &was, const Wait &now, bool nextBlock)
        {
            Suggestion suggestion;
            suggestion.block = blockIndex;
            suggestion.movedLine = lineOf(ctx, moved);
            suggestion.movedText = codeOf(ctx, moved);
            suggestion.producerLine = lineOf(ctx, p);
            suggestion.producerText = codeOf(ctx, p);
            suggestion.consumerLine = lineOf(ctx, c);
            suggestion.consumerText = codeOf(ctx, c);
            suggestion.reg = reorder::detail::registerName(ctx, c, was.reg);
            suggestion.slotsNeeded = slotsNeeded(ctx, p);
            suggestion.slotsPresent = suggestion.slotsNeeded - was.stalls;
            suggestion.saved = was.stalls - now.stalls;
            suggestion.wrongWithoutNops = !ctx.hazardUnit && ctx.waitStudent[c].stalls > 0;
            suggestion.why = suggestion.producerText + " (line " + std::to_string(suggestion.producerLine) + ") → " +
                             suggestion.consumerText + " (line " + std::to_string(suggestion.consumerLine) + ") on " +
                             suggestion.reg + ": needs " + plural(suggestion.slotsNeeded, "slot") + ", has " +
                             std::to_string(suggestion.slotsPresent);
            if (nextBlock)
            {
                suggestion.why += "; line " + std::to_string(suggestion.consumerLine) +
                                  " is in the next block (cross-block precaution)";
            }
            suggestion.effect = "−" + plural(suggestion.saved, unitWord);
            result.suggestions.push_back(suggestion);
            if (std::find(fillers.begin(), fillers.end(), moved) == fillers.end())
            {
                fillers.push_back(moved);
            }
        };
        for (int c = start; c < end; ++c)
        {
            const Wait &was = ctx.waitBefore[c];
            const Wait &now = ctx.waitAfter[c];
            const int p = was.producer;
            if (was.stalls <= 0 || p < start || was.stalls <= now.stalls)
            {
                continue;
            }
            int moved = -1;
            int distance = -1;
            for (int x = start; x < end; ++x) // now between producer and consumer, and not before
            {
                const bool betweenNow = newPos(p) < newPos(x) && newPos(x) < newPos(c);
                const bool betweenBefore = p < x && x < c;
                if (x != c && betweenNow && !betweenBefore && std::abs(newPos(x) - (x - start)) > distance)
                {
                    moved = x;
                    distance = std::abs(newPos(x) - (x - start));
                }
            }
            if (moved < 0)
            {
                moved = newPos(c) != c - start ? c : p;
            }
            addSuggestion(p, c, moved, was, now, false);
        }

        // Cross-block: a line at the start of the next block waits less because a producer in
        // this block moved up (or lines moved from above it to below it).
        int last = end - 1; // last instruction shown in the timelines
        const int lookahead = lookahead_lines(const_cast<HazardModel *>(&ctx.model));
        for (int c = end; c < std::min(end + lookahead, ctx.instructionCount); ++c)
        {
            const Wait &was = ctx.waitBefore[c];
            const Wait &now = ctx.waitAfter[c];
            const int p = was.producer;
            if (was.stalls <= now.stalls || p < start || p >= end)
            {
                continue;
            }
            int moved = p;
            int distance = std::abs(newPos(p) - (p - start));
            for (int x = start; x < end; ++x) // was above the producer, is below it now
            {
                if (x < p && newPos(x) > newPos(p) && std::abs(newPos(x) - (x - start)) > distance)
                {
                    moved = x;
                    distance = std::abs(newPos(x) - (x - start));
                }
            }
            addSuggestion(p, c, moved, was, now, true);
            last = std::max(last, c);
        }
        for (int i = start; i <= last; ++i)
        {
            change.before.push_back(timelineRow(ctx, i, ctx.waitBefore[i], baseBefore));
        }
        for (int k = start; k <= last; ++k)
        {
            const int instruction = ctx.order[k];
            TimelineRow row = timelineRow(ctx, instruction, ctx.waitAfter[instruction], baseAfter);
            row.moved = instruction != k;
            row.fillsStall = std::find(fillers.begin(), fillers.end(), instruction) != fillers.end();
            change.after.push_back(row);
        }
        result.blocks.push_back(change);
    }

    std::sort(result.suggestions.begin(), result.suggestions.end(),
              [](const Suggestion &a, const Suggestion &b) { return a.consumerLine < b.consumerLine; });
    result.ok = true;
    return result;
}

std::string applyBlock(const std::string &source, const BlockChange &change)
{
    bool endsWithNewline = false;
    std::vector<std::string> lines = splitLines(source, endsWithNewline);
    const int first = std::clamp(change.firstLine - 1, 0, static_cast<int>(lines.size()));
    const int last = std::clamp(change.lastLine, first, static_cast<int>(lines.size()));
    lines.erase(lines.begin() + first, lines.begin() + last);
    lines.insert(lines.begin() + first, change.newLines.begin(), change.newLines.end());

    std::string text;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        text += lines[i];
        if (i + 1 < lines.size() || endsWithNewline)
        {
            text += '\n';
        }
    }
    return text;
}

std::string applyAll(const std::string &source, const Options &options)
{
    const reorder::AnalysisOptions analysisOptions = analysisOptionsFor(options);
    const reorder::AnalysisResult result = reorder::analyzeProgram(source, analysisOptions);
    return result.ok ? result.newText : source;
}

} // namespace suggestions
} // namespace Kites
