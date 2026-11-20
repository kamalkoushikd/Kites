/**
 * @file run_profile.cpp
 * @brief Measured stalls per line (see run_profile.h).
 */
#include "run_profile.h"

#include "assembler/assembler.h"
#include "processor/processor_constants.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "processor/rv5s/rv5s_processor_nh_f.h"
#include "processor/rv5s/rv5s_processor_nh_nf.h"

#include <sstream>

namespace Kites
{
namespace reorder
{

namespace
{

constexpr uint32_t kNop = 0x00000013; // addi x0, x0, 0

/// One instruction in a pipeline register.
struct Slot
{
    uint64_t pc = INVALID_PC;
    uint32_t instruction = kNop;
    bool valid() const { return pc != INVALID_PC; }
};

/// Registers an instruction writes and reads: 0..31 = x0..x31, 32..63 = f0..f31, -1 = none.
struct RegisterUse
{
    int writes = -1;
    int reads[3] = {-1, -1, -1};
};

RegisterUse decode(uint32_t instruction)
{
    const int opcode = static_cast<int>(instruction & 0x7f);
    const int rd = static_cast<int>((instruction >> 7) & 31);
    const int rs1 = static_cast<int>((instruction >> 15) & 31);
    const int rs2 = static_cast<int>((instruction >> 20) & 31);
    const int rs3 = static_cast<int>((instruction >> 27) & 31);
    const int f = 32; // offset of the FP registers
    RegisterUse use;
    auto set = [&](int writes, int a, int b = -1, int c = -1) {
        use.writes = writes == 0 ? -1 : writes; // x0 is never written
        use.reads[0] = a;
        use.reads[1] = b;
        use.reads[2] = c;
    };
    switch (opcode)
    {
    case 0x33: // OP
    case 0x3b: // OP-32
        set(rd, rs1, rs2);
        break;
    case 0x13: // OP-IMM
    case 0x1b: // OP-IMM-32
    case 0x03: // LOAD
    case 0x67: // JALR
        set(rd, rs1);
        break;
    case 0x07: // LOAD-FP
        set(f + rd, rs1);
        break;
    case 0x23: // STORE
    case 0x63: // BRANCH
        set(-1, rs1, rs2);
        break;
    case 0x27: // STORE-FP
        set(-1, rs1, f + rs2);
        break;
    case 0x37: // LUI
    case 0x17: // AUIPC
    case 0x6f: // JAL
        set(rd, -1);
        break;
    case 0x43: // FMADD, FMSUB, FNMSUB, FNMADD
    case 0x47:
    case 0x4b:
    case 0x4f:
        set(f + rd, f + rs1, f + rs2, f + rs3);
        break;
    case 0x53: // OP-FP
        switch (instruction >> 27)
        {
        case 0x14: // feq, flt, fle: integer result
            set(rd, f + rs1, f + rs2);
            break;
        case 0x18: // fcvt.w(u)/l(u).s: integer result
        case 0x1c: // fmv.x.w, fclass.s
            set(rd, f + rs1);
            break;
        case 0x1a: // fcvt.s.w(u)/l(u): from an integer register
        case 0x1e: // fmv.w.x
            set(f + rd, rs1);
            break;
        case 0x0b: // fsqrt.s
            set(f + rd, f + rs1);
            break;
        default: // fadd, fsub, fmul, fdiv, fsgnj, fmin, fmax
            set(f + rd, f + rs1, f + rs2);
            break;
        }
        break;
    case 0x73: // ecall reads a7, a0, a1
        set(-1, 17, 10, 11);
        break;
    default:
        break;
    }
    return use;
}

bool readsWhatItWrites(uint32_t reader, uint32_t writer)
{
    const int reg = decode(writer).writes;
    if (reg < 0)
    {
        return false;
    }
    for (int r : decode(reader).reads)
    {
        if (r == reg && r != 0)
        {
            return true;
        }
    }
    return false;
}

/// Read access to a processor's pipeline registers (protected in RV5StageVM_Base).
class Probe
{
  public:
    virtual ~Probe() = default;
    virtual ProcessorBase &vm() = 0;
    virtual bool running() = 0;
    virtual void cycle() = 0;
    virtual Slot ifId() const = 0;
    virtual Slot idEx() const = 0;
    virtual Slot exMem() const = 0;
    virtual Slot memWb() const = 0;
};

template <class Processor> class ProbeOf : public Processor, public Probe
{
  public:
    ProcessorBase &vm() override { return *this; }
    bool running() override
    {
        return !this->stop_requested_ && (this->program_counter_ < this->program_size_ || !this->is_pipeline_drained());
    }
    void cycle() override
    {
        this->Step();
        // Step() keeps every cycle for Undo; a long run does not need that
        while (!this->undo_stack_.empty())
        {
            this->undo_stack_.pop();
        }
    }
    Slot ifId() const override { return {this->if_id_reg_.pc, this->if_id_reg_.instruction}; }
    Slot idEx() const override { return {this->id_ex_reg_.pc, this->id_ex_reg_.instruction}; }
    Slot exMem() const override { return {this->ex_mem_reg_.pc, this->ex_mem_reg_.instruction}; }
    Slot memWb() const override { return {this->mem_wb_reg_.pc, this->mem_wb_reg_.instruction}; }
};

std::unique_ptr<Probe> makeProbe(PipelineVariant variant)
{
    switch (variant)
    {
    case PipelineVariant::HazardUnitForwarding:
        return std::make_unique<ProbeOf<RV5StageProcessorHF>>();
    case PipelineVariant::HazardUnitNoForwarding:
        return std::make_unique<ProbeOf<RV5StageProcessorHNF>>();
    case PipelineVariant::NoHazardUnitForwarding:
        return std::make_unique<ProbeOf<RV5StageProcessorNHF>>();
    case PipelineVariant::NoHazardUnitNoForwarding:
        return std::make_unique<ProbeOf<RV5StageProcessorNHNF>>();
    }
    return nullptr;
}

} // namespace

struct ProfiledRun::Impl
{
    RunProfile profile;
    AssembledProgram program;
    std::unique_ptr<Probe> probe;
    unsigned maxCycles = 0;
    bool done = false;

    int lineOf(uint64_t pc) const
    {
        const auto it = program.instruction_number_line_number_mapping.find(static_cast<unsigned>(pc / 4));
        return it == program.instruction_number_line_number_mapping.end() ? -1 : static_cast<int>(it->second);
    }

    void finish()
    {
        done = true;
        ProcessorBase &vm = probe->vm();
        profile.cycles = vm.cycle_s_;
        for (size_t r = 1; r < 32; ++r)
        {
            profile.registers.push_back(vm.registers_.ReadGpr(r));
        }
        for (size_t r = 0; r < 32; ++r)
        {
            uint64_t value = vm.registers_.ReadFpr(r);
            if ((value >> 32) == 0xffffffffULL)
            {
                value &= 0xffffffffULL; // NaN-boxed single-precision value
            }
            profile.registers.push_back(value);
        }
        profile.ok = profile.error.empty();
    }
};

ProfiledRun::ProfiledRun(const std::string &source, PipelineVariant variant, unsigned maxCycles)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->maxCycles = maxCycles;
    try
    {
        std::istringstream stream(source);
        m_impl->program = assemble(stream);
    }
    catch (const std::exception &error)
    {
        m_impl->profile.error = std::string("The program does not assemble: ") + error.what();
        m_impl->done = true;
        return;
    }
    m_impl->probe = makeProbe(variant);
    ProcessorBase &vm = m_impl->probe->vm();
    vm.step_delay_ = 0;
    vm.LoadProgram(m_impl->program);
    vm.ClearStop();
}

ProfiledRun::~ProfiledRun() = default;

bool ProfiledRun::step(unsigned cycles)
{
    Impl &m = *m_impl;
    if (m.done)
    {
        return false;
    }
    Probe &probe = *m.probe;
    RunProfile &profile = m.profile;
    try
    {
        for (unsigned n = 0; n < cycles; ++n)
        {
            if (!probe.running())
            {
                profile.finished = true;
                m.finish();
                return false;
            }
            if (probe.vm().cycle_s_ >= m.maxCycles)
            {
                m.finish();
                return false;
            }
            const Slot waiting = probe.ifId();
            const Slot ahead[2] = {probe.idEx(), probe.exMem()};
            probe.cycle();

            // stall: the instruction in IF/ID is still there
            const Slot now = probe.ifId();
            if (waiting.valid() && now.valid() && now.pc == waiting.pc)
            {
                ++profile.stalls;
                ++profile.lines[m.lineOf(waiting.pc)].stalls;
                int cause = -1;
                for (const Slot &slot : ahead)
                {
                    if (cause < 0 && slot.valid() && readsWhatItWrites(waiting.instruction, slot.instruction))
                    {
                        cause = m.lineOf(slot.pc);
                    }
                }
                if (cause >= 0)
                {
                    ++profile.lines[cause].caused;
                }
                else
                {
                    ++profile.unexplained;
                    ++profile.lines[m.lineOf(waiting.pc)].unexplained;
                }
            }

            // the instruction that reached WB this cycle
            const Slot written = probe.memWb();
            if (written.valid())
            {
                ++profile.instructions;
                ++profile.lines[m.lineOf(written.pc)].executed;
                if (written.instruction == kNop)
                {
                    ++profile.nops;
                }
            }
        }
    }
    catch (const std::exception &error)
    {
        // e.g. a wrong address computed by a program that is not correct on this pipeline
        profile.error = std::string("The run stopped with an error: ") + error.what();
        m.finish();
        return false;
    }
    return true;
}

const RunProfile &ProfiledRun::profile() const
{
    return m_impl->profile;
}

RunProfile profileRun(const std::string &source, PipelineVariant variant, unsigned maxCycles)
{
    ProfiledRun run(source, variant, maxCycles);
    while (run.step(100000))
    {
    }
    return run.profile();
}

} // namespace reorder
} // namespace Kites
