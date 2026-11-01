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
