#!/system/bin/sh
# Early boot crash mirror → /cache (survives KPDPWR_AND_RESIN DRAM wipe).
# Debuggable builds only. After death, from TWRP:
#   adb pull /cache/v4xx-crashlog
#
# Guard: only eng/userdebug. Init starts us on post-fs-data_done for all
# builds that set that prop — exit quietly on user.

DIR=/cache/v4xx-crashlog

debuggable=$(getprop ro.debuggable 2>/dev/null)
if [ "$debuggable" != "1" ]; then
  # Non-oneshot service: exiting would respawn forever. Idle instead.
  while true; do sleep 3600; done
fi

# Retry mkdir briefly (SEPolicy/init race); never exit 0 on failure — that
# caused an init restart storm when mkdir was denied.
ok=0
i=0
while [ "$i" -lt 30 ]; do
  mkdir -p "$DIR" 2>/dev/null && ok=1 && break
  i=$((i + 1))
  sleep 1
done
if [ "$ok" != "1" ]; then
  echo "v4xx-crashlog: mkdir $DIR failed" > /dev/kmsg
  # Stay alive doing nothing so init does not respawn us every few seconds
  while true; do sleep 3600; done
fi

for f in meta.txt dmesg-at-start.txt kmsg.log logcat.log; do
  [ -e "$DIR/$f" ] || continue
  mv -f "$DIR/$f" "$DIR/${f}.prev" 2>/dev/null
done
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

: > "$DIR/kmsg.log"
cat /proc/kmsg >> "$DIR/kmsg.log" 2>/dev/null &

(
  i=0
  while [ "$i" -lt 90 ]; do
    [ -e /dev/socket/logdr ] && break
    i=$((i + 1))
    sleep 1
  done
  logcat -v threadtime -b all -f "$DIR/logcat.log" -r 1024 -n 8
) &

while true; do
  sync
  sz=$(wc -c < "$DIR/kmsg.log" 2>/dev/null || echo 0)
  if [ "$sz" -gt 2097152 ]; then
    tail -c 1048576 "$DIR/kmsg.log" > "$DIR/kmsg.log.trim" 2>/dev/null && \
      mv -f "$DIR/kmsg.log.trim" "$DIR/kmsg.log"
  fi
  sleep 1
done
