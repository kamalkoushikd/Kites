#include "compare_mode.h"

#include "assembler/assembler.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "processor/rv5s/rv5s_processor_nh_f.h"
#include "processor/rv5s/rv5s_processor_nh_nf.h"
#include "processor/rvss/rvss_processor.h"
#include "scheduler/scheduler.h"
#include "utils/utils.h"

#include <QApplication>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

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

/// What a run leaves behind: the state we compare, and how long it took.
struct RunResult
{
    bool ok = false;
    std::string error;
    std::vector<uint64_t> state; ///< x1..x31, f0..f31, then the memory bytes
    unsigned cycles = 0;
    unsigned stalls = 0;
};

std::unique_ptr<ProcessorBase> makeProcessor(const std::string &name)
{
    if (name == "rv5-h-f")
        return std::make_unique<RV5StageProcessorHF>();
    if (name == "rv5-h-nf")
        return std::make_unique<RV5StageProcessorHNF>();
    if (name == "rv5-nh-f")
        return std::make_unique<RV5StageProcessorNHF>();
    if (name == "rv5-nh-nf")
        return std::make_unique<RV5StageProcessorNHNF>();
    return std::make_unique<RVSSProcessor>();
}

RunResult run(const std::string &source, const std::string &processor, uint64_t memStart, uint64_t memBytes)
{
    RunResult result;
    AssembledProgram program;
    try
    {
        std::istringstream stream(source);
        program = assemble(stream);
    }
    catch (const std::exception &e)
    {
        result.error = std::string("does not assemble: ") + e.what();
        return result;
    }
    std::unique_ptr<ProcessorBase> vm = makeProcessor(processor);
    vm->step_delay_ = 0;
    vm->LoadProgram(program);

    // DebugRun prints every cycle: keep that out of our output. A watchdog stops endless loops.
    std::atomic<bool> done{false};
    std::thread watchdog(
        [&]()
        {
            for (int i = 0; i < 200 && !done; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!done)
                vm->RequestStop();
        });
    std::ostringstream sink;
    std::streambuf *old = std::cout.rdbuf(sink.rdbuf());
    try
    {
        vm->DebugRun();
    }
    catch (const std::exception &e)
    {
        result.error = e.what(); // e.g. a wrong address computed by a program that is not correct on this pipeline
    }
    std::cout.rdbuf(old);
    done = true;
    watchdog.join();
    if (!result.error.empty())
    {
        return result;
    }

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
        result.state.push_back(vm->memory_controller_.peekByte(memStart + a));
    result.cycles = vm->cycle_s_;
    result.stalls = vm->stall_cycles_;
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

void usage()
{
    std::cerr << "usage: kites-cli compare FILE [--vm rv5-h-f|rv5-h-nf|rv5-nh-f|rv5-nh-nf|all] [--window N]\n"
                 "                              [--mem START_HEX BYTES] [--write-reordered PREFIX]\n";
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

    std::cout << "file " << file << "  (window " << window << ", memory 0x" << std::hex << memStart << std::dec << " + "
              << memBytes << " bytes)\n";
    std::cout << std::left << std::setw(11) << "pipeline" << std::setw(16) << "stalls (model)" << std::setw(18)
              << "cycles (Kites)" << std::setw(20) << "original on pipe" << std::setw(20) << "reordered on rvss"
              << "reordered on pipeline\n";
    bool allMatch = true;
    for (const Pipeline &pipeline : kPipelines)
    {
        if (which != "all" && which != pipeline.name)
            continue;
        scheduler::Options options;
        options.forwarding = pipeline.forwarding;
        options.hazardUnit = pipeline.hazardUnit;
        options.window = window;
        const scheduler::Result analysis = scheduler::analyze(original, options);
        const std::string reordered = scheduler::applyAll(original, options);
        if (!writePrefix.empty())
        {
            std::ofstream(writePrefix + "_" + pipeline.name + ".s") << reordered;
        }
        const RunResult before = run(original, pipeline.name, memStart, memBytes);
        const RunResult afterSingle = run(reordered, "rvss", memStart, memBytes);
        const RunResult after = run(reordered, pipeline.name, memStart, memBytes);
        const bool singleMatch = afterSingle.ok && afterSingle.state == reference.state;
        const bool pipelineMatch = after.ok && after.state == reference.state;
        const bool originalMatch = before.ok && before.state == reference.state;
        allMatch = allMatch && singleMatch && pipelineMatch;
        std::ostringstream stalls;
        stalls << analysis.stallsBefore << " -> " << analysis.stallsAfter;
        std::ostringstream cycles;
        cycles << (before.ok ? std::to_string(before.cycles) : std::string("-")) << " -> " << after.cycles;
        // "original on pipe" is informational: a plain program without NOPs is not meant to be
        // correct on a pipeline without a hazard unit.
        std::cout << std::left << std::setw(11) << pipeline.name << std::setw(16) << stalls.str() << std::setw(18)
                  << cycles.str() << std::setw(20) << (originalMatch ? "same" : "different")
                  << std::setw(20) << (singleMatch ? "same" : "DIFFERENT") << (pipelineMatch ? "same" : "DIFFERENT")
                  << "\n";
        if (!singleMatch)
            std::cout << "    reordered on rvss: " << differences(reference, afterSingle) << "\n";
        if (!pipelineMatch)
            std::cout << "    reordered on " << pipeline.name << ": " << differences(reference, after) << "\n";
    }
    std::cout << (allMatch ? "OK: every reordered program gives the reference registers and memory\n"
                           : "FAILED: a reordered program differs from the reference\n");
    return allMatch ? 0 : 1;
}

} // namespace Kites
