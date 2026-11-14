#!/system/bin/sh
# SoftapController writes ctrl_interface=/data/misc/wifi/sockets but attaches
# via /data/misc/wifi/hostapd/<iface>. Bind the dirs so SoftAP ctrl works.
# (A symlink is denied by SELinux: netd cannot read wifi_data_file lnk_file.)

SOCKETS=/data/misc/wifi/sockets
HOSTAPD=/data/misc/wifi/hostapd

mkdir -p "$SOCKETS" "$HOSTAPD"
chown wifi:wifi "$SOCKETS" "$HOSTAPD"
chmod 770 "$SOCKETS" "$HOSTAPD"

if ! mountpoint -q "$HOSTAPD"; then
    mount --bind "$SOCKETS" "$HOSTAPD"
fi
