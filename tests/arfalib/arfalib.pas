(*=y+,s7*)program arfalib(output);
(* ARFALIB - библиотека управления зашифрованными областями архива
   АРФА ОС ДИСПАК для внешних вызовов из Паскаль-программ.
   Объектный модуль записывается в персональную библиотеку на томе
   2348 в зонах 02570-02572 (сразу за районом библиотеки ГОСТ 02560).
   Экспортируемые точки входа (имена каталога не длиннее 8 букв):
     ARFACRE - создать область архива (Э63, подкод 01)
     ARFADEL - уничтожить область (Э63, подкод 02)
     ARFAWRE - зашифровать ГОСТ 28147-89 и записать зону (Э63 12 + Э70)
     ARFARDD - считать зону и расшифровать (Э70 + Э63 12)
   Шифрование выполняют внешние процедуры ENCCTR/DECCTR библиотеки
   GOSTLIB (том 2348, зона 02560); контекст ГОСТ (ключ, s-боксы)
   готовит вызывающая программа через GOSTINIT и передает параметром.
   Имя области задается "по-старому": nl, nr - слово из 6 букв,
   n3 - слово еще из 3 букв (код УПП/ГОСТ-10859), sc - шифр хозяина.
   Код ответа Э63 возвращается через параметр ok (0 - выполнено,
   иначе код ответа архивной системы). Библиотека ничего не печатает.
   Буферные листы обмена Э70 - страницы 010-013 (020000-027777):
   страница 010 - исходный текст, 011 - шифрограмма (запись),
   012 - принятая шифрограмма, 013 - расшифрованный текст (чтение).
   Слова текста передаются половинками по 24 разряда (2 слова на
   alfa-слово), так как арифметика ГОСТ нормализует слова < 2^32.
   ВАЖНО: модуль НЕ ИМЕЕТ глобальных переменных. Глобали объектного
   модуля библиотеки компоновщик размещает поверх данных вызывающей
   программы (isw модуля ложится на смещение 42 кадра задачи), поэтому
   всё рабочее состояние - локальные переменные процедур. Контекст ГОСТ
   передается по ссылке (var): копия 160 слов по значению идет через
   участок передачи параметров P/LNGAR и также небезопасна. *)
type
  w2 = packed record l, r: 0..16777215 end;
  CtxT = record
    roundkey: array [1..32] of integer;
    sbox: array [1..8] of array [0..15] of integer
  end;
  PtrW = @integer;
procedure ENCCTR(c2: CtxT; txt: PtrW; r2: PtrW;
  l2: integer; iv: integer); external;
procedure DECCTR(c2: CtxT; ct2: PtrW; r2: PtrW;
  l2: integer); external;

(*=e+*)procedure ARFACRE(nl, nr, n3, sc, ln: integer; var ok: integer);
(* Создать область архива длиной ln зон. *)
var
  isw: array [1..3] of w2;
  kl, prmw, answerw, tmpw: w2;
  pi: @w2;
begin
  kl.l := 52053075B;
  kl.r := 67040060B;
  isw[2].l := nl;
  isw[2].r := nr;
  isw[3].l := n3;
  isw[3].r := sc;
  isw[1].l := 1000000B;
  isw[1].r := 1;
  prmw.l := 0;
  prmw.r := ln;
  pi := ref(prmw);
  besm(400005B);
  tmpw := prmw;
  besm(24120000B);
  pi := ref(isw[1]);
  besm(400006B);
  tmpw := kl;
  besm(30630000B);
  answerw := ;
  besm(420016B);
  tmpw := ;
  ok := tmpw.r
end;

(*=e+*)procedure ARFADEL(nl, nr, n3, sc: integer; var ok: integer);
(* Уничтожить область архива. *)
var
  isw: array [1..3] of w2;
  kl, answerw, tmpw: w2;
  pi: @w2;
begin
  kl.l := 52053075B;
  kl.r := 67040060B;
  isw[2].l := nl;
  isw[2].r := nr;
  isw[3].l := n3;
  isw[3].r := sc;
  isw[1].l := 2000000B;
  isw[1].r := 1;
  pi := ref(isw[1]);
  besm(400006B);
  tmpw := kl;
  besm(30630000B);
  answerw := ;
  besm(420016B);
  tmpw := ;
  ok := tmpw.r
end;

(*=e+*)procedure ARFAWRE(nl, nr, n3, sc, nw, zn, iv: integer;
  var c2: CtxT; src: PtrW; var ok: integer);
(* Зашифровать ГОСТ 28147-89 (режим CTR, начало гаммы iv) и записать
   в зону zn области nw alfa-слов текста. Текст передается массивом
   целых половинок по 24 разряда (2 слова на alfa-слово), src - адрес
   массива (ref-выражение вызывающей программы). В зоне: слово iv,
   затем 2*nw слов шифрограммы. *)
var
  isw: array [1..3] of w2;
  kl, prmw, ks, answerw, tmpw: w2;
  pi: @w2;
  i, code, unitn: integer;
  pw: PtrW;
begin
  kl.l := 52053075B;
  kl.r := 67040060B;
  isw[2].l := nl;
  isw[2].r := nr;
  isw[3].l := n3;
  isw[3].r := sc;
  isw[1].l := 12000000B;
  isw[1].r := 1;
  prmw.l := 77600000B;
  prmw.r := 0;
  pi := ref(prmw);
  besm(400005B);
  tmpw := prmw;
  besm(24120000B);
  pi := ref(isw[1]);
  besm(400006B);
  tmpw := kl;
  besm(30630000B);
  answerw := ;
  besm(420016B);
  tmpw := ;
  code := tmpw.r;
  unitn := answerw.r;
  if code = 0 then begin
    pw := ptr(8191);
    for i := 1 to 2 * nw do begin
      pw := succ(pw);
      pw@ := src@;
      src := succ(src)
    end;
    encctr(c2, ptr(8192), ptr(9216), 2 * nw, iv);
    ks.l := 0 * 4096 + 9 * 64;
    ks.r := unitn * 4096 + zn;
    pi := ref(ks);
    besm(400005B);
    besm(24700000B);
    ok := 0
  end else
    ok := code
end;

(*=e+*)procedure ARFARDD(nl, nr, n3, sc, nw, zn: integer;
  var c2: CtxT; dst: PtrW; var ok: integer);
(* Считать зону zn области, расшифровать (начало гаммы - первое слово
   зоны) и выдать nw alfa-слов (2*nw половинок) по адресу dst. *)
var
  isw: array [1..3] of w2;
  kl, prmw, ks, answerw, tmpw: w2;
  pi: @w2;
  i, code, unitn: integer;
  pw: PtrW;
begin
  kl.l := 52053075B;
  kl.r := 67040060B;
  isw[2].l := nl;
  isw[2].r := nr;
  isw[3].l := n3;
  isw[3].r := sc;
  isw[1].l := 12000000B;
  isw[1].r := 1;
  prmw.l := 77600000B;
  prmw.r := 0;
  pi := ref(prmw);
  besm(400005B);
  tmpw := prmw;
  besm(24120000B);
  pi := ref(isw[1]);
  besm(400006B);
  tmpw := kl;
  besm(30630000B);
  answerw := ;
  besm(420016B);
  tmpw := ;
  code := tmpw.r;
  unitn := answerw.r;
  if code = 0 then begin
    ks.l := 8 * 4096 + 10 * 64;
    ks.r := unitn * 4096 + zn;
    pi := ref(ks);
    besm(400005B);
    besm(24700000B);
    decctr(c2, ptr(10240), ptr(11264), 2 * nw);
    pw := ptr(11263);
    for i := 1 to 2 * nw do begin
      pw := succ(pw);
      dst@ := pw@;
      dst := succ(dst)
    end;
    ok := 0
  end else
    ok := code
end;

begin
end.
