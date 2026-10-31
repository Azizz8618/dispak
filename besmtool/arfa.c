/*
 * Просмотр тома архива АРФА ОС ДИСПАК.
 *
 * Структура размеченного тома АРФЫ:
 *   зона 0       - паспорт тома ДИСПАК;
 *   зона 1       - паспорт тома АРФА (метка "АRFA", слово 060)
 *                  и каталог архива; копии каталога лежат
 *                  в зонах 07..023 и выбираются по метке;
 *   данные файлов - с зоны, указанной в записи каталога.
 *
 * Запись каталога (слово зоны - через 4 слова после имени):
 *   имя набора (ГОСТ, выровнено влево), атрибуты,
 *   слово зоны: [зона ст.][зона мл.][признаки][0][6X][0],
 *   признаки: бит 0200 - спецзапись (без файла данных).
 *   Номер зоны хранится десятичным, младшим байтом вперёд.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 */
#include <stdio.h>
#include <string.h>
#include "besmtool.h"
#include "disk.h"
#include "encoding.h"
#include "gost10859.h"

/*
 * Латиница Дубны: 7-битный код текстовых файлов АРФЫ.
 * Заглавные латинские буквы, совпадающие по начертанию с русскими,
 * используются как русские; остальные русские буквы - строчными
 * латинскими и спецзнаками: b=Б c=Ц d=Д f=Ф g=Г i=И j=Й l=Л
 * p=П q=Я v=Ж x=Ь y=Ы z=З `=Ю ~=Ч |=Э {=Ш }=Щ s=Ъ.
 * Не отображённые коды (0) печатаются как ASCII.
 */
static const unsigned short latin_to_unicode [128] = {
/* 000-017 */   0,      0,      0,      0,      0,      0,      0,      0,
                0,      0,      '\n',   0,      0,      0,      0,      0,
/* 020-037 */   0,      0,      0,      0,      0,      0,      0,      0,
                0,      0,      0,      0,      0,      0,      0,      0,
/* 040-057 */   ' ',    '!',    '"',    '#',    '$',    '%',    '&',    '\'',
                '(',    ')',    '*',    '+',    ',',    '-',    '.',    '/',
/* 060-077 */   '0',    '1',    '2',    '3',    '4',    '5',    '6',    '7',
                '8',    '9',    ':',    ';',    '<',    '=',    '>',    '?',
/* 100-117 */   '@',    0x0410, 0x0412, 0x0421, 'D',    0x0415, 'F',    'G',
                0x041D, 'I',    'J',    0x041A, 'L',    0x041C, 'N',    0x041E,
/* 120-137 */   0x0420, 'Q',    'R',    'S',    0x0422, 'U',    'V',    'W',
                0x0425, 0x0423, 'Z',    '[',    '\\',   ']',    '^',    '_',
/* 140-157 */   0x042E, 'a',    0x0411, 0x0426, 0x0414, 'e',    0x0424, 0x0413,
                'h',    0x0418, 0x0419, 'k',    0x041B, 'm',    'n',    'o',
/* 160-177 */   0x041F, 0x042F, 'r',    0x042A, 't',    'u',    0x0416, 'w',
                0x042C, 0x042B, 0x0417, 0x0428, 0x042D, 0x0429, 0x0427, 0,
};

/*
 * Печать символа латиницы Дубны.
 * Коды 0201-0337 - серия пробелов (архивное сжатие).
 */
static void
print_latin_char (unsigned char c)
{
        if (c >= 0201) {
                printf ("%*c", c & 0177, ' ');
                return;
        }
        if (latin_to_unicode [c])
                unicode_putc (latin_to_unicode [c], stdout);
        else if (c >= ' ' && c < 0177)
                putchar (c);
}

/*
 * Печать начала текстового файла АРФА (первые строки).
 * Файл может начинаться с бинарной заголовной зоны -
 * просматриваем до 4 первых зон и печатаем первую текстовую.
 */
static void
arfa_peek (void *disk, int zone, int maxlines)
{
        unsigned char data [ZBYTES];
        int z, i, lines, textual;

        for (z = zone; z < zone + 8; ++z) {
                unsigned long good = 0;

                lines = 0;
                if (z <= 0 || disk_read (disk, z, (char*) data) != DISK_IO_OK)
                        break;
                for (i = 0; i < 512; ++i) {
                        unsigned char c = data[i];
                        if (c == '\n' || c >= ' ' || c >= 0201)
                                ++good;
                }
                textual = good > 512 * 9 / 10;
                if (! textual)
                        continue;
                printf ("                    ");
                for (i = 0; i < ZBYTES && lines < maxlines; ++i) {
                        unsigned char c = data[i];
                        if (c == '\n') {
                                if (++lines == maxlines)
                                        break;
                                printf ("\n                    ");
                                continue;
                        }
                        if (c == 0)
                                continue;
                        print_latin_char (c);
                }
                printf ("\n");
                return;
        }
        printf ("                    <бинарные данные>\n");
}

/*
 * Слово похоже на имя набора (текст ГОСТ)?
 */
static int
is_name_word (unsigned char *w)
{
        int i, letters = 0;

        for (i = 0; i < 6; ++i) {
                unsigned char c = w[i];
                if (c == GOST_SPACE)
                        continue;
                if (c < 020 || c > 0137)
                        return 0;
                ++letters;
        }
        return letters > 0;
}

/*
 * Слово зоны каталога: [зона ст.][зона мл.][признаки][0][6X][0],
 * где 6X - байт 0x60..0x77 (атрибут записи).
 */
static int
is_zone_word (unsigned char *w)
{
        return w[3] == 0 && w[4] >= 0x60 && w[4] <= 0x77 &&
                w[5] == 0 && (w[2] & 0x7f) == 0;
}

static void
print_name (unsigned char *w)
{
        int i;

        for (i = 0; i < 6; ++i)
                gost_putc (w[i], stdout);
}

void
arfa_catalog (unsigned diskno)
{
        void *disk;
        unsigned char data [ZBYTES];
        int z, i, copies [16], ncopy = 0, nrec = 0;

        disk = disk_open (diskno, DISK_READ_ONLY);
        if (! disk) {
                fprintf (stderr, "Disk %d: cannot open\n", diskno);
                return;
        }

        /* Ищем копии каталога по метке "АRFA" в слове 060. */
        for (z = 1; z <= 023; ++z) {
                unsigned char *w;
                if (disk_read (disk, z, (char*) data) != DISK_IO_OK)
                        break;
                w = data + 030*6;
                if (w[0] == GOST_A && w[1] == GOST_R &&
                    w[2] == GOST_F && w[3] == GOST_A)
                        copies[ncopy++] = z;
        }
        if (! ncopy) {
                printf ("Диск %d: том АРФА не найден "
                        "(нет метки АRFA в зонах 1..023).\n", diskno);
                disk_close (disk);
                return;
        }

        printf ("Том АРФА на диске %d.\n", diskno);
        printf ("  Метка тома: ");
        disk_read (disk, copies[0], (char*) data);
        print_name (data + 030*6);
        printf (" ");
        print_name (data + 031*6);
        printf ("\n");
        printf ("  Копии каталога: зоны");
        for (i = 0; i < ncopy; ++i)
                printf (" %04o", copies[i]);
        printf ("\n\n");
        printf ("Каталог архива (зона %04o):\n", copies[0]);
        printf ("  имя        зона   содержимое\n");
        printf ("  -----------------------------------------\n");

        disk_read (disk, copies[0], (char*) data);
        for (i = 4; i < 1020; ++i) {
                unsigned char *zw = data + i*6;
                int zone, special;

                if (! is_zone_word (zw))
                        continue;
                if (! is_name_word (data + (i-4)*6))
                        continue;
                zone = (zw[0] << 8) | zw[1];
                special = zw[2] & 0200;
                ++nrec;
                printf ("  ");
                if (data[(i-4)*6+5] != GOST_SPACE &&
                    is_name_word (data + (i-5)*6))
                        print_name (data + (i-5)*6);
                print_name (data + (i-4)*6);
                printf ("  %6o(%d)", zone, zone);
                if (special) {
                        printf ("  спецзапись\n");
                } else {
                        printf ("\n");
                        arfa_peek (disk, zone, 3);
                }
        }
        printf ("\nВсего записей: %d\n", nrec);
        disk_close (disk);
}
