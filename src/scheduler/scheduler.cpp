/**
 * @file scheduler.cpp
 * @brief List scheduling on Kites' parsed program (see scheduler.h).
 *
 * The steps, in the order analyze() runs them:
 *   1. readProgram()  editor text -> Kites assembler -> one Unit per source line
 *   2. findBlocks()   basic blocks: a label, a branch/jump, or an instruction we must not move ends a block
 *   3. per block, per window of W units:
 *        buildGraph()     weighted dependence graph (RAW = latency, other edges = keep the order)
 *        listSchedule()   one order with the height rule, one with the succ rule
 *        pick the order with the fewest stalls, then the smallest D (original order wins ties)
 *   4. simulate()     exact timing, instruction by instruction, carried across windows and blocks
 *   5. NOPs: the student's NOPs are taken out; only the NOPs still needed are written back
 *   6. explain every hazard that got shorter: Moved / Why / Effect
 *
 * Latencies (P-1 report, Table 1): with forwarding a load's result can be used 2 slots
 * later and any other result 1 slot later; without forwarding every result 3 slots later.
 */
#include "scheduler.h"

#include "assembler/assembler.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>

namespace Kites
{
namespace scheduler
{
namespace
{

constexpr int kRegisters = 64; // x0..x31 = 0..31, f0..f31 = 32..63

// ---------------------------------------------------------------------------------------
// Program model
// ---------------------------------------------------------------------------------------

enum class Kind
{
    Alu,
    Load,
    Store,
    Branch,
    Jump,
    System
};

/// One machine instruction, as Kites' assembler produced it.
struct Op
{
    Kind kind = Kind::Alu;
    int rd = -1;                 ///< register written, -1 if none (x0 counts as none)
    std::vector<int> sources;    ///< registers read (x0 left out)
    int base = -1;               ///< loads/stores: base register
    long long offset = 0;
    bool offsetKnown = false;
    int bytes = 0;
    bool unconditionalJump = false; ///< jal/jalr with rd = x0: the next line is reached from elsewhere
};

/// One source line that holds an instruction. Pseudo-instructions (la, call, ...) stay one unit.
struct Unit
{
    int line = 0;           ///< 0-based editor line
    std::vector<Op> ops;
    std::string code;       ///< instruction text without label and comment
    bool barrier = false;   ///< never moves, and nothing moves across it
    bool endsBlock = false; ///< branch, jump, ecall: the block ends after it
};

/// What we need to know about every editor line.
struct Line
{
    std::string text;
    int labelEnd = -1;       ///< position just after the last label ':' (-1: no label)
    bool directive = false;  ///< .data, .word, ...
    bool studentNop = false; ///< a line with just "nop" (taken out before scheduling)
    int unit = -1;           ///< index into units, -1 if the line has no (kept) instruction
    int nopGap = -1;         ///< for studentNop lines: the unit they stand in front of
};

struct Program
{
    std::vector<Line> lines;
    bool endsWithNewline = false;
    std::vector<Unit> units;
    std::vector<std::pair<int, int>> blocks;   ///< [first, end) unit ranges
    std::vector<std::vector<int>> gapNops;     ///< gapNops[k] = student NOP lines in front of unit k
};

std::string trim(const std::string &text)
{
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos)
    {
        return "";
    }
    const size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

bool isLabelCharacter(char c, bool first)
{
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '$';
    return letter || (!first && c >= '0' && c <= '9');
}

/// Reads labels, directive and "nop" from one editor line.
Line describeLine(const std::string &text)
{
    Line line;
    line.text = text;
    const size_t comment = text.find_first_of("#;");
    const std::string code = comment == std::string::npos ? text : text.substr(0, comment);
    size_t pos = 0;
    while (true) // labels: "name:" (several are allowed)
    {
        size_t start = code.find_first_not_of(" \t", pos);
        if (start == std::string::npos)
        {
            break;
        }
        size_t end = start;
        while (end < code.size() && isLabelCharacter(code[end], end == start))
        {
            ++end;
        }
        size_t colon = code.find_first_not_of(" \t", end);
        if (end == start || colon == std::string::npos || code[colon] != ':')
        {
            break;
        }
        line.labelEnd = static_cast<int>(colon) + 1;
        pos = colon + 1;
    }
    const std::string rest = trim(code.substr(pos));
    line.directive = !rest.empty() && rest[0] == '.';
    line.studentNop = rest == "nop";
    return line;
}

int registerNumber(const std::string &name)
{
    static const char *abi[32] = {"zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0",
                                  "a1",   "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5",
                                  "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
    static const char *fabi[32] = {"ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7", "fs0",  "fs1", "fa0",
                                   "fa1", "fa2", "fa3", "fa4", "fa5", "fa6", "fa7", "fs2", "fs3", "fs4", "fs5",
                                   "fs6", "fs7", "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11"};
    if (name.size() >= 2 && (name[0] == 'x' || name[0] == 'f') && std::isdigit(static_cast<unsigned char>(name[1])))
    {
        char *end = nullptr;
        const long n = std::strtol(name.c_str() + 1, &end, 10);
        if (*end == '\0' && n >= 0 && n < 32)
        {
            return static_cast<int>(n) + (name[0] == 'f' ? 32 : 0);
        }
        return -1;
    }
    if (name == "fp")
    {
        return 8;
    }
    for (int i = 0; i < 32; ++i)
    {
        if (name == abi[i])
        {
            return i;
        }
        if (name == fabi[i])
        {
            return 32 + i;
        }
    }
    return -1;
}

std::string abiName(int reg)
{
    static const char *abi[32] = {"zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0",
                                  "a1",   "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5",
                                  "s6",   "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
    if (reg >= 0 && reg < 32)
    {
        return abi[reg];
    }
    return reg >= 32 && reg < 64 ? "f" + std::to_string(reg - 32) : "?";
}

/// The register as the student wrote it in this instruction (t0 or x5).
std::string spelling(const std::string &code, int reg)
{
    std::string token;
    for (size_t i = 0; i <= code.size(); ++i)
    {
        const char c = i < code.size() ? code[i] : ' ';
        if (c == ' ' || c == '\t' || c == ',' || c == '(' || c == ')')
        {
            if (!token.empty() && registerNumber(token) == reg)
            {
                return token;
            }
            token.clear();
        }
        else
        {
            token += c;
        }
    }
    return abiName(reg);
}

int fieldRegister(const std::array<char, 6> &field)
{
    const std::string name(field.data());
    const int reg = name.empty() ? -1 : registerNumber(name);
    return reg == 0 ? -1 : reg; // x0 is always zero: no dependence
}

Op decode(const ICUnit &unit)
{
    Op op;
    const std::string name(unit.opcode.data());
    static const std::map<std::string, int> loads = {{"lb", 1}, {"lbu", 1}, {"lh", 2}, {"lhu", 2}, {"lw", 4},
                                                     {"lwu", 4}, {"ld", 8}, {"flw", 4}, {"fld", 8}};
    static const std::map<std::string, int> stores = {{"sb", 1}, {"sh", 2}, {"sw", 4}, {"sd", 8}, {"fsw", 4}, {"fsd", 8}};
    if (loads.count(name))
    {
        op.kind = Kind::Load;
        op.bytes = loads.at(name);
    }
    else if (stores.count(name))
    {
        op.kind = Kind::Store;
        op.bytes = stores.at(name);
    }
    else if (name == "beq" || name == "bne" || name == "blt" || name == "bge" || name == "bltu" || name == "bgeu")
    {
        op.kind = Kind::Branch;
    }
    else if (name == "jal" || name == "jalr")
    {
        op.kind = Kind::Jump;
    }
    else if (name == "ecall" || name == "ebreak" || name == "fence" || name == "fence.i" || name.rfind("csr", 0) == 0 ||
             name == "mret" || name == "sret" || name == "uret" || name == "wfi")
    {
        op.kind = Kind::System;
    }
    op.rd = (op.kind == Kind::Store || op.kind == Kind::Branch) ? -1 : fieldRegister(unit.rd);
    for (const std::array<char, 6> *field : {&unit.rs1, &unit.rs2, &unit.rs3})
    {
        const int reg = fieldRegister(*field);
        if (reg >= 0)
        {
            op.sources.push_back(reg);
        }
    }
    if (op.kind == Kind::Load || op.kind == Kind::Store)
    {
        op.base = fieldRegister(unit.rs1);
        char *end = nullptr;
        const std::string imm(unit.imm.data());
        op.offset = std::strtoll(imm.c_str(), &end, 0);
        op.offsetKnown = !imm.empty() && *end == '\0';
    }
    op.unconditionalJump = op.kind == Kind::Jump && std::string(unit.rd.data()) == "x0";
    return op;
}

/// Step 1: Kites' assembler reads the program; we keep one unit per source line.
bool readProgram(const std::string &source, Program &program, std::string &error)
{
    {
        std::string current;
        for (char c : source)
        {
            if (c == '\n')
            {
                program.lines.push_back(describeLine(current));
                current.clear();
            }
            else
            {
                current += c;
            }
        }
        program.endsWithNewline = !source.empty() && source.back() == '\n';
        if (!program.endsWithNewline)
        {
            program.lines.push_back(describeLine(current));
        }
    }

    AssembledProgram assembled;
    try
    {
        std::istringstream stream(source);
        assembled = assemble(stream);
    }
    catch (const std::exception &)
    {
        error = "The program does not assemble. Fix the errors shown in the editor first.";
        return false;
    }

    std::map<int, int> unitOfLine; // 0-based line -> unit
    for (const auto &entry : assembled.intermediate_code)
    {
        const ICUnit &ic = entry.first;
        const int line = static_cast<int>(ic.line_number) - 1;
        if (line < 0 || line >= static_cast<int>(program.lines.size()))
        {
            continue;
        }
        if (!unitOfLine.count(line))
        {
            unitOfLine[line] = static_cast<int>(program.units.size());
            Unit unit;
            unit.line = line;
            const Line &text = program.lines[line];
            const size_t from = text.labelEnd < 0 ? 0 : static_cast<size_t>(text.labelEnd);
            const size_t comment = text.text.find_first_of("#;", from);
            unit.code = trim(text.text.substr(from, comment == std::string::npos ? std::string::npos : comment - from));
            program.units.push_back(unit);
        }
        program.units[unitOfLine[line]].ops.push_back(decode(ic));
    }

    // Classify units; take the student's NOPs out.
    std::vector<Unit> kept;
    for (Unit &unit : program.units)
    {
        Line &line = program.lines[unit.line];
        if (line.studentNop && unit.ops.size() == 1)
        {
            continue; // re-attached to a gap below
        }
        const std::string mnemonic = unit.code.substr(0, unit.code.find_first_of(" \t"));
        for (const Op &op : unit.ops)
        {
            unit.barrier = unit.barrier || op.kind == Kind::System;
        }
        unit.barrier = unit.barrier || mnemonic == "auipc"; // its value depends on its own address
        const Kind last = unit.ops.back().kind;
        unit.endsBlock = last == Kind::Branch || last == Kind::Jump || last == Kind::System;
        line.unit = static_cast<int>(kept.size());
        kept.push_back(unit);
    }
    program.units = kept;

    program.gapNops.assign(program.units.size() + 1, {});
    int next = 0;
    for (size_t l = 0; l < program.lines.size(); ++l)
    {
        Line &line = program.lines[l];
        if (line.unit >= 0)
        {
            next = line.unit + 1;
        }
        else if (line.studentNop)
        {
            line.nopGap = next;
            program.gapNops[next].push_back(static_cast<int>(l));
        }
    }
    return true;
}

/// Step 2: basic blocks. A label or directive, a branch/jump before, or a barrier starts a new block.
void findBlocks(Program &program)
{
    const int count = static_cast<int>(program.units.size());
    int start = 0;
    for (int i = 1; i <= count; ++i)
    {
        bool newBlock = i == count;
        if (!newBlock)
        {
            const Unit &before = program.units[i - 1];
            const Unit &unit = program.units[i];
            newBlock = before.endsBlock || before.barrier || unit.barrier;
            for (int l = before.line + 1; l <= unit.line && !newBlock; ++l)
            {
                newBlock = program.lines[l].labelEnd >= 0 || program.lines[l].directive;
            }
        }
        if (newBlock)
        {
            program.blocks.push_back({start, i});
            start = i;
        }
    }
}

// ---------------------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------------------

/// Slots after the producer at which a reader may enter the pipeline (P-1 report, Table 1).
int latency(const Op &producer, bool forwarding)
{
    if (!forwarding)
    {
        return 3; // value written in the first half of WB, read in the second half of ID
    }
    return producer.kind == Kind::Load ? 2 : 1;
}

struct State
{
    std::array<int, kRegisters> ready{};  ///< slot from which the register can be read
    std::array<int, kRegisters> writer{}; ///< unit that wrote it last (-1: none / not tracked)
    std::array<int, kRegisters> slot{};   ///< slot of that writing instruction
    int next = 0;                         ///< next free issue slot
    State()
    {
        writer.fill(-1);
    }
};

struct Timing
{
    int fetch = 0;      ///< first free slot when the unit arrives (its IF)
    int stalls = 0;     ///< all cycles it waits
    int external = 0;   ///< waits for other units (NOPs can cover these)
    int internal = 0;   ///< waits inside a pseudo-instruction (la, li, ...): NOPs cannot help
    int producer = -1;  ///< unit it waits for first
    int reg = -1;
    int needed = 0;     ///< slots the producer needs before the reader
};

/// Issue one unit in program order; with a hazard unit it waits, the waiting cycles are its stalls.
Timing issue(const Program &program, int id, State &state, bool forwarding)
{
    Timing timing;
    timing.fetch = state.next;
    for (const Op &op : program.units[id].ops)
    {
        int start = state.next;
        int cause = -1;
        int reg = -1;
        for (int source : op.sources)
        {
            if (state.writer[source] >= 0 && state.ready[source] > start)
            {
                start = state.ready[source];
                cause = state.writer[source];
                reg = source;
            }
        }
        const int waited = start - state.next;
        if (waited > 0 && cause == id)
        {
            timing.internal += waited;
        }
        else if (waited > 0)
        {
            timing.external += waited;
            if (timing.producer < 0)
            {
                timing.producer = cause;
                timing.reg = reg;
                timing.needed = state.ready[reg] - state.slot[reg];
            }
        }
        if (op.rd >= 0)
        {
            state.ready[op.rd] = start + latency(op, forwarding);
            state.writer[op.rd] = id;
            state.slot[op.rd] = start;
        }
        state.next = start + 1;
        if (op.unconditionalJump)
        {
            state.writer.fill(-1);
        }
    }
    timing.stalls = timing.external + timing.internal;
    return timing;
}

int stallsOf(const Program &program, const std::vector<int> &order, State state, bool forwarding)
{
    int stalls = 0;
    for (int id : order)
    {
        stalls += issue(program, id, state, forwarding).stalls;
    }
    return stalls;
}

// ---------------------------------------------------------------------------------------
// Dependence graph and list scheduling (P-1 report, Sections 4 and 5)
// ---------------------------------------------------------------------------------------

struct Graph
{
    int n = 0;
    std::vector<std::vector<int>> weight; ///< weight[u][v]: v at least this many slots after u, -1 = no edge
    std::vector<int> external;            ///< earliest slot because of producers before the window
    std::vector<int> height;
    std::vector<int> successors;
    std::vector<int> size;                ///< machine instructions of each node
};

bool mayOverlap(const Op &a, int aBaseWriter, const Op &b, int bBaseWriter)
{
    if (a.base < 0 || a.base != b.base || aBaseWriter != bBaseWriter || !a.offsetKnown || !b.offsetKnown)
    {
        return true; // cannot prove the addresses differ
    }
    return !(a.offset + a.bytes <= b.offset || b.offset + b.bytes <= a.offset);
}

const Op *memoryOp(const Unit &unit)
{
    for (const Op &op : unit.ops)
    {
        if (op.kind == Kind::Load || op.kind == Kind::Store)
        {
            return &op;
        }
    }
    return nullptr;
}

/// Nodes are the units first..first+n-1. RAW edges carry the latency, the others only keep the order.
Graph buildGraph(const Program &program, int first, int n, const State &state, bool forwarding)
{
    Graph graph;
    graph.n = n;
    graph.weight.assign(n, std::vector<int>(n, -1));
    graph.external.assign(n, 0);
    graph.height.assign(n, 0);
    graph.successors.assign(n, 0);
    graph.size.assign(n, 0);
    auto addEdge = [&](int u, int v, int w) { graph.weight[u][v] = std::max(graph.weight[u][v], w); };

    std::array<int, kRegisters> lastWriter;
    lastWriter.fill(-1);
    std::vector<int> baseWriter(n, -1);
    for (int v = 0; v < n; ++v)
    {
        const Unit &unit = program.units[first + v];
        graph.size[v] = static_cast<int>(unit.ops.size());
        std::vector<int> writesSoFar;
        for (const Op &op : unit.ops)
        {
            for (int reg : op.sources)
            {
                if (std::find(writesSoFar.begin(), writesSoFar.end(), reg) != writesSoFar.end())
                {
                    continue; // produced inside this unit
                }
                const int u = lastWriter[reg];
                if (u >= 0)
                {
                    // RAW: slots from u's first instruction to the reader = position of the writing op + latency
                    const Unit &producer = program.units[first + u];
                    int position = 0;
                    int w = 1;
                    for (size_t k = 0; k < producer.ops.size(); ++k)
                    {
                        if (producer.ops[k].rd == reg)
                        {
                            position = static_cast<int>(k);
                            w = position + latency(producer.ops[k], forwarding);
                        }
                    }
                    addEdge(u, v, w);
                }
                else if (state.writer[reg] >= 0)
                {
                    graph.external[v] = std::max(graph.external[v], state.ready[reg]);
                }
            }
            if (op.rd >= 0)
            {
                writesSoFar.push_back(op.rd);
            }
        }
        for (int u = 0; u < v; ++u) // WAR and WAW: only the order matters
        {
            const Unit &earlier = program.units[first + u];
            for (const Op &a : earlier.ops)
            {
                for (const Op &b : unit.ops)
                {
                    const bool war = b.rd >= 0 && std::find(a.sources.begin(), a.sources.end(), b.rd) != a.sources.end();
                    const bool waw = b.rd >= 0 && a.rd == b.rd;
                    if (war || waw)
                    {
                        addEdge(u, v, graph.size[u]);
                    }
                }
            }
        }
        if (const Op *mine = memoryOp(unit)) // memory: a store and a load/store that may overlap
        {
            baseWriter[v] = mine->base >= 0 ? lastWriter[mine->base] : -1;
            for (int u = 0; u < v; ++u)
            {
                const Op *other = memoryOp(program.units[first + u]);
                if (other && (other->kind == Kind::Store || mine->kind == Kind::Store) &&
                    mayOverlap(*other, baseWriter[u], *mine, baseWriter[v]))
                {
                    addEdge(u, v, graph.size[u]);
                }
            }
        }
        if (unit.endsBlock) // the branch/jump stays last
        {
            for (int u = 0; u < v; ++u)
            {
                addEdge(u, v, graph.size[u]);
            }
        }
        for (const Op &op : unit.ops)
        {
            if (op.rd >= 0)
            {
                lastWriter[op.rd] = v;
            }
        }
    }
    for (int u = n - 1; u >= 0; --u) // height: longest path to the end, in slots
    {
        graph.height[u] = graph.size[u];
        for (int v = u + 1; v < n; ++v)
        {
            if (graph.weight[u][v] >= 0)
            {
                graph.successors[u]++;
                graph.height[u] = std::max(graph.height[u], graph.weight[u][v] + graph.height[v]);
            }
        }
    }
    return graph;
}

enum class Rule
{
    Height,
    Successors
};

/// Rule R3 of the report: place the ready node that can start earliest; ties by the rule, then program order.
std::vector<int> listSchedule(const Graph &graph, int startSlot, Rule rule)
{
    const int n = graph.n;
    std::vector<int> earliest = graph.external;
    std::vector<int> predecessorsLeft(n, 0);
    for (int u = 0; u < n; ++u)
    {
        for (int v = 0; v < n; ++v)
        {
            predecessorsLeft[v] += graph.weight[u][v] >= 0 ? 1 : 0;
        }
    }
    std::vector<bool> placed(n, false);
    std::vector<int> order;
    int t = startSlot;
    for (int step = 0; step < n; ++step)
    {
        int best = -1;
        int bestStart = 0;
        for (int v = 0; v < n; ++v)
        {
            if (placed[v] || predecessorsLeft[v] > 0)
            {
                continue;
            }
            const int start = std::max(t, earliest[v]);
            bool better = best < 0 || start < bestStart;
            if (!better && start == bestStart)
            {
                const int primaryV = rule == Rule::Height ? graph.height[v] : graph.successors[v];
                const int primaryB = rule == Rule::Height ? graph.height[best] : graph.successors[best];
                const int secondaryV = rule == Rule::Height ? graph.successors[v] : graph.height[v];
                const int secondaryB = rule == Rule::Height ? graph.successors[best] : graph.height[best];
                better = primaryV > primaryB || (primaryV == primaryB && secondaryV > secondaryB);
            }
            if (better)
            {
                best = v;
                bestStart = start;
            }
        }
        order.push_back(best);
        placed[best] = true;
        t = bestStart + graph.size[best];
        for (int v = 0; v < n; ++v)
        {
            if (graph.weight[best][v] >= 0)
            {
                predecessorsLeft[v]--;
                earliest[v] = std::max(earliest[v], bestStart + graph.weight[best][v]);
            }
        }
    }
    return order;
}

// ---------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------

std::string leadingSpace(const std::string &text)
{
    return text.substr(0, text.find_first_not_of(" \t"));
}

/// The instruction (with its comment) of `unit`, written on line `slotLine`.
std::string render(const Program &program, int unit, int slotLine)
{
    const Line &slot = program.lines[slotLine];
    if (slot.unit == unit)
    {
        return slot.text;
    }
    const Line &source = program.lines[program.units[unit].line];
    std::string part = source.text.substr(source.labelEnd < 0 ? 0 : source.labelEnd);
    part = part.substr(std::min(part.size(), part.find_first_not_of(" \t")));
    if (slot.labelEnd >= 0)
    {
        std::string gap = leadingSpace(slot.text.substr(slot.labelEnd));
        return slot.text.substr(0, slot.labelEnd) + (gap.empty() ? " " : gap) + part;
    }
    return leadingSpace(slot.text) + part;
}

std::string plural(int n, const std::string &word)
{
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

// ---------------------------------------------------------------------------------------
// One block
// ---------------------------------------------------------------------------------------

struct BlockWork
{
    std::vector<int> order;               ///< new order (unit ids)
    std::vector<Timing> before;           ///< per position, original order without student NOPs
    std::vector<Timing> after;            ///< per position, new order
    std::vector<Timing> withStudentNops;  ///< per position, original order with the student's NOPs
};

BlockWork scheduleBlock(const Program &program, int first, int end, const State &entry, const Options &options)
{
    BlockWork work;
    const bool fwd = options.forwarding;
    State state = entry;
    for (int ws = first; ws < end; ws += options.window)
    {
        const int we = std::min(end, ws + options.window);
        const int n = we - ws;
        std::vector<int> candidates[3];
        for (int k = 0; k < n; ++k)
        {
            candidates[0].push_back(k);
        }
        if (n > 1 && !program.units[ws].barrier)
        {
            const Graph graph = buildGraph(program, ws, n, state, fwd);
            candidates[1] = listSchedule(graph, state.next, Rule::Height);
            candidates[2] = listSchedule(graph, state.next, Rule::Successors);
        }
        // Rule R5: fewest stalls (window + rest of the block as written), then smallest D; original wins ties.
        int best = 0;
        int bestStalls = -1;
        int bestMoves = 0;
        for (int c = 0; c < 3; ++c)
        {
            if (candidates[c].empty())
            {
                continue;
            }
            std::vector<int> ids;
            int moves = 0;
            for (int k = 0; k < n; ++k)
            {
                ids.push_back(ws + candidates[c][k]);
                moves += std::abs(candidates[c][k] - k);
            }
            for (int rest = we; rest < end; ++rest)
            {
                ids.push_back(rest);
            }
            const int stalls = stallsOf(program, ids, state, fwd);
            if (bestStalls < 0 || stalls < bestStalls || (stalls == bestStalls && moves < bestMoves))
            {
                best = c;
                bestStalls = stalls;
                bestMoves = moves;
            }
        }
        for (int k = 0; k < n; ++k) // carry the state into the next window
        {
            work.order.push_back(ws + candidates[best][k]);
            issue(program, work.order.back(), state, fwd);
        }
    }

    State original = entry;
    State reordered = entry;
    State student = entry;
    for (int k = first; k < end; ++k)
    {
        work.before.push_back(issue(program, k, original, fwd));
        work.after.push_back(issue(program, work.order[k - first], reordered, fwd));
        student.next += static_cast<int>(program.gapNops[k].size());
        work.withStudentNops.push_back(issue(program, k, student, fwd));
    }
    return work;
}

/// Where NOPs for the first instruction of a block go: in front of the labels above it,
/// so they run once on the way in and not on every loop iteration.
int nopInsertionLine(const Program &program, int unitLine, bool blockStart)
{
    if (!blockStart)
    {
        return unitLine;
    }
    int best = unitLine;
    for (int l = unitLine - 1; l >= 0; --l)
    {
        const Line &line = program.lines[l];
        if (line.unit >= 0 || line.directive)
        {
            break;
        }
        if (line.labelEnd >= 0)
        {
            best = l;
        }
    }
    return best;
}

} // namespace

// ---------------------------------------------------------------------------------------
// analyze
// ---------------------------------------------------------------------------------------

Result analyze(const std::string &source, const Options &requested)
{
    Result result;
    result.options = requested;
    result.options.window = std::clamp(requested.window, 1, 64);
    const Options &options = result.options;
    const bool fwd = options.forwarding;
    const std::string unitWord = options.hazardUnit ? "stall cycle" : "NOP";

    Program program;
    if (!readProgram(source, program, result.error))
    {
        return result;
    }
    findBlocks(program);
    for (const Line &line : program.lines)
    {
        result.studentNops += line.studentNop ? 1 : 0;
    }

    State entry; // the program as written, up to the current block (with the student's NOPs)
    for (size_t b = 0; b < program.blocks.size(); ++b)
    {
        const int first = program.blocks[b].first;
        const int end = program.blocks[b].second;
        const BlockWork work = scheduleBlock(program, first, end, entry, options);

        // NOPs: the student's NOPs in front of a slot stay as far as they are still needed.
        std::map<int, std::vector<std::string>> insertBefore; // editor line -> NOP lines
        std::map<int, std::vector<std::string>> insertAfter;
        std::vector<bool> keepNop(program.lines.size(), true);
        bool nopsChange = false;
        bool internalHazard = false;
        for (int k = first; k < end; ++k)
        {
            const Timing &timing = work.after[k - first];
            internalHazard = internalHazard || timing.internal > 0;
            const std::vector<int> &gap = program.gapNops[k];
            const int needed = timing.external;
            const int kept = std::min(static_cast<int>(gap.size()), needed);
            for (size_t g = kept; g < gap.size(); ++g)
            {
                keepNop[gap[g]] = false;
                nopsChange = true;
            }
            if (needed > kept)
            {
                nopsChange = true;
                const Unit &unit = program.units[work.order[k - first]];
                const std::string indent = leadingSpace(program.lines[program.units[k].line].text);
                const std::string text = (indent.empty() ? std::string("    ") : indent) + "nop    # wait for " +
                                         spelling(unit.code, timing.reg);
                std::vector<std::string> &list =
                    kept > 0 ? insertAfter[gap[kept - 1]]
                             : insertBefore[nopInsertionLine(program, program.units[k].line, k == first)];
                for (int extra = kept; extra < needed; ++extra)
                {
                    list.push_back(text);
                }
            }
        }
        bool reordered = false;
        for (int k = first; k < end; ++k)
        {
            reordered = reordered || work.order[k - first] != k;
        }

        int stallsBefore = 0;
        int stallsAfter = 0;
        for (size_t k = 0; k < work.before.size(); ++k)
        {
            stallsBefore += work.before[k].stalls;
            stallsAfter += work.after[k].stalls;
        }
        result.stallsBefore += stallsBefore;
        result.stallsAfter += stallsAfter;

        if (internalHazard && !options.hazardUnit)
        {
            result.notes.push_back("Line " + std::to_string(program.units[first].line + 1) +
                                   "'s block uses a pseudo-instruction that Kites expands into two dependent "
                                   "instructions (la, call, li with a large value, ...). Without a hazard unit no "
                                   "NOP can be placed between them.");
        }

        if (reordered || (nopsChange && !options.hazardUnit))
        {
            BlockChange change;
            change.index = static_cast<int>(b);
            const int previousLast = first > 0 ? program.units[first - 1].line : -1;
            change.firstLine = previousLast + 2;
            change.lastLine = program.units[end - 1].line + 1;
            for (int l = change.firstLine - 1; l <= change.lastLine - 1; ++l)
            {
                const Line &line = program.lines[l];
                if (insertBefore.count(l))
                {
                    for (const std::string &nop : insertBefore[l])
                    {
                        change.newLines.push_back(nop);
                    }
                }
                if (line.unit >= 0)
                {
                    change.newLines.push_back(render(program, work.order[line.unit - first], l));
                }
                else if (line.studentNop && !keepNop[l])
                {
                    if (line.labelEnd >= 0)
                    {
                        change.newLines.push_back(line.text.substr(0, line.labelEnd));
                    }
                }
                else
                {
                    change.newLines.push_back(line.text);
                }
                if (insertAfter.count(l))
                {
                    for (const std::string &nop : insertAfter[l])
                    {
                        change.newLines.push_back(nop);
                    }
                }
            }
            change.stallsBefore = stallsBefore;
            change.stallsAfter = stallsAfter;

            // Timelines (cycle 0 = the block's first IF).
            const int base = work.before.front().fetch;
            std::vector<int> movedUnits;
            for (int k = first; k < end; ++k)
            {
                TimelineRow row;
                row.line = program.units[k].line + 1;
                row.text = program.units[k].code;
                row.fetchCycle = work.before[k - first].fetch - base;
                row.stalls = work.before[k - first].stalls;
                row.instructions = static_cast<int>(program.units[k].ops.size());
                change.before.push_back(row);
            }

            // Suggestions: every wait that got shorter, with the instruction that made it shorter.
            std::vector<int> position(end - first);
            for (int p = 0; p < end - first; ++p)
            {
                position[work.order[p] - first] = p;
            }
            auto newPos = [&](int unit) { return unit < first ? -1 : position[unit - first]; };
            for (int c = first; c < end; ++c)
            {
                const Timing &was = work.before[c - first];
                const int now = work.after[position[c - first]].stalls;
                if (was.external <= 0 || was.producer < 0 || was.external <= now)
                {
                    continue;
                }
                const int p = was.producer;
                int moved = -1;
                int distance = -1;
                for (int x = first; x < end; ++x) // instructions now between producer and consumer that were not before
                {
                    const bool betweenNow = newPos(p) < newPos(x) && newPos(x) < newPos(c);
                    const bool betweenBefore = p < x && x < c;
                    if (x != c && betweenNow && !betweenBefore && std::abs(newPos(x) - (x - first)) > distance)
                    {
                        moved = x;
                        distance = std::abs(newPos(x) - (x - first));
                    }
                }
                if (moved < 0)
                {
                    moved = newPos(c) != c - first ? c : p;
                }
                Suggestion s;
                s.block = static_cast<int>(result.blocks.size());
                s.movedLine = program.units[moved].line + 1;
                s.movedText = program.units[moved].code;
                s.producerLine = program.units[p].line + 1;
                s.producerText = program.units[p].code;
                s.consumerLine = program.units[c].line + 1;
                s.consumerText = program.units[c].code;
                s.reg = spelling(program.units[c].code, was.reg);
                s.slotsNeeded = was.needed;
                s.slotsPresent = was.needed - was.external;
                s.saved = was.external - std::min(was.external, now);
                s.wrongWithoutNops = !options.hazardUnit && work.withStudentNops[c - first].external > 0;
                s.why = s.producerText + " (line " + std::to_string(s.producerLine) + ") → " + s.consumerText +
                        " (line " + std::to_string(s.consumerLine) + ") on " + s.reg + ": needs " +
                        plural(s.slotsNeeded, "slot") + ", has " + std::to_string(s.slotsPresent);
                s.effect = "−" + plural(s.saved, unitWord);
                result.suggestions.push_back(s);
                if (std::find(movedUnits.begin(), movedUnits.end(), moved) == movedUnits.end())
                {
                    movedUnits.push_back(moved);
                }
            }
            const int afterBase = work.after.front().fetch;
            for (int p = 0; p < end - first; ++p)
            {
                const int unit = work.order[p];
                TimelineRow row;
                row.line = program.units[unit].line + 1;
                row.text = program.units[unit].code;
                row.fetchCycle = work.after[p].fetch - afterBase;
                row.stalls = work.after[p].stalls;
                row.instructions = static_cast<int>(program.units[unit].ops.size());
                row.moved = unit != first + p;
                row.fillsStall = std::find(movedUnits.begin(), movedUnits.end(), unit) != movedUnits.end();
                change.after.push_back(row);
            }
            result.blocks.push_back(change);
        }

        // The next block starts from the program as written, the student's NOPs included.
        // (The NOPs in front of the next block belong to that block: its Apply rewrites them.)
        for (int k = first; k < end; ++k)
        {
            entry.next += static_cast<int>(program.gapNops[k].size());
            issue(program, k, entry, fwd);
        }
    }

    std::sort(result.suggestions.begin(), result.suggestions.end(),
              [](const Suggestion &a, const Suggestion &b) { return a.consumerLine < b.consumerLine; });
    for (const Unit &unit : program.units)
    {
        if (unit.barrier)
        {
            result.notes.push_back("Line " + std::to_string(unit.line + 1) + " (" + unit.code +
                                   ") stays in place: nothing is moved across it.");
        }
    }
    result.ok = true;
    return result;
}

std::string applyBlock(const std::string &source, const BlockChange &change)
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
    const bool newline = !source.empty() && source.back() == '\n';
    if (!newline)
    {
        lines.push_back(current);
    }
    const int first = std::clamp(change.firstLine - 1, 0, static_cast<int>(lines.size()));
    const int last = std::clamp(change.lastLine, first, static_cast<int>(lines.size()));
    lines.erase(lines.begin() + first, lines.begin() + last);
    lines.insert(lines.begin() + first, change.newLines.begin(), change.newLines.end());
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        out += lines[i];
        if (i + 1 < lines.size() || newline)
        {
            out += '\n';
        }
    }
    return out;
}

std::string applyAll(const std::string &source, const Options &options)
{
    std::string text = source;
    for (int round = 0; round < 1000; ++round)
    {
        const Result result = analyze(text, options);
        if (!result.ok || result.blocks.empty())
        {
            break;
        }
        const std::string next = applyBlock(text, result.blocks.front());
        if (next == text)
        {
            break;
        }
        text = next;
    }
    return text;
}

} // namespace scheduler
} // namespace Kites
