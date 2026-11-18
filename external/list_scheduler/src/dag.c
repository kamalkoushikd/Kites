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
 * "just keep the order" = gap of size(u) slots, i.e. v anywhere after u.
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
 * nodes[0..n-1] = instruction indices of the window, in original order.
 * st = pipeline state just before the window (for registers written
 * by earlier instructions).
 */
void build_dag(Program *p, HazardModel *m, PipeState *st, int nodes[], int n, Dag *g)
{
    int last_writer[32];        /* node that last wrote register r, -1 = before window */
    int base_writer[MAX_WIN];   /* last writer of the base register, for may_alias     */
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
        base_writer[u] = -1;
        for (v = 0; v < n; v++)
            g->edge[u][v] = -1;
    }
    for (r = 0; r < 32; r++)
        last_writer[r] = -1;

    for (v = 0; v < n; v++) {
        iv = &p->ins[nodes[v]];

        /* RAW: from the last writer of each source register */
        for (k = 0; k < iv->nsrc; k++) {
            r = iv->src[k];
            u = last_writer[r];
            if (u != -1) {
                iu = &p->ins[nodes[u]];
                gap = iu->size - 1 + required_gap(p, m, nodes[u]);
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
                add_edge(g, u, v, iu->size, 'A');
            if (iv->rd > 0 && iu->rd == iv->rd)
                add_edge(g, u, v, iu->size, 'O');
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
                    add_edge(g, u, v, iu->size, 'M');
            }
        }

        /* control: the branch/jump at the end of a block stays last */
        if (iv->ends_block == 1) {
            for (u = 0; u < v; u++)
                add_edge(g, u, v, p->ins[nodes[u]].size, 'C');
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

    /* height = longest path (sum of gaps) from u to the end.
       Edges only go forward, so go from the last node to the first. */
    for (u = n - 1; u >= 0; u--) {
        g->height[u] = p->ins[nodes[u]].size;
        for (v = u + 1; v < n; v++) {
            if (g->edge[u][v] != -1 && g->edge[u][v] + g->height[v] > g->height[u])
                g->height[u] = g->edge[u][v] + g->height[v];
        }
    }
}
