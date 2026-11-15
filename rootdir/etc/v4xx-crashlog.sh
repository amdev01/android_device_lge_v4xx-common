#!/system/bin/sh
# Early boot crash mirror → /cache (survives KPDPWR_AND_RESIN DRAM wipe).
# Debuggable builds only. After death, from TWRP:
#   adb pull /cache/v4xx-crashlog

DIR=/cache/v4xx-crashlog
mkdir -p "$DIR" 2>/dev/null || exit 0

for f in meta.txt dmesg-at-start.txt kmsg.log logcat.log; do
  [ -e "$DIR/$f" ] || continue
  mv -f "$DIR/$f" "$DIR/${f}.prev" 2>/dev/null
done
# rotate prior logcat chunks
for f in "$DIR"/logcat.log.*; do
  [ -e "$f" ] || continue
  mv -f "$f" "${f}.prev" 2>/dev/null
done

{
  echo "=== v4xx-crashlog start ==="
  date -u 2>/dev/null
  getprop ro.build.flavor 2>/dev/null
  getprop ro.build.type 2>/dev/null
  getprop ro.build.version.incremental 2>/dev/null
  cat /proc/cmdline 2>/dev/null
} > "$DIR/meta.txt" 2>/dev/null

dmesg > "$DIR/dmesg-at-start.txt" 2>/dev/null
sync

# Kernel console → eMMC (bounded by periodic truncate in sync loop)
: > "$DIR/kmsg.log"
cat /proc/kmsg >> "$DIR/kmsg.log" 2>/dev/null &

# logd → rotating 1MB x 8 once socket exists
(
  i=0
  while [ "$i" -lt 90 ]; do
    [ -e /dev/socket/logdr ] && break
    i=$((i + 1))
    sleep 1
  done
  logcat -v threadtime -b all -f "$DIR/logcat.log" -r 1024 -n 8
) &

# fsync barrier + keep kmsg.log from growing without bound
while true; do
  sync
  sz=$(wc -c < "$DIR/kmsg.log" 2>/dev/null || echo 0)
  if [ "$sz" -gt 2097152 ]; then
    tail -c 1048576 "$DIR/kmsg.log" > "$DIR/kmsg.log.trim" 2>/dev/null && \
      mv -f "$DIR/kmsg.log.trim" "$DIR/kmsg.log"
  fi
  sleep 1
done
