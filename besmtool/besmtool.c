/*
 * Create, modify, display BESM-6 disk images.
 * Usage:
 *      besmtool list [<disk-number>]
 *      besmtool erase <disk-number> [<options>...]
 *      besmtool zero <disk-number> [<options>...]
 *      besmtool dump <disk-number> [<options>...] [--to-file=<filename>]
 *      besmtool write <disk-number> [<options>...]
 *
 * Options:
 *      --start=<zone>
 *      --last=<zone>
 *      --length=<nzones>
 *
 * Write options:
 *      --from-file=<filename>
 *      --from-disk=<disknum>
 *      --from-start=<zone>
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
#include <string.h>
#include <getopt.h>
#include "config.h"
#include "besmtool.h"

enum {
        OPT_START,
        OPT_LAST,
        OPT_LENGTH,
        OPT_FROM_FILE,
        OPT_FROM_DISK,
        OPT_FROM_DIR,
        OPT_FROM_START,
        OPT_TO_FILE,
        OPT_ENCODING,
        OPT_LABEL,
        OPT_ATTR,
        OPT_SPECIAL,
};

/* Table of options. */
static struct option longopts[] = {
        /* option            has arg            integer code */
        { "help",               0,      0,      'h'             },
        { "version",            0,      0,      'V'             },
        { "start",              1,      0,      OPT_START       },
        { "last",               1,      0,      OPT_LAST        },
        { "length",             1,      0,      OPT_LENGTH      },
        { "from-file",          1,      0,      OPT_FROM_FILE   },
        { "from-disk",          1,      0,      OPT_FROM_DISK   },
        { "from-dir",           1,      0,      OPT_FROM_DIR    },
        { "from-start",         1,      0,      OPT_FROM_START  },
        { "to-file",            1,      0,      OPT_TO_FILE     },
        { "encoding",           1,      0,      OPT_ENCODING    },
        { "label",              1,      0,      OPT_LABEL       },
        { "attr",               1,      0,      OPT_ATTR        },
        { "special",            0,      0,      OPT_SPECIAL     },
        { 0,                    0,      0,      0               },
};

void
usage ()
{
        fprintf (stderr, "besmtool version %s\n", PACKAGE_VERSION);
        fprintf (stderr, "Handle BESM-6 disk images.\n");
        fprintf (stderr, "\n");

        fprintf (stderr, "Usage:\n");
        fprintf (stderr, "\tbesmtool list [<disk-number>]\n");
        fprintf (stderr, "\tbesmtool pass [<disk-number>]\n");
        fprintf (stderr, "\tbesmtool arfa <disk-number>\n");
        fprintf (stderr, "\tbesmtool arfa-format <disk-number> [--label=<том>]\n");
        fprintf (stderr, "\tbesmtool arfa-create <disk-number> <имя> [--start=<zone>]"
                        " [--length=<nzones>] [--attr=XX] [--special]\n");
        fprintf (stderr, "\tbesmtool arfa-destroy <disk-number> <имя>\n");
        fprintf (stderr, "\tbesmtool search <disk-number> <pattern>\n");
        fprintf (stderr, "\tbesmtool erase <disk-number> [<options>...]\n");
        fprintf (stderr, "\tbesmtool zero <disk-number> [<options>...]\n");
        fprintf (stderr, "\tbesmtool view <disk-number> [<options>...] [--encoding=g,k,t,i]\n");
        fprintf (stderr, "\tbesmtool dump <disk-number> [<options>...] [--to-file=<filename>]\n");
        fprintf (stderr, "\tbesmtool write <disk-number> [<options>...]\n");

        fprintf (stderr, "Options:\n");
        fprintf (stderr, "\t--start=<zone>\n");
        fprintf (stderr, "\t--last=<zone>\n");
        fprintf (stderr, "\t--length=<nzones>\n");

        fprintf (stderr, "View options:\n");
        fprintf (stderr, "\t--encoding=g,k,t,i (default g,k)\n");
        fprintf (stderr, "\t\tg - GOST-10859 encoding\n");
        fprintf (stderr, "\t\tk - KOI-7 encoding\n");
        fprintf (stderr, "\t\tt - 'Text' encoding of Dubna monitoring system\n");
        fprintf (stderr, "\t\ti - encoding of IPMCE autocode by Chaikovsky\n");

        fprintf (stderr, "Write options:\n");
        fprintf (stderr, "\t--from-file=<filename>\n");
        fprintf (stderr, "\t--from-disk=<disknum>\n");
        fprintf (stderr, "\t--from-dir=<dirname>\n");
        fprintf (stderr, "\t--from-start=<zone>\n");

        fprintf (stderr, "Arfa options:\n");
        fprintf (stderr, "\t--label=<том>      метка тома АРФЫ, до 6 символов (по умолчанию 001)\n");
        fprintf (stderr, "\t--attr=XX         атрибут зонового слова, 0x60..0x77 (по умолчанию 0x68)\n");
        fprintf (stderr, "\t--special         спецзапись (без файла данных)\n");
        exit (-1);
}

int
main (int argc, char **argv)
{
        unsigned length = 0, from_diskno = 0, from_start = 0;
        int start = 0, last = -1;
        char *from_file = 0, *to_file = 0, *from_dir = 0, *view_encoding = "g,k";
        char *arfa_label = "001";
        unsigned arfa_attr = 0x68;
        int arfa_special = 0;
        unsigned diskno;
        int c;

        for (;;) {
                c = getopt_long (argc, argv, "hV", longopts, 0);
                if (c < 0)
                        break;
                switch (c) {
                case 'h':
                        usage ();
                        break;
                case 'V':
                        printf ("Version: %s\n", PACKAGE_VERSION);
                        return 0;
                case OPT_START:
                        start = strtol (optarg, 0, 0);
                        break;
                case OPT_LAST:
                        last = strtol (optarg, 0, 0);
                        break;
                case OPT_LENGTH:
                        length = strtol (optarg, 0, 0);
                        break;
                case OPT_FROM_FILE:
                        from_file = optarg;
                        break;
                case OPT_FROM_DISK:
                        from_diskno = strtol (optarg, 0, 0);
                        break;
                case OPT_FROM_DIR:
                        from_dir = optarg;
                        break;
                case OPT_FROM_START:
                        from_start = strtol (optarg, 0, 0);
                        break;
                case OPT_TO_FILE:
                        to_file = optarg;
                        break;
                case OPT_ENCODING:
                        view_encoding = optarg;
                        break;
                case OPT_LABEL:
                        arfa_label = optarg;
                        break;
                case OPT_ATTR:
                        arfa_attr = strtol (optarg, 0, 0);
                        break;
                case OPT_SPECIAL:
                        arfa_special = 1;
                        break;
                }
        }
        argc -= optind;
        argv += optind;
        if (last >= start && ! length)
                length = 1 + last - start;
        switch (argc) {
        case 1:
                if (strcmp ("list", argv[0]) == 0) {
                        list_all_disks ();
                        return 0;
                }
                if (strcmp ("pass", argv[0]) == 0) {
                        passports (2053, start);
                        return 0;
                }
                if (strcmp ("arfa", argv[0]) == 0) {
                        fprintf (stderr, "Укажите номер диска: besmtool arfa <disk-number>\n");
                        return 0;
                }
                if (strcmp ("arfa-format", argv[0]) == 0) {
                        fprintf (stderr, "Укажите номер диска: "
                                "besmtool arfa-format <disk-number> [--label=<том>]\n");
                        return 0;
                }
                break;
        case 2:
                diskno = strtol (argv[1], 0, 0);

                if (strcmp ("list", argv[0]) == 0) {
                        list_disk (diskno);
                        return 0;
                }
                if (strcmp ("pass", argv[0]) == 0) {
                        passports (diskno, start);
                        return 0;
                }
                if (strcmp ("arfa", argv[0]) == 0) {
                        arfa_catalog (diskno);
                        return 0;
                }
                if (strcmp ("arfa-format", argv[0]) == 0) {
                        if (arfa_format (diskno, arfa_label) < 0)
                                return 1;
                        return 0;
                }
                if (strcmp ("erase", argv[0]) == 0) {
                        erase_disk (diskno, start, length, 1);
                        return 0;
                }
                if (strcmp ("zero", argv[0]) == 0) {
                        erase_disk (diskno, start, length, 0);
                        return 0;
                }
                if (strcmp ("view", argv[0]) == 0) {
                        view_disk (diskno, start, length, view_encoding);
                        return 0;
                }
                if (strcmp ("dump", argv[0]) == 0) {
                        if (to_file)
                                disk_to_file (diskno, start, length, to_file);
                        else
                                dump_disk (diskno, start, length);
                        return 0;
                }
                if (strcmp ("check", argv[0]) == 0) {
                        check_disk (diskno, start, length);
                        return 0;
                }
                if (strcmp ("write", argv[0]) == 0) {
                        if (from_file)
                                file_to_disk (diskno, start, length,
                                        from_file, from_start);
                        else if (from_dir)
                                dir_to_disk (diskno, from_dir);
                        else
                                disk_to_disk (diskno, start, length,
                                        from_diskno, from_start);
                        return 0;
                }
                break;
        case 3:
                diskno = strtol (argv[1], 0, 0);

                if (strcmp ("search", argv[0]) == 0) {
                        search_disk (diskno, (unsigned char*) argv[2], start, length);
                        return 0;
                }
                if (strcmp ("arfa-create", argv[0]) == 0) {
                        if (arfa_create (diskno, argv[2], start, length,
                                arfa_attr, arfa_special) < 0)
                                return 1;
                        return 0;
                }
                if (strcmp ("arfa-destroy", argv[0]) == 0) {
                        if (arfa_destroy (diskno, argv[2]) < 0)
                                return 1;
                        return 0;
                }
                break;
        }
        usage ();
        return 0;
}

unsigned long long userid() {
        return 0x419912345678LL;
}
