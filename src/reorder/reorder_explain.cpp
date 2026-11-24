/**
 * @file reorder_explain.cpp
 * @brief Fixed sentence templates that turn the scheduler's facts into explanations.
 *
 * Nothing here is generated freely: every sentence is a template whose blanks are
 * filled with facts computed in reorder_analysis.cpp (dependence edges, which
 * instruction waits for which register, what happens if a move is undone). All
 * wording lives in this file so it can be reviewed and changed in one place.
 */
#include "reorder_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace Kites
{
namespace reorder
{
namespace detail
{

std::string escapeHtml(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
    {
        switch (c)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::string instructionCode(const Context &ctx, int instruction)
{
    return escapeHtml(ctx.program->ins[instruction].code);
}

std::string registerName(const Context &ctx, int instruction, int reg)
{
    if (reg < 0 || reg >= NREGS)
    {
        return "?";
    }
    char code[MAX_TEXT];
    std::strncpy(code, ctx.program->ins[instruction].code, MAX_TEXT - 1);
    code[MAX_TEXT - 1] = '\0';
    char words[MAX_WORDS][MAX_WORD];
    const int count = split_words(code, words);
    for (int k = 1; k < count; ++k)
    {
        if (reg_number(words[k]) == reg)
        {
            return escapeHtml(words[k]); // the spelling the student used (t0 or x5)
        }
    }
    char name[MAX_WORD];
    reg_name(reg, name); // x and f registers (0..31, 32..63)
    return name;
}

namespace
{

std::string plural(int count, const std::string &word)
{
    return std::to_string(count) + " " + word + (count == 1 ? "" : "s");
}

std::string code(const Context &ctx, int instruction)
{
    return "<code>" + instructionCode(ctx, instruction) + "</code>";
}

std::string lineOf(const Context &ctx, int instruction)
{
    return "line " + std::to_string(originalLineOf(ctx, instruction));
}

std::string capitalized(std::string text)
{
    if (!text.empty() && text[0] >= 'a' && text[0] <= 'z')
    {
        text[0] = static_cast<char>(text[0] - 'a' + 'A');
    }
    return text;
}

std::string joinList(const std::vector<std::string> &items)
{
    std::string out;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i > 0)
        {
            out += i + 1 == items.size() ? " and " : ", ";
        }
        out += items[i];
    }
    return out;
}

std::string lineList(const Context &ctx, std::vector<int> instructions)
{
    std::sort(instructions.begin(), instructions.end());
    std::vector<std::string> numbers;
    const size_t shown = std::min<size_t>(instructions.size(), 8);
    for (size_t i = 0; i < shown; ++i)
    {
        numbers.push_back(std::to_string(originalLineOf(ctx, instructions[i])));
    }
    if (instructions.size() > shown)
    {
        numbers.push_back(std::to_string(instructions.size() - shown) + " more");
    }
    return (instructions.size() == 1 ? "line " : "lines ") + joinList(numbers);
}

bool isLoad(const Context &ctx, int instruction)
{
    return ctx.program->ins[instruction].cls == CLASS_LOAD;
}

bool reads(const Context &ctx, int instruction, int reg)
{
    return reads_reg(&ctx.program->ins[instruction], reg) == 1;
}

std::string memoryOperand(const Context &ctx, int instruction)
{
    char text[MAX_TEXT];
    std::strncpy(text, ctx.program->ins[instruction].code, MAX_TEXT - 1);
    text[MAX_TEXT - 1] = '\0';
    char words[MAX_WORDS][MAX_WORD];
    if (split_words(text, words) >= 4)
    {
        return "<code>" + escapeHtml(words[2]) + "(" + escapeHtml(words[3]) + ")</code>";
    }
    return "memory";
}

std::string stallWord(const Context &ctx)
{
    return ctx.hazardUnit ? "stall" : "NOP";
}

std::string windowWord(const Context &ctx)
{
    return ctx.options.slidingWindows ? "sliding windows" : "fixed windows";
}

/// Why `later` has to stay after `earlier` (there is a dependence edge between them).
std::string dependence(const Context &ctx, int earlier, int later)
{
    const Instr &a = ctx.program->ins[earlier];
    const Instr &b = ctx.program->ins[later];
    if (a.rd > 0 && reads(ctx, later, a.rd))
    {
        return lineOf(ctx, later) + " reads " + registerName(ctx, later, a.rd) + ", which " + lineOf(ctx, earlier) +
               " writes";
    }
    if (b.rd > 0 && reads(ctx, earlier, b.rd))
    {
        return lineOf(ctx, later) + " overwrites " + registerName(ctx, earlier, b.rd) + ", which " +
               lineOf(ctx, earlier) + " still has to read";
    }
    if (a.rd > 0 && a.rd == b.rd)
    {
        return "both write " + registerName(ctx, earlier, a.rd) + ", and the value of " + lineOf(ctx, later) +
               " must be the one that is left";
    }
    if (is_memory(const_cast<Instr *>(&a)) && is_memory(const_cast<Instr *>(&b)))
    {
        return lineOf(ctx, earlier) + " and " + lineOf(ctx, later) + " may access the same memory (" +
               memoryOperand(ctx, earlier) + " and " + memoryOperand(ctx, later) + "), and one of them is a store";
    }
    if (b.ends_block == 1)
    {
        return lineOf(ctx, later) + " ends the block, so everything else stays in front of it";
    }
    return lineOf(ctx, later) + " depends on " + lineOf(ctx, earlier);
}

/// "in the instruction just before it" / "with only 1 instruction in between"
std::string distanceText(int distance)
{
    if (distance <= 1)
    {
        return "in the instruction just before it";
    }
    return "with only " + plural(distance - 1, "instruction") + " in between";
}

std::string causeSentence(const Context &ctx, const Wait &wait, const std::string &reg)
{
    std::string text;
    if (ctx.forwarding)
    {
        text = "With forwarding, an ALU result can go straight to the next instruction, but a loaded value "
               "only exists at the end of the MEM stage, so the instruction right after a load has to wait one "
               "cycle (a load-use hazard).";
    }
    else
    {
        text = "Without forwarding, " + reg +
               " only reaches the register file in WB (it is written in the first half of that cycle and read "
               "in the second half of ID), so the reader must enter the pipeline at least 3 cycles after the "
               "instruction that produces it.";
    }
    if (ctx.hazardUnit)
    {
        text += " The hazard detection unit holds it back for " + plural(wait.stalls, "cycle") + ".";
    }
    else
    {
        text += " This processor has no hazard detection unit, so the code itself has to leave " +
                plural(wait.stalls, "free slot") + " here (NOPs or other instructions); otherwise it reads the old "
                "value of " + reg + ".";
    }
    return text;
}

std::string hazardKind(const Context &ctx)
{
    return ctx.forwarding ? "load-use" : "no forwarding";
}

} // namespace

std::string fixedNote(const Context &ctx, int consumer)
{
    const int producer = ctx.waitBefore[consumer].producer;
    if (producer < 0)
    {
        return "";
    }
    std::vector<int> fillers;
    for (int slot = ctx.slotOf[producer] + 1; slot < ctx.slotOf[consumer]; ++slot)
    {
        const int other = ctx.order[slot];
        if (!(producer < other && other < consumer))
        {
            fillers.push_back(other);
        }
    }
    if (!fillers.empty())
    {
        return capitalized(lineList(ctx, fillers)) + (fillers.size() == 1 ? " now sits" : " now sit") + " between " +
               lineOf(ctx, producer) + " and " + lineOf(ctx, consumer) + ".";
    }
    if (ctx.moveOf[consumer] >= 0)
    {
        return capitalized(lineOf(ctx, consumer)) + " now comes later.";
    }
    if (ctx.moveOf[producer] >= 0)
    {
        return capitalized(lineOf(ctx, producer)) + " now comes earlier.";
    }
    return "The instruction it waits for is now further away.";
}

std::string remainingNote(const Context &ctx, int consumer)
{
    const Wait &wait = ctx.waitAfter[consumer];
    const int producer = wait.producer;
    if (wait.stalls <= 0 || producer < 0)
    {
        return "";
    }
    if (ctx.blockOf[producer] != ctx.blockOf[consumer])
    {
        for (int between = producer + 1; between < consumer; ++between)
        {
            if (ctx.barrier[between] != Barrier::None)
            {
                return "Its value comes from " + lineOf(ctx, producer) + ", and " + lineOf(ctx, between) +
                       " between them stays in place (the tool does not analyze it or keeps it fixed), so "
                       "reordering cannot remove this wait.";
            }
        }
        return "Its value comes from " + lineOf(ctx, producer) +
               " in an earlier block. Instructions are never moved past a label or a branch, so reordering "
               "cannot remove this wait.";
    }
    const int block = ctx.blockOf[consumer];
    const int start = blockStart(ctx, block);
    const int end = blockEnd(ctx, block);
    const int window = ctx.options.windowSize;
    if (!ctx.options.slidingWindows && (producer - start) / window != (consumer - start) / window)
    {
        return capitalized(lineOf(ctx, producer)) + " is in an earlier fixed window of " + std::to_string(window) +
               " instructions, and instructions only move inside their window. Sliding windows or a larger "
               "window may help.";
    }

    // Instructions that must come before the producer / after the consumer (whole chains).
    const int count = end - start;
    std::vector<bool> beforeProducer(static_cast<size_t>(count), false);
    std::vector<bool> afterConsumer(static_cast<size_t>(count), false);
    beforeProducer[producer - start] = true;
    for (int v = producer; v >= start; --v)
    {
        for (int w = v + 1; w <= producer && !beforeProducer[v - start]; ++w)
        {
            beforeProducer[v - start] = beforeProducer[w - start] && dependsOn(ctx, v, w);
        }
    }
    afterConsumer[consumer - start] = true;
    for (int v = consumer; v < end; ++v)
    {
        for (int u = consumer; u < v && !afterConsumer[v - start]; ++u)
        {
            afterConsumer[v - start] = afterConsumer[u - start] && dependsOn(ctx, u, v);
        }
    }
    std::vector<int> candidates;
    for (int v = start; v < end; ++v)
    {
        if (v == producer || v == consumer || beforeProducer[v - start] || afterConsumer[v - start])
        {
            continue;
        }
        const int slot = ctx.slotOf[v];
        const bool alreadyBetween = ctx.slotOf[producer] < slot && slot < ctx.slotOf[consumer];
        if (!alreadyBetween)
        {
            candidates.push_back(v);
        }
    }
    if (candidates.empty())
    {
        return "No other instruction in this block can go between " + lineOf(ctx, producer) + " and " +
               lineOf(ctx, consumer) + " without breaking a dependence, so reordering cannot remove this wait.";
    }
    return capitalized(lineList(ctx, candidates)) + " could be placed between them without breaking a "
           "dependence, but the scheduler's rules did not put " + (candidates.size() == 1 ? "it" : "them") +
           " there. (They are simple greedy rules inside windows of " + std::to_string(window) +
           " instructions, so they do not always find the best order.)";
}

std::string waitTooltip(const Context &ctx, int consumer, bool newOrder)
{
    const Wait &wait = newOrder ? ctx.waitAfter[consumer] : ctx.waitBefore[consumer];
    if (wait.stalls <= 0 || wait.producer < 0)
    {
        return "";
    }
    const int producer = wait.producer;
    const std::string reg = registerName(ctx, consumer, wait.reg);
    const int distance = newOrder ? ctx.slotOf[consumer] - ctx.slotOf[producer] : consumer - producer;

    std::string html;
    if (ctx.hazardUnit)
    {
        html += "<b>" + capitalized(lineOf(ctx, consumer)) + " waits " + plural(wait.stalls, "cycle") + " for " +
                reg + "</b>";
    }
    else
    {
        html += "<b>" + capitalized(lineOf(ctx, consumer)) + " needs " + plural(wait.stalls, "free slot") +
                " before it</b>";
    }
    html += "<p>" + code(ctx, consumer) + " reads <b>" + reg + "</b>, which " + code(ctx, producer) + " (" +
            lineOf(ctx, producer) + ") " + (isLoad(ctx, producer) ? "loads " : "writes ") + distanceText(distance) +
            ".</p>";
    html += "<p>" + causeSentence(ctx, wait, reg) + "</p>";

    if (!newOrder)
    {
        const Wait &after = ctx.waitAfter[consumer];
        if (after.stalls == 0)
        {
            html += "<p><b>After reordering:</b> it does not wait any more. " + fixedNote(ctx, consumer) + "</p>";
        }
        else if (after.stalls < wait.stalls)
        {
            html += "<p><b>After reordering:</b> it waits only " + plural(after.stalls, "cycle") + ". " +
                    fixedNote(ctx, consumer) + "</p>";
        }
        else if (after.stalls > wait.stalls)
        {
            html += "<p><b>After reordering:</b> it waits longer, " + plural(after.stalls, "cycle") +
                    ", but the program as a whole has fewer stalls. " + remainingNote(ctx, consumer) + "</p>";
        }
        else
        {
            html += "<p><b>After reordering:</b> it still waits. " + remainingNote(ctx, consumer) + "</p>";
        }
    }
    else
    {
        const std::string note = remainingNote(ctx, consumer);
        if (!note.empty())
        {
            html += "<p><b>Why it is not removed:</b> " + note + "</p>";
        }
    }
    return html;
}

std::string missingNopTooltip(const Context &ctx, int consumer)
{
    const Wait &wait = ctx.waitStudent[consumer];
    if (wait.stalls <= 0 || wait.producer < 0)
    {
        return "";
    }
    const int producer = wait.producer;
    const std::string reg = registerName(ctx, consumer, wait.reg);
    std::string html = "<b>Missing NOP before " + lineOf(ctx, consumer) + "</b>";
    html += "<p>" + code(ctx, consumer) + " reads <b>" + reg + "</b>, which " + code(ctx, producer) + " (" +
            lineOf(ctx, producer) + ") " + (isLoad(ctx, producer) ? "loads" : "writes") + " too close before it: " +
            plural(wait.stalls, "more free slot") + " " + (wait.stalls == 1 ? "is" : "are") + " needed.</p>";
    html += "<p>This processor has no hazard detection unit, so " + lineOf(ctx, consumer) + " reads the <b>old</b> "
            "value of " + reg + " and the program computes a wrong result.</p>";
    html += "<p><b>In the suggestion:</b> this hazard is covered";
    if (ctx.waitAfter[consumer].stalls > 0)
    {
        html += " by NOPs.</p>";
    }
    else if (ctx.moveOf[consumer] >= 0 || ctx.slotOf[consumer] - ctx.slotOf[producer] > consumer - producer)
    {
        html += " by reordering.</p>";
    }
    else
    {
        html += ".</p>";
    }
    return html;
}

std::string innerStallTooltip(const Context &ctx, int instruction)
{
    const int inner = ctx.waitAfter[instruction].inner;
    return "<b>" + capitalized(lineOf(ctx, instruction)) + " waits " + plural(inner, "cycle") +
           " inside itself</b><p>" + code(ctx, instruction) +
           " is a pseudo-instruction that Kites expands into two instructions (la = auipc + addi, a large li = "
           "lui + addi, call = auipc + jalr). The second one reads the register the first one writes, and without "
           "forwarding it has to wait for it. Reordering cannot change this, but the cycles are real, so they are "
           "counted.</p>";
}

std::string moveShortText(const Context &ctx, const MoveInfo &move)
{
    const int distance = move.toPos - move.fromPos;
    std::string text = capitalized(lineOf(ctx, move.instruction)) + (distance < 0 ? " ↑" : " ↓") +
                       std::to_string(std::abs(distance)) + ": ";
    switch (move.reason)
    {
    case MoveReason::FillsGap:
        text += "fills the gap before " + lineOf(ctx, move.gapConsumer);
        break;
    case MoveReason::Separates:
        text += "waits less for " + lineOf(ctx, move.gapProducer);
        break;
    case MoveReason::FollowsDependency:
        text += move.pinnedBy >= 0 ? "moves together with " + lineOf(ctx, move.pinnedBy) : "follows a dependence";
        break;
    case MoveReason::Indirect:
        text += "removes " + plural(move.saving, stallWord(ctx));
        break;
    case MoveReason::Neutral:
        text += "side effect of the scheduling rule";
        break;
    case MoveReason::KeepsRelativePlace:
        text += "other lines moved past it";
        break;
    }
    return text;
}

std::string moveTooltip(const Context &ctx, const MoveInfo &move)
{
    const int instruction = move.instruction;
    const int distance = move.toPos - move.fromPos;
    const int start = blockStart(ctx, move.block);
    const int end = blockEnd(ctx, move.block);
    const int slot = ctx.slotOf[instruction];
    const int before = slot - 1 >= start ? ctx.order[slot - 1] : -1;
    const int after = slot + 1 < end ? ctx.order[slot + 1] : -1;

    std::string html = "<b>" + std::string(distance < 0 ? "Moved up " : "Moved down ") +
                       std::to_string(std::abs(distance)) + "</b> &nbsp;" + code(ctx, instruction) + " (" +
                       lineOf(ctx, instruction) + ")<br>";
    if (before >= 0 && after >= 0)
    {
        html += "Now between " + lineOf(ctx, before) + " and " + lineOf(ctx, after) + ".";
    }
    else if (before >= 0)
    {
        html += "Now after " + lineOf(ctx, before) + ", at the end of its block.";
    }
    else if (after >= 0)
    {
        html += "Now at the start of its block, before " + lineOf(ctx, after) + ".";
    }

    std::string why;
    switch (move.reason)
    {
    case MoveReason::FillsGap:
    {
        const int producer = move.gapProducer;
        const int consumer = move.gapConsumer;
        const std::string reg = registerName(ctx, consumer, ctx.program->ins[producer].rd);
        why = "It fills the gap between " + code(ctx, producer) + " (" + lineOf(ctx, producer) + ") and " +
              code(ctx, consumer) + " (" + lineOf(ctx, consumer) + "), which needs " + reg + " (" +
              hazardKind(ctx) + ").";
        std::vector<int> others;
        for (int fixed : move.fixedConsumers)
        {
            if (fixed != consumer)
            {
                others.push_back(fixed);
            }
        }
        if (!others.empty())
        {
            why += " It also shortens the wait of " + lineList(ctx, others) + ".";
        }
        break;
    }
    case MoveReason::Separates:
    {
        const int producer = move.gapProducer;
        const std::string reg =
            producer >= 0 ? registerName(ctx, instruction, ctx.program->ins[producer].rd) : std::string("a value");
        why = "It needs " + reg + (producer >= 0 ? " from " + code(ctx, producer) + " (" + lineOf(ctx, producer) + ")"
                                                 : std::string()) +
              ". Moving it later gives that value time to be ready, so it waits less.";
        break;
    }
    case MoveReason::FollowsDependency:
        if (move.pinnedBy >= 0)
        {
            why = "It cannot go back to its old place: it would have to pass " + lineOf(ctx, move.pinnedBy) + ", but " +
                  (move.pinnedBefore ? dependence(ctx, instruction, move.pinnedBy)
                                     : dependence(ctx, move.pinnedBy, instruction)) +
                  ". So it moved together with " + lineOf(ctx, move.pinnedBy) + ".";
        }
        else
        {
            why = "It cannot go back to its old place without breaking a dependence.";
        }
        break;
    case MoveReason::Indirect:
        why = "Putting it back on its own would add " + plural(move.saving, stallWord(ctx)) +
              (move.gapConsumer >= 0 ? " (at " + lineOf(ctx, move.gapConsumer) + ")" : std::string()) + ".";
        break;
    case MoveReason::Neutral:
        if (move.saving < 0)
        {
            why = "On its own this move adds " + plural(-move.saving, stallWord(ctx)) + ". It is part of the "
                  "order the scheduler chose for this block, which as a whole needs the fewest " + stallWord(ctx) +
                  "s it found.";
        }
        else
        {
            why = "This move does not change the number of " + stallWord(ctx) +
                  "s by itself: putting it back gives the same result. It is a side effect of the scheduling "
                  "rule that ordered this part of the block.";
        }
        break;
    case MoveReason::KeepsRelativePlace:
        why = "It keeps its place relative to the instructions around it; other lines moved past it.";
        break;
    }
    html += "<p><b>Why:</b> " + why + "</p>";

    if (move.limit >= 0)
    {
        html += "<p><b>Limit:</b> it cannot go " + std::string(move.limitAbove ? "above " : "below ") +
                lineOf(ctx, move.limit) + ": " +
                (move.limitAbove ? dependence(ctx, move.limit, instruction) : dependence(ctx, instruction, move.limit)) +
                ".</p>";
    }
    if (move.saving > 0 && move.reason != MoveReason::Neutral)
    {
        html += "<p><b>Effect:</b> saves " + plural(move.saving, stallWord(ctx)) + ".</p>";
    }
    return html;
}

std::string nopTooltip(const Context &ctx, int slot, bool kept, bool added)
{
    if (slot >= ctx.instructionCount)
    {
        return "<b>NOP after the last instruction</b><p>It is not about a data hazard, so it stays as it is.</p>";
    }
    const int consumer = ctx.order[slot];
    const Wait &wait = ctx.waitAfter[consumer];
    std::string reason;
    if (wait.stalls > 0 && wait.producer >= 0)
    {
        const std::string reg = registerName(ctx, consumer, wait.reg);
        reason = code(ctx, consumer) + " (" + lineOf(ctx, consumer) + ") reads <b>" + reg + "</b>, which " +
                 code(ctx, wait.producer) + " (" + lineOf(ctx, wait.producer) + ") " +
                 (isLoad(ctx, wait.producer) ? "loads" : "writes") + " too close before it (" + hazardKind(ctx) +
                 "). This processor has no hazard detection unit, so without " +
                 (wait.stalls == 1 ? "this NOP" : "these NOPs") + " it would read the old value.";
    }
    if (added)
    {
        return "<b>Added NOP</b><p>" + reason + "</p>";
    }
    if (kept)
    {
        return "<b>Your NOP is still needed</b><p>" + reason + "</p>";
    }
    const GapNops &gap = ctx.gaps[slot];
    std::string html = "<b>NOP no longer needed</b><p>";
    if (gap.needed == 0)
    {
        html += "In the new order " + code(ctx, consumer) + " (" + lineOf(ctx, consumer) +
                ") does not wait for any result here.";
    }
    else
    {
        html += "In the new order only " + plural(gap.needed, "NOP") + (gap.needed == 1 ? " is" : " are") +
                " needed before " + code(ctx, consumer) + " (" + lineOf(ctx, consumer) + ").";
    }
    if (static_cast<int>(gap.studentLines.size()) > ctx.waitBefore[slot].stalls)
    {
        html += " Not every NOP here was needed in your order either.";
    }
    return html + "</p>";
}

std::string blockDecisionHtml(const Context &ctx, int block)
{
    const int start = blockStart(ctx, block);
    const int end = blockEnd(ctx, block);
    const std::string word = stallWord(ctx);
    int before = 0;
    int after = 0;
    for (int i = start; i < end; ++i)
    {
        before += ctx.waitBefore[i].stalls;
        after += ctx.waitAfter[i].stalls;
    }
    // the next block's first lines: stalls this block's end causes there (cross-block)
    int nextBefore = 0;
    int nextAfter = 0;
    for (int i = end; i < std::min(end + lookahead_lines(const_cast<HazardModel *>(&ctx.model)), ctx.instructionCount);
         ++i)
    {
        if (ctx.waitBefore[i].producer >= start && ctx.waitBefore[i].producer < end)
        {
            nextBefore += ctx.waitBefore[i].stalls;
        }
        if (ctx.waitAfter[i].producer >= start && ctx.waitAfter[i].producer < end)
        {
            nextAfter += ctx.waitAfter[i].stalls;
        }
    }

    std::string html = "<b>Block " + std::to_string(block + 1) + "</b> (lines " +
                       std::to_string(originalLineOf(ctx, start)) + "–" +
                       std::to_string(originalLineOf(ctx, end - 1)) + ")";
    html += "<p>" + std::string(ctx.hazardUnit ? "As written: " : "As written (not counting your NOPs): ") +
            plural(before, word) + "; reordered: " + plural(after, word) + ".";
    if (nextBefore > 0 || nextAfter > 0)
    {
        html += " At the start of the next block, because of this block's last lines: " + plural(nextBefore, word) +
                " → " + plural(nextAfter, word) + ".";
    }
    html += "</p><p><i>How the order is chosen: the block is scheduled in " + windowWord(ctx) + " of " +
            std::to_string(ctx.options.windowSize) +
            " instructions. In each window the scheduler tries the order as written, the longest-chain rule and the "
            "most-dependents rule";
    if (ctx.options.crossBlock)
    {
        html += " (each also with the cross-block precaution: producers that the next block reads early go first). "
                "It keeps the order with the fewest " + word +
                "s in the block, then the fewest at the start of the next block, then the one that moves lines the "
                "least.</i></p>";
    }
    else
    {
        html += ". It keeps the order with the fewest " + word +
                "s in the block, then the one that moves lines the least (the cross-block precaution is off).</i></p>";
    }
    return html;
}

std::string stallBadge(const Context &ctx, int consumer, bool newOrder)
{
    const Wait &wait = newOrder ? ctx.waitAfter[consumer] : ctx.waitBefore[consumer];
    if (wait.reg < 0)
    {
        return hazardKind(ctx);
    }
    return hazardKind(ctx) + " · " + registerName(ctx, consumer, wait.reg);
}

std::string overviewHtml(const Context &ctx, const AnalysisResult &result)
{
    std::string html = "<p><b>Pipeline:</b> " + escapeHtml(variantDescription(ctx.options.variant)) +
                       " &nbsp;·&nbsp; <b>Window:</b> " + std::to_string(ctx.options.windowSize) +
                       " instructions, " + windowWord(ctx) + "</p>";
    if (ctx.hazardUnit)
    {
        html += "<p><b>Stalls:</b> " + std::to_string(result.stallsBefore) + " → " +
                std::to_string(result.stallsAfter);
    }
    else
    {
        html += "<p><b>NOPs:</b> " + std::to_string(result.nopsBefore) + " → " +
                std::to_string(result.nopsAfter);
    }
    html += " &nbsp;·&nbsp; <b>Cycles:</b> " + std::to_string(result.cyclesBefore) + " → " +
            std::to_string(result.cyclesAfter) +
            "<br><i>Static estimate: every instruction counted once (loops run more often).</i></p>";
    if (!ctx.hazardUnit && result.missingNops > 0)
    {
        html += "<p><b>Warning:</b> your code is missing " + plural(result.missingNops, "NOP") +
                ". On this processor those instructions read old register values (rows marked ⚠). The "
                "suggestion fixes this.</p>";
    }

    if (!result.changed)
    {
        html += "<p>No change found: the scheduler found no order with fewer " + stallWord(ctx) + "s.</p>";
    }

    if (!ctx.moves.empty())
    {
        html += "<p><b>Moved lines</b></p><ul>";
        const size_t shown = std::min<size_t>(ctx.moves.size(), 40);
        for (size_t i = 0; i < shown; ++i)
        {
            html += "<li>" + escapeHtml(moveShortText(ctx, ctx.moves[i])) + "</li>";
        }
        if (ctx.moves.size() > shown)
        {
            html += "<li>… and " + std::to_string(ctx.moves.size() - shown) + " more</li>";
        }
        html += "</ul>";
    }

    std::vector<std::string> fixed;
    std::vector<std::string> remaining;
    for (int i = 0; i < ctx.instructionCount; ++i)
    {
        const Wait &before = ctx.waitBefore[i];
        const Wait &after = ctx.waitAfter[i];
        if (before.stalls > after.stalls && before.producer >= 0)
        {
            fixed.push_back(capitalized(lineOf(ctx, i)) + " (" + registerName(ctx, i, before.reg) + " from " +
                            lineOf(ctx, before.producer) + "): " + std::to_string(before.stalls) + " → " +
                            plural(after.stalls, stallWord(ctx)) + ". " + fixedNote(ctx, i));
        }
        if (after.stalls > 0 && after.producer >= 0)
        {
            remaining.push_back(capitalized(lineOf(ctx, i)) + " (" + registerName(ctx, i, after.reg) + " from " +
                                lineOf(ctx, after.producer) + "): " + plural(after.stalls, stallWord(ctx)) + ". " +
                                remainingNote(ctx, i));
        }
    }
    auto list = [](const std::string &title, const std::vector<std::string> &items) {
        if (items.empty())
        {
            return std::string();
        }
        std::string out = "<p><b>" + title + "</b></p><ul>";
        const size_t shown = std::min<size_t>(items.size(), 40);
        for (size_t i = 0; i < shown; ++i)
        {
            out += "<li>" + items[i] + "</li>";
        }
        if (items.size() > shown)
        {
            out += "<li>… and " + std::to_string(items.size() - shown) + " more</li>";
        }
        return out + "</ul>";
    };
    html += list(ctx.hazardUnit ? "Waits removed or shortened" : "NOPs no longer needed", fixed);
    html += list(ctx.hazardUnit ? "Waits that remain" : "NOPs that remain", remaining);

    if (!result.notes.empty())
    {
        html += "<p><b>Notes</b></p><ul>";
        for (const std::string &note : result.notes)
        {
            html += "<li>" + escapeHtml(note) + "</li>";
        }
        html += "</ul>";
    }
    html += "<p><i>Hover over a row to see why it changed. The explanations are built from the scheduler's "
            "dependence graph and the pipeline timing, using fixed sentence templates.</i></p>";
    return html;
}

} // namespace detail
} // namespace reorder
} // namespace Kites
