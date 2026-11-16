#!/system/bin/sh
# QCRIL with persist.radio.jbims=1 creates qmux_radio/rild_ims;
# m216 ImsService connects to qmux_radio/rild_ims0 (HITL 2026-11-16).
i=0
while [ "$i" -lt 90 ]; do
    if [ -S /dev/socket/qmux_radio/rild_ims ]; then
        ln -sfn rild_ims /dev/socket/qmux_radio/rild_ims0
        chmod 660 /dev/socket/qmux_radio/rild_ims 2>/dev/null
        exit 0
    fi
    i=$((i + 1))
    sleep 1
done
exit 1
