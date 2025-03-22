/*    6800 Assembler
 *    Copyright
 *        (C) 2011 Joseph H. Allen
 *
 * This is free software; you can redistribute it and/or modify it under the 
 * terms of the GNU General Public License as published by the Free Software 
 * Foundation; either version 1, or (at your option) any later version.  
 *
 * It is distributed in the hope that it will be useful, but WITHOUT ANY 
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS 
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more 
 * details.  
 * 
 * You should have received a copy of the GNU General Public License along with 
 * this software; see the file COPYING.  If not, write to the Free Software Foundation, 
 * 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "exorsim.h"    /* JMR20201103 */
#include "asm6800.h"
#include "utils.h"    /* JMR20201103 */

/* Fixup types */

enum {
    FIXUP_EXT,    /* An extended address */
    FIXUP_DIR,    /* An 8-bit address */
    FIXUP_REL,    /* A branch offset */
};

/* A pending fixup */

struct fixup {
    struct fixup *next;
    unsigned short fixup;    /* Address of data which needs to be fixed up */
    int type;        /* Type of fixup */
    int ofst;
};

/* Symbol table */

struct symbol {
    struct symbol *next;
    char *name;        /* Name of symbol */
    int valid;        /* Set if symbol's value is known */
    unsigned short val;    /* Value of symbol */
    struct fixup *fixups;    /* Fixups waiting for this symbol */
} *symbols;

struct symbol *find_symbol(char *name)
{
    struct symbol *sy;
    for (sy = symbols; sy; sy = sy->next)
        if (!strcmp(sy->name, name))
            return sy;
    sy = (struct symbol *)malloc(sizeof(struct symbol));
    sy->next = symbols;
    symbols = sy;
    sy->name = strdup(name);
    sy->valid = 0;
    sy->val = 0;
    sy->fixups = 0;
    return sy;
}

/* Get symbol name by address */

char *find_label(unsigned short val)
{
    struct symbol *sy;
    for (sy = symbols; sy; sy = sy->next)
        if (sy->val == val)
            return sy->name;
    return 0;
}

/* Add a fixup */

void add_fixup(struct symbol *sy, unsigned short addr, int type, int ofst)
{
    struct fixup *fix = (struct fixup *)malloc(sizeof(struct fixup));
    fix->type = type;
    fix->fixup = addr;
    fix->next = sy->fixups;
    fix->ofst = ofst;
    sy->fixups = fix;
}

/* Show symbol table */

void show_syms(FILE *f)
{
    struct symbol *sy;
    for (sy = symbols; sy; sy = sy->next) {
        struct fixup *fix;
        if (sy->valid) {
            fprintf(f,"%4.4X %s\n", sy->val, sy->name);
        } else {
            fprintf(f,"???? %s\n", sy->name);
        }
        for (fix = sy->fixups; fix; fix = fix->next) {
            if (fix->type == FIXUP_EXT)
                fprintf(f,"    16-bit fixup at %4.4X\n", fix->fixup);
            else if (fix->type == FIXUP_DIR)
                fprintf(f,"    8-bit fixup at  %4.4X\n", fix->fixup);
            else if (fix->type == FIXUP_REL)
                fprintf(f,"    8-bit rel fixup %4.4X\n", fix->fixup);
        }
    }
}

/* Clear symbol table */

void clr_syms(void)
{
    struct symbol *sy;
    while ((sy = symbols)) {
        struct fixup *fix;
        symbols = sy->next;
        while ((fix = sy->fixups)) {
            sy->fixups = fix->next;
            free(fix);
        }
        free(sy->name);
        free(sy);
    }
}

/* Set symbol's value, process pending fixups */

void set_symbol(unsigned char *mem, struct symbol *sy, unsigned short val)
{
    struct fixup *fix;
    if (!sy)
        return;
    if (sy->valid) {
        printf("Symbol '%s' already defined to %4.4x\n", sy->name, sy->val);
        return;
    }
    sy->valid = 1;
    sy->val = val;
    while ((fix = sy->fixups)) {
        sy->fixups = fix->next;
        if (fix->type == FIXUP_EXT) {
            mem[fix->fixup] = ((val + fix->ofst) >> 8);
            mem[fix->fixup + 1]  = (val + fix->ofst);
            printf("Address at %4.4X set to %4.4X\n", fix->fixup, val + fix->ofst);
        } else if (fix->type == FIXUP_DIR) {
            mem[fix->fixup]  = (val + fix->ofst);
            printf("Byte at %4.4X set to %2.2X\n", fix->fixup, ((val + fix->ofst) & 255));
        } else if (fix->type == FIXUP_REL) {
            mem[fix->fixup] = val + fix->ofst - (fix->fixup + 1);
            printf("Offset at %4.4X set to %2.2X\n", fix->fixup, val + fix->ofst - (fix->fixup + 1));
        }
        free(fix);
    }
}

/* Instruction type codes
 *
 *   RM   #imm8  -> 0x00
 *    dir8 -> 0x10
 *    nn,x -> 0x20
 *    ext16 -> 0x30
 *
 *   IDX  #imm16 -> 0x00
 *    dir8 -> 0x10
 *    nn,x -> 0x20
 *    ext16 -> 0x30
 *
 *   REL  rel8
 *
 *   RMW  nn,x  -> 0x20
 *    ext16 -> 0x30
 */

enum {
    RM,    /* Register-Memory */
    RMW,    /* Read-modify-write */
    REL,    /* Relative branch */
    IDX,    /* Index register-Memory */
    FCB,    /* FCB pseudo-op */
    FDB,    /* FDB pseudo-op */
    EQU,    /* EQU pseudo-op */
    RMB,    /* RMB pseudo-op */
    ORG,    /* ORG pseudo-op */
    IGN,    /* Ignore these pseudo-ops */
    ACC,    /* Accumulator needed */
    ACC1,    /* Accumulator optional */
    ACCB,    /* we need an A or a B: add 0x01 for B */
/* #ifdef SIM6801 */
    ACCD,    /* LDD,STD,ADDD,SUBD JMR20201117 */
/* #endif / * def SIM6801 */
    NONE    /* No operand */
};

/* Include these in enum? */
#define STO 0x80    /* STore Only, no immediate, must be single bit JMR20201122 */
#define STunMASK( type ) ( (type) & 0x1F )    /* breathing room for more than 16 enums JMR20201122 */

struct { const char *insn; int opcode; int type; unsigned cpu; } table[] =
{
    { "lda", 0x86, ACC1, 0x6800 },
    { "sta", 0x87, ACC1|STO, 0x6800 },
    { "ora", 0x8a, ACC1, 0x6800 },

    { "sub", 0x80, ACC, 0x6800 },
    { "cmp", 0x81, ACC, 0x6800 },
    { "sbc", 0x82, ACC, 0x6800 },
    { "and", 0x84, ACC, 0x6800 },
    { "bit", 0x85, ACC, 0x6800 },
    { "eor", 0x88, ACC, 0x6800 },
    { "adc", 0x89, ACC, 0x6800 },
    { "add", 0x8b, ACC, 0x6800 },

    { "suba", 0x80, RM, 0x6800 },
    { "cmpa", 0x81, RM, 0x6800 },
    { "sbca", 0x82, RM, 0x6800 },
    { "anda", 0x84, RM, 0x6800 },
    { "bita", 0x85, RM, 0x6800 },
    { "ldaa", 0x86, RM, 0x6800 },
    { "staa", 0x87, RM|STO, 0x6800 },
    { "eora", 0x88, RM, 0x6800 },
    { "adca", 0x89, RM, 0x6800 },
    { "oraa", 0x8a, RM, 0x6800 },
    { "adda", 0x8b, RM, 0x6800 },

    { "subb", 0xc0, RM, 0x6800 },
    { "cmpb", 0xc1, RM, 0x6800 },
    { "sbcb", 0xc2, RM, 0x6800 },
    { "andb", 0xc4, RM, 0x6800 },
    { "bitb", 0xc5, RM, 0x6800 },
    { "ldb", 0xc6, RM, 0x6800 },
    { "stb", 0xc7, RM|STO, 0x6800 },
    { "ldab", 0xc6, RM, 0x6800 },
    { "stab", 0xc7, RM|STO, 0x6800 },
    { "eorb", 0xc8, RM, 0x6800 },
    { "adcb", 0xc9, RM, 0x6800 },
    { "orb", 0xca, RM, 0x6800 },
    { "orab", 0xca, RM, 0x6800 },
    { "addb", 0xcb, RM, 0x6800 },

    { "subd", 0x83, ACCD, 0x6801 },
    { "addd", 0xc3, ACCD, 0x6801 },
    { "ldd", 0xcc, ACCD, 0x6801 },
    { "std", 0xcd, ACCD|STO, 0x6801 },

    { "cpx", 0x8c, IDX, 0x6800 },
    { "bsr", 0x8d, REL, 0x6800 },
    { "lds", 0x8e, IDX, 0x6800 },
    { "sts", 0x8f, IDX|STO, 0x6800 },
    { "jsr", 0x8d, RMW, 0x6800 },
    { "jsr", 0x8d, IDX|STO, 0x6801 }, /* Must come after 6800 jsr! */
    { "ldx", 0xce, IDX, 0x6800 },
    { "stx", 0xcf, IDX|STO, 0x6800 },

    { "neg", 0x40, RMW, 0x6800 },
    { "com", 0x43, RMW, 0x6800 },
    { "lsr", 0x44, RMW, 0x6800 },
    { "ror", 0x46, RMW, 0x6800 },
    { "asr", 0x47, RMW, 0x6800 },
    { "asl", 0x48, RMW, 0x6800 },
    { "lsl", 0x48, RMW, 0x6800 }, /* alias */
    { "rol", 0x49, RMW, 0x6800 },
    { "dec", 0x4a, RMW, 0x6800 },
    { "inc", 0x4c, RMW, 0x6800 },
    { "tst", 0x4d, RMW, 0x6800 },
    { "jmp", 0x4e, RMW, 0x6800 },
    { "clr", 0x4f, RMW, 0x6800 },

    { "nega", 0x40, NONE, 0x6800 },
    { "coma", 0x43, NONE, 0x6800 },
    { "lsra", 0x44, NONE, 0x6800 },
    { "rora", 0x46, NONE, 0x6800 },
    { "asra", 0x47, NONE, 0x6800 },
    { "asla", 0x48, NONE, 0x6800 },
    { "lsla", 0x48, NONE, 0x6800 }, /* alias */
    { "rola", 0x49, NONE, 0x6800 },
    { "deca", 0x4a, NONE, 0x6800 },
    { "inca", 0x4c, NONE, 0x6800 },
    { "tsta", 0x4d, NONE, 0x6800 },
    { "clra", 0x4f, NONE, 0x6800 },

    { "negb", 0x50, NONE, 0x6800 },
    { "comb", 0x53, NONE, 0x6800 },
    { "lsrb", 0x54, NONE, 0x6800 },
    { "rorb", 0x56, NONE, 0x6800 },
    { "asrb", 0x57, NONE, 0x6800 },
    { "aslb", 0x58, NONE, 0x6800 },
    { "lslb", 0x58, NONE, 0x6800 }, /* alias */
    { "rolb", 0x59, NONE, 0x6800 },
    { "decb", 0x5a, NONE, 0x6800 },
    { "incb", 0x5c, NONE, 0x6800 },
    { "tstb", 0x5d, NONE, 0x6800 },
    { "clrb", 0x5f, NONE, 0x6800 },

    { "nop", 0x01, NONE, 0x6800 },
    { "lsrd", 0x04, NONE, 0x6801 },
    { "asld", 0x05, NONE, 0x6801 },
    { "lsld", 0x05, NONE, 0x6801 }, /* alias */
    { "tap", 0x06, NONE, 0x6800 },
    { "tpa", 0x07, NONE, 0x6800 },
    { "inx", 0x08, NONE, 0x6800 },
    { "dex", 0x09, NONE, 0x6800 },
    { "clv", 0x0a, NONE, 0x6800 },
    { "sev", 0x0b, NONE, 0x6800 },
    { "clc", 0x0c, NONE, 0x6800 },
    { "sec", 0x0d, NONE, 0x6800 },
    { "cli", 0x0e, NONE, 0x6800 },
    { "sei", 0x0f, NONE, 0x6800 },
    { "sba", 0x10, NONE, 0x6800 },
    { "cba", 0x11, NONE, 0x6800 },
    { "tab", 0x16, NONE, 0x6800 },
    { "tba", 0x17, NONE, 0x6800 },
    { "daa", 0x19, NONE, 0x6800 },
    { "aba", 0x1b, NONE, 0x6800 },

    { "bra", 0x20, REL, 0x6800 },
    { "brn", 0x21, REL, 0x6801 },
    { "bhi", 0x22, REL, 0x6800 },
    { "bls", 0x23, REL, 0x6800 },
    { "bcc", 0x24, REL, 0x6800 },
    { "bhs", 0x24, REL, 0x6800 }, /* alias */
    { "bcs", 0x25, REL, 0x6800 },
    { "blo", 0x25, REL, 0x6800 }, /* alias */
    { "bne", 0x26, REL, 0x6800 },
    { "beq", 0x27, REL, 0x6800 },
    { "bvc", 0x28, REL, 0x6800 },
    { "bvs", 0x29, REL, 0x6800 },
    { "bpl", 0x2a, REL, 0x6800 },
    { "bmi", 0x2b, REL, 0x6800 },
    { "bge", 0x2c, REL, 0x6800 },
    { "blt", 0x2d, REL, 0x6800 },
    { "bgt", 0x2e, REL, 0x6800 },
    { "ble", 0x2f, REL, 0x6800 },

    { "tsx", 0x30, NONE, 0x6800 },
    { "ins", 0x31, NONE, 0x6800 },
    { "pul", 0x32, ACCB, 0x6800 },
    { "pula", 0x32, NONE, 0x6800 },
    { "pulb", 0x33, NONE, 0x6800 },
    { "des", 0x34, NONE, 0x6800 },
    { "txs", 0x35, NONE, 0x6800 },
    { "psh", 0x36, ACCB, 0x6800 },
    { "psha", 0x36, NONE, 0x6800 },
    { "pshb", 0x37, NONE, 0x6800 },
    { "pulx", 0x38, NONE, 0x6801 },
    { "rts", 0x39, NONE, 0x6800 },
    { "abx", 0x3a, NONE, 0x6801 },
    { "rti", 0x3b, NONE, 0x6800 },
    { "pshx", 0x3c, NONE, 0x6801 },
    { "mul", 0x3d, NONE, 0x6801 },
    { "wai", 0x3e, NONE, 0x6800 },
    { "swi", 0x3f, NONE, 0x6800 },

    { "fcb", 0, FCB, 0x6800 },
    { "fcc", 0, FCB, 0x6800 },
    { "fdb", 0, FDB, 0x6800 },
    { "equ", 0, EQU, 0x6800 },
    { "rmb", 0, RMB, 0x6800 },
    { "org", 0, ORG, 0x6800 },

    { "end", 0, IGN, 0x6800 },
    { "mon", 0, IGN, 0x6800 },
    { "opt", 0, IGN, 0x6800 },
    { "nam", 0, IGN, 0x6800 },
    { "ttl", 0, IGN, 0x6800 },
    { "spc", 0, IGN, 0x6800 },
    { "page", 0, IGN, 0x6800 },

    { 0, 0, 0, 0 }
};

/* Parse a value (this should really be an expression parser) */

int parse_val(char **buf, int *operand, struct symbol **sy, unsigned short addr)
{
    char str[80];
    *sy = 0;
    if (!parse_dec(buf, operand)) {
        if (parse_word(buf, str)) {
            char *p;
            if (!strcmp(str, "*")) {
                *operand = addr;
            } else {
                *sy = find_symbol(str);
                *operand = (*sy)->val;
                if ((*sy)->valid)
                    *sy = 0;
            }
            p = *buf;
            if (*p == '+' || *p == '-') {
                char c = *p++;
                int ofst;
                *buf = p;
                if (parse_dec(buf, &ofst)) {
                    if (c == '+')
                        *operand += ofst;
                    else
                        *operand -= ofst;
                } else {
                    printf("Missing value after + or -\n");
                }
            }
        } else {
            return 0;
        }
    }
    return 1;
}

unsigned assemble(unsigned char *mem, unsigned addr, char *buf)
{
    char str[80];
    unsigned short label_addr = addr;
    struct symbol *label_sy;
    struct symbol *sy;
    int opcode = -1;
    int operand;
    int type;
    int store_only = 0;    /* JMR 20201122 */
    int x;

    label_sy = 0;
    sy = 0;

    if (buf[0] == '*' || !buf[0]) {
        /* Comment line, ignore */
        return addr;
    }

    if (!(buf[0] == ' ' || buf[0] == '\t') && parse_word(&buf, str)) {
        /* A label */
        label_sy = find_symbol(str);

        skipws(&buf);

        if (!parse_word(&buf, str)) {
            goto done;
        }
    } else {
        skipws(&buf);
        if (!parse_word(&buf, str)) {
            printf("Huh?\n");
            return addr;
        }
    }

    /* Lookup instruction */
    for (x = 0; table[x].insn; ++x)
        if (!jstricmp(table[x].insn, str) && table[x].cpu <= cputype) { /* JMR20201122 */
            opcode = table[x].opcode;
            type = STunMASK( table[x].type );
            store_only = table[x].type & STO;
            break;
        }

    if (!table[x].insn) {
        printf("Huh?\n");
        return addr;
    }

    skipws(&buf);

    if (type == IGN) {
        goto done;
    } else if (type == NONE) {
        mem[addr++] = opcode;
        goto done;
    } else if (type == ACC) {
        type = RM;
        if ((buf[0] == 'a' || buf[0] == 'A') && (!buf[1] || buf[1] == ' ' || buf[1] == '\t')) {
            buf++;
        } else if ((buf[0] == 'b' || buf[0] == 'B') && (!buf[1] || buf[1] == ' ' || buf[1] == '\t')) {
            opcode += 0x40;
            buf++;
        } else {
            printf("Accu missing after intrustion\n");
            return addr;
        }
        skipws(&buf);
        goto norm;
    } else if (type == ACC1) {
        type = RM;
        if ((buf[0] == 'a' || buf[0] == 'A') && (!buf[1] || buf[1] == ' ' || buf[1] == '\t')) {
            buf++;
        } else if ((buf[0] == 'b' || buf[0] == 'B') && (!buf[1] || buf[1] == ' ' || buf[1] == '\t')) {
            opcode += 0x40;
            buf++;
        }
        skipws(&buf);
        goto norm;
    } else if (type == RMW) {
        /* More syntax of old asm: psh a, inc a, etc. */
        if ((buf[0] == 'a' || buf[0] == 'A') && (!buf[1] || buf[1] == ' ' || buf[1] == '\t')) {
            mem[addr++] = opcode;
            goto done;
        } else if ((buf[0] == 'b' || buf[0] == 'B') && (!buf[1] || buf[1] == ' ' || buf[1] == '\t')) {
            mem[addr++] = opcode + 0x10;
            goto done;
        } else {
            goto normal;
        }
    } else if (type == ACCB) {
        /* More syntax of old asm: psh a, inc a, etc. */
        if ((buf[0] == 'a' || buf[0] == 'A') && (!buf[1] || buf[1] == ' ' || buf[1] == '\t')) {
            mem[addr++] = opcode;
            goto done;
        } else if ((buf[0] == 'b' || buf[0] == 'B') && (!buf[1] || buf[1] == ' ' || buf[1] == '\t')) {
            mem[addr++] = opcode + 0x01;
            goto done;
        } else {
            printf("Missing accumulator\n");
            return addr;
        }
    } else if (type == FCB) {
        char c;
        if (!*buf) {
            printf("Missing value\n");
            return addr;
        }
        fcb_loop:
        /* Special case for FCB/FCC */
        if (*buf == '"' || *buf == '/') {
            c = *buf++;
            while (*buf && *buf != c) {
                mem[addr++] = *buf++;
            }
            if (*buf)
                ++buf;
        } else if (parse_val(&buf, &operand, &sy, addr)) {
            if (sy) {
                add_fixup(sy, addr, FIXUP_DIR, operand);
                mem[addr++] = 0;
            } else {
                mem[addr++] = operand;
            }
        } else {
            mem[addr++] = 0;
        }
        skipws(&buf);
        if (buf[0] == ',') {
            ++buf;
            skipws(&buf);
            goto fcb_loop;
        }
        goto done;
    } else {
        norm:
        if (*buf == '#' && type != RMW && !store_only) { /* JMR20201122 */
            ++buf;
            if (!parse_val(&buf, &operand, &sy, addr)) {
                printf("Missing number or label after #\n");
                return addr;
            }
            if (type == RM) {
                /* 8-bit immediate */
                mem[addr++] = opcode;
                if (sy)
                    add_fixup(sy, addr, FIXUP_DIR, operand);
                mem[addr++] = operand;
                goto done;
            } else if (type == IDX || (cputype >= SIM6801 && type == ACCD)) {
                /* 16-bit immediate */
                mem[addr++] = opcode;
                if (sy)
                    add_fixup(sy, addr, FIXUP_EXT, operand);
                mem[addr++] = (operand >> 8);
                mem[addr++] = operand;
                goto done;
            } else {
                printf("Invalid operand\n");
                return addr;
            }
        } else {
            normal:
            /* Check for stupid syntax of old asm: lda x is same as lda 0,x */
            if ((buf[0] == 'x' || buf[0] == 'X') && (!buf[1] || buf[1] == ' ' || buf[1] == '\t')) {
                operand = 0;
                goto ndx;
            }
            if (parse_val(&buf, &operand, &sy, addr)) {
                if (buf[0] == ',' && (buf[1] == 'x' || buf[1] == 'X')) {
                    ndx:
                    /* We have an indexed operand */
                    if (type == RM || type == IDX || type == RMW 
                        || (cputype >= 0x6801 && type == ACCD)) {
                        mem[addr++] = opcode + 0x20;;
                        if (sy)
                            add_fixup(sy, addr, FIXUP_DIR, operand);
                        mem[addr++] = operand;
                        goto done;
                    } else {
                        printf("Invalid operand for this instruction\n");
                        return addr;
                    }
                } else {
                    /* We have a direct address operand */
                    if ((type == RM || type == IDX || (cputype >= 0x6801 && type == ACCD)) 
                        && !sy && operand < 256) {
                        mem[addr++] = opcode + 0x10;
                        mem[addr++] = operand;
                        goto done;
                    } else if (type == RM || type == IDX || type == RMW 
                           || (cputype >= 0x6801 && type == ACCD)) {
                        mem[addr++] = opcode + 0x30;
                        if (sy)
                            add_fixup(sy, addr, FIXUP_EXT, operand);
                        mem[addr++] = (operand >> 8);
                        mem[addr++] = operand;
                        goto done;
                    } else if (type == REL) {
                        mem[addr++] = opcode;
                        if (sy) {
                            mem[addr] = 0;
                            add_fixup(sy, addr, FIXUP_REL, operand);
                        } else {
                            mem[addr] = operand - (addr + 1);
                        }
                        addr++;
                        goto done;
                    } else if (type == FDB) {
                        if (!*buf) {
                            printf("Value missing\n");
                            return addr;
                        }
                        more_fdb:
                        if (sy) {
                            add_fixup(sy, addr, FIXUP_EXT, operand);
                            mem[addr++] = 0;
                            mem[addr++] = 0;
                        } else {
                            mem[addr++] = (operand >> 8);
                            mem[addr++] = operand;
                        }
                        skipws(&buf);
                        if (buf[0] == ',') {
                            ++buf;
                            skipws(&buf);
                            if (parse_val(&buf, &operand, &sy, addr)) {
                                goto more_fdb;
                            } else {
                                operand = 0;
                                sy = 0;
                                goto more_fdb;
                            }
                        }
                        goto done;
                    } else if (type == RMB) {
                        if (sy) {
                            printf("Resolved symbol required for rmb\n");
                        } else {
                            addr += operand;
                        }
                        goto done;
                    } else if (type == EQU) {
                        if (sy) {
                            printf("Resolved symbol required for equ\n");
                        } else if (!label_sy) {
                            printf("Label required for equ\n");
                        } else {
                            label_addr = operand;
                        }
                        goto done;
                    } else if (type == ORG) {
                        if (sy) {
                            printf("Resolved symbol required for org\n");
                        }
                        label_addr = addr = operand;
                        goto done;
                    } else {
                        printf("Invalid operand for this instruction\n");
                        return addr;
                    }
                }
            }
            /* We have no operand */
            if (type == NONE) {
                mem[addr++] = opcode;
                goto done;
            } else {
                printf("Operand required\n");
                return addr;
            }
        }
    }
    done:
    set_symbol(mem, label_sy, label_addr);
    return addr;
}
