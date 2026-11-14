/*
 * BESM-6 processor control unit.
 * Main loop of CPU emulator.
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
#include <stdlib.h>
#define EXTERN                          /* to allocate common data      */
#include <string.h>
#include "defs.h"
#include "optab.h"
#include "encoding.h"

/* Отладочная трасса этапа 1 проекта ГЛИССАДА-6: дампы Э70/Э50/Э62
 * и окна МОНА-80 в stderr.  Включается переменной BESM6_DD_DEBUG. */
static int
dd_debug (void)
{
        static int v = -1;
        if (v < 0)
                v = getenv ("BESM6_DD_DEBUG") != NULL;
        return v;
}

long aumodes[] = {
        0,
        1,              /* LG */
        0x0100,         /* MG */
        0x010000,       /* AG */
};

extern ulong icnt;

void
unpack(unsigned pc)
{
        word_t         *wp = &core[pc];
        uinstr_t       *ip = uicore[pc];
        ip->i_reg = Lreg(*wp);
        ip->i_opcode = Lopcode(*wp);
        ip->i_addr = Laddr(*wp);
        ++ip;
        ip->i_reg = Rreg(*wp);
        ip->i_opcode = Ropcode(*wp);
        ip->i_addr = Raddr(*wp);
        cflags[pc] |= C_UNPACKED;
}

int
_abort(int err)
{
        lasterr = err;
        if (ninter) {
                ninter--;
                pc = abpc;
                right = abright;
/*              fprintf(stderr, "Error %d\n", err);     */
                utf8_puts(_(errtxt[err]), stderr);
                putc('\n', stderr);
                visual = 1;
                where();
                JMP(intercept);
                return E_SUCCESS;
        } else
                return(err);
}

#define CHK_STACK       \
        if (!addr && (ui.i_reg == STACKREG)) {   \
                reg[STACKREG] = ADDR(reg[STACKREG] - 1);        \
        }
#define GET_OP  {\
                LOAD(enreg, XADDR(addr + reg[ui.i_reg]));\
                if (op.o_flags & F_AR) {\
                        accex = zeroword;\
                        UNPCK(enreg);\
                        UNPCK(acc);\
                } else if (op.o_flags & F_AROP) {\
                        UNPCK(enreg);\
                }\
}
#define GET_NAI_OP      {\
        enreg.o = (addr + reg[ui.i_reg]) & 0x7f;\
        enreg.ml = enreg.mr = 0;\
        if (op.o_flags & F_AR) {\
                accex = zeroword;\
                UNPCK(acc);\
        };\
}
#define STK_PUSH        {\
        STORE(acc, reg[STACKREG] | (supmode & sup_mmap));\
        reg[STACKREG] = ADDR(reg[STACKREG] + 1);\
}
#define STK_POP         {\
        reg[STACKREG] = ADDR(reg[STACKREG] - 1);\
        LOAD(acc, reg[STACKREG] | (supmode & sup_mmap));\
}

static inline uint64_t
rdtsc(void)
{
#ifdef i386
        uint64_t rv;

        asm volatile(".byte 0x0f, 0x31" : "=A" (rv));
        return (rv);
#else
        return 0ull;
#endif
}

int covmap[32768];

ulong   run() {
        optab_t        op;
        ushort         addr;
        ushort                  cf;
        uinstr_t                ui;
        reg_t                   nextpc, pcm;
        ushort                  cnt, r, err = 0;
        int                     i;
        ulong                   icount = 0;
        uchar                   last_op = 0;
        uint64_t                tsc = rdtsc();

FOREVER

        if (goahead && !right) {
                goahead = 0;
                STORE(acc, ehandler - 11);
                enreg.o = 0;
                if (dis_exc)
                        enreg.o = 040;
                if (G_LOG)
                        enreg.o |= 004;
                if (G_MUL)
                        enreg.o |= 010;
                if (G_ADD)
                        enreg.o |= 020;
                if (dis_round)
                        enreg.o |= 002;
                if (dis_norm)
                        enreg.o |= 001;
                enreg.l = (long) enreg.o << 17;
                enreg.r = 0;
                STORE(enreg, ehandler - 10);
                STORE(accex, ehandler - 9);
                enreg.l = 0;
                enreg.r = pc;
                STORE(enreg, ehandler - 8);
                enreg.r = reg[TRAPRETREG];
                STORE(enreg, ehandler - 7);
                enreg.r = (addrmod << 1) | right;
                STORE(enreg, ehandler - 6);
                enreg.r = reg[MODREG];
                STORE(enreg, ehandler - 5);
                enreg.r = reg[017];
                STORE(enreg, ehandler - 4);
                enreg.r = reg[016];
                STORE(enreg, ehandler - 3);
                enreg.r = reg[015];
                STORE(enreg, ehandler - 2);
                enreg.r = 0;
                STORE(enreg, ehandler - 1);

                eenab = 0;
                reg[016] = pc = ehandler;
                right = 0;
                acc.l = emask;
                acc.r = events;
                events = 0;
        }
        nextpc = ADDR(pc + 1);
        pcm = pc | supmode;

        cf = cflags[pcm];
        if (!(cf & C_UNPACKED) && !right)
                unpack(pcm);

        if (!pcm || (!no_insn_check && (convol[pcm] & CV_NUMBER)))
                ABORT(E_CHECK);

        ui = uicore[pcm][right];
        op = optab[ui.i_opcode];

        if ((cf & (C_NEXT|C_BPT) && !right) | (stepflg == 1) | breakflg) {
dbg:
                pcm_dbg = pcm;
                stepflg = breakflg = quitflg = 0;
                cflags[pcm] &= ~C_NEXT;
                where();
                for (cmdflg = 1; cmdflg; command());
                if (quitflg) STOP;
        } else if (stepflg)
                 --stepflg;
        icnt = ++icount;

        if (stats) {
                ++optab[ui.i_opcode].o_count;
                tsc += optab[last_op].o_ticks += rdtsc() - tsc;
                last_op = ui.i_opcode;
        } else
                tsc = rdtsc();

        abpc = pc;
        abright = right;
        pc = ADDR(pc + right);
        right ^= 1;

        if (addrmod) {
                addrmod = 0;
                addr = ADDR(ui.i_addr + reg[MODREG]);
        } else
                addr = ui.i_addr;

        if (covflg) {
                if (abpc == 01000 && abright == 0)
                        memset(covmap, 0, sizeof(covmap));
                covmap[abpc] |= 1 << abright;
        }

        {   /* отладка этапа 1: кто вызывает Э70 и заходит ли в МОNRЕАД*
             * (печатается только при BESM6_DD_DEBUG) */
            static int montrace = 0, monseen = 0;
            int dbg = dd_debug ();
            static int core_on_pc = -1, core_done = 0, core_skip = 0;
            static int kcount_on_pc = -1, kcount_done = 0;
            /* Шаг 2б (ГЛИССАДА): окно MTR настраивается через
             * BESM6_MTR_LO/BESM6_MTR_HI (восьмеричные) и длина через
             * BESM6_MTR_N (десятичная); по умолчанию 10260-12500,
             * 30000 команд (комплектация GLIS1R). */
            static int mtr_lo = -1, mtr_hi = -1, mtr_n = 0, mtr_skip = 0;
            if (mtr_lo < 0) {
                const char *s = getenv ("BESM6_MTR_LO");
                const char *h = getenv ("BESM6_MTR_HI");
                const char *n = getenv ("BESM6_MTR_N");
                const char *k = getenv ("BESM6_MTR_SKIP");
                mtr_lo = s ? (int) strtol (s, NULL, 8) : 010260;
                mtr_hi = h ? (int) strtol (h, NULL, 8) : 012500;
                mtr_n  = n ? atoi (n) : 30000;
                mtr_skip = k ? atoi (k) : 0;
            }
            if (core_on_pc < 0) {
                const char *c = getenv ("BESM6_CORE_ON_PC");
                const char *cs = getenv ("BESM6_CORE_SKIP");
                core_on_pc = c ? (int) strtol (c, NULL, 8) : 0;
                core_skip = cs ? atoi (cs) : 0;
            }
            if (kcount_on_pc < 0) {
                const char *c = getenv ("BESM6_KCOUNT_ON_PC");
                kcount_on_pc = c ? (int) strtol (c, NULL, 8) : 0;
                fprintf (stderr, "KCNT: on_pc=%05o env=%s\n",
                         kcount_on_pc, c ? c : "(none)");
            }
            if (abpc >= mtr_lo && abpc <= mtr_hi) {
                /* BESM6_MTR_SKIP: пропустить первые K срабатываний окна
                 * (реверс повторно входных процедур - ГЛИССАДА 13.11). */
                if (! monseen) {
                    monseen = 1;
                    if (mtr_skip > 0) --mtr_skip;
                    else montrace = mtr_n;
                } else if (mtr_skip > 0) {
                    --mtr_skip;
                } else if (montrace <= 0) {
                    montrace = mtr_n;   /* ре-арминг окна */
                }
            }
            if (dbg && ui.i_opcode == 070)   /* Э70 */
                fprintf(stderr, "E70at %05o%c r016=%05o uil=%o/%o uir=%o/%o\n",
                        abpc, abright ? 'R' : 'L', reg[016],
                        uicore[abpc][0].i_opcode, uicore[abpc][0].i_addr,
                        uicore[abpc][1].i_opcode, uicore[abpc][1].i_addr);
            if (dbg && ui.i_opcode == 050)   /* Э50 */
                fprintf(stderr, "E50at %05o%c type=%05o acc=%08o%08o\n",
                        abpc, abright ? 'R' : 'L', ui.i_addr,
                        (uint)acc.l, (uint)acc.r);
            if (dbg && ui.i_opcode == 062)   /* Э62 */
                fprintf(stderr, "E62at %05o%c r016=%05o acc=%08o%08o\n",
                        abpc, abright ? 'R' : 'L', reg[016],
                        (uint)acc.l, (uint)acc.r);
            /* Шаг 2е (ГЛИССАДА): дамп памяти задачи при попадании pc
             * на адрес BESM6_CORE_ON_PC (восьмеричный). */
            if (dbg && core_on_pc > 0 && abpc == core_on_pc && !core_done) {
                /* BESM6_CORE_SKIP: пропустить первые K попаданий
                 * (тиражируемые члены грузятся многократно - 13.11). */
                if (core_skip > 0) { --core_skip; }
                else {
                FILE *cf = fopen ("core-pc.bin", "wb");
                int q;
                core_done = 1;
                if (cf) {
                    for (q = 0; q < 65536; ++q) {   /* ГЛИССАДА: полное ядро (супервизор 0100000+) */
                        unsigned long long v = 0;
                        int b;
                        for (b = 0; b < 6; ++b)
                            v = (v << 8) | core[q].w_b[b];
                        fputc ((v >> 40) & 0xFF, cf);
                        fputc ((v >> 32) & 0xFF, cf);
                        fputc ((v >> 24) & 0xFF, cf);
                        fputc ((v >> 16) & 0xFF, cf);
                        fputc ((v >> 8) & 0xFF, cf);
                        fputc (v & 0xFF, cf);
                    }
                    fclose (cf);
                    /* ГЛИССАДА 13.11: дамп кэша команд (uicore) - члены
                     * выгружаются из core, но кэш живёт до перезагрузки */
                    cf = fopen ("core-uic.bin", "wb");
                    if (cf) {
                        for (q = 0; q < 65536; ++q) {
                            unsigned int c0 = ((uicore[q][0].i_reg & 017) << 20) |
                                              (uicore[q][0].i_opcode << 12) |
                                              (uicore[q][0].i_addr & 07777);
                            unsigned int c1 = ((uicore[q][1].i_reg & 017) << 20) |
                                              (uicore[q][1].i_opcode << 12) |
                                              (uicore[q][1].i_addr & 07777);
                            fputc ((c0 >> 16) & 0xFF, cf);
                            fputc ((c0 >> 8) & 0xFF, cf);
                            fputc (c0 & 0xFF, cf);
                            fputc ((c1 >> 16) & 0xFF, cf);
                            fputc ((c1 >> 8) & 0xFF, cf);
                            fputc (c1 & 0xFF, cf);
                        }
                        fclose (cf);
                    }
                    fprintf (stderr, "CORE DUMPED at pc=%05o -> core-pc.bin\n",
                             abpc);
                    /* А2 14.11-18 п.18.19: конволюция буферов 37000/76000
                     * в момент дампа (классификатор листа читает
                     * конволюционные биты слов записей). */
                    {
                        int qa;
                        for (qa = 0; qa < 52; ++qa)
                            fprintf (stderr, "CV [%04o] 37000=%02o 76000=%02o\n",
                                     qa, convol [37000 + qa],
                                     convol [76000 + qa]);
                    }
                    /* ГЛИССАДА 13.11: контрольные ячейки конвейера ввода */
                    {
                        int qa;
                        static const int wcells[] = {0600, 0603, 0607, 0624,
                                                     0417, 0577, 0600, 0601,
                                                     02034, 02035, 02401};
                        for (qa = 0; qa < 11; ++qa) {
                            int c2 = wcells[qa];
                            unsigned long long v2 = 0;
                            int b2;
                            for (b2 = 0; b2 < 6; ++b2)
                                v2 = (v2 << 8) | core [c2].w_b [b2];
                            fprintf (stderr, "CWCHK [%05o] = %014llo\n",
                                     c2, v2);
                        }
                    }
                    }
                }
            }
            /* А2 13.11-18 (ГЛИССАДА, п.18.18(1)): восстановление
             * характеристики строки MONCARD [0417] (A+L; РЛС-ротация
             * пишет веса 0414-0416, стадию 0417 не выполняет - патч
             * Шага 14.8-3) на КАЖДОМ входе конвейера READ/WRITE
             * (pc = BESM6_KCOUNT_ON_PC, восьмеричный). В g1r-прогоне
             * [0417] обнуляется ротацией на каждой записи, единичный
             * реарм при redirect u=45 теряется -> 201-петля на картах
             * данных (g1r-native3/4/6, echo "RЕСОRD⊃ 5 1").
             * ВНИМАНИЕ: КCOUNT*[0600] НЕ трогать - это естественный
             * указатель лексем-потока (команды -> данные); запись в
             * него на 01000 ломает командный разбор (g1r-native7:
             * "ОТСУТСТВУЕТ П/К *NAME"). */
            if (dbg && kcount_on_pc > 0 && abpc == kcount_on_pc) {
                const char *ex = getenv ("BESM6_LUN45_EXECUTE");
                if (ex && getenv ("BESM6_LUN45_NATIVE")) {
                    ushort ev = (ushort) strtol (ex, NULL, 8);
                    core [0417].w_b [4] = 0;
                    core [0417].w_b [5] = ev & 0xFF;
                    if (! kcount_done) {
                        kcount_done = 1;
                        fprintf (stderr, "EXECUTE [0417] := %05o at pc %05o (each entry)\n",
                                 ev, abpc);
                    }
                }
            }
            if (dbg && montrace > 0) {
                unsigned long long mv = ((unsigned long long)acc.l << 24) | acc.r;
                fprintf(stderr, "MTR %05o%c %-6s %o(%o) AU=%013llo",
                        abpc, abright ? 'R' : 'L', op.o_name,
                        addr, ui.i_reg, mv);
                /* Шаг 4в (ГЛИССАДА): печать всех М-регистров для реверса */
                fprintf(stderr, " M:");
                for (int rr = 1; rr <= 14; ++rr)
                    fprintf(stderr, " %02o=%05o", rr, reg[rr] & 077777);
                fprintf(stderr, " R16=%05o R17=%05o\n",
                        reg[016] & 077777, reg[017] & 077777);
                --montrace;
            }
            if (dbg && abpc == 010354 && abright) {
                /* отладка А1: ключ скана рекорда */
                fprintf(stderr, "SCAN pc=%05oR M17=%05o key[М17-1]=%016llo "
                        "scanned[М17]=%016llo\n",
                        abpc, reg[017],
                        (((unsigned long long)core[(reg[017] - 1) & 0x7fff].w_b[0] << 40) |
                         ((unsigned long long)core[(reg[017] - 1) & 0x7fff].w_b[1] << 32) |
                         ((unsigned long long)core[(reg[017] - 1) & 0x7fff].w_b[2] << 24) |
                         ((unsigned long long)core[(reg[017] - 1) & 0x7fff].w_b[3] << 16) |
                         ((unsigned long long)core[(reg[017] - 1) & 0x7fff].w_b[4] << 8) |
                         (unsigned long long)core[(reg[017] - 1) & 0x7fff].w_b[5]),
                        (((unsigned long long)core[reg[017]].w_b[0] << 40) |
                         ((unsigned long long)core[reg[017]].w_b[1] << 32) |
                         ((unsigned long long)core[reg[017]].w_b[2] << 24) |
                         ((unsigned long long)core[reg[017]].w_b[3] << 16) |
                         ((unsigned long long)core[reg[017]].w_b[4] << 8) |
                         (unsigned long long)core[reg[017]].w_b[5]));
            }
        }

        if (trace >= 2) {
                char str [40] = "";
                LOAD(enreg, XADDR(addr + reg[ui.i_reg]));
                fprintf(stderr, "%05o: %-4s", abpc, op.o_name);
                if (addr >= 077700) {
                        if (ui.i_reg)
                                sprintf (str, "-%o(%o)", (addr ^ 077777) + 1, ui.i_reg);
                        else
                                sprintf (str, "-%o", (addr ^ 077777) + 1);
                } else if (addr) {
                        if (ui.i_reg)
                                sprintf (str, "%o(%o)", addr, ui.i_reg);
                        else
                                sprintf (str, "%o", addr);
                } else {
                        if (ui.i_reg) {
                                sprintf (str, "(%o)", ui.i_reg);
                        }
                }
                fprintf(stderr, "%-16s (=%08o%08o) acc=%08o%08o",
                        str, (uint)enreg.l, (uint)enreg.r,
                        (uint)acc.l, (uint)acc.r);
                if (ui.i_reg)
                        fprintf(stderr, " r[%o]=%05o",
                                ui.i_reg, reg[ui.i_reg]);
                fprintf(stderr, "\n");
                fflush(stderr);
        }

        /* ГЛИССАДА 13.11-2: точка наблюдения BESM6_WATCH (восьм.) -
         * печать обращений (чтение/запись) к ячейке по исполнительному
         * адресу. BESM6_WATCH_RW: 0=все (умолч.), 1=только запись,
         * 2=только чтение. Самомодифицируемый код: адрес вычисляется
         * по факту (XADDR), а не по полю команды. */
        {
            static int watch_addr = -2, watch_rw = 0, watch_done = 0;
            if (! watch_done) {
                const char *w = getenv ("BESM6_WATCH");
                const char *r = getenv ("BESM6_WATCH_RW");
                watch_addr = w ? (int) strtol (w, NULL, 8) : -2;
                watch_rw = r ? atoi (r) : 0;
                watch_done = 1;
            }
            if (watch_addr >= 0) {
                static const uchar wops[] = { 000, 001, 004, 005, 006,
                                              016, 017, 020, 021, 022, 023 };
                int isw = 0;
                unsigned qi;
                for (qi = 0; qi < sizeof (wops); ++qi)
                    if (ui.i_opcode == wops[qi]) { isw = 1; break; }
                if (ui.i_opcode == 002) isw = 0;    /* MOD не пишет память */
                {
                    unsigned ea = XADDR(addr + reg[ui.i_reg]) & 077777;
                    if (ea == (unsigned) watch_addr &&
                        ((watch_rw == 0) ||
                         (watch_rw == 1 && isw) ||
                         (watch_rw == 2 && ! isw))) {
                        unsigned long long mv =
                                ((unsigned long long) acc.l << 24) | acc.r;
                        fprintf (stderr,
                                 "WATCH %05o%c %-6s %o(%o) EA=%05o %s "
                                 "AU=%014llo M:",
                                 abpc, abright ? 'R' : 'L', op.o_name,
                                 addr, ui.i_reg, ea, isw ? "WR" : "RD", mv);
                        for (qi = 1; qi <= 14; ++qi)
                            fprintf (stderr, " %02o=%05o", qi, reg[qi] & 077777);
                        fprintf (stderr, " R16=%05o R17=%05o\n",
                                 reg[016] & 077777, reg[017] & 077777);
                    }
                }
            }
        }

        /* ГЛИССАДА 13.11-3: лог операнда аох 0(17) - слова, которые
         * сканер РЛС (77176/77143) OR-ит в накопитель [3].
         * BESM6_AOX17_LOG=1 включает печать pc, EA, операнда. */
        {
            static int aox17_log = -1;
            if (aox17_log < 0)
                aox17_log = getenv ("BESM6_AOX17_LOG") ? 1 : 0;
            if (aox17_log && ui.i_opcode == 015 && ui.i_reg == 017) {
                unsigned qa = XADDR (addr + reg[017]) & 077777;
                unsigned long long qv = 0;
                int qb;
                for (qb = 0; qb < 6; ++qb)
                    qv = (qv << 8) | core[qa].w_b[qb];
                fprintf (stderr,
                         "AOX17 %05o%c EA=%05o [%05o]=%014llo "
                         "acc=%08o%08o M5=%05o M12=%05o M15=%05o\n",
                         abpc, abright ? 'R' : 'L', qa, qa, qv,
                         (uint) acc.l, (uint) acc.r,
                         reg[5] & 077777, reg[12] & 077777,
                         reg[15] & 077777);
            }
        }

        switch (op.o_inline) {
        case I_ATX:
                STORE(acc, XADDR(addr + reg[ui.i_reg]));
                if (trace >= 2)
                        fprintf(stderr, "       %05o: store %08o%08o\n",
                                XADDR(addr + reg[ui.i_reg]), acc.l, acc.r);
                if (!addr && (ui.i_reg == STACKREG))
                        reg[STACKREG] = ADDR(reg[STACKREG] + 1);
                NEXT;
        case I_STX:
                STORE(acc, XADDR(addr + reg[ui.i_reg]));
                if (trace >= 2)
                        fprintf(stderr, "       %05o: store %08o%08o\n",
                                XADDR(addr + reg[ui.i_reg]), acc.l, acc.r);
                STK_POP;
                break;
        case I_XTS: /* Major ISA change: swapping next 2 lines */
                STK_PUSH;
                GET_OP;
                acc = enreg;
                break;
        case I_XTA:
                CHK_STACK;
                GET_OP;
                acc = enreg;
                break;
        case I_VTM:
                reg[ui.i_reg] = addr;
                reg[0] = 0;
                if (supmode && ui.i_reg == 0) {
                        reg[PSREG] &= ~02003;
                        reg[PSREG] |= addr & 02003;
                        sup_mmap = (addr & 1) << 15;
                }
                NEXT;
        case I_UTM:
                reg[ui.i_reg] = ADDR(addr + reg[ui.i_reg]);
                reg[0] = 0;
                if (supmode && ui.i_reg == 0) {
                        reg[PSREG] &= ~02003;
                        reg[PSREG] |= addr & 02003;
                        sup_mmap = (addr & 1) << 15;
                }
                NEXT;
        case I_VLM:
                if (!reg[ui.i_reg])
                        break;
                reg[ui.i_reg] = ADDR(reg[ui.i_reg] + 1);
                JMP(addr);
                NEXT;
        case I_UJ:
                JMP(ADDR(addr + reg[ui.i_reg]));
                NEXT;
        case I_STOP:
                if (!(cf & C_STOPPED)) {
                        cf |= C_STOPPED;
                        pc = abpc;
                        pcm = pc | supmode;
                        right = abright;
                        goto dbg;
                }
                NEXT;
        case I_ITS:
                STK_PUSH;
                /*      fall    thru    */
        case I_ITA:
                acc.l = 0;
                acc.r = reg[(addr + reg[ui.i_reg]) & (supmode ? 0x1f : 0xf)];
                break;
        case I_XTR:
                CHK_STACK;
                GET_OP;
set_mode:
                dis_exc = (enreg.o & 040) != 0;
                G_ADD = (enreg.o & 020) != 0;
                G_MUL = (enreg.o & 010) != 0;
                G_LOG = (enreg.o & 004) != 0;
                dis_round = (enreg.o & 002) != 0;
                dis_norm = (enreg.o & 001) != 0;
                NEXT;
        case I_NTR:
                GET_NAI_OP;
                goto set_mode;
        case I_RTE:
                GET_NAI_OP;
                acc.o = 0;
                if (dis_exc)
                        acc.o = 040;
                if (G_LOG)
                        acc.o |= 004;
                if (G_MUL)
                        acc.o |= 010;
                if (G_ADD)
                        acc.o |= 020;
                if (dis_round)
                        acc.o |= 002;
                if (dis_norm)
                        acc.o |= 001;
                acc.l = (long) (acc.o & enreg.o) << 17;
                acc.r = 0;
                break;
        case I_ASUB:
                CHK_STACK;
                GET_OP;
                if (NEGATIVE(acc))
                        NEGATE(&acc);
                if (!NEGATIVE(enreg))
                        NEGATE(&enreg);
                err = add();
                if (err)
                        ABORT(err);
                break;
        case I_RSUB:
                CHK_STACK;
                GET_OP;
                NEGATE(&acc);
                err = add();
                if (err)
                        ABORT(err);
                break;
        case I_SUB:
                CHK_STACK;
                GET_OP;
                NEGATE(&enreg);
                err = add();
                if (err)
                        ABORT(err);
                break;
        case I_ADD:
                CHK_STACK;
                GET_OP;
                err = add();
                if (err)
                        ABORT(err);
                break;
        case I_YTA:
                if (G_LOG) {
                        acc = accex;
                        break;
                }
                UNPCK(accex);
                UNPCK(acc);
                acc.mr = accex.mr;
                acc.ml = accex.ml & 0xffff;
                acc.o += ((addr + reg[ui.i_reg]) & 0x7f) - 64;
                op.o_flags |= F_AR;
                enreg = accex;
                accex = zeroword;
                PACK(enreg);
                break;
        case I_UZA:
                accex = acc;
                if (G_ADD) {
                        if (acc.l & 0x10000)
                                        NEXT;
                } else if (G_MUL) {
                        if (!(acc.l & 0x800000))
                                        NEXT;
                } else if (G_LOG) {
                        if (acc.l | acc.r)
                                        NEXT;
                } else
                        NEXT;
                JMP(ADDR(addr + reg[ui.i_reg]));
                NEXT;
        case I_UIA:
                accex = acc;
                if (G_ADD) {
                        if (!(acc.l & 0x10000))
                                        NEXT;
                } else if (G_MUL) {
                        if (acc.l & 0x800000)
                                        NEXT;
                } else if (G_LOG) {
                        if (!(acc.l | acc.r))
                                        NEXT;
                } else
                        /* fall thru, i.e. branch */;
                JMP(ADDR(addr + reg[ui.i_reg]));
                NEXT;
        case I_UTC:
                reg[MODREG] = ADDR(addr + reg[ui.i_reg]);
                addrmod = 1;
                NEXT;
        case I_WTC:
                CHK_STACK;
                GET_OP;
                reg[MODREG] = ADDR(enreg.r);
                addrmod = 1;
                NEXT;
        case I_VZM:
                if (ui.i_opcode == 0115) {
                        if (reg[ui.i_reg]) {
                                JMP(addr);
                        }
                } else {
                        if (!reg[ui.i_reg]) {
                                JMP(addr);
                        }
                }
                NEXT;
        case I_VJM:
                reg[ui.i_reg] = nextpc;
                reg[0] = 0;
                JMP(addr);
                NEXT;
        case I_ATI:
                if (supmode) {
                        reg[i = (addr + reg[ui.i_reg]) & 0x1f] = ADDR(acc.r);
                        if (i == PSREG)
                                sup_mmap = (reg[PSREG] & 1) << 15;
                } else
                        reg[(addr + reg[ui.i_reg]) & 0xf] = ADDR(acc.r);
                reg[0] = 0;
                NEXT;
        case I_STI: {
                uchar   rg = (addr + reg[ui.i_reg]) & (supmode ? 0x1f : 0xf);
                ushort  ad = ADDR(acc.r);

                reg[rg] = ad;
                reg[0] = 0;
                if (rg != STACKREG)
                        reg[STACKREG] = ADDR(reg[STACKREG] - 1);
                LOAD(acc, reg[STACKREG] | (supmode & sup_mmap));
                if (rg == PSREG)
                        sup_mmap = (reg[PSREG] & 1) << 15;
                break;
        }
        case I_MTJ:
                if (supmode) {
mtj:
                        reg[addr & 0x1f] = reg[ui.i_reg];
                        if ((addr & 0x1f) == PSREG)
                                sup_mmap = (reg[PSREG] & 1) << 15;
                } else
                        reg[addr & 0xf] = reg[ui.i_reg];
                reg[0] = 0;
                NEXT;
        case I_MPJ:
                i = addr & 0xf;
                if (i & 020 && supmode)
                        goto mtj;
                reg[i] = ADDR(reg[i] + reg[ui.i_reg]);
                reg[0] = 0;
                NEXT;
        case I_MOD:
                if (supmode)
                        NEXT;
                else
                        ABORT(E_PRIV);
        case I_IRET:
                if (!supmode)
                        ABORT(E_PRIV);
                if (ui.i_reg == 016) {
                        err = emu_call();
                        if (err) {
                                ninter = 0;     /* can't be intercepted */
                                ABORT(err);
                        }
                        NEXT;
                }
                reg[PSREG] = reg[PSSREG] & 02003;
                JMP(reg[(ui.i_reg & 3) | 030]);
                right = !!(reg[PSSREG] & 0400);
                sup_mmap = reg[PSSREG] & 1 ? 0100000 : 0;
                supmode = reg[PSSREG] & 014 ? 0100000 : 0;
                abpc = pc;
                abright = right;
                pcm = pc | supmode;
                spec = spec_saved | supmode;
                err = reg[TRAPNREG];
                reg[TRAPNREG] = 0;
                if (err)
                        ABORT(err);
                NEXT;
        case I_TRAP:
                reg[TRAPRETREG] = nextpc;
                if (!(cf & C_STOPPED) &&
                                ((1 << (ui.i_opcode - 050)) & ecode_intr)) {
                        cf |= C_STOPPED;
                        pc = abpc;
                        right = abright;
                        goto dbg;
                }
                JMP(nextpc);
                reg[016] = ADDR(addr + reg[ui.i_reg]);
                reg[TRAPNREG] = ui.i_opcode - 050;
                stopwatch();
                if (trace == 1 && (ui.i_opcode != 075 || reg[016] < 2)) {
                        /* Do not trace e75, it's too verbose. */
                        LOAD(enreg, reg[016] | (supmode & sup_mmap));
                        fprintf(stderr, "%05o: %-4s%-5o (=%08o%08o) acc=%08o%08o\n",
                                abpc, op.o_name, reg[016], (uint)enreg.l, (uint)enreg.r,
                                (uint)acc.l, (uint)acc.r);
                        fflush(stderr);
                }
                switch (ui.i_opcode) {
                case 047:
                        break;
                case 050:
                        err = e50();
                        goto errchk;
                case 051:
                        err = e51();
                        goto errchk;
                case 052:
                        err = elfun(EF_COS);
                        goto errchk;
                case 053:
                        err = e53();
                        goto errchk;
                case 054:
                        err = elfun(EF_ARCSIN);
                        goto errchk;
                case 055:
                        err = elfun (EF_ALOG);
                        goto errchk;
                case 056:
                        err = elfun (EF_EXP);
                        goto errchk;
                case 057:
                        err = elfun(EF_ENTIER);
                        goto errchk;
                case 060:
                        err = e60();
                        goto errchk;
                case 061:
                        err = e61();
                        goto errchk;
                case 062:
                        err = e62();
                        if (err == E_TERM)
                                STOP;
                        goto errchk;
                case 063:
                        err = e63();
                        goto errchk;
                case 064:
                        err = print();
                        goto errchk;
                case 065:
                        err = physaddr();
                        goto errchk;
                case 067:
                        err = deb();
                        goto errchk;
                case 070:
                        if (reg[016] == 0) {
                                /* control word in ACC (Dubna) */
                                alureg_t r;

                                LOAD(r, 1);
                                STORE(acc, 1);
                                reg[016] = 1;
                                err = ddio();
                                STORE(r, 1);
                        } else
                                err = ddio();
                        goto errchk;
                case 071:
                        err = term();
                        if (err == E_TERM)
                                STOP;
                        goto errchk;
                case 072:
                        err = resources();
                        goto errchk;
                case 074:
                        if ((acc.l == 0x737973) &&      /* "syscal" */
                                        (acc.r = 0x63616c)) {
                                err = usyscall();
                                goto errchk;
                        }
                        if ((accex.l == 0x737973) &&      /* "syscal" */
                                        (accex.r = 0x63616c)) {
                                accex = zeroword;
                                err = emu_call();
                                goto errchk;
                        }
                        err = eexit();
                        if (err == E_TERM)
                                STOP;
                        goto errchk;
                case 075:
                        STORE(acc, reg[016]);
                        convol[reg[016]] &= ~CV_NUMBER;
                        switch (reg[016]) {
                        /* undocumented trick (ВРЕМЕННО ФИРСОВ) */
                        case 0: spec = 1; break;
                        case 1: spec = pspspec; break;
                        }
                        break;
                case 077:
                        /* Закрытие записи на МЛ: на реальной машине записывает
                         * признак конца информации на ленту.  Образ ленты в
                         * эмуляторе пишется напрямую при Э70, поэтому здесь
                         * достаточно успешного ответа.  Используется МАРС-6
                         * после операций записи (besm(770000B) в Паскале). */
                        acc.l = acc.r = 0;
                        break;
                default:
                        err = E_UNIMP;
errchk:
                        if (err == E_UNIMP) {
                                /* try the supervisor then */
                                spec_saved = spec;
                                reg[PSSREG] = reg[PSREG] & 02003;
                                if (supmode)
                                        reg[PSSREG] |= 014;
                                supmode = 0100000;
                                sup_mmap = 0100000;
                                reg[PSREG] = 02007;
                                /* words formed by the kernel in user memory can be instructions */
                                spec = 1;
                                JMP(XCODE_ENTRYPT);
                        } else if (err) {
                                startwatch();
                                ABORT(err);
                        }
                        break;
                }
                startwatch();
                NEXT;
        default:
                if (!addr && (ui.i_reg == STACKREG) && (op.o_flags & F_STACK))
                        reg[STACKREG] = ADDR(reg[STACKREG] - 1);

                if (op.o_flags & F_OP) {
                        GET_OP;
                } else if (op.o_flags & F_NAI)
                        GET_NAI_OP;

                err = (*op.o_impl)();
                if (err)
                        ABORT(err);
                break;
        }

        if ((i = op.o_flags & F_GRP))
                augroup.gl_au = aumodes[i];

        if (op.o_flags & F_AR) {
                uint    rr = 0;
                switch ((acc.ml >> 16) & 3) {
                case 2:
                case 1:
                        rnd_rq |= acc.mr & 1;
                        accex.mr = (accex.mr >> 1) | (accex.ml << 23);
                        accex.ml = (accex.ml >> 1) | (acc.mr << 15);
                        acc.mr = (acc.mr >> 1) | (acc.ml << 23);
                        acc.ml >>= 1;
                        ++acc.o;
                        goto chk_rnd;
                }

                if (dis_norm)
                        goto chk_rnd;
                if (!(i = (acc.ml >> 15) & 3)) {
                        if ((r = acc.ml & 0xffff)) {
                                for (cnt = 0; (r & 0x8000) == 0;
                                                        ++cnt, r <<= 1);
                                acc.ml = (r & 0xffff) |
                                                (acc.mr >> (24 - cnt));
                                acc.mr = (acc.mr << cnt) |
                                                (rr = accex.ml >> (16 - cnt));
                                accex.ml = (accex.ml << cnt) |
                                                (accex.mr >> (24 - cnt));
                                accex.mr <<= cnt;
                                acc.o -= cnt;
                                goto chk_zero;
                        }
                        if ((r = acc.mr >> 16)) {
                                int     fcnt;
                                for (cnt = 0; (r & 0x80) == 0;
                                                        ++cnt, r <<= 1);
                                acc.ml = acc.mr >> (8 - cnt);
                                acc.mr = (acc.mr << (fcnt = 16 + cnt)) |
                                                (accex.ml << cnt) |
                                                (accex.mr >> (24 - cnt));
                                accex.ml = accex.mr >> (8-cnt);
                                accex.mr <<= fcnt;
                                acc.o -= fcnt;
                                rr = acc.r & ((1l << fcnt) - 1);
                                goto chk_zero;
                        }
                        if ((r = acc.mr & 0xffff)) {
                                for (cnt = 0; (r & 0x8000) == 0;
                                                        ++cnt, r <<= 1);
                                acc.ml = (r & 0xffff) |
                                                (accex.ml >> (16 - cnt));
                                acc.mr = (accex.ml << (8 + cnt)) |
                                                (accex.mr >> (16 - cnt));
                                accex.ml = accex.mr << cnt;
                                accex.mr = 0;
                                acc.o -= 24 + cnt;
                                rr = (acc.ml & ((1 << cnt) - 1)) | acc.mr;
                                goto chk_zero;
                        }
                        if ((r = accex.ml & 0xffff)) {
                                rr = accex.ml | accex.mr;
                                for (cnt = 0; (r & 0x8000) == 0;
                                                        ++cnt, r <<= 1);
                                acc.ml = (r & 0xffff) |
                                                (accex.mr >> (24 - cnt));
                                acc.mr = (accex.mr << cnt);
                                accex.ml = accex.mr = 0;
                                acc.o -= 40 + cnt;
                                goto chk_zero;
                        }
                        if ((r = accex.mr >> 16)) {
                                rr = accex.ml | accex.mr;
                                for (cnt = 0; (r & 0x80) == 0;
                                                        ++cnt, r <<= 1);
                                acc.ml = accex.mr >> (8 - cnt);
                                acc.mr = accex.mr << (16 + cnt);
                                accex.ml = accex.mr = 0;
                                acc.o -= 56 + cnt;
                                goto chk_zero;
                        }
                        if ((r = accex.mr & 0xffff)) {
                                rr = accex.ml | accex.mr;
                                for (cnt = 0; (r & 0x8000) == 0;
                                                        ++cnt, r <<= 1);
                                acc.ml = (r & 0xffff);
                                acc.mr = accex.ml = accex.mr = 0;
                                acc.o -= 64 + cnt;
                                goto chk_zero;
                        }
                        goto zero;
                } else if (i == 3) {
                        if ((r = ~acc.ml & 0xffff)) {
                                for (cnt = 0; (r & 0x8000) == 0;
                                                        ++cnt, r = (r << 1) | 1);
                                acc.ml = 0x10000 | (~r & 0xffff) |
                                                (acc.mr >> (24 - cnt));
                                acc.mr = (acc.mr << cnt) |
                                                (rr = accex.ml >> (16 - cnt));
                                accex.ml = ((accex.ml << cnt) |
                                                (accex.mr >> (24 - cnt)))
                                                & 0xffff;
                                accex.mr <<= cnt;
                                acc.o -= cnt;
                                goto chk_zero;
                        }
                        if ((r = (~acc.mr >> 16) & 0xff)) {
                                int     fcnt;
                                for (cnt = 0; (r & 0x80) == 0;
                                                        ++cnt, r = (r << 1) | 1);
                                acc.ml = 0x10000 | (acc.mr >> (8 - cnt));
                                acc.mr = (acc.mr << (fcnt = 16 + cnt)) |
                                                (accex.ml << cnt) |
                                                (accex.mr >> (24 - cnt));
                                accex.ml = ((accex.ml << fcnt) |
                                                (accex.mr >> (8 - cnt)))
                                                & 0xffff;
                                accex.mr <<= fcnt;
                                acc.o -= fcnt;
                                rr = acc.r & ((1l << fcnt) - 1);
                                goto chk_zero;
                        }
                        if ((r = ~acc.mr & 0xffff)) {
                                for (cnt = 0; (r & 0x8000) == 0;
                                                        ++cnt, r = (r << 1) | 1);
                                acc.ml = 0x10000 | (~r & 0xffff) |
                                                (accex.ml >> (16 - cnt));
                                acc.mr = (accex.ml << (8 + cnt)) |
                                                (accex.mr >> (16 - cnt));
                                accex.ml = (accex.mr << cnt) & 0xffff;
                                accex.mr = 0;
                                acc.o -= 24 + cnt;
                                rr = (acc.ml & ((1 << cnt) - 1)) | acc.mr;
                                goto chk_zero;
                        }
                        if ((r = ~accex.ml & 0xffff)) {
                                rr = accex.ml | accex.mr;
                                for (cnt = 0; (r & 0x8000) == 0;
                                                        ++cnt, r = (r << 1) | 1);
                                acc.ml = 0x10000 | (~r & 0xffff) |
                                                (accex.mr >> (24 - cnt));
                                acc.mr = (accex.mr << cnt);
                                accex.ml = accex.mr = 0;
                                acc.o -= 40 + cnt;
                                goto chk_zero;
                        }
                        if ((r = (~accex.mr >> 16) & 0xff)) {
                                rr = accex.ml | accex.mr;
                                for (cnt = 0; (r & 0x80) == 0;
                                                        ++cnt, r = (r << 1) | 1);
                                acc.ml = 0x10000 | (accex.mr >> (8 - cnt));
                                acc.mr = accex.mr << (16 + cnt);
                                accex.ml = accex.mr = 0;
                                acc.o -= 56 + cnt;
                                goto chk_zero;
                        }
                        if ((r = ~accex.mr & 0xffff)) {
                                rr = accex.ml | accex.mr;
                                for (cnt = 0; (r & 0x8000) == 0;
                                                        ++cnt, r = (r << 1) | 1);
                                acc.ml = 0x10000 | (~r & 0xffff);
                                acc.mr = accex.ml = accex.mr = 0;
                                acc.o -= 64 + cnt;
                                goto chk_zero;
                        } else {
                                rr = 1;
                                acc.ml = 0x10000;
                                acc.mr = accex.ml = accex.mr = 0;
                                acc.o -= 80;
                                goto chk_zero;
                        }
                }
chk_zero:
                rnd_rq = rnd_rq && !rr;
chk_rnd:
                if (acc.o & 0x8000)
                        goto zero;
                if (acc.o & 0x80) {
                        acc.o = 0;
                        if (!dis_exc)
                                ABORT(E_OVFL);
                }
                if (!dis_round && rnd_rq)
                        acc.mr |= 1;

                if (!acc.ml && !acc.mr && !dis_norm) {
zero:
                        acc.l = acc.r = accex.l = accex.r = 0;
                        goto done;
                }
                acc.l = ((uint) (acc.o & 0x7f) << 17) | (acc.ml & 0x1ffff);
                acc.r = acc.mr & 0xffffff;

                accex.l = ((uint) (accex.o & 0x7f) << 17) | (accex.ml & 0x1ffff);
                accex.r = accex.mr & 0xffffff;
done:
                if (op.o_inline == I_YTA)
                        accex = enreg;
                rnd_rq = 0;
        }

ENDFOREVER
        if (pout_enable && xnative)
                pout_decode(pout_file);
        if (covflg) {
                for (i = 01000; i < 32768; ++i) {
                        if (covmap[i])
                                printf("%05o: %c%c\n", i,
                                    covmap[i] & 1 ? 'L' : ' ',
                                    covmap[i] & 2 ? 'R' : ' ');
                        else
                                printf("%05o: --\n", i);
                }
        }
        pc = abpc;
        right = abright;
        pcm_dbg = pcm;
        if (pout_enable || err != 1) {
/*              printf("Error %d\n", err);     */
                utf8_puts(_(errtxt[err]), stdout);
                putc('\n', stdout);
                where();
        }
        return icount;

}

int
priv() {
        return E_PRIV;
}
