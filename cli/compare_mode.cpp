#include "compare_mode.h"

#include "assembler/assembler.h"
#include "processor/rvss/rvss_processor.h"
#include "reorder/run_profile.h"
#include "reorder/suggestions.h"
#include "utils/utils.h"

#include <QApplication>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <streambuf>

namespace Kites
{
namespace
{

struct Pipeline
{
    const char *name;
    bool forwarding;
    bool hazardUnit;
};

const Pipeline kPipelines[] = {
    {"rv5-h-f", true, true},
    {"rv5-h-nf", false, true},
    {"rv5-nh-f", true, false},
    {"rv5-nh-nf", false, false},
};

constexpr unsigned kMaxCycles = 5000000; ///< a run that takes longer is stopped (no exit call, endless loop)

/// What a run leaves behind: the state we compare, and the measured profile (pipelines).
struct RunResult
{
    bool ok = false;
    std::string error;
    std::vector<uint64_t> state; ///< x1..x31, f0..f31, then the memory bytes
    uint64_t codeBytes = 0;      ///< size of the program's code
    reorder::RunProfile profile; ///< 5-stage pipelines only
};

/// A return address: a code address, so it moves when NOPs are added in front of it.
bool isCodeAddress(uint64_t value, uint64_t codeBytes)
{
    return value % 4 == 0 && value <= codeBytes;
}

/// Same registers and memory. ra (x1) may differ when both values are code addresses: the
/// reordered program can have more lines (added NOPs), so its return addresses move.
bool sameResult(const RunResult &expected, const RunResult &actual)
{
    if (!actual.ok || expected.state.size() != actual.state.size())
        return false;
    for (size_t i = 0; i < expected.state.size(); ++i)
    {
        if (expected.state[i] == actual.state[i])
            continue;
        const bool returnAddress = i == 0 && isCodeAddress(expected.state[i], expected.codeBytes) &&
                                   isCodeAddress(actual.state[i], actual.codeBytes);
        if (!returnAddress)
            return false;
    }
    return true;
}

/// Kites prints a line per cycle; throw it away instead of keeping it in memory.
class DiscardBuffer : public std::streambuf
{
  protected:
    int overflow(int c) override { return c; }
};

/// Hides std::cout while it exists.
class QuietOutput
{
  public:
    QuietOutput() : m_old(std::cout.rdbuf(&m_discard)) {}
    ~QuietOutput() { std::cout.rdbuf(m_old); }

  private:
    DiscardBuffer m_discard;
    std::streambuf *m_old;
};

reorder::PipelineVariant variantOf(const std::string &name)
{
    if (name == "rv5-h-nf")
        return reorder::PipelineVariant::HazardUnitNoForwarding;
    if (name == "rv5-nh-f")
        return reorder::PipelineVariant::NoHazardUnitForwarding;
    if (name == "rv5-nh-nf")
        return reorder::PipelineVariant::NoHazardUnitNoForwarding;
    return reorder::PipelineVariant::HazardUnitForwarding;
}

/// The single-cycle processor, stepped one instruction at a time with the same end rules as
/// the measured pipeline runs (src/reorder/run_profile.h), and without keeping Undo history.
class ReferenceProcessor : public RVSSProcessor
{
  public:
    /// Runs to the end; returns "" or why it stopped early.
    std::string runToEnd(const AssembledProgram &program)
    {
        ClearStop();
        for (unsigned step = 0; step < kMaxCycles; ++step)
        {
            if (stop_requested_ || program_counter_ >= program_size_)
                return "";
            const uint64_t pc = program_counter_;
            const uint32_t instruction = program.text_buffer[pc / 4];
            Step();
            while (!undo_stack_.empty())
                undo_stack_.pop();
            if (reorder::jumpsToItself(instruction) && program_counter_ == pc)
                return "";
            if ((instruction & 0x7f) == 0x67 && program_counter_ == 0) // jalr back to the start (ra = 0)
                return "";
        }
        return "did not finish within " + std::to_string(kMaxCycles) + " instructions";
    }
};

RunResult run(const std::string &source, const std::string &processor, uint64_t memStart, uint64_t memBytes)
{
    RunResult result;
    QuietOutput quiet;
    const ProcessorBase *vm = nullptr;
    std::unique_ptr<ReferenceProcessor> reference;
    std::unique_ptr<reorder::ProfiledRun> pipeline;
    try
    {
        if (processor == "rvss")
        {
            std::istringstream stream(source);
            const AssembledProgram program = assemble(stream);
            reference = std::make_unique<ReferenceProcessor>();
            reference->step_delay_ = 0;
            reference->LoadProgram(program);
            result.error = reference->runToEnd(program);
            vm = reference.get();
        }
        else
        {
            pipeline = std::make_unique<reorder::ProfiledRun>(source, variantOf(processor), kMaxCycles);
            while (pipeline->step(100000))
            {
            }
            result.profile = pipeline->profile();
            result.error = result.profile.error;
            if (result.error.empty() && !result.profile.finished)
                result.error = "did not finish within " + std::to_string(kMaxCycles) + " cycles";
            vm = pipeline->processor();
        }
    }
    catch (const std::exception &e)
    {
        result.error = std::string("does not assemble: ") + e.what();
    }
    if (!result.error.empty() || !vm)
    {
        if (result.error.empty())
            result.error = "does not assemble";
        return result;
    }

    result.codeBytes = vm->program_.text_buffer.size() * 4;
    for (size_t r = 1; r < 32; ++r)
        result.state.push_back(vm->registers_.ReadGpr(r));
    for (size_t r = 0; r < 32; ++r)
    {
        // Kites' pipelines NaN-box single-precision values (upper 32 bits all ones) and the
        // single-cycle processor does not; both mean the same float, so compare the lower half then.
        uint64_t value = vm->registers_.ReadFpr(r);
        if ((value >> 32) == 0xffffffffULL)
            value &= 0xffffffffULL;
        result.state.push_back(value);
    }
    for (uint64_t a = 0; a < memBytes; ++a)
        result.state.push_back(const_cast<ProcessorBase *>(vm)->memory_controller_.peekByte(memStart + a));
    result.ok = true;
    return result;
}

/// The first differences between two runs, e.g. "x7: 6 vs 10000001".
std::string differences(const RunResult &expected, const RunResult &actual)
{
    if (!actual.ok)
    {
        return actual.error;
    }
    std::ostringstream out;
    int shown = 0;
    for (size_t i = 0; i < expected.state.size() && shown < 4; ++i)
    {
        if (expected.state[i] == actual.state[i])
            continue;
        if (i < 31)
            out << "x" << i + 1;
        else if (i < 63)
            out << "f" << i - 31;
        else
            out << "mem+" << i - 63;
        out << ": " << std::hex << expected.state[i] << " vs " << actual.state[i] << std::dec << "  ";
        ++shown;
    }
    return out.str();
}

/// Lines with measured stalls, most first: "line  ran  stalls  caused".
void printLines(const std::string &title, const reorder::RunProfile &profile)
{
    std::vector<std::pair<int, reorder::LineRun>> rows(profile.lines.begin(), profile.lines.end());
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second.stalls > b.second.stalls; });
    std::cout << "    " << title << ": line  ran  stalls  caused\n";
    for (const auto &[line, run] : rows)
    {
        if (run.stalls > 0 || run.caused > 0)
            std::cout << "      " << std::setw(6) << line << std::setw(5) << run.executed << std::setw(8) << run.stalls
                      << std::setw(8) << run.caused << "\n";
    }
    if (profile.unexplained > 0)
        std::cout << "      " << profile.unexplained << " stall cycle(s) with no instruction ahead that writes what "
                     "the waiting line reads (Kites' hazard unit)\n";
}

void usage()
{
    std::cerr << "usage: kites-cli compare FILE [--vm rv5-h-f|rv5-h-nf|rv5-nh-f|rv5-nh-nf|all] [--window N]\n"
                 "                              [--fixed] [--lines] [--mem START_HEX BYTES] [--write-reordered PREFIX]\n";
}

} // namespace

int runCompareMode(int argc, char *argv[])
{
    // The processors draw their circuit diagrams, which needs a (here invisible) GUI application.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    setupVmStateDirectory();

    if (argc < 3)
    {
        usage();
        return 2;
    }
    const std::string file = argv[2];
    std::string which = "all";
    std::string writePrefix;
    int window = 16;
    bool sliding = true;    // --fixed: the P-1 fixed windows
    bool showLines = false; // --lines: measured stalls per line
    uint64_t memStart = 0x10000000;
    uint64_t memBytes = 256;
    for (int i = 3; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--vm" && i + 1 < argc)
            which = argv[++i];
        else if (arg == "--window" && i + 1 < argc)
            window = std::stoi(argv[++i]);
        else if (arg == "--mem" && i + 2 < argc)
        {
            memStart = std::stoull(argv[++i], nullptr, 16);
            memBytes = std::stoull(argv[++i]);
        }
        else if (arg == "--fixed")
            sliding = false;
        else if (arg == "--lines")
            showLines = true;
        else if (arg == "--write-reordered" && i + 1 < argc)
            writePrefix = argv[++i];
        else
        {
            usage();
            return 2;
        }
    }
    std::ifstream in(file);
    if (!in)
    {
        std::cerr << "cannot open " << file << "\n";
        return 2;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string original = buffer.str();

    const RunResult reference = run(original, "rvss", memStart, memBytes);
    if (!reference.ok)
    {
        std::cerr << file << ": " << reference.error << "\n";
        return 2;
    }

    std::cout << "file " << file << "  (window " << window << (sliding ? " sliding" : " fixed") << ", memory 0x" << std::hex << memStart << std::dec << " + "
              << memBytes << " bytes)\n";
    std::cout << std::left << std::setw(11) << "pipeline" << std::setw(16) << "stalls (model)" << std::setw(18)
              << "cycles (Kites)" << std::setw(18) << "stalls (Kites)" << std::setw(20) << "original on pipe" << std::setw(20) << "reordered on rvss"
              << "reordered on pipeline\n";
    bool allMatch = true;
    for (const Pipeline &pipeline : kPipelines)
    {
        if (which != "all" && which != pipeline.name)
            continue;
        suggestions::Options options;
        options.forwarding = pipeline.forwarding;
        options.hazardUnit = pipeline.hazardUnit;
        options.window = window;
        options.slidingWindows = sliding;
        const suggestions::Result analysis = suggestions::analyze(original, options);
        const std::string reordered = suggestions::applyAll(original, options);
        if (!writePrefix.empty())
        {
            std::ofstream(writePrefix + "_" + pipeline.name + ".s") << reordered;
        }
        const RunResult before = run(original, pipeline.name, memStart, memBytes);
        const RunResult afterSingle = run(reordered, "rvss", memStart, memBytes);
        const RunResult after = run(reordered, pipeline.name, memStart, memBytes);
        const bool singleMatch = sameResult(reference, afterSingle);
        const bool pipelineMatch = sameResult(reference, after);
        const bool originalMatch = sameResult(reference, before);
        allMatch = allMatch && singleMatch && pipelineMatch;
        std::ostringstream stalls;
        stalls << analysis.stallsBefore << " -> " << analysis.stallsAfter;

        // "original on pipe" is informational: a plain program without NOPs is not meant to be
        // correct on a pipeline without a hazard unit.
        // measured: stall cycles with a hazard unit, executed NOPs without one
        const reorder::RunProfile &measuredBefore = before.profile;
        const reorder::RunProfile &measuredAfter = after.profile;
        auto measured = [&](const reorder::RunProfile &profile) {
            return profile.ok ? std::to_string(pipeline.hazardUnit ? profile.stalls : profile.nops) : std::string("-");
        };
        // cycles of the measured runs (they also end lab programs that have no exit call)
        std::ostringstream cycles;
        cycles << (measuredBefore.ok ? std::to_string(measuredBefore.cycles) : std::string("-")) << " -> "
               << (measuredAfter.ok ? std::to_string(measuredAfter.cycles) : std::string("-"));
        const std::string kitesStalls = measured(measuredBefore) + " -> " + measured(measuredAfter) +
                                        (pipeline.hazardUnit ? "" : " nop");
        std::cout << std::left << std::setw(11) << pipeline.name << std::setw(16) << stalls.str() << std::setw(18)
                  << cycles.str() << std::setw(18) << kitesStalls << std::setw(20) << (originalMatch ? "same" : "different")
                  << std::setw(20) << (singleMatch ? "same" : "DIFFERENT") << (pipelineMatch ? "same" : "DIFFERENT")
                  << "\n";
        if (!singleMatch)
            std::cout << "    reordered on rvss: " << differences(reference, afterSingle) << "\n";
        if (!pipelineMatch)
            std::cout << "    reordered on " << pipeline.name << ": " << differences(reference, after) << "\n";
        if (measuredBefore.endReason != measuredAfter.endReason || measuredBefore.endReason != "exit ecall")
            std::cout << "    measured runs ended: " << measuredBefore.endReason << " / " << measuredAfter.endReason << "\n";
        if (showLines && pipeline.hazardUnit)
        {
            printLines("original", measuredBefore);
            printLines("reordered", measuredAfter);
        }
    }
    std::cout << (allMatch ? "OK: every reordered program gives the reference registers and memory\n"
                           : "FAILED: a reordered program differs from the reference\n");
    return allMatch ? 0 : 1;
}

} // namespace Kites
