/**
 * @file reorder_diff.cpp
 * @brief Builds the new program text and the diff (side by side and unified) shown in the UI.
 *
 * The diff is built from the scheduler's own mapping (which instruction went to
 * which slot), not from a text diff, so every row knows exactly which
 * instruction it is, even when two lines have the same text.
 */
#include "reorder_internal.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <map>
#include <sstream>

namespace Kites
{
namespace reorder
{
namespace detail
{
namespace
{

std::string leadingWhitespace(const std::string &text)
{
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
    {
        ++i;
    }
    return text.substr(0, i);
}

std::string trimLeft(const std::string &text)
{
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
    {
        ++i;
    }
    return text.substr(i);
}

/// The instruction part of a line (label removed), with its comment.
std::string instructionPart(const LineInfo &info)
{
    if (info.labelColon >= 0)
    {
        return trimLeft(info.text.substr(static_cast<size_t>(info.labelColon) + 1));
    }
    return trimLeft(info.text);
}

/// The text of `instruction` when it is placed on the line of another slot.
std::string renderInSlot(const Context &ctx, int instruction, int slotLine)
{
    const LineInfo &slot = ctx.lines[slotLine];
    if (slot.instruction == instruction)
    {
        return slot.text;
    }
    const LineInfo &source = ctx.lines[originalLineOf(ctx, instruction) - 1];
    std::string part = instructionPart(source);
    const bool sourceCr = !part.empty() && part.back() == '\r';
    const bool slotCr = !slot.text.empty() && slot.text.back() == '\r';
    if (sourceCr && !slotCr)
    {
        part.pop_back();
    }
    else if (!sourceCr && slotCr)
    {
        part.push_back('\r');
    }
    if (slot.labelColon >= 0)
    {
        const std::string prefix = slot.text.substr(0, static_cast<size_t>(slot.labelColon) + 1);
        std::string gap = leadingWhitespace(slot.text.substr(static_cast<size_t>(slot.labelColon) + 1));
        if (gap.empty())
        {
            gap = " ";
        }
        return prefix + gap + part;
    }
    return leadingWhitespace(slot.text) + part;
}

std::string nopText(const Context &ctx, int slot)
{
    const Program *p = ctx.program.get();
    const LineInfo &slotLine = ctx.lines[p->ins[slot].line_no - 1];
    std::string indent = slotLine.labelColon >= 0 ? std::string() : leadingWhitespace(slotLine.text);
    if (indent.empty())
    {
        indent = "    ";
    }
    const int consumer = ctx.order[slot];
    const Wait &wait = ctx.waitAfter[consumer];
    std::string text = indent + "nop";
    if (wait.reg >= 0)
    {
        char name[MAX_WORD];
        reg_name(wait.reg, name);
        text += "    # wait for " + std::string(name) + (ctx.forwarding ? " (load-use)" : " (no forwarding)");
    }
    bool crlf = false;
    for (const LineInfo &info : ctx.lines)
    {
        crlf = crlf || (!info.text.empty() && info.text.back() == '\r');
    }
    if (crlf)
    {
        text.push_back('\r');
    }
    return text;
}

std::string plural(int count, const std::string &word)
{
    return std::to_string(count) + " " + word + (count == 1 ? "" : "s");
}

std::string arrow(int distance)
{
    return std::string(distance < 0 ? "↑" : "↓") + std::to_string(std::abs(distance));
}

struct Placed
{
    int segment = 0;
    DiffCell cell;
};

bool isGhost(const DiffCell &cell)
{
    return cell.kind == CellKind::Stall || cell.kind == CellKind::MissingNop;
}

struct Group
{
    std::vector<DiffCell> ghosts;
    DiffCell main;
    bool hasMain = false;
    bool anchor = false;
    std::string key;
};

std::vector<Group> groupCells(const std::vector<DiffCell> &cells)
{
    std::vector<Group> groups;
    Group current;
    for (const DiffCell &cell : cells)
    {
        if (isGhost(cell))
        {
            current.ghosts.push_back(cell);
            continue;
        }
        current.main = cell;
        current.hasMain = true;
        if (cell.change == CellChange::None)
        {
            switch (cell.kind)
            {
            case CellKind::Text:
                current.anchor = true;
                current.key = "T" + std::to_string(cell.sourceLine);
                break;
            case CellKind::Instruction:
                current.anchor = true;
                current.key = "I" + std::to_string(cell.instruction);
                break;
            case CellKind::Nop:
                current.anchor = true;
                current.key = "N" + std::to_string(cell.sourceLine);
                break;
            default:
                break;
            }
        }
        groups.push_back(current);
        current = Group{};
    }
    if (!current.ghosts.empty())
    {
        groups.push_back(current);
    }
    return groups;
}

void emitRow(std::vector<UnifiedRow> &rows, const DiffCell &cell, UnifiedSign sign)
{
    UnifiedRow row;
    row.sign = sign;
    row.cell = cell;
    if (!isGhost(cell))
    {
        if (sign == UnifiedSign::Removed)
        {
            row.oldLine = cell.line;
        }
        else if (sign == UnifiedSign::Added)
        {
            row.newLine = cell.line;
        }
    }
    rows.push_back(row);
}

void emitGroup(std::vector<UnifiedRow> &rows, const Group &group, UnifiedSign sign)
{
    for (const DiffCell &ghost : group.ghosts)
    {
        emitRow(rows, ghost, sign);
    }
    if (group.hasMain)
    {
        emitRow(rows, group.main, sign);
    }
}

std::vector<UnifiedRow> unifiedRows(const DiffSegment &segment)
{
    std::vector<UnifiedRow> rows;
    if (!segment.changed)
    {
        for (size_t i = 0; i < segment.right.size(); ++i)
        {
            UnifiedRow row;
            row.cell = segment.right[i];
            if (!isGhost(row.cell))
            {
                row.oldLine = i < segment.left.size() ? segment.left[i].line : -1;
                row.newLine = row.cell.line;
            }
            rows.push_back(row);
        }
        return rows;
    }

    const std::vector<Group> left = groupCells(segment.left);
    const std::vector<Group> right = groupCells(segment.right);
    size_t i = 0;
    size_t r = 0;
    while (i < left.size() || r < right.size())
    {
        while (i < left.size() && !left[i].anchor)
        {
            emitGroup(rows, left[i++], UnifiedSign::Removed);
        }
        while (r < right.size() && !right[r].anchor)
        {
            emitGroup(rows, right[r++], UnifiedSign::Added);
        }
        if (i < left.size() && r < right.size())
        {
            const Group &a = left[i++];
            const Group &b = right[r++];
            if (a.key != b.key || a.main.text != b.main.text)
            {
                emitGroup(rows, a, UnifiedSign::Removed);
                emitGroup(rows, b, UnifiedSign::Added);
                continue;
            }
            const size_t common = std::min(a.ghosts.size(), b.ghosts.size());
            for (size_t g = 0; g < common; ++g)
            {
                if (a.ghosts[g].text == b.ghosts[g].text && a.ghosts[g].kind == b.ghosts[g].kind)
                {
                    emitRow(rows, b.ghosts[g], UnifiedSign::Context);
                }
                else
                {
                    emitRow(rows, a.ghosts[g], UnifiedSign::Removed);
                    emitRow(rows, b.ghosts[g], UnifiedSign::Added);
                }
            }
            for (size_t g = common; g < a.ghosts.size(); ++g)
            {
                emitRow(rows, a.ghosts[g], UnifiedSign::Removed);
            }
            for (size_t g = common; g < b.ghosts.size(); ++g)
            {
                emitRow(rows, b.ghosts[g], UnifiedSign::Added);
            }
            UnifiedRow row;
            row.sign = UnifiedSign::Context;
            row.cell = b.main;
            row.oldLine = a.main.line;
            row.newLine = b.main.line;
            rows.push_back(row);
        }
        else if (i < left.size())
        {
            emitGroup(rows, left[i++], UnifiedSign::Removed);
        }
        else if (r < right.size())
        {
            emitGroup(rows, right[r++], UnifiedSign::Added);
        }
    }
    return rows;
}

std::string signature(const std::vector<DiffCell> &cells)
{
    std::string text;
    for (const DiffCell &cell : cells)
    {
        text += static_cast<char>('0' + static_cast<int>(cell.kind));
        text += cell.text;
        text += '\n';
    }
    return text;
}

std::vector<int> relatedForMove(const MoveInfo &move)
{
    std::vector<int> related = move.passed;
    for (int extra : {move.gapProducer, move.gapConsumer, move.limit, move.pinnedBy})
    {
        if (extra >= 0)
        {
            related.push_back(extra);
        }
    }
    return related;
}

std::string notesForBarriers(const Context &ctx, Barrier kind)
{
    std::vector<int> lines;
    std::vector<std::string> names;
    for (int i = 0; i < ctx.instructionCount; ++i)
    {
        if (ctx.barrier[i] != kind)
        {
            continue;
        }
        lines.push_back(originalLineOf(ctx, i));
        const std::string mnemonic = ctx.program->ins[i].mnem;
        if (std::find(names.begin(), names.end(), mnemonic) == names.end() && names.size() < 6)
        {
            names.push_back(mnemonic);
        }
    }
    if (lines.empty())
    {
        return "";
    }
    std::string list;
    const size_t shown = std::min<size_t>(lines.size(), 10);
    for (size_t i = 0; i < shown; ++i)
    {
        list += (i > 0 ? ", " : "") + std::to_string(lines[i]);
    }
    if (lines.size() > shown)
    {
        list += " and " + std::to_string(lines.size() - shown) + " more";
    }
    std::string mnemonics;
    for (size_t i = 0; i < names.size(); ++i)
    {
        mnemonics += (i > 0 ? ", " : "") + names[i];
    }
    const std::string where = (lines.size() == 1 ? "Line " : "Lines ") + list;
    if (kind == Barrier::UnknownMnemonic)
    {
        return where + " (" + mnemonics +
               "): instructions the reordering tool does not know (for example F/D extension, CSR or custom "
               "pseudo-instructions). They stay where they are, and nothing is moved across them.";
    }
    return where + " (" + mnemonics +
           "): operands written in a form the reordering tool does not read reliably (for example "
           "\"lw t0, label\" or \"lw t0, (a0)\"). They stay where they are, and nothing is moved across them.";
}

} // namespace

void buildDiff(Context &ctx, AnalysisResult &result)
{
    Program *p = ctx.program.get();
    const int count = ctx.instructionCount;
    const int lineCount = static_cast<int>(ctx.lines.size());
    const bool noHazardUnit = !ctx.hazardUnit;

    // Segments: each block's line range, and the lines between blocks.
    std::vector<int> lineBlock(static_cast<size_t>(lineCount), -1);
    for (int b = 0; b < p->nblocks; ++b)
    {
        const int first = p->ins[p->blocks[b].start].line_no - 1;
        const int last = p->ins[p->blocks[b].end - 1].line_no - 1;
        for (int line = first; line <= last; ++line)
        {
            lineBlock[line] = b;
        }
    }
    std::vector<int> lineSegment(static_cast<size_t>(lineCount), 0);
    std::vector<int> segmentBlock;
    for (int line = 0; line < lineCount; ++line)
    {
        if (line == 0 || lineBlock[line] != lineBlock[line - 1])
        {
            segmentBlock.push_back(lineBlock[line]);
        }
        lineSegment[line] = static_cast<int>(segmentBlock.size()) - 1;
    }

    // No-hazard-unit pipelines: where added NOPs go.
    std::map<int, std::vector<int>> addBefore;
    std::map<int, std::vector<int>> addAfter;
    if (noHazardUnit)
    {
        for (int k = 0; k < count; ++k)
        {
            const GapNops &gap = ctx.gaps[k];
            if (gap.needed > gap.kept)
            {
                if (gap.insertAfterLine >= 0)
                {
                    addAfter[gap.insertAfterLine].push_back(k);
                }
                else
                {
                    addBefore[gap.insertBeforeLine].push_back(k);
                }
            }
        }
    }

    // ---- right side: the new program --------------------------------------------------
    std::vector<Placed> right;
    std::vector<int> newLineOfInstruction(static_cast<size_t>(count), -1);
    result.lineMap.assign(static_cast<size_t>(lineCount), -1);
    std::vector<int> gapCycleIndex(static_cast<size_t>(count) + 1, 0);
    int newLine = 0;

    auto nopCycle = [&](int gapIndex, bool newOrder) {
        const int index = gapCycleIndex[gapIndex]++;
        if (gapIndex >= count)
        {
            return (newOrder ? ctx.finalSlotRight : ctx.finalSlotStudent) -
                   static_cast<int>(ctx.gaps[count].studentLines.size()) + index + 1;
        }
        if (newOrder)
        {
            const int instruction = ctx.order[gapIndex];
            return ctx.rightStart[instruction] - ctx.gaps[gapIndex].total() + index + 1;
        }
        return ctx.waitStudent[gapIndex].startSlot - static_cast<int>(ctx.gaps[gapIndex].studentLines.size()) +
               index + 1;
    };

    auto addNops = [&](int segment, int slot) {
        const GapNops &gap = ctx.gaps[slot];
        for (int n = gap.kept; n < gap.needed; ++n)
        {
            Placed placed;
            placed.segment = segment;
            DiffCell &cell = placed.cell;
            cell.kind = CellKind::Nop;
            cell.change = CellChange::Added;
            cell.line = ++newLine;
            cell.text = nopText(ctx, slot);
            cell.cycle = nopCycle(slot, true);
            cell.badge = "added";
            cell.tooltipHtml = nopTooltip(ctx, slot, false, true);
            const int consumer = ctx.order[slot];
            cell.related = {consumer};
            if (ctx.waitAfter[consumer].producer >= 0)
            {
                cell.related.push_back(ctx.waitAfter[consumer].producer);
            }
            right.push_back(placed);
        }
    };

    for (int line = 0; line < lineCount; ++line)
    {
        const LineInfo &info = ctx.lines[line];
        const int segment = lineSegment[line];
        if (noHazardUnit)
        {
            const auto it = addBefore.find(line);
            if (it != addBefore.end())
            {
                for (int slot : it->second)
                {
                    addNops(segment, slot);
                }
            }
        }

        if (info.instruction >= 0)
        {
            const int slot = info.instruction;
            const int instruction = ctx.order[slot];
            const Wait &wait = ctx.waitAfter[instruction];
            if (ctx.hazardUnit)
            {
                for (int s = 0; s < wait.stalls; ++s)
                {
                    Placed placed;
                    placed.segment = segment;
                    DiffCell &cell = placed.cell;
                    cell.kind = CellKind::Stall;
                    cell.instruction = instruction;
                    cell.cycle = wait.startSlot - wait.stalls + s + 1;
                    cell.text = "stall — waits for " + registerName(ctx, instruction, wait.reg) + " from line " +
                                std::to_string(originalLineOf(ctx, wait.producer));
                    cell.badge = stallBadge(ctx, instruction, true);
                    cell.tooltipHtml = waitTooltip(ctx, instruction, true);
                    cell.related = {instruction, wait.producer};
                    right.push_back(placed);
                }
            }
            Placed placed;
            placed.segment = segment;
            DiffCell &cell = placed.cell;
            cell.kind = CellKind::Instruction;
            cell.instruction = instruction;
            cell.line = ++newLine;
            cell.sourceLine = originalLineOf(ctx, instruction);
            cell.text = renderInSlot(ctx, instruction, line);
            cell.cycle = (ctx.hazardUnit ? wait.startSlot : ctx.rightStart[instruction]) + 1;
            newLineOfInstruction[instruction] = cell.line;
            const int moveIndex = ctx.moveOf[instruction];
            if (moveIndex >= 0)
            {
                const MoveInfo &move = ctx.moves[moveIndex];
                const int distance = move.toPos - move.fromPos;
                cell.change = distance < 0 ? CellChange::MovedUp : CellChange::MovedDown;
                cell.distance = std::abs(distance);
                cell.badge = arrow(distance) + " from line " + std::to_string(cell.sourceLine);
                cell.tooltipHtml = moveTooltip(ctx, move);
                cell.related = relatedForMove(move);
            }
            else if (wait.stalls > 0)
            {
                cell.badge = ctx.hazardUnit ? "waits " + plural(wait.stalls, "cycle")
                                            : "after " + plural(wait.stalls, "NOP");
                cell.tooltipHtml = waitTooltip(ctx, instruction, true);
                cell.related = {wait.producer};
            }
            else if (ctx.hazardUnit && wait.inner > 0)
            {
                cell.badge = "waits " + plural(wait.inner, "cycle") + " inside";
                cell.tooltipHtml = innerStallTooltip(ctx, instruction);
            }
            else if (ctx.waitBefore[instruction].stalls > 0)
            {
                const Wait &before = ctx.waitBefore[instruction];
                cell.badge = ctx.hazardUnit ? "✓ no wait" : "✓ no NOP needed";
                cell.tooltipHtml = "<b>" + std::string(ctx.hazardUnit ? "No longer waits" : "No NOP needed any more") +
                                   "</b><p>" +
                                   std::string(ctx.hazardUnit ? "Before reordering it waited " + plural(before.stalls, "cycle")
                                                              : "In your order it needed " + plural(before.stalls, "free slot")) +
                                   " for " +
                                   registerName(ctx, instruction, before.reg) + " from line " +
                                   std::to_string(originalLineOf(ctx, before.producer)) + ". " +
                                   fixedNote(ctx, instruction) + "</p>";
                cell.related = {before.producer};
            }
            right.push_back(placed);
        }
        else if (info.strippedNop)
        {
            if (nopIsKept(ctx, line))
            {
                Placed placed;
                placed.segment = segment;
                DiffCell &cell = placed.cell;
                cell.kind = CellKind::Nop;
                cell.line = ++newLine;
                cell.sourceLine = line + 1;
                cell.text = info.text;
                cell.cycle = nopCycle(info.nopGap, true);
                cell.tooltipHtml = nopTooltip(ctx, info.nopGap, true, false);
                if (info.nopGap < count)
                {
                    cell.related = {ctx.order[info.nopGap]};
                }
                result.lineMap[line] = cell.line;
                right.push_back(placed);
            }
            else if (info.labelColon >= 0)
            {
                Placed placed;
                placed.segment = segment;
                DiffCell &cell = placed.cell;
                cell.kind = CellKind::Text;
                cell.change = CellChange::Added;
                cell.line = ++newLine;
                cell.sourceLine = line + 1;
                cell.text = info.text.substr(0, static_cast<size_t>(info.labelColon) + 1);
                if (!info.text.empty() && info.text.back() == '\r')
                {
                    cell.text.push_back('\r');
                }
                result.lineMap[line] = cell.line;
                right.push_back(placed);
            }
        }
        else
        {
            Placed placed;
            placed.segment = segment;
            placed.cell.kind = CellKind::Text;
            placed.cell.line = ++newLine;
            placed.cell.sourceLine = line + 1;
            placed.cell.text = info.text;
            result.lineMap[line] = placed.cell.line;
            right.push_back(placed);
        }

        if (noHazardUnit)
        {
            const auto it = addAfter.find(line);
            if (it != addAfter.end())
            {
                for (int slot : it->second)
                {
                    addNops(segment, slot);
                }
            }
        }
    }
    for (int line = 0; line < lineCount; ++line)
    {
        if (ctx.lines[line].instruction >= 0)
        {
            result.lineMap[line] = newLineOfInstruction[ctx.lines[line].instruction];
        }
    }

    // ---- left side: the original program ------------------------------------------------
    std::vector<Placed> left;
    std::fill(gapCycleIndex.begin(), gapCycleIndex.end(), 0);
    for (int line = 0; line < lineCount; ++line)
    {
        const LineInfo &info = ctx.lines[line];
        const int segment = lineSegment[line];
        if (info.instruction >= 0)
        {
            const int instruction = info.instruction;
            const Wait &wait = ctx.hazardUnit ? ctx.waitBefore[instruction] : ctx.waitStudent[instruction];
            for (int s = 0; s < wait.stalls; ++s)
            {
                Placed placed;
                placed.segment = segment;
                DiffCell &cell = placed.cell;
                cell.instruction = instruction;
                cell.related = {instruction, wait.producer};
                if (ctx.hazardUnit)
                {
                    cell.kind = CellKind::Stall;
                    cell.cycle = wait.startSlot - wait.stalls + s + 1;
                    cell.text = "stall — waits for " + registerName(ctx, instruction, wait.reg) + " from line " +
                                std::to_string(originalLineOf(ctx, wait.producer));
                    cell.badge = stallBadge(ctx, instruction, false);
                    cell.tooltipHtml = waitTooltip(ctx, instruction, false);
                }
                else
                {
                    cell.kind = CellKind::MissingNop;
                    cell.text = "⚠ missing NOP — line " + std::to_string(line + 1) + " reads " +
                                registerName(ctx, instruction, wait.reg) + " too early";
                    cell.badge = "wrong result";
                    cell.tooltipHtml = missingNopTooltip(ctx, instruction);
                }
                left.push_back(placed);
            }
            Placed placed;
            placed.segment = segment;
            DiffCell &cell = placed.cell;
            cell.kind = CellKind::Instruction;
            cell.instruction = instruction;
            cell.line = line + 1;
            cell.sourceLine = line + 1;
            cell.text = info.text;
            cell.cycle = wait.startSlot + 1;
            const int moveIndex = ctx.moveOf[instruction];
            if (moveIndex >= 0)
            {
                const MoveInfo &move = ctx.moves[moveIndex];
                const int distance = move.toPos - move.fromPos;
                cell.change = distance < 0 ? CellChange::MovedUp : CellChange::MovedDown;
                cell.distance = std::abs(distance);
                cell.badge = arrow(distance) + " to line " + std::to_string(newLineOfInstruction[instruction]);
                cell.tooltipHtml = moveTooltip(ctx, move);
                cell.related = relatedForMove(move);
            }
            else if (wait.stalls > 0)
            {
                cell.badge = ctx.hazardUnit ? "waits " + plural(wait.stalls, "cycle") : "⚠ missing NOP";
                cell.tooltipHtml =
                    ctx.hazardUnit ? waitTooltip(ctx, instruction, false) : missingNopTooltip(ctx, instruction);
                cell.related = {wait.producer};
            }
            else if (ctx.hazardUnit && wait.inner > 0)
            {
                cell.badge = "waits " + plural(wait.inner, "cycle") + " inside";
                cell.tooltipHtml = innerStallTooltip(ctx, instruction);
            }
            left.push_back(placed);
        }
        else if (info.strippedNop)
        {
            const bool kept = nopIsKept(ctx, line);
            Placed placed;
            placed.segment = segment;
            DiffCell &cell = placed.cell;
            cell.kind = CellKind::Nop;
            cell.change = kept ? CellChange::None : CellChange::Removed;
            cell.line = line + 1;
            cell.sourceLine = line + 1;
            cell.text = info.text;
            cell.cycle = nopCycle(info.nopGap, false);
            cell.badge = kept ? "" : "not needed";
            cell.tooltipHtml = nopTooltip(ctx, info.nopGap, kept, false);
            if (info.nopGap < count)
            {
                cell.related = {ctx.order[info.nopGap]};
            }
            left.push_back(placed);
        }
        else
        {
            Placed placed;
            placed.segment = segment;
            placed.cell.kind = CellKind::Text;
            placed.cell.line = line + 1;
            placed.cell.sourceLine = line + 1;
            placed.cell.text = info.text;
            left.push_back(placed);
        }
    }

    // ---- segments ---------------------------------------------------------------------
    result.segments.assign(segmentBlock.size(), DiffSegment{});
    for (size_t s = 0; s < segmentBlock.size(); ++s)
    {
        result.segments[s].block = segmentBlock[s];
    }
    for (int line = 0; line < lineCount; ++line)
    {
        DiffSegment &segment = result.segments[lineSegment[line]];
        if (segment.firstLine == 0)
        {
            segment.firstLine = line + 1;
        }
        segment.lastLine = line + 1;
    }
    for (const Placed &placed : left)
    {
        result.segments[placed.segment].left.push_back(placed.cell);
    }
    for (const Placed &placed : right)
    {
        result.segments[placed.segment].right.push_back(placed.cell);
    }
    for (DiffSegment &segment : result.segments)
    {
        bool anyChange = false;
        for (const DiffCell &cell : segment.left)
        {
            anyChange = anyChange || cell.change != CellChange::None;
        }
        for (const DiffCell &cell : segment.right)
        {
            anyChange = anyChange || cell.change != CellChange::None;
        }
        segment.changed = anyChange || signature(segment.left) != signature(segment.right);

        if (segment.block >= 0)
        {
            const int b = segment.block;
            const int start = p->blocks[b].start;
            const int end = p->blocks[b].end;
            int missing = 0;
            for (int k = start; k < end; ++k)
            {
                if (ctx.hazardUnit)
                {
                    segment.stallsBefore += ctx.waitBefore[k].stalls + ctx.waitBefore[k].inner;
                    segment.stallsAfter += ctx.waitAfter[k].stalls + ctx.waitAfter[k].inner;
                }
                else
                {
                    segment.stallsBefore += static_cast<int>(ctx.gaps[k].studentLines.size());
                    segment.stallsAfter += ctx.gaps[k].total();
                    missing += ctx.waitStudent[k].stalls;
                }
            }
            segment.title = "Block " + std::to_string(b + 1) + " · lines " +
                            std::to_string(originalLineOf(ctx, start)) + "–" +
                            std::to_string(originalLineOf(ctx, end - 1)) + " · " +
                            (ctx.hazardUnit ? "stalls " : "NOPs ") + std::to_string(segment.stallsBefore) + " → " +
                            std::to_string(segment.stallsAfter);
            if (missing > 0)
            {
                segment.title += " · ⚠ " + std::to_string(missing) + " missing";
            }
            segment.tooltipHtml = blockDecisionHtml(ctx, b);
        }
        segment.unified = unifiedRows(segment);
    }

    // ---- new text and totals ------------------------------------------------------------
    std::string text;
    bool first = true;
    int nopsAfter = 0;
    for (const Placed &placed : right)
    {
        if (isGhost(placed.cell))
        {
            continue;
        }
        if (!first)
        {
            text += '\n';
        }
        text += placed.cell.text;
        first = false;
        nopsAfter += placed.cell.kind == CellKind::Nop ? 1 : 0;
    }
    if (ctx.endsWithNewline)
    {
        text += '\n';
    }
    result.newText = text;
    result.changed = result.newText != result.originalText;

    result.instructionCount = count;
    result.blockCount = p->nblocks;
    for (int i = 0; i < count; ++i)
    {
        // stalls inside la / big li / call (no forwarding) are real cycles too, as in the tool's totals
        result.stallsBefore += ctx.waitBefore[i].stalls + ctx.waitBefore[i].inner;
        result.stallsAfter += ctx.waitAfter[i].stalls + ctx.waitAfter[i].inner;
        if (noHazardUnit)
        {
            result.missingNops += ctx.waitStudent[i].stalls;
        }
    }
    if (noHazardUnit)
    {
        int nopsBefore = 0;
        for (const GapNops &gap : ctx.gaps)
        {
            nopsBefore += static_cast<int>(gap.studentLines.size());
        }
        result.nopsBefore = nopsBefore;
        result.nopsAfter = nopsAfter;
        result.cyclesBefore = count > 0 ? ctx.finalSlotStudent + 4 : 0;
        result.cyclesAfter = count > 0 ? ctx.finalSlotRight + 4 : 0;
    }
    else
    {
        result.cyclesBefore = count > 0 ? ctx.finalSlotBefore + 4 : 0;
        result.cyclesAfter = count > 0 ? ctx.finalSlotAfter + 4 : 0;
    }

    for (const MoveInfo &move : ctx.moves)
    {
        MoveSummary summary;
        summary.instruction = move.instruction;
        summary.fromLine = originalLineOf(ctx, move.instruction);
        summary.toLine = newLineOfInstruction[move.instruction];
        summary.distance = move.toPos - move.fromPos;
        summary.shortText = moveShortText(ctx, move);
        summary.detailHtml = moveTooltip(ctx, move);
        result.moves.push_back(summary);
    }
    result.movedCount = static_cast<int>(result.moves.size());

    bool anyBarrier = false;
    bool pcRelative = false;
    bool expansionHazard = false;
    for (int i = 0; i < count; ++i)
    {
        anyBarrier = anyBarrier || ctx.barrier[i] == Barrier::UnknownMnemonic ||
                     ctx.barrier[i] == Barrier::UnclearOperands || ctx.barrier[i] == Barrier::PcRelative;
        pcRelative = pcRelative || ctx.barrier[i] == Barrier::PcRelative;
        expansionHazard = expansionHazard || p->ins[i].size >= 2;
    }
    if (pcRelative)
    {
        result.notes.push_back("auipc depends on its own address, so it stays in place. Adding or removing lines "
                               "before it still changes the value it computes.");
    }
    if (noHazardUnit && anyBarrier)
    {
        result.notes.push_back("Hazards through lines the tool does not analyze are not checked. Your NOPs next to "
                               "them are kept, and the instructions next to them stay in place.");
    }
    if (noHazardUnit && !ctx.forwarding && expansionHazard)
    {
        result.notes.push_back("Kites expands la, li with a large value, call and \"lw rd, label\" into two "
                               "dependent instructions. On a pipeline without forwarding and without a hazard unit "
                               "the second one reads the old value whatever the order, so these lines give a wrong "
                               "result on this processor. Reordering cannot fix that.");
    }
    for (Barrier kind : {Barrier::UnknownMnemonic, Barrier::UnclearOperands})
    {
        const std::string note = notesForBarriers(ctx, kind);
        if (!note.empty())
        {
            result.notes.push_back(note);
        }
    }
    for (const std::string &note : ctx.notes)
    {
        result.notes.push_back(note);
    }
    if (count == 0)
    {
        result.notes.push_back("There are no instructions to analyze.");
    }
    result.overviewHtml = overviewHtml(ctx, result);
}

} // namespace detail

std::string makeUnifiedPatch(const AnalysisResult &result, const std::string &fileName)
{
    struct PatchLine
    {
        char sign;
        std::string text;
        int oldLine;
        int newLine;
    };
    std::vector<PatchLine> rows;
    for (const DiffSegment &segment : result.segments)
    {
        for (const UnifiedRow &row : segment.unified)
        {
            if (row.cell.kind == CellKind::Stall || row.cell.kind == CellKind::MissingNop)
            {
                continue;
            }
            const char sign = row.sign == UnifiedSign::Removed ? '-' : row.sign == UnifiedSign::Added ? '+' : ' ';
            rows.push_back({sign, row.cell.text, row.oldLine, row.newLine});
        }
    }

    std::vector<int> changes;
    for (size_t i = 0; i < rows.size(); ++i)
    {
        if (rows[i].sign != ' ')
        {
            changes.push_back(static_cast<int>(i));
        }
    }
    if (changes.empty())
    {
        return "";
    }

    auto lineCountOf = [](const std::string &text) {
        if (text.empty())
        {
            return 0;
        }
        int lines = static_cast<int>(std::count(text.begin(), text.end(), '\n'));
        return text.back() == '\n' ? lines : lines + 1;
    };
    const int oldLast = lineCountOf(result.originalText);
    const int newLast = lineCountOf(result.newText);
    const bool oldNewline = !result.originalText.empty() && result.originalText.back() == '\n';
    const bool newNewline = !result.newText.empty() && result.newText.back() == '\n';

    std::ostringstream out;
    out << "--- a/" << fileName << "\n+++ b/" << fileName << "\n";
    const int context = 3;
    size_t c = 0;
    while (c < changes.size())
    {
        const int hunkStart = std::max(0, changes[c] - context);
        int lastChange = changes[c];
        size_t d = c;
        while (d + 1 < changes.size() && changes[d + 1] - lastChange <= 2 * context)
        {
            lastChange = changes[++d];
        }
        const int hunkStop = std::min(static_cast<int>(rows.size()) - 1, lastChange + context);

        int oldStart = -1;
        int newStart = -1;
        int oldCount = 0;
        int newCount = 0;
        for (int i = hunkStart; i <= hunkStop; ++i)
        {
            if (rows[i].sign != '+')
            {
                ++oldCount;
                if (oldStart < 0)
                {
                    oldStart = rows[i].oldLine;
                }
            }
            if (rows[i].sign != '-')
            {
                ++newCount;
                if (newStart < 0)
                {
                    newStart = rows[i].newLine;
                }
            }
        }
        // An empty side starts at the line before the hunk (unified diff convention).
        auto lineBefore = [&](bool oldSide) {
            for (int i = hunkStart - 1; i >= 0; --i)
            {
                const int line = oldSide ? rows[i].oldLine : rows[i].newLine;
                if (line > 0)
                {
                    return line;
                }
            }
            return 0;
        };
        if (oldStart < 0)
        {
            oldStart = lineBefore(true);
        }
        if (newStart < 0)
        {
            newStart = lineBefore(false);
        }
        out << "@@ -" << oldStart << "," << oldCount << " +" << newStart << "," << newCount << " @@\n";
        for (int i = hunkStart; i <= hunkStop; ++i)
        {
            const PatchLine &row = rows[i];
            const bool oldEnd = row.sign != '+' && row.oldLine == oldLast && !oldNewline;
            const bool newEnd = row.sign != '-' && row.newLine == newLast && !newNewline;
            if (row.sign == ' ' && oldEnd != newEnd)
            {
                // the same text, but the last line on only one side: not the same line
                out << '-' << row.text << "\n";
                if (oldEnd)
                {
                    out << "\\ No newline at end of file\n";
                }
                out << '+' << row.text << "\n";
                if (newEnd)
                {
                    out << "\\ No newline at end of file\n";
                }
                continue;
            }
            out << row.sign << row.text << "\n";
            if (oldEnd || newEnd)
            {
                out << "\\ No newline at end of file\n";
            }
        }
        c = d + 1;
    }
    return out.str();
}

} // namespace reorder
} // namespace Kites
