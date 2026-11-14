#!/system/bin/sh
# Restart the modem subsystem so UIM re-detects a physically reinserted SIM.
# No card-detect GPIO on v410/v490 — hot-insert stays ABSENT until MPSS SSR
# or a full reboot. Prefer init (root) via property; Magisk root can also
# write debugfs directly if this image's init lacks the handler.
setprop sys.radio.modem_ssr 1
exit 0
