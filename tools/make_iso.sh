#!/bin/bash
# Builds a GameCube disc image (.iso / GCM) from an hl2gc disc folder:
#   <disc>/sys/boot.bin  <disc>/sys/bi2.bin  <disc>/sys/apploader.img  <disc>/sys/main.dol
#   <disc>/files/...     (becomes the disc's file system)
# Uses only bash + coreutils, so it runs inside devkitPro's MSYS2 shell on Windows.
# usage: make_iso.sh <disc folder> <out.iso>
set -euo pipefail
DISC=${1:?disc folder}
OUT=${2:?output iso}
SYS="$DISC/sys"
for f in boot.bin bi2.bin apploader.img main.dol; do
	[ -f "$SYS/$f" ] || { echo "make_iso: missing $SYS/$f" >&2; exit 1; }
done

align() { echo $(( ($1 + $2 - 1) / $2 * $2 )); }
fsize() { stat -c %s "$1"; }
# big-endian 32-bit value, written straight to stdout as raw bytes
be32() { printf "\\x$(printf %02x $(( ($1 >> 24) & 255 )))\\x$(printf %02x $(( ($1 >> 16) & 255 )))\\x$(printf %02x $(( ($1 >> 8) & 255 )))\\x$(printf %02x $(( $1 & 255 )))"; }
# write file $1 into $OUT at byte offset $2
put() { dd if="$1" of="$OUT" bs=1M seek="$2" oflag=seek_bytes conv=notrunc status=none; }

# ---- file system table (FST): depth-first, entries sorted case-insensitively like Nintendo's tools
TYPES=() NAMES=() PARENTS=() PATHS=() NEXTS=()
add_dir() { # $1 = folder, $2 = parent index
	local dir=$1 parent=$2 entry name idx
	while IFS= read -r entry; do
		name=$(basename "$entry")
		idx=${#TYPES[@]}
		if [ -d "$entry" ]; then
			TYPES+=(1) NAMES+=("$name") PARENTS+=("$parent") PATHS+=("") NEXTS+=(0)
			add_dir "$entry" "$idx"
			NEXTS[$idx]=${#TYPES[@]}
		else
			TYPES+=(0) NAMES+=("$name") PARENTS+=(0) PATHS+=("$entry") NEXTS+=(0)
		fi
	done < <(find -L "$dir" -mindepth 1 -maxdepth 1 | LC_ALL=C sort -f)
}
TYPES+=(1) NAMES+=("") PARENTS+=(0) PATHS+=("") NEXTS+=(0) # root
[ -d "$DISC/files" ] && add_dir "$DISC/files" 0
COUNT=${#TYPES[@]}
NEXTS[0]=$COUNT

# ---- layout
APP_SIZE=$(fsize "$SYS/apploader.img")
DOL_SIZE=$(fsize "$SYS/main.dol")
DOL_OFF=$(align $(( 0x2440 + APP_SIZE )) 256)
FST_OFF=$(align $(( DOL_OFF + DOL_SIZE )) 256)
NAMES_SIZE=0
for ((i = 1; i < COUNT; i++)); do NAMES_SIZE=$(( NAMES_SIZE + ${#NAMES[$i]} + 1 )); done
FST_SIZE=$(( COUNT * 12 + NAMES_SIZE ))
pos=$(align $(( FST_OFF + FST_SIZE )) 32768)
OFFS=() SIZES=()
for ((i = 0; i < COUNT; i++)); do
	if [ "${TYPES[$i]}" = 0 ]; then
		OFFS[$i]=$pos
		SIZES[$i]=$(fsize "${PATHS[$i]}")
		pos=$(align $(( pos + SIZES[$i] )) 32768)
	fi
done
TOTAL=$pos

# ---- write
rm -f "$OUT"
truncate -s "$TOTAL" "$OUT"
put "$SYS/boot.bin" 0
{ be32 "$DOL_OFF"; be32 "$FST_OFF"; be32 "$FST_SIZE"; be32 "$FST_SIZE"; } > "$OUT.hdr"
put "$OUT.hdr" $(( 0x420 ))
put "$SYS/bi2.bin" $(( 0x440 ))
put "$SYS/apploader.img" $(( 0x2440 ))
put "$SYS/main.dol" "$DOL_OFF"
{
	nameoff=0
	for ((i = 0; i < COUNT; i++)); do
		if [ "$i" = 0 ]; then
			printf '\x01\x00\x00\x00'; be32 0; be32 "$COUNT"
		else
			be32 $(( (TYPES[$i] << 24) | nameoff ))
			if [ "${TYPES[$i]}" = 1 ]; then be32 "${PARENTS[$i]}"; be32 "${NEXTS[$i]}"
			else be32 "${OFFS[$i]}"; be32 "${SIZES[$i]}"; fi
			nameoff=$(( nameoff + ${#NAMES[$i]} + 1 ))
		fi
	done
	for ((i = 1; i < COUNT; i++)); do printf '%s\0' "${NAMES[$i]}"; done
} > "$OUT.fst"
put "$OUT.fst" "$FST_OFF"
for ((i = 0; i < COUNT; i++)); do
	[ "${TYPES[$i]}" = 0 ] && put "${PATHS[$i]}" "${OFFS[$i]}"
done
rm -f "$OUT.hdr" "$OUT.fst"
echo "make_iso: $OUT  $(( TOTAL / 1048576 )) MB, $(( COUNT - 1 )) entries"
