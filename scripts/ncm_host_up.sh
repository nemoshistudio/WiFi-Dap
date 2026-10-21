#!/usr/bin/env bash
# Bring up the host side of the C1 USB-NCM link: wait for the cdc_ncm netdev,
# assign 192.168.7.2/24, take it away from NetworkManager and verify ping to
# 192.168.7.1.
#
# Usage: scripts/ncm_host_up.sh [tries]
#
# Root: the script needs root for `ip`/`nmcli`.  By default it uses sudo; when
# sudo is unavailable (e.g. no password in an automation session) point
# ROOT_HELPER at a command prefix that runs its argument as root, e.g.:
#   ROOT_HELPER=/tmp/hostroot.sh scripts/ncm_host_up.sh
set -u
TRIES="${1:-20}"
ROOT_HELPER="${ROOT_HELPER:-sudo}"

as_root() {
  # shellcheck disable=SC2086
  $ROOT_HELPER "$@"
}

for i in $(seq 1 "$TRIES"); do
  IF=$(ip -br link 2>/dev/null | awk '$1 ~ /^enx/ {print $1; exit}')
  if [ -n "$IF" ]; then
    # NetworkManager must not manage the probe interface: it flushes the manual
    # IPv4 address after its DHCP attempt fails, which kills in-flight USBIP /
    # OpenOCD sessions (observed as a hung flash + vhci_hcd xmit -110).
    as_root nmcli device set "$IF" managed no >/dev/null 2>&1 || true
    as_root ip addr replace 192.168.7.2/24 dev "$IF" 2>/dev/null || true
    as_root ip link set "$IF" up 2>/dev/null || true
    if ping -c1 -W1 192.168.7.1 >/dev/null 2>&1; then
      echo "NCM_READY iface=$IF"
      exit 0
    fi
  fi
  sleep 2
done
echo "NCM_TIMEOUT" >&2
exit 1
