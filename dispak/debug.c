/*
 * Instruction-level interactive debugger.
 * Resembles DIAPAK debugger.
 *
 * Hand-written replacement for the bison grammar debug.y,
 * providing the same command language:
 *   q | f | g [OCT] | s [n DEC | OCT] | w | h | b OCT | W OCT |
 *   e OO | t [D] | v | n | j | x ... |
 *   a - | a d | y - | i OCT - | OCT - | OCT t/b/u/I/i// /d |
 *   a = WORD | y = WORD | i OCT = OCT | OCT = WORD |
 *   d OCT - OCT
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You can redistribute this program and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation;
 * either version 2 of the License, or (at your discretion) any later version.
 * See the accompanying file "COPYING" for more details.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "defs.h"
#include "optab.h"
#include "encoding.h"

#define PROMPT "- "

static alureg_t wd;
static void pfloat(unsigned long, unsigned long);
static void prcmd(int);
static void prword(int);

/*
 * Лексер: пропускает пробелы, выдаёт очередной символ.
 */
static int
lex (void)
{
        while (*lineptr == ' ' || *lineptr == '\t')
                ++lineptr;
        if (*lineptr)
                return (*lineptr++);
        return (0);
}

static int
peek (void)
{
        while (*lineptr == ' ' || *lineptr == '\t')
                ++lineptr;
        return (*lineptr);
}

/*
 * Восьмеричное число (1 и более восьмеричных цифр).
 */
static int
octal (void)
{
        int v = 0, c;

        while ((c = peek ()) >= '0' && c <= '7') {
                v = v << 3 | (c - '0');
                ++lineptr;
        }
        return v;
}

static int
odigit (void)
{
        int c = peek ();

        if (c >= '0' && c <= '7') {
                ++lineptr;
                return c - '0';
        }
        return -1;
}

/*
 * Десятичное число.
 */
static int
decimal (void)
{
        int v = 0, c;

        while ((c = peek ()) >= '0' && c <= '9') {
                v = v * 10 + c - '0';
                ++lineptr;
        }
        return v;
}

/*
 * 48-битное слово из восьмеричных цифр (для a=, y=, OCT=).
 */
static void
word (void)
{
        int d;

        d = odigit ();
        if (d < 0)
                return;
        wd.l = 0;
        wd.r = d;
        while ((d = odigit ()) >= 0) {
                wd.l = wd.l << 3 | wd.r >> 21;
                wd.r = wd.r << 3 | d;
                wd.l &= 077777777;
                wd.r &= 077777777;
        }
}

void
command ()
{
        char line [128], *np;
        int c, addr;

        printf (PROMPT);
        if (!fgets(line, 128, stdin))
                exit(0);
        if ((np = strchr(line, '\n')))
                *np = 0;
        lineptr = line;

        c = lex ();
        switch (c) {
        case 0:
                return;
        case 'q':
                quitflg = 1;
                cmdflg = 0;
                return;
        case 'f':
                puts (ifile);
                return;
        case 'g':
                cmdflg = 0;
                if (peek () >= '0' && peek () <= '7') {
                        pc = ADDR (octal ());
                        right = 0;
                }
                return;
        case 's':
                cmdflg = 0;
                if (peek () == 'n') {
                        lex ();
                        stepflg = decimal ();
                } else if (peek () >= '0' && peek () <= '7') {
                        pc = ADDR (octal ());
                        right = 0;
                        stepflg = 1;
                } else
                        stepflg = 1;
                return;
        case 'w':
                where ();
                return;
        case 'h':
                help ();
                return;
        case 'b':
                breakpoint (octal () & 0177777);
                return;
        case 'W':
                bpw (ADDR (octal ()));
                return;
        case 'e': {
                int hi = odigit (), lo = odigit (), o2;

                if (hi < 0 || lo < 0)
                        return;
                o2 = hi << 3 | lo;
                if (o2 < 050)
                        printf ("Illegal ecode\n");
                else {
                        unsigned long bit = 1 << (o2 - 050);
                        ecode_intr ^= bit;
                        printf ("break on ecode %2o %s\n", o2,
                                ecode_intr & bit ? "on" : "off");
                }
                return;
        }
        case 't':
                if (peek () >= '0' && peek () <= '7')
                        trace = odigit ();
                printf ("trace level %d\n", trace);
                return;
        case 'v':
                if ((visual = !visual))
                        printf ("visual on\n");
                else
                        printf ("visual off\n");
                return;
        case 'n':
                LOAD (wd, pc);
                if ((! right && (wd.l & 03700000) == 03100000) ||
                    (right && (wd.r & 03700000) == 03100000))
                        cflags[pc+1] |= C_NEXT;
                else
                        stepflg = 1;
                cmdflg = 0;
                return;
        case 'j': {
                int i;

                for (i = 0; i < JHBSZ; ++i)
                        printf ("%05o->%05o\n",
                                (uint) jhbuf[(jhbi + i) % JHBSZ] >> 16,
                                (uint) jhbuf[(jhbi + i) % JHBSZ] & 077777);
                return;
        }
        case 'x': {          /* execute - заглушка, как в debug.y */
                int hi = odigit (), lo = odigit ();

                (void) hi; (void) lo;
                odigit (); odigit (); odigit (); odigit (); odigit ();
                return;
        }
        case 'a':
                switch (lex ()) {
                case '-':
                        printf ("%08o%08o\n", acc.l, acc.r);
                        break;
                case 'd':
                        pfloat (acc.l, acc.r);
                        break;
                case '=':
                        word ();
                        acc = wd;
                        break;
                }
                return;
        case 'y':
                if (lex () == '=')
                        word (), accex = wd;
                else
                        printf ("%08o%08o\n", accex.l, accex.r);
                return;
        case 'i': {
                int n = octal ();

                if (lex () == '=') {
                        if (n &= 017)
                                reg[n] = ADDR (octal ());
                } else
                        printf ("%05o\n", reg[n & 037]);
                return;
        }
        case 'd': {
                int from, to, i;

                from = octal ();
                lex ();               /* '-' */
                to = octal ();
                for (i = from; i <= to; ++i) {
                        if (convol[i] & CV_NUMBER) {
                                printf ("C ");
                                prword (i);
                        } else {
                                printf ("K ");
                                prcmd (i);
                        }
                }
                return;
        }
        default:
                break;
        }

        /* OCT command | OCT- ... */
        if (c >= '0' && c <= '7') {
                --lineptr;
                addr = octal ();
                switch (lex ()) {
                case '-': {
                        int i;

                        for (i = 0; i < 8; ++i)
                                prword (addr + i);
                        break;
                }
                case 't': {
                        int i, j;

                        for (j = 0; j < 8; ++j) {
                                LOAD (wd, addr);
                                for (i = 0; i < 4; ++i)
                                        gost_putc (text_to_gost[(wd.l >> (18 - i * 6)) & 63], stdout);
                                for (i = 0; i < 4; ++i)
                                        gost_putc (text_to_gost[(wd.r >> (18 - i * 6)) & 63], stdout);
                                putchar ('\n');
                                ++addr;
                        }
                        break;
                }
                case 'b': {
                        int i, j;

                        for (j = 0; j < 8; ++j) {
                                for (i = 0; i < 6; ++i)
                                        printf ("%03o ", core[addr].w_b[i]);
                                putchar ('\n');
                                ++addr;
                        }
                        break;
                }
                case 'u': {
                        int i, j;

                        for (j = 0; j < 8; ++j) {
                                for (i = 0; i < 6; ++i)
                                        gost_putc (core[addr].w_b[i], stdout);
                                putchar ('\n');
                                ++addr;
                        }
                        break;
                }
                case 'I': {
                        int i, j;

                        for (j = 0; j < 8; ++j) {
                                for (i = 0; i < 6; ++i)
                                        gost_putc (itm_to_gost[core[addr].w_b[i]], stdout);
                                putchar ('\n');
                                ++addr;
                        }
                        break;
                }
                case 'i': {
                        int i;

                        for (i = 0; i < 8; ++i) {
                                printf ("%6.6s\n", core[addr].w_b);
                                ++addr;
                        }
                        break;
                }
                case '/': {
                        int i;

                        for (i = 0; i < 8; ++i)
                                prcmd (addr + i);
                        break;
                }
                case 'd':
                        if (addr) {
                                LOAD (wd, addr);
                                pfloat (wd.l, wd.r);
                        } else
                                printf ("0.0");
                        break;
                case '=':
                        word ();
                        STORE (wd, addr);
                        convol[addr] &= ~CV_NUMBER;
                        break;
                }
        }
}

void
where ()
{
        okno (visual ? 3 : 1);
}

void
help ()
{
        printf ("Quit\n");
        printf ("Help\n");
        printf ("Where\n");
        printf ("Visual\n");
        printf ("File\n");
        printf ("Go [OCTAL]\n");
        printf ("Step [OCTAL] \n");
        printf ("Trace [OCTDIGIT]\n");
        printf ("execute COMMAND\n");
        printf ("( a | y | r | c | rtag | i OCTAL | OCTAL ) -\n");
        printf ("( a | y | r | c | rtag | i OCTAL | OCTAL ) = WORD\n");
}

void
breakpoint (int addr) {
        printf ("%s breakpoint on %05o\n", (cflags[addr] ^= C_BPT) & C_BPT ?
                        "set" : "clear", addr);
}

void
bpw (int addr) {
        printf ("%s break on data write to%05o\n",
                        (cflags[addr] ^= C_BPW) & C_BPW ?
                        "set" : "clear", addr);
}

void
okno (int trace)
{
        word_t  *wp;
        int     i;

        wp = &core[pcm_dbg];
        printf ("%c%05o%c", pcm_dbg & 0x8000 ? 'S' : ' ',
                                ADDR(pcm_dbg), right ? ' ' : ':');
        if (right)
                if (Rstruct(*wp))
                        printf (" %02o %02o %05o\t%s\n", i = Rreg(*wp),
                                Rop1(*wp) | 020, Raddr1(*wp),
                                optab[Ropcode(*wp)].o_name);
                else
                        printf (" %02o %03o %04o\t%s\n", i = Rreg(*wp),
                                Rop2(*wp) | (Rexp(*wp) ? 0100 : 0),
                                Raddr2(*wp), optab[Ropcode(*wp)].o_name);
        else
                if (Lstruct(*wp))
                        printf (" %02o %02o %05o\t%s\n", i = Lreg(*wp),
                                Lop1(*wp) | 020, Laddr1(*wp),
                                optab[Lopcode(*wp)].o_name);
                else
                        printf (" %02o %03o %04o\t%s\n", i = Lreg(*wp),
                                Lop2(*wp) | (Lexp(*wp) ? 0100 : 0),
                                Laddr2(*wp), optab[Lopcode(*wp)].o_name);
        if (trace < 2)
                return;

        printf ("  acc = %08o%08o", acc.l, acc.r);
        printf (" Y = %08o%08o", accex.l, accex.r);
        printf (" R = %01o%01o%01o%01o%01o%01o", dis_exc, G_MUL,
                G_ADD, G_LOG, dis_round, dis_norm);
        if (addrmod)
                printf (" c=%05o", reg[MODREG]);
        else
                printf ("        ");
        if (i)
                printf (" i=%05o", reg[i]);
        putchar('\n');
        if (trace < 3)
                return;

        printf ("      ");
        for (i=1; i<=8; i++)
                printf (" %05o", reg[i]);
        printf ("\n      ");
        for (i=9; i<=15; i++)
                printf (" %05o", reg[i]);
        printf ("\n");
}

static void
prcmd (int addr)
{
        LOAD(wd, addr);
        if (wd.l & 0x80000)
                printf ("%02o %02o %05o\t",
                (uint)wd.l >> 20, (uint)(wd.l >> 15) & 037,
                (uint)wd.l & 077777);
        else
                printf ("%02o %03o %04o\t",
                (uint)wd.l >> 20, (uint)(wd.l >> 12) & 0177,
                (uint)wd.l & 07777);
        if (wd.r & 0x80000)
                printf ("%02o %02o %05o\n",
                (uint)wd.r >> 20, (uint)(wd.r >> 15) & 037,
                (uint)wd.r & 077777);
        else
                printf ("%02o %03o %04o\n",
                (uint)wd.r >> 20, (uint)(wd.r >> 12) & 0177,
                (uint)wd.r & 07777);
}

static void
prword (int addr)
{
        LOAD(wd, addr);
        printf ("%04o %04o %04o %04o\n",
        (uint)wd.l >> 12, (uint)wd.l & 07777, (uint)wd.r >> 12, (uint)wd.r & 07777);
}

static void
pfloat (unsigned long l, unsigned long r)
{
        int e, manh, manl;
        double d;

        e = (l >> 17) - 64;
        manh = l & 0xffff;
        manl = r;
        d = (double) manh + (double) manl / ((long) 1 << 24);
        d /= (long) (1 << 16);
        if (l & 0x10000)
                d -= 1.0;
        while (e) {
                d *= (e > 0) ? 2.0 : 0.5;
                e = (e > 0) ? e-1 : e+1;
        }
        printf ("%25.15g\n", d);
}
