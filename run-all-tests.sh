#!/bin/sh
# Прогон всех тестов dispak вручную (без cmake/ctest).
# Требует собранных build/dispak/dispak и build/besmtool/besmtool.
cd "$(dirname "$0")" || exit 1
PASS=0; FAIL=0
for d in tests/*/; do
    [ -f "$d/options.txt" ] || continue
    name=$(basename "$d")
    if (cd "$d" && BESM6_PATH=../../build/dispak python3 ../run-test.py > /tmp/t.out 2>&1); then
        PASS=$((PASS+1))
    else
        FAIL=$((FAIL+1)); echo "FAIL: $name"; sed 's/^/    /' /tmp/t.out | head -8
    fi
done
echo "Пройдено: $PASS, ошибок: $FAIL"
