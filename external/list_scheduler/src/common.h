/*
 * common.h
 *
 * All constants, data structures and function declarations of the
 * list scheduler are in this one file. Every .c file includes it.
 *
 * Files:
 *   isa.c       table of RISC-V instructions, register names
 *   parser.c    read a .s file into a Program, write it back
 *   hazard.c    pipeline timing: how many stalls an instruction gets
 *   dag.c       build the dependence graph (DAG) of a window
 *   sched.c     list scheduling + loop over blocks and windows
 *   verify.c    SEPARATE program: checks a reordered .s against the original
 *   optimal.c   SEPARATE program: brute-force best order of small blocks
 *   show.c      print the original and reordered programs with nops
 *   main.c      command line and report
 */

/* These two lines and the #endif at the bottom stop the file from
   being read twice by the compiler. */
#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Limits (fixed-size arrays everywhere, no malloc)                    */
/* ------------------------------------------------------------------ */
#define MAX_LINES   2000    /* lines in the .s file                     */
#define MAX_INSTR   1000    /* instructions in the .s file              */
#define MAX_TEXT    256     /* characters in one line                   */
#define MAX_SRC     3       /* source registers of one instruction      */
#define MAX_WIN     64      /* largest window size W                    */
#define MAX_WORDS   9       /* words on one line: mnemonic + 8 operands */
#define MAX_WORD    32      /* characters in one word                   */
#define NREGS       64      /* registers: x0..x31 = 0..31, f0..f31 = 32..63 */
#define FREG(k)     (32 + (k))  /* number of floating-point register fk  */

/* ------------------------------------------------------------------ */
/* Instruction classes (decide the pipeline behaviour)                 */
/* ------------------------------------------------------------------ */
#define CLASS_ALU     0     /* add, addi, lui, li, mv, mul, ...         */
#define CLASS_LOAD    1     /* lb lh lw ld lbu lhu lwu                  */
#define CLASS_STORE   2     /* sb sh sw sd                              */
#define CLASS_BRANCH  3     /* beq bne ... beqz bnez ...                */
#define CLASS_JUMP    4     /* jal jalr j jr ret call tail              */
#define CLASS_SYSTEM  5     /* ecall ebreak fence                       */

/* ------------------------------------------------------------------ */
/* Operand formats: how the operands are written in the .s file        */
/* ------------------------------------------------------------------ */
#define FMT_R       0   /* add  rd, rs1, rs2                            */
#define FMT_I       1   /* addi rd, rs1, imm                            */
#define FMT_LOAD    2   /* lw   rd, off(rs1)                            */
#define FMT_STORE   3   /* sw   rs2, off(rs1)                           */
#define FMT_BRANCH  4   /* beq  rs1, rs2, label                         */
#define FMT_U       5   /* lui  rd, imm                                 */
#define FMT_JAL     6   /* jal  rd, label    or  jal label              */
#define FMT_JALR    7   /* jalr rd, off(rs1) or  jalr rd, rs1, off  or  jalr rs1 */
#define FMT_NONE    8   /* nop, ebreak, fence                           */
#define FMT_ECALL   9   /* ecall (reads a7, a0, a1)                     */
#define FMT_RD_RS   10  /* mv   rd, rs   (also not, neg, seqz, ...)     */
#define FMT_LI      11  /* li   rd, imm                                 */
#define FMT_LA      12  /* la   rd, symbol                              */
#define FMT_BZ      13  /* beqz rs, label                               */
#define FMT_J       14  /* j    label                                   */
#define FMT_JR      15  /* jr   rs                                      */
#define FMT_RET     16  /* ret                                          */
#define FMT_CALL    17  /* call label                                   */
#define FMT_TAIL    18  /* tail label                                   */
#define FMT_R4      19  /* fmadd.s rd, rs1, rs2, rs3                    */

/* One row of the instruction table in isa.c */
struct OpInfo {
    char name[10];
    int  format;        /* FMT_...                                      */
    int  cls;           /* CLASS_...                                    */
    int  mem_bytes;     /* bytes read/written (loads and stores)        */
    int  size;          /* machine instructions it expands to           */
    int  ends_block;    /* 1 for branches, jumps, ecall                 */
    int  is_uncond;     /* 1 for j, jr, ret, tail (never falls through) */
};
typedef struct OpInfo OpInfo;

/* ------------------------------------------------------------------ */
/* One instruction of the program                                      */
/* ------------------------------------------------------------------ */
struct Instr {
    int  line_no;               /* line number in the .s file           */
    char text[MAX_TEXT];        /* the line exactly as it will be printed */
    char code[MAX_TEXT];        /* only the instruction, for reports    */
    char mnem[MAX_WORD];        /* mnemonic, lower case                 */
    int  known;                 /* 0 if the mnemonic is not in the table */
    int  cls;                   /* CLASS_...                            */

    int  rd;                    /* destination register, -1 if none (0..63, see NREGS) */
    int  src[MAX_SRC];          /* source registers                     */
    int  nsrc;                  /* how many source registers            */

    int  mem_base;              /* base register of a load/store, -1 if unknown */
    long mem_off;               /* offset of a load/store               */
    int  mem_off_known;         /* 1 if the offset is a plain number    */
    int  mem_bytes;             /* bytes accessed                       */

    int  size;                  /* machine instructions (li/la/call: 2) */
    int  starts_block;          /* 1 = first instruction of a basic block */
    int  ends_block;            /* 1 = last instruction of a basic block  */
    int  is_uncond;             /* 1 = j / jr / ret / tail              */
};
typedef struct Instr Instr;

/* ------------------------------------------------------------------ */
/* The whole program                                                   */
/* ------------------------------------------------------------------ */
#define LINE_TEXT   0   /* label, directive, comment, blank: printed as is  */
#define LINE_INSTR  1   /* an instruction slot: filled from the new order   */

struct Line {
    int  type;                  /* LINE_TEXT or LINE_INSTR              */
    char text[MAX_TEXT];        /* only used for LINE_TEXT              */
};
typedef struct Line Line;

struct Block {
    int start;                  /* first instruction index              */
    int end;                    /* one past the last instruction index  */
};
typedef struct Block Block;

struct Program {
    Line  lines[MAX_LINES];     /* every line of the file               */
    int   nlines;
    Instr ins[MAX_INSTR];       /* only the instructions                */
    int   nins;
    Block blocks[MAX_INSTR];    /* basic blocks                         */
    int   nblocks;
};
typedef struct Program Program;

/* ------------------------------------------------------------------ */
/* Pipeline model                                                      */
/* ------------------------------------------------------------------ */
struct HazardModel {
    int forwarding;     /* 1 = fwd mode, 0 = nofwd mode                 */
};
typedef struct HazardModel HazardModel;

/*
 * State of the pipeline while we "issue" instructions one by one.
 * Time is counted in issue slots: normally one instruction per slot.
 */
struct PipeState {
    int writer[NREGS];  /* instruction that last wrote register r (-1 = none) */
    int slot[NREGS];    /* slot in which that writer's result is made    */
    int next_slot;      /* next free slot                               */
};
typedef struct PipeState PipeState;

/* ------------------------------------------------------------------ */
/* Dependence graph of one window (adjacency matrix)                   */
/* ------------------------------------------------------------------ */
/*
 * Nodes are numbered 0..n-1 in original order; node[k] is the
 * instruction index. edge[u][v] = -1 means no edge, otherwise v must
 * come after u and issue at least edge[u][v] slots after u.
 * type[u][v] says why: 'R' RAW, 'A' WAR, 'O' WAW, 'M' memory, 'C' control.
 */
struct Dag {
    int  n;
    int  node[MAX_WIN];
    int  edge[MAX_WIN][MAX_WIN];
    char type[MAX_WIN][MAX_WIN];
    int  ext_ready[MAX_WIN];    /* earliest slot because of instructions BEFORE the window */
    int  occ[MAX_WIN];          /* slots the node occupies (2-part pseudos: more) */
    int  height[MAX_WIN];       /* longest path to the end of the window */
    int  nsucc[MAX_WIN];        /* number of successors                 */
    int  npred[MAX_WIN];        /* number of predecessors               */
};
typedef struct Dag Dag;

/* ------------------------------------------------------------------ */
/* Options from the command line                                       */
/* ------------------------------------------------------------------ */
/*
 * Rules for list scheduling. Every window is scheduled once with each
 * rule. The original order and the two schedules are compared: fewest
 * stalls wins; on a tie, the smallest swap length
 * (sum of |new position - original position|).
 */
#define PRIO_HEIGHT  1      /* longest dependence chain first, then most successors */
#define PRIO_SUCC    2      /* most successors first, then longest chain            */

/*
 * How the windows move through a block (option -a):
 *   slide    w1 = 1..W,  w2 = 2..W+1, ...    (the default: only the first
 *            instruction of each window is kept for good, the window then
 *            moves by one)
 *   fixed    w1 = 1..W,  w2 = W+1..2W, ...   (the P-1 method)
 * In slide, w2 sees the instructions in the order chosen by w1.
 */
#define WIN_FIXED    0
#define WIN_SLIDE    1

struct Options {
    int window;         /* window size W                                */
    int win_mode;       /* WIN_FIXED or WIN_SLIDE                        */
    int cross_block;    /* 1 = cross-block precaution on (default), 0 = off */
    int reorder;        /* 0 = keep original order (only count stalls)  */
    int insert_nops;    /* 1 = write nops where stalls remain           */
};
typedef struct Options Options;

/* ------------------------------------------------------------------ */
/* Functions                                                           */
/* ------------------------------------------------------------------ */

/* isa.c */
int  find_op(char mnem[], OpInfo *op);
int  reg_number(char name[]);
void reg_name(int r, char out[]);
int  reads_reg(Instr *in, int r);
int  is_memory(Instr *in);

/* parser.c */
int  read_program(char filename[], Program *p);
void write_program(FILE *out, Program *p, int order[], int nops[], int inner[], int with_nops);

/* hazard.c */
int  required_gap(Program *p, HazardModel *m, int producer);
int  lookahead_lines(HazardModel *m);
int  inner_stalls(Program *p, HazardModel *m, int i);
int  slots_used(Program *p, HazardModel *m, int i);
void reset_state(PipeState *st);
void copy_state(PipeState *to, PipeState *from);
int  issue(Program *p, HazardModel *m, PipeState *st, int i);
int  stall_producer(Program *p, HazardModel *m, PipeState *st, int i);
int  count_stalls(Program *p, HazardModel *m, int order[], int stalls[]);

/* dag.c */
void build_dag(Program *p, HazardModel *m, PipeState *st, int nodes[], int n, Dag *g,
               int rest_start, int block_end);
int  may_alias(Instr *a, int a_base_writer, Instr *b, int b_base_writer);

/* sched.c */
void list_schedule(Program *p, Dag *g, int start_slot, int priority, int result[]);
void schedule_program(Program *p, HazardModel *m, Options *opt, int order[]);
int  swap_length(int perm[], int n);


/* show.c */
void show_program(Program *p, HazardModel *m, int order[], char title[]);

#endif
