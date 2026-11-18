/*
 * parser.c
 *
 * Reads a .s file line by line.
 *   - labels, directives, comments and blank lines are kept as LINE_TEXT
 *     and printed back unchanged, in the same place
 *   - every instruction becomes a LINE_INSTR "slot"; when the program is
 *     written back, slot k gets the instruction the scheduler put at
 *     position k
 * A trailing comment on an instruction line moves with the instruction.
 * A comment on its own line stays where it is.
 */
#include <stdio.h>
#include <string.h>
#include "common.h"

/* ------------------------------------------------------------------ */
/* small helpers on strings                                            */
/* ------------------------------------------------------------------ */

int is_space(char c)
{
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        return 1;
    return 0;
}

/* Remove spaces and tabs at both ends of s. s itself is changed. */
void trim(char s[])
{
    int len = strlen(s);
    int first = 0;
    int last = len - 1;
    int i;

    while (first < len && is_space(s[first]))
        first++;
    while (last >= first && is_space(s[last]))
        last--;

    /* move s[first..last] to the front of s */
    for (i = first; i <= last; i++)
        s[i - first] = s[i];
    s[last - first + 1] = '\0';
}

/* Position of the first c in s, or -1 if there is none. */
int find_char(char s[], char c)
{
    int i;
    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] == c)
            return i;
    }
    return -1;
}

/* Add src at the end of dst (dst never gets longer than MAX_TEXT-1 characters). */
void append(char dst[], char src[])
{
    int len = strlen(dst);
    int i = 0;
    while (src[i] != '\0' && len < MAX_TEXT - 1) {
        dst[len] = src[i];
        len++;
        i++;
    }
    dst[len] = '\0';
}

/* Value of one digit: '7' -> 7, 'b' -> 11. -1 if not a digit. */
int digit_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/*
 * If s is a number (12, -8, 0x50) store it in *value and return 1.
 * Otherwise return 0 (for example for a label name).
 */
int parse_number(char s[], long *value)
{
    int i = 0;
    int base = 10;
    int d;
    long v = 0;

    if (s[0] == '-')
        i = 1;
    if (s[i] == '0' && s[i + 1] == 'x') {
        base = 16;
        i = i + 2;
    }
    if (s[i] == '\0')
        return 0;                   /* no digits at all */

    while (s[i] != '\0') {
        d = digit_value(s[i]);
        if (d < 0 || d >= base)
            return 0;               /* not a digit: not a number */
        v = v * base + d;
        i++;
    }
    if (s[0] == '-')
        v = -v;
    *value = v;
    return 1;
}

/* Characters that separate the words of an instruction. */
int is_separator(char c)
{
    if (c == ' ' || c == '\t' || c == ',' || c == '(' || c == ')')
        return 1;
    return 0;
}

/*
 * Split an instruction into words.
 *    "lw t0, 8(a0)"  ->  words[0]="lw" words[1]="t0" words[2]="8" words[3]="a0"
 * Returns the number of words.
 */
int split_words(char code[], char words[][MAX_WORD])
{
    int i = 0;
    int n = 0;
    int len;

    while (code[i] != '\0') {
        if (is_separator(code[i])) {
            i++;
            continue;
        }
        if (n == MAX_WORDS)
            break;
        /* a word starts at i: copy it */
        len = 0;
        while (code[i] != '\0' && !is_separator(code[i])) {
            if (len < MAX_WORD - 1) {
                words[n][len] = code[i];
                len++;
            }
            i++;
        }
        words[n][len] = '\0';
        n++;
    }
    return n;
}

void add_text_line(Program *p, char text[])
{
    p->lines[p->nlines].type = LINE_TEXT;
    strcpy(p->lines[p->nlines].text, text);
    p->nlines++;
}

/* ------------------------------------------------------------------ */
/* operands                                                            */
/* ------------------------------------------------------------------ */

void set_rd(Instr *in, char name[])
{
    int r = reg_number(name);
    if (r < 0)
        printf("warning: line %d: '%s' is not a register\n", in->line_no, name);
    if (r <= 0)
        in->rd = -1;           /* no destination, or x0 (writes are thrown away) */
    else
        in->rd = r;
}

void add_src_reg(Instr *in, int r)
{
    /* x0 is always zero, so reading it is not a dependence */
    if (r > 0 && in->nsrc < MAX_SRC) {
        in->src[in->nsrc] = r;
        in->nsrc++;
    }
}

void add_src(Instr *in, char name[])
{
    int r = reg_number(name);
    if (r < 0)
        printf("warning: line %d: '%s' is not a register\n", in->line_no, name);
    add_src_reg(in, r);
}

/*
 * Set rd / src / memory fields from the words of the instruction.
 * w[0] is the mnemonic, w[1] the first operand, w[2] the second, ...
 * nops = number of operands.
 */
void decode_operands(Instr *in, OpInfo *op, char w[][MAX_WORD], int nops)
{
    long value = 0;

    switch (op->format) {

    case FMT_R:                                   /* add rd, rs1, rs2 */
        if (nops < 3) break;
        set_rd(in, w[1]);
        add_src(in, w[2]);
        add_src(in, w[3]);
        break;

    case FMT_I:                                   /* addi rd, rs1, imm */
        if (nops < 3) break;
        set_rd(in, w[1]);
        add_src(in, w[2]);
        break;

    case FMT_LOAD:                                /* lw rd, off(rs1)  -> words: lw rd off rs1 */
        if (nops < 3) break;
        set_rd(in, w[1]);
        in->mem_off_known = parse_number(w[2], &value);
        in->mem_off = value;
        in->mem_base = reg_number(w[3]);
        add_src(in, w[3]);
        break;

    case FMT_STORE:                               /* sw rs2, off(rs1) -> words: sw rs2 off rs1 */
        if (nops < 3) break;
        add_src(in, w[1]);
        in->mem_off_known = parse_number(w[2], &value);
        in->mem_off = value;
        in->mem_base = reg_number(w[3]);
        add_src(in, w[3]);
        break;

    case FMT_BRANCH:                              /* beq rs1, rs2, label */
        if (nops < 3) break;
        add_src(in, w[1]);
        add_src(in, w[2]);
        break;

    case FMT_U:                                   /* lui rd, imm */
        if (nops < 2) break;
        set_rd(in, w[1]);
        break;

    case FMT_JAL:                                 /* jal rd, label  or  jal label */
        if (nops == 1)
            in->rd = 1;                           /* ra */
        else if (nops >= 2)
            set_rd(in, w[1]);
        if (in->rd == -1)
            in->is_uncond = 1;                    /* jal x0, label is just j label */
        break;

    case FMT_JALR:                                /* jalr rd, off(rs1) */
        if (nops < 3) break;
        set_rd(in, w[1]);
        add_src(in, w[3]);
        if (in->rd == -1)
            in->is_uncond = 1;                    /* jalr x0, 0(rs1): return */
        break;

    case FMT_ECALL:                               /* reads a7 (call number), a0, a1 */
        add_src_reg(in, 17);
        add_src_reg(in, 10);
        add_src_reg(in, 11);
        break;

    case FMT_RD_RS:                               /* mv rd, rs */
        if (nops < 2) break;
        set_rd(in, w[1]);
        add_src(in, w[2]);
        break;

    case FMT_LI:                                  /* li rd, imm */
        if (nops < 2) break;
        set_rd(in, w[1]);
        if (parse_number(w[2], &value) && value >= -2048 && value <= 2047)
            in->size = 1;                         /* addi rd, x0, imm     */
        else
            in->size = 2;                         /* lui + addi           */
        break;

    case FMT_LA:                                  /* la rd, symbol (auipc + addi) */
        if (nops < 2) break;
        set_rd(in, w[1]);
        break;

    case FMT_BZ:                                  /* beqz rs, label */
    case FMT_JR:                                  /* jr rs */
        if (nops < 1) break;
        add_src(in, w[1]);
        break;

    case FMT_RET:                                 /* ret reads ra */
        add_src_reg(in, 1);
        break;

    case FMT_CALL:                                /* call writes ra */
        in->rd = 1;
        break;

    case FMT_TAIL:                                /* tail uses t1 as a temporary */
        in->rd = 6;
        break;

    default:                                      /* FMT_NONE, FMT_J: no registers */
        break;
    }
}

/* ------------------------------------------------------------------ */
/* one instruction                                                     */
/* ------------------------------------------------------------------ */

/*
 * code       = the instruction without comment, e.g. "lw t0, 0(a0)"
 * text       = what to print for it (keeps the original spacing/comment)
 * new_block  = 1 if a label or directive came just before it
 */
void add_instruction(Program *p, char code[], char text[], int line_no, int new_block)
{
    Instr *in = &p->ins[p->nins];
    char words[MAX_WORDS][MAX_WORD];
    int nwords, k;
    OpInfo op;

    /* start with an "empty" instruction */
    in->line_no = line_no;
    strcpy(in->text, text);
    strcpy(in->code, code);
    in->mnem[0] = '\0';
    in->known = 0;
    in->cls = CLASS_ALU;
    in->rd = -1;
    for (k = 0; k < MAX_SRC; k++)
        in->src[k] = 0;
    in->nsrc = 0;
    in->mem_base = -1;
    in->mem_off = 0;
    in->mem_off_known = 0;
    in->mem_bytes = 0;
    in->size = 1;
    in->starts_block = 0;
    in->ends_block = 0;
    in->is_uncond = 0;

    /* split "lw t0, 8(a0)" into  lw | t0 | 8 | a0 */
    nwords = split_words(code, words);
    if (nwords > 0)
        strcpy(in->mnem, words[0]);

    if (find_op(in->mnem, &op) == 0) {
        /* unknown: never move it, and do not move anything across it */
        printf("warning: line %d: unknown instruction '%s' (left in place)\n", line_no, in->mnem);
        in->known = 0;
        in->cls = CLASS_SYSTEM;
        in->ends_block = 1;
        new_block = 1;
    } else {
        in->known = 1;
        in->cls = op.cls;
        in->size = op.size;
        in->mem_bytes = op.mem_bytes;
        in->ends_block = op.ends_block;
        in->is_uncond = op.is_uncond;
        decode_operands(in, &op, words, nwords - 1);
    }

    /* does a new basic block start here? */
    if (p->nins == 0 || new_block == 1 || p->ins[p->nins - 1].ends_block == 1)
        in->starts_block = 1;

    p->lines[p->nlines].type = LINE_INSTR;
    p->nlines++;
    p->nins++;
}

/* ------------------------------------------------------------------ */
/* whole file                                                          */
/* ------------------------------------------------------------------ */

void find_blocks(Program *p)
{
    int i;
    p->nblocks = 0;
    for (i = 0; i < p->nins; i++) {
        if (p->ins[i].starts_block) {
            p->blocks[p->nblocks].start = i;
            p->nblocks++;
        }
        p->blocks[p->nblocks - 1].end = i + 1;
    }
}

int read_program(char filename[], Program *p)
{
    FILE *f;
    char line[MAX_TEXT];    /* the line as it is in the file         */
    char code[MAX_TEXT];    /* the line without comment and spaces   */
    char rest[MAX_TEXT];    /* what comes after "label:"             */
    char label[MAX_TEXT];
    char text[MAX_TEXT];
    int line_no = 0;
    int new_block = 1;
    int len, hash, colon, i;

    f = fopen(filename, "r");
    if (f == NULL) {
        printf("cannot open %s\n", filename);
        return -1;
    }
    p->nlines = 0;
    p->nins = 0;

    while (fgets(line, MAX_TEXT, f) != NULL) {
        line_no++;

        /* remove the newline at the end (keep leading spaces) */
        len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[len - 1] = '\0';
            len--;
        }

        /* code = the line without its comment, trimmed */
        strcpy(code, line);
        hash = find_char(code, '#');
        if (hash >= 0)
            code[hash] = '\0';
        trim(code);

        /* blank or comment-only line */
        if (code[0] == '\0') {
            add_text_line(p, line);
            continue;
        }

        /* label: everything before ':' (maybe followed by an instruction) */
        colon = find_char(code, ':');
        if (colon > 0) {
            new_block = 1;

            /* rest = everything after the colon */
            strcpy(rest, &code[colon + 1]);
            trim(rest);

            if (rest[0] == '\0' || rest[0] == '.') {
                add_text_line(p, line);           /* "loop:" or "arr: .word 1, 2" */
            } else {
                /* "loop: addi t0, t0, 1": the label stays, the instruction may move */
                for (i = 0; i < colon; i++)
                    label[i] = code[i];
                label[colon] = ':';
                label[colon + 1] = '\0';
                add_text_line(p, label);

                /* text = "    " + instruction + "  " + comment (if any) */
                text[0] = '\0';
                append(text, "    ");
                append(text, rest);
                hash = find_char(line, '#');
                if (hash >= 0) {
                    append(text, "  ");
                    append(text, &line[hash]);
                }
                add_instruction(p, rest, text, line_no, new_block);
                new_block = 0;
            }
            continue;
        }

        /* directive: .text .data .word ... */
        if (code[0] == '.') {
            add_text_line(p, line);
            new_block = 1;
            continue;
        }

        /* plain instruction */
        add_instruction(p, code, line, line_no, new_block);
        new_block = 0;
    }

    fclose(f);
    find_blocks(p);
    return 0;
}

/*
 * Print the program to out (a file, or stdout for the screen).
 * order[k] = instruction placed at position k.
 * If with_nops is 1, print nops[i] nops before instruction i.
 */
void write_program(FILE *out, Program *p, int order[], int nops[], int with_nops)
{
    int i, j, idx;
    int k = 0;      /* position in order[] */

    for (i = 0; i < p->nlines; i++) {
        if (p->lines[i].type == LINE_TEXT) {
            fprintf(out, "%s\n", p->lines[i].text);
        } else {
            idx = order[k];
            if (with_nops == 1) {
                for (j = 0; j < nops[idx]; j++)
                    fprintf(out, "    nop                 # inserted: hazard\n");
            }
            fprintf(out, "%s\n", p->ins[idx].text);
            k++;
        }
    }
}
