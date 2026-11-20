/*
 * dag.c
 *
 * Builds the dependence graph (DAG) of one window of instructions.
 * The graph is an adjacency matrix: edge[u][v].
 *
 * Edge u -> v means "v must stay after u". edge[u][v] is the smallest
 * number of slots between them:
 *
 *   'R' RAW  v reads a register u writes      gap from hazard.c
 *   'A' WAR  v writes a register u reads      just keep the order
 *   'O' WAW  v writes a register u writes     just keep the order
 *   'M' MEM  load/store pairs that may touch  just keep the order
 *            the same memory (at least one store)
 *   'C' CTRL everything before the branch/jump at the end of the block
 *
 * "just keep the order" = gap of occ(u) slots, i.e. v anywhere after u
 * (occ = slots the instruction occupies: 1, or more for la / big li / call).
 *
 * nodes[] is the window in its CURRENT order (the original order, or the
 * order a previous sliding window chose). Any legal order gives the
 * same dependences, so the graph is the same.
 */
#include "common.h"

/* Add edge u -> v (keep the bigger gap if the edge already exists). */
void add_edge(Dag *g, int u, int v, int gap, char type)
{
    if (g->edge[u][v] == -1) {
        g->edge[u][v] = gap;
        g->type[u][v] = type;
    } else {
        if (gap > g->edge[u][v])
            g->edge[u][v] = gap;
        if (type == 'R')
            g->type[u][v] = 'R';
    }
}

/*
 * Can loads/stores a and b touch the same bytes?  1 = maybe, 0 = no.
 * We only say "no" when both use the same base register holding the
 * same value (same last writer) and the byte ranges do not overlap.
 * Example: sw t0, 0(s0) and lw t1, 4(s0)  ->  no alias.
 */
int may_alias(Instr *a, int a_base_writer, Instr *b, int b_base_writer)
{
    if (a->mem_base < 0 || b->mem_base < 0)
        return 1;                       /* symbol address: unknown          */
    if (a->mem_base != b->mem_base)
        return 1;                       /* different registers: unknown     */
    if (a_base_writer != b_base_writer)
        return 1;                       /* base register changed in between */
    if (a->mem_off_known == 0 || b->mem_off_known == 0)
        return 1;

    /* byte ranges [off, off + bytes) */
    if (a->mem_off + a->mem_bytes <= b->mem_off)
        return 0;
    if (b->mem_off + b->mem_bytes <= a->mem_off)
        return 0;
    return 1;
}

/*
 * Cross-block look-ahead (a precaution for the next block).
 *
 * The first lines after the block (1 with fwd, 2 without) may read a value
 * that this window produces. If that producer ends up too close to the end
 * of the block, the next block stalls, and the next block often can not fix
 * it (e.g. all its lines depend on the value). So such a producer must have
 * enough slots after it, up to the end of the block:
 *
 *     need = slots of u - 1 + gap(u)          (from u's start to "ready")
 *            - position of the reader after the block
 *            - slots of the block after this window
 *
 * need[u] is a lower bound for u's height: the height rule then places u
 * early enough. Nothing is moved across the block edge.
 * Only when the block can fall through into the next line (it does not end
 * with j / jr / ret / tail).
 */
void cross_block_need(Program *p, HazardModel *m, Dag *g, int last_writer[],
                      int rest_start, int block_end, int need[])
{
    int rewritten[NREGS];       /* 1 = a later line writes r again: not our value */
    int rest_slots = 0;         /* slots of the block after the window            */
    int pos = 0;                /* slot of the reader, counted from the block end  */
    int k, j, c, r, u, x;

    for (r = 0; r < NREGS; r++)
        rewritten[r] = 0;
    for (k = rest_start; k < block_end; k++) {
        if (p->ins[k].rd > 0)
            rewritten[p->ins[k].rd] = 1;
        rest_slots = rest_slots + slots_used(p, m, k);
    }

    for (j = 0; j < lookahead_lines(m); j++) {
        c = block_end + j;
        if (c >= p->nins)
            break;
        for (k = 0; k < p->ins[c].nsrc; k++) {
            r = p->ins[c].src[k];
            u = last_writer[r];
            if (u == -1 || rewritten[r] == 1)
                continue;
            x = g->occ[u] - 1 + required_gap(p, m, g->node[u]) - pos - rest_slots;
            if (x > need[u])
                need[u] = x;
        }
        if (p->ins[c].rd > 0)
            rewritten[p->ins[c].rd] = 1;
        if (p->ins[c].is_uncond == 1)
            break;
        pos = pos + slots_used(p, m, c);
    }
}

/*
 * nodes[0..n-1] = instruction indices of the window, in their current order.
 * st = pipeline state just before the window (for registers written
 * by earlier instructions).
 * rest_start .. block_end-1 = the rest of the block after the window (still
 * in original order). block_end = -1: no cross-block look-ahead.
 */
void build_dag(Program *p, HazardModel *m, PipeState *st, int nodes[], int n, Dag *g,
               int rest_start, int block_end)
{
    int last_writer[NREGS];     /* node that last wrote register r, -1 = before window */
    int base_writer[MAX_WIN];   /* last writer of the base register, for may_alias     */
    int need[MAX_WIN];          /* least height asked for by the next block's lines    */
    int u, v, k, r, gap, ready;
    Instr *iu;
    Instr *iv;

    /* empty graph */
    g->n = n;
    for (u = 0; u < n; u++) {
        g->node[u] = nodes[u];
        g->ext_ready[u] = 0;
        g->height[u] = 0;
        g->nsucc[u] = 0;
        g->npred[u] = 0;
        g->occ[u] = slots_used(p, m, nodes[u]);
        base_writer[u] = -1;
        for (v = 0; v < n; v++)
            g->edge[u][v] = -1;
    }
    for (r = 0; r < NREGS; r++)
        last_writer[r] = -1;

    for (v = 0; v < n; v++) {
        iv = &p->ins[nodes[v]];

        /* RAW: from the last writer of each source register */
        for (k = 0; k < iv->nsrc; k++) {
            r = iv->src[k];
            u = last_writer[r];
            if (u != -1) {
                iu = &p->ins[nodes[u]];
                gap = g->occ[u] - 1 + required_gap(p, m, nodes[u]);
                add_edge(g, u, v, gap, 'R');
            } else if (st->writer[r] != -1) {
                /* written before the window: gives an earliest start slot */
                ready = st->slot[r] + required_gap(p, m, st->writer[r]);
                if (ready > g->ext_ready[v])
                    g->ext_ready[v] = ready;
            }
        }

        /* WAR and WAW: v writes a register an earlier node reads or writes */
        for (u = 0; u < v; u++) {
            iu = &p->ins[nodes[u]];
            if (iv->rd > 0 && reads_reg(iu, iv->rd))
                add_edge(g, u, v, g->occ[u], 'A');
            if (iv->rd > 0 && iu->rd == iv->rd)
                add_edge(g, u, v, g->occ[u], 'O');
        }

        /* memory: keep store/load and store/store order if they may alias */
        if (is_memory(iv)) {
            if (iv->mem_base > 0)
                base_writer[v] = last_writer[iv->mem_base];
            else
                base_writer[v] = -1;
            for (u = 0; u < v; u++) {
                iu = &p->ins[nodes[u]];
                if (!is_memory(iu))
                    continue;
                if (iu->cls == CLASS_LOAD && iv->cls == CLASS_LOAD)
                    continue;                       /* two loads can swap */
                if (may_alias(iu, base_writer[u], iv, base_writer[v]))
                    add_edge(g, u, v, g->occ[u], 'M');
            }
        }

        /* control: the branch/jump at the end of a block stays last */
        if (iv->ends_block == 1) {
            for (u = 0; u < v; u++)
                add_edge(g, u, v, g->occ[u], 'C');
        }

        /* now v is the last writer of its destination */
        if (iv->rd > 0)
            last_writer[iv->rd] = v;
    }

    /* count successors / predecessors */
    for (u = 0; u < n; u++) {
        for (v = 0; v < n; v++) {
            if (g->edge[u][v] != -1) {
                g->nsucc[u]++;
                g->npred[v]++;
            }
        }
    }

    /* what the next block needs early (if the block falls through into it) */
    for (u = 0; u < n; u++)
        need[u] = 0;
    if (block_end > 0 && p->ins[block_end - 1].is_uncond == 0)
        cross_block_need(p, m, g, last_writer, rest_start, block_end, need);

    /* height = longest path (sum of gaps) from u to the end, at least need[u].
       Edges only go forward, so go from the last node to the first. */
    for (u = n - 1; u >= 0; u--) {
        g->height[u] = g->occ[u];
        if (need[u] > g->height[u])
            g->height[u] = need[u];
        for (v = u + 1; v < n; v++) {
            if (g->edge[u][v] != -1 && g->edge[u][v] + g->height[v] > g->height[u])
                g->height[u] = g->edge[u][v] + g->height[v];
        }
    }
}
