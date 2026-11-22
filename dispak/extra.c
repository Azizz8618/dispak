/*
 * Implementation of BESM-6 extracodes.
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
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include "defs.h"
#include "disk.h"
#include "iobuf.h"
#include "gost10859.h"
#include "encoding.h"
#include "plotter_bridge.h"

#define SMALL_DISKS()   (getenv("BESM6_SMALL_DISKS") || disk_emulate_725)

static int du_plot_keep(void);  /* used by the atexit report below */
static int du_benson_mode(void); /* [аналог NASA, БЕНСОН-501] */
static void du_benson_close(void);

/* Отладочная трасса этапа 1 проекта ГЛИССАДА-6: дампы Э70/дд_обмена
 * в stderr.  Включается переменной окружения BESM6_DD_DEBUG. */
static int
dd_debug (void)
{
    static int v = -1;
    if (v < 0)
        v = getenv ("BESM6_DD_DEBUG") != NULL;
    return v;
}

/*
 * Functional NITI / ГРАФОР emulation layer (NITI-EMULATION-PLAN.md, stage B2).
 *
 * ЭК 1210/1216/1217 select the NITI plotter scheme: in the real OS ТРУБКА
 * loads the ДУДКА driver through gпнити.  The frame that ДУДКА would pass
 * to ФИЗОБМ with KUS bit 27 (е27гп) is captured through the E50 01211
 * writer below.
 *
 * ЭК 1200-1207 are the native ГРАФОР/ГП extract codes dispatched by Э5200
 * (ТРУБКА).  In the K-71 build none of them performs a 64-word plotter
 * copy: the copy loop lives in .ОБЩ2, which is excluded from that build.
 * ЭК 1205 (ГРАФИК) only prints "ВК.Г" on the terminal via ВЫДТ1/ТВ40;
 * 1204 writes a word to the tablo, 1206 (БОБН) requests an archive reel and
 * 1207 (ЗАХВ) toggles the BENSON capture flag.  These handlers exist to
 * keep the native library off the E_UNIMP path; they log state only and
 * never produce plot.dat.  Registers, buffers and interrupts are not
 * emulated, so the plot.dat format of the real 01211 path stays identical.
 */
static int      niti_active;            /* a NITI scheme has been selected  */
static unsigned niti_scheme;            /* last selected subfunction        */
static unsigned niti_schemes;           /* number of scheme selections      */
static unsigned niti_frames;            /* number of captured frames        */
static unsigned niti_words;             /* number of captured words         */
static unsigned gp_mode;                /* last 1200-1207 subfunction       */
static unsigned gp_events[8];           /* counters for ЭК 1200..01207      */
static unsigned gp_status;              /* ЭК 1200: "ready" status          */
static unsigned gp_benson;              /* ЭК 1207: BENSON capture flag     */

/* Functional DUDKA / BENSON (НИТИ) state: B1 on top of the B2 trace layer.
 * These are the C equivalents of the DMS cells D10113..D10164 in dudka.be.
 * The guest vector-table replacement (обвнп1+30) is deliberately NOT emulated. */
#define DUDKA_REG34_MAX   32
#define DUDKA_UVV_MAX     16
#define DU_FRAME_WORDS    64
#define DU_REG240_WORDS   64
/* Core address with both high bits set: unreachable by ADDR()/XADDR() in the
 * guest, so the BENSON frame can be staged here before calling niti_frame(). */
#define DU_CORE_SCRATCH   60000

#define DU_F_E18  (1u << 0)     /* Бенсон interrupt enabled/delivered        */
#define DU_F_E25  (1u << 1)     /* end-of-transfer bit added to КУС записи   */
#define DU_F_E28  (1u << 2)     /* command strobe                            */
#define DU_F_E48  (1u << 3)     /* завершение передачи (проверка E48)        */
#define DU_F_WTC  (1u << 4)     /* 64 words collected in the active buffer   */

/* Command constants from dudka.be, in BESM-6 octal: bits 28/25, 28/21,
 * 28, D10171 and D10167.  Written in octal to match the source listing. */
#define DU_E28E25  0000000044000000ULL
#define DU_E28E21  0000001004000000ULL
#define DU_E28     0000000004000000ULL
#define DU_D10171  0000001140000000ULL
#define DU_D10167  0000001500000040ULL

static uint64_t du_reg34[DUDKA_REG34_MAX];   /* words written to РЕГ '34'      */
static unsigned du_reg34_count;              /* total РЕГ '34' writes          */
static unsigned du_reg240_reads;             /* total РЕГ '240' reads          */
static uint64_t du_reg240_queue[DU_REG240_WORDS];
static unsigned du_reg240_pos;               /* next word from Бенсон          */
static unsigned du_frames;                   /* frames handed to ФИЗОБМ        */
static unsigned du_words;                    /* words received from Бенсон     */
static unsigned du_buf_active;               /* 0 = D10161 page, 1 = D10162    */
static uint64_t du_buf_a[DU_FRAME_WORDS];
static uint64_t du_buf_b[DU_FRAME_WORDS];
static uint64_t du_flags;
static unsigned du_word_idx;                 /* position in active buffer      */
static unsigned du_ack;                      /* 5-word IRQ ACK sequences        */
static int      du_ready;                    /* Бенсон initialised             */
static unsigned dudka_uvv_writes;
static unsigned dudka_uvv[DUDKA_UVV_MAX];      /* device numbers             */
ushort dudka_uvv_addr;                  /* EXT 033 address field, set in cu.c */
static unsigned du_page_words;          /* РЕГ '240' reads on current page  */
static unsigned du_mask_loads;          /* formerly du_init: 9-word mask tag */

/* Phase 5.  There is no ГРП/МГРП/ПРП/МПРП model and no guest vector table in
 * dispak.  DU_PROXY_EVENT is therefore a CONDITIONAL PROXY: in dispak's
 * 1-based event scale event 18 means "обращение в закрытый лист", NOT the
 * разряд 18 ГРП line that the guest vectors through обвнп1+30.
 * NOT modeled: guest vector обвнп1+30 / GRP18 dispatch. */
#define DU_PROXY_EVENT        (1u << 17)
#define DU_KUS_GP27           (1ULL << 21) /* е27гп: 27-й разряд КУС считается
                                            * слева, как E28 = 1<<20 above */
#define DU_UVV_READY_E13      (1u << 12)   /* simh PI2_START, not confirmed
                                            * against a dispak hardware spec */

static unsigned du_grp18;               /* TV67/TV67A bit 18, set via ЭК 01215 */
static unsigned du_shg;                 /* guest ШГ E37 marker              */
static unsigned du_irq_raised;          /* eraise(proxy) calls               */
static unsigned du_irq_taken;           /* calls that armed goahead         */
static unsigned du_irq_proxy;           /* proxy events delivered           */
static unsigned du_waketasks;           /* WAKETASK E37 markers             */
static uchar    du_ring[40];            /* 0330..0377 byte ring, C mirror    */
static unsigned du_ring_pos;

/* Device model: 0.1 mm steps, 50 x 75 cm page (5000 x 7500 steps).
 * The 0.1 mm step and the five +X/-X/+Y/-Y/pen lines come from NASA
 * TM-76182 (Электроника-1001 analogue), not from a НИТИ interface spec. */
#define BEN_LIMIT_X  5000
#define BEN_LIMIT_Y  7500
struct bench_state {
        int x, y;
        int pen_down;
        int step_x, step_y;
        int limit_x, limit_y;
        int steps;
};
static struct bench_state ben = { 0, 0, 0, 1, 1, BEN_LIMIT_X, BEN_LIMIT_Y, 0 };

/* Batch decks are entered by МОНИТОР-80 in user mode, so ДУДКА commands
 * would be E_PRIV there.  This opt-in hook lets a dedicated deck exercise
 * the same interception path without changing any default behaviour. */
int
dudka_test_mode(void)
{
        static int v = -1;
        if (v < 0)
                v = getenv("BESM6_DUDKA_TEST") != NULL;
        return v;
}

static int
niti_debug(void)
{
        static int v = -1;
        if (v < 0)
                v = getenv("BESM6_NITI_DEBUG") != NULL;
        return v;
}

/* Second, human-readable output stream.  plot.dat stays byte-for-byte
 * identical; every PEN/DECODE/IRQ line is mirrored here as well as stderr. */
static FILE *plotlog;

static void
du_log(const char *fmt, ...)
{
        va_list ap;

        if (!plotlog) {
                plotlog = fopen("plot-log.txt", "w");
                if (!plotlog)
                        return;
        }
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        va_start(ap, fmt);
        vfprintf(plotlog, fmt, ap);
        va_end(ap);
}

static void
niti_report(void)
{
        fprintf(stderr,
                "NITI: summary schemes=%u frames=%u words=%u active=%d scheme=%04o\n",
                niti_schemes, niti_frames, niti_words, niti_active, niti_scheme);
        if (niti_debug())
                fprintf(stderr,
                        "GP: mode=%04o status=%u benson=%u "
                        "events 1200..1207 = %u,%u,%u,%u,%u,%u,%u,%u\n",
                        gp_mode, gp_status, gp_benson,
                        gp_events[0], gp_events[1], gp_events[2],
                        gp_events[3], gp_events[4], gp_events[5], gp_events[6],
                        gp_events[7]);
        fprintf(stderr, "DUDKA: summary reg34=%u reg240=%u uvv=%u\n",
                du_reg34_count, du_reg240_reads, dudka_uvv_writes);
        fprintf(stderr,
                "DUDKA: emu init=%u ack=%u frames=%u words=%u active=%u\n",
                du_mask_loads, du_ack, du_frames, du_words, du_buf_active);
        fprintf(stderr,
                "DUDKA: emu page_words=%u maskloads=%u grp18=%u shg=%u\n",
                du_page_words, du_mask_loads, du_grp18, du_shg);
        fprintf(stderr, "DUDKA: IRQ summary raised=%u taken=%u proxy=%u\n",
                du_irq_raised, du_irq_taken, du_irq_proxy);
        fprintf(stderr, "DUDKA: WAKETASK E37 SHG=%u "
                "(task wakeup NOT modeled: guest пб БПУ)\n", du_waketasks);
        fprintf(stderr, "DUDKA: BENCH X=%d Y=%d pen=%s steps=%d "
                "[аналог NASA, БЕНСОН-501]\n",
                ben.x, ben.y, ben.pen_down ? "DOWN" : "UP", ben.steps);
        du_benson_close();
        /* The proven renderer always writes calcomp.svg; in BENSON mode the
         * user-visible stream is БЕНСОН-501 codes, so rename that SVG and keep
         * the raw Calcomp file off disk.  [аналог NASA, БЕНСОН-501] */
        dubna_plot_finish(du_benson_mode() ? 0 : du_plot_keep());
        if (du_benson_mode()) {
                if (rename("calcomp.svg", "benson.svg") != 0 && errno != ENOENT)
                        fprintf(stderr, "DUDKA: cannot rename SVG "
                                "[аналог NASA, БЕНСОН-501]\n");
                remove("calcomp.out");
        }
        if (plotlog) {
                fclose(plotlog);
                plotlog = NULL;
        }
}

static void
niti_report_register(void)
{
        static int registered;
        if (!registered) {
                atexit(niti_report);
                registered = 1;
        }
}

uint64_t getword(ptr *bp);      /* defined below, used by niti_plot_write */

/* Write one plotter frame: octal 6-bit groups, eight numbers per line.
 * Shared by the legacy E50 01211 handler and the NITI frame hook so that
 * both produce the byte-for-byte identical plot.dat format. */
static void
niti_plot_write(ptr txt, int nwords)
{
        static FILE *plot;
        int i;

        if (plot == NULL) {
                plot = fopen("plot.dat", "w");
                if (!plot) {
                        perror("Cannot open plot.dat\n");
                        exit(1);
                }
        }
        for (i = 0; i < nwords; ++i) {
                unsigned long long v = getword(&txt);
                int j;
                for (j = 7; j >= 0; --j)
                        fprintf(plot, "%02o ", (unsigned)((v >> (j * 6)) & 077));
                fputc('\n', plot);
        }
        fflush(plot);
}

/* Decoders for a captured NITI frame.
 *
 * PROVEN path (default, BESM6_NITI_DECODE=proven|dubna): a 48-bit BESM word
 * is six 8-bit bytes, byte k = (word >> (40 - 8*k)) & 0xff, k=0..5.  This is
 * the byte order of Dubna's BytePointer (memory.h:77-108).  The bytes go to
 * the proven Calcomp back end of Dubna's plotter.cpp via the C bridge.  The
 * six Calcomp step bits (01=-X, 02=+X, 04=pen up, 010=pen down, 020=+Y,
 * 040=-Y) are fixed by the Dubna golden tests, but applying them to the
 * dispak bench is still [аналог CalComp], not a proven БЕНСОН/НИТИ protocol.
 *
 * HYPOTHESIS path (BESM6_NITI_DECODE=builtin|calcomp|nasa), retained only
 * for A/B comparison: 13 bits payload | 2 bits class | 1 bit extra, with
 * class = (w >> 13) & 3, payload = w & 017777, extra = (w >> 15) & 1.
 * Every line it logs says [hypothesis] or [аналог ...]; none of it is a
 * proven БЭСМ-6 protocol fact. */
enum du_action {
        ACT_NOP = 0, ACT_XPLUS, ACT_XMINUS, ACT_YPLUS, ACT_YMINUS,
        ACT_PEN_UP, ACT_PEN_DOWN
};

struct du_decode_rule {
        const char *name;
        const char *tag;
        int action[8];                  /* index = class * 2 + extra */
};

static const struct du_decode_rule du_decode_rules[] = {
        { "builtin", "[hypothesis]", {
                ACT_XPLUS, ACT_YPLUS, ACT_PEN_UP, ACT_PEN_DOWN,
                ACT_NOP, ACT_NOP, ACT_NOP, ACT_NOP } },
        { "calcomp", "[аналог CalComp]", {
                ACT_XPLUS, ACT_XMINUS, ACT_YPLUS, ACT_YMINUS,
                ACT_PEN_UP, ACT_PEN_DOWN, ACT_NOP, ACT_NOP } },
        { "nasa", "[аналог NASA]", {
                ACT_XPLUS, ACT_YPLUS, ACT_XMINUS, ACT_YMINUS,
                ACT_PEN_DOWN, ACT_PEN_UP, ACT_NOP, ACT_NOP } },
};

static const struct du_decode_rule *
du_decode_rule(void)
{
        static const struct du_decode_rule *rule;
        const char *env;
        unsigned i;

        if (rule)
                return rule;
        rule = &du_decode_rules[0];
        env = getenv("BESM6_NITI_DECODE");
        if (env)
                for (i = 0; i < sizeof du_decode_rules / sizeof du_decode_rules[0]; ++i)
                        if (!strcmp(env, du_decode_rules[i].name))
                                rule = &du_decode_rules[i];
        du_log("DUDKA: DECODE hypothesis table=%s 13+2+1 "
               "(13 payload / 2 class / 1 extra bits)\n", rule->name);
        return rule;
}

/* The proven path is the default; the old 13+2+1 table stays selectable
 * for A/B comparison only. */
static int
du_decode_proven(void)
{
        static int v = -1;
        const char *env;

        if (v < 0) {
                v = 1;
                env = getenv("BESM6_NITI_DECODE");
                if (env && (!strcmp(env, "builtin") || !strcmp(env, "calcomp") ||
                            !strcmp(env, "nasa")))
                        v = 0;
        }
        return v;
}

/* Plotter device for the bridge: 0 Calcomp (default), 1 Tektronix,
 * 2 Watanabe, -1 no bytes.  Any device is an analogue, never a claim about
 * the real БЕНСОН device. */
static int
du_plot_device(void)
{
        static int initialized;
        static int device;
        const char *env;

        if (!initialized) {
                initialized = 1;
                device = 0;
                env = getenv("BESM6_DUBNA_PLOT_DEVICE");
                if (env) {
                        if (!strcmp(env, "tektronix"))
                                device = 1;
                        else if (!strcmp(env, "watanabe"))
                                device = 2;
                        else if (!strcmp(env, "off"))
                                device = -1;
                }
        }
        return device;
}

static const char *const du_plot_device_name[] = {
        "calcomp", "tektronix", "watanabe"
};

static unsigned du_plot_bytes;          /* bytes forwarded to the bridge */

/* Dubna writes the raw *.out file only when temporary files are kept. */
static int
du_plot_keep(void)
{
        static int v = -1;
        if (v < 0)
                v = getenv("BESM6_DUBNA_PLOT_KEEP") != NULL;
        return v;
}

/* Select the user-visible plotter command stream.  Calcomp stays the default
 * so the existing decoder tests are unchanged.  [аналог NASA, БЕНСОН-501] */
static int
du_benson_mode(void)
{
        static int v = -1;
        const char *env;

        if (v < 0) {
                v = 0;
                env = getenv("BESM6_DUDKA_PLOT");
                if (env && !strcmp(env, "benson"))
                        v = 1;
        }
        return v;
}

/* Coordinate frame decoder for the FOREX -> PDEV -> ЭК 01211 path.
 *
 * FRAME CONTRACT (agreed with fortran/verify/PDEV.mad):
 *   word 0 = X in steps of 0.1 mm, octal integer
 *   word 1 = Y in steps of 0.1 mm, octal integer
 *   word 2 = pen state (0 = up, 1 = down)
 *   words 3..63 = 0
 * FOREX 4.12 stores INTEGER values with a type tag in the high 24 bits
 * (0xd00000...); the coordinate is the low 24 bits.  The tag is stripped
 * here rather than in PDEV, which only points ЭК 01211 at the COMMON.
 *
 * `coords` is the default when BESM6_DUDKA_PLOT=benson.  The previous raw
 * 6-bytes/word path stays available as BESM6_NITI_DECODE=proven (also
 * dubna/raw), so the A/B comparison against Dubna is not broken. */
static int
du_decode_coords(void)
{
        static int v = -1;
        const char *env;

        if (v < 0) {
                v = 0;
                env = getenv("BESM6_NITI_DECODE");
                if (env && !strcmp(env, "coords"))
                        v = 1;
                else if (du_benson_mode()) {
                        v = 1;
                        if (env && (!strcmp(env, "proven") ||
                                    !strcmp(env, "dubna") ||
                                    !strcmp(env, "raw")))
                                v = 0;
                }
        }
        return v;
}

static const char *const du_action_name[] = {
        "NOP", "+X", "-X", "+Y", "-Y", "UP", "DOWN"
};

static void
bench_move(const char *dir, int dx, int dy, const char *tag)
{
        int nx = ben.x + dx * ben.step_x;
        int ny = ben.y + dy * ben.step_y;

        if (nx < 0 || nx > ben.limit_x) {
                du_log("DUDKA: LIMIT X\n");
                return;
        }
        if (ny < 0 || ny > ben.limit_y) {
                du_log("DUDKA: LIMIT Y\n");
                return;
        }
        ben.x = nx;
        ben.y = ny;
        ++ben.steps;
        du_log("DUDKA: PEN %s step %s X=%05d Y=%05d (mm %d.%d,%d.%d) %s\n",
               ben.pen_down ? "DOWN" : "UP", dir, ben.x, ben.y,
               ben.x / 10, ben.x % 10, ben.y / 10, ben.y % 10, tag);
}

static void
bench_apply(int action)
{
        switch (action) {
        case ACT_XPLUS:  bench_move("+X",  1,  0, "[аналог NASA]"); break;
        case ACT_XMINUS: bench_move("-X", -1,  0, "[аналог NASA]"); break;
        case ACT_YPLUS:  bench_move("+Y",  0,  1, "[аналог NASA]"); break;
        case ACT_YMINUS: bench_move("-Y",  0, -1, "[аналог NASA]"); break;
        case ACT_PEN_UP:
                ben.pen_down = 0;
                du_log("DUDKA: PEN UP step NONE X=%05d Y=%05d [аналог NASA]\n",
                       ben.x, ben.y);
                break;
        case ACT_PEN_DOWN:
                ben.pen_down = 1;
                du_log("DUDKA: PEN DOWN step NONE X=%05d Y=%05d [аналог NASA]\n",
                       ben.x, ben.y);
                break;
        default: break;
        }
}

/* Proven Calcomp command-byte bit assignments, used only for the human-
 * readable PEN log.  This is [аналог CalComp], not a БЕНСОН protocol claim. */
static void
du_decode_calcomp_byte(unsigned char byte)
{
        char dir[8];
        int dx = 0, dy = 0;

        if (byte & 01)
                dx = -1;
        if (byte & 02)
                dx = 1;
        if (byte & 04) {
                ben.pen_down = 0;
                du_log("DUDKA: PEN UP step NONE X=%05d Y=%05d [аналог CalComp]\n",
                       ben.x, ben.y);
        }
        if (byte & 010) {
                ben.pen_down = 1;
                du_log("DUDKA: PEN DOWN step NONE X=%05d Y=%05d [аналог CalComp]\n",
                       ben.x, ben.y);
        }
        if (byte & 020)
                dy = 1;
        if (byte & 040)
                dy = -1;
        if (!dx && !dy)
                return;

        dir[0] = '\0';
        if (dx > 0)
                strcpy(dir, "+X");
        else if (dx < 0)
                strcpy(dir, "-X");
        if (dy > 0)
                strcat(dir, "+Y");
        else if (dy < 0)
                strcat(dir, "-Y");
        bench_move(dir, dx, dy, "[аналог CalComp]");
}

/* Command codes of the БЕНСОН-501 interface to Электроника-1001, table 2
 * (p.7) of NASA TM-76182.  These are external-device command codes, never an
 * НИТИ/БЭСМ-6 wire protocol.  [аналог NASA, БЕНСОН-501] */
#define BENSON_X_PLUS   06502
#define BENSON_X_MINUS  06514
#define BENSON_Y_PLUS   06501
#define BENSON_Y_MINUS  06504
#define BENSON_PEN_UP   06511
#define BENSON_PEN_DOWN 06512

static FILE *benson_out;

/* Internal bridge only: turn one БЕНСОН-501 command into the equivalent byte
 * of the already-proven plotter.cpp renderer.  A second SVG renderer would
 * duplicate tested code, so the user stream stays БЕНСОН codes while this
 * private conversion feeds the Calcomp parser through dubna_plot_put(0, .).
 * [аналог NASA, БЕНСОН-501] */
static unsigned char
benson_to_renderer_byte(unsigned code)
{
        switch (code) {
        case BENSON_X_PLUS:   return 02;
        case BENSON_X_MINUS:  return 01;
        case BENSON_Y_PLUS:   return 020;
        case BENSON_Y_MINUS:  return 040;
        case BENSON_PEN_UP:   return 04;
        case BENSON_PEN_DOWN: return 010;
        default:              return 0;
        }
}

/* Emit exactly one command: human-readable log, optional benson.out line and
 * the internal bridge to the renderer.  [аналог NASA, БЕНСОН-501] */
static void
du_benson_emit(unsigned code, const char *name, int dx, int dy, int pen_event)
{
        if (pen_event) {
                ben.pen_down = pen_event > 0;
        } else {
                bench_move(name, dx, dy, "[аналог NASA, БЕНСОН-501]");
        }
        du_log("DUDKA: BENSON command %03o (%s) X=%05d Y=%05d "
               "[аналог NASA, БЕНСОН-501]\n", code, name, ben.x, ben.y);
        if (du_plot_keep()) {
                if (!benson_out)
                        benson_out = fopen("benson.out", "w");
                if (benson_out) {
                        fprintf(benson_out, "%03o %s X=%05d Y=%05d "
                                "[аналог NASA, БЕНСОН-501]\n",
                                code, name, ben.x, ben.y);
                        fflush(benson_out);
                }
        }
        if (du_plot_device() >= 0)
                dubna_plot_put(0, benson_to_renderer_byte(code));
        ++du_plot_bytes;
}

/* One step of the БЕНСОН-501 command set.  The pen command comes first, then
 * X, then Y; a diagonal step becomes two commands.  [аналог NASA, БЕНСОН-501] */
static void
du_decode_benson_step(int dx, int dy, int pen_event)
{
        if (pen_event < 0)
                du_benson_emit(BENSON_PEN_UP, "UP", 0, 0, -1);
        else if (pen_event > 0)
                du_benson_emit(BENSON_PEN_DOWN, "DOWN", 0, 0, 1);
        if (dx > 0)
                du_benson_emit(BENSON_X_PLUS, "+X", 1, 0, 0);
        else if (dx < 0)
                du_benson_emit(BENSON_X_MINUS, "-X", -1, 0, 0);
        if (dy > 0)
                du_benson_emit(BENSON_Y_PLUS, "+Y", 0, 1, 0);
        else if (dy < 0)
                du_benson_emit(BENSON_Y_MINUS, "-Y", 0, -1, 0);
}

/* Decode an extracted byte into a БЕНСОН-501 step without calling the Calcomp
 * logger on the main path.  [аналог NASA, БЕНСОН-501] */
static void
du_benson_decode_byte(unsigned char byte)
{
        int dx = 0, dy = 0, pen[2], np = 0;

        if (byte & 01)
                dx = -1;
        if (byte & 02)
                dx = 1;
        if (byte & 04)
                pen[np++] = -1;
        if (byte & 010)
                pen[np++] = 1;
        if (byte & 020)
                dy = 1;
        if (byte & 040)
                dy = -1;
        if (!dx && !dy && !np)
                return;
        /* A byte may carry both pen bits, as in the proven bit layout; keep
         * both events, up first, then down with the step. */
        if (np == 2)
                du_decode_benson_step(0, 0, pen[0]);
        du_decode_benson_step(dx, dy, np == 2 ? pen[1] : (np ? pen[0] : 0));
}

static void
du_benson_close(void)
{
        if (benson_out) {
                fclose(benson_out);
                benson_out = NULL;
        }
}

static void
du_decode_element(unsigned w)
{
        const struct du_decode_rule *rule = du_decode_rule();
        unsigned cls = (w >> 13) & 3;
        unsigned extra = (w >> 15) & 1;
        unsigned payload = w & 017777;
        int action = rule->action[cls * 2 + extra];

        if (action == ACT_NOP) {
                du_log("DUDKA: DECODE unknown class=%u extra=%u payload=%06o "
                       "[hypothesis]\n", cls, extra, payload);
                return;
        }
        if (niti_debug())
                du_log("DUDKA: DECODE class=%u extra=%u payload=%06o -> %s %s\n",
                       cls, extra, payload, du_action_name[action], rule->tag);
        bench_apply(action);
}

/* [hypothesis] Old 13+2+1 decoder, kept for A/B comparison only.  Each
 * 48-bit word was read as three 16-bit elements (печшк3 with сда 64 /
 * mask 0177777).  Never used by the default proven path. */
static void
du_decode_stream_hypothesis(ptr txt, int nwords)
{
        int i, j;

        (void) du_decode_rule();
        for (i = 0; i < nwords; ++i) {
                uint64_t w = getword(&txt);
                for (j = 2; j >= 0; --j)
                        du_decode_element((unsigned)((w >> (j * 16)) & 0xffff));
        }
}

/* Low 24 bits of a FOREX INTEGER word; the high 24 bits are the type tag. */
static int
du_coord_value(uint64_t w)
{
        uint32_t v = (uint32_t)(w & 0xffffffu);

        if (v & 0x800000u)
                v |= 0xff000000u;               /* sign-extend negatives */
        return (int)(int32_t)v;
}

/* New default for BESM6_DUDKA_PLOT=benson: one frame carries one point.
 * UP/DOWN first, then all X steps, then all Y steps, then lift the pen so
 * the next frame starts from a known up state.  [аналог NASA, БЕНСОН-501] */
static void
du_decode_stream_coords(ptr txt, int nwords)
{
        static int logged;
        uint64_t wx, wy, wp;
        int tx, ty, pen, i;

        if (!logged) {
                logged = 1;
                du_log("DUDKA: BENSON coords frame X/Y/pen (0.1 mm steps) "
                       "[аналог NASA, БЕНСОН-501]\n");
        }
        if (nwords < 3) {
                du_log("DUDKA: BENSON coords short frame words=%d\n", nwords);
                return;
        }
        wx = getword(&txt);
        wy = getword(&txt);
        wp = getword(&txt);
        tx = du_coord_value(wx);
        ty = du_coord_value(wy);
        pen = du_coord_value(wp) ? 1 : 0;

        if (tx < 0 || tx > ben.limit_x || ty < 0 || ty > ben.limit_y) {
                du_log("DUDKA: BENSON coords out of range X=%d Y=%d\n", tx, ty);
                return;
        }
        for (i = 3; i < nwords; ++i) {
                uint64_t w = getword(&txt);

                if (w && niti_debug())
                        du_log("DUDKA: BENSON coords nonzero pad word=%d\n", i);
        }
        if (niti_debug())
                du_log("DUDKA: BENSON coords X=%d Y=%d pen=%d\n", tx, ty, pen);

        if (pen != (ben.pen_down ? 1 : 0))
                du_decode_benson_step(0, 0, pen ? 1 : -1);
        while (ben.x < tx)
                du_decode_benson_step(1, 0, 0);
        while (ben.x > tx)
                du_decode_benson_step(-1, 0, 0);
        while (ben.y < ty)
                du_decode_benson_step(0, 1, 0);
        while (ben.y > ty)
                du_decode_benson_step(0, -1, 0);
        if (pen)
                du_decode_benson_step(0, 0, -1);
        /* The proven plotter.cpp renderer flushes a path only when a move
         * arrives with the pen up.  Step out and back with the pen up so the
         * stroke just drawn reaches benson.svg; the position is restored. */
        du_decode_benson_step(1, 0, 0);
        du_decode_benson_step(-1, 0, 0);
}

/* Proven extraction: six 8-bit bytes per 48-bit word, in BytePointer order.
 * The bytes are forwarded to the Dubna plotter bridge; the raw plot.dat has
 * already been written by niti_plot_write(), so its SHA never depends on
 * this decoder or on the bridge. */
static void
du_decode_stream(ptr txt, int nwords)
{
        static int logged;
        int device;
        int i, k;

        if (!du_decode_proven()) {
                du_decode_stream_hypothesis(txt, nwords);
                return;
        }
        if (du_decode_coords()) {
                du_decode_stream_coords(txt, nwords);
                return;
        }
        if (!logged) {
                logged = 1;
                if (du_benson_mode())
                        du_log("DUDKA: BENSON codec, 6 bytes/word BytePointer "
                               "order (legacy) [аналог NASA, БЕНСОН-501]\n");
                else
                        du_log("DUDKA: DECODE proven 6 bytes/word BytePointer order "
                               "[аналог CalComp]\n");
        }
        device = du_plot_device();
        for (i = 0; i < nwords; ++i) {
                uint64_t w = getword(&txt);
                for (k = 0; k < 6; ++k) {
                        unsigned char byte =
                                (unsigned char)((w >> (40 - 8 * k)) & 0xff);
                        if (du_benson_mode()) {
                                /* Emits БЕНСОН-501 commands and feeds the
                                 * proven renderer internally. */
                                du_benson_decode_byte(byte);
                        } else {
                                if (device >= 0)
                                        dubna_plot_put(device, byte);
                                ++du_plot_bytes;
                                du_decode_calcomp_byte(byte);
                        }
                }
        }
}

/* ЭК 1210/1216/1217: select the NITI scheme. */
static void
niti_select(unsigned code)
{
        niti_report_register();
        niti_active = 1;
        niti_scheme = code;
        ++niti_schemes;
        fprintf(stderr, "NITI: scheme %04o selected\n", code);
}

/* Capture one 64-word frame from the NITI / E50 01211 data path.
 * A full B1 emulation would call this from the ФИЗОБМ KUS-bit-27 path;
 * for B2 the existing E50 01211 handler is the frame source. */
static void
niti_frame(ptr txt, int nwords)
{
        niti_report_register();
        ++niti_frames;
        niti_words += nwords;
        niti_plot_write(txt, nwords);
        du_log("DUDKA: FRAME %u words=%d\n", niti_frames, nwords);
        du_decode_stream(txt, nwords);  /* txt is passed by value: still at start */
        if (du_benson_mode())
                du_log("DUDKA: BENSON codec %s commands=%u page=%u "
                       "[аналог NASA, БЕНСОН-501]\n",
                       du_decode_coords() ? "coords" : "legacy-6bytes",
                       du_plot_bytes, niti_frames);
        else
                du_log("DUDKA: DUBNA-PLOT device=%s bytes=%u page=%u %s\n",
                       du_plot_device() >= 0 ? du_plot_device_name[du_plot_device()] : "off",
                       du_plot_bytes, niti_frames,
                       du_decode_proven() ? "[аналог CalComp]" : "[hypothesis]");
        if (niti_debug())
                fprintf(stderr, "NITI: frame %u captured (%d words)\n",
                        niti_frames, nwords);
}

/* Record one native ГРАФОР (ЭК 1200..01207) call.  The argument is the
 * accumulator value at the moment of the extracode; it is only logged,
 * no data is copied to the plotter. */
static void
gp_event(unsigned code, unsigned arg, const char *what)
{
        niti_report_register();
        gp_mode = code;
        if (code >= 01200 && code <= 01207)
                ++gp_events[code - 01200];
        if (niti_debug())
                fprintf(stderr, "GP: %04o %s arg=%06o\n", code, what, arg);
}

/* РЕГ '34': the nine words are ПРП/ГРП masks (rukava.be:404,536), NOT commands
 * to the Benson device.  The old "INIT" tag survives only as a sequence
 * recogniser for the b1-* regression decks; interrupt enable is a separate
 * TV67/TV67A event driven by ЭК 01215 (see dudka_irq_ctrl()). */
void
dudka_reg34_write(uint left, uint right)
{
        static const uint64_t init_seq[9] = {
                DU_E28E25, 0, DU_E28, 0, DU_E28E21, 0, DU_E28, 0, DU_D10171
        };
        static const uint64_t ack_seq[5] = {
                DU_D10167, 0, DU_E28E25, 0, DU_D10171
        };
        static unsigned last_init, last_ack;
        uint64_t word = ((uint64_t)(left & HALFW) << 24) | (right & HALFW);
        unsigned n, k, match;

        niti_report_register();
        du_reg34[du_reg34_count % DUDKA_REG34_MAX] = word;
        ++du_reg34_count;
        fprintf(stderr, "DUDKA: REG34 word=%08o\n", (uint)(right & HALFW));
        if (niti_debug())
                fprintf(stderr, "DUDKA: REG34 full=%08o%08o\n",
                        (uint)(left & HALFW), (uint)(right & HALFW));

        /* dudka.be:143-162 loads masks, :253-264 sends the ISR ACK. */
        n = du_reg34_count;
        if (n >= 9 && n != last_init) {
                for (match = 1, k = 0; k < 9; ++k)
                        if (du_reg34[(n - 9 + k) % DUDKA_REG34_MAX] != init_seq[k])
                                match = 0;
                if (match) {
                        last_init = n;
                        ++du_mask_loads;
                        du_ready = 1;
                        /* No DU_F_E18 here: TV67 (ЭК 01215) enables it. */
                        du_flags |= DU_F_E28;
                        fprintf(stderr,
                                "DUDKA: MASK9 loaded (ПРП/ГРП)\n");
                }
        }
        if (n >= 5 && n != last_ack) {
                for (match = 1, k = 0; k < 5; ++k)
                        if (du_reg34[(n - 5 + k) % DUDKA_REG34_MAX] != ack_seq[k])
                                match = 0;
                if (match) {
                        last_ack = n;
                        ++du_ack;
                        du_flags |= DU_F_E28;
                        fprintf(stderr, "DUDKA: MASK5 acknowledged\n");
                }
        }
}

/* РЕГ '240': ДУДКА reads one word from Бенсон on every interrupt.  A bounded
 * deterministic queue stands in for the real device; words are assembled into
 * the active 64-word buffer (D10160) exactly as dudka.be:266-278 does. */
void
dudka_reg240_read(void)
{
        static int queue_ready;
        uint64_t word, *buf;
        unsigned i;

        niti_report_register();
        if (!queue_ready) {
                /* Keep the first two B2 trace values so the old d2 deck stays
                 * byte-compatible; the rest carry deterministic frame data. */
                du_reg240_queue[0] = DU_D10171;
                du_reg240_queue[1] = DU_D10167;
                for (i = 2; i < DU_REG240_WORDS; ++i)
                        du_reg240_queue[i] =
                                ((uint64_t)(i & 077) << 42) |
                                ((uint64_t)((i * 7 + 1) & 077) << 36) |
                                ((uint64_t)((i * 3 + 2) & 077) << 30) |
                                ((uint64_t)((i + 011) & 077) << 24) |
                                ((uint64_t)((i * 5 + 3) & 077) << 18) |
                                ((uint64_t)((i * 11 + 4) & 077) << 12) |
                                ((uint64_t)((i * 13 + 5) & 077) << 6) |
                                (uint64_t)(i & 077);
                queue_ready = 1;
        }
        word = du_reg240_queue[du_reg240_pos % DU_REG240_WORDS];
        ++du_reg240_pos;
        acc.l = (uint)(word >> 24) & HALFW;
        acc.r = (uint)word & HALFW;
        ++du_reg240_reads;
        ++du_page_words;                /* D10155-style page word counter */
        fprintf(stderr, "DUDKA: REG240 read=%08o%08o\n", acc.l, acc.r);
        if (niti_debug())
                fprintf(stderr, "DUDKA: REG240 page_words=%u ring=%u "
                                "(D10155 cell NOT mapped to guest)\n",
                        du_page_words, du_ring_pos % 40);

        buf = du_buf_active ? du_buf_b : du_buf_a;
        buf[du_word_idx % DU_FRAME_WORDS] = word;
        ++du_word_idx;
        ++du_words;
        if (du_word_idx >= DU_FRAME_WORDS && !(du_flags & DU_F_WTC)) {
                du_flags |= DU_F_WTC;
                if (niti_debug())
                        fprintf(stderr, "DUDKA: buffer %u complete\n",
                                du_buf_active);
        }
}

/* Phase 5: CONDITIONAL PROXY, not a guest interrupt.  In dispak event 18
 * means "обращение в закрытый лист"; it is not the guest разряд 18 ГРП.
 * NOT modeled: guest vector обвнп1+30 / GRP18 dispatch, the ГРП/МГРП
 * registers, and the ШГ E37 dispatching done by guest пб БПУ.  The proxy is
 * gated first by the TV67/TV67A software flag, then by e53's
 * ehandler/eenab/emask test. */
static int
dudka_raise_proxy(unsigned frame)
{
        int go;

        if (!du_grp18 || !(du_flags & DU_F_E18)) {
                du_log("DUDKA: IRQ proxy event 18 frame=%u raised=0 "
                       "NOT modeled: guest vector обвнп1+30 / GRP18 dispatch\n",
                       frame);
                du_log("DUDKA: IRQ suppressed (TV67A mask cleared)\n");
                return 0;
        }
        if (!ehandler || !eenab || !(emask & DU_PROXY_EVENT)) {
                du_log("DUDKA: IRQ proxy event 18 frame=%u raised=0 "
                       "goahead=0 (ehandler=%u eenab=%u emask18=%u) "
                       "NOT modeled: guest vector обвнп1+30 / GRP18 dispatch\n",
                       frame, ehandler != 0, eenab != 0,
                       (emask & DU_PROXY_EVENT) != 0);
                return 0;
        }
        go = eraise(DU_PROXY_EVENT) != 0;
        ++du_irq_raised;
        ++du_irq_proxy;
        if (go)
                ++du_irq_taken;
        du_log("DUDKA: IRQ proxy event 18 frame=%u raised=1 goahead=%d "
               "NOT modeled: guest vector обвнп1+30 / GRP18 dispatch\n",
               frame, go);
        return go;
}

/* ЭК 01215: subfunction in the LEFT accumulator half, argument in the right.
 * 0 = TV67 (enable mask 18), 1 = TV67A (clear mask 18),
 * 2 = raise proxy without a frame, 3 = WAKETASK E37 marker.
 * reg[016] carries the ЭК number itself here, so it cannot also carry a
 * subfunction.  State hooks only: no guest vector table, no GRP18 dispatch. */
static void
dudka_irq_ctrl(unsigned sub, unsigned arg)
{
        switch (sub) {
        case 0:
                du_grp18 = 1;
                du_flags |= DU_F_E18;
                du_log("DUDKA: TV67 MASK18 set (ГРП разряд 18)\n");
                break;
        case 1:
                du_grp18 = 0;
                du_flags &= ~DU_F_E18;
                du_log("DUDKA: MASK18 cleared (TV67A, AND NOT in МГРП)\n");
                break;
        case 2: {
                int go = dudka_raise_proxy(0);
                du_log("DUDKA: IRQ raise event=18 arg=%06o goahead=%d\n",
                       arg, go);
                break;
        }
        case 3:
                du_shg = arg ? arg : 1;
                ++du_waketasks;
                du_log("DUDKA: WAKETASK E37 SHG=%u "
                       "(task wakeup NOT modeled: guest пб БПУ)\n",
                       du_waketasks);
                break;
        default:
                du_log("DUDKA: IRQ ctrl unknown sub=%u\n", sub);
                break;
        }
}

/* Transfer one 64-word frame to the plotter.  This is the C equivalent of
 * dudka.be:167-197 (вдудку, КУС записи + E25, пв ФИЗОБМ) with the KUS bit 27
 * (е27гп) checked by the ЭК 01214 hook instead of being implied.  The frame
 * is staged in guest-inaccessible core so niti_frame()/niti_plot_write()
 * remain the single plot.dat writer. */
static void
dudka_frame_xfer(void)
{
        uint64_t *buf = du_buf_active ? du_buf_b : du_buf_a;
        ptr txt;
        unsigned i, j;

        if (!du_ready) {
                fprintf(stderr,
                        "DUDKA: frame ignored (MASKS not loaded)\n");
                return;
        }
        for (i = 0; i < DU_FRAME_WORDS; ++i) {
                uint64_t w = buf[i];
                for (j = 0; j < 6; ++j)
                        core[DU_CORE_SCRATCH + i].w_b[j] =
                                (uchar)((w >> ((5 - j) * 8)) & 0377);
        }
        txt.p_w = DU_CORE_SCRATCH;
        txt.p_b = 0;
        niti_frame(txt, DU_FRAME_WORDS);

        /* dudka.be:279-289 order: D10114 -> D10157 -> D10160 -> ШГ E37. */
        ++du_frames;
        du_flags |= DU_F_E25 | DU_F_E48 | (1u << 5); /* D10114 = 1 */
        du_flags &= ~DU_F_WTC;
        du_word_idx = 0;
        du_buf_active ^= 1;             /* D10157 ^= E1 */
        fprintf(stderr, "DUDKA: buffer switched to %u "
                "(D10157=%u D10160=D10161)\n",
                du_buf_active, du_buf_active);
        du_shg = 1;
        ++du_waketasks;
        du_log("DUDKA: WAKETASK E37 SHG=%u "
               "(task wakeup NOT modeled: guest пб БПУ)\n", du_waketasks);
        dudka_raise_proxy(du_frames);
}

/* УВВ (033): only the device numbers used by ДУДКА/ТРУБКА/БЕНСОН are
 * accepted in supervisor mode; everything else keeps the old E_PRIV path.
 * 0166 here is УВВ (opcode 033), distinct from ЭК 0166 (opcode 050) used by
 * the archive-volume handler; the octal log prefix keeps them apart. */
int
ext_plot(void)
{
        static const unsigned devices[] = {
                7, 0155, 0166, 0164, 0165, 04023, 0147, 04102,
        };
        unsigned i;

        if (!supmode && !dudka_test_mode())
                return E_PRIV;
        for (i = 0; i < sizeof devices / sizeof devices[0]; ++i) {
                if (dudka_uvv_addr == devices[i]) {
                        unsigned byte = acc.r & 077;

                        niti_report_register();
                        if (dudka_uvv_writes < DUDKA_UVV_MAX)
                                dudka_uvv[dudka_uvv_writes] = devices[i];
                        ++dudka_uvv_writes;
                        fprintf(stderr, "DUDKA: UVV %o\n", devices[i]);
                        if (devices[i] == 0164) {
                                du_ring[du_ring_pos % 40] = byte;
                                ++du_ring_pos;
                                fprintf(stderr,
                                        "DUDKA: UVV 164 byte=%02o ring=%u "
                                        "(0330..0377 C mirror)\n",
                                        byte, du_ring_pos % 40);
                        } else if (devices[i] == 0165) {
                                fprintf(stderr,
                                        "DUDKA: UVV 165 byte=%02o inv=%02o "
                                        "(нтж П377)\n", byte, byte ^ 077);
                        } else if (devices[i] == 04102) {
                                acc.r |= DU_UVV_READY_E13;
                                fprintf(stderr,
                                        "DUDKA: UVV 4102 ready E13 "
                                        "acc=%06o (simh PI2_START, "
                                        "dispak representation NOT verified)\n",
                                        acc.r);
                        }
                        return E_SUCCESS;
                }
        }
        return E_PRIV;
}

#define IS_DIGIT(c)     (c >= '0' && c <= '9')
#define IS_CHAR(c)      ((c >= 0101 && c <= 0132) || \
                         (c >= 0140 && c <= 0176))

#define INTERCEPT_E64 002
#define INTERCEPT_E70 004
#define INTERCEPT_E71 010
#define INTERCEPT_E74 020

static int
intercept_ex(int mask) {
    alureg_t t;
    t.l = t.r = 0;
    STORE(acc, intercept_addr-1);
    t.r = reg[TRAPRETREG];
    STORE(t, intercept_addr-2);
    t.r = reg[016] | mask << 15;
    STORE(t, intercept_addr-3);
    intercept_mask &= ~mask;
    JMP(intercept_addr);
    return E_SUCCESS;
}

static void     exform(void);

static inline uchar
peekbyte(ptr *bp)
{
        return core[bp->p_w].w_b[bp->p_b];
}

uchar
getbyte(ptr *bp)
{
        uchar   c = peekbyte(bp);

        ++bp->p_b;
        if (bp->p_b == 6) {
                bp->p_b = 0;
                ++bp->p_w;
        }
        return c;
}

void
putbyte(ptr *bp, uchar c)
{
        core[bp->p_w].w_b[bp->p_b++] = c;

        if (bp->p_b == 6) {
                bp->p_b = 0;
                ++bp->p_w;
        }
}

uint64_t
getword(ptr *bp)
{
        uint64_t        w = 0;
        int             i;

        if (bp->p_b) {
                bp->p_b = 0;
                ++bp->p_w;
        }
        for (i = 0; i < 6; ++i)
                w = w << 8 | core[bp->p_w].w_b[i];

        ++bp->p_w;

        return w;
}

void
cwadj(uinstr_t *ip)
{
        if (ip->i_opcode >= 0100) {
                ip->i_opcode = (ip->i_opcode - 060) << 3;
                ip->i_opcode |= ip->i_addr >> 12;
                ip->i_addr &= 0xfff;
        } else if (ip->i_addr & 070000) {
                ip->i_addr &= 07777;
                ip->i_opcode |= 0100;
        }
}

void
terminate(void)
{
        unsigned        u;

        for (u = 030; u < 070; ++u)
                if (disks[u].diskh)
                        disk_close(disks[u].diskh);
}

static int
lflush(uchar *line)
{
        int     i;

        for (i = 127; i >= 0; --i)
                if (line[i] != GOST_SPACE)
                        break;

        if (i < 0)
                return 0;

        gost_write (line, i + 1, stdout);
        memset (line, GOST_SPACE, i + 1);
        return 1;
}

void
print_text_debug (ushort addr0, ushort addr1, int itm_flag, int pos)
{
        ptr             bp;
        int             c;

        printf ("*** E64  %s ", itm_flag ? "itm" : "gost");
        bp.p_w = addr0;
        bp.p_b = 0;
        for (;;) {
                if (! bp.p_w) {
done:                   printf ("\n");
                        return;
                }
                if (addr1 && bp.p_w == addr1 + 1)
                        goto done;

                c = getbyte(&bp);
                printf ("-%03o", c);

                /* end of information */
                if (itm_flag)
                        switch (c) {
                        case 0140: /* end of information */
                                goto done;
                        case 0173: /* repeat last symbol */
                                c = getbyte(&bp);
                                printf ("-%03o", c);
                                if (c == 040)
                                        pos = 0;
                                else
                                        pos += c & 017;
                                break;
                        default:
                                /* No space left on line. */
                                if (! addr1)
                                        goto done;
                                ++pos;
                                if (pos == 128) {
                                        /* No space left on line. */
                                        putchar('/');
                                        pos = 0;
                                }
                                break;
                        }
                else {
                        switch (c) {
                        case GOST_END_OF_INFORMATION:
                        case 0231:
                        case GOST_EOF:
                                goto done;
                        case 0201: /* new page */
                                if (pos > 0)
                                        pos = 0;
                                ++pos;
                                break;
                        case GOST_CARRIAGE_RETURN:
                        case GOST_NEWLINE:
                                pos = 0;
                                break;
                        case 0143: /* null width symbol */
                        case 0341:
                                break;
                        case GOST_SET_POSITION:
                        case 0200: /* set position */
                                c = getbyte(&bp);
                                printf ("-%03o", c);
                                pos = c % 128;
                                break;
                        default:
                                /* No space left on line. */
                                ++pos;
                                if (pos == 128) {
                                        /* No space left on line. */
                                        putchar('/');
                                        if (addr1)
                                                pos = 0;
                                }
                                break;
                        }
                }
        }
}

/*
 * Print string in GOST format.
 * Return next data address.
 */
static ushort
print_gost(ushort addr0, ushort addr1, uchar *line, int pos, int *need_newline)
{
        ptr             bp;
        uchar           c, lastc = GOST_SPACE;

        bp.p_w = addr0;
        bp.p_b = 0;
        for (;;) {
                if (! bp.p_w)
                        return 0;

                /* No data to print. */
                if (addr1 && bp.p_w == addr1 + 1)
                        return bp.p_w;

                c = getbyte(&bp);
                switch (c) {
                case GOST_EOF:
                case GOST_END_OF_INFORMATION:
                case 0231:
                        if (pos==0 || pos==128)
                                *need_newline = 0;
                        if (bp.p_b)
                                ++bp.p_w;
                        return bp.p_w;
                case 0201: /* new page */
                        if (pos) {
                                lflush(line);
                                pos = 0;
                        }
                        if (! isatty (1))
                                utf8_puts("\f", stdout);
                        line[pos++] = GOST_SPACE;
                        break;
                case GOST_CARRIAGE_RETURN:
                case GOST_NEWLINE:
                        if (pos == 128) {
                                pos = 0;
                                break;
                        }
                        if (pos) {
                                lflush(line);
                                pos = 0;
                        }
                        utf8_puts("\n", stdout);
                        break;
                case 0143: /* null width symbol */
                case 0341:
                        break;
                case GOST_SET_POSITION:
                case 0200: /* set position */
                        c = getbyte(&bp);
                        pos = c % 128;
                        break;
                case 0174:
                case 0265: /* repeat last symbol */
                        c = getbyte(&bp);
                        while (c-- > 0) {
                                if (pos == 128) {
                                        lflush(line);
                                        putchar('\n');
                                        pos = 0;
                                }
                                if (line[pos] == GOST_SPACE)
                                        line[pos] = lastc;
                                ++pos;
                        }
                        break;
                case GOST_SPACE2: /* blank */
                case 0242: /* used as space by forex */
                        c = GOST_SPACE;
                        /* fall through... */
                default:
                        if (pos == 128) {
                                if (addr1)
                                        pos = 0;
                                else {
                                        /* No space left on line. */
                                        *need_newline = 0;
                                        if (bp.p_b)
                                                ++bp.p_w;
                                        return bp.p_w;
                                }
                        }
                        if (line[pos] != GOST_SPACE) {
                                lflush(line);
                                fputs("\\\n", stdout);
                        }
                        line[pos] = c;
                        lastc = c;
                        ++pos;
                        if (pos == 128) {
                                /* No space left on line. */
                                lflush(line);
                                putchar('\n');
                        }
                        break;
                }
        }
}

/*
 * Print string in ITM format.
 * Return next data address.
 */
static ushort
print_itm(ushort addr0, ushort addr1, uchar *line, int pos)
{
        ptr             bp;
        uchar           c, lastc = GOST_SPACE;

        bp.p_w = addr0;
        bp.p_b = 0;
        for (;;) {
                if (! bp.p_w)
                        return 0;

                /* No data to print. */
                if (addr1 && bp.p_w == addr1 + 1)
                        return bp.p_w;

                /* No space left on line. */
                if (pos == 128) {
                        if (! addr1) {
                                if (bp.p_b)
                                        ++bp.p_w;
                                return bp.p_w;
                        }
                        lflush(line);
                        putchar('\n');
                        pos = 0;
                }
                c = getbyte(&bp);
                switch (c) {
                case 0140: /* end of information */
                        if (bp.p_b)
                                ++bp.p_w;
                        return bp.p_w;
                case 040: /* blank */
                        line[pos++] = GOST_SPACE;
                        break;
                case 0173: /* repeat last symbol */
                        c = getbyte(&bp);
                        if (c == 040) {
                                /* fill line by last symbol (?) */
                                memset (line, lastc, 128);
                                lflush(line);
                                putchar('\n');
                                pos = 0;
                        } else
                                while (c-- & 017)
                                        line[pos++] = lastc;
                        break;
                default:
                        lastc = itm_to_gost [c];
                        line[pos++] = lastc;
                        break;
                }
        }
}

static void
print_char (uchar *line, int *pos, int sym)
{
        if (*pos == 128) {
                lflush (line);
                putchar ('\n');
        }
        line [(*pos) & 127] = sym;
        ++(*pos);
}

/*
 * Print word(s) in octal format.
 * Return next data address.
 */
static ushort
print_octal(ushort addr0, ushort addr1, uchar *line, int pos,
        int digits, int width, int repeat)
{
        uint64_t w;
        int i;

        if (digits > 16)
                digits = 16;
        for (;;) {
                if (! addr0)
                        return 0;

                /* No data to print. */
                if (addr1 && addr0 == addr1 + 1)
                        return addr0;

                /* No space left on line. */
                if (pos >= 128) {
                        if (! addr1)
                                return 0;
                        return addr0;
                }
                w = (uint64_t) core[addr0].w_b[0] << 40 |
                        (uint64_t) core[addr0].w_b[1] << 32 |
                        (uint) core[addr0].w_b[2] << 24 |
                        (uint) core[addr0].w_b[3] << 16 |
                        core[addr0].w_b[4] << 8 | core[addr0].w_b[5];
                ++addr0;

                w <<= 64 - digits * 3;
                for (i=0; i<digits; ++i) {
                        print_char (line, &pos, (int) (w >> 61) & 7);
                        w <<= 3;
                }

                if (! repeat)
                        return addr0;
                --repeat;
                if (width)
                        pos += width - digits;
        }
}

static void
print_command1 (uchar *line, int *pos, unsigned long cmd)
{
        print_char (line, pos, cmd >> 23 & 1);
        print_char (line, pos, cmd >> 20 & 7);
        print_char (line, pos, GOST_SPACE);
        if (cmd & 02000000) {
                /* long address command */
                print_char (line, pos, cmd >> 18 & 3);
                print_char (line, pos, cmd >> 15 & 7);
                print_char (line, pos, GOST_SPACE);
                print_char (line, pos, cmd >> 12 & 7);
        } else {
                /* short address command */
                print_char (line, pos, cmd >> 18 & 1);
                print_char (line, pos, cmd >> 15 & 7);
                print_char (line, pos, cmd >> 12 & 7);
                print_char (line, pos, GOST_SPACE);
        }
        print_char (line, pos, cmd >> 9 & 7);
        print_char (line, pos, cmd >> 6 & 7);
        print_char (line, pos, cmd >> 3 & 7);
        print_char (line, pos, cmd & 7);
}

/*
 * Print CPU instruction(s).
 * Return next data address.
 */
static ushort
print_command(ushort addr0, ushort addr1, uchar *line, int pos,
        int width, int repeat)
{
        unsigned long a, b;

        for (;;) {
                if (! addr0)
                        return 0;

                /* No data to print. */
                if (addr1 && addr0 == addr1 + 1)
                        return addr0;

                /* No space left on line. */
                if (pos >= 128) {
                        if (! addr1)
                                return 0;
                        return addr0;
                }
                a = (unsigned long) core[addr0].w_b[0] << 16 |
                        core[addr0].w_b[1] << 8 | core[addr0].w_b[2];
                b = (unsigned long) core[addr0].w_b[3] << 16 |
                        core[addr0].w_b[4] << 8 | core[addr0].w_b[5];
                ++addr0;

                print_command1 (line, &pos, a);
                print_char (line, &pos, GOST_SPACE);
                print_command1 (line, &pos, b);

                if (! repeat)
                        return addr0;
                --repeat;
                if (width)
                        pos += width - 23;
        }
}

/*
 * Extract decimal exponent from the real value.
 * Return value in range 0.1 - 0.9(9).
 * Input value must be nonzero positive.
 */
static double
real_exponent (double value, int *exponent)
{
        *exponent = 0;
        if (value <= 0)
                return 0; /* cannot happen */

        while (value >= 1000000) {
                *exponent += 6;
                value /= 1000000;
        }
        while (value >= 1) {
                ++*exponent;
                value /= 10;
        }
        while (value < 0.0000001) {
                *exponent -= 6;
                value *= 1000000;
        }
        while (value < 0.1) {
                --*exponent;
                value *= 10;
        }
        return value;
}

/*
 * Print real number(s).
 * Return next data address.
 */
static ushort
print_real(ushort addr0, ushort addr1, uchar *line, int pos,
        int digits, int width, int repeat)
{
        int i, negative, exponent, digit;
        double value;

        if (digits > 20)
                digits = 20;
        for (;;) {
                if (! addr0)
                        return 0;

                /* No data to print. */
                if (addr1 && addr0 == addr1 + 1)
                        return addr0;

                /* No space left on line. */
                if (pos >= 128) {
                        if (! addr1)
                                return 0;
                        return addr0;
                }

                negative = (core[addr0].w_b[0] & 1);
                if (! (core[addr0].w_b[0] >> 1) && ! core[addr0].w_b[0] &&
                    ! core[addr0].w_b[1] && ! core[addr0].w_b[2] &&
                    ! core[addr0].w_b[3] && ! core[addr0].w_b[4] &&
                    ! core[addr0].w_b[5]) {
                        value = 0;
                        exponent = 0;
                } else {
                        value = fetch_real (addr0);
                        if (value < 0)
                                value = -value;
                        value = real_exponent (value, &exponent);
                }
                ++addr0;

                print_char (line, &pos, GOST_SPACE);
                print_char (line, &pos, negative ? GOST_MINUS : GOST_PLUS);
                for (i=0; i<digits-4; ++i) {
                        value = value * 10;
                        digit = (int) value;
                        print_char (line, &pos, digit);
                        value -= digit;
                }
                print_char (line, &pos, GOST_LOWER_TEN);
                if (exponent >= 0)
                        print_char (line, &pos, GOST_PLUS);
                else {
                        print_char (line, &pos, GOST_MINUS);
                        exponent = -exponent;
                }
                print_char (line, &pos, exponent / 10);
                print_char (line, &pos, exponent % 10);

                if (! repeat)
                        return addr0;
                --repeat;
                if (width)
                        pos += width - digits - 2;
        }
}

/*
 * Extracode 64: printing.
 *
 * The information array has the following format
 * - First word:
 *   iiii ........ xxxxxxxxxxxx
 *   jjjj ........ yyyyyyyyyyyy
 * - Other words:
 *   ffff bbbbbbbb dddddddddddd
 *   esss wwwwwwww rrrrrrrrrrrr
 *
 * Here:
 * x+Ri - start address of data
 * y+Rj - end address of data
 * f    - print format
 * b    - starting position, 0 - most left
 * d    - number of digits (for integer formats)
 * e    - 1 for final word
 * s    - skip this number of lines
 * w    - total field width (for integer formats)
 * r    - repetition counter (0-once, 1-twice etc)
 *
 * Print formats:
 * 0    - text in GOST encoding
 * 1    - CPU instruction
 * 2    - octal number
 * 3    - real number (mantissa=digits-4)
 * 4    - text in ITM encoding
 */
int
print(void)
{
        ushort          cwaddr = reg[016];
        word_t          *wp0, *wp;
        ushort          addr0, addr1;
        uchar           line[128];
        int             format, offset, digits, final, width, repeat;
        int             need_newline;

        if (intercept_mask & INTERCEPT_E64) {
            return intercept_ex(INTERCEPT_E64);
        }
        if (! pout_enable)
                return E_SUCCESS;
        if (xnative)
                return E_UNIMP;
        if (cwaddr < 2)
                return E_SUCCESS;

        /* Get data pointers. */
        wp0 = &core[cwaddr];
        addr0 = ADDR(Laddr1(*wp0) + reg[Lreg(*wp0)]);
        addr1 = ADDR(Raddr1(*wp0) + reg[Rreg(*wp0)]);
        if (addr1 <= addr0)
                addr1 = 0; /* No limit */

        /* Execute every format word in order. */
        memset(line, GOST_SPACE, sizeof(line));
again:
        wp = wp0;
        for (;;) {
                ++wp;
                if (wp >= core+CORESZ || ! addr0)
                        return E_NOTERM;

                format = Lreg(*wp);
                offset = wp->w_b[1] >> 4 | ((wp->w_b[0] << 4) & 0xf0);
                digits = Laddr2(*wp);
                final = Rreg(*wp);
                width = wp->w_b[4] >> 4 | ((wp->w_b[3] << 4) & 0xf0);
                repeat = Raddr2(*wp);
                if (trace_e64) {
                        printf ("*** E64  %05o-%05o  format=%d offset=%d", addr0, addr1, format, offset);
                        if (digits) printf (" digits=%d", digits);
                        if (width) printf (" width=%d", width);
                        if (repeat) printf (" repeat=%d", repeat);
                        if (final) printf (" FINAL=%#o", final);
                        printf ("\n");
                }
                need_newline = 1;
                switch (format) {
                case 0: /* text in GOST encoding */
                case 8:
                        if (trace_e64)
                                print_text_debug (addr0, addr1, 0, offset);
                        addr0 = print_gost(addr0, addr1, line, offset,
                                &need_newline);
                        break;
                case 1: /* CPU instruction */
                case 5:
                case 9:
                case 13:
                        addr0 = print_command(addr0, addr1, line, offset,
                                width, repeat);
                        break;
                case 2: /* octal number */
                case 10:
                        addr0 = print_octal(addr0, addr1, line, offset,
                                digits, width, repeat);
                        break;
                case 3: /* real number */
                case 11:
                        addr0 = print_real(addr0, addr1, line, offset,
                                digits, width, repeat);
                        break;
                case 4: /* text in ITM encoding */
                case 12:
                        if (trace_e64)
                                print_text_debug (addr0, addr1, 1, offset);
                        addr0 = print_itm(addr0, addr1, line, offset);
                        break;
                case 6: /* hex number */
                case 7:
                case 14:
                case 15:
                        //TODO
                        //addr0 = print_hex(addr0, addr1, line, offset,
                        //      digits, width, repeat);
                        break;
                }
                if (final & 8) {
                        final &= 7;
                        if (lflush(line) || (need_newline && ! final))
                                ++final;
                        if (addr1 && addr0 <= addr1) {
                                /* Repeat printing task until all data expired. */
                                putchar('\n');
                                goto again;
                        }
                        while (final-- > 0)
                                putchar('\n');
                        break;
                }
                /* Check the limit of data pointer. */
                if (addr1 && addr0 > addr1) {
                        lflush(line);
                        putchar('\n');
                        break;
                }
        }
        fflush(stdout);
        return E_SUCCESS;
}

void
restore_state() {
        int addr = acc.r ? acc.r & 077777 : ehandler;
        LOAD(acc, addr - 11);
        LOAD(enreg, addr - 10);
        enreg.l >>= 17;
        dis_exc = enreg.l & 040;
        G_LOG = enreg.o & 004;
        G_MUL = enreg.o & 010;
        G_ADD = enreg.o & 020;
        dis_round = enreg.o & 002;
        dis_norm = enreg.o & 001;
        LOAD(accex, addr - 9);
        LOAD(enreg, addr - 7);
        reg[TRAPRETREG] = enreg.r & 077777;
        LOAD(enreg, addr - 6);
        right = enreg.r & 1;
        LOAD(enreg, addr - 5);
        reg[MODREG] = enreg.r & 077777;
        LOAD(enreg, addr - 4);
        reg[017] = enreg.r & 077777;
        LOAD(enreg, addr - 3);
        reg[016] = enreg.r & 077777;
        LOAD(enreg, addr - 2);
        reg[015] = enreg.r & 077777;
        LOAD(enreg, addr - 8);
        JMP(enreg.r & 077777);
        eenab = 1;
}

int
e53(void)
{
        switch (reg[016]) {
        case 010: {     /* get time since midnight */
                acc.r = ticks_since_midnight();
                acc.l = 0;
                return E_SUCCESS;
        }
        case 011:               /* set handler address          */
                ehandler = ADDR(acc.r);
                acc.l = acc.r = 0;
                return E_SUCCESS;
        case 012:               /* set event mask */
                emask = acc.r & 0xffffff;
                return E_SUCCESS;
        case 013:               /* disable async processes */
                eenab = 0;
                return E_SUCCESS;
        case 014:               /* enable async processes */
                eenab = 1;
                return E_SUCCESS;
        case 015:
                restore_state();
                return E_SUCCESS;
        case 017: {             /* wait for events      */
                struct itimerval        itv = {{0, 0}, {0, 0}};

                acc.l = 0;
                if (!(emask & 1)) {
                        acc.r = 2;
                        return E_SUCCESS;
                }
                eenab = 1;
                if (eraise(0)) {
                        acc.r = 0;
                        return E_SUCCESS;
                }
                getitimer(ITIMER_REAL, &itv);
                if (!(itv.it_value.tv_sec | itv.it_value.tv_usec)) {
                        itv.it_value.tv_usec = 040 * 80000;
                        setitimer(ITIMER_REAL, &itv, NULL);
                }
                pause();
                acc.r = 0;
                return E_SUCCESS;
        }
        case 021:               /* clear or declare events      */
                if (acc.l & 0x800000)   /* clear        */
                        events &= ~acc.r;
                else
                        (void) eraise(acc.r & 0xffffff);
                return E_SUCCESS;
        case 000:
                return elfun(EF_ARCTG);
        default:
                return E_UNIMP;
        }
}

int
e51(void)
{
        return elfun(reg[016] ? EF_COS : EF_SIN);
}

uint64_t userid() {
        return (uint64_t) user.l << 24 | user.r;
}

/*
 * Э50 12 - распознаватель текстовой строки (dесоdеr)
 *
 * А.П.Сапожников 22/12/80
 */
int
e50_12(void)
{
        /*
         * Строка подается в isо или в соsу. Признак конца - байт '000' или '012'.
         * Информация для экстракода на сумматоре:
         *     1-15, 21-24 разряды - индексированный адрес начала строки или 0, если работа с
         *          текущего места строки.
         *     16 - признак поглощения пробелов
         *     17 - символы '*' и '/' суть буквы.
         *     18 - восьмеричные числа задаются без суффикса "в"
         *     19 - посимвольное сканирование
         *
         * Результатом работы является тип фрагмента в индекс-регистре 14
         * и сам фрагмент. Пробелы в начале фрагмента типа "число"
         * всегда сглатываются. Если фрагмент - число, то это число
         * выдается на сумматоре, а в РМР выдается (1-8 разряды) код
         * ограничителя и (25-33 разряды) позиция этого ограничителя во
         * входной строке. Если фрагмент - идентификатор или текст,
         * заключенный в апострофы, то он выдается на сумматоре (начало) и
         * в РМР (продолжение). Хвост текста заполняется пробелами. Если
         * текст более 12 символов, то этот факт запоминается, и остаток
         * текста будет выдан при следующем обращении.
         *
         * Возможные значения индекс-регистра 14:
         *     0 - ошибка, фрагмент не распознан
         *     1 - восьмеричное (123в, 99d, -10в)
         *     2 - целое (8000, -999)
         *     3 - вещественное (3.62, -3.141е-3)
         *     4 - идентификатор или текст в апострофах
         *     5 - то же, но длиной > 12 символов
         *     6 - пустой фрагмент
         */
        int addr = acc.r & 077777;
        //int skip_spaces = (acc.r >> 15) & 1;
        int star_slash_flag = (acc.r >> 16) & 1;
        //int octal_flag = (acc.r >> 17) & 1;
        int char_mode = (acc.r >> 18) & 1;
        int m = (acc.r >> 20) & 15;
        static ptr bp;
        static int index;
        static char ident[128];
        static int ident_len;
        unsigned long long value;
        int negate = 0;

        //printf ("*** e50_12: acc = %08o%08o\n", acc.l, acc.r);
        if (m)
                addr += reg[m];

        if (addr != 0) {
                bp.p_w = addr;
                bp.p_b = 0;
                index = 0;
        } else {
                /* Continue from current place. */
                if (bp.p_w == 0) {
                        /* Parse error. */
                        reg[14] = 0;
                        acc.r = acc.l = 0;
                        //printf ("    --- Parse error\n");
                        return E_SUCCESS;
                }
        }
#if 0
        ptr tp = bp;
        while (tp.p_w != 0) {
                int c = getbyte(&tp);
                //printf ("-%03o", c);
                printf ("%c", c ? c : '@');

                if (c == 0 || c == 012)
                        break;
        }
        printf ("\n");
#endif
        if (char_mode) {
                /*
                 * В режиме посимвольного сканирования текущий символ строки выдается на
                 * сумматоре (прижат влево и дополнен пробелами) и в 1-8 разряды РМР.
                 * В 25-33 разрядах РМР выдается номер позиции символа в строке.
                 * Пробелы при необходимости поглощаются.
                 *
                 * Тип символа выдается в индекс-регистре 14:
                 *     0 - конец строки ('000','012')
                 *     1 - цифра (0 - 9)
                 *     2 - буква (а-z-я, по заказу: /, *)
                 *     3 - разделитель (не цифра и не буква)
                 */
                for (;;) {
                        if (bp.p_w == 0) {
                                /* Error */
                                reg[14] = 0;
                                acc.l = acc.r = 0;
                                accex.l = index;
                                accex.r = 0;
                                //printf ("    --- Error\n");
                                return E_SUCCESS;
                        }
                        int c = getbyte(&bp);
                        index++;
                        switch (c) {
                        case 0:
                        case 012:
                                /* End of line */
                                //printf ("    --- End of line, index = %u\n", index);
                                reg[14] = 0;
ret:
                                acc.l = c<<16 | ' '<<8 | ' ';
                                acc.r = ' '<<16 | ' '<<8 | ' ';
                                accex.l = index;
                                accex.r = c;
                                return E_SUCCESS;

                        case '0': case '1': case '2': case '3': case '4':
                        case '5': case '6': case '7': case '8': case '9':
                                /* Digit */
                                reg[14] = 1;
                                //printf ("    --- Digit = %d, index = %u\n", c-'0', index);
                                goto ret;

                        case '*':
                        case '/':
                                if (star_slash_flag)
                                        goto letter;
                                goto delimiter;

                        default:
                                if (IS_CHAR(c)) {
letter:                                 reg[14] = 2;
                                        //printf ("    --- Letter = '%c', index = %u\n", c, index);
                                        goto ret;
                                }
delimiter:                      reg[14] = 3;
                                //printf ("    --- Delimiter = '%c', index = %u\n", c, index);
                                goto ret;
                        }
                }
        }

        for (;;) {
                if (bp.p_w == 0) {
                        /* Error */
                        reg[14] = 0;
                        acc.l = acc.r = 0;
                        //printf ("    --- Error\n");
                        return E_SUCCESS;
                }
                int c = getbyte(&bp);
                index++;
                switch (c) {
                case 0:
                case 012:
                        /* Empty fragment */
empty:                  reg[14] = 6;
                        acc.l = 0;
                        acc.r = c;
                        //printf ("    --- Delimiter = '%c'\n", c);
                        return E_SUCCESS;

                case ' ':
                        continue;

                case '0': case '1': case '2': case '3': case '4':
                case '5': case '6': case '7': case '8': case '9':
number:                 value = c - '0';
                        for (;;) {
                                c = getbyte(&bp);
                                index++;

                                if (!IS_DIGIT(c))
                                        break;

                                /* Currently only octal numbers are supported. */
                                value = (value << 3) + (c - '0');
                        }
                        if (negate)
                                value = -value;

                        /* Return value, delimiter and it's index */
                        reg[14] = 1;
                        acc.l = value >> 24;
                        acc.r = value & 0xffffff;
                        accex.l = index;
                        accex.r = c;
                        //printf ("    --- Number = %#jo, delimiter = '%c', index = %u\n", (intmax_t)value, c, index);
                        return E_SUCCESS;

                case '-':
                        c = peekbyte(&bp);
                        if (IS_DIGIT(c)) {
                                negate = 1;
                                goto number;
                        }
                        c = '-';
                        goto empty;

                case '*':
                case '/':
                        if (star_slash_flag)
                                goto ident;
                        goto empty;

                default:
                        if (!IS_CHAR(c))
                                goto empty;

                        /* Get identifier */
ident:                  ident[0] = c;
                        ident_len = 1;
                        memset(ident + 1, ' ', sizeof(ident) - 1);
                        while (ident_len < sizeof(ident)) {
                                c = getbyte(&bp);
                                index++;

                                if (!IS_CHAR(c) && !IS_DIGIT(c) &&
                                    (!star_slash_flag || (c != '*' && c != '/')))
                                        break;

                                /* Currently only octal numbers are supported. */
                                ident[ident_len++] = c;
                        }

                        /* Only short identifier are supported for now */
                        reg[14] = 4;
                        acc.l = ident[0] << 16 | ident[1] << 8 | ident[2];
                        acc.r = ident[3] << 16 | ident[4] << 8 | ident[5];
                        accex.l = ident[6] << 16 | ident[7] << 8 | ident[8];
                        accex.r = ident[9] << 16 | ident[10] << 8 | ident[11];
                        //printf ("    --- Identifier = '%.*s'\n", ident_len, ident);
                        return E_SUCCESS;
                }
        }
}

/*
 * Э50 К, где К = 13, 14, 15 - форматные преобразования из двоичного кода в ISO.
 *
 * Информация на сумматоре:
 *      25-39, 45-48 - индексир.адрес binary
 *      1-15, 21-24 - индексир.адрес выходной строки или 0, если результат
 *              пишется с текущей позиции выходной строки.
 *      40-44 - N: число позиций для размещения полученного текстового фрагмента.
 *      16-19 - М: число цифр мантиссы.
 *      20    - Т: тип нормализации текста числа внутри своего N-поля
 *              (1 - вправо, 0 - влево).
 *
 * Результат работы для всех трех экстракодов: сумматор - длина
 * выходной строки в символах, и.р.14 = 1, если превышен размер поля,
 * иначе и.р.14 = 0. В каждый момент времени последнее
 * слово выходной строки дополнено до конца пробелами.
 */
int
e50_13(void)
{
        /*
         * К=13 - спецификация Н: перенос в выходную строку N символов входной
         * строки вinаrу. Т несущественно. При М=0 символы с кодами <40в
         * заменяются на пробелы.
         */
        int src_addr = acc.l & 077777;
        int dst_addr = acc.r & 077777;
        int src_r = (acc.l >> 20) & 15;
        int dst_r = (acc.r >> 20) & 15;
        int n = (acc.l >> 15) & 0x1f;
        int m = (acc.r >> 15) & 0x0f;

        if (src_r)
                src_addr += reg[src_r];
        if (dst_r)
                dst_addr += reg[dst_r];

        printf ("*** e50_13: acc = %08o%08o, src = %05o, dst = %05o, n/m/t = %d/%d\n",
                acc.l, acc.r, src_addr, dst_addr, n, m);
        //TODO
        return E_UNIMP;
}

int
e50_14(void)
{
        /*
         * К=14 - спецификация О: преобразование вinаrу по формату О<n>
         * со вставкой одного пробела после каждых м восьмеричных цифр.
         */
        int src_addr = acc.l & 077777;
        int dst_addr = acc.r & 077777;
        int src_r = (acc.l >> 20) & 15;
        int dst_r = (acc.r >> 20) & 15;
        int n = (acc.l >> 15) & 0x1f;
        //TODO: int m = (acc.r >> 15) & 0x0f;
        int right_align = (acc.r >> 19) & 1;
        char buf[64];
        int i;
        static ptr bp;

        if (src_r)
                src_addr += reg[src_r];
        if (dst_r)
                dst_addr += reg[dst_r];

        //printf ("*** e50_14: acc = %08o%08o, src = %05o, dst = %05o, n/m/t = %d/%d/%d\n",
        //      acc.l, acc.r, src_addr, dst_addr, n, m, right_align);

        if (dst_addr != 0) {
                bp.p_w = dst_addr;
                bp.p_b = 0;
        } else {
                /* Continue from current place. */
                if (bp.p_w == 0) {
                        /* Error. */
                        reg[14] = 1;
                        acc.r = acc.l = 0;
                        //printf ("    --- Error\n");
                        return E_SUCCESS;
                }
        }

        word_t *wp = &core[src_addr];
        uint64_t w = (uint64_t) wp->w_b[0] << 40 | (uint64_t) wp->w_b[1] << 32 |
                (uint) wp->w_b[2] << 24 | (uint) wp->w_b[3] << 16 |
                wp->w_b[4] << 8 | wp->w_b[5];

        snprintf(buf, sizeof(buf), "%016jo", (intmax_t)w);
        //printf ("    --- value = %s\n", buf);

        /* Right align */
        if (right_align)
                for (i=16; i<n; i++)
                        putbyte(&bp, ' ');

        /* Print N digits */
        char *p = buf;
        if (n < 16)
                p += 16 - n;
        for (i=0; i<n && i<16; i++)
                putbyte(&bp, *p++);

        /* Left align */
        if (!right_align)
                for (i=16; i<n; i++)
                        putbyte(&bp, ' ');

        /* Fill last word with spaces */
        while (bp.p_b != 0)
                putbyte(&bp, ' ');

        reg[14] = 0;
        acc.r = n;
        acc.l = 0;
        return E_SUCCESS;
}

int
e50_15(void)
{
        /*
         * К=15 - спецификация G: преобразование по формату I<N> если М=0,
         * иначе по формату F<N>.<М>, а если не влезаем в размер поля, то
         * по формату Е<N>.<М>. Если задано М>12, число безусловно
         * выдается по Е-формату.
         */
        int src_addr = acc.l & 077777;
        int dst_addr = acc.r & 077777;
        int src_r = (acc.l >> 20) & 15;
        int dst_r = (acc.r >> 20) & 15;
        int n = (acc.l >> 15) & 0x1f;
        int m = (acc.r >> 15) & 0x0f;
        int right_align = (acc.r >> 19) & 1;

        if (src_r)
                src_addr += reg[src_r];
        if (dst_r)
                dst_addr += reg[dst_r];

        printf ("*** e50_15: acc = %08o%08o, src = %05o, dst = %05o, n/m/t = %d/%d/%d\n",
                acc.l, acc.r, src_addr, dst_addr, n, m, right_align);
        //TODO
        return E_UNIMP;
}



int
e50_76(void)
{
        int addr = acc.r & 077777;
        static ptr tp;

        tp.p_w = addr;
        tp.p_b = 0;
        int c;

        while (tp.p_w != 0) {
                c = getbyte(&tp);
                c = toupper(c);

                printf ("%c", (c > 037) ? c : '@');

                if (c == 0 || c == 012)
                        break;
        }
        printf ("\n");
        return (E_SUCCESS);
}

/*
 * Тома, захваченные экстракодом Э50 0157 в монопольное владение.
 * Захват относится к набору данных и держится до конца задачи или до
 * явного освобождения Э50 0116, поэтому отказ от МЛ/МД (Э72 с кодом 020)
 * его не снимает.
 */
static uchar e50_157_locked[NDISKS];

/*
 * Разбор номера тома из младших 16 разрядов acc.r (двоично-десятичный).
 * Возвращает 0 и записывает номер в *diskno либо -1 при ошибке.
 */
static int
bcd_volume(int *diskno)
{
        unsigned bcd = acc.r & 0xffff;

        if (((bcd >> 12) & 0xf) > 9 || ((bcd >> 8) & 0xf) > 9 ||
            ((bcd >> 4) & 0xf) > 9 || (bcd & 0xf) > 9)
                return -1;
        *diskno = ((bcd >> 12) & 0xf) * 1000 + ((bcd >> 8) & 0xf) * 100 +
                ((bcd >> 4) & 0xf) * 10 + (bcd & 0xf);
        if (*diskno < 1 || *diskno > 4095)
                return -1;
        return 0;
}

int
e50_157(void)
{
        /*
         * Э50 0157 — запрос тома в монопольное владение с кодом ответа.
         * Информационное слово в ACC: младшие 16 разрядов (acc.r & 0xffff) —
         * номер тома в двоично-десятичном виде.  Пакетный диспак
         * однопользовательский: конкурирующих задач нет, поэтому захват
         * всегда успешен, если том существует.
         * Ответ: 0 — том захвачен текущей задачей, 1 — ошибка.
         */
        unsigned u;
        int diskno;

        if (bcd_volume(&diskno))
                goto bad;

        /* Том уже привязан к какому-либо логическому каналу? */
        for (u = 030; u < 070; ++u) {
                if (disks[u].diskno == diskno) {
                        e50_157_locked[u] = 1;
                        acc.l = 0;
                        acc.r = 0;
                        return E_SUCCESS;
                }
        }

        /* Ищем свободный логический канал. */
        for (u = 030; u < 070; ++u)
                if (!disks[u].diskh && !disks[u].diskno)
                        break;
        if (u >= 070)
                goto bad;

        if (!(disks[u].diskh = disk_open(diskno, DISK_READ_WRITE)))
                goto bad;
        disk_close(disks[u].diskh);
        disks[u].diskh = 0;
        disks[u].diskno = diskno;
        disks[u].offset = 0;
        disks[u].mode = DISK_READ_WRITE;
        e50_157_locked[u] = 1;

        acc.l = 0;
        acc.r = 0;
        return E_SUCCESS;

bad:
        acc.l = 0;
        acc.r = 1;
        return E_SUCCESS;
}

/*
 * Э50 0116 — освобождение набора данных (отказ от монопольного
 * владения).  Идентификатор НД — тот же BCD-номер тома в младших
 * 16 разрядах acc.r, что и у 0157.  Для незахваченного НД экстракод
 * игнорируется; сам том остаётся привязанным к каналу.
 */
int
e50_116(void)
{
        unsigned u;
        int diskno;

        if (bcd_volume(&diskno))
                return E_SUCCESS;
        for (u = 030; u < 070; ++u)
                if (disks[u].diskno == diskno)
                        e50_157_locked[u] = 0;
        return E_SUCCESS;
}

/*
 * Признак архивного тома (Э50 0166).
 *
 * В ОС ДИСПАК том, занесённый в состав архива, несёт в паспорте
 * признак, защищающий его от прямого обращения экстракодом обмена
 * (руководство системного программиста, п. 8.6.1 "УСТСНЯ"); установка,
 * снятие и выдача признака выполняются экстракодом Э50 0166
 * (ИР 050, АИСП=166(8), И15 = 0/1/2, номер тома в 16-1 разрядах СМ).
 *
 * В паспорте тома (логическая зона 0) признак хранится байтом 4
 * слова 0007: 0 - не архивный, 2 - архивный (значение в реальных
 * томах АРФА 2095/2096).  Экстракод обмена Э70 с томом, имеющим
 * признак, завершается ошибкой E_CWERR.
 */
#define ARFA_PASS_BYTE  (6*0007 + 4)    /* байт признака в зоне 0 */
#define ARFA_PASS_SET   0x02            /* признак установлен */

static unsigned arfa_flag_disk;         /* том, для которого известен признак */
static uchar    arfa_flag_val;
static uchar    arfa_flag_known;

/* Чтение признака архивного тома из паспорта (зона 0).  0 - успех. */
static int
arfa_flag_get (unsigned diskno, unsigned char *flag)
{
        void * disk = disk_open (diskno, DISK_READ_ONLY);
        unsigned char data [ZONE_SIZE];

        if (! disk)
                return -1;
        if (disk_readi (disk, 0, (char*) data, NULL, NULL,
                        DISK_MODE_QUIET) != DISK_IO_OK) {
                disk_close (disk);
                return -1;
        }
        disk_close (disk);
        *flag = data [ARFA_PASS_BYTE];
        return 0;
}

/* Признак установлен?  (с кэшированием последнего чтения) */
static int
arfa_protected (unsigned diskno)
{
        if (! arfa_flag_known || arfa_flag_disk != diskno) {
                if (arfa_flag_get (diskno, &arfa_flag_val))
                        arfa_flag_val = 0;
                arfa_flag_disk = diskno;
                arfa_flag_known = 1;
        }
        return arfa_flag_val == ARFA_PASS_SET;
}

/*
 * Э50 0166 - занесение/уничтожение/выдача признака архивного тома.
 * СМ (16-1 разряды) - номер тома в двоично-десятичном виде;
 * И15: 0 - занести признак, 1 - уничтожить, 2 - выдать.
 * Ответ на запрос (И15=2): СМ = 0 (не архивный) или 1 (архивный).
 */
static int
e50_166 (void)
{
        unsigned char data [ZONE_SIZE], convol [1024];
        int num, op = reg [15] & 3;
        void * disk;

        if (getenv ("BESM6_ARFA_DEBUG"))
                fprintf (stderr, "Э50 0166: И15=%o СМ=%o_%o\n",
                        reg [15], acc.l, acc.r);
        if (bcd_volume (& num))
                return E_CWERR;
        if (op == 2) {                  /* выдача признака */
                unsigned char flag = 0;
                if (! arfa_flag_get (num, &flag))
                        arfa_flag_val = flag, arfa_flag_disk = num,
                        arfa_flag_known = 1;
                acc.l = 0;
                acc.r = (flag == ARFA_PASS_SET) ? 1 : 0;
                return E_SUCCESS;
        }
        /* занести / уничтожить признак: правка паспорта (зона 0) */
        if (! (disk = disk_open (num, DISK_READ_WRITE)))
                return E_SUCCESS;       /* том недоступен - правки нет */
        if (disk_readi (disk, 0, (char*) data, NULL, NULL,
                        DISK_MODE_QUIET) != DISK_IO_OK) {
                disk_close (disk);
                return E_SUCCESS;
        }
        data [ARFA_PASS_BYTE] = (op == 0) ? ARFA_PASS_SET : 0;
        memset (convol, 1, sizeof (convol));    /* слова данных */
        if (disk_writei (disk, 0, (char*) data, (char*) convol, NULL,
                        DISK_MODE_QUIET) != DISK_IO_OK) {
                disk_close (disk);
                return E_INT;
        }
        disk_close (disk);
        arfa_flag_val = data [ARFA_PASS_BYTE];
        arfa_flag_disk = num;
        arfa_flag_known = 1;
        return E_SUCCESS;
}

int
e50(void)
{
        switch (reg[016]) {
        case 0:
                return elfun(EF_SQRT);
        case 1:
                return elfun(EF_SIN);
        case 2:
                return elfun(EF_COS);
        case 3:
                return elfun(EF_ARCTG);
        case 4:
                return elfun(EF_ARCSIN);
        case 5:
                return elfun(EF_ALOG);
        case 6:
                return elfun(EF_EXP);
        case 7:
                return elfun(EF_ENTIER);
        case 014:       /* Text line decoder, OS Dubna specific */
                return e50_12();
        case 015:       /* Binary to string, OS Dubna specific */
                return e50_13();
        case 016:       /* Binary to octal string, OS Dubna specific */
                return e50_14();
        case 017:       /* Binary to decimal/float string, OS Dubna specific */
                return e50_15();
        case 067:       /* DATE*, OS Dubna specific */
                acc.l = 077077774; /* mask */
                acc.r = 0;
                return E_SUCCESS;
        case 074:       /* OS Dubna specific */
                /* Looks like setting up an address to jump on error. */
                return E_SUCCESS;

        case 076:       /* OS Dubna specific */
                /* Term OUTPUT */
                return e50_76();

        case 0100:      /* get account id */
                acc = user;
                return E_SUCCESS;
        case 0101:      /* get error code */
                acc.ml = 0;
                acc.mr = lasterr;
                return E_SUCCESS;
        case 0102:      /* set number of exceptions to catch */
                ninter = (acc.r & 0177) + 1;
                return E_SUCCESS;
        case 0103:      /* set exception handler address */
                intercept = ADDR(acc.r);
                ninter = 1;
                return E_SUCCESS;
        case 0104:
        case 0106:
        case 0107:      /*  get user name from user ID */
            return E_UNIMP;
        case 0105:      /* get volume number by handle */
                acc.l = 0;
                {
                        unsigned        u = (acc.r >> 12) & 077;
                        if (disks[u].diskno)
                                acc.r = to_2_10(disks[u].diskno);
                        else if (!disks[u].diskh)
                                acc.r = 0;
                        else
                                acc.r = 077777;
                }
                return E_SUCCESS;
        case 0112: {    /* set volume offset */
                uint unit = (acc.r >> 12) & 077;
                /* set volume offset in zones for tapes,
                 * in chunks of 040 blocks for disks
                 */
                disks[unit].offset =
                        (SMALL_DISKS() || disks[unit].diskno < 2048 ? acc.r : acc.r << 5) & 07777;
                return E_SUCCESS;
        }
        case 0113: {    /* get current offset */
                uint unit = (acc.r >> 12) & 077;
                acc.l = disks[unit].offset;
                if (disks[unit].diskno >= 2048 && !SMALL_DISKS())
                        acc.l >>= 5;
                acc.r = 0;
                return E_SUCCESS;
        }
        case 0114: {    /* get date */
                time_t t;
                struct tm * d;
                time(&t);
                d = localtime(&t);
                ++d->tm_mon;
                acc.l = (d->tm_mday / 10) << 9 |
                        (d->tm_mday % 10) << 5 |
                        (d->tm_mon / 10) << 4  |
                        (d->tm_mon % 10);
                acc.r = (d->tm_year % 10) << 20 |
                        ((d->tm_year / 10) % 10) << 16 |
                        1;
                return E_SUCCESS;
        }
        case 0115: case 0116:   /* grab/release a volume */
                return E_SUCCESS;
        case 0121:              /* specify volume password */
                return E_SUCCESS;
        case 0127:              /* query presence of passwords */
                acc.r = acc.l = 0;
                return E_SUCCESS;
        case 0130: {            /* read control words of a zone; imitation */
            ushort addr = ADDR(((acc.l >> 6) & 037) * 02000);
                alureg_t t;
                t.l = t.r = 0;
                STORE(t, addr + 2);
                STORE(t, addr + 3);
                STORE(t, addr + 6);
                STORE(t, addr + 7);
                t.l = 01370707;
#define BCDDISK(d)      (((d)/1000) << 12 | ((d)/100%10) << 8 | \
                        ((d)/10%10) << 4  | (d)%10)
                t.r = BCDDISK(disks[(acc.r >> 12) &077].diskno) << 12;
                STORE(t, addr + 1);
                STORE(t, addr + 5);
                t.r = 0;
                t.l = ((acc.r & 07777)+4) << 13;
                STORE(t, addr);
                t.l = ((acc.r & 07777)+4) << 13 | (1 << 12);
                STORE(t, addr+4);
        }       return E_SUCCESS;
        case 0131: {            /* attach volume to handle */
                unsigned        u;

                u = acc.l >> 18;
                if ((((acc.l >> 12) &  077) != 077) | (u < 030) | (u > 067))
                        return E_CWERR;
                acc.l = 0;
                if (disks[u].diskh || disks[u].diskno) {
                        acc.r = 3;
                        return E_SUCCESS;
                }
                disks[u].diskno = NDISK(acc.r);
                disks[u].mode = acc.r & 0x10000 ? DISK_READ_ONLY : DISK_READ_WRITE;
                if (!(disks[u].diskh = disk_open(disks[u].diskno, disks[u].mode))) {
                        acc.r = 1;
                        disks[u].diskno = 0;
                        return E_SUCCESS;
                }
                disk_close(disks[u].diskh);
                disks[u].diskh = 0;
                disks[u].offset = 0;
                acc.r = 0;
                return E_SUCCESS;
        }
        case 0135:      /* get phys. number of a console */
                return E_SUCCESS;
        case 0136:      /* undocumented */
                return E_SUCCESS;
        case 0137:      /* undocumented */
                return E_SUCCESS;

        case 0156: { /* get volume type */
                int i = disks[(acc.r >> 12) & 077].diskno;
                acc.l = 0;
                if (i == 0)
                        acc.r = 077777;
                else if (i >= 2048)
                        /* pretend that the disks are 29.5 Mb */
                        acc.r = 1;
                else
                        /* BESM-6 tape, type code 41 decimal (051 octal) -
                         * as returned by the real ДИСПАК monitor; the golden
                         * GOST program checks tip=41 for "лента типа БЭСМ" */
                        acc.r = 051;
                return E_SUCCESS;
        }
        case 0157:      /* request volume in exclusive ownership, with answer code */
                return e50_157();
        case 0165:      /* BESM owner + version + sys. disk info */
                acc.l = GOST_B << 8 | GOST_EL;
                acc.r = GOST_A << 16 | GOST_DE << 8 | GOST_E;
                accex.l = GOST_EL << 16 | GOST_E << 8 | GOST_TSE;
                accex.r = GOST_REVERSE_E << 16 | GOST_B << 8 | GOST_M;
                reg[016] = 0x222;
                reg[015] = 2053;
                return E_SUCCESS;
        case 0166:      /* признак архивного тома */
                return e50_166 ();
        case 0177:      /* resource request     */
                acc.l = acc.r = 0;
                return E_SUCCESS;
        case 0200:      /* page status  */
                acc.l = acc.r = 0;      /* present */
                return E_SUCCESS;
        case 0202:      /* get error description */
                {
                        uchar           *sp, *dp;
                        unsigned        di;

                        if (acc.r > E_MAX)
                                acc.r = 0;
                        sp = (uchar*) _(errtxt[acc.r]);
                        dp = core[reg[015]].w_b;
                        for (di = 0; di < 18; ++di)
                                *dp++ = *sp ? utf8_to_gost(&sp) : GOST_SPACE;
                }
                return E_SUCCESS;
        case 01200:     /* КУС7Н: опрос статуса УВВ '4023' / УВВ 7 */
                gp_event(reg[016], acc.r, "status");
                gp_status = 1;          /* "готов" */
                acc.r = 0;
                return E_SUCCESS;
        case 01201:     /* подготовка к обмену (Э1240, в K-71 заглушка НОП) */
        case 01202:
        case 01203:
                gp_event(reg[016], acc.r, "prepare");
                return E_SUCCESS;
        case 01204:     /* Э1204: УВВ '147', запись слова на табло */
                gp_event(reg[016], acc.r, "tablo");
                if (niti_debug())
                        fprintf(stderr, "GP: 1204 tablo word=%06o\n", acc.r);
                return E_SUCCESS;
        case 01205:     /* ГРАФИК (Э1205): печать "ВК.Г" на терминал,
                         * НЕ вывод 64 слов на ГП (цикл в .ОБЩ2 исключён) */
                gp_event(reg[016], acc.r, "graph");
                if (niti_debug())
                        fprintf(stderr, "GP: 1205 terminal message 'ВК.Г'\n");
                return E_SUCCESS;
        case 01206: {   /* БОБН (Э1206): запрос архивной бобины,
                         * печать "УСТ.СР.К.N БХХХХ" на терминал */
                void * h;
                unsigned key = acc.r;
                gp_event(reg[016], key, "reel");
                /* Ключ 71717 в симуляции не проверяем. */
                h = disk_open(NDISK(key), DISK_READ_ONLY);
                if (h) {
                        acc.r = 1;
                        disk_close(h);
                } else acc.r = 0;
                if (niti_debug())
                        fprintf(stderr, "GP: 1206 reel volume=%u found=%u\n",
                                NDISK(key), acc.r);
        }        return E_SUCCESS;
        case 01207:     /* ЗАХВ (Э1207): шкала ЗАХВМЛ, захват/освобождение */
                gp_event(reg[016], acc.r, "capture");
                gp_benson = !gp_benson;
                if (niti_debug())
                        fprintf(stderr, "GP: 1207 BENSON %s\n",
                                gp_benson ? "captured" : "released");
                return E_SUCCESS;
        case 01210:     /* NITI: select plotter scheme (ДУДКА via ТРУБКА) */
        case 01216:     /* NITI: ГП НИТИ, вариант К-71 */
        case 01217:     /* NITI: ГП НИТИ, вариант К-71 */
                niti_select(reg[016]);
                return E_SUCCESS;
        case 01211:     /* plotter output (64 words starting from the address on acc) */
               if (reg[3] == 071717 || niti_active) {
                ptr txt;
                txt.p_w = ADDR(acc.r);
                txt.p_b = 0;
                niti_frame(txt, 64);
        } return E_SUCCESS;
        case 01213:     /* B1 test hook: unconditional frame transfer.
                         * Not a real ДИСПАК extracode; the guest calls пв
                         * ФИЗОБМ as a subroutine, invisible to the emulator. */
                dudka_frame_xfer();
                return E_SUCCESS;
        case 01214: {   /* FISOBM hook with the КУС argument in the accumulator.
                         * The guest subroutine пв ФИЗОБМ is not interceptable;
                         * this is a minimal substitute, not a real ЭК. */
                uint64_t kus = ((uint64_t)(acc.l & HALFW) << 24)
                             | (uint64_t)(acc.r & HALFW);
                if (!(kus & DU_KUS_GP27)) {
                        fprintf(stderr,
                                "DUDKA: FISOBM KUS no GP bit, ignored "
                                "(KUS=%012llo)\n",
                                (unsigned long long)kus);
                        return E_SUCCESS;
                }
                fprintf(stderr,
                        "DUDKA: FISOBM KUS bit27 set (KUS=%012llo) "
                        "[test hook, guest пв ФИЗОБМ NOT modeled]\n",
                        (unsigned long long)kus);
                dudka_frame_xfer();
                return E_SUCCESS;
        }
        case 01215:     /* Phase 5 state hook: TV67/TV67A, proxy raise,
                         * WAKETASK, ПРП 19, vector slot (see dudka_irq_ctrl). */
                dudka_irq_ctrl((unsigned)acc.l, (unsigned)acc.r);
                return E_SUCCESS;
        case 01212:     /* discard print stream */
                return E_SUCCESS;
        case 07700:     /* set alarm */
                if (!(acc.r & 0x7fff)) {
                        struct itimerval        itv = {{0, 0}, {0, 0}};
                        setitimer(ITIMER_REAL, &itv, NULL);
                        return E_SUCCESS;
                }
                if (!ehandler || (acc.l != 0xffffff))
                        usleep((acc.r & 0x7fff) * 80000);
                else {
                        struct itimerval        itv = {{0, 0}, {0, 0}};
                        itv.it_value.tv_usec = (acc.r & 0x7fff) * 80000;
                        setitimer(ITIMER_REAL, &itv, NULL);
                }
                return E_SUCCESS;
        case 07701:     /* form new task */
                exform();
                return E_SUCCESS;
        case 07702:     /* where am I? for position-independent code */
                acc.l = 0;
                acc.r = reg[016] = pc;
                return E_SUCCESS;
        default:
                fprintf(stderr, "E50 %04o\n", reg[016]);
                return E_UNIMP;
        }
}

int
eexit(void)
{
    if (intercept_mask & INTERCEPT_E74) {
        return intercept_ex(INTERCEPT_E74);
    }
        if (exitaddr) {
                JMP(exitaddr);
                return E_SUCCESS;
        }
        return E_TERM;
}

int
e62(void)
{
        unsigned        u;
        alureg_t        r;
        int             e;

        switch (reg[016]) {
        case 0:         /* unconditional termination */
                return E_TERM;
        case 0042:      /* flush output stream, but we don't */
                return E_SUCCESS;
        case 0044:      /* cancel output stream, but we don't */
                return E_SUCCESS;
        case 0053:      /* set extracode intercept mask, we feign sucess */
                acc.l = 0;
                acc.r = 077777;
                return E_SUCCESS;
        case 0055:      /* get error catcher address */
                acc.l = 0;
                acc.r = 040000000;
                return E_SUCCESS;
        case 0057:      /* acquire a terminal by number */
                if (dd_debug ())
                        fprintf(stderr, "Acquiring terminal %08o %08o\n", acc.l, acc.r);
                acc.l = 0;
                acc.r = 077777;
                return E_SUCCESS;
        case 0076:      /* drop incognito */
                return E_SUCCESS;
        case 0102:      /* stop reading from terminal */
                return E_SUCCESS;
        case 0103:      /* get logical console number by physical */
                acc.l = acc.r = 0;
                return E_SUCCESS;
        case 0120:      /* undocumented */
                return E_SUCCESS;
        case 0123:      /* enable-disable punching */
                return E_SUCCESS;
        case 0124:      /* as e70 but with control word in ACC (unsafe) */
                LOAD(r, 1);
                STORE(acc, 1);
                reg[016] = 1;
                e = ddio();
                STORE(r, 1);
                return e;
        case 0131:      /* system volume information */
                acc.l = 2;
                acc.r = (5<<8)+3;
                accex = acc;
                return E_SUCCESS;
        default:        /* set volume offset or close volume */
                u = reg[016] >> 9;
                if ((u >= 030) && (u < 070)) {
                        if (!disks[u].diskno)
                                return E_SUCCESS;
                        if ((disks[u].offset = reg[016] & 0777) == 0777) {
                                if (disks[u].diskh)
                                        disk_close(disks[u].diskh);
                                disks[u].diskh = 0;
                                disks[u].diskno = 0;
                        }
                        return E_SUCCESS;
                }
                return E_UNIMP;
        }
}

/*
 * Константа 'КЛЮЧАР' в коде УПП (ГОСТ-10859):
 * К=052 Л=053 Ю=075 Ч=067 А=040 Р=060.
 */
#define ARFA_KEY_L      052053075
#define ARFA_KEY_R      067040060

/* Текст "777777" (отмена ограничения времени) в коде УПП. */
#define E63_CANCEL_L    0007007007
#define E63_CANCEL_R    0007007007

/* Установленное ограничение времени решения (Э63 2 и 3). */
static int    e63_limit_set;
static double e63_limit;

/*
 * Разбор текста "ЧЧММСС" с сумматора (шесть ГОСТ-цифр).
 * Возвращает интервал в секундах или -1 при ошибке.
 */
static int
e63_parse_time (void)
{
        int dig [6], i;
        uint v [2] = { acc.l, acc.r };

        for (i = 0; i < 6; ++i) {
                int byte = i < 3 ? (int) (v [0] >> (16 - 8*i)) :
                        (int) (v [1] >> (16 - 8*(i-3)));
                byte &= 0xff;
                if (byte < GOST_0 || byte > GOST_9)
                        return -1;
                dig [i] = byte - GOST_0;
        }
        if (dig [1] > 5 || dig [3] > 5)
                return -1;
        return ((dig [0] * 10 + dig [1]) * 3600 +
                (dig [2] * 10 + dig [3]) * 60 +
                dig [4] * 10 + dig [5]);
}

/*
 * Установка/смена/снятие ограничения времени решения (Э63 2 и 3).
 * Возбуждение авоста ИСТЕКЛО ВРЕМЯ ПО ЭК не эмулируется.
 */
static void
e63_set_limit (int t, int reserve)
{
        struct timeval ct;
        double now;

        gettimeofday (&ct, NULL);
        now = TIMEDIFF (start_time, ct) - excuse;
        if (t < 0 || (double) t > 3600.0 - now) {
                acc.l = acc.r = 0;         /* превышает оставшееся */
                return;
        }
        e63_limit_set = 1;
        e63_limit = reserve ? 3600.0 - t : now + t;
}

int
arfa(void)
{
    int addr = reg[016];
    alureg_t arg;
    LOAD(arg, addr);
    reg[016] = 0;
    switch (arg.l >> 18) {
    case 016:                   /* admin password confirmation */
        break;
    case 065: {                 /* reading main archive volume */
        void * h;
        int page, zone, r;
        h = disk_open(2248, DISK_READ_ONLY);
        if (!h) {
            if (dd_debug ())
                fprintf(stderr, "No archive volume 2248\n");
            return E_UNIMP;
        }
        page = (arg.l >> 6) & 037;
        zone = arg.r & 07777;
        r = disk_readi(h, zone, (char *)(core + page*02000), (char *)convol + (page*02000), NULL, DISK_MODE_QUIET);
        disk_close(h);
        return r == DISK_IO_OK ? E_SUCCESS : E_DISKERR;
    } break;
    default:
        return E_UNIMP;
    }
    return E_SUCCESS;
}

int
e63(void)
{
        struct timeval  ct;

        /* Архивная система: на СМ - константа 'КЛЮЧАР',
         * в адресной части - адрес информационных слов. */
        if (acc.l == ARFA_KEY_L && acc.r == ARFA_KEY_R)
                return arfa_e63 ();

        switch (reg[016]) {
        case 0: /* время до конца решения задачи в виде float */
                acc.l = 0xd00000;
                acc.r = 3600;
                return E_SUCCESS;
        case 1: {     /* печать времени счета задачи */
                char    buf [80];
                uchar   gbuf [80];
                uchar   *p;
                int     n = 0;
                double  used;
                uint    secs, intr;

                if (! pout_enable)
                        return E_SUCCESS;
                gettimeofday(&ct, NULL);
                used = TIMEDIFF(start_time, ct) - excuse;
                if (used < 0)
                        used = 0;
                /* целые секунды - для детерминированности выдачи */
                secs = (uint) used;
                intr = secs * 50;   /* число прерываний по 20 мс */
                snprintf (buf, sizeof (buf),
                        "ВРЕМЯ СЧЕТА: %02d.%02d.%02d %o",
                        secs / 3600, secs / 60 % 60, secs % 60, intr);
                /* Перекодировка UTF-8 -> ГОСТ и печать. */
                for (p = (uchar*) buf; *p && n < (int) sizeof (gbuf) - 1; )
                        gbuf [n++] = utf8_to_gost (&p);
                gost_write (gbuf, n, stdout);
                putchar ('\n');
                fflush (stdout);
                return E_SUCCESS;
        }
        case 2:       /* ограничение времени решения / его отмена */
                if (acc.l == E63_CANCEL_L && acc.r == E63_CANCEL_R) {
                        e63_limit_set = 0;   /* отмена по "777777" */
                        return E_SUCCESS;
                }
                if (e63_limit_set) {       /* прежнее не отменено */
                        e63_limit_set = 0;
                        return E_SUCCESS;
                }
                e63_set_limit (e63_parse_time (), 0);
                return E_SUCCESS;
        case 3:       /* резервирование времени на обработку авоста */
                if (e63_limit_set)
                        return E_SUCCESS;   /* игнорируется */
                e63_set_limit (e63_parse_time (), 1);
                return E_SUCCESS;
        case 4:
                gettimeofday(&ct, NULL);
                acc.l = 0;
                acc.r = (uint) (TIMEDIFF(start_time, ct) - excuse) * 50;
                return E_SUCCESS;
        default:
                if (reg[016] > 7)
                        return physaddr();
                return E_UNIMP;
        }
}

int parity(int byte)
{
        byte = (byte ^ (byte >> 4)) & 0xf;
        byte = (byte ^ (byte >> 2)) & 0x3;
        byte = (byte ^ (byte >> 1)) & 0x1;
        return !byte;
}

/* get front panel switches */
int
e61(void)
{
        int             i;
        ushort          addr;

        for (addr = reg[016], i = 0; i < 7; ++i, ++addr)
                STORE(zeroword, addr);
        return E_SUCCESS;
}

int
deb(void)
{
        alureg_t        r;

        LOAD(r, reg[016]);
        JMP(ADDR(r.l));
        return E_SUCCESS;
}

void
put_check_words(ushort u, ushort zone, ushort addr, int copy2_req) {
        int i;
        alureg_t t;
        t.l = t.r = 0;
        STORE(t, 010);  /* unit id + date */
        STORE(user, 011); /* writing user id, use current */
        /* volume numbers - assigned and physical */
        t.l = disks[u].diskno << 6;
        t.r = disks[u].diskno;
        STORE(t, 012);
        /* zone numbers and a magic key are duplicated */
        t.l = 070707;
        t.r = (2*zone + 010) << 12 | (2*zone + 010 + copy2_req);
        STORE(t, 013);
        STORE(t, 016);
        /* last word of the zone - tech. legacy reasons */
        LOAD(t, addr + 01777);
        STORE(t, 015);
        t.l = t.r = 0;
        for (i = 0; i < 02000; i++) {
                uint carry;
                alureg_t s;
                LOAD(s, addr + i);
                t.r = (carry = t.r + s.r) & 0xffffff;
                carry >>= 24;
                t.l = (carry = t.l + s.l + carry) & 0xffffff;
                carry >>= 24;
                t.r = (carry = t.r + carry) & 0xffffff;
                carry >>= 24;
                t.l = (carry = t.l + carry) & 0xffffff;
        }
        STORE(t, 017);  /* checksum */
}

/* Объединение двух линий разработки: A2 13.11-18 (развёртка родной зоны УИР)
 * и A2 14.11-26 (восстановление v13-v15 + v19-подмена буфера 76000). */
#define NTV_MAXREC  700
#define NTV_MAXLEN  96

static uchar  ntv_nbuf [6144];   /* слова родной зоны */
static uchar  ntv_ncv  [1024];
static char   ntv_rec  [NTV_MAXREC] [NTV_MAXLEN];
static uchar  ntv_kbuf [6144];   /* конверт: 1024 слова x 6 байт (BE) */
static uchar  ntv_kcv  [1024];
static uchar  ntv_tbuf [6144];   /* пустой лист ЛУН46 */
static uchar  ntv_tcv  [1024];
static int    ntv_active = 0;   /* 1 = конверт k12-формата построен на отдельном тракте */
static int    nat_armed = 0;    /* УИК-arming прошёл (v17) */
static int    nat_enabled = -1; /* BESM6_LUN45_NATIVE кэш */
static int    ntv_tract = -1;   /* BESM6_NTV_TRACT: тракт конверта */
static int    ntv_drum = -1;    /* барабан конверта (= LUN45_DRUM) */
static int    ntv_tract0 = -1;  /* родная raw-зона (= LUN45_TRACT) */

static uint64_t
ntv_gw (const uchar *b)
{
    int i;
    uint64_t v = 0;
    for (i = 0; i < 6; ++i)
        v = (v << 8) | b [i];
    return v;
}

/* v19 (п.18.26): глобальная копия конверта для подмены буфера 76000
 * при входе форматного конвейера МОНА-80 (kcount_pc = 01004). */
uchar           glis1r_ntv_kbuf_glob [6144];
int             glis1r_ntv_kbuf_ready = 0;

void
glis1r_ntv_v19_patch (int abpc)
{
    static int v19_seen = 0;
    int k4, b4;
    if (! glis1r_ntv_kbuf_ready)
        return;
    for (k4 = 0; k4 < 1024; ++k4)
        for (b4 = 0; b4 < 6; ++b4)
            core [076000 + k4].w_b [b4] =
                glis1r_ntv_kbuf_glob [k4 * 6 + b4];
    if (! v19_seen) {
        v19_seen = 1;
        fprintf (stderr, "NTV v19: k12-конверт -> буфер 76000 при "
                 "входе %05o (после МОNREAD*-развёртки)\n", abpc);
    }
}

static void
ntv_pw (uchar *b, uint64_t v)
{
    int i;
    for (i = 5; i >= 0; --i) {
        b [i] = (uchar) (v & 0xFF);
        v >>= 8;
    }
}

/* карта -> 13 слов конверта: 12 слов ISO-72 (по 6 байт) + EOR-слово
 * 0o1002004010020012 (эталон [М1+211], k12/seed-h; п.18.6б - MONCARD
 * ест развёрнутый ISO, не RLE) */
static uchar
ntv_iso (uchar c)
{
    if (c >= 'a' && c <= 'z')
        c -= 32;
    switch (c) {                    /* МОНА-ISO спец-символы (mk_drum_seed) */
    case '!': return 041;
    case '"': return 042;
    case '#': return 023;
    case '$': return 044;
    case '%': return 045;
    case '&': return 046;
    case '\'': return 047;
    case '(': return 050;
    case ')': return 051;
    case '*': return 052;
    case '+': return 053;
    case ',': return 054;
    case '-': return 055;
    case '.': return 056;
    case '/': return 057;
    case ':': return 072;
    case ';': return 073;
    case '<': return 074;
    case '=': return 075;
    case '>': return 076;
    case '?': return 077;
    case '@': return 060;
    }
    return c;                       /* буквы/цифры/пробел = ASCII */
}
static int
ntv_pack_iso72 (const char *text, uint64_t *out, uint64_t eor)
{
    uchar cs [72];
    int i, j, n = strlen (text);
    for (i = 0; i < 72; ++i)
        cs [i] = (i < n) ? ntv_iso ((uchar) text [i]) : 040;
    for (i = 0; i < 12; ++i) {
        uint64_t v = 0;
        for (j = 0; j < 6; ++j)
            v = (v << 8) | cs [i * 6 + j];
        out [i] = v;
    }
    out [12] = eor;    /* WORD_EOR задаётся вызывающей стороной */
    return 13;
}

static int
lun45_native_view (int d45, int t45, int d46, int t46)
{
    const uint64_t mark_end  = (uint64_t) 03 << 45;   /* [11200] */
    const uint64_t mark_cont = (uint64_t) 06 << 45;   /* [11201] */
    const uint64_t word_end  = 0xff000000000aULL;   /* 0o7760000000000012 */
    /* v2 (п.18.19): заголовок k12 (12 слов conv=1, лесенка 0o64..0o24
     * + счётчик 0o22015 - эталон RUNS/run-2026-11-13f/seed-k12.bin,
     * READ OK в TSTREAD) */
    static const uint64_t k12_header [12] = {
        022015ULL,
        (uint64_t) 064 << 42, (uint64_t) 060 << 42,
        (uint64_t) 054 << 42, (uint64_t) 050 << 42,
        (uint64_t) 044 << 42, (uint64_t) 040 << 42,
        (uint64_t) 014 << 42, (uint64_t) 020 << 42,
        (uint64_t) 024 << 42, 0, 0
    };
    const uint64_t filler = 01002004010020040ULL;   /* филлер k8-стиль
     * (mk_seed_k12 FILLER = 0o1002004010020040, ISO-пробелы СИДА,
     * "READ OK в TSTREAD"; v13/v14 использовали 0x202020202020) */
    const uint64_t headkey = (uint64_t) 034 << 42;  /* ключ-вес головы */
    int i, j, nrec = 0, exec_i = -1, endf_i = -1, ncards, pos, bnd;
    uint64_t zone_id;

    if (d45 < 0)
        return 0;
    if (disk_readi (drumh, disks [d45].offset + t45,
                    (char *) ntv_nbuf, (char *) ntv_ncv, NULL,
                    DISK_MODE_QUIET) != DISK_IO_OK)
        return 0;

    /* 1. Декод RLE-записей родной зоны (п.18.4) с границами по словам:
     *    байт 0o200+N = N пробелов, EOR-байт 0o12 инлайн (конец записи,
     *    остаток слова - набивка), контрольные байты 0o240+ и нули в
     *    текст не входят; конец потока = слово 0o7760000000000012. */
    {
        char cur [NTV_MAXLEN];
        int cl = 0;
        for (i = 0; i < 1024; ++i) {
            uint64_t w = ntv_gw (ntv_nbuf + i * 6);
            int eor = 0;
            if (w == word_end)
                break;
            for (j = 0; j < 6; ++j) {
                int b = (int) ((w >> (8 * (5 - j))) & 0xFF);
                if (b == 0012) {                /* EOR - конец записи */
                    eor = 1;
                    break;
                }
                if (b >= 0200 && b <= 0237) {
                    int n = b - 0200;
                    while (n-- > 0 && cl < NTV_MAXLEN - 1)
                        cur [cl++] = ' ';
                } else if (b >= 0040 && b <= 0176) {
                    if (cl < NTV_MAXLEN - 1)
                        cur [cl++] = (char) b;
                }
                /* прочие байты (0, 0o240+) - контрольные, пропускаем */
            }
            if (eor) {
                cur [cl] = 0;
                if (nrec < NTV_MAXREC) {
                    if (cl > 0)
                        strcpy (ntv_rec [nrec], cur);
                    else
                        ntv_rec [nrec][0] = 0;
                    ++nrec;
                }
                cl = 0;
            }
        }
    }
    for (i = 0; i < nrec; ++i) {
        if (exec_i < 0 && ! strncmp (ntv_rec [i], "*EXECUTE", 8))
            exec_i = i;
        else if (exec_i >= 0 && ! strncmp (ntv_rec [i], "*END FILE", 9))
            { endf_i = i; break; }
    }
    if (exec_i < 0 || endf_i < 0) {
        if (dd_debug ())
            fprintf (stderr, "LUN45 NATIVE: данных пока нет "
                     "(записей %d, *EXECUTE@%d)\n", nrec, exec_i);
        return 0;                               /* зона ещё не полна */
    }
    ncards = endf_i - exec_i;                   /* карты + *END FILE */

    /* 2. КОНВЕРТ k12-ФОРМАТА (п.18.19): границы записей 0o12+0o32k,
     *    маркер [11201] на каждой (последняя = [11200], конец листа);
     *    запись i занимает [b_{i-1}+1 .. b_i-1] (0o31 слово): карта =
     *    12 слов ISO-72 + EOR-слово 0o1002004010020012, хвост записи -
     *    филлер ISO-пробелов; conv: заголовок+маркер@0o12 = 1
     *    (CONV_CMD), тело и поздние маркеры+[11200] = 2 (CONV_DATA).
     *    Раскладка = эталон seed-k12.bin (READ OK в TSTREAD, п.18.10);
     *    смерть v1: маркеров на 0o44/0o76 нет -> классификатор листа
     *    (walker 0o13+0o32k, [75441]=тип 1, [75443]=0o21) - "ИСПОРЧЕН
     *    РЕКОРД POINT 00012" на ПЕРВОЙ выборке данных (native13). */
    if (ntv_tract < 0) {
        const char *t = getenv ("BESM6_NTV_TRACT");
        ntv_tract = t ? (int) strtol (t, NULL, 8) : 06;
    }
    memset (ntv_kbuf, 0, sizeof ntv_kbuf);
    memset (ntv_kcv, 0, sizeof ntv_kcv);
    for (i = 0; i < 12; ++i) {
        ntv_pw (ntv_kbuf + i * 6, k12_header [i]);
        ntv_kcv [i] = 1;
    }
    /* v9 (п.18.20): КОНТРАКТ IOXXFMR (ioxxfmr%.asm, ETAP1 п.15.6:
     * "AC - '1' в 48 р. = выход на новый рекорд; '0' - работа с
     * массивом; 41 = проверка на код 012; 42 = закрытие рекорда"):
     * каждая запись конверта = 0o32 слова: [AC-маркер нового рекорда
     * 0o1000000000000000 ('1' в 48-м разряде = эталон-7 классификатора
     * 10633; MTR g1r-mtr20: walker М4 ходит 0o12 -> 0o44 -> ... шаг
     * 0o32, эталоны [М2+6]=0o24<<42 / [М2+7]=0o10<<42)][карта: 12 слов
     * ISO-72 + EOR + филлер до 0o31]. Header как в h: лесенка +
     * [11201]@0o12 + вес@0o13 conv=1. Конец листа: [11200] после
     * последней записи (как mk_seed_k12). */
    if (ntv_tract < 0) {
        const char *t = getenv ("BESM6_NTV_TRACT");
        ntv_tract = t ? (int) strtol (t, NULL, 8) : 06;
    }
    memset (ntv_kbuf, 0, sizeof ntv_kbuf);
    memset (ntv_kcv, 0, sizeof ntv_kcv);
    for (i = 0; i < 12; ++i) {
        ntv_pw (ntv_kbuf + i * 6, k12_header [i]);
        ntv_kcv [i] = 1;
    }
    ntv_pw (ntv_kbuf + 012 * 6, mark_cont);
    ntv_kcv [012] = 1;
    ntv_pw (ntv_kbuf + 013 * 6, headkey);
    ntv_kcv [013] = 1;
    pos = 014;
    /* v13 (п.18.21): ЭТАЛОНЫ 10633 - ЭТО k12-МАРКЕРЫ: aex 6(2) читает
     * [М2+6] = [11172+6] = [11200], aex 7(2) = [11201] (восьмеричная
     * арифметика! п.18.20(г) ошибочно читал [11176]/[11177]).
     * Подтверждение: mtr18-хвост: старьё 0o4024160122640520 ^ 0o3000...
     * = 0o7024160122640520 (digit 4^3=7); вызов (1): [11201]@0o12 ->
     * XOR-7 = 0 -> тип И14=0 (atx AU=0). Walker 10633 ходит границами
     * 0o12+0o32k и ждёт [11201] (лист продолжается) / [11200] (конец).
     * Раскладка = mk_seed_k12-стиль: маркер на 0o12+0o32k, карта
     * (12 слов ISO + EOR) + филлер МЕЖДУ маркерами (с bnd+1).
     * БАГ v10: pos после маркера не сдвигался -> следующая карта
     * затирала маркер -> death на 0o44 при любой раскладке v9-v12. */
    for (i = 0; i < ncards; ++i) {
        uint64_t ws [13];
        bnd = 012 + 032 * (i + 1);              /* граница за записью */
        if (bnd >= 1024)
            return 0;                           /* не умещается */
        ntv_pack_iso72 (ntv_rec [exec_i + 1 + i], ws, 0x20202020200aULL);
        if (pos + 13 > bnd - 1)
            return 0;                           /* карта длиннее записи */
        for (j = 0; j < 13; ++j, ++pos) {
            ntv_pw (ntv_kbuf + pos * 6, ws [j]);
            ntv_kcv [pos] = 2;
        }
        for (; pos < bnd - 1; ++pos) {          /* филлер до EOR */
            ntv_pw (ntv_kbuf + pos * 6, filler);
            ntv_kcv [pos] = 2;
        }
        ntv_pw (ntv_kbuf + bnd * 6,
                (i == ncards - 1) ? mark_end : mark_cont);
        ntv_kcv [bnd] = 2;
        pos = bnd + 1;                          /* v13: следующая запись
                                                 * ЗА маркером (v10 не
                                                 * сдвигал - карты затирали
                                                 * маркеры!) */
    }
    /* v15: СИД k12-стиль (mk_seed_k12.py, "READ OK в TSTREAD"):
     * после последней границы [11200] на границе, ЗА ней - НУЛИ
     * (buf и так обнулён) - дубль mark_end в хвосте УБРАН: МОНА
     * конвейер после [11200] читал слово-дубль как начало новой
     * записи (201-петля v13/v14 - 6 повторов карты 1). */

    /* 3. Запись конверта на ОТДЕЛЬНЫЙ тракт (BESM6_NTV_TRACT, умолч.
     *    06): native-зона МОНЫ НЕ трогается - дек RLE нужен МОНе для
     *    трансляции (перекачка в буфер 76000 и word-pointer [0600] уже
     *    прошли до УИК-проверки). v1 писал конверт на место родной
     *    зоны - дек затирался, контракты обоих читателей не сходились. */
    zone_id = disks [d45].offset + ntv_tract;
    if (disk_writei (drumh, zone_id, (char *) ntv_kbuf, (char *) ntv_kcv,
                     NULL, DISK_MODE_QUIET) != DISK_IO_OK)
        return 0;

    /* 3а. KCOUNT*[0600] := 0o76012 - НАЧАЛО конверта в буфере 76000.
     *     В k46 МОНА ставит указатель на конверт сама (привязка УИР
     *     45/0); в натуральном пути [0600] стоит ЗА деком (0o77146 -
     *     позиция карт в raw-раскладке), а конверт перезаписал буфер
     *     76000 целиком - word-pointer остаётся на нулях -> тип 1.
     *     Word-pointer с 0o12 читает маркеры-записи штатно (k46:
     *     [0600] дошёл до 0o76153 ПОСЛЕ [11200]@0o130, [0607]-[0624] =
     *     слова карты). native7 ломал разбор, записывая [0600] на
     *     КАЖДОМ входе конвейера с мусорной позицией 0o77144 - здесь
     *     ОДИН реарм, позиция = валидное начало конверта. */
    core [0600].w_b [4] = 076;
    core [0600].w_b [5] = 012;
    fprintf (stderr, "NTV: KCOUNT [0600] := 076012 (начало конверта)\n");

    /* 4. Пустой лист на тракт ЛУН46 ([11200]@0o12, conv=1) - конец
     *    очереди для обхода GIVELEXX (как запись 1 сида k12). */
    if (d46 >= 0) {
        memset (ntv_tbuf, 0, sizeof ntv_tbuf);
        memset (ntv_tcv, 0, sizeof ntv_tcv);
        ntv_pw (ntv_tbuf + 012 * 6, mark_end);
        ntv_tcv [012] = 1;
        disk_writei (drumh, disks [d46].offset + t46, (char *) ntv_tbuf,
                     (char *) ntv_tcv, NULL, DISK_MODE_QUIET);
    }

    /* 5. BESM6_LUN45_EXECUTE переустановка (в g1r [0417] обнуляется
     *    ротацией MONCARD на каждой записи; см. также реарм по
     *    BESM6_KCOUNT_ON_PC в cu.c). КCOUNT*[0600] НЕ трогаем -
     *    естественный указатель лексем-потока (g1r-native7). */
    {
        const char *ex = getenv ("BESM6_LUN45_EXECUTE");
        if (ex) {
            ushort ev = (ushort) strtol (ex, NULL, 8);
            core [0417].w_b [4] = 0;
            core [0417].w_b [5] = ev & 0xFF;
            fprintf (stderr, "EXECUTE [0417] := %05o (re-arm)\n", ev);
        }
    }
    fprintf (stderr, "LUN45 NATIVE: зона %02o/%02o -> развёртка "
             "(записей в зоне %d, карт данных %d:",
             d45, t45, nrec, ncards);
    for (i = exec_i + 1; i <= endf_i && i < exec_i + 6; ++i)
        fprintf (stderr, " '%.20s'", ntv_rec [i]);
    fprintf (stderr, ")\n");
    ntv_active = 1;
    return 1;
}

/* v19-совместимая заглушка: в слитой ветке оставлена реализация
 * lun45_native_view() из main (декод родной зоны 20/04). Полная
 * v16-v21-ветка из master здесь не сохранена. */
static int
lun45_native_view_v19 (int drum, int tract)
{
    (void) drum;
    (void) tract;
    return 0;
}

int
ddio(void)
{
        ushort          addr = reg[016], u, zone;
        ushort          sector = 0;
        uinstr_t        uil, uir;
        int             r;
        static uchar    buf[6144];
        static char     cvbuf[1024];
        ushort          mask = getenv("BESM6_SMALL_DISKS") ? 01777 : 07777;
        if (intercept_mask & INTERCEPT_E70) {
            return intercept_ex(INTERCEPT_E70);
        }
        LOAD(acc, addr);
        unpack(addr);
        uil = uicore[addr][0];
        uir = uicore[addr][1];
        cwadj(&uil);
        cwadj(&uir);
        addr = (uil.i_addr & 03700) << 4;   // адрес листа
        u = uir.i_opcode & 077;             // номер диска
        /* Том с признаком архивного закрыт для прямого обмена (Э50 0166).
         * Исключение - лог.номер, к которому Э63 подключил ОБЛАСТЬ архива:
         * это санкционированный АРФОЙ доступ, он разрешён. */
        if (u >= 030 && u < 070 && disks[u].diskno &&
            ! arfa_area_unit (u) &&
            arfa_protected (disks[u].diskno))
                return E_CWERR;
        if ((u < 030) || (u >= 070))
                zone = uir.i_addr & 037;    // тракт барабана
        else
                zone = uir.i_addr & 07777;  // зона диска

        ddisk_t * dd = disks + u;
        if (dd_debug ())
        {   /* отладка этапа 1: каждое УС Э70 с расшифровкой */
            static int dumped45 = 0;
            fprintf (stderr, "DD pc=%05o%c r016=%05o u=%02o z=%04o %s "
                     "uil=%o/%o/%o uir=%o/%o/%o\n",
                     pc, right ? 'R' : 'L', reg[016], u, zone,
                     (uil.i_reg & 8) ? "SEC" : (uil.i_opcode & 010) ? "RD" : "WR",
                     uil.i_opcode, uil.i_reg, uil.i_addr,
                     uir.i_opcode, uir.i_reg, uir.i_addr);
            if (u == 045 && !dumped45) {
                int q;
                dumped45 = 1;
                fprintf (stderr, "--- CW 75500..75520 ---\n");
                for (q = 075500; q < 075520; ++q) {
                    unsigned long long v = 0;
                    int b;
                    for (b = 0; b < 6; ++b)
                        v = (v << 8) | core[q].w_b [b];
                    fprintf (stderr, "CW %05o %016llo\n", q, v);
                }
                fprintf (stderr, "--- CORE 12000..13400 (МОNRЕАD* pc 12203..) ---\n");
                for (q = 012000; q < 013400; ++q) {
                    unsigned long long v = 0;
                    int b;
                    for (b = 0; b < 6; ++b)
                        v = (v << 8) | core[q].w_b [b];
                    fprintf (stderr, "%05o %016llo\n", q, v);
                }
            }
        }
        /* Шаг 2ж (ГЛИССАДА): перенаправление чтения входного потока
         * задачи (монread% идёт по шаблону УИР 45/0 на диск) на
         * барабан-спул МОНЫ, куда та уже положила колоду с картами
         * данных (Шаг 2е: буфер 76000 = спул, ISO-8бит, EOR).
         * BESM6_LUN45_DRUM=барабан (восьм.), BESM6_LUN45_TRACT=тракт. */
        /* А2 13.11-18: взведение nat_armed по предстартовой проверке УИК
         * (М16=75504, УИР 45/0) - ОБЯЗАТЕЛЬНО ДО redirect-блоков: тот
         * меняет u с 0o45 на барабан, и признак u==045 теряется
         * (g1r-native9: блок стоял после redirect - взведение не
         * сработало, конверт-вид не построился). */
        if (nat_enabled < 0)
            nat_enabled = getenv ("BESM6_LUN45_NATIVE") ? 1 : 0;
        if (nat_enabled && reg [016] == 075504 && u == 045 && ! nat_armed) {
            /* v5 (п.18.20): конверт строится и кладётся в буфер 76000
             * СРАЗУ при УИК-проверке (arming): классификатор 10633
             * читает [76000+М4] между arming и первым fetch'ем задачи
             * (MTR g1r-mtr18: 12205 -> 10633 [76012]=старьё -> 00052).
             * Развёртка на death-fetch (v2-v4) опаздывала всегда. */
            static int natd5 = -1, natdone5 = 0, nd455, nt455, nd465, nt465;
            nat_armed = 1;
            if (natd5 < 0) {
                const char *d, *t;
                natd5 = 1;
                d = getenv ("BESM6_LUN45_DRUM");
                t = getenv ("BESM6_LUN45_TRACT");
                nd455 = d ? (int) strtol (d, NULL, 8) : -1;
                nt455 = t ? (int) strtol (t, NULL, 8) : 0;
                d = getenv ("BESM6_LUN46_DRUM");
                t = getenv ("BESM6_LUN46_TRACT");
                nd465 = d ? (int) strtol (d, NULL, 8) : -1;
                nt465 = t ? (int) strtol (t, NULL, 8) : 0;
            }
            if (! natdone5 && lun45_native_view (nd455, nt455, nd465, nt465)) {
                natdone5 = 1;
                /* v7 (п.18.20): САМ Э70 УИК-ПРОВЕРКИ (этот вызов ddio!)
                 * перенаправляется на конверт-тракт: её буфер = 76000
                 * (лексем-область, SHEET-лог: buf=76000), и МОНА-класс
                 * конвейер (10633, между УИК-проверкой и первым fetch'ем)
                 * ходит ИМЕННО по ней. В s6h-эре (h-сид на тракте 04)
                 * УИК-проверка грузила в 76000 КОНВЕРТ штатно; в
                 * натуральном пути она грузила 20/04 = RLE -> старьё
                 * на [76012] -> тип 1 -> 0o21 -> ИСПОРЧЕН POINT 00012. */
                u = nd455;
                zone = ntv_tract > 0 ? ntv_tract : 06;
                dd = disks + u;
                fprintf (stderr, "NTV v7: УИК-проверка -> u=%02o z=%04o "
                         "(конверт в буфер 76000)\n", u, zone);
            }
        } else if (nat_enabled && reg [016] == 075504 && u == 045)
            nat_armed = 1;
        {
            static int l45_drum = -1, l45_tract = -1;
            if (l45_drum < 0) {
                const char *d = getenv ("BESM6_LUN45_DRUM");
                const char *t = getenv ("BESM6_LUN45_TRACT");
                l45_drum  = d ? (int) strtol (d, NULL, 8) : -1;
                l45_tract = t ? (int) strtol (t, NULL, 8) : 0;
            }
            if (l45_drum >= 0 && u == 045) {
                /* Шаг 6 (ГЛИССАДА): подсадка лист-конверта в барабан-спул
                 * МОНЫ - в момент ПЕРВОГО redirect: спулирование МОНЫ
                 * к этому времени завершено, конверт не затирается.
                 * BESM6_DRUM_SEED=файл-заготовка; формат записи:
                 * 1024 слова по 6 байт (BE) + 1024 байта конволюции;
                 * запись k подсаживается на тракт (tract+k) барабана
                 * BESM6_DRUM_SEED_DRUM (восьм., умолч. = LUN45_DRUM). */
                static int seeded = 0;
                if (! seeded) {
                    /* Шаг 6-бис (ГЛИССАДА, план А2 п.11.9-2): позиционировать
                     * указатель входного потока КCOUNT*[0600] на начало
                     * конверта в буфере 76000 - МОНИТОР при старте задачи по
                     * *execute НЕ ставит его на карты данных (п.11.7).
                     * BESM6_LUN45_KCOUNT=адрес (восьм.). */
                    const char *kc = getenv ("BESM6_LUN45_KCOUNT");
                    if (kc) {
                        ushort kv = (ushort) strtol (kc, NULL, 8);
                        core [0600].w_b [4] = (kv >> 8) & 0xFF;
                        core [0600].w_b [5] = kv & 0xFF;
                        fprintf (stderr, "KCOUNT [0600] := %05o\n", kv);
                    }
                    /* А2 14.8-3 (ГЛИССАДА): эмуляция штатной установки
                     * [0417] - 4-го слова характеристики строки MONCARD
                     * (SIMFAIL* paramt%: A, K, сдвиг, A+L - конец строки+1;
                     * буфер 0414-0417, ETAP1 п.14.8-1). РЛС-ротация пишет
                     * веса в 0414/0415/0416 ([0415]:=0o64, [0416]:=0o20),
                     * но стадию для 0417 в данной конфигурации не выполняет,
                     * и контроль IOCONT* 01523-01526 упирается в
                     * ((W & 0o40000) | [0417]) != 0 при [0417]=0.
                     * BESM6_LUN45_EXECUTE=значение (восьм., пишется в
                     * младшие разряды [0417] при ПЕРВОМ redirect u=45;
                     * правдоподобное штатное значение 0o414 = A+L для
                     * MK@0o400, L=12 слов). */
                    {
                        const char *ex = getenv ("BESM6_LUN45_EXECUTE");
                        if (ex) {
                            ushort ev = (ushort) strtol (ex, NULL, 8);
                            core [0417].w_b [4] = 0;
                            core [0417].w_b [5] = ev & 0xFF;
                            fprintf (stderr, "EXECUTE [0417] := %05o\n", ev);
                        }
                    }
                    const char *sf = getenv ("BESM6_DRUM_SEED");
                    seeded = 1;
                    if (sf) {
                        const char *sd = getenv ("BESM6_DRUM_SEED_DRUM");
                        int sdrum = sd ? (int) strtol (sd, NULL, 8) : l45_drum;
                        int k, applied = 0;
                        FILE *fp = fopen (sf, "rb");
                        if (! fp)
                            fprintf (stderr, "DRUM_SEED: cannot open %s\n", sf);
                        else {
                            char sbuf [6144], scvbuf [1024];
                            for (k = 0; ; ++k) {
                                if (fread (sbuf, 1, 6144, fp) != 6144 ||
                                    fread (scvbuf, 1, 1024, fp) != 1024)
                                    break;
                                disk_writei (drumh,
                                        disks [sdrum].offset + l45_tract + k,
                                        sbuf, scvbuf, NULL, DISK_MODE_QUIET);
                                ++applied;
                            }
                            fclose (fp);
                            fprintf (stderr, "DRUM_SEED: %d tracts applied "
                                     "(drum %02o tract %02o+)\n",
                                     applied, sdrum, l45_tract);
                        }
                    }
                }
            /* А2 14.11-26 (v13-v15, восстановление): NATIVE-путь -
             * k12-конверт из колоды на BESM6_NTV_TRACT при первом
             * redirect; УИК-проверка (М16=75504) на конверт-тракт
             * (v7, ETAP1 18.20(б)); остальные чтения u=45 - родная
             * зона l45_tract (дек RLE, данные МОНА берёт 1:1). */
            if (nat_enabled < 0) {
                const char *ne = getenv ("BESM6_LUN45_NATIVE");
                nat_enabled = (ne && *ne && *ne != '0') ? 1 : 0;
            }
            if (nat_enabled && reg [016] == 075504) {
                static int nat_done = 0;
                if (! nat_done) {
                    nat_done = 1;
                    ntv_drum = l45_drum;        /* v17: запомнить
                     * родную raw-зону для подмены data-чтений */
                    ntv_tract0 = l45_tract;
                    lun45_native_view_v19 (l45_drum, l45_tract);
                }
                nat_armed = 1;                  /* v17: arming прошёл */
                if (ntv_tract < 0)
                    ntv_tract = 06;
                u = l45_drum;
                zone = ntv_tract;       /* УИК-проверка -> конверт */
                dd = disks + u;
                fprintf (stderr, "NTV v7: УИК-проверка -> u=%02o "
                         "z=%04o (конверт в буфер 76000)\n", u, zone);
            } else {
                u = l45_drum;
                zone = l45_tract;
                dd = disks + u;     /* dd уже назначен по старому u */
                if (dd_debug ())
                    fprintf (stderr, "LUN45 redirect -> u=%02o z=%04o\n",
                             u, zone);
            }
        }
        }
        /* А2 13.11-10 (ГЛИССАДА, п.18.11(3)): ЛУН46 = следующий лист
         * очереди спула. После исчерпания листа ЛУН45 GIVELEXX (05355)
         * читает Э70 с юнитом 46 (УС 0o0010370000460000, зона 0 ->
         * лист 37000); на реальной системе следующий лист существует
         * (следующее задание очереди), в прогоне k12 юнит 46 не
         * подключён (disks[46].diskno=0) -> E_CWERR "ОШ.В ИНФ.СЛ.ЭКСТ".
         * Подсадка: запись 1 сида k12 уже лежит на тракте
         * BESM6_LUN46_TRACT барабана BESM6_LUN46_DRUM (подсажена
         * DRUM_SEED вместе с конвертом); это пустой лист с маркером
         * конца [11200]@0o12 -> штатный "конец очереди".
         * Side-effects (KCOUNT/EXECUTE/DRUM_SEED) остаются при первом
         * redirect u=45 - здесь только переадресация обмена. */
        {
            static int l46_drum = -1, l46_tract = -1;
            if (l46_drum < 0) {
                const char *d = getenv ("BESM6_LUN46_DRUM");
                const char *t = getenv ("BESM6_LUN46_TRACT");
                l46_drum  = d ? (int) strtol (d, NULL, 8) : -1;
                l46_tract = t ? (int) strtol (t, NULL, 8) : 0;
            }
            /* А2 13.11-18 (ГЛИССАДА, п.18.18(1)): в многолистовом
             * задании (GLIS1R) УИК МОНЫ указывает и ЛУН45, и ЛУН46
             * на ОДНУ зону 20/04 ("зона из УИР; последний писавший
             * выигрывает", 18.17в): печать-Э70 задачи несёт М16=0o46
             * при УИР 20/04, редирект по u=046 не срабатывает, WRITE(6)
             * затирает конверт/поток -> I/O-ERROR 205/232/224 на печати
             * (g1r-native4). Добавка reg[016]==0o46 ТОЛЬКО после
             * построения конверт-вида (ntv_active): в приемном спуле
             * МОНА тоже пишет с М16=0o46 (g1r-native5: разметка уходит
             * на 20/05, трансляция читает мусор) - до старта задачи
             * редирект только по u. */
            if (l46_drum >= 0 && (u == 046 ||
                                  (ntv_active && reg [016] == 046))) {
                u = l46_drum;
                zone = l46_tract;
                dd = disks + u;     /* dd уже назначен по старому u */
                if (dd_debug ())
                    fprintf (stderr, "LUN46 redirect -> u=%02o z=%04o\n",
                             u, zone);
            }
        }
        /* А2 13.11-17 (ГЛИССАДА, п.А3): ЛУНы 0o47-0o57 = следующие
         * листы очереди после 46. Обход GIVELEXX многолистового
         * задания (GLIS1R, 6 модулей) доходит до них и умирает
         * E_CWERR (СТП 00020). BESM6_LUN_TAIL=тракт пустого листа
         * ([11200]@0o12, "конец очереди") на барабане
         * BESM6_LUN46_DRUM: все ЛУНы 0o47-0o57 redirected туда. */
        {
            static int ltail_tract = -2;
            if (ltail_tract == -2) {
                const char *t = getenv ("BESM6_LUN_TAIL");
                ltail_tract = t ? (int) strtol (t, NULL, 8) : -1;
            }
            if (ltail_tract >= 0 && u >= 047 && u <= 057) {
                static int l46d2 = -1;
                int oldu = u;
                if (l46d2 < 0) {
                    const char *d = getenv ("BESM6_LUN46_DRUM");
                    l46d2 = d ? (int) strtol (d, NULL, 8) : 020;
                }
                u = l46d2;
                zone = ltail_tract;
                dd = disks + u;
                if (dd_debug ())
                    fprintf (stderr, "LUN%02o tail redirect -> u=%02o z=%04o\n",
                             oldu, u, zone);
            }
        }
        /* А2 13.11-18 (ГЛИССАДА, п.18.18(1)-(2)): НАТУРАЛЬНЫЙ ПУТЬ -
         * конверт-вид РОДНОЙ зоны из УИР. Диагноз (RUNS/run-2026-11-14/
         * g1r-noseed): МОНА-80 spoolирует входной поток (колода + карты
         * данных после *execute) в зону УИР (ЛУН45 -> 20/04 по УИК
         * супервизора) в СВОЁМ формате: RLE-текст (байт 0o200+N = N
         * пробелов), EOR-байт 0o12 инлайн в последнем слове записи,
         * контрольные байты 0o240+, конец потока 0o7760000000000012.
         * Карты данных лежат в зоне (для GLIS1R: *EXECUTE@0o1142,
         * карта 1 "    3    1    0    5    1"@0o1144, карта 2
         * "   2400.   7351.     1.5     .60"@0o1146, *END FILE@0o1152,
         * конец потока@0o1154 - dd.m20 g1r-noseed), НО читатель конверта
         * (монread%) ждёт КОНВЕРТ-формат: заголовок, маркер [11201]/
         * [11200] на сл.0o12, записи-границы через 0o32 слова (реверс
         * п.18.9в; k12-конверт - READ OK в TSTREAD, п.18.10). На слове
         * 0o12 нативной зоны стоит текст -> класс-контроль -> статус
         * 0o21 -> "ИСПОРЧЕН РЕКОРД ... POINT 00012" (п.9.6). Штатной
         * упаковки данных в конверт нет ни у dispak-МОНА, ни исторически
         * (эталон GLIS1R.LOG: смерть на READ(5), п.18.15д); в drum 21
         * (конверт-члены) данных тоже нет (g1r-native декод, 18.18(2)).
         * BESM6_LUN45_NATIVE=1: при первом Э70 ЛУН45 (М16=0o45, чтение
         * целой зоны), когда зона УИР уже содержит ПОЛНЫЙ набор данных
         (*EXECUTE + карты + *END FILE + конец потока), читаем РОДНУЮ
         * зону (drum LUN45_DRUM, tract LUN45_TRACT - как в УИР),
         * выделяем записи данных после *EXECUTE, перепаковываем их в
         * конверт-формат k12 (заголовок 4023/з0o554 + [11201]@0o12 +
         * записи 0o32: 12 слов ISO-72 + EOR-слово + филлер, [11200] на
         * последней границе) и пишем НА МЕСТО родной зоны; текущее Э70
         * читает уже конверт-вид. Сид НЕ нужен: данные 1:1 из нативной
         * зоны. На тракт ЛУН46 кладётся пустой лист ([11200]@0o12,
         * conv=1) - штатный "конец очереди" для обхода GIVELEXX.
         * BESM6_LUN45_EXECUTE переустанавливается ПОСЛЕ инициализации
         * МОНЫ (в g1r-прогоне [0417] обнуляется при старте задачи). */
        /* А2 13.11-18 (ГЛИССАДА, п.18.18(1)): взведение по предстартовой
         * проверке УИК супервизором: Э70 с М16=75504 (ячейка ЛУН45 в
         * УИК, УИР 45/0) выполняется ТОЛЬКО при старте задачи (*execute)
         * - после этого идет выборка листа входного потока для задачи
         * (М16=0o45). Приемные verify-чтения МОНЫ (М16=0o45 в процессе
         * спулирования) и 121-записное состояние конверт-головы
         * (g1r-native2: сборка на нем портила трансляцию "NONTEXT CARD";
         * g1r-native3/4/6/8: конверт-вид перезаписывался дозаписью дека,
         * цепочка лексем собиралась из RLE-дека -> 201-петля) мимо
         * взведения. */
        if (nat_armed && nat_enabled && reg [016] == 045 && (uil.i_opcode & 010)
            && ! (uil.i_opcode & 4) && ! (uil.i_reg & 8)) {
            static int natd = -1, natdone = 0, nd45, nt45, nd46, nt46;
            if (dd_debug ())
                fprintf (stderr, "NTV try pc=%05o uil=%o/%o/%o reg16=%05o "
                         "armed=%d done=%d\n", pc, uil.i_opcode,
                         uil.i_reg, uil.i_addr, reg [016], nat_armed, natdone);
            if (natd < 0) {
                const char *d, *t;
                natd = 1;
                d = getenv ("BESM6_LUN45_DRUM");
                t = getenv ("BESM6_LUN45_TRACT");
                nd45 = d ? (int) strtol (d, NULL, 8) : -1;
                nt45 = t ? (int) strtol (t, NULL, 8) : 0;
                d = getenv ("BESM6_LUN46_DRUM");
                t = getenv ("BESM6_LUN46_TRACT");
                nd46 = d ? (int) strtol (d, NULL, 8) : -1;
                nt46 = t ? (int) strtol (t, NULL, 8) : 0;
            }
            if (! natdone && lun45_native_view (nd45, nt45, nd46, nt46))
                natdone = 1;
            /* п.18.19: fetch'и задачи (М16=45 после УИК-проверки, чтение
             * целой зоны) идут на КОНВЕРТ-тракт (BESM6_NTV_TRACT):
             * native-зона 20/04 = raw спул МОНЫ, конверт k12-формата
             * построен отдельно - читатель листа (walker маркеров
             * 0o13+0o32k) получает штатный конверт, как в k46. */
            u = nd45;
            zone = ntv_tract;
            dd = disks + u;
            /* п.18.19: буфер 76000 - ЛЕКСЕМ-область МОНЫ: классификатор
             * 10633 классифицирует [76000+М4] СРАЗУ после fetch'а, до
             * того как МОНА-копия успевает разнести конверт из 37000
             * (native16: классификация видела старьё 0o4024... вместо
             * [11201] на 0o12). Fetch кладёт k12-конверт прямо в
             * лексем-буфер - как в k46, где [0600] ходит по 76000+. */
            uil.i_addr = 07600;
            if (dd_debug ())
                fprintf (stderr, "NTV redirect -> u=%02o z=%04o buf=76000\n",
                         u, zone);
        }
        if (uil.i_opcode & 4) {         /* физобмен */
                zone += (u - phdrum) * 040;
                if (phys_dd.diskh) {
                    // The disk LUN associated with the phys. drum has been relinquished.
                    dd = &phys_dd;
                } else {
                    dd = disks + phys_dd.diskno; /* diskno is LUN here */
                }
        }
        if (!dd->diskh) {
            if (!dd->diskno) {
                if (dd_debug ())
                {
                    /* отладка этапа 1: дамп МОNRЕАD* из памяти задачи */
                    int q;
                    fprintf (stderr, "ddio: E_CWERR u=%02o zone=%04o "
                             "uil=%o reg=%o addr=%o / uir=%o addr=%o / "
                             "pc=%05o right=%d r016=%05o\n",
                             u, zone, uil.i_opcode, uil.i_reg, uil.i_addr,
                             uir.i_opcode, uir.i_addr,
                             pc, right, reg[016]);
                    {
                        int s0 = reg[016];
                        int b2;
                        for (b2 = 0; b2 < 4; ++b2) {
                            unsigned long long v = 0;
                            int b;
                            for (b = 0; b < 6; ++b)
                                v = (v << 8) | core[s0 + b2].w_b [b];
                            fprintf (stderr, "CW %05o %012llo\n", s0 + b2, v);
                        }
                    }
                    fprintf (stderr, "--- CORE 12000..13400 (МОNRЕАД* pc 12203..) ---\n");
                    for (q = 012000; q < 013400; ++q) {
                        unsigned long long v = 0;
                        int b;
                        for (b = 0; b < 6; ++b)
                            v = (v << 8) | core[q].w_b [b];
                        fprintf (stderr, "%05o %012llo\n", q, v);
                    }
                    fprintf (stderr, "--- CORE SUP 100000..177777 ---\n");
                    for (q = 0100000; q < 0200000; ++q) {
                        unsigned long long v = 0;
                        int b;
                        for (b = 0; b < 6; ++b)
                            v = (v << 8) | core[q].w_b [b];
                        fprintf (stderr, "%05o %012llo\n", q - 0100000, v);
                    }
                }
                return E_CWERR;
            } else {
                if (!(dd->diskh = disk_open(dd->diskno, dd->mode)))
                        return E_INT;
            }
        }

        /* А2 14.11-18 п.18.19: лог контракта листа при дозагрузке -
         * слово 0o12 буфера 37000 перед/после каждого обмена зоны 20/04
         * (RD = зона->буфер, WR = буфер->зона). */
        if (dd_debug () && u == 020 && zone == 4) {
            unsigned long long w = 0;
            int b;
            for (b = 0; b < 6; ++b)
                w = (w << 8) | core [037012].w_b [b];
            fprintf (stderr, "SHEET %s pc=%05o%c r016=%05o buf=%05o "
                     "buf012=%016llo\n",
                     (uil.i_opcode & 010) ? "RD" : "WR",
                     pc, right ? 'R' : 'L', reg [016],
                     (uil.i_addr & 03700) << 4, w);
        }
        /* v17 (п.18.23, ETAP1 18.22(л)): МОНА-конвейер данных читает
         * raw-зону ПРЯМО (u=барабан 20, z=04, DD pc=00052L r016=00045
         * - минуя ЛУН45-канал и УИК-redirect); после УИК-arming все
         * (ntv_drum, ntv_tract0)-чтения -> конверт-тракт, иначе
         * перекачка raw (Э70 53) кладёт дек-слова поверх конверта в
         * буфере 76000 (двойной источник -> 201-петля x6). Трансляция
         * (до УИК) не затрагивается. BESM6_NTV_ALL=1 включает. */
        if (nat_armed && ntv_drum >= 0 && u == ntv_drum &&
            zone == ntv_tract0) {
            const char *na = getenv ("BESM6_NTV_ALL");
            if (na && *na == '1') {
                zone = ntv_tract;
                fprintf (stderr, "NTV v17: data-чтение (20,%04o) -> "
                         "конверт z=%04o\n", ntv_tract0, ntv_tract);
            }
        }
        {   /* v17-диагностика: первые 200 (u, zone) после arming */
            static int nat_diag = 0;
            if (nat_armed && nat_diag < 200) {
                ++nat_diag;
                fprintf (stderr, "NTV17x: u=%02o z=%04o armed=%d "
                         "drum=%02o t0=%04o\n", u, zone, nat_armed,
                         ntv_drum, ntv_tract0);
            }
        }
        if (uil.i_reg & 8) {                // секторный обмен
               /* согласно ВЗУ и ХЛАМу, 36-й разряд означает, что номер "зоны"
                * есть не номер тракта, а номер сектора (обмен по КУС).
                */
               if (uil.i_addr & 04000) {
                 zone = uir.i_addr & 0177;
                 sector = zone & 3;
                 zone >>= 2;
               } else {
                 sector = (uir.i_addr >> 6) & 3;
               }
                r = disk_readi(dd->diskh,
                        (zone + dd->offset) & 0xfff,
                               (char *)buf, cvbuf, NULL, DISK_MODE_QUIET);
                if (!(uil.i_opcode & 010)) {
                        memcpy(
                                buf + sector * 256 * 6,
                                core + addr + (uil.i_addr & 3) * 256,
                                256 * 6
                        );
                        memcpy(
                               cvbuf + sector * 256,
                               convol + addr + (uil.i_addr & 3) * 256,
                               256
                               );
                        r = disk_writei(dd->diskh,
                                (zone + dd->offset) & 0xfff,
                                        (char *)buf, cvbuf, NULL, DISK_MODE_QUIET);
                }
        } else if (uil.i_opcode & 010) {    // чтение целой зоны
                char cwords[48];
                int iomode = DISK_MODE_QUIET;
                if (uil.i_addr & 04000 && dd->diskno >= 2048) {
                        /* листовой обмен с диском по КУС - физический номер зоны */
                        iomode = DISK_MODE_PHYS;
                }
                r = disk_readi(dd->diskh,
                        (zone + dd->offset) & mask,
                               (char *)(core + addr), (char *)convol + addr, cwords, iomode);
                if (uil.i_opcode & 1 && dd->diskno < 2048) {
                        /* check words requested for tape */
                        put_check_words(u, zone, addr, 0 != (uir.i_opcode & 0200));
                } else if (uil.i_addr & 04000 && dd->diskno >= 2048) {
                        /* copy disk check words to requested page */
                        /* what should happen to the zone data? */
                        memcpy((char*)(core + addr), cwords, 48);
                }
        } else {                            // запись целой зоны
            r = disk_writei(dd->diskh,
                            (zone + dd->offset) & mask,
                            (char *)(core + addr), (char *)convol + addr, NULL, DISK_MODE_QUIET);
        }
        if (dd->diskno) {
                disk_close(dd->diskh);
                dd->diskh = 0;
        }
        if (r != DISK_IO_OK)
                return E_DISKERR;
        if (uil.i_reg & 8 && uil.i_opcode & 010) {
                memcpy(core + addr + (uil.i_addr & 3) * 256,
                       buf + sector * 256 * 6,
                       256 * 6);
                memcpy(convol + addr + (uil.i_addr & 3) * 256,
                       cvbuf + sector * 256,
                       256);
                for (u = addr + (uil.i_addr & 3) * 256;
                     u < addr + (uil.i_addr & 3) * 256 + 256; ++u)
                        cflags[u] &= ~C_UNPACKED;
        } else if (uil.i_opcode & 010) {
                for (u = addr; u < addr + 1024; ++u)
                        cflags[u] &= ~C_UNPACKED;
                if (uil.i_opcode & 1)
                        for (u = 010; u < 020; ++u)
                                cflags[u] &= ~C_UNPACKED;
        }
        return E_SUCCESS;
}

#define E71BUFSZ (324*6)
#define PUTB(c) *dp++ = (c)

int
ttout(uchar flags, ushort a1, ushort a2)
{
        uchar   *sp, *start;
        start = sp = core[a1].w_b;
        if (flags == 0220) {
                /* output to operator's console - first char is channel num */
                putchar(' ');
                ++sp;
        }
        while ((sp - start < E71BUFSZ) && (sp - start < (a2 - a1 + 1) * 6)) {
                if (flags & 1) {
                        if (*sp == 0)
                                break;
                        /* bit 37 means raw I/O, but convert control chars to ANSI escapes */
                        switch (*sp &0x7f) {
                        case '\037': fputs("\033[H\033[J", stdout); break;      // clrscr
                        case '\014': fputs("\033[H", stdout); break;            // home
                        case '\031': fputs("\033[A", stdout); break;            // up
                        case '\032': fputs("\033[B", stdout); break;            // down
                        case '\030': fputs("\033[C", stdout); break;            // right
                        case '\010': fputs("\033[D", stdout); break;            // left
                        default:
                                koi7_putc(*sp & 0x7f, stdout);
                        }
                } else
                switch (*sp) {
                case GOST_END_OF_INFORMATION:
                case GOST_EOF:
                        if (!(flags & 010)) {
                                /* if not a prompt */
                                putchar('\n');
                        }
                        goto done;
                case GOST_CARRIAGE_RETURN:
                        /* maybe should output backslash here ? */
                case GOST_NEWLINE:
                        putchar('\n');
                        break;
                case 0136:
                        putchar('?');
                        break;
                case 0141:
                case 0142:
                        /* zero-width space */
                        usleep (100000);
                        break;
                case 0143:
                        /* ignored */
                        break;
                case 0146:
                case 0170:
                        /* non-destructive backspace */
                        /* assuming ANSI compatibility */
                        fputs("\033[D", stdout);
                        break;
                case 0171:
                        /* move right - assuming ANSI compatibility */
                        fputs("\033[C", stdout);
                        break;
                case 0176:
                        /* move up - assuming ANSI compatibility */
                        fputs("\033[A", stdout);
                        break;
                case 0177:
                        /* move down - assuming ANSI compatibility */
                        fputs("\033[B", stdout);
                        break;
                case 0162:
                        /* erase */
                        fputs("\033[H\033[J", stdout);
                        break;
                case 0167:
                        /* home */
                        fputs("\033[H", stdout);
                        break;
                case 021:
                        if (flags == 0220) {
                                /* up arrow is end of text for op. console */
                                putchar('\n');
                                goto done;
                        }
                        /* fall thru */
                default:
                        if (*sp <= 0134)
                                gost_putc(*sp, stdout);
                        else {
                                printf("[%03o]", *sp);
                        }
                        break;
                }
                fflush(stdout);
                ++sp;
        }
done:
        fflush(stdout);

        if (!(flags & 010))
                (void) eraise(010);
        return E_SUCCESS;
}

int
ttin(uchar flags, ushort a1, ushort a2)
{
        uchar   buf[0324 * 6], *sp, *dp;

        if (flags & 4)          /* non-standard prompt */
                ttout(flags, a1, a2);
        else
                fputs("-\r", stdout);
        fflush(stdout);
        if (! fgets((char*) buf, sizeof(buf), stdin)) {
                if (feof(stdin))
                        return E_TERM;
                perror("stdin");
                return E_INT;
        }
        dp = core[a1].w_b;
        sp = buf;
        while (dp - core[a1].w_b < (a2 - a1 + 1) * 6) {
                if (*sp == '\n') {
                        /* End of line. */
                        PUTB(flags & 1 ? 0 : GOST_EOF);
                        break;
                }
                if (flags & 1) {
                        /* Raw input. */
                        PUTB (utf8_to_koi7(&sp));
                } else
                        PUTB (utf8_to_gost (&sp));
        }
        while ((dp - core[a1].w_b) % 6)
                PUTB(0);
        eraise(4);
        return E_SUCCESS;
}

void punch_braille(FILE * fd, unsigned char * sp, int left) {
    // Print 3 lines of 40 Braille characters per line representing a punchcard.
    unsigned char bytes[3][40];
    int line, col;
    memset(bytes, 0, 120);
    for (line = 0; line < 12; ++line) {
        for (col = 0; col < 80; ++col) {
            int idx = 1 + 12*line + (col>=40) + col/8;
            int bit = idx >= left ? 0 : (sp[idx] >> (7-col%8)) & 1;
            if (bit)
                bytes[line/4][col/2] |= "\x01\x08\x02\x10\x04\x20\x40\x80"[line%4*2+col%2];
        }
    }
    for (line = 0; line < 3; ++line) {
        for (col = 0; col < 40; ++col) {
            fprintf(fd, "\342%c%c", 0240+(bytes[line][col] >> 6), 0200 + (bytes[line][col] & 077));
        }
        putc('\n', fd);
    }
    putc('\n', fd);
}

int punch(ushort a1, ushort a2)
{
        static FILE * fd = 0;
        unsigned char * sp;
        int bytecnt = 0, max;
        if (!punchfile)
                return E_SUCCESS;
        if (!fd) {
                if (!(fd = fopen(punchfile, "w"))) {
                        perror(punchfile);
                        punchfile = 0;
                        return E_UNIMP;
                }
                /* fputs("P4 80 N\n", fd); */
        }
        sp = core[a1].w_b;
        max = (a2 - a1 + 1) * 6;
        if (punch_unicode) {
            while (bytecnt < max) {
                punch_braille(fd, sp, max-bytecnt);
                sp += 144;
                bytecnt += 144;
            }
        } else {
            while (bytecnt < max) {
                if (bytecnt % 6) {
                        if (punch_binary)
                                fputc(*sp, fd);
                        else if (bytecnt % 6) {
#if 1
                                fputc(*sp & 0x80 ? 'O' : '.', fd);
                                fputc(*sp & 0x40 ? 'O' : '.', fd);
                                fputc(*sp & 0x20 ? 'O' : '.', fd);
                                fputc(*sp & 0x10 ? 'O' : '.', fd);
                                fputc(*sp & 0x08 ? 'O' : '.', fd);
                                fputc(*sp & 0x04 ? 'O' : '.', fd);
                                fputc(*sp & 0x02 ? 'O' : '.', fd);
                                fputc(*sp & 0x01 ? 'O' : '.', fd);
#else
                                int curcnt = bytecnt % 144;
                                fprintf(fd, *sp & 0x80 ? "\342\226\213" : "%c", curcnt < 24 ? ' ' : '0' + curcnt/12-2);
                                fprintf(fd, *sp & 0x40 ? "\342\226\213" : "%c", curcnt < 24 ? ' ' : '0' + curcnt/12-2);
                                fprintf(fd, *sp & 0x20 ? "\342\226\213" : "%c", curcnt < 24 ? ' ' : '0' + curcnt/12-2);
                                fprintf(fd, *sp & 0x10 ? "\342\226\213" : "%c", curcnt < 24 ? ' ' : '0' + curcnt/12-2);
                                fprintf(fd, *sp & 0x08 ? "\342\226\213" : "%c", curcnt < 24 ? ' ' : '0' + curcnt/12-2);
                                fprintf(fd, *sp & 0x04 ? "\342\226\213" : "%c", curcnt < 24 ? ' ' : '0' + curcnt/12-2);
                                fprintf(fd, *sp & 0x02 ? "\342\226\213" : "%c", curcnt < 24 ? ' ' : '0' + curcnt/12-2);
                                fprintf(fd, *sp & 0x01 ? "\342\226\213" : "%c", curcnt < 24 ? ' ' : '0' + curcnt/12-2);
#endif
                        }
                }
                sp++;
                bytecnt++;
                if (! punch_binary) {
                        if (bytecnt % 12 == 0)
                                fputc('\n', fd);
                        if (bytecnt % 144 == 0)
                                fputc('\n', fd);
                }
            }
            /* if a partial card was punched, flush it */
            if ((a2 - a1 + 1) % 24 != 0) {
                int remain = 24 - (a2 - a1 + 1) % 24;
                while (remain--) {
                        if (punch_binary)
                                fwrite("\0\0\0\0\0", 5, 1, fd);
                        else {
                                fwrite("........................................", 40, 1, fd);
                                if (remain % 2 == 0)
                                        fputc('\n', fd);
                        }
                }
                if (! punch_binary)
                        fputc('\n', fd);
            }
    }
    return E_SUCCESS;
}

int
term(void)
{
        ushort          addr = reg[016];
        alureg_t        r;
        uinstr_t        uil, uir;
        int             err;

        if(intercept_mask & INTERCEPT_E71) {
            return intercept_ex(INTERCEPT_E71);
        }
        reg[016] = 0;   /* Function key code    */
        if (addr == 0) {
                if (notty)
                        acc.r = acc.l = 0;
                else
                        acc.r = acc.l = 0x004000;
                return E_SUCCESS;
        }
        LOAD(r, addr);
        if ((r.l == 0xffffff && r.r == 0xffffff) ||
            (r.l == 0x040000 && r.r == 0xffffff)) {     /* for POPLAN */
                if (notty)
                        acc.r = acc.l = 0;
                else {
                        acc.l = 0x004000;               /* terminal 12(8) */
                        acc.r = 0;
                }
                return E_SUCCESS;
        }
        for (;;++addr) {
                unpack(addr);
                uil = uicore[addr][0];
                uir = uicore[addr][1];
                cwadj(&uil);
                cwadj(&uir);
                switch (uil.i_opcode & 0360) {
                case 020:       /* i/o */
                        err = (uil.i_opcode & 010 ? ttin : ttout)
                                (uil.i_opcode,
                                        ADDR(reg[uil.i_reg] + uil.i_addr),
                                        ADDR(reg[uir.i_reg] + uir.i_addr));
                        if (err != E_SUCCESS)
                                return err;
                        break;
                case 0120:      /* status/release */
oporos:
                        acc.l = 010000002;
                        acc.r = 012;
                        return E_SUCCESS;
                case 0220:
                        ttout(uil.i_opcode,
                                        ADDR(reg[uil.i_reg] + uil.i_addr),
                                        ADDR(reg[uir.i_reg] + uir.i_addr));
                        return E_SUCCESS;
                default:
                        if (uil.i_opcode == 010) {      /* punchcards */
                                return punch(ADDR(reg[uil.i_reg] + uil.i_addr),
                                 ADDR(reg[uir.i_reg] + uir.i_addr));
                        }
printf ("e71: unknown op %#o\n", uil.i_opcode);
                        return E_UNIMP;
                }
                if (uir.i_opcode & 0100)
                        goto oporos;
        }
}

#define IPZ     062121

int
physaddr(void)
{
        ushort          addr = reg[016];

        switch (addr) {                 /* GUS  */
        case 0:
                acc.l = 046000000;
                acc.r = IPZ;
                break;
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
                LOAD(acc, addr | 0100000);
                break;
        case IPZ + 076:                 /* ??? */
                acc.l = 0;
                acc.r = 0;
                break;
        case IPZ + 077:
                acc = user;
                break;
        case IPZ + 040:
                acc.l = 0;
                acc.r = 077;
                break;
        case IPZ + 071:                 /* общтом */
                acc.l = 0;
                acc.r = 0;
                break;
        case 0221:                      /* GOD */
                acc.l = 0;
                acc.r = 1;
                break;
        case 0322:
                acc.l = 040500000;      /* EXP0 */
                acc.r = 0;
                break;
        case 0476:
                acc.l = 0; acc.r = 077740000; /* МОНИТ */
                break;
        case 0603:
        case 0604:
                acc.l = 077777777; acc.r = 077777777; /* НОММБ, НОММБ1 */
                break;
        case 0500: /* для МС ДУБНА */
        case 0765:
        case 01026:
                acc.l = acc.r = 0;
                break;
        case 0522:
                acc.l = 0777; acc.r = 0; /* E33П25 */
                break;
        case 01413:                     /* ВРЕМЯ */
                reg[016] = 010;
                return e53();
        case 02040:                     /* ПРЕДЕЛ БЭСМ-6 ? */
        case 02100:                     /* ПРЕДЕЛ, 16р = есть архив */
                acc.l = acc.r = 0;
                break;
        default:
                return E_UNIMP;
        }
        return E_SUCCESS;
}

int
resources(void)
{
        ushort          addr = reg[016];
        alureg_t        r;
        uchar           arg[8];
        int             i;

        LOAD(r, addr);
        if ((acc.l == 022642531) && (acc.r == 021233462)) {     /* KEYE72 */
            if ((r.l >> 18) != 07) {
                // The word @EA does not have the proper key,
                // use EA as the address of the intercept routine.
                exitaddr = addr;
            } else {
                // The word @EA has the key, use the mask and the address as indicated.
                intercept_mask = r.r & 03777;
                intercept_addr = r.l & 077777;
            }
            return E_SUCCESS;
        }
        for (i = 0; i < 4; ++i) {
                arg[i] = (r.l >> ((3 - i) * 6)) & 077;
                arg[i + 4] = (r.r >> ((3 - i) * 6)) & 077;
        }
        switch (arg[0]) {
        case 020:                               /* close tape/disk */
                for (i = 1; i < 7; ++i) {
                    int u = arg[i];
                    if (u == 077)
                        break;
                    if (!disks[u].diskno)
                        continue;
                    if (e50_157_locked[u])
                        continue;
                    if (disks[u].diskh)
                        disk_close(disks[u].diskh);

                    if (u == phys_dd.diskno) {
                        phys_dd = disks[u]; /* Using the current disk offset */
                        phys_dd.diskh = disk_open(phys_dd.diskno, phys_dd.mode);
                        // Handle this descriptor as a "drum".
                        phys_dd.diskno = 0;
                    }

                    disks[u].diskno = 0;
                    disks[u].diskh = 0;
                }
                return E_SUCCESS;
        case 047:                               /* free drums/tracks */
        case 040:                               /* free unused tracks */
                return E_SUCCESS;
        case 037:                               /* rename tape/disk */
#define USRC    (arg[i])
#define UDST    (arg[i + 2])
                for (i = 1; i < 7; i += 3) {
                        if (USRC == 077)
                                return E_SUCCESS;
                        if (!disks[USRC].diskno || disks[UDST].diskno)
                                return E_RESOP;
                        disks[UDST].diskno = disks[USRC].diskno;
                        disks[UDST].mode =
                                arg[i + 1] ? DISK_READ_ONLY : DISK_READ_WRITE;
                        disks[UDST].offset = 0;
                        disks[USRC].diskno = 0;
                }
                return E_SUCCESS;
#undef USRC
#undef UDST
        case 030:                               /* exchange tape/disk */
#define USRC    (arg[i])
#define UDST    (arg[i + 2])
                for (i = 1; i < 7; i += 3) {
                        ddisk_t tmp;
                        if (USRC == 077)
                                return E_SUCCESS;
                        if (!disks[USRC].diskno || !disks[UDST].diskno)
                                return E_RESOP;
                        tmp = disks[USRC];
                        disks[USRC] = disks[UDST];
                        disks[UDST] = tmp;
                        disks[UDST].mode =
                                arg[i + 1] & 1 ? DISK_READ_ONLY : DISK_READ_WRITE;
                        disks[USRC].mode =
                                arg[i + 1] & 2 ? DISK_READ_ONLY : DISK_READ_WRITE;
                }
                return E_SUCCESS;
#undef USRC
#undef UDST
        case 010:                               /* exchange RAM pages */
#define USRC    (arg[i])
#define UDST    (arg[i + 1])
                for (i = 1; i < 7; i += 2) {
                        int j;
                        uchar tmp[6144];
                        if (USRC == 077)
                                return E_SUCCESS;

                        if (USRC > 037 || UDST > 037)
                                return E_CWERR;

                        memcpy(tmp, &core[USRC * 02000], 6144);
                        memcpy(&core[USRC * 02000], &core[UDST * 02000], 6144);
                        memcpy(&core[UDST * 02000], tmp, 6144);
                        memcpy(tmp, &convol[USRC * 02000], 1024);
                        memcpy(&convol[USRC * 02000], &convol[UDST * 02000], 1024);
                        memcpy(&convol[UDST * 02000], tmp, 1024);
                        for (j = 0; j < 02000; j++) {
                                cflags[j + USRC * 02000] &= ~C_UNPACKED;
                                cflags[j + UDST * 02000] &= ~C_UNPACKED;
                        }
                }
                return E_SUCCESS;
        case 000:                               /* free RAM pages */
                return E_SUCCESS;
        case 050:                               /* exchange drum tracks */
                return E_UNIMP;
        default:
                return E_UNIMP;
        }
}

extern uchar
eraise(uint newev)
{
        events |= newev & 0xffffff;
        return goahead |= ehandler && eenab && (events & emask);
}

void
alrm_handler(int sig)
{
        (void) signal(SIGALRM, alrm_handler);
        (void) eraise(1);
}

static ptr      txt;

static unsigned
uget(void)
{
        uchar   c;
rpt:
        c = getbyte(&txt);
        switch (c) {
        case GOST_CARRIAGE_RETURN:
        case 0341:
                c = GOST_NEWLINE;
                break;
        case 0143:
                goto rpt;
        case 0342:
                c = GOST_LEFT_QUOTATION;
                break;
        }
        return c;
}

uint64_t
nextw(void)
{
        return getword(&txt);
}

static ushort   diagaddr;

static void
diag(const char *s)
{
        uchar    *cp, *dp;

        if (diagaddr) {
                dp = (uchar*) (core + diagaddr + 1);
                for (cp=(uchar*)s; *cp; )
                        *dp++ = utf8_to_gost (&cp);
                *dp = GOST_END_OF_INFORMATION;
                enreg.l = 0;
                enreg.r = strlen(s) / 6 + 1;
                STORE(enreg, diagaddr);
                fputs(s, stderr);
        } else
                fputs(s, stderr);
}

static char *extptr;

static unsigned
extget(void)
{
        uchar   c;
rpt:
        c = *extptr++;
        switch (c) {
        case GOST_CARRIAGE_RETURN:
        case 0341:
                c = GOST_NEWLINE;
                break;
        case 0143:
                goto rpt;
        case 0342:
                c = GOST_LEFT_QUOTATION;
                break;
        }
        return c;
}

static void
exform(void)
{
        uint64_t        w;
        int             r;
        uchar           c;

        txt.p_w = ADDR(acc.r);
        txt.p_b = 0;

        do {
                c = getbyte(&txt);
                gost_putc(c, stdout);
        } while(c != GOST_TSE);
        putchar('\n');
        txt.p_w = ADDR(acc.r);
        txt.p_b = 0;
        w = getword(&txt);
        diagaddr = 0;
        if ((acc.l & 077700000) == 077200000) {
            // loading from a drum or a disk
            uint64_t myacc = ((uint64_t)acc.l << 24) | acc.r;
            int u = (myacc >> 33) & 077;
            int zone = (myacc >> 21) & 07777;
            int nzon = (myacc >> 15) & 077;
            int startw = reg[015];
            int curzone = 0;
            char *buf = malloc(nzon * 6144);
            diagaddr = ADDR(w);
            do {
                r = disk_readi(disks[u].diskh,
                               (zone + disks[u].offset + curzone) & 0xfff,
                               (char *)buf + curzone * 6144, NULL, NULL, DISK_MODE_QUIET);
                if (r != DISK_IO_OK) {
                    acc.r = 0100077777;
                    acc.l = 0;
                    return;
                }
            } while (++curzone < nzon);
            extptr = buf + startw*6;
            if (diagaddr) {
                r = vsinput(extget, diag, 1);
            } else {
                r = vsinput(extget, diag, 0);
            }
            free(buf);
        } else if ((w & 0xffffff000000ull) == TKH000) {
                diagaddr = ADDR(w);
                r = vsinput(uget, diag, 1);
        } else {
                --txt.p_w;
                r = vsinput(uget, diag, 0);
        }
        if (r < 0) {
                acc.r = -r;
                acc.l = 0;
        } else
                reg[016] = r;
}

int
emu_call(void)
{
        switch (reg[016] ^ 040000) {
        case 0: {               /* phys i/o */
                unsigned        u;
                ushort          zone;
                char            *addr;
                int             r;

                u = (acc.l >> 22 & 7) | (acc.r >> 7 & 7);
                if (u >= OSD_NUM)
                        return E_INT;
                u += OSD_OFF;
                if (!disks[u].diskh)
                        return E_INT;
                if (!(acc.r & 01000000))  /* page/paragraph */
                        return E_INT;
                addr = (char *) (core + (accex.r & 0176000));
                zone = acc.l & 01777;
                r = acc.r & 0400000 ? disk_read(disks[u].diskh, zone, addr)
                                    : disk_write(disks[u].diskh, zone, addr);
                if (r != DISK_IO_OK)
                        return E_DISKERR;
                return E_SUCCESS;
        }
        case 'b':       /* break on first cmd */
                breakflg = acc.r & 1;
                break;
        case 'v':       /* visual on */
                visual = acc.r & 1;
                break;
        case 't':       /* trace on */
                trace = acc.r & 1;
                break;
        case 's':       /* statistics on */
                stats = acc.r & 1;
                break;
        case 'p':
                pout_enable = acc.r & 1;
                break;
        case 'x':       /* native xcodes */
                xnative = acc.r & 1;
                break;
        default:
                return E_INT;
        }
        return E_SUCCESS;
}

#define FUWORD(a)       ((core[a].w_b[2] << 24) | (core[a].w_b[3] << 16) | \
                        (core[a].w_b[4] << 8) | core[a].w_b[5])

int
usyscall(void)
{
        ushort  ap = reg[016];
        int     r;
        uint   ftn, a0, a1, a2;

        ftn = FUWORD(ap); ap = ADDR(ap + 1);
        a0 = FUWORD(ap); ap = ADDR(ap + 1);
        a1 = FUWORD(ap); ap = ADDR(ap + 1);
        a2 = FUWORD(ap);

        switch (ftn) {
        case 0: /* open(path, flags, mode) */
                r = open((char *) core + a0, a1, a2);
                break;
        case 1: /* close(fd) */
                r = close(a0);
                break;
        case 2: /* read(fd, buf, size) */
                r = read(a0, (char *) core + a1, a2);
                break;
        case 3: /* write(fd, buf, size) */
                r = write(a0, (char *) core + a1, a2);
                break;
        case 4: /* lseek(fd, offset, whence) */
                r = lseek(a0, a1, a2);
                break;
        default:
                return E_CWERR;
        }
        acc.l = r >> 24;
        acc.r = r & 0xffffff;
        reg[016] = errno;
        return E_SUCCESS;
}

uint
to_2_10(uint src)
{
        uint   dst = 0;
        int     i;

        for (i = 0; i < 6; ++i) {
                if (!src)
                        break;
                dst |= (src % 10) << (i * 4);
                src /= 10;
        }
        return dst;
}
