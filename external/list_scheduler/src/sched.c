/*
 * sched.c
 *
 * List scheduling.
 *
 * For every basic block, cut into windows of W instructions:
 *   1. build the DAG of the window                           (dag.c)
 *   2. make one schedule with each rule: height, succ        (list_schedule)
 *      and the same two again with the cross-block precaution: producers
 *      that the next block's first lines read get a higher height (dag.c)
 *   3. compare the current order and the four schedules; keep the one with
 *        a. the fewest stalls inside the block (window + rest of the block)
 *        b. on a tie, the fewest stalls in the next block's first lines
 *           (cross-block: only decides between orders that are equally good
 *           inside the block, so it never costs a stall inside the block)
 *        c. on a tie, the smallest swap length
 *           (sum of |new position - original position|)
 *      If that also ties, the earlier one is kept (current order first),
 *      so with no savings the code is unchanged.
 * The windows move through the block in one of two ways (option -a,
 * see commit_count): slide (default) or fixed.
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
        slot = best_start + g->occ[best];

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
 * the block (rest_start .. block_end-1) in original order. Returns those
 * stalls (inside the block). *next_stalls = stalls of the next block's first
 * lines (1 with fwd, 2 without), which this block's end can still cause
 * (cross-block look-ahead; the next block is not scheduled yet, so its
 * original order is used). Works on a copy of the state: *st is not changed.
 */
int count_window_stalls(Program *p, HazardModel *m, PipeState *st, Dag *g,
                        int perm[], int rest_start, int block_end, int *next_stalls)
{
    PipeState copy;
    int k;
    int stalls = 0;
    int after = 0;

    copy_state(&copy, st);
    for (k = 0; k < g->n; k++)
        stalls = stalls + issue(p, m, &copy, g->node[perm[k]]);
    /* positions after the window have not been touched yet: they are
       still in the original order, so position k holds instruction k */
    for (k = rest_start; k < block_end; k++)
        stalls = stalls + issue(p, m, &copy, k);

    /* the next block's first lines (not scheduled yet: original order).
       After j / jr / ret, issue() has cleared the state, so they add 0. */
    for (k = block_end; k < block_end + lookahead_lines(m) && k < p->nins; k++) {
        after = after + issue(p, m, &copy, k);
        if (p->ins[k].is_uncond == 1)
            break;
    }
    *next_stalls = after;
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
 * How many instructions of a window are kept for good before the next
 * window starts (the rest are scheduled again by the next window).
 *   fixed:   all W  -> next window starts right after this one
 *   slide:   1      -> next window starts one position later
 */
int commit_count(Options *opt)
{
    if (opt->win_mode == WIN_SLIDE)
        return 1;
    return opt->window;
}

/*
 * Schedule the whole program.
 * order[k] = instruction placed at position k.
 *
 * order[] starts as the original order and is changed in place. For each
 * block, a window covers positions pos .. pos+W-1 of the CURRENT order:
 *   1. build the DAG of those instructions (in their current order)
 *   2. make the height and the succ schedule (list_schedule)
 *   3. keep the best of: current order, height, succ
 *      (fewest stalls, then smallest swap length, then the earlier one)
 *   4. write it back into order[], keep the first commit_count() positions
 *      for good (issue them into the pipeline state) and move pos on.
 * With the fixed mode every position is kept at once, so each window is
 * scheduled exactly once (the original algorithm).
 */
void schedule_program(Program *p, HazardModel *m, Options *opt, int order[])
{
    PipeState st;
    Dag g;
    int nodes[MAX_WIN];
    Dag g2;                     /* the same window with the cross-block heights */
    int cand[5][MAX_WIN];       /* current, height, succ, height+cb, succ+cb      */
    int nb, best_next;          /* stalls in the next block's first lines        */
    int b, k, c, n, keep;
    int block_end;
    int pos, we;                /* window = positions pos .. we-1 of order[] */
    int best, best_stalls, best_len, s, len;

    for (k = 0; k < p->nins; k++)
        order[k] = k;
    if (opt->reorder == 0)
        return;

    reset_state(&st);

    for (b = 0; b < p->nblocks; b++) {
        block_end = p->blocks[b].end;
        pos = p->blocks[b].start;

        while (pos < block_end) {
            we = pos + opt->window;
            if (we > block_end)
                we = block_end;
            n = we - pos;

            for (k = 0; k < n; k++)
                nodes[k] = order[pos + k];
            build_dag(p, m, &st, nodes, n, &g, we, -1);
            build_dag(p, m, &st, nodes, n, &g2, we, block_end);

            /* candidate 0: the current order */
            for (k = 0; k < n; k++)
                cand[0][k] = k;

            /* candidates 1, 2: height and succ rule (as in P-1);
               candidates 3, 4: the same with the cross-block precaution */
            list_schedule(p, &g, st.next_slot, PRIO_HEIGHT, cand[1]);
            list_schedule(p, &g, st.next_slot, PRIO_SUCC, cand[2]);
            list_schedule(p, &g2, st.next_slot, PRIO_HEIGHT, cand[3]);
            list_schedule(p, &g2, st.next_slot, PRIO_SUCC, cand[4]);

            /* keep the best: fewest stalls inside the block, then fewest
               stalls at the next block's start, then smallest swap length */
            best = 0;
            best_stalls = count_window_stalls(p, m, &st, &g, cand[0], we, block_end, &best_next);
            best_len = 0;
            for (c = 1; c <= 4; c++) {
                s = count_window_stalls(p, m, &st, &g, cand[c], we, block_end, &nb);
                len = swap_length(cand[c], n);
                if (s < best_stalls || (s == best_stalls && nb < best_next) ||
                    (s == best_stalls && nb == best_next && len < best_len)) {
                    best = c;
                    best_stalls = s;
                    best_next = nb;
                    best_len = len;
                }
            }

            /* write the window back into order[] */
            for (k = 0; k < n; k++)
                order[pos + k] = nodes[cand[best][k]];

            /* keep the first positions for good; the last window of a block
               keeps everything */
            keep = commit_count(opt);
            if (we == block_end || keep > n)
                keep = n;
            for (k = 0; k < keep; k++)
                issue(p, m, &st, order[pos + k]);
            pos = pos + keep;
        }
    }
}
