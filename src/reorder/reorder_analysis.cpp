/**
 * @file reorder_analysis.cpp
 * @brief Runs the list scheduler on editor text and records why it moved each instruction.
 *
 * All scheduling and timing comes from the list scheduler in external/list_scheduler
 * (plain C, an unchanged copy of the project's tool). This file only calls its public
 * functions: read_program, schedule_program, issue, stall_producer, build_dag. To update
 * the scheduler, copy its new files over the old ones; nothing here copies its loops.
 *
 * Steps (see analyzeProgram at the bottom):
 *   1. prepare()        editor text -> scheduler Program (temp file + read_program),
 *                       instructions whose operands the scheduler might misread become
 *                       fixed barriers
 *   2. schedule()       schedule_program() for the new order
 *   3. keepsMeaning()   independent check that every block computes the same values
 *   4. computeDeps()    which instructions must stay in order (build_dag), for the explanations
 *   5. computeWaits()   why each instruction waits, in the old and the new order
 *   6. computeNopGaps() no-hazard-unit pipelines: which NOPs the new code needs
 *   7. computeMoves()   which lines count as moved (weighted LIS) and why (counterfactual)
 */
#include "reorder_internal.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>

namespace Kites
{
namespace reorder
{

bool hasForwarding(PipelineVariant variant)
{
    return variant == PipelineVariant::HazardUnitForwarding ||
           variant == PipelineVariant::NoHazardUnitForwarding;
}

bool hasHazardUnit(PipelineVariant variant)
{
    return variant == PipelineVariant::HazardUnitForwarding ||
           variant == PipelineVariant::HazardUnitNoForwarding;
}

std::string variantDescription(PipelineVariant variant)
{
    switch (variant)
    {
    case PipelineVariant::HazardUnitForwarding:
        return "5-stage, hazard unit, forwarding";
    case PipelineVariant::HazardUnitNoForwarding:
        return "5-stage, hazard unit, no forwarding";
    case PipelineVariant::NoHazardUnitForwarding:
        return "5-stage, no hazard unit, forwarding";
    case PipelineVariant::NoHazardUnitNoForwarding:
        return "5-stage, no hazard unit, no forwarding";
    }
    return "";
}

namespace detail
{

bool isNoHazardUnit(const Context &ctx)
{
    return !ctx.hazardUnit;
}

int blockStart(const Context &ctx, int block)
{
    return ctx.program->blocks[block].start;
}

int blockEnd(const Context &ctx, int block)
{
    return ctx.program->blocks[block].end;
}

int originalLineOf(const Context &ctx, int instruction)
{
    return ctx.program->ins[instruction].line_no;
}

bool dependsOn(const Context &ctx, int earlier, int later)
{
    if (earlier < 0 || later < 0 || earlier >= later || ctx.blockOf[earlier] != ctx.blockOf[later])
    {
        return false;
    }
    const int block = ctx.blockOf[earlier];
    const int start = blockStart(ctx, block);
    return ctx.deps[block][earlier - start][later - start] != 0;
}

bool nopIsKept(const Context &ctx, int line)
{
    const LineInfo &info = ctx.lines[line];
    if (!info.strippedNop || info.nopGap < 0)
    {
        return false;
    }
    const GapNops &gap = ctx.gaps[info.nopGap];
    const auto it = std::find(gap.studentLines.begin(), gap.studentLines.end(), line);
    return it != gap.studentLines.end() && (it - gap.studentLines.begin()) < gap.kept;
}

namespace
{

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::string trim(const std::string &text)
{
    size_t first = 0;
    while (first < text.size() && isSpace(text[first]))
    {
        ++first;
    }
    size_t last = text.size();
    while (last > first && isSpace(text[last - 1]))
    {
        --last;
    }
    return text.substr(first, last - first);
}

/// What read_program() in parser.c makes of one line. Mirrors its rules exactly.
struct LineParse
{
    bool instruction = false;
    bool blankOrComment = false;
    bool labelOnly = false;
    int labelColon = -1; ///< position of the label ':' in the line
    std::string code;    ///< the instruction without label and comment
};

LineParse parseLine(const std::string &line)
{
    LineParse result;
    const size_t hash = line.find('#');
    const std::string codePart = hash == std::string::npos ? line : line.substr(0, hash);
    size_t lead = 0;
    while (lead < codePart.size() && isSpace(codePart[lead]))
    {
        ++lead;
    }
    const std::string code = trim(codePart);
    if (code.empty())
    {
        result.blankOrComment = true;
        return result;
    }

    const size_t colon = code.find(':');
    if (colon != std::string::npos && colon > 0)
    {
        result.labelColon = static_cast<int>(lead + colon);
        const std::string rest = trim(code.substr(colon + 1));
        if (rest.empty())
        {
            result.labelOnly = true;
            return result;
        }
        if (rest[0] == '.')
        {
            return result; // "arr: .word 1, 2"
        }
        result.instruction = true;
        result.code = rest;
        return result;
    }
    if (code[0] == '.')
    {
        return result; // directive
    }
    result.instruction = true;
    result.code = code;
    return result;
}

/**
 * Does the scheduler read every register of this instruction correctly?
 * read_program() silently skips operands it does not understand (for example
 * "lw t0, label" or "lw t0, (a0)" lose their destination register), which
 * would hide a dependence. Such instructions are turned into barriers.
 */
bool operandsUnderstood(Instr *in)
{
    OpInfo op{};
    if (find_op(in->mnem, &op) == 0)
    {
        return false;
    }
    char code[MAX_TEXT];
    std::strncpy(code, in->code, MAX_TEXT - 1);
    code[MAX_TEXT - 1] = '\0';
    char words[MAX_WORDS][MAX_WORD];
    const int operandCount = split_words(code, words) - 1;
    auto isRegister = [&](int k) { return k >= 1 && k <= operandCount && reg_number(words[k]) >= 0; };

    // FP instructions may end with a rounding mode (rne, rtz, dyn, ...), which is not a register
    switch (op.format)
    {
    case FMT_R:
        return (operandCount == 3 || operandCount == 4) && isRegister(1) && isRegister(2) && isRegister(3);
    case FMT_R4:
        return (operandCount == 4 || operandCount == 5) && isRegister(1) && isRegister(2) && isRegister(3) &&
               isRegister(4);
    case FMT_I:
        return operandCount == 3 && isRegister(1) && isRegister(2);
    case FMT_LOAD:
    case FMT_STORE:
    case FMT_JALR:
        return operandCount == 3 && isRegister(1) && isRegister(3);
    case FMT_BRANCH:
        return operandCount == 3 && isRegister(1) && isRegister(2);
    case FMT_U:
    case FMT_LI:
    case FMT_LA:
    case FMT_BZ:
        return operandCount == 2 && isRegister(1);
    case FMT_JAL:
        return operandCount == 1 || (operandCount == 2 && isRegister(1));
    case FMT_RD_RS:
        return (operandCount == 2 || operandCount == 3) && isRegister(1) && isRegister(2);
    case FMT_NONE:
    case FMT_ECALL:
    case FMT_RET:
        return operandCount == 0;
    case FMT_J:
    case FMT_CALL:
    case FMT_TAIL:
        return operandCount == 1;
    case FMT_JR:
        return operandCount == 1 && isRegister(1);
    default:
        return false;
    }
}

/// read_program() treats every line between .data (.rodata, .bss, .sdata) and .text as data,
/// e.g. the continuation line of a .word list. Mirrors its rule: returns the new state.
bool updateDataSection(const std::string &line, bool inData)
{
    const size_t hash = line.find('#');
    const std::string code = trim(hash == std::string::npos ? line : line.substr(0, hash));
    auto startsWith = [&](const char *word) { return code.rfind(word, 0) == 0; };
    if (startsWith(".text"))
    {
        return false;
    }
    if (startsWith(".data") || startsWith(".rodata") || startsWith(".bss") || startsWith(".sdata"))
    {
        return true;
    }
    if (startsWith(".section"))
    {
        return code.find(".text") == std::string::npos;
    }
    return inData;
}

std::filesystem::path makeTempPath()
{
    static std::atomic<unsigned> counter{0};
    std::error_code error;
    std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    if (error)
    {
        directory = std::filesystem::current_path(error);
    }
    unsigned noise = 0;
    try
    {
        std::random_device device;
        noise = device();
    }
    catch (...)
    {
        noise = 0;
    }
    return directory / ("kites_reorder_" + std::to_string(noise) + "_" +
                        std::to_string(counter.fetch_add(1)) + ".s");
}

bool isLabelCharacter(char c, bool first)
{
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '$';
    return letter || (!first && c >= '0' && c <= '9');
}

/**
 * Kites reads "x: y: add ..." as two labels and '.asciz "a: b"' as a directive,
 * but read_program() would take the text after the first ':' as an instruction.
 * Returns the line as the scheduler should see it; extraLabels is set when a
 * second label was removed from in front of an instruction.
 */
std::string sanitizeForScheduler(const std::string &line, bool &extraLabels)
{
    extraLabels = false;
    const size_t hash = line.find('#');
    const std::string code = hash == std::string::npos ? line : line.substr(0, hash);
    if (code.find(':') == std::string::npos)
    {
        return line;
    }
    size_t pos = 0;
    while (pos < code.size() && isSpace(code[pos]))
    {
        ++pos;
    }
    const size_t lead = pos;
    int labels = 0;
    size_t firstColon = std::string::npos;
    size_t afterLabels = pos;
    while (true)
    {
        size_t end = pos;
        while (end < code.size() && isLabelCharacter(code[end], end == pos))
        {
            ++end;
        }
        size_t colon = end;
        while (colon < code.size() && (code[colon] == ' ' || code[colon] == '\t'))
        {
            ++colon;
        }
        if (end == pos || colon >= code.size() || code[colon] != ':')
        {
            break;
        }
        ++labels;
        if (firstColon == std::string::npos)
        {
            firstColon = colon;
        }
        pos = colon + 1;
        while (pos < code.size() && isSpace(code[pos]))
        {
            ++pos;
        }
        afterLabels = pos;
    }
    if (labels == 0)
    {
        // no label: a ':' inside a directive (".asciz \"a: b\"") must not become a label
        return lead < code.size() && code[lead] == '.' ? code.substr(0, lead) + ".d" : line;
    }
    if (labels == 1)
    {
        return line;
    }
    const std::string rest = trim(code.substr(afterLabels));
    const std::string first = code.substr(0, firstColon + 1);
    if (rest.empty() || rest[0] == '.')
    {
        return first;
    }
    extraLabels = true;
    return first + " " + rest;
}

/// Machine instructions Kites emits for "li rd, value" (pseudo_formats.cpp), or -1 if unknown.
int liInstructionCount(long long value)
{
    if (value >= -2048 && value <= 2047)
    {
        return 1;
    }
    const long long low = ((value & 0xfff) ^ 0x800) - 0x800; // sign-extended low 12 bits
    if (value >= INT32_MIN && value <= INT32_MAX)
    {
        return low == 0 ? 1 : 2;
    }
    long long high = (value - low) >> 12;
    int shift = 12;
    while (high != 0 && (high & 1) == 0)
    {
        high >>= 1;
        ++shift;
    }
    (void)shift;
    const int upper = liInstructionCount(high);
    return upper < 0 ? -1 : upper + 1 + (low != 0 ? 1 : 0);
}

void markBarrier(Program *p, int i)
{
    Instr *in = &p->ins[i];
    in->starts_block = 1;
    in->ends_block = 1;
    if (i + 1 < p->nins)
    {
        p->ins[i + 1].starts_block = 1;
    }
}

bool prepare(Context &ctx, const std::string &source, std::string &error)
{
    std::vector<std::string> raw;
    {
        std::string current;
        for (char c : source)
        {
            if (c == '\n')
            {
                raw.push_back(current);
                current.clear();
            }
            else
            {
                current.push_back(c);
            }
        }
        ctx.endsWithNewline = !source.empty() && source.back() == '\n';
        if (!ctx.endsWithNewline)
        {
            raw.push_back(current);
        }
    }

    // The copy given to the scheduler: ';' comments become '#' comments (the
    // scheduler only knows '#'), and on no-hazard-unit pipelines nop lines are
    // left out so that the NOPs can be recomputed for the new order.
    std::vector<std::string> schedulerLines;
    std::vector<bool> expectInstruction;
    std::vector<int> extraLabelLines;
    int expectedInstructions = 0;
    int expectedEntries = 0;
    bool inData = false;
    ctx.lines.resize(raw.size());
    for (size_t i = 0; i < raw.size(); ++i)
    {
        LineInfo &info = ctx.lines[i];
        info.text = raw[i];
        std::string line = raw[i];
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.size() > static_cast<size_t>(MAX_TEXT - 2))
        {
            error = "Line " + std::to_string(i + 1) + " is longer than " +
                    std::to_string(MAX_TEXT - 2) + " characters, which the reordering tool cannot read.";
            return false;
        }
        const size_t comment = line.find_first_of("#;");
        if (comment != std::string::npos && line[comment] == ';')
        {
            line[comment] = '#';
        }

        bool extraLabels = false;
        line = sanitizeForScheduler(line, extraLabels);
        if (extraLabels)
        {
            extraLabelLines.push_back(static_cast<int>(i));
        }
        LineParse parse = parseLine(line);
        inData = updateDataSection(line, inData);
        if (inData && !parse.blankOrComment)
        {
            parse = LineParse{}; // data, printed back as it is
        }
        if (parse.instruction && !ctx.hazardUnit && parse.code == "nop")
        {
            info.strippedNop = true;
            line = parse.labelColon >= 0 ? line.substr(0, static_cast<size_t>(parse.labelColon) + 1)
                                         : std::string();
            parse = parseLine(line);
        }
        info.labelColon = parse.labelColon;
        info.labelOnly = parse.labelOnly;
        info.blankOrComment = parse.blankOrComment && !info.strippedNop;
        if (parse.instruction)
        {
            ++expectedInstructions;
            expectedEntries += parse.labelColon >= 0 ? 2 : 1;
        }
        else
        {
            ++expectedEntries;
        }
        expectInstruction.push_back(parse.instruction);
        schedulerLines.push_back(line);
    }
    if (expectedEntries > MAX_LINES || expectedInstructions > MAX_INSTR)
    {
        error = "The program is too large for the reordering tool (at most " + std::to_string(MAX_INSTR) +
                " instructions and " + std::to_string(MAX_LINES) + " lines).";
        return false;
    }

    const std::filesystem::path path = makeTempPath();
    {
        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            error = "Could not write a temporary file for the analysis (" + path.string() + ").";
            return false;
        }
        for (const std::string &line : schedulerLines)
        {
            out << line << '\n';
        }
    }
    const std::string pathText = path.string();
    std::vector<char> pathBuffer(pathText.begin(), pathText.end());
    pathBuffer.push_back('\0');
    ctx.program = std::make_unique<Program>();
    const int status = read_program(pathBuffer.data(), ctx.program.get());
    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    if (status != 0)
    {
        error = "The reordering tool could not read the program.";
        return false;
    }

    Program *p = ctx.program.get();
    if (p->nins != expectedInstructions)
    {
        error = "Internal error: the scheduler read a different number of instructions.";
        return false;
    }
    ctx.instructionCount = p->nins;
    int previousLine = -1;
    for (int i = 0; i < p->nins; ++i)
    {
        const int line = p->ins[i].line_no - 1;
        if (line <= previousLine || line >= static_cast<int>(ctx.lines.size()) || !expectInstruction[line])
        {
            error = "Internal error: could not match the scheduler's instructions to the editor lines.";
            return false;
        }
        ctx.lines[line].instruction = i;
        previousLine = line;
    }

    // Instructions the scheduler might misread become barriers, exactly like the
    // unknown instructions it already leaves in place: a block of their own.
    ctx.barrier.assign(static_cast<size_t>(p->nins), Barrier::None);
    bool blocksChanged = false;
    for (int i = 0; i < p->nins; ++i)
    {
        Instr *in = &p->ins[i];
        if (in->known == 0)
        {
            ctx.barrier[i] = Barrier::UnknownMnemonic;
            continue;
        }
        if (std::strcmp(in->mnem, "li") == 0)
        {
            // the scheduler guesses 2 slots for every large immediate; use what Kites emits.
            // The value is read from the whole line: the scheduler keeps only MAX_WORD - 1
            // characters of a word, which cuts long binary literals.
            const std::string code = in->code;
            const size_t comma = code.rfind(',');
            const std::string operand = comma == std::string::npos ? std::string() : trim(code.substr(comma + 1));
            char *end = nullptr;
            errno = 0;
            const long long value = std::strtoll(operand.c_str(), &end, 0);
            if (!operand.empty() && end != operand.c_str() && *end == '\0' && errno == 0)
            {
                const int count = liInstructionCount(value);
                if (count > 0)
                {
                    in->size = count;
                }
            }
        }
        const bool pcRelative = std::strcmp(in->mnem, "auipc") == 0;
        if (pcRelative || !operandsUnderstood(in))
        {
            // auipc: its value depends on its address, so it must keep its place
            ctx.barrier[i] = pcRelative ? Barrier::PcRelative : Barrier::UnclearOperands;
            const bool symbolAccess = (in->cls == CLASS_LOAD || in->cls == CLASS_STORE) &&
                                      std::strchr(in->code, '(') == nullptr;
            in->known = 0;
            in->cls = CLASS_SYSTEM;
            in->rd = -1;
            in->nsrc = 0;
            in->mem_base = -1;
            in->mem_off_known = 0;
            in->mem_bytes = 0;
            if (symbolAccess)
            {
                in->size = 2; // "lw rd, label" = auipc + lw
            }
            markBarrier(p, i);
            blocksChanged = true;
        }
    }
    for (int line : extraLabelLines)
    {
        const int i = ctx.lines[line].instruction;
        if (i >= 0 && ctx.barrier[i] == Barrier::None)
        {
            ctx.barrier[i] = Barrier::UnclearOperands; // "x: y: add ..." keeps its place
            markBarrier(p, i);
            blocksChanged = true;
        }
    }
    if (!ctx.hazardUnit)
    {
        // The tool cannot see the registers of a barrier. Without a hazard unit a
        // wrong distance to it gives wrong results, so its neighbours keep their places too.
        std::vector<int> real;
        for (int i = 0; i < p->nins; ++i)
        {
            if (ctx.barrier[i] != Barrier::None)
            {
                real.push_back(i);
            }
        }
        for (int i : real)
        {
            for (int j = std::max(0, i - 2); j <= std::min(p->nins - 1, i + 2); ++j)
            {
                if (ctx.barrier[j] == Barrier::None)
                {
                    ctx.barrier[j] = Barrier::NearBarrier;
                    markBarrier(p, j);
                    blocksChanged = true;
                }
            }
        }
    }
    if (blocksChanged)
    {
        find_blocks(p);
    }

    ctx.blockOf.assign(static_cast<size_t>(p->nins), 0);
    for (int b = 0; b < p->nblocks; ++b)
    {
        for (int i = p->blocks[b].start; i < p->blocks[b].end; ++i)
        {
            ctx.blockOf[i] = b;
        }
    }
    return true;
}

/// Issue instruction i with the scheduler's timing (issue() in hazard.c) and record why it waits
/// (stall_producer()). Call it in program order on one PipeState.
Wait issueAndRecord(Program *p, HazardModel *m, PipeState *st, int i)
{
    Wait wait;
    wait.producer = stall_producer(p, m, st, i);
    const Instr &in = p->ins[i];
    for (int k = 0; k < in.nsrc && wait.producer >= 0; ++k)
    {
        if (st->writer[in.src[k]] == wait.producer)
        {
            wait.reg = in.src[k];
            break;
        }
    }
    const int slotBefore = st->next_slot;
    const int total = issue(p, m, st, i);
    wait.inner = inner_stalls(p, m, i);
    wait.stalls = total - wait.inner;
    wait.startSlot = slotBefore + wait.stalls;
    return wait;
}

/// No hazard unit: the instruction enters at once, whatever it reads. Returns how long it
/// should have waited. (issue() with its sources hidden for a moment, so it does not wait.)
Wait enterWithoutWaiting(Program *p, HazardModel *m, PipeState *st, int i)
{
    PipeState probe;
    copy_state(&probe, st);
    Wait wait = issueAndRecord(p, m, &probe, i);
    wait.startSlot = st->next_slot;
    Instr *in = &p->ins[i];
    const int sources = in->nsrc;
    in->nsrc = 0;
    issue(p, m, st, i);
    in->nsrc = sources;
    return wait;
}

/// Wait of every instruction when the whole program runs in `order` (order[k] = instruction).
std::vector<Wait> waitsInOrder(Context &ctx, const std::vector<int> &order)
{
    std::vector<Wait> waits(order.size());
    PipeState state;
    reset_state(&state);
    for (int instruction : order)
    {
        waits[instruction] = issueAndRecord(ctx.program.get(), &ctx.model, &state, instruction);
    }
    return waits;
}

int totalStalls(const std::vector<Wait> &waits)
{
    int total = 0;
    for (const Wait &wait : waits)
    {
        total += wait.stalls + wait.inner;
    }
    return total;
}

struct Sequence
{
    std::vector<std::array<int, MAX_SRC>> sourceWriter; ///< per position
    std::vector<int> baseWriter;                        ///< per position
    std::array<int, NREGS> finalWriter{};
};

/// Which instruction each register value comes from, inside one block.
Sequence analyseSequence(const Program *p, const std::vector<int> &sequence)
{
    Sequence result;
    std::array<int, NREGS> lastWriter;
    lastWriter.fill(-1);
    for (int instruction : sequence)
    {
        const Instr &in = p->ins[instruction];
        std::array<int, MAX_SRC> sources;
        sources.fill(-2);
        for (int k = 0; k < in.nsrc; ++k)
        {
            sources[k] = lastWriter[in.src[k]];
        }
        result.sourceWriter.push_back(sources);
        result.baseWriter.push_back(in.mem_base > 0 ? lastWriter[in.mem_base] : -1);
        if (in.rd > 0)
        {
            lastWriter[in.rd] = instruction;
        }
    }
    result.finalWriter = lastWriter;
    return result;
}

/**
 * Independent safety net (the same checks as verify.c): every register is read
 * from the same instruction as before, every register's final value comes from
 * the same instruction, loads/stores that may overlap keep their order and the
 * branch/jump stays last.
 */
bool keepsMeaning(Program *p, int start, int end, const std::vector<int> &order)
{
    const int count = end - start;
    std::vector<int> newPosition(static_cast<size_t>(count), -1);
    for (int k = start; k < end; ++k)
    {
        const int instruction = order[k];
        if (instruction < start || instruction >= end || newPosition[instruction - start] != -1)
        {
            return false;
        }
        newPosition[instruction - start] = k - start;
    }

    std::vector<int> original;
    std::vector<int> reordered;
    for (int k = start; k < end; ++k)
    {
        original.push_back(k);
        reordered.push_back(order[k]);
    }
    const Sequence before = analyseSequence(p, original);
    const Sequence after = analyseSequence(p, reordered);
    if (before.finalWriter != after.finalWriter)
    {
        return false;
    }
    for (int instruction = start; instruction < end; ++instruction)
    {
        if (before.sourceWriter[instruction - start] != after.sourceWriter[newPosition[instruction - start]])
        {
            return false;
        }
    }
    for (int u = start; u < end; ++u)
    {
        if (!is_memory(&p->ins[u]))
        {
            continue;
        }
        for (int v = u + 1; v < end; ++v)
        {
            if (!is_memory(&p->ins[v]))
            {
                continue;
            }
            if (p->ins[u].cls == CLASS_LOAD && p->ins[v].cls == CLASS_LOAD)
            {
                continue;
            }
            if (may_alias(&p->ins[u], before.baseWriter[u - start], &p->ins[v], before.baseWriter[v - start]) &&
                newPosition[u - start] > newPosition[v - start])
            {
                return false;
            }
        }
    }
    if (p->ins[end - 1].ends_block == 1 && order[end - 1] != end - 1)
    {
        return false;
    }
    return true;
}

void schedule(Context &ctx)
{
    Program *p = ctx.program.get();
    const int count = p->nins;

    ::Options options{};
    options.window = ctx.options.windowSize;
    options.win_mode = ctx.options.slidingWindows ? WIN_SLIDE : WIN_FIXED;
    options.cross_block = ctx.options.crossBlock ? 1 : 0;
    options.reorder = 1;
    options.insert_nops = 0;
    ctx.order.assign(static_cast<size_t>(std::max(count, 1)), 0);
    schedule_program(p, &ctx.model, &options, ctx.order.data());
    ctx.order.resize(static_cast<size_t>(count));

    for (int b = 0; b < p->nblocks; ++b)
    {
        const int start = p->blocks[b].start;
        const int end = p->blocks[b].end;
        if (!keepsMeaning(p, start, end, ctx.order))
        {
            for (int k = start; k < end; ++k)
            {
                ctx.order[k] = k;
            }
            ctx.notes.push_back("Block " + std::to_string(b + 1) +
                                " keeps its original order: the safety check of the new order failed.");
        }
    }

    ctx.slotOf.assign(static_cast<size_t>(count), 0);
    for (int k = 0; k < count; ++k)
    {
        ctx.slotOf[ctx.order[k]] = k;
    }
}

/// deps[b][u][v]: the scheduler's dependence graph (build_dag) of each block in its original
/// order. One graph per start position, so pairs up to MAX_WIN apart are exact; pairs further
/// apart are taken as dependent (only the explanations use this).
void computeDeps(Context &ctx)
{
    Program *p = ctx.program.get();
    ctx.deps.assign(static_cast<size_t>(p->nblocks), {});
    auto dag = std::make_unique<Dag>();
    PipeState empty;
    reset_state(&empty);
    int nodes[MAX_WIN];
    for (int b = 0; b < p->nblocks; ++b)
    {
        const int start = p->blocks[b].start;
        const int count = p->blocks[b].end - start;
        auto &deps = ctx.deps[b];
        deps.assign(static_cast<size_t>(count), std::vector<unsigned char>(static_cast<size_t>(count), 1));
        for (int u = 0; u < count; ++u)
        {
            const int span = std::min(count - u, MAX_WIN);
            for (int k = 0; k < span; ++k)
            {
                nodes[k] = start + u + k;
            }
            build_dag(p, &ctx.model, &empty, nodes, span, dag.get(), start + u + span, -1);
            for (int k = 1; k < span; ++k)
            {
                deps[u][u + k] = dag->edge[0][k] != -1 ? 1 : 0;
            }
        }
    }
}

void computeWaits(Context &ctx)
{
    Program *p = ctx.program.get();
    const int count = p->nins;
    PipeState state;

    ctx.waitBefore.assign(static_cast<size_t>(count), Wait{});
    reset_state(&state);
    for (int k = 0; k < count; ++k)
    {
        ctx.waitBefore[k] = issueAndRecord(p, &ctx.model, &state, k);
    }
    ctx.finalSlotBefore = state.next_slot;

    ctx.waitAfter.assign(static_cast<size_t>(count), Wait{});
    reset_state(&state);
    for (int k = 0; k < count; ++k)
    {
        ctx.waitAfter[ctx.order[k]] = issueAndRecord(p, &ctx.model, &state, ctx.order[k]);
    }
    ctx.finalSlotAfter = state.next_slot;
}

/// Where NOPs for the first instruction of a block go: in front of the labels
/// above it, so that they run once on the way in instead of on every loop
/// iteration (the producer is in the code before the label).
int nopInsertionLine(const Context &ctx, int slot)
{
    const Program *p = ctx.program.get();
    const int slotLine = p->ins[slot].line_no - 1;
    if (!p->ins[slot].starts_block)
    {
        return slotLine;
    }
    int best = slotLine;
    for (int line = slotLine - 1; line >= 0; --line)
    {
        const LineInfo &info = ctx.lines[line];
        if (info.instruction >= 0)
        {
            break;
        }
        if (info.labelOnly)
        {
            best = line;
            continue;
        }
        if (info.strippedNop || info.blankOrComment)
        {
            continue;
        }
        break; // a directive or a data label
    }
    return best;
}

void computeNopGaps(Context &ctx)
{
    Program *p = ctx.program.get();
    const int count = p->nins;
    ctx.gaps.assign(static_cast<size_t>(count) + 1, GapNops{});

    int slot = 0;
    for (size_t line = 0; line < ctx.lines.size(); ++line)
    {
        LineInfo &info = ctx.lines[line];
        if (info.instruction >= 0)
        {
            slot = info.instruction + 1;
        }
        else if (info.strippedNop)
        {
            info.nopGap = slot;
            ctx.gaps[slot].studentLines.push_back(static_cast<int>(line));
        }
    }

    // The original text with its own NOPs. Timing on a pipeline that never stalls;
    // missing NOPs = the fewest NOPs to add (a NOP added early also helps later lines).
    ctx.waitStudent.assign(static_cast<size_t>(count), Wait{});
    PipeState state;
    PipeState counting;
    reset_state(&state);
    reset_state(&counting);
    for (int k = 0; k < count; ++k)
    {
        const int nops = static_cast<int>(ctx.gaps[k].studentLines.size());
        state.next_slot += nops;
        counting.next_slot += nops;
        const Wait timing = enterWithoutWaiting(p, &ctx.model, &state, k);
        ctx.waitStudent[k] = issueAndRecord(p, &ctx.model, &counting, k);
        ctx.waitStudent[k].startSlot = timing.startSlot;
    }
    state.next_slot += static_cast<int>(ctx.gaps[count].studentLines.size());
    ctx.finalSlotStudent = state.next_slot;

    // Barriers hide their registers, so NOPs near one are always kept.
    auto nearBarrier = [&](int gapIndex) {
        for (int slot = std::max(0, gapIndex - 3); slot <= std::min(count - 1, gapIndex + 2); ++slot)
        {
            const Barrier kind = ctx.barrier[slot];
            if (kind != Barrier::None && kind != Barrier::NearBarrier)
            {
                return true;
            }
        }
        return false;
    };

    for (int k = 0; k < count; ++k)
    {
        GapNops &gap = ctx.gaps[k];
        gap.needed = ctx.waitAfter[ctx.order[k]].stalls;
        gap.kept = std::min(static_cast<int>(gap.studentLines.size()), gap.needed);
        if (nearBarrier(k))
        {
            gap.kept = static_cast<int>(gap.studentLines.size());
        }
        if (gap.needed > gap.kept)
        {
            if (gap.kept > 0)
            {
                gap.insertAfterLine = gap.studentLines[gap.kept - 1];
            }
            else
            {
                gap.insertBeforeLine = nopInsertionLine(ctx, k);
            }
        }
    }
    // NOPs after the last instruction are not about data hazards: keep them.
    GapNops &trailing = ctx.gaps[count];
    trailing.kept = static_cast<int>(trailing.studentLines.size());
    trailing.needed = trailing.kept;

    // Timing of the new text with all its NOPs (kept + added), no stalls.
    ctx.rightStart.assign(static_cast<size_t>(count), 0);
    reset_state(&state);
    for (int k = 0; k < count; ++k)
    {
        state.next_slot += ctx.gaps[k].total();
        ctx.rightStart[ctx.order[k]] = enterWithoutWaiting(p, &ctx.model, &state, ctx.order[k]).startSlot;
    }
    ctx.finalSlotRight = state.next_slot + trailing.kept;
}

/**
 * Why one instruction moved. Counterfactual: take the new order, put only this
 * instruction back as close to its old place as the dependences allow, and count
 * the stalls of the whole program again (the scheduler's own timing).
 */
void explainMove(Context &ctx, MoveInfo &move, int stallsChosen)
{
    const int instruction = move.instruction;
    const int start = blockStart(ctx, move.block);
    const int end = blockEnd(ctx, move.block);
    const int count = end - start;

    std::vector<int> position(static_cast<size_t>(count), 0); // new position inside the block
    for (int k = start; k < end; ++k)
    {
        position[ctx.order[k] - start] = k - start;
    }
    const int current = position[instruction - start];
    auto mustStayOrdered = [&](int a, int b) { return a < b ? dependsOn(ctx, a, b) : dependsOn(ctx, b, a); };

    // Instructions whose order with it changed: it passed them.
    for (int other = start; other < end; ++other)
    {
        if (other != instruction && (other < instruction) != (position[other - start] < current))
        {
            move.passed.push_back(other);
        }
    }

    // The nearest dependence in the direction it moved.
    const bool movedUp = move.toPos < move.fromPos;
    int limitPosition = movedUp ? -1 : INT_MAX;
    for (int other = start; other < end; ++other)
    {
        const int at = position[other - start];
        if (movedUp && other < instruction && dependsOn(ctx, other, instruction) && at > limitPosition)
        {
            limitPosition = at;
            move.limit = other;
        }
        if (!movedUp && other > instruction && dependsOn(ctx, instruction, other) && at < limitPosition)
        {
            limitPosition = at;
            move.limit = other;
        }
    }
    move.limitAbove = movedUp;

    // The new block order without it, and where it would go back.
    std::vector<int> rest;
    for (int k = start; k < end; ++k)
    {
        if (ctx.order[k] != instruction)
        {
            rest.push_back(ctx.order[k]);
        }
    }
    int target = 0; // right after the last instruction that came before it
    for (int index = 0; index < static_cast<int>(rest.size()); ++index)
    {
        if (rest[index] < instruction)
        {
            target = index + 1;
        }
    }
    int low = 0;
    int high = static_cast<int>(rest.size());
    for (int index = 0; index < static_cast<int>(rest.size()); ++index)
    {
        if (rest[index] < instruction && mustStayOrdered(rest[index], instruction))
        {
            low = std::max(low, index + 1);
        }
        if (rest[index] > instruction && mustStayOrdered(instruction, rest[index]))
        {
            high = std::min(high, index);
        }
    }
    if (target == current)
    {
        move.reason = MoveReason::KeepsRelativePlace;
        return;
    }
    const int back = std::clamp(target, low, std::max(low, high));
    if (back == current)
    {
        move.reason = MoveReason::FollowsDependency;
        if (target > current && high < static_cast<int>(rest.size()))
        {
            move.pinnedBy = rest[high];
            move.pinnedBefore = true;
        }
        else if (target < current && low > 0)
        {
            move.pinnedBy = rest[low - 1];
            move.pinnedBefore = false;
        }
        return;
    }

    std::vector<int> putBack = rest;
    putBack.insert(putBack.begin() + back, instruction);
    std::vector<int> fullOrder = ctx.order;
    std::copy(putBack.begin(), putBack.end(), fullOrder.begin() + start);
    const std::vector<Wait> backWaits = waitsInOrder(ctx, fullOrder);
    move.saving = totalStalls(backWaits) - stallsChosen;
    if (move.saving <= 0)
    {
        move.reason = MoveReason::Neutral;
        return;
    }

    // Who waits longer when it goes back (this block, or the next block's first lines).
    for (int other = 0; other < ctx.instructionCount; ++other)
    {
        if (backWaits[other].stalls > ctx.waitAfter[other].stalls)
        {
            move.fixedConsumers.push_back(other);
        }
    }
    move.reason = MoveReason::Indirect;
    // Only name hazards that really were in the student's order and got shorter.
    auto reallyShorter = [&](int consumer) {
        return ctx.waitBefore[consumer].stalls > ctx.waitAfter[consumer].stalls &&
               ctx.waitBefore[consumer].producer >= 0;
    };
    if (std::find(move.fixedConsumers.begin(), move.fixedConsumers.end(), instruction) != move.fixedConsumers.end() &&
        reallyShorter(instruction))
    {
        move.reason = MoveReason::Separates;
        move.gapConsumer = instruction;
        move.gapProducer = ctx.waitBefore[instruction].producer;
        return;
    }
    for (int consumer : move.fixedConsumers)
    {
        if (!reallyShorter(consumer))
        {
            continue;
        }
        const int producer = ctx.waitBefore[consumer].producer;
        const bool betweenNow =
            ctx.slotOf[producer] < ctx.slotOf[instruction] && ctx.slotOf[instruction] < ctx.slotOf[consumer];
        const bool betweenBefore = producer < instruction && instruction < consumer;
        if (betweenNow && !betweenBefore)
        {
            move.reason = MoveReason::FillsGap;
            move.gapProducer = producer;
            move.gapConsumer = consumer;
            return;
        }
    }
    if (!move.fixedConsumers.empty())
    {
        move.gapConsumer = move.fixedConsumers.front();
        move.gapProducer = backWaits[move.gapConsumer].producer;
    }
}

void computeMoves(Context &ctx)
{
    Program *p = ctx.program.get();
    ctx.moves.clear();
    ctx.moveOf.assign(static_cast<size_t>(p->nins), -1);
    const int stallsChosen = totalStalls(ctx.waitAfter);

    for (int b = 0; b < p->nblocks; ++b)
    {
        const int start = p->blocks[b].start;
        const int end = p->blocks[b].end;
        bool reordered = false;
        for (int k = start; k < end; ++k)
        {
            reordered = reordered || ctx.order[k] != k;
        }
        if (!reordered)
        {
            continue;
        }

        // The block's lines as items; text lines and kept NOPs never move.
        const int firstLine = p->ins[start].line_no - 1;
        const int lastLine = p->ins[end - 1].line_no - 1;
        std::vector<int> itemSlot;     // slot index, or -1 for a fixed line
        std::vector<int> slotItem(static_cast<size_t>(end - start), -1);
        for (int line = firstLine; line <= lastLine; ++line)
        {
            const LineInfo &info = ctx.lines[line];
            if (info.instruction >= 0)
            {
                slotItem[info.instruction - start] = static_cast<int>(itemSlot.size());
                itemSlot.push_back(info.instruction);
            }
            else if (!info.strippedNop || nopIsKept(ctx, line))
            {
                itemSlot.push_back(-1);
            }
        }

        // Which instructions are part of a hazard, and which ones now fill a gap.
        auto isEndpoint = [&](int instruction) {
            if (ctx.waitBefore[instruction].stalls > 0 || ctx.waitAfter[instruction].stalls > 0)
            {
                return true;
            }
            for (int other = start; other < end; ++other)
            {
                if ((ctx.waitBefore[other].stalls > 0 && ctx.waitBefore[other].producer == instruction) ||
                    (ctx.waitAfter[other].stalls > 0 && ctx.waitAfter[other].producer == instruction))
                {
                    return true;
                }
            }
            return false;
        };
        auto isFiller = [&](int instruction) {
            for (int consumer = start; consumer < end; ++consumer)
            {
                const Wait &wait = ctx.waitBefore[consumer];
                const int producer = wait.producer;
                if (wait.stalls <= ctx.waitAfter[consumer].stalls || producer < start)
                {
                    continue;
                }
                const bool betweenNow = ctx.slotOf[producer] < ctx.slotOf[instruction] &&
                                        ctx.slotOf[instruction] < ctx.slotOf[consumer];
                const bool betweenBefore = producer < instruction && instruction < consumer;
                if (betweenNow && !betweenBefore)
                {
                    return true;
                }
            }
            return false;
        };

        const int itemCount = static_cast<int>(itemSlot.size());
        std::vector<int> newIndex(static_cast<size_t>(itemCount));
        std::vector<long long> weight(static_cast<size_t>(itemCount));
        for (int a = 0; a < itemCount; ++a)
        {
            if (itemSlot[a] < 0)
            {
                newIndex[a] = a;
                weight[a] = 1000000000LL;
                continue;
            }
            const int instruction = itemSlot[a]; // the original instruction of this slot
            newIndex[a] = slotItem[ctx.slotOf[instruction] - start];
            if (newIndex[a] == a)
            {
                weight[a] = 1000000000LL; // same slot as before: always consistent with the fixed lines
                continue;
            }
            long long w = 100;
            if (isEndpoint(instruction))
            {
                w += 20;
            }
            if (isFiller(instruction))
            {
                w -= 50;
            }
            weight[a] = w;
        }

        // Maximum-weight increasing subsequence = the lines that stay.
        std::vector<long long> best(static_cast<size_t>(itemCount), 0);
        std::vector<int> previous(static_cast<size_t>(itemCount), -1);
        int bestEnd = -1;
        for (int a = 0; a < itemCount; ++a)
        {
            best[a] = weight[a];
            for (int c = 0; c < a; ++c)
            {
                if (newIndex[c] < newIndex[a] && best[c] + weight[a] > best[a])
                {
                    best[a] = best[c] + weight[a];
                    previous[a] = c;
                }
            }
            if (bestEnd < 0 || best[a] > best[bestEnd])
            {
                bestEnd = a;
            }
        }
        std::vector<bool> stays(static_cast<size_t>(itemCount), false);
        for (int a = bestEnd; a >= 0; a = previous[a])
        {
            stays[a] = true;
        }

        for (int a = 0; a < itemCount; ++a)
        {
            if (itemSlot[a] < 0 || stays[a])
            {
                continue;
            }
            MoveInfo move;
            move.instruction = itemSlot[a];
            move.block = b;
            move.fromPos = move.instruction - start;
            move.toPos = ctx.slotOf[move.instruction] - start;
            explainMove(ctx, move, stallsChosen);
            ctx.moveOf[move.instruction] = static_cast<int>(ctx.moves.size());
            ctx.moves.push_back(move);
        }
    }
}

} // namespace
} // namespace detail

namespace detail
{

bool analyze(Context &ctx, const std::string &source, const AnalysisOptions &options, AnalysisResult &result)
{
    result = AnalysisResult{};
    result.options = options;
    result.options.windowSize = std::clamp(options.windowSize, 1, MAX_WIN);
    result.originalText = source;

    ctx.options = result.options;
    ctx.forwarding = hasForwarding(options.variant);
    ctx.hazardUnit = hasHazardUnit(options.variant);
    ctx.model.forwarding = ctx.forwarding ? 1 : 0;

    try
    {
        std::string error;
        if (!prepare(ctx, source, error))
        {
            result.error = error;
            return false;
        }
        schedule(ctx);
        computeDeps(ctx);
        computeWaits(ctx);
        if (!ctx.hazardUnit)
        {
            computeNopGaps(ctx);
        }
        computeMoves(ctx);
        buildDiff(ctx, result);
    }
    catch (const std::exception &exception)
    {
        result = AnalysisResult{};
        result.options = options;
        result.originalText = source;
        result.error = std::string("Internal error in the reordering analysis: ") + exception.what();
        return false;
    }
    result.ok = true;
    return true;
}

} // namespace detail

AnalysisResult analyzeProgram(const std::string &source, const AnalysisOptions &options)
{
    detail::Context ctx;
    AnalysisResult result;
    detail::analyze(ctx, source, options, result);
    return result;
}

} // namespace reorder
} // namespace Kites
