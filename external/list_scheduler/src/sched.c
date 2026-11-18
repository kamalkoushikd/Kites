/*
 * sched.c
 *
 * List scheduling.
 *
 * For every basic block, cut into windows of W instructions:
 *   1. build the DAG of the window                           (dag.c)
 *   2. make one schedule with each rule: height, succ        (list_schedule)
 *   3. count the stalls of the original order and of the two
 *      schedules; keep the one with the fewest stalls (most savings).
 *      On a tie keep the one with the smallest swap length
 *      (sum of |new position - original position|).
 *      If that also ties, the earlier one is kept, in the order
 *      original, height, succ. So with no savings the code is unchanged.
 *
 * Checking that the new order is correct is a separate program (verify.c).
 */
#include "common.h"

/*
 * Should node a be picked instead of node b?  1 = yes, 0 = no.
 * start_a / start_b = slot in which they could issue right now.
 */
int is_better(Dag *g, int priority, int a, int start_a, int b, int start_b)
{
    /* 1. the one that can go earlier (fewer stalls) */
    if (start_a < start_b) return 1;
    if (start_a > start_b) return 0;

    /* 2. the priority rule (height then succ, or succ then height) */
    if (priority == PRIO_HEIGHT) {
        if (g->height[a] > g->height[b]) return 1;
        if (g->height[a] < g->height[b]) return 0;
        if (g->nsucc[a] > g->nsucc[b]) return 1;
        if (g->nsucc[a] < g->nsucc[b]) return 0;
    }
    if (priority == PRIO_SUCC) {
        if (g->nsucc[a] > g->nsucc[b]) return 1;
        if (g->nsucc[a] < g->nsucc[b]) return 0;
        if (g->height[a] > g->height[b]) return 1;
        if (g->height[a] < g->height[b]) return 0;
    }

    /* 3. still tied: the one that came first in the original program */
    if (a < b)
        return 1;
    return 0;
}

/*
 * One list-scheduling pass.
 * result[k] = node placed at position k.
 *
 * A node is "ready" when all its predecessors are placed.
 * At each step pick the best ready node, place it, and update the
 * earliest slot of its successors.
 */
void list_schedule(Program *p, Dag *g, int start_slot, int priority, int result[])
{
    int earliest[MAX_WIN];      /* earliest slot each node can issue  */
    int preds_left[MAX_WIN];    /* predecessors not yet placed        */
    int placed[MAX_WIN];        /* 1 = already placed                 */
    int slot = start_slot;      /* next free slot                     */
    int n = g->n;
    int i, k, start, best, best_start;

    for (i = 0; i < n; i++) {
        earliest[i] = g->ext_ready[i];
        preds_left[i] = g->npred[i];
        placed[i] = 0;
    }

    for (k = 0; k < n; k++) {
        best = -1;
        best_start = 0;

        /* look at every ready node, remember the best one */
        for (i = 0; i < n; i++) {
            if (placed[i] == 1 || preds_left[i] > 0)
                continue;
            start = slot;
            if (earliest[i] > start)
                start = earliest[i];
            if (best == -1 || is_better(g, priority, i, start, best, best_start)) {
                best = i;
                best_start = start;
            }
        }

        /* place it */
        result[k] = best;
        placed[best] = 1;
        slot = best_start + p->ins[g->node[best]].size;

        /* its successors: one predecessor less, maybe a later earliest slot */
        for (i = 0; i < n; i++) {
            if (g->edge[best][i] != -1) {
                preds_left[i]--;
                if (best_start + g->edge[best][i] > earliest[i])
                    earliest[i] = best_start + g->edge[best][i];
            }
        }
    }
}

/*
 * Stalls if the window is issued in order perm[], followed by the rest of
 * the block (rest_start .. block_end-1) in original order.
 * Works on a copy of the state, so *st is not changed.
 */
int count_window_stalls(Program *p, HazardModel *m, PipeState *st, Dag *g,
                        int perm[], int rest_start, int block_end)
{
    PipeState copy;
    int k;
    int stalls = 0;

    copy_state(&copy, st);
    for (k = 0; k < g->n; k++)
        stalls = stalls + issue(p, m, &copy, g->node[perm[k]]);
    for (k = rest_start; k < block_end; k++)
        stalls = stalls + issue(p, m, &copy, k);
    return stalls;
}

/*
 * Swap length: sum of |new position - original position| over all
 * instructions of the window (0 = original order).
 */
int swap_length(int perm[], int n)
{
    int k;
    int d = 0;
    for (k = 0; k < n; k++) {
        if (perm[k] > k)
            d = d + (perm[k] - k);
        else
            d = d + (k - perm[k]);
    }
    return d;
}

/*
 * Schedule the whole program.
 * order[k] = instruction placed at position k.
 */
void schedule_program(Program *p, HazardModel *m, Options *opt, int order[])
{
    PipeState st;
    Dag g;
    int nodes[MAX_WIN];
    int cand[3][MAX_WIN];       /* cand[0] = original order, cand[1] = height, cand[2] = succ */
    int b, k, c, n;
    int block_start, block_end;
    int ws, we;                 /* window = instructions ws .. we-1 */
    int best, best_stalls, best_len, s, len;

    for (k = 0; k < p->nins; k++)
        order[k] = k;
    if (opt->reorder == 0)
        return;

    reset_state(&st);

    for (b = 0; b < p->nblocks; b++) {
        block_start = p->blocks[b].start;
        block_end = p->blocks[b].end;

        for (ws = block_start; ws < block_end; ws = ws + opt->window) {
            we = ws + opt->window;
            if (we > block_end)
                we = block_end;
            n = we - ws;

            for (k = 0; k < n; k++)
                nodes[k] = ws + k;
            build_dag(p, m, &st, nodes, n, &g);

            /* candidate 0: original order */
            for (k = 0; k < n; k++)
                cand[0][k] = k;

            /* candidate 1: height rule, candidate 2: succ rule */
            list_schedule(p, &g, st.next_slot, PRIO_HEIGHT, cand[1]);
            list_schedule(p, &g, st.next_slot, PRIO_SUCC, cand[2]);

            /* keep the best: fewest stalls, then smallest swap length */
            best = 0;
            best_stalls = count_window_stalls(p, m, &st, &g, cand[0], we, block_end);
            best_len = 0;
            for (c = 1; c <= 2; c++) {
                s = count_window_stalls(p, m, &st, &g, cand[c], we, block_end);
                len = swap_length(cand[c], n);
                if (s < best_stalls || (s == best_stalls && len < best_len)) {
                    best = c;
                    best_stalls = s;
                    best_len = len;
                }
            }

            /* write the window into order[] and advance the pipeline state */
            for (k = 0; k < n; k++) {
                order[ws + k] = nodes[cand[best][k]];
                issue(p, m, &st, order[ws + k]);
            }
        }
    }
}
