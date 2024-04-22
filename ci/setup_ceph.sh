#!/bin/bash
# Lifted over from https://github.com/nerc-project/coldfront-plugin-cloud

set -ex

OSD_BIN_DIR=/tmp


function install_pkgs() {
  apt-get update
  apt-get install -y cephadm ceph-common lvm2 ipcalc jq iproute2
}

function init_ceph() {
  DEFAULT_DEVICE=$(ip -j route show default | jq -r '.[0].dev')
  IP=$(ip -j add show dev $DEFAULT_DEVICE | jq -r '.[0].addr_info[0].local')
  PREFIX=$(ip -j add show dev $DEFAULT_DEVICE | jq -r '.[0].addr_info[0].prefixlen')
  NETWORK=$(ipcalc $IP/$PREFIX | grep -i network: | awk '{ print $2 }')

  cephadm bootstrap \
    --cluster-network $NETWORK \
    --mon-ip $IP \
    --allow-fqdn-hostname \
    --single-host-defaults \
    --log-to-file \
    --skip-firewalld \
    --skip-dashboard \
    --skip-monitoring-stack \
    --allow-overwrite
}

function osd_setup() {
  OSD1_BIN=$OSD_BIN_DIR/osd0.bin
  OSD2_BIN=$OSD_BIN_DIR/osd1.bin
  dd if=/dev/zero of=$OSD1_BIN bs=512M count=8
  dd if=/dev/zero of=$OSD2_BIN bs=512M count=8
  OSD1_DEV=$(losetup -f)
  losetup $OSD1_DEV $OSD1_BIN
  OSD2_DEV=$(losetup -f)
  losetup $OSD2_DEV $OSD2_BIN
  pvcreate $OSD1_DEV
  pvcreate $OSD2_DEV
  vgcreate rgw $OSD1_DEV $OSD2_DEV
  lvcreate -n rgw-ceph-osd0 -L 4000M rgw
  lvcreate -n rgw-ceph-osd1 -L 4000M rgw
  cephadm shell ceph orch daemon add osd $HOSTNAME:/dev/rgw/rgw-ceph-osd0
  cephadm shell ceph orch daemon add osd $HOSTNAME:/dev/rgw/rgw-ceph-osd1
}

function rgw_setup() {
  cephadm shell ceph orch apply rgw test --placement=1
}

install_pkgs
init_ceph
osd_setup
rgw_setup
