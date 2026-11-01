/*
 * Архивная система АРФА ОС ДИСПАК: экстракоды Э63 со словом "КЛЮЧАР"
 * на сумматоре.
 *
 * Реализация соответствует "Руководству программиста" ОС ДИСПАК
 * (ИЫ.00110-01 33 01, разд. 5.3.156-5.3.180) и "Инструкции по
 * конфигурированию ОС" (ИЫ.00110-01 32 01, разд. 8.7 "Экстракоды
 * обращения к архивной системе"):
 *
 *   Э63 АИСП, СМ = "КЛЮЧАР" (код УПП/ГОСТ-10859),
 *   где АИСП - адрес нулевого информационного слова (ИС).
 *   Тип работы - в разрядах 48-43 слова ИС:
 *
 *     01 - создание области              10В - запрос каталога областей
 *     02 - уничтожение области           11В - запрос каталога общих областей
 *     03 - установка/смена/снятие пароля 12В - дозаказ области
 *     04 - задание прав доступа          13В - запрос имени и идентификатора
 *     05 - подтверждение пароля          14В - установка сдвига по области
 *     06 - запрос списка допущенных лиц  15В - запрос величины сдвига
 *     07 - запрос сведений об области    16В - подтверждение прав администратора
 *                                        17В - смена шифра и ключа администратора
 *     30В - захват области               31В - освобождение области
 *
 *   Разряды слова ИС: 48-43 - тип работы, 42-37 - логический номер,
 *   36-25 - пароль, 24/23 - монопольный/безусловный захват,
 *   24-13 - новый пароль, 12 - признак чтения, 21 - знак сдвига,
 *   20-5 - величина сдвига, 4-1 - способ задания имени
 *   (0 - лог.номер/идентификатор, 1..7 - "по-старому",
 *   10В - "по-новому" в УПП, 14В - "по-новому" в МС-80).
 *   Параметры передаются на РМР (регистре младших разрядов).
 *   Ответ: при нормальном завершении М16 = 0, иначе - код ответа
 *   (таблица 5 разд. 8.7.1.3).
 *
 * Том архива отыскивается так:
 *   - переменная среды BESM6_ARFA задает десятичный номер тома;
 *   - иначе просматриваются тома, подключенные логическим номерам
 *     030..067;
 *   - иначе просматриваются каталоги пути поиска образов (BESM6_PATH);
 *     берется первый том с меткой "АRFA" в слове 030 зоны 1.
 *
 * Устоявшаяся часть состояния (имена, зоны и длины областей) хранится
 * в каталоге тома в формате реальных томов АРФЫ и переживает перезапуск
 * эмулятора. Пароли, права, захваты и шифры хозяев существуют только
 * в памяти эмулятора (формат соответствующих слов реального каталога
 * не расшифрован); кодировка МС-80 для имен не поддерживается.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "defs.h"
#include "disk.h"
#include "encoding.h"
#include "gost10859.h"

/* Слова зоны каталога (как в besmtool/arfa.c). */
#define ARFA_LBL_WORD   0030    /* метка "АRFA" */
#define ARFA_FIRST_REC  0100    /* первое слово записей каталога */
#define ARFA_REC_WORDS  13      /* слот записи: 12 слов + разделитель */
#define ARFA_DATA_MIN   0030    /* данные областей не раньше этой зоны */
#define ARFA_MAX_AREAS  60      /* максимум записей в зоне каталога */

/* Коды ответа (таблица 5 разд. 8.7.1.3, восьмеричные). */
#define ARF_OK          0       /* ВЫПОЛНЕНО */
#define ARF_NO_NAME     1       /* ТАКОГО ИМЕНИ НЕТ */
#define ARF_BAD_NAME    2       /* НЕВЕРНОЕ ИМЯ ОБЛАСТИ */
#define ARF_NO_VIRT     4       /* НЕТ ВИРТ. УСТРОЙСТВА */
#define ARF_BAD_SCHIFR  5       /* ОШИБКА В ШИФРЕ */
#define ARF_EXISTS      6       /* ОБЛАСТЬ УЖЕ ЕСТЬ */
#define ARF_BUSY        010     /* ОБЛАСТЬ ЗАНЯТА */
#define ARF_NO_VOLUME   012     /* НЕ УСТАНОВЛЕН ТОМ */
#define ARF_VIRT_N      013     /* ВИРТ. УСТРОЙСТВ > 12 */
#define ARF_TOO_LONG    015     /* ДЛИНА ОБЛ. > РАЗР. */
#define ARF_NO_SPACE    022     /* НЕТ МЕСТА НА ТОМЕ */
#define ARF_NO_RIGHTS   023     /* НЕТ ПОЛНОМОЧИЙ */
#define ARF_IS_CATALOG  024     /* ОБЛАСТЬ / КАТАЛОГ */
#define ARF_BAD_KIND    027     /* ОШИБКА ВИДА */
#define ARF_NEED_PASS   031     /* ПОДТВЕРДИ / НЕ ЗАДАН ПАРОЛЬ */
#define ARF_VIRT_BUSY   032     /* ВИРТ. НОМЕР ЗАНЯТ */
#define ARF_AVOST       0100000 /* признак фатального авоста */

/* Диапазон логических номеров областей. */
#define ARF_LN_MIN      030
#define ARF_LN_MAX      067

/*
 * Извлечение разрядов слова: l-бит k соответствует разряду k+25,
 * r-бит k - разряду k+1 (старший разряд слова - 48-й).
 */
#define W48_43(w)       (((w).l >> 18) & 077)         /* тип работы */
#define W48_37(w)       (((w).l >> 12) & 077)         /* лог. номер */
#define W36_25(w)       ((w).l & 07777)               /* пароль */
#define W24(w)          (((w).r >> 23) & 1)           /* монопольный захват */
#define W24_13(w)       (((w).r >> 12) & 07777)       /* новый пароль */
#define W12(w)          (((w).r >> 11) & 1)           /* признак чтения */
#define W21(w)          (((w).r >> 20) & 1)           /* отрицат. сдвиг */
#define W20_5(w)        (((w).r >> 4) & 0xffff)       /* величина сдвига */
#define W5(w)           (((w).r >> 4) & 1)            /* верхний уровень */
#define W4_1(w)         ((w).r & 017)                 /* характер. имени */
#define W22_21(w)       (((w).r >> 20) & 3)           /* группа */
#define W20_18(w)       (((w).r >> 17) & 7)           /* вид области */
#define W17(w)          (((w).r >> 16) & 1)           /* индивидуальная */
#define W16_1(w)        ((w).r & 0xffff)              /* длина области */
#define W2_1(w)         ((w).r & 3)                   /* права 0..3 */

/*
 * Область архива. Поля zone/nzones/special/name хранятся в каталоге
 * тома, остальные - только в памяти эмулятора.
 */
typedef struct {
        unsigned char   name [12];      /* имя области (ГОСТ) */
        int             namelen;        /* длина имени без хвоста */
        int             slot;           /* слово имени в каталоге */
        int             zone;           /* начальная зона области */
        int             nzones;         /* число зон области */
        int             special;        /* спецзапись (бит 0200) */
        uint            owner;          /* шифр хозяина (0 - неизвестен) */
        int             kind, group;    /* вид и группа области */
        int             individual;     /* признак индивидуальности */
        uint            pass_r, pass_w; /* пароли чтения и записи */
        int             has_r, has_w;   /* пароли установлены */
        uint            rschifr [6];    /* шифры допущенных лиц */
        uint            rrights [6];    /* их права 0..3 */
        int             nrights;        /* число допущенных */
        int             other_rights;   /* права прочих (-1 - не заданы) */
        int             grab;           /* 0 - нет, 1 - совместный, 2 - моноп. */
        int             shift;          /* сдвиг по области, зон */
} arfa_area_t;

static arfa_area_t        areas [ARFA_MAX_AREAS];
static int                nareas;
static int                unit_area [64];        /* лог.номер -> индекс+1 */

/* Найденный том архива. */
static void              *arfa_disk;
static unsigned           arfa_diskno;
static int                arfa_ro;               /* том без права записи */
static int                arfa_copy_zones [16];  /* зоны копий каталога */
static int                arfa_ncopy;

/* Ключ администратора архива (только в памяти). */
static alureg_t           arfa_admin_key;

/*
 * Запись зоны каталога со свёрткой "данные", как в реальном томе.
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
 * Слово похоже на имя набора: допустимые байты и хотя бы одна буква.
 */
static int
name_char_ok (unsigned char c)
{
        return c <= 011 || c == GOST_SPACE ||
                (c >= 020 && c <= 0137);
}

static int
is_name_word (unsigned char *w)
{
        int i, letters = 0;

        for (i = 0; i < 6; ++i) {
                unsigned char c = w[i];
                if (c == GOST_SPACE || c == 0)
                        continue;
                if (! name_char_ok (c))
                        return 0;
                ++letters;
        }
        return letters > 0;
}

/*
 * Зоновое слово записи каталога:
 * [зона ст.][зона мл.][признаки][0][6X][0].
 */
static int
is_zone_word (unsigned char *w)
{
        return w[3] == 0 && w[4] >= 0x60 && w[4] <= 0x77 &&
                w[5] == 0 && (w[2] & 0x7f) == 0;
}

static int
arfa_name_len (unsigned char *name)
{
        int i, n = 0;

        for (i = 0; i < 12; ++i)
                if (name [i] != GOST_SPACE && name [i] != 0)
                        n = i + 1;
        return n;
}

static int
arfa_name_equal (unsigned char *name, unsigned char *gost)
{
        int i, na = arfa_name_len (name), nb = arfa_name_len (gost);

        if (na != nb)
                return 0;
        for (i = 0; i < na; ++i) {
                unsigned char a = name [i] ? name [i] : GOST_SPACE;
                unsigned char b = gost [i] ? gost [i] : GOST_SPACE;
                if (a != b)
                        return 0;
        }
        return 1;
}

/*
 * Загрузка каталога тома в таблицу areas[].
 * Поля, существующие только в памяти, переносятся по имени.
 */
static int
arfa_load_catalog (void)
{
        unsigned char data [ZONE_SIZE];
        arfa_area_t prev [ARFA_MAX_AREAS];
        int i, j, nprev = nareas;

        if (disk_read (arfa_disk, arfa_copy_zones [0], (char*) data)
            != DISK_IO_OK)
                return -1;

        memcpy (prev, areas, sizeof (areas));
        nareas = 0;
        for (i = 4; i < 1020 && nareas < ARFA_MAX_AREAS; ++i) {
                unsigned char *zw = data + i*6;
                arfa_area_t *r;

                if (! is_zone_word (zw))
                        continue;
                if (! is_name_word (data + (i-4)*6))
                        continue;
                r = &areas [nareas];
                memset (r, 0, sizeof (*r));
                memcpy (r->name, data + (i-4)*6, 6);
                memcpy (r->name + 6, data + (i-3)*6, 6);
                r->slot = i - 4;
                r->namelen = arfa_name_len (r->name);
                r->zone = (zw [0] << 8) | zw [1];
                r->nzones = (data [(i-1)*6 + 4] << 8) | data [(i-1)*6 + 5];
                r->special = zw [2] & 0200;
                r->other_rights = -1;
                /* перенос полей из памяти по имени области */
                for (j = 0; j < nprev; ++j)
                        if (arfa_name_equal (prev [j].name, r->name)) {
                                uint own = prev [j].owner;
                                int grb = prev [j].grab;
                                int shf = prev [j].shift;
                                memcpy (r->rschifr, prev [j].rschifr,
                                        sizeof (r->rschifr));
                                memcpy (r->rrights, prev [j].rrights,
                                        sizeof (r->rrights));
                                r->pass_r = prev [j].pass_r;
                                r->pass_w = prev [j].pass_w;
                                r->has_r = prev [j].has_r;
                                r->has_w = prev [j].has_w;
                                r->owner = own;
                                r->kind = prev [j].kind;
                                r->group = prev [j].group;
                                r->individual = prev [j].individual;
                                r->nrights = prev [j].nrights;
                                r->other_rights = prev [j].other_rights;
                                r->grab = grb;
                                r->shift = shf;
                                break;
                        }
                ++nareas;
        }
        return 0;
}

/*
 * Запись одного слота каталога во все копии зоны каталога тома.
 * Прочие слова зоны (заголовок, метка, чужие записи) не меняются.
 * Возвращает 0 при успехе.
 */
static int
arfa_write_slot (arfa_area_t *r)
{
        unsigned char data [ZONE_SIZE];
        unsigned char *w;
        int n, zone = r->special ? 2 : r->zone;

        if (r->slot < 0 || r->slot + ARFA_REC_WORDS > 1024)
                return -1;
        for (n = 0; n < arfa_ncopy; ++n) {
                if (disk_read (arfa_disk, arfa_copy_zones [n], (char*) data)
                    != DISK_IO_OK)
                        return -1;
                w = data + r->slot*6;
                memset (w, 0, ARFA_REC_WORDS*6);
                memcpy (w, r->name, 12);                      /* +0, +1 */
                w [2*6+0] = 0x99; w [2*6+1] = 0x99; w [2*6+2] = 0x99;
                w [2*6+4] = r->owner ? 0x40 : 0;              /* +2 */
                w [3*6+4] = (r->nzones >> 8) & 0xff;          /* +3 */
                w [3*6+5] = r->nzones & 0xff;
                w [4*6+0] = (zone >> 8) & 0xff;               /* +4 */
                w [4*6+1] = zone & 0xff;
                w [4*6+2] = r->special ? 0200 : 0;
                w [4*6+4] = 0x68;
                w [5*6+0] = 0x99; w [5*6+1] = 0x99; w [5*6+2] = 0x99;
                w [5*6+4] = 0x08; w [5*6+5] = 0x2f;           /* +5 */
                w [6*6+0] = 0x41; w [6*6+1] = 0x03;           /* +6 */
                /* +7..+11 - нули (даты) */
                if (arfa_write_zone (arfa_disk, arfa_copy_zones [n], data)
                    != DISK_IO_OK)
                        return -1;
        }
        return 0;
}

/*
 * Обнуление слота каталога во всех копиях (уничтожение области).
 * Прочие слова зоны не меняются.
 */
static int
arfa_clear_slot (int slot)
{
        unsigned char data [ZONE_SIZE];
        int n;

        for (n = 0; n < arfa_ncopy; ++n) {
                if (disk_read (arfa_disk, arfa_copy_zones [n], (char*) data)
                    != DISK_IO_OK)
                        return -1;
                if (slot < 0 || slot + ARFA_REC_WORDS > 1024)
                        return -1;
                memset (data + slot*6, 0, ARFA_REC_WORDS*6);
                if (arfa_write_zone (arfa_disk, arfa_copy_zones [n], data)
                    != DISK_IO_OK)
                        return -1;
        }
        return 0;
}

/*
 * Проверка метки "АRFA" в зоне 1 тома.
 */
static int
arfa_labeled (void *disk)
{
        unsigned char data [ZONE_SIZE];
        unsigned char *w;

        if (disk_read (disk, 1, (char*) data) != DISK_IO_OK)
                return 0;
        w = data + ARFA_LBL_WORD*6;
        return w[0] == GOST_A && w[1] == GOST_R &&
                w[2] == GOST_F && w[3] == GOST_A;
}

/*
 * Открытие тома архива: env BESM6_ARFA, затем лог. номера 030..067,
 * затем просмотр пути поиска образов. Возвращает 0 при успехе.
 */
static int
arfa_volume (void)
{
        unsigned char data [ZONE_SIZE];
        const char *env;
        char *p;
        unsigned num;
        int u, z, ncopy, copies [16];

        if (arfa_disk)
                return 0;

        /* 1. Явно заданный номер тома. */
        env = getenv ("BESM6_ARFA");
        if (env && *env) {
                char *tail;
                num = strtoul (env, &tail, 10);
                if (*tail)
                        return -1;
                if (! (arfa_disk = disk_open (num, DISK_READ_WRITE)) &&
                    ! (arfa_disk = disk_open (num, DISK_READ_ONLY)))
                        return -1;
                arfa_ro = ! disk_open (num, DISK_READ_WRITE);
                if (arfa_ro)
                        disk_close (arfa_disk),
                        arfa_disk = disk_open (num, DISK_READ_ONLY);
                arfa_diskno = num;
                goto found;
        }

        /* 2. Том, подключенный логическим номером 030..067. */
        for (u = ARF_LN_MIN; u <= ARF_LN_MAX; ++u) {
                if (! disks [u].diskno)
                        continue;
                arfa_disk = disk_open (disks [u].diskno, DISK_READ_WRITE);
                if (! arfa_disk)
                        continue;
                if (arfa_labeled (arfa_disk)) {
                        arfa_diskno = disks [u].diskno;
                        arfa_ro = 0;
                        goto found;
                }
                disk_close (arfa_disk);
                arfa_disk = 0;
        }

        /* 3. Просмотр пути поиска образов. */
        if (! disk_path) {
                char pathbuf [256];
                disk_find_path (pathbuf, 0);     /* инициализирует путь */
        }
        if (! disk_path)
                return -1;
        p = disk_path;
        while (p && ! arfa_disk) {
                char dir [256], *q = strchr (p, ':');
                DIR *d;
                struct dirent *e;

                if (q) {
                        if (q > p)
                                memcpy (dir, p, q-p);
                        dir [q-p] = 0;
                        p = q+1;
                } else {
                        strcpy (dir, p);
                        p = 0;
                }
                if (! (d = opendir (dir)))
                        continue;
                while ((e = readdir (d))) {
                        char *tail;
                        num = strtoul (e->d_name, &tail, 10);
                        if (*tail || ! num || num > 4095)
                                continue;
                        arfa_disk = disk_open (num, DISK_READ_WRITE);
                        if (! arfa_disk)
                                continue;
                        if (arfa_labeled (arfa_disk)) {
                                arfa_diskno = num;
                                arfa_ro = 0;
                                break;
                        }
                        disk_close (arfa_disk);
                        arfa_disk = 0;
                }
                closedir (d);
        }
        if (! arfa_disk)
                return -1;
found:
        ncopy = 0;
        for (z = 1; z <= 023; ++z) {
                unsigned char *w;
                if (disk_read (arfa_disk, z, (char*) data) != DISK_IO_OK)
                        break;
                w = data + ARFA_LBL_WORD*6;
                if (w[0] == GOST_A && w[1] == GOST_R &&
                    w[2] == GOST_F && w[3] == GOST_A)
                        copies [ncopy++] = z;
        }
        if (! ncopy) {
                disk_close (arfa_disk);
                arfa_disk = 0;
                return -1;
        }
        memcpy (arfa_copy_zones, copies, sizeof (copies));
        arfa_ncopy = ncopy;
        return 0;
}

/*===========================================================*
 * Разбор имени области из информационных слов.              *
 *===========================================================*/

/*
 * Добавление ГОСТ-символа к имени (не длиннее 12).
 */
static int
name_add (unsigned char *name, int *len, unsigned char c)
{
        if (c == 0143)                 /* пустой символ */
                return 0;
        if (*len >= 12)
                return -1;
        if (! name_char_ok (c))
                return -1;
        name [(*len)++] = c;
        return 0;
}

/*
 * Имя "по-новому" в коде УПП (ГОСТ): слова от ИС+1, байты до
 * кода 377 или нуля; '/' отделяет шифр хозяина.
 */
static int
parse_name_new (ushort isaddr, unsigned char *name, int *len, uint *schifr)
{
        alureg_t w;
        int addr, i, n = 0, slash = 0;
        uint sch = 0;

        *len = 0;
        *schifr = 0;
        memset (name, 0, 12);
        for (addr = isaddr + 1; addr <= isaddr + 20; ++addr) {
                LOAD (w, ADDR (addr));
                if (! w.l && ! w.r)
                        break;
                for (i = 0; i < 6; ++i) {
                        unsigned char c = i < 3 ?
                                (w.l >> (16 - 8*i)) & 0xff :
                                (w.r >> (16 - 8*(i-3))) & 0xff;
                        if (c == 0377 || c == 0)
                                goto done;
                        if (slash) {
                                if (c >= GOST_0 && c <= GOST_9) {
                                        sch = sch * 10 + (c - GOST_0);
                                        continue;
                                }
                                return -1;        /* не цифра в шифре */
                        }
                        if (c == GOST_SLASH) {
                                slash = 1;
                                continue;
                        }
                        if (name_add (name, &n, c) < 0)
                                return -1;
                }
        }
done:
        *len = n;
        *schifr = slash ? sch : 0;
        return n ? 0 : -1;
}

/*
 * Имя "по-старому": n простых имен (n = 1..7). Возвращает 0 при
 * успехе, -1 при ошибке, 1 если имя составное (более одного имени).
 */
static int
parse_name_old (ushort isaddr, int n, unsigned char *name, int *len,
        uint *schifr)
{
        alureg_t w;
        int addr = isaddr + 1, i, simple, m = 0;

        *len = 0;
        *schifr = 0;
        memset (name, 0, 12);
        for (simple = 0; simple < n; ++simple) {
                if (m >= 12)
                        return -1;
                if (simple)
                        name [m++] = GOST_DOT;
                LOAD (w, ADDR (addr));             /* 6 символов */
                for (i = 0; i < 6; ++i) {
                        unsigned char c = i < 3 ?
                                (w.l >> (16 - 8*i)) & 0xff :
                                (w.r >> (16 - 8*(i-3))) & 0xff;
                        if (c == GOST_SPACE || c == 0)
                                continue;
                        if (name_add (name, &m, c) < 0)
                                return -1;
                }
                ++addr;
                LOAD (w, ADDR (addr));             /* 3 символа + шифр */
                for (i = 0; i < 3; ++i) {
                        unsigned char c = (w.l >> (16 - 8*i)) & 0xff;
                        if (c == GOST_SPACE || c == 0)
                                continue;
                        if (name_add (name, &m, c) < 0)
                                return -1;
                }
                if (simple == 0)
                        *schifr = w.r & 0xffffff;  /* разряды 24-1 */
                ++addr;
        }
        *len = m;
        return n > 1 ? 1 : 0;
}

/*
 * Разбор 34-разрядного идентификатора области из слова ИС+1.
 * Возвращает индекс области или -1.
 */
static int
parse_ident (ushort isaddr)
{
        alureg_t w;
        uint inner;
        int i;

        LOAD (w, ADDR (isaddr + 1));
        if (! ((w.l >> 9) & 1))                    /* 34-й разряд */
                return -1;
        if ((w.r & 0777) != 0356)                  /* ключ 9-1 */
                return -1;
        if ((w.r >> 9) & 0x7f)                     /* 16-10 должны быть 0 */
                return -1;
        inner = ((w.l & 0x1ff) << 7) | ((w.r >> 17) & 0x7f);
        /* внутреннее имя: зона области (спецзаписи - слово записи) */
        for (i = 0; i < nareas; ++i)
                if ((uint) (areas [i].special ?
                     ARFA_FIRST_REC + i * ARFA_REC_WORDS :
                     areas [i].zone) == inner)
                        return i;
        return -1;
}

/*
 * Поиск области по имени.
 */
static int
find_area (unsigned char *name, int len)
{
        int i;

        (void) len;
        for (i = 0; i < nareas; ++i)
                if (arfa_name_equal (areas [i].name, name))
                        return i;
        return -1;
}

/*
 * Определение области: по лог.номеру, идентификатору или имени.
 * Возвращает индекс, -1 если не найдена, -2 если имя задано неверно.
 */
static int
resolve_area (alureg_t is, ushort isaddr)
{
        unsigned char name [12];
        int len;
        uint schifr;

        if (W48_37 (is)) {                         /* лог. номер */
                int u = W48_37 (is), ia;
                if (u < ARF_LN_MIN || u > ARF_LN_MAX)
                        return -2;
                ia = unit_area [u];
                if (ia > 0 && ia <= nareas &&
                    disks [u].diskno == arfa_diskno)
                        return ia - 1;
                return -1;
        }
        if (W4_1 (is) == 014)                      /* МС-80 не поддержан */
                return -2;
        if (W4_1 (is) > 7)
                return -2;
        if (W4_1 (is) == 010) {
                if (parse_name_new (isaddr, name, &len, &schifr) < 0)
                        return -2;
                return find_area (name, len);
        }
        if (W4_1 (is) == 0)
                return parse_ident (isaddr);
        {
                int rc = parse_name_old (isaddr, W4_1 (is), name, &len,
                        &schifr);
                if (rc < 0)
                        return -2;
                return find_area (name, len);
        }
}

/*===========================================================*
 * Вспомогательные операции.                                 *
 *===========================================================*/

/*
 * АВОСТ архивной системы: печать диагностики и фатальное
 * окончание задачи.
 */
static int
arfa_avost (const char *msg)
{
        fprintf (stderr, "АРФА: авост - %s\n", msg);
        return ARF_AVOST;
}

/*
 * Выдача имени области в буфер "по-новому" в коде УПП:
 * символы ГОСТ, завершение - код 377.
 */
static void
store_name_new (int addr, unsigned char *name, int len)
{
        unsigned char b [6];
        alureg_t w;
        int i, j, n = 0;

        for (i = 0; i < len; i += 6) {
                memset (b, 0143, 6);               /* пустые символы */
                for (j = 0; j < 6 && i + j < len; ++j)
                        b [j] = name [i + j];
                w.l = ((uint) b [0] << 16) | (b [1] << 8) | b [2];
                w.r = ((uint) b [3] << 16) | (b [4] << 8) | b [5];
                STORE (w, ADDR (addr + n));
                ++n;
        }
        w.l = ((uint) 0377) << 16;                 /* конец имени */
        w.r = 0;
        STORE (w, ADDR (addr + n));
}

/*
 * Идентификатор области (разряды 34-1): 34Р=1, 33-18 - внутреннее
 * имя, 16-10 = 0, 9-1 = '356'В.
 */
static alureg_t
area_ident (int idx)
{
        alureg_t w;
        uint inner = areas [idx].special ?
                ARFA_FIRST_REC + idx * ARFA_REC_WORDS :
                (uint) areas [idx].zone;

        w.l = (1 << 9) | (inner >> 7);
        w.r = ((inner & 0x7f) << 17) | 0356;
        return w;
}

/*
 * Обновление offset юнитов, заказанных на область (после сдвига).
 */
static void
update_units (int idx)
{
        int u;

        for (u = ARF_LN_MIN; u <= ARF_LN_MAX; ++u)
                if (unit_area [u] == idx + 1 &&
                    disks [u].diskno == arfa_diskno)
                        disks [u].offset = areas [idx].zone +
                                areas [idx].shift;
}

/*===========================================================*
 * Операции Э63.                                             *
 *===========================================================*/

/*
 * 01 - создание области.
 */
static int
op_create (alureg_t is, ushort isaddr, alureg_t prm)
{
        unsigned char name [12];
        int len, i, idx, zone, place;
        uint schifr;

        if (! W4_1 (is) || W4_1 (is) == 014)
                return ARF_BAD_NAME;
        if (W4_1 (is) == 010) {
                if (parse_name_new (isaddr, name, &len, &schifr) < 0)
                        return ARF_BAD_NAME;
        } else {
                int rc = parse_name_old (isaddr, W4_1 (is), name, &len,
                        &schifr);
                if (rc < 0)
                        return ARF_BAD_NAME;
                if (rc > 0)
                        return ARF_NO_NAME;   /* нет области-каталога */
        }
        if (! len)
                return ARF_BAD_NAME;
        if (! schifr)
                return ARF_BAD_SCHIFR;       /* шифр хозяина обязателен */
        if (find_area (name, len) >= 0)
                return ARF_EXISTS;
        if (W20_18 (prm) == 6)
                return ARF_BAD_KIND;         /* недопустимый вид */
        if (nareas >= ARFA_MAX_AREAS)
                return ARF_NO_SPACE;         /* нет места в каталоге */

        /* размещение: за последней областью, не раньше 0030 */
        zone = ARFA_DATA_MIN;
        for (i = 0; i < nareas; ++i)
                if (! areas [i].special &&
                    areas [i].zone + areas [i].nzones > zone)
                        zone = areas [i].zone + areas [i].nzones;
        if (zone + (int) W16_1 (prm) > disk_size (arfa_disk))
                return ARF_NO_SPACE;

        /* свободный слот каталога: 13 нулевых слов */
        {
                unsigned char data [ZONE_SIZE];
                int i, j, slot = -1;

                if (disk_read (arfa_disk, arfa_copy_zones [0], (char*) data)
                    != DISK_IO_OK)
                        return ARF_NO_VOLUME;
                for (i = ARFA_FIRST_REC; i + ARFA_REC_WORDS <= 1024;
                     i += ARFA_REC_WORDS) {
                        int busy = 0;
                        for (j = 0; j < ARFA_REC_WORDS; ++j) {
                                unsigned char *w = data + (i+j)*6;
                                if (w[0] | w[1] | w[2] | w[3] | w[4] | w[5]) {
                                        busy = 1;
                                        break;
                                }
                        }
                        if (! busy) {
                                slot = i;
                                break;
                        }
                }
                if (slot < 0)
                        return ARF_NO_SPACE;   /* нет места в каталоге */
                place = slot;
        }

        idx = nareas;
        memset (&areas [idx], 0, sizeof (areas [idx]));
        memcpy (areas [idx].name, name, 12);
        areas [idx].namelen = len;
        areas [idx].slot = place;
        areas [idx].zone = zone;
        areas [idx].nzones = W16_1 (prm);
        areas [idx].owner = schifr;
        areas [idx].kind = W20_18 (prm);
        areas [idx].group = W22_21 (prm);
        areas [idx].individual = W17 (prm);
        areas [idx].other_rights = -1;
        if (! W16_1 (prm))
                areas [idx].special = 1;     /* область-каталог */
        ++nareas;

        if (arfa_ro)
                return ARF_NO_VOLUME;
        if (arfa_write_slot (&areas [idx]) < 0)
                return ARF_NO_VOLUME;
        return ARF_OK;
}

/*
 * 02 - уничтожение области.
 */
static int
op_destroy (alureg_t is, ushort isaddr, alureg_t prm)
{
        unsigned char name [12];
        int len, idx, u;
        uint schifr, pass = W36_25 (is);

        (void) prm;
        if (! W4_1 (is) || W4_1 (is) == 014)
                return ARF_BAD_NAME;
        if (W4_1 (is) == 010) {
                if (parse_name_new (isaddr, name, &len, &schifr) < 0)
                        return ARF_BAD_NAME;
        } else {
                if (parse_name_old (isaddr, W4_1 (is), name, &len, &schifr)
                    < 0)
                        return ARF_BAD_NAME;
        }
        idx = find_area (name, len);
        if (idx < 0)
                return ARF_NO_NAME;
        if (areas [idx].owner && schifr && areas [idx].owner != schifr)
                return ARF_NO_RIGHTS;        /* чужая область */
        if (areas [idx].has_w) {
                if (! pass)
                        return ARF_NEED_PASS;
                if (areas [idx].pass_w != pass)
                        return arfa_avost ("НЕВЕРНЫЙ ПАРОЛЬ");
        }
        if (arfa_ro)
                return ARF_NO_VOLUME;
        if (arfa_clear_slot (areas [idx].slot) < 0)
                return ARF_NO_VOLUME;
        memmove (&areas [idx], &areas [idx + 1],
                (nareas - idx - 1) * sizeof (arfa_area_t));
        --nareas;
        for (u = 0; u < 64; ++u) {          /* поправить ссылки юнитов */
                if (unit_area [u] == idx + 1)
                        unit_area [u] = 0;
                else if (unit_area [u] > idx + 1)
                        --unit_area [u];
        }
        return ARF_OK;
}

/*
 * 03 - установка, смена и снятие пароля на область.
 */
static int
op_password (alureg_t is, ushort isaddr, alureg_t prm)
{
        int idx = resolve_area (is, isaddr);
        uint old = W36_25 (is), new = W24_13 (is);

        (void) prm;
        if (idx == -2)
                return ARF_BAD_NAME;
        if (idx < 0)
                return ARF_NO_NAME;
        if (areas [idx].special)
                return ARF_IS_CATALOG;
        if (W12 (is)) {                            /* пароль чтения */
                if (areas [idx].has_r && old != areas [idx].pass_r)
                        return arfa_avost ("НЕВЕРНЫЙ ПАРОЛЬ");
                areas [idx].has_r = new != 0;
                areas [idx].pass_r = new;
        } else {                                    /* пароль записи */
                if (areas [idx].has_w && old != areas [idx].pass_w)
                        return arfa_avost ("НЕВЕРНЫЙ ПАРОЛЬ");
                areas [idx].has_w = new != 0;
                areas [idx].pass_w = new;
        }
        return ARF_OK;
}

/*
 * 04 - задание прав доступа к области.
 */
static int
op_grant (alureg_t is, ushort isaddr, alureg_t prm)
{
        int idx = resolve_area (is, isaddr);
        uint schifr = prm.l & 0xffffff;
        int rights = W2_1 (prm), i;

        if (idx == -2)
                return ARF_BAD_NAME;
        if (idx < 0)
                return ARF_NO_NAME;
        if (! schifr) {
                areas [idx].other_rights = rights;
                return ARF_OK;
        }
        for (i = 0; i < areas [idx].nrights; ++i)
                if (areas [idx].rschifr [i] == schifr) {
                        areas [idx].rrights [i] = rights;
                        return ARF_OK;
                }
        if (areas [idx].nrights >= 6)
                return ARF_NO_SPACE;
        areas [idx].rschifr [areas [idx].nrights] = schifr;
        areas [idx].rrights [areas [idx].nrights] = rights;
        ++areas [idx].nrights;
        return ARF_OK;
}

/*
 * 05 - подтверждение пароля на область.
 */
static int
op_confirm (alureg_t is, ushort isaddr, alureg_t prm)
{
        int idx = resolve_area (is, isaddr);
        uint pass = W36_25 (is);

        (void) prm;
        if (idx == -2)
                return ARF_BAD_NAME;
        if (idx < 0)
                return ARF_NO_NAME;
        if (W12 (is)) {
                if (areas [idx].has_r) {
                        if (pass != areas [idx].pass_r)
                                return arfa_avost ("НЕВЕРНЫЙ ПАРОЛЬ");
                }
        } else {
                if (areas [idx].has_w) {
                        if (pass != areas [idx].pass_w)
                                return arfa_avost ("НЕВЕРНЫЙ ПАРОЛЬ");
                }
        }
        return ARF_OK;
}

/*
 * 06 - запрос списка допущенных лиц.
 * Права "прочих" выдаются в разрядах 47-46 слова В+0
 * (в документации соответствующее поле описано не полностью).
 */
static int
op_list (alureg_t is, ushort isaddr, alureg_t prm)
{
        int idx = resolve_area (is, isaddr);
        uint pass = W36_25 (is);
        ushort buf = prm.r & 077777;
        alureg_t w0, w1, w2, w3;
        int i;

        if (idx == -2)
                return ARF_BAD_NAME;
        if (idx < 0)
                return ARF_NO_NAME;
        if (areas [idx].has_r && pass != areas [idx].pass_r)
                return arfa_avost ("НЕВЕРНЫЙ ПАРОЛЬ");

        w0.l = w0.r = 0;
        if (areas [idx].other_rights >= 0)
                w0.l = (areas [idx].other_rights & 3) << 22;
        for (i = 0; i < areas [idx].nrights && i < 6; ++i)
                w0.r |= (areas [idx].rrights [i] & 3) << (10 - 2*i);
        w1.l = w1.r = w2.l = w2.r = w3.l = w3.r = 0;
        for (i = 0; i < areas [idx].nrights && i < 6; ++i) {
                uint s = areas [idx].rschifr [i];
                switch (i) {
                case 0: w1.l = s; break;
                case 1: w1.r = s; break;
                case 2: w2.l = s; break;
                case 3: w2.r = s; break;
                case 4: w3.l = s; break;
                case 5: w3.r = s; break;
                }
        }
        STORE (w0, ADDR (buf));
        STORE (w1, ADDR (buf + 1));
        STORE (w2, ADDR (buf + 2));
        STORE (w3, ADDR (buf + 3));
        return ARF_OK;
}

/*
 * 07 - запрос сведений об области.
 */
static int
op_info (alureg_t is, ushort isaddr, alureg_t prm)
{
        int idx = resolve_area (is, isaddr);

        (void) prm;
        if (idx == -2)
                return ARF_BAD_NAME;
        if (idx < 0)
                return ARF_NO_NAME;
        acc.l = areas [idx].owner;
        acc.r = ((uint) (areas [idx].has_r ? 1 : 0) << 23) |
                ((uint) (areas [idx].has_w ? 1 : 0) << 22) |
                ((uint) (areas [idx].group & 3) << 17) |
                ((uint) (areas [idx].individual ? 1 : 0) << 16) |
                (areas [idx].nzones & 0xffff);
        return ARF_OK;
}

/*
 * 10 - запрос каталога областей (верхний уровень).
 */
static int
op_catalog (alureg_t is, ushort isaddr, alureg_t prm)
{
        ushort buf = prm.r & 077777;
        int count = (prm.l >> 12) & 0xfff;
        int first = prm.l & 0xfff;
        int i, n, addr;
        alureg_t w;

        if (! W5 (is)) {
                int idx = resolve_area (is, isaddr);
                if (idx == -2)
                        return ARF_BAD_NAME;
                if (idx < 0)
                        return ARF_NO_NAME;
                return ARF_IS_CATALOG;   /* областей-каталогов нет */
        }
        if (! count || ! first || ! buf)
                return ARF_BAD_NAME;
        n = 0;
        addr = buf;
        for (i = first - 1; i < nareas && n < count; ++i, ++n) {
                alureg_t w1, w2;
                unsigned char nm [12];
                int j;

                memset (nm, GOST_SPACE, 12);
                for (j = 0; j < areas [i].namelen && j < 9; ++j)
                        nm [j] = areas [i].name [j] ?
                                areas [i].name [j] : GOST_SPACE;
                w1.l = ((uint) nm [0] << 16) | (nm [1] << 8) | nm [2];
                w1.r = ((uint) nm [3] << 16) | (nm [4] << 8) | nm [5];
                w2.l = ((uint) nm [6] << 16) | (nm [7] << 8) | nm [8];
                w2.r = areas [i].owner;    /* шифр хозяина */
                STORE (w1, ADDR (addr));
                STORE (w2, ADDR (addr + 1));
                addr += 2;
        }
        w.l = w.r = 0xffffff;
        STORE (w, ADDR (addr));            /* терминатор */
        return ARF_OK;
}

/*
 * 11 - запрос каталога общих областей.
 * Общая библиотека не поддерживается: возвращается пустой каталог.
 */
static int
op_catalog_common (alureg_t is, ushort isaddr, alureg_t prm)
{
        ushort buf = prm.r & 077777;
        alureg_t w;

        (void) is; (void) isaddr;
        if (! buf)
                return ARF_BAD_NAME;
        w.l = w.r = 0xffffff;
        STORE (w, ADDR (buf));
        return ARF_OK;
}

/*
 * 12 - дозаказ области по имени (или по лог.номеру/идентификатору).
 */
static int
op_order (alureg_t is, ushort isaddr, alureg_t prm)
{
        int idx = resolve_area (is, isaddr);
        uint32_t scale = ((uint32_t) prm.l << 8) | ((prm.r >> 16) & 0xff);
        int i, k, u, nbusy = 0;

        if (idx == -2)
                return ARF_BAD_NAME;
        if (idx < 0)
                return ARF_NO_NAME;
        if (areas [idx].special)
                return ARF_IS_CATALOG;
        for (i = ARF_LN_MIN; i <= ARF_LN_MAX; ++i)
                if (disks [i].diskno)
                        ++nbusy;
        if (nbusy >= 12)
                return ARF_VIRT_N;         /* ВИРТ.УСТР-В > 12 */
        if (getenv ("BESM6_ARFA_DEBUG")) {
                int u2;
                fprintf (stderr, "АРФА: дозаказ, заняты:");
                for (u2 = ARF_LN_MIN; u2 <= ARF_LN_MAX; ++u2)
                        if (disks [u2].diskno)
                                fprintf (stderr, " %o:%d", u2,
                                        disks [u2].diskno);
                fprintf (stderr, "\n");
        }

        /* первый свободный лог.номер из шкалы на РМР */
        for (k = 0; k <= ARF_LN_MAX - ARF_LN_MIN; ++k) {
                if (! (scale & (0x80000000u >> k)))
                        continue;
                u = ARF_LN_MIN + k;
                if (disks [u].diskno)
                        continue;
                disks [u].diskno = arfa_diskno;
                disks [u].mode = W12 (is) ? DISK_READ_ONLY :
                        DISK_READ_WRITE;
                disks [u].offset = areas [idx].zone + areas [idx].shift;
                disks [u].diskh = 0;
                unit_area [u] = idx + 1;
                acc.l = 0;
                acc.r = u;                 /* лог.номер на СМ */
                return ARF_OK;
        }
        return ARF_VIRT_BUSY;
}

/*
 * 13 - запрос имени и идентификатора области.
 */
static int
op_link (alureg_t is, ushort isaddr, alureg_t prm)
{
        int idx = resolve_area (is, isaddr);
        ushort buf = prm.r & 077777;

        if (idx == -2)
                return ARF_BAD_NAME;
        if (idx < 0)
                return ARF_NO_NAME;
        acc = area_ident (idx);            /* идентификатор на СМ */
        if (buf)
                store_name_new (buf, areas [idx].name, areas [idx].namelen);
        return ARF_OK;
}

/*
 * 14 - задание сдвига по области.
 */
static int
op_set_shift (alureg_t is, ushort isaddr, alureg_t prm)
{
        int idx = resolve_area (is, isaddr);
        int val = W20_5 (is);

        (void) prm;
        if (idx == -2)
                return ARF_BAD_NAME;
        if (idx < 0)
                return ARF_NO_NAME;
        if (areas [idx].special)
                return ARF_IS_CATALOG;
        areas [idx].shift = W21 (is) ? -val : val;
        update_units (idx);
        return ARF_OK;
}

/*
 * 15 - запрос сдвига по области.
 */
static int
op_get_shift (alureg_t is, ushort isaddr, alureg_t prm)
{
        int idx = resolve_area (is, isaddr);

        (void) prm;
        if (idx == -2)
                return ARF_BAD_NAME;
        if (idx < 0)
                return ARF_NO_NAME;
        acc.l = 0;
        acc.r = ((uint) areas [idx].shift) & 0xffffff;
        return ARF_OK;
}

/*
 * 16 - переход в режим администратора архива.
 * Защита ключом эмулируется, только если задана переменная среды
 * BESM6_ARFA_KEY (48-битный ключ в восьмеричной записи).
 */
static int
op_admin (alureg_t is, ushort isaddr, alureg_t prm)
{
        alureg_t key;
        const char *env = getenv ("BESM6_ARFA_KEY");

        (void) is;
        LOAD (key, ADDR (isaddr + 1));
        if (env && *env) {
                uint64_t v = strtoull (env, 0, 8);
                alureg_t want;
                want.l = (uint) (v >> 24);
                want.r = (uint) (v & 0xffffff);
                if (key.l != want.l || key.r != want.r)
                        return arfa_avost ("НЕВЕРНЫЙ КЛЮЧ АДМИНИСТРАТОРА");
        }
        arfa_admin_key = key;
        acc.l = user.l & 0xffffff;         /* "истинный" шифр задачи */
        acc.r = prm.l & 0xffffff;          /* шифр бюджета */
        return ARF_OK;
}

/*
 * 17 - смена шифра и ключа администратора.
 */
static int
op_admin_key (alureg_t is, ushort isaddr, alureg_t prm)
{
        alureg_t newkey, oldkey;
        const char *env = getenv ("BESM6_ARFA_KEY");

        (void) is; (void) prm;
        LOAD (newkey, ADDR (isaddr + 1));
        LOAD (oldkey, ADDR (isaddr + 2));
        if (env && *env) {
                if (oldkey.l != arfa_admin_key.l ||
                    oldkey.r != arfa_admin_key.r)
                        return arfa_avost ("НЕВЕРНЫЙ КЛЮЧ АДМИНИСТРАТОРА");
        }
        arfa_admin_key = newkey;
        return ARF_OK;
}

/*
 * 30 - захват области.
 */
static int
op_grab (alureg_t is, ushort isaddr, alureg_t prm)
{
        int idx = resolve_area (is, isaddr);
        int i, excl = W24 (is);

        (void) prm;
        if (idx == -2)
                return ARF_BAD_NAME;
        if (idx < 0)
                return ARF_NO_NAME;
        if (areas [idx].special)
                return ARF_IS_CATALOG;
        if (areas [idx].grab == 2 || (areas [idx].grab == 1 && excl))
                return ARF_BUSY;
        /* одной задаче - одна захваченная область */
        for (i = 0; i < nareas; ++i)
                if (i != idx && areas [i].grab)
                        return ARF_BUSY;
        areas [idx].grab = excl ? 2 : 1;
        return ARF_OK;
}

/*
 * 31 - освобождение области.
 */
static int
op_release (alureg_t is, ushort isaddr, alureg_t prm)
{
        int idx = resolve_area (is, isaddr);

        (void) prm;
        if (idx == -2)
                return ARF_BAD_NAME;
        if (idx < 0)
                return ARF_NO_NAME;
        if (areas [idx].special)
                return ARF_IS_CATALOG;
        if (! areas [idx].grab)
                return arfa_avost ("ЗАПРЕЩ.ЭКСТРАКОД "
                        "(освобождение без захвата)");
        areas [idx].grab = 0;
        return ARF_OK;
}

/*===========================================================*
 * Диспетчер архивных экстракодов Э63.                       *
 *===========================================================*/

int
arfa_e63 (void)
{
        alureg_t is, prm;
        ushort isaddr = ADDR (reg [016]);
        int code;

        if (arfa_volume () < 0 || arfa_load_catalog () < 0) {
                reg [016] = ARF_NO_VOLUME;
                return E_SUCCESS;
        }

        LOAD (is, isaddr);
        prm = accex;                        /* РМР */
        if (getenv ("BESM6_ARFA_DEBUG")) {
                alureg_t w1, w2;
                LOAD (w1, ADDR (isaddr + 1));
                LOAD (w2, ADDR (isaddr + 2));
                fprintf (stderr, "АРФА: АИСП=%o ИС=%o_%o РМР=%o_%o "
                        "областей=%d ИМЯ1=%o_%o ИМЯ2=%o_%o\n", isaddr,
                        is.l, is.r, prm.l, prm.r, nareas,
                        w1.l, w1.r, w2.l, w2.r);
        }

        switch (W48_43 (is)) {
        case 01:    code = op_create (is, isaddr, prm); break;
        case 02:    code = op_destroy (is, isaddr, prm); break;
        case 03:    code = op_password (is, isaddr, prm); break;
        case 04:    code = op_grant (is, isaddr, prm); break;
        case 05:    code = op_confirm (is, isaddr, prm); break;
        case 06:    code = op_list (is, isaddr, prm); break;
        case 07:    code = op_info (is, isaddr, prm); break;
        case 010:   code = op_catalog (is, isaddr, prm); break;
        case 011:   code = op_catalog_common (is, isaddr, prm); break;
        case 012:   code = op_order (is, isaddr, prm); break;
        case 013:   code = op_link (is, isaddr, prm); break;
        case 014:   code = op_set_shift (is, isaddr, prm); break;
        case 015:   code = op_get_shift (is, isaddr, prm); break;
        case 016:   code = op_admin (is, isaddr, prm); break;
        case 017:   code = op_admin_key (is, isaddr, prm); break;
        case 030:   code = op_grab (is, isaddr, prm); break;
        case 031:   code = op_release (is, isaddr, prm); break;
        default:
                fprintf (stderr, "АРФА: неизвестный тип работы %o\n",
                        W48_43 (is));
                reg [016] = ARF_BAD_NAME;
                return E_SUCCESS;
        }

        if (code == ARF_AVOST)              /* фатальный авост */
                return E_TERM;
        reg [016] = code;                   /* 16-й модификатор задачи */
        return E_SUCCESS;
}
