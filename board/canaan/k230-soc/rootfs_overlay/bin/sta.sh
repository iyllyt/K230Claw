#!/bin/sh
usage()
{
    echo "help"
    cat <<EOF
    usage: $0 wlanx  ssid password;
    example: $0 wlan0  wjx_pc 123456789

EOF
}
[ $# -ne 3 ] && { usage $* ;} && { exit 1 ;}
set -x;
wlandev="$1"
ssid="$2"
password="$3"


set -x;
cat >/etc/wpa_supplicant.conf  <<EOF
ctrl_interface=/var/run/wpa_supplicant
ap_scan=1

network={
  key_mgmt=NONE
}
EOF

$(ps | grep wpa_supplicant | grep -v grep  >/dev/null 2>&1 ;) && { $(killall wpa_supplicant); sleep 2 ;}
wpa_supplicant -D nl80211 -i ${wlandev} -c /etc/wpa_supplicant.conf -B
sleep 5
wpa_cli -i ${wlandev} scan;
sleep 3
wpa_cli -i ${wlandev} scan_result;
sleep 1
NETID=$(wpa_cli -i ${wlandev} add_network | tail -1)
wpa_cli -i ${wlandev} set_network ${NETID} ssid \"${ssid}\"
wpa_cli -i ${wlandev} set_network ${NETID} psk \"${password}\"

wpa_cli -i ${wlandev} select_network ${NETID}

if `which udhcpc >/dev/null 2>&1` ; then
    udhcpc -i ${wlandev} -q -n #
else
cat >/etc/systemd/network/10-wlan0.network <<EOF
[Match]
Name=wlan0

[Network]
DHCP=yes
EOF
systemctl restart systemd-networkd
fi



#ifmetric ${wlandev} 100  #设置路由优先级
# 查看网络状态
wpa_cli -i ${wlandev} status
