/*
 * hazard.c
 *
 * Timing of the 5-stage pipeline  IF ID EX MEM WB  (course notes, Sept 9).
 *
 * We count time in "issue slots": normally one instruction enters the
 * pipeline per slot. If an instruction needs a value that is not ready,
 * it waits (stalls) and enters later.
 *
 * required_gap(P) = how many slots after producer P an instruction that
 *                   reads its result must enter, to get the new value.
 *
 *                              fwd mode         nofwd mode
 *   ALU  -> next instruction   1 (no stall)     3 (2 stalls)
 *   LOAD -> next instruction   2 (1 stall)      3 (2 stalls)
 *
 * fwd:   ALU result is forwarded EX/MEM -> EX. A load's value exists only
 *        after MEM, so the next instruction waits 1 cycle (load-use).
 * nofwd: the value goes through the register file. It is written in the
 *        first half of WB and read in the second half of ID, so the
 *        consumer's ID can be in the producer's WB cycle: gap 3.
 *
 * Control hazards (branch flushes) are not modelled: branches always
 * stay at the end of their block, so reordering cannot change them.
 *
 * Two-part pseudo-instructions (la = auipc + addi, big li = lui + addi,
 * call = auipc + jalr, tail = auipc + jr): the second half reads the
 * register the first half writes, so it waits like any ALU consumer:
 * 0 stalls with fwd, 2 with nofwd ("inner stalls"). Reordering cannot
 * remove them, but they are real cycles, so they are counted.
 *
 * Registers: 0..31 = x0..x31, 32..63 = f0..f31 (F extension). FP
 * instructions are timed like integer ones (FP arithmetic = ALU, flw = load).
 *
 * Stalls also equal the NOPs a programmer would have to insert.
 */
#include "common.h"

int required_gap(Program *p, HazardModel *m, int producer)
{
    if (m->forwarding == 0)
        return 3;
    if (p->ins[producer].cls == CLASS_LOAD)
        return 2;
    return 1;
}

/*
 * How many lines after a block can still stall because of the block's last
 * lines (the largest gap minus 1): with fwd only a load is slow (gap 2), so
 * only the next line; without fwd every producer needs gap 3, so 2 lines.
 */
int lookahead_lines(HazardModel *m)
{
    if (m->forwarding == 1)
        return 1;
    return 2;
}

/* Stalls inside instruction i itself (only 2-part pseudo-instructions). */
int inner_stalls(Program *p, HazardModel *m, int i)
{
    if (p->ins[i].size < 2)
        return 0;
    if (m->forwarding == 1)
        return 0;           /* ALU -> ALU with forwarding: no stall      */
    return 2;               /* ALU -> ALU without forwarding: 2 stalls   */
}

/* Issue slots instruction i occupies: its parts plus the stalls between them. */
int slots_used(Program *p, HazardModel *m, int i)
{
    return p->ins[i].size + inner_stalls(p, m, i);
}

/* Empty pipeline: no register is waiting for a value. */
void reset_state(PipeState *st)
{
    int r;
    for (r = 0; r < NREGS; r++) {
        st->writer[r] = -1;
        st->slot[r] = 0;
    }
    st->next_slot = 0;
}

/* Make "to" an exact copy of "from". */
void copy_state(PipeState *to, PipeState *from)
{
    int r;
    for (r = 0; r < NREGS; r++) {
        to->writer[r] = from->writer[r];
        to->slot[r] = from->slot[r];
    }
    to->next_slot = from->next_slot;
}

/*
 * Put instruction i into the pipeline (in program order).
 * Returns the number of stalls before it (plus its inner stalls, if it
 * is a 2-part pseudo-instruction) and updates the state.
 */
int issue(Program *p, HazardModel *m, PipeState *st, int i)
{
    Instr *in = &p->ins[i];
    int start = st->next_slot;      /* slot in which i will enter */
    int k, r, ready, stalls;

    /* wait for every source register */
    for (k = 0; k < in->nsrc; k++) {
        r = in->src[k];
        if (st->writer[r] != -1) {
            ready = st->slot[r] + required_gap(p, m, st->writer[r]);
            if (ready > start)
                start = ready;
        }
    }
    stalls = start - st->next_slot;

    /* a pseudo-instruction of size 2 (li, la, call) uses 2 slots plus the
       inner stalls between its halves; its result comes from the last one */
    if (in->rd > 0) {
        st->writer[in->rd] = i;
        st->slot[in->rd] = start + slots_used(p, m, i) - 1;
    }
    st->next_slot = start + slots_used(p, m, i);

    /* the inner stalls are real cycles, so they are counted too */
    stalls = stalls + inner_stalls(p, m, i);

    /* after j / jr / ret the next line is reached from somewhere else:
       assume all older values are ready */
    if (in->is_uncond == 1) {
        for (r = 0; r < NREGS; r++)
            st->writer[r] = -1;
    }
    return stalls;
}

/*
 * Which instruction makes instruction i wait (call it BEFORE issue()):
 * the writer of the source register that is ready last. -1 if i does not
 * have to wait. Only used to report the cause of each stall (option -b).
 */
int stall_producer(Program *p, HazardModel *m, PipeState *st, int i)
{
    Instr *in = &p->ins[i];
    int latest = st->next_slot;
    int cause = -1;
    int k, r, ready;

    for (k = 0; k < in->nsrc; k++) {
        r = in->src[k];
        if (st->writer[r] != -1) {
            ready = st->slot[r] + required_gap(p, m, st->writer[r]);
            if (ready > latest) {
                latest = ready;
                cause = st->writer[r];
            }
        }
    }
    return cause;
}

/*
 * Run the whole program in the given order.
 * stalls[i] = stalls before instruction i. Returns the total.
 */
int count_stalls(Program *p, HazardModel *m, int order[], int stalls[])
{
    PipeState st;
    int k, i;
    int total = 0;

    reset_state(&st);
    for (k = 0; k < p->nins; k++) {
        i = order[k];
        stalls[i] = issue(p, m, &st, i);
        total = total + stalls[i];
    }
    return total;
}
