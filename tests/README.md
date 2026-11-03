# How to add a new test

## 1. Create directory

Create a directory for the new test, say 'tests/mytest'.

## 2. Input file

Put here a file with input for dispak, for example 'tests/mytest/mytest.b6'.

## 3. Options

Create a file 'tests/mytest/options.txt' with command line parameters for dispak.
The file should contains at least a filename of the input file, for example:

    mytest.b6

Here is a list of available options, from 'dispak --help':

    -x, --native           use native extracode E64
    -t, --trace            trace all extracodes
    --trace-e64            trace extracode 064
    -s, --stats            show statistics for machine instructions
    -p, --output-enable    display printing output (default for batch tasks)
    -q, --output-disable   no printing output (default for TELE tasks)
    -l, --output-latin     use Latin letters for output
    --output-cyrillic      use Cyrillic letters for output (default)
    --punch-binary         punch in binary format (default dots and holes)
    --input-encoding=code  set encoding for input files: utf8 koi8 cp1251 cp866
    --no-insn-check        all words but at addr 0 are treated as insns

## 4. Expected output

Create a file with expected output 'output.txt'. It can be obtained by
manually running dispak and redirecting the output to a file.
For example:

    cd tests/mytest
    dispak mytest.b6 > output.txt

## 5. Update the list

File tests/CMakeLists.txt contains a list of all tests.
You need to update it to include your test.
This can be done by script:

    cd tests
    ./show-tests.py -u

## Wildcards for host-dependent values

The '#' symbol in 'output.txt' matches any character of the actual
output line (see 'match_line' in run-test.py). Use it for values that
depend on the host, otherwise the test fails every day:

* current date in the listing header (bemsh-ms, forex, sort, ...):
  write '##.##.##' instead of the date;
* KIPS in whetstone depends on the host speed: replace all digits
  after 'КIРS' with '#';
* serp-ogl-2113: the year of ЗПУ/ЗПБ for the six oldest packages
  (КЧ, ПРОРОК, СПОЭ67, КАЧКА, Э60, ПРИК5) is displayed depending on
  the current date - write '##' for the year; the dates of the other
  packages are stable;
* a diff hunk with several changed lines matches as a whole when the
  number of removed/added lines is equal and every removed line
  matches the corresponding added line (run-test.py, 2026-11).

## Local disk volumes (tests/disks)

If a test needs a writable disk volume, do NOT commit a per-test copy
of the image: the job would dirty the tracked file on every run and
the copies would diverge.  Instead:

* put the pristine image into `tests/disks/` (large volumes may be
  stored xz-compressed as `tests/disks/<NNNN>.xz`, e.g. `2348.xz`);
* list the disk number in `tests/<name>/disks.txt`, one per line;
* `run-test.py` unpacks/copies a fresh working copy into the test
  directory before each run, so every run starts from the pristine
  state; the working copies are gitignored (`tests/*/NNNN`).

Current volumes:

* `2196` - scratch АРФА volume for arfa-e63, arfa-io, arfalib,
  pascal-arfa;
* `2296` - small АРФА volume with the archive flag set (Э50 0166)
  for arfa-flag;
* `2348.xz` - system volume with GOSTLIB in zone 02560, the pasred
  reader from the pascal-re project and the ARFALIB library in zone
  02570 (deliverable image of the besm-crypto project).

## System-disk dependent tests (librefer-2048, librefer-2053, stat)

The output of these tests is defined by the CONTENT of the system
volumes (monitor and supervisor banners, library listings, zone
statistics), not only by the deck.  When the disks are taken from
the installed set (~/.besm6 or /usr/local/share/besm6) or from
$BESM6_PATH, a stale image makes the test fail forever - the
recorded etalons match the CURRENT images from besm6.org.

Therefore these tests run with `--path=../../build/dispak`, i.e.
against the volumes downloaded by cmake during the build (2048,
2053, 2099) - exactly the images the etalons were recorded
against.  The option overrides both $BESM6_PATH and the default
install path, so the tests are independent of the host
environment.  The disks are not modified by the decks, so the
build directory volumes stay pristine.

CAVEAT: the cmake rules download a volume only when it does not
exist in the build directory yet.  When besm6.org updates its
images (sbor2048.bin and sbor2053.bin were both refreshed on
2026-10-23: 2048 gained the NEWTAR/PASSFORM/ISO/GOST crypto
libraries, 2053 got a different zone statistics history), an OLD
build directory keeps the pre-update volumes and these three
tests fail forever against the current etalons.  Do NOT re-record
the etalons against the stale volumes - refresh the volumes
instead:

    rm build/dispak/2048 build/dispak/2053 build/dispak/2099
    make -C build

In this fork the download is not a silent fallback.  By default the
build takes the volumes from $BESM6_PATH (colon search list, first
hit) and then from the install share; a besm6.org download happens
only with BESM6_FETCH_DISKS=1, which also forces a fresh copy over
an existing local one (dispak/fetch_disk.sh):

    rm build/dispak/2048 build/dispak/2053 build/dispak/2099
    BESM6_FETCH_DISKS=1 make -C build

(the same applies to the other fetched volumes 2063, 2086, 2113,
2148, 2153, 2248, 4001 if their etalons ever start failing).

The remaining disk-dependent tests (librefer-2113/2148/2248,
serp-*) still use the installed volumes (their disks are not
fetched by the build); if besm6.org ever updates those images,
re-record the etalons or pin the volumes in tests/disks/.

## tests/sort

The upstream deck (ee12f49, "Added test of E66 with the 'keep in
memory' flag") contained a duplicated '*66 is' / 'kk 0,'10'(8)' pair
with the author's own note "comment out this line ... to make the
test pass".  The recorded output.txt corresponds to the deck WITHOUT
that pair; with it the job abends (*53) on the current besm6.org
disks.  The pair was removed on 2026-11 (see sort.b6); the first
'*66/kk' pair still exercises Э66 with the 'keep in memory' flag.

## tests/pascal-arfa

The dates 'LIВRАRУ ОТ 29.10.26/30.10.26' in output.txt are the
compilation dates of GOSTLIB modules baked into disk 2348 (see the
besm-crypto project); they are stable as long as 2348 is not rebuilt.
