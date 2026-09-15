# .clinerules — Правила проекта ДИСПАК (dispak)
# Версия: 2026-09-15

## === ПРОЕКТ-СПЕЦИФИЧНЫЕ ПРАВИЛА ===

PROJECT_ROOT = /home/azizz/Yandex.Disk/dispak
BUILD_DIR    = /home/azizz/Yandex.Disk/dispak/build
BUILD_LOG    = /tmp/dispak_build.log

# Ключевые файлы
LARGE_FILES:
  - dispak/extra.c     (~2572 строки — экстракоды)
  - dispak/encoding.c  (~1547 строки — кодировки)
  - dispak/cu.c        (~884 строки — главный цикл)
  - dispak/disk.c      (~731 строки — дисковые операции)
  - dispak/arith.c     (~619 строки — арифметика)
  - disbesm6/disbesm6.c (~1011 строка — дизассемблер)

# Команды проекта
BUILD_CMD = cd /home/azizz/Yandex.Disk/dispak/build && cmake .. > /tmp/dispak_build.log 2>&1 && make -j$(nproc) >> /tmp/dispak_build.log 2>&1
CHECK_CMD = tail -20 /tmp/dispak_build.log
TEST_CMD  = cd /home/azizz/Yandex.Disk/dispak/build && make test

# === СТРАТЕГИЯ РАБОТЫ С ФАЙЛАМИ ===
# Правило 1: Пакетное чтение (один вызов read_files для всех файлов)
# Правило 2: Карты кода для LARGE_FILES (.cline/maps/*.map)
# Правило 3: Не перечитывать уже прочитанные файлы

# === СТРАТЕГИЯ КОМАНД ===
# Долгие команды (make, cmake) — в фоне:
#   BUILD_CMD > /tmp/dispak_build.log 2>&1 &
# Проверка: tail -20 /tmp/dispak_build.log
