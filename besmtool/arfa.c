/*
 * Просмотр и изменение тома архива АРФА ОС ДИСПАК.
 *
 * Структура размеченного тома АРФЫ (по образцу реального тома 2095):
 *   зона 0       - паспорт тома ДИСПАК (не изменяется);
 *   зона 1       - паспорт тома АРФА (метка "АRFA", слово 030)
 *                  и каталог архива; копии каталога лежат
 *                  в зонах 011, 017, 020, 023 и выбираются по метке;
 *   данные файлов - с зоны, указанной в записи каталога.
 *
 * Заголовок зоны каталога - слова 0..2, метка тома - слова 030..031,
 * слова 032 и 035 - заполнители (99 99 99 00 00 00).
 * Записи каталога начинаются со слова 0100. Запись - 12 слов данных
 * и нулевой разделитель:
 *   +0, +1 - имя набора (ГОСТ, выровнено влево, до 12 символов,
 *            хвост заполняется пробелами 017 и нулями);
 *   +2     - атрибуты (99 99 99 00 40 00);
 *   +3     - число зон области в байтах 4..5, старшим вперёд
 *            (проверено на томе 2095: ИЛЬИН - 192 зоны, 122+192=314,
 *            следующая область SIРRОD начинается с зоны 314);
 *   +4     - зоновое слово [зона ст.][зона мл.][признаки][0][6X][0];
 *            номер зоны - старшим байтом вперёд; бит 0200 признаков -
 *            спецзапись (без файла данных); 6X - атрибут записи;
 *   +5, +6 - атрибуты (99 99 99 00 08 2f и 41 03 00 00 00 00);
 *   +7..11 - даты (формат не расшифрован, при создании - нули).
 * Уничтоженная запись обнуляется; новые записи дописываются
 * в конец каталога - после последней записи.
 *
 * Слова зоны каталога записываются со свёрткой "данные" (2),
 * как в реальном томе.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "besmtool.h"
#include "disk.h"
#include "encoding.h"
#include "gost10859.h"

/* Слова зоны каталога */
#define ARFA_LBL_WORD   0030    /* метка "АRFA" */
#define ARFA_VOL_WORD   0031    /* номер тома (хвост метки) */
#define ARFA_FIRST_REC  0100    /* первое слово записей каталога */
#define ARFA_REC_WORDS  13      /* слот записи: 12 слов + разделитель */

/* Копии каталога: основная зона 1 и копии, как в реальных томах */
static const int arfa_copy_zones [] = { 1, 011, 017, 020, 023 };
#define ARFA_NCOPIES (sizeof (arfa_copy_zones) / sizeof (arfa_copy_zones [0]))

/* Признак архивного тома (см. arfa_flag_cmd ниже) */
#define ARFA_PASS_BYTE  (6*0007 + 4)
#define ARFA_PASS_SET   0x02
static int      arfa_passport_flag (unsigned diskno, unsigned char *flag);

/* Данные областей не размещаются раньше конца копий каталога */
#define ARFA_DATA_MIN  0030

/* Атрибут зонового слова по умолчанию (как у большинства записей 2095) */
#define ARFA_ATTR_DEFAULT 0x68

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
 * Файл может начинаться с бинарной заголовочной зоны -
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
 * Байт допустим в имени набора: цифра (0..011), пробел (017),
 * буква или знак (020..0137)?
 */
static int
name_char_ok (unsigned char c)
{
        return c <= 011 || c == GOST_SPACE ||
                (c >= 020 && c <= 0137);
}

/*
 * Слово похоже на имя набора: все байты допустимы и есть
 * хотя бы одна буква или знак (цифровые и нулевые слова
 * именами не считаются).
 */
static int
is_name_word (unsigned char *w)
{
        int i, letters = 0;

        for (i = 0; i < 6; ++i) {
                unsigned char c = w[i];
                if (c == GOST_SPACE)
                        continue;
                if (! name_char_ok (c))
                        return 0;
                if (c >= 020 && c <= 0137)
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

/*
 * Запись каталога АРФЫ.
 */
typedef struct {
        int             name_word;      /* слово имени (начало записи) */
        int             zone_word;      /* слово зоны (имя + 4) */
        int             zone;           /* начальная зона области */
        int             nzones;         /* число зон области */
        int             special;        /* спецзапись (бит 0200) */
        unsigned char   name [12];      /* имя набора */
} arfa_rec_t;

/*
 * Поиск копий каталога по метке "АRFA" в слове 030 зон 1..023.
 * Возвращает число найденных копий.
 */
static int
arfa_find_copies (void *disk, int copies [], int maxcopies)
{
        unsigned char data [ZBYTES];
        int z, ncopy = 0;

        for (z = 1; z <= 023; ++z) {
                unsigned char *w;
                if (disk_read (disk, z, (char*) data) != DISK_IO_OK)
                        break;
                w = data + ARFA_LBL_WORD*6;
                if (w[0] == GOST_A && w[1] == GOST_R &&
                    w[2] == GOST_F && w[3] == GOST_A)
                        if (ncopy < maxcopies)
                                copies [ncopy++] = z;
        }
        return ncopy;
}

/*
 * Сбор записей каталога из зоны data.
 * Имя - слова (i-4) и (i-3), зоновое слово - i.
 * Возвращает число записей (не более maxrecs).
 */
static int
arfa_scan_records (unsigned char *data, arfa_rec_t recs [], int maxrecs)
{
        int i, nrec = 0;

        for (i = 4; i < 1020; ++i) {
                unsigned char *zw = data + i*6;
                arfa_rec_t *r;

                if (! is_zone_word (zw))
                        continue;
                if (! is_name_word (data + (i-4)*6))
                        continue;
                if (nrec >= maxrecs)
                        break;
                r = &recs [nrec];
                r->name_word = i - 4;
                r->zone_word = i;
                r->zone = (zw [0] << 8) | zw [1];
                r->nzones = (data [(i-1)*6 + 4] << 8) | data [(i-1)*6 + 5];
                r->special = zw [2] & 0200;
                memcpy (r->name, data + (i-4)*6, 6);
                memcpy (r->name + 6, data + (i-3)*6, 6);
                ++nrec;
        }
        return nrec;
}

/*
 * Запись зоны каталога со свёрткой "данные" (как в реальном томе).
 */
static int
arfa_write_zone (void *disk, int zone, unsigned char *data)
{
        char convol [1024];

        memset (convol, 1, sizeof (convol));
        return disk_writei (disk, zone, (char*) data, convol, NULL,
                DISK_MODE_QUIET);
}

/*
 * Длина имени без хвостовых пробелов и нулей-заполнителей.
 */
static int
arfa_name_len (unsigned char *name)
{
        int i, n = 0;

        for (i = 0; i < 12; ++i)
                if (name [i] != GOST_SPACE && name [i] != 0)
                        n = i + 1;
        return n;
}

/*
 * Перекодировка имени из командной строки (UTF-8) в ГОСТ.
 * До 12 символов, выравнивание влево, хвост - пробелы 017.
 * Возвращает 0 при успехе.
 */
static int
arfa_name_to_gost (const char *utf8, unsigned char gost [12])
{
        unsigned char *p = (unsigned char*) utf8;
        int n = 0;

        memset (gost, GOST_SPACE, 12);
        while (*p) {
                unsigned char g;
                if (n >= 12) {
                        fprintf (stderr, "Имя '%s' длиннее 12 символов\n", utf8);
                        return -1;
                }
                g = utf8_to_gost (&p);
                if (! name_char_ok (g)) {
                        fprintf (stderr, "Символ без кода ГОСТ в имени '%s'\n",
                                utf8);
                        return -1;
                }
                gost [n++] = g;
        }
        if (! n) {
                fprintf (stderr, "Пустое имя области\n");
                return -1;
        }
        return 0;
}

/*
 * Печать имени ГОСТ в UTF-8 (без хвостовых пробелов и нулей).
 */
static void
arfa_print_name (unsigned char *name, FILE *fout)
{
        int i, n = arfa_name_len (name);

        for (i = 0; i < n; ++i)
                gost_putc (name [i], fout);
}

/*
 * Сравнение имени из командной строки с именем записи
 * (хвостовые пробелы и нули не учитываются).
 */
static int
arfa_name_equal (unsigned char *name, unsigned char *gost)
{
        int i;

        for (i = 0; i < 12; ++i) {
                unsigned char a = name [i], b = gost [i];
                if (a == 0)
                        a = GOST_SPACE;
                if (b == 0)
                        b = GOST_SPACE;
                if (a != b)
                        return 0;
                if (a == GOST_SPACE && i >= arfa_name_len (name) &&
                    i >= arfa_name_len (gost))
                        break;
        }
        return 1;
}

/*
 * Просмотр тома: паспорт и каталог архива.
 */
void
arfa_catalog (unsigned diskno)
{
        void *disk;
        unsigned char data [ZBYTES];
        int i, copies [16], ncopy, nrec;
        arfa_rec_t recs [128];

        disk = disk_open (diskno, DISK_READ_ONLY);
        if (! disk) {
                fprintf (stderr, "Disk %d: cannot open\n", diskno);
                return;
        }

        ncopy = arfa_find_copies (disk, copies, 16);
        if (! ncopy) {
                printf ("Диск %d: том АРФА не найден "
                        "(нет метки АRFA в зонах 1..023).\n", diskno);
                disk_close (disk);
                return;
        }

        printf ("Том АРФА на диске %d.\n", diskno);
        printf ("  Метка тома: ");
        disk_read (disk, copies[0], (char*) data);
        print_name (data + ARFA_LBL_WORD*6);
        printf (" ");
        print_name (data + ARFA_VOL_WORD*6);
        printf ("\n");
        {
                unsigned char flag = 0;
                if (! arfa_passport_flag (diskno, &flag) &&
                    flag == ARFA_PASS_SET)
                        printf ("  Признак архивного тома: ЕСТЬ "
                                "(том закрыт для СЕРПа и Э70)\n");
        }
        printf ("  Копии каталога: зоны");
        for (i = 0; i < ncopy; ++i)
                printf (" %04o", copies[i]);
        printf ("\n\n");
        printf ("Каталог архива (зона %04o):\n", copies[0]);
        printf ("  имя          зона  длн  содержимое\n");
        printf ("  -------------------------------------------\n");

        nrec = arfa_scan_records (data, recs, 128);
        for (i = 0; i < nrec; ++i) {
                printf ("  ");
                arfa_print_name (recs [i].name, stdout);
                printf (" %6o(%d)", recs [i].zone, recs [i].zone);
                if (recs [i].special)
                        printf ("  спецзапись\n");
                else {
                        printf (" %4d\n", recs [i].nzones);
                        arfa_peek (disk, recs [i].zone, 3);
                }
        }
        printf ("\nВсего записей: %d\n", nrec);
        disk_close (disk);
}

/*
 * Признак архивного тома (Э50 0166) хранится в паспорте тома
 * (логическая зона 0), байт 4 слова 0007: 0 - не архивный,
 * 2 - архивный. Том с признаком закрыт для прямого обращения
 * экстракодом обмена (Э70), что делает его невидимым для СЕРПа.
 */

/* Чтение признака архивного тома из паспорта. 0 - успех. */
static int
arfa_passport_flag (unsigned diskno, unsigned char *flag)
{
        void *disk = disk_open (diskno, DISK_READ_ONLY);
        unsigned char data [ZBYTES];

        if (! disk)
                return -1;
        if (disk_read (disk, 0, (char*) data) != DISK_IO_OK) {
                disk_close (disk);
                return -1;
        }
        disk_close (disk);
        *flag = data [ARFA_PASS_BYTE];
        return 0;
}

/*
 * Установка/снятие/выдача признака архивного тома
 * (аналог Э50 0166 и программы УСТСНЯ).
 * action: NULL или "" - выдать, "on" - занести, "off" - уничтожить.
 * Возвращает 0 при успехе, -1 при ошибке; для запроса - значение признака.
 */
int
arfa_flag_cmd (unsigned diskno, const char *action)
{
        void *disk;
        unsigned char data [ZBYTES], convol [1024];
        unsigned char flag;

        if (! action || ! *action || strcmp (action, "query") == 0) {
                if (arfa_passport_flag (diskno, &flag)) {
                        fprintf (stderr, "Диск %d: паспорт не читается\n",
                                diskno);
                        return -1;
                }
                printf ("Диск %d: признак архивного тома: %s\n", diskno,
                        flag == ARFA_PASS_SET ? "ЕСТЬ" : "нет");
                return flag == ARFA_PASS_SET;
        }
        if (strcmp (action, "on") && strcmp (action, "off")) {
                fprintf (stderr, "besmtool arfa-flag: действие '%s' "
                        "неизвестно (on|off|query)\n", action);
                return -1;
        }
        disk = disk_open (diskno, DISK_READ_WRITE);
        if (! disk) {
                fprintf (stderr, "Диск %d: не открывается на запись\n", diskno);
                return -1;
        }
        if (disk_read (disk, 0, (char*) data) != DISK_IO_OK) {
                fprintf (stderr, "Диск %d: зона 0 не читается\n", diskno);
                disk_close (disk);
                return -1;
        }
        data [ARFA_PASS_BYTE] = strcmp (action, "on") == 0 ?
                ARFA_PASS_SET : 0;
        memset (convol, 1, sizeof (convol));    /* слова данных */
        if (disk_writei (disk, 0, (char*) data, (char*) convol, NULL,
                        DISK_MODE_QUIET) != DISK_IO_OK) {
                fprintf (stderr, "Диск %d: зона 0 не записывается\n", diskno);
                disk_close (disk);
                return -1;
        }
        disk_close (disk);
        printf ("Диск %d: признак архивного тома %s.\n", diskno,
                strcmp (action, "on") == 0 ? "установлен (том закрыт для СЕРПа)" :
                "снят (том открыт для обмена)");
        return 0;
}

/*
 * Паспорт тома (зона 0) - по образцу реальных томов АРФА 2095/2096.
 * Номер тома записывается в двоично-десятичном виде (слова 0017, 0020,
 * 0023). Байт 4 слова 0007 - признак архивного тома (см. выше).
 */
static void
arfa_build_passport (unsigned char *data, unsigned diskno)
{
        unsigned bcd12 = (unsigned) (diskno / 1000 % 10) << 4 | diskno / 100 % 10;
        unsigned bcd34 = (unsigned) (diskno / 10 % 10) << 4 | diskno % 10;

        memset (data, 0, ZBYTES);
        data [6*0000+3] = 0x08;                 /* код паспорта АРФЫ */
        data [6*0000+4] = 0xaa; data [6*0000+5] = 0xa0;
        data [6*0002+4] = 0x09; data [6*0002+5] = 0xaa;
        data [6*0003+1] = 0x01;
        data [6*0004+2] = 0xfa;
        data [6*0006+0] = data [6*0006+1] = data [6*0006+2] = 0x99;
        data [6*0006+5] = 0x99;
        data [6*0007+2] = 0xee;
        data [ARFA_PASS_BYTE] = ARFA_PASS_SET;  /* признак архивного тома */
        /* свёртка паспорта - значение реального тома (не пересчитывается) */
        data [6*0010+0] = 0x05; data [6*0010+1] = 0x7e; data [6*0010+2] = 0x40;
        data [6*0010+3] = 0x01; data [6*0010+4] = 0x5f; data [6*0010+5] = 0x90;
        data [6*0016+5] = 0x04;
        data [6*0017+0] = bcd12; data [6*0017+1] = bcd34;
        data [6*0017+2] = bcd12; data [6*0017+3] = bcd34;
        data [6*0017+5] = 0x03;
        data [6*0020+1] = 0x0f;
        data [6*0020+4] = bcd12; data [6*0020+5] = bcd34;
        data [6*0023+0] = bcd12;
        data [6*0030+0] = 0x80;
}

/*
 * Разметка тома АРФЫ: паспорт (зона 0) и пустой каталог в зонах
 * 1, 011, 017, 020, 023. Паспорт записывается, только если зона 0
 * пуста (том новый); том с данными в зоне 0 паспорта не получает.
 * Уже размеченный том повторно не размечается. Возвращает 0 при успехе.
 */
int
arfa_format (unsigned diskno, const char *volabel)
{
        void *disk;
        unsigned char data [ZBYTES], gost [6], pass [ZBYTES];
        unsigned char *w, *p;
        int z, n, ncopy, copies [16], write_pass;

        disk = disk_open (diskno, DISK_CREATE);
        if (! disk) {
                fprintf (stderr, "Диск %d: не открывается\n", diskno);
                return -1;
        }

        ncopy = arfa_find_copies (disk, copies, 16);
        if (ncopy) {
                fprintf (stderr, "Диск %d: том АРФА уже размечен "
                        "(копии каталога в зонах", diskno);
                for (z = 0; z < ncopy; ++z)
                        fprintf (stderr, " %04o", copies [z]);
                fprintf (stderr, "). Повторная разметка запрещена;\n"
                        "обнулите зоны каталога командой "
                        "'besmtool zero %d --start=1 --length=023'.\n", diskno);
                disk_close (disk);
                return -1;
        }

        /* Паспорт тома пишем только на пустую зону 0 (нули или
         * несуществующая зона; LOUD вернёт DISK_IO_NEW). */
        write_pass = 1;
        z = disk_readi (disk, 0, (char*) pass, NULL, NULL, DISK_MODE_LOUD);
        if (z == DISK_IO_OK) {
                for (z = 0; z < ZBYTES; ++z)
                        if (pass [z]) {
                                write_pass = 0;
                                break;
                        }
        } else if (z != DISK_IO_NEW) {
                fprintf (stderr, "Диск %d: зона 0 не читается\n", diskno);
                disk_close (disk);
                return -1;
        }

        /* Метка тома: хвост до 6 символов, прижатый вправо. */
        memset (gost, GOST_SPACE, 6);
        p = (unsigned char*) volabel;
        n = 0;
        while (*p) {
                unsigned char g;
                if (n >= 6) {
                        fprintf (stderr, "Метка '%s' длиннее 6 символов\n",
                                volabel);
                        disk_close (disk);
                        return -1;
                }
                g = utf8_to_gost (&p);
                if (g > 0137) {
                        fprintf (stderr, "Символ без кода ГОСТ в метке '%s'\n",
                                volabel);
                        disk_close (disk);
                        return -1;
                }
                gost [n++] = g;
        }
        if (n < 6) {
                memmove (gost + (6 - n), gost, n);
                memset (gost, GOST_SPACE, 6 - n);
        }

        memset (data, 0, sizeof (data));

        /* Заголовок зоны каталога - как в реальном томе 2095. */
        w = data;                  /* слово 0 */
        w[3] = 0x09; w[4] = 0xaa; w[5] = 0xa1;
        w = data + 6;              /* слово 1 */
        w[5] = 0x08;
        w = data + 12;             /* слово 2 */
        w[4] = 0xff; w[5] = 0xff;

        /* Метка тома: "АRFA" + номер тома. */
        w = data + ARFA_LBL_WORD*6;
        w[0] = GOST_A; w[1] = GOST_R; w[2] = GOST_F; w[3] = GOST_A;
        w[4] = GOST_SPACE; w[5] = GOST_SPACE;
        memcpy (data + ARFA_VOL_WORD*6, gost, 6);

        /* Заполнители. */
        w = data + 032*6;  w[0] = w[1] = w[2] = 0x99;
        w = data + 035*6;  w[0] = w[1] = w[2] = 0x99;

        for (z = 0; (unsigned) z < ARFA_NCOPIES; ++z) {
                if (arfa_write_zone (disk, arfa_copy_zones [z], data)
                    != DISK_IO_OK) {
                        fprintf (stderr, "Ошибка записи зоны %04o диска %d\n",
                                arfa_copy_zones [z], diskno);
                        disk_close (disk);
                        return -1;
                }
        }
        if (write_pass)
                arfa_build_passport (pass, diskno);
        else
                fprintf (stderr, "Диск %d: зона 0 непуста, паспорт тома "
                        "не записан, признак архивного тома не установлен\n",
                        diskno);
        if (write_pass) {
                unsigned char convol [1024];
                memset (convol, 1, sizeof (convol));    /* слова данных */
                if (disk_writei (disk, 0, (char*) pass, (char*) convol,
                                NULL, DISK_MODE_QUIET) != DISK_IO_OK) {
                        fprintf (stderr, "Ошибка записи паспорта (зона 0) "
                                "диска %d\n", diskno);
                        disk_close (disk);
                        return -1;
                }
        }
        disk_close (disk);

        printf ("Диск %d размечен как том АРФА.\n", diskno);
        printf ("  Метка тома: АRFA ");
        for (z = 0; z < 6; ++z)
                gost_putc (gost [z], stdout);
        printf ("\n  Каталог (пустой) записан в зоны");
        for (z = 0; (unsigned) z < ARFA_NCOPIES; ++z)
                printf (" %04o", arfa_copy_zones [z]);
        printf ("\n  Данные областей размещаются с зоны %04o.\n", ARFA_DATA_MIN);
        if (write_pass)
                printf ("  Паспорт тома записан в зону 0, признак архивного "
                        "тома установлен (том невидим в СЕРПе).\n");
        return 0;
}

/*
 * Создание области: запись в каталог (все копии) с выделением
 * зон данных. Если start == 0, зона выбирается автоматически -
 * за последней областью каталога (не раньше зоны 0030).
 * nzones - число зон области (по умолчанию 1).
 * attr - атрибут зонового слова (0x60..0x77, по умолчанию 0x68).
 * special - спецзапись (бит 0200, без файла данных).
 * Возвращает 0 при успехе.
 */
int
arfa_create (unsigned diskno, const char *name, unsigned start,
        unsigned nzones, unsigned attr, int special)
{
        void *disk;
        unsigned char data [ZBYTES], gost [12];
        int copies [16], ncopy, nrec, i, n;
        arfa_rec_t recs [128];
        int place, maxend, zone;
        unsigned char *w;

        if (attr < 0x60 || attr > 0x77) {
                fprintf (stderr, "Атрибут 0x%x вне диапазона 0x60..0x77\n", attr);
                return -1;
        }
        if (! nzones)
                nzones = 1;
        if (arfa_name_to_gost (name, gost) < 0)
                return -1;

        disk = disk_open (diskno, DISK_READ_WRITE);
        if (! disk) {
                fprintf (stderr, "Диск %d: не открывается\n", diskno);
                return -1;
        }
        ncopy = arfa_find_copies (disk, copies, 16);
        if (! ncopy) {
                fprintf (stderr, "Диск %d: том АРФА не размечен "
                        "(нет метки АRFA). Сначала 'besmtool arfa-format %d'.\n",
                        diskno, diskno);
                disk_close (disk);
                return -1;
        }
        if (disk_read (disk, copies [0], (char*) data) != DISK_IO_OK) {
                fprintf (stderr, "Ошибка чтения зоны %04o\n", copies [0]);
                disk_close (disk);
                return -1;
        }

        nrec = arfa_scan_records (data, recs, 128);

        /* Имя уже существует? */
        for (i = 0; i < nrec; ++i) {
                if (arfa_name_equal (recs [i].name, gost)) {
                        fprintf (stderr, "Область '");
                        arfa_print_name (recs [i].name, stderr);
                        fprintf (stderr, "' уже есть в каталоге (зона %o)\n",
                                recs [i].zone);
                        disk_close (disk);
                        return -1;
                }
        }

        /* Выбор начальной зоны: явно или за последней областью. */
        zone = start;
        if (! zone) {
                zone = ARFA_DATA_MIN;
                for (i = 0; i < nrec; ++i)
                        if (recs [i].zone + recs [i].nzones > zone)
                                zone = recs [i].zone + recs [i].nzones;
        }
        if (zone + nzones > (unsigned) disk_size (disk)) {
                fprintf (stderr, "Область (зоны %o..%o) не помещается на диск\n",
                        zone, zone + nzones - 1);
                disk_close (disk);
                return -1;
        }

        /* Место записи: после последней записи каталога. */
        place = ARFA_FIRST_REC;
        maxend = ARFA_FIRST_REC;
        for (i = 0; i < nrec; ++i)
                if (recs [i].name_word + ARFA_REC_WORDS > maxend)
                        maxend = recs [i].name_word + ARFA_REC_WORDS;
        if (maxend > place)
                place = maxend;
        if (place + ARFA_REC_WORDS > 1024) {
                fprintf (stderr, "Каталог архива переполнен\n");
                disk_close (disk);
                return -1;
        }

        /* Запись каталога: 12 слов данных и разделитель. */
        w = data + place*6;
        memset (w, 0, ARFA_REC_WORDS*6);
        memcpy (w, gost, 12);                              /* +0, +1: имя */
        w [2*6+0] = 0x99; w [2*6+1] = 0x99; w [2*6+2] = 0x99;
        w [2*6+4] = 0x40;                                  /* +2: атрибуты */
        w [3*6+4] = (nzones >> 8) & 0xff;                  /* +3: число зон */
        w [3*6+5] = nzones & 0xff;
        w [4*6+0] = (zone >> 8) & 0xff;                    /* +4: зоновое слово */
        w [4*6+1] = zone & 0xff;
        w [4*6+2] = special ? 0200 : 0;
        w [4*6+4] = attr;
        w [5*6+0] = 0x99; w [5*6+1] = 0x99; w [5*6+2] = 0x99;
        w [5*6+4] = 0x08; w [5*6+5] = 0x2f;                /* +5: атрибуты */
        w [6*6+0] = 0x41; w [6*6+1] = 0x03;                /* +6: атрибуты */
        /* +7..+11: нули (даты) */

        for (n = 0; n < ncopy; ++n) {
                if (arfa_write_zone (disk, copies [n], data) != DISK_IO_OK) {
                        fprintf (stderr, "Ошибка записи зоны %04o диска %d\n",
                                copies [n], diskno);
                        disk_close (disk);
                        return -1;
                }
        }
        disk_close (disk);

        printf ("Создана область '");
        arfa_print_name (gost, stdout);
        printf ("' на диске %d: зоны %04o..%04o (%d), запись каталога в слове %04o,\n"
                "каталог обновлен в зонах",
                diskno, zone, zone + nzones - 1, nzones, place);
        for (n = 0; n < ncopy; ++n)
                printf (" %04o", copies [n]);
        printf ("\n");
        return 0;
}

/*
 * Уничтожение области: запись каталога обнуляется во всех копиях.
 * Зоны данных не стираются (могут быть заняты повторно).
 * Возвращает 0 при успехе.
 */
int
arfa_destroy (unsigned diskno, const char *name)
{
        void *disk;
        unsigned char data [ZBYTES], gost [12];
        int copies [16], ncopy, nrec, i, n, found = -1;
        arfa_rec_t recs [128];

        if (arfa_name_to_gost (name, gost) < 0)
                return -1;

        disk = disk_open (diskno, DISK_READ_WRITE);
        if (! disk) {
                fprintf (stderr, "Диск %d: не открывается\n", diskno);
                return -1;
        }
        ncopy = arfa_find_copies (disk, copies, 16);
        if (! ncopy) {
                fprintf (stderr, "Диск %d: том АРФА не размечен\n", diskno);
                disk_close (disk);
                return -1;
        }
        if (disk_read (disk, copies [0], (char*) data) != DISK_IO_OK) {
                fprintf (stderr, "Ошибка чтения зоны %04o\n", copies [0]);
                disk_close (disk);
                return -1;
        }

        nrec = arfa_scan_records (data, recs, 128);
        for (i = 0; i < nrec; ++i) {
                if (arfa_name_equal (recs [i].name, gost)) {
                        found = i;
                        break;
                }
        }
        if (found < 0) {
                fprintf (stderr, "Область '");
                arfa_print_name (gost, stdout);
                fprintf (stderr, "' в каталоге не найдена\n");
                disk_close (disk);
                return -1;
        }

        printf ("Уничтожается область '");
        arfa_print_name (recs [found].name, stdout);
        printf ("': зоны %04o..%04o (%d), запись каталога в слове %04o\n",
                recs [found].zone,
                recs [found].zone + recs [found].nzones - 1,
                recs [found].nzones, recs [found].name_word);

        memset (data + recs [found].name_word*6, 0, ARFA_REC_WORDS*6);

        for (n = 0; n < ncopy; ++n) {
                if (arfa_write_zone (disk, copies [n], data) != DISK_IO_OK) {
                        fprintf (stderr, "Ошибка записи зоны %04o диска %d\n",
                                copies [n], diskno);
                        disk_close (disk);
                        return -1;
                }
        }
        disk_close (disk);
        printf ("Запись удалена из каталога (зоны");
        for (n = 0; n < ncopy; ++n)
                printf (" %04o", copies [n]);
        printf ("). Зоны данных не стирались.\n");
        return 0;
}
