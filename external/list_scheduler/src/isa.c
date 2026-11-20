/*
 * isa.c
 *
 * Table of the RISC-V instructions we understand: RV64I, RV64M, RV64F
 * (single precision) and the pseudo-instructions from the reference card.
 * Also: register names, and small questions about one instruction
 * ("does it read register r?", ...).
 */
#include <string.h>
#include "common.h"

/*
 * The table. The last row has an empty name: it marks the end.
 *
 *    name      format      class         bytes size ends uncond
 */
OpInfo op_table[] = {
    /* R-type ALU */
    {"add",    FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"sub",    FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"xor",    FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"or",     FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"and",    FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"sll",    FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"srl",    FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"sra",    FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"slt",    FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"sltu",   FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"addw",   FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"subw",   FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"sllw",   FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"srlw",   FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"sraw",   FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    /* M extension (assumed to take one cycle in EX) */
    {"mul",    FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"mulh",   FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"mulhsu", FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"mulhu",  FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"div",    FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"divu",   FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"rem",    FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"remu",   FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"mulw",   FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"divw",   FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"divuw",  FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"remw",   FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"remuw",  FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    /* I-type ALU */
    {"addi",   FMT_I,      CLASS_ALU,    0, 1, 0, 0},
    {"xori",   FMT_I,      CLASS_ALU,    0, 1, 0, 0},
    {"ori",    FMT_I,      CLASS_ALU,    0, 1, 0, 0},
    {"andi",   FMT_I,      CLASS_ALU,    0, 1, 0, 0},
    {"slli",   FMT_I,      CLASS_ALU,    0, 1, 0, 0},
    {"srli",   FMT_I,      CLASS_ALU,    0, 1, 0, 0},
    {"srai",   FMT_I,      CLASS_ALU,    0, 1, 0, 0},
    {"slti",   FMT_I,      CLASS_ALU,    0, 1, 0, 0},
    {"sltiu",  FMT_I,      CLASS_ALU,    0, 1, 0, 0},
    {"addiw",  FMT_I,      CLASS_ALU,    0, 1, 0, 0},
    {"slliw",  FMT_I,      CLASS_ALU,    0, 1, 0, 0},
    {"srliw",  FMT_I,      CLASS_ALU,    0, 1, 0, 0},
    {"sraiw",  FMT_I,      CLASS_ALU,    0, 1, 0, 0},
    /* loads */
    {"lb",     FMT_LOAD,   CLASS_LOAD,   1, 1, 0, 0},
    {"lh",     FMT_LOAD,   CLASS_LOAD,   2, 1, 0, 0},
    {"lw",     FMT_LOAD,   CLASS_LOAD,   4, 1, 0, 0},
    {"ld",     FMT_LOAD,   CLASS_LOAD,   8, 1, 0, 0},
    {"lbu",    FMT_LOAD,   CLASS_LOAD,   1, 1, 0, 0},
    {"lhu",    FMT_LOAD,   CLASS_LOAD,   2, 1, 0, 0},
    {"lwu",    FMT_LOAD,   CLASS_LOAD,   4, 1, 0, 0},
    /* stores */
    {"sb",     FMT_STORE,  CLASS_STORE,  1, 1, 0, 0},
    {"sh",     FMT_STORE,  CLASS_STORE,  2, 1, 0, 0},
    {"sw",     FMT_STORE,  CLASS_STORE,  4, 1, 0, 0},
    {"sd",     FMT_STORE,  CLASS_STORE,  8, 1, 0, 0},
    /* branches */
    {"beq",    FMT_BRANCH, CLASS_BRANCH, 0, 1, 1, 0},
    {"bne",    FMT_BRANCH, CLASS_BRANCH, 0, 1, 1, 0},
    {"blt",    FMT_BRANCH, CLASS_BRANCH, 0, 1, 1, 0},
    {"bge",    FMT_BRANCH, CLASS_BRANCH, 0, 1, 1, 0},
    {"bltu",   FMT_BRANCH, CLASS_BRANCH, 0, 1, 1, 0},
    {"bgeu",   FMT_BRANCH, CLASS_BRANCH, 0, 1, 1, 0},
    {"bgt",    FMT_BRANCH, CLASS_BRANCH, 0, 1, 1, 0},
    {"ble",    FMT_BRANCH, CLASS_BRANCH, 0, 1, 1, 0},
    {"bgtu",   FMT_BRANCH, CLASS_BRANCH, 0, 1, 1, 0},
    {"bleu",   FMT_BRANCH, CLASS_BRANCH, 0, 1, 1, 0},
    {"beqz",   FMT_BZ,     CLASS_BRANCH, 0, 1, 1, 0},
    {"bnez",   FMT_BZ,     CLASS_BRANCH, 0, 1, 1, 0},
    {"blez",   FMT_BZ,     CLASS_BRANCH, 0, 1, 1, 0},
    {"bgez",   FMT_BZ,     CLASS_BRANCH, 0, 1, 1, 0},
    {"bltz",   FMT_BZ,     CLASS_BRANCH, 0, 1, 1, 0},
    {"bgtz",   FMT_BZ,     CLASS_BRANCH, 0, 1, 1, 0},
    /* jumps */
    {"jal",    FMT_JAL,    CLASS_JUMP,   0, 1, 1, 0},
    {"jalr",   FMT_JALR,   CLASS_JUMP,   0, 1, 1, 0},
    {"j",      FMT_J,      CLASS_JUMP,   0, 1, 1, 1},
    {"jr",     FMT_JR,     CLASS_JUMP,   0, 1, 1, 1},
    {"ret",    FMT_RET,    CLASS_JUMP,   0, 1, 1, 1},
    {"call",   FMT_CALL,   CLASS_JUMP,   0, 2, 1, 0},
    {"tail",   FMT_TAIL,   CLASS_JUMP,   0, 2, 1, 1},
    /* upper immediates */
    {"lui",    FMT_U,      CLASS_ALU,    0, 1, 0, 0},
    {"auipc",  FMT_U,      CLASS_ALU,    0, 1, 0, 0},
    /* system */
    {"ecall",  FMT_ECALL,  CLASS_SYSTEM, 0, 1, 1, 0},
    {"ebreak", FMT_NONE,   CLASS_SYSTEM, 0, 1, 1, 0},
    {"fence",  FMT_NONE,   CLASS_SYSTEM, 0, 1, 1, 0},
    /* ALU pseudo-instructions */
    {"nop",    FMT_NONE,   CLASS_ALU,    0, 1, 0, 0},
    {"li",     FMT_LI,     CLASS_ALU,    0, 1, 0, 0},   /* size set by parser */
    {"la",     FMT_LA,     CLASS_ALU,    0, 2, 0, 0},
    {"mv",     FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"not",    FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"neg",    FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"negw",   FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"sext.w", FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"seqz",   FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"snez",   FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"sltz",   FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"sgtz",   FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},

    /* ---------------- F extension (single precision) ----------------
       Timed like the integer instructions: FP arithmetic = one EX cycle
       (class ALU), flw = load, fsw = store. A rounding-mode operand
       (rne, rtz, dyn, ...) at the end is allowed and ignored.          */
    /* FP loads / stores (4 bytes; same memory as lw / sw) */
    {"flw",       FMT_LOAD,   CLASS_LOAD,   4, 1, 0, 0},
    {"fsw",       FMT_STORE,  CLASS_STORE,  4, 1, 0, 0},
    /* FP arithmetic: frd, frs1, frs2 */
    {"fadd.s",    FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"fsub.s",    FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"fmul.s",    FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"fdiv.s",    FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"fmin.s",    FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"fmax.s",    FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"fsgnj.s",   FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"fsgnjn.s",  FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"fsgnjx.s",  FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"fsqrt.s",   FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    /* fused multiply-add: frd, frs1, frs2, frs3 */
    {"fmadd.s",   FMT_R4,     CLASS_ALU,    0, 1, 0, 0},
    {"fmsub.s",   FMT_R4,     CLASS_ALU,    0, 1, 0, 0},
    {"fnmadd.s",  FMT_R4,     CLASS_ALU,    0, 1, 0, 0},
    {"fnmsub.s",  FMT_R4,     CLASS_ALU,    0, 1, 0, 0},
    /* compares: integer rd, FP sources */
    {"feq.s",     FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"flt.s",     FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"fle.s",     FMT_R,      CLASS_ALU,    0, 1, 0, 0},
    {"fgt.s",     FMT_R,      CLASS_ALU,    0, 1, 0, 0},   /* pseudo: flt with swapped operands */
    {"fge.s",     FMT_R,      CLASS_ALU,    0, 1, 0, 0},   /* pseudo: fle with swapped operands */
    {"fclass.s",  FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    /* conversions and moves between the register files */
    {"fcvt.w.s",  FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"fcvt.wu.s", FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"fcvt.l.s",  FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"fcvt.lu.s", FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"fcvt.s.w",  FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"fcvt.s.wu", FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"fcvt.s.l",  FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"fcvt.s.lu", FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"fmv.x.w",   FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"fmv.w.x",   FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"fmv.x.s",   FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},   /* old name of fmv.x.w */
    {"fmv.s.x",   FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},   /* old name of fmv.w.x */
    /* FP pseudo-instructions (fsgnj with the same source twice) */
    {"fmv.s",     FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"fneg.s",    FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},
    {"fabs.s",    FMT_RD_RS,  CLASS_ALU,    0, 1, 0, 0},

    /* end of the table */
    {"",       0,          0,            0, 0, 0, 0}
};

/*
 * Look up a mnemonic (must be lower case).
 * If found, copy its row into *op and return 1. Otherwise return 0.
 */
int find_op(char mnem[], OpInfo *op)
{
    int i = 0;
    while (op_table[i].name[0] != '\0') {
        if (strcmp(op_table[i].name, mnem) == 0) {
            *op = op_table[i];
            return 1;
        }
        i++;
    }
    return 0;
}

/* ABI names of the 32 registers: reg_names[5] is "t0" (= x5) */
char reg_names[32][5] = {
    "zero", "ra", "sp",  "gp",  "tp", "t0", "t1", "t2",
    "s0",   "s1", "a0",  "a1",  "a2", "a3", "a4", "a5",
    "a6",   "a7", "s2",  "s3",  "s4", "s5", "s6", "s7",
    "s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6"
};

/* ABI names of the 32 floating-point registers: freg_names[10] is "fa0" (= f10) */
char freg_names[32][5] = {
    "ft0", "ft1", "ft2",  "ft3",  "ft4", "ft5", "ft6",  "ft7",
    "fs0", "fs1", "fa0",  "fa1",  "fa2", "fa3", "fa4",  "fa5",
    "fa6", "fa7", "fs2",  "fs3",  "fs4", "fs5", "fs6",  "fs7",
    "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11"
};

/* "12" at name[start] -> 12; -1 if it is not a number below 32 */
int small_number(char name[], int start)
{
    int i = start;
    int n = 0;
    if (name[i] < '0' || name[i] > '9')
        return -1;
    while (name[i] >= '0' && name[i] <= '9') {
        n = n * 10 + (name[i] - '0');
        i++;
    }
    if (name[i] != '\0' || n >= 32)
        return -1;
    return n;
}

/*
 * "x5" or "t0" -> 5,  "f10" or "fa0" -> FREG(10) = 42.
 * Returns -1 if the name is not a register.
 */
int reg_number(char name[])
{
    int i, n;

    /* x0 .. x31 */
    if (name[0] == 'x') {
        n = small_number(name, 1);
        if (n >= 0)
            return n;
    }

    /* f0 .. f31 */
    if (name[0] == 'f') {
        n = small_number(name, 1);
        if (n >= 0)
            return FREG(n);
    }

    /* fp is another name for s0 */
    if (strcmp(name, "fp") == 0)
        return 8;

    /* ABI names */
    for (i = 0; i < 32; i++) {
        if (strcmp(name, reg_names[i]) == 0)
            return i;
        if (strcmp(name, freg_names[i]) == 0)
            return FREG(i);
    }
    return -1;
}

/* 5 -> "x5",  FREG(10) -> "f10"  (for messages) */
void reg_name(int r, char out[])
{
    int k = 1;
    out[0] = 'x';
    if (r >= 32) {
        out[0] = 'f';
        r = r - 32;
    }
    if (r >= 10) {
        out[k] = '0' + r / 10;
        k++;
    }
    out[k] = '0' + r % 10;
    out[k + 1] = '\0';
}

/* Does instruction in read register r?  1 = yes, 0 = no */
int reads_reg(Instr *in, int r)
{
    int k;
    for (k = 0; k < in->nsrc; k++) {
        if (in->src[k] == r)
            return 1;
    }
    return 0;
}

/* Is it a load or a store? */
int is_memory(Instr *in)
{
    if (in->cls == CLASS_LOAD || in->cls == CLASS_STORE)
        return 1;
    return 0;
}
