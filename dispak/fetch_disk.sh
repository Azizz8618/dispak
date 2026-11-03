#!/bin/sh
#
# Provision one disk image into the build directory.
#
#   fetch_disk.sh <volume> <archive-file> <diskdir> <archive-url>
#
#     volume       single-disk deck name, e.g. 2053
#     archive-file file name on besm6.org, e.g. sbor2053.bin
#     diskdir      install share (last resort local source)
#     archive-url  besm6.org download root
#
# Search order (without $BESM6_FETCH_DISKS):
#   1. the directories of $BESM6_PATH (colon-separated search list,
#      the same variable dispak itself uses for volumes),
#   2. <diskdir>.
# With $BESM6_FETCH_DISKS set, always download a fresh copy from
# besm6.org instead (use it to refresh stale local volumes); without
# any local source the volume stays absent and the build fails with
# an explanatory message.
#
vol="$1"
arc="$2"
diskdir="$3"
url="$4"

if [ -n "$BESM6_FETCH_DISKS" ]; then
    exec wget -nv -O "$vol" "$url/$arc"
fi

for d in $(echo "$BESM6_PATH" | tr ':' ' ') "$diskdir"; do
    if [ -n "$d" ] && [ -e "$d/$vol" ]; then
        cp -a -L "$d/$vol" .
        exit 0
    fi
done

if [ -n "$BESM6_FETCH_DISKS" ]; then
    wget -nv -O "$vol" "$url/$arc"
else
    echo "$vol: not found in BESM6_PATH or $diskdir; run make with BESM6_FETCH_DISKS=1 to force download it from besm6.org" >&2
    exit 1
fi
