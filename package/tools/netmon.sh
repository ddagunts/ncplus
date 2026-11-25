#!/bin/sh
# netmon.sh [interval_s] [log] -- TV-side Wi-Fi logger for the periodic drop investigation.
# Every interval (default 5 s) it records one line per signal, so a drop can be lined up with
# what changed on the TV: wlan0 address/counters, ARP table, default route, link quality,
# wpa_supplicant state and the DHCP client. Log goes to RAM (/tmp), never flash.
I=${1:-5}
L=${2:-/tmp/netmon.log}
IF=wlan0
WPACLI=$(command -v wpa_cli 2>/dev/null)
echo "=== netmon start $(date) uptime=$(cut -d' ' -f1 /proc/uptime)" >> "$L"
while true; do
  t=$(cut -d' ' -f1 /proc/uptime)
  ip=$(ifconfig $IF 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p')
  st=/sys/class/net/$IF/statistics
  rx=$(cat $st/rx_packets 2>/dev/null); tx=$(cat $st/tx_packets 2>/dev/null)
  rxe=$(cat $st/rx_errors 2>/dev/null); txe=$(cat $st/tx_errors 2>/dev/null)
  rxm=$(cat $st/multicast 2>/dev/null)
  car=$(cat /sys/class/net/$IF/carrier 2>/dev/null)
  q=$(awk -v i="$IF:" '$1==i{print $3,$4,$5}' /proc/net/wireless)
  arp=$(awk 'NR>1{printf "%s/%s/%s ", $1, $3, $4}' /proc/net/arp)
  rt=$(awk 'NR>1 && $2=="00000000"{print $3}' /proc/net/route)
  echo "$t ip=$ip car=$car rx=$rx tx=$tx rxe=$rxe txe=$txe mc=$rxm q=[$q] gw=$rt arp=[$arp]" >> "$L"
  if [ -n "$WPACLI" ]; then
    w=$($WPACLI -i $IF status 2>/dev/null | grep -E '^(wpa_state|bssid|pairwise_cipher|group_cipher|ip_address)=' | tr '\n' ' ')
    [ -n "$w" ] && echo "$t wpa $w" >> "$L"
  fi
  sleep "$I"
done
