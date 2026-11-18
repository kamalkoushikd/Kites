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

/* Empty pipeline: no register is waiting for a value. */
void reset_state(PipeState *st)
{
    int r;
    for (r = 0; r < 32; r++) {
        st->writer[r] = -1;
        st->slot[r] = 0;
    }
    st->next_slot = 0;
}

/* Make "to" an exact copy of "from". */
void copy_state(PipeState *to, PipeState *from)
{
    int r;
    for (r = 0; r < 32; r++) {
        to->writer[r] = from->writer[r];
        to->slot[r] = from->slot[r];
    }
    to->next_slot = from->next_slot;
}

/*
 * Put instruction i into the pipeline (in program order).
 * Returns the number of stalls before it and updates the state.
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

    /* a pseudo-instruction of size 2 (li, la, call) uses 2 slots;
       its result comes from the last one */
    if (in->rd > 0) {
        st->writer[in->rd] = i;
        st->slot[in->rd] = start + in->size - 1;
    }
    st->next_slot = start + in->size;

    /* after j / jr / ret the next line is reached from somewhere else:
       assume all older values are ready */
    if (in->is_uncond == 1) {
        for (r = 0; r < 32; r++)
            st->writer[r] = -1;
    }
    return stalls;
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
