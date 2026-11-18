/*
 * isa.c
 *
 * Table of the RISC-V instructions we understand: RV64I, RV64M and the
 * pseudo-instructions from the reference card.
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

/* "x5" or "t0" -> 5.  Returns -1 if the name is not a register. */
int reg_number(char name[])
{
    int i, n;

    /* x0 .. x31 */
    if (name[0] == 'x' && name[1] >= '0' && name[1] <= '9') {
        n = 0;
        i = 1;
        while (name[i] >= '0' && name[i] <= '9') {
            n = n * 10 + (name[i] - '0');
            i++;
        }
        if (n < 32)
            return n;
        return -1;
    }

    /* fp is another name for s0 */
    if (strcmp(name, "fp") == 0)
        return 8;

    /* ABI names */
    for (i = 0; i < 32; i++) {
        if (strcmp(name, reg_names[i]) == 0)
            return i;
    }
    return -1;
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
