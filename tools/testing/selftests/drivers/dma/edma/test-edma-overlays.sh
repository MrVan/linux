#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Runtime error-path tests for the i.MX95 fsl-edma per-channel child-node
# support.
#
# Mainline does not expose /sys/kernel/config/device-tree.  This uses the
# of-overlay-test module, which calls of_overlay_fdt_apply() on a .dtbo
# loaded through request_firmware_direct().
#
# Requirements:
#   CONFIG_OF_OVERLAY=y
#   CONFIG_FW_LOADER=y
#   kernel build directory for of-overlay-test.ko
#
# Usage:
#   sudo ./test-edma-overlays.sh
#
# Optional environment:
#   DRV=fsl-edma
#   DEV=42000000.dma-controller
#   DTC=/path/to/scripts/dtc/dtc
#   KDIR=/lib/modules/$(uname -r)/build
#   MODULE=/path/to/of-overlay-test.ko
#   FW_DIR=/lib/firmware/edma-test

DRV=${DRV:-fsl-edma}
DEV=${DEV:-42000000.dma-controller}
DTC=${DTC:-$PWD/scripts/dtc/dtc}
KDIR=${KDIR:-/lib/modules/$(uname -r)/build}
MODULE=${MODULE:-$PWD/of-overlay-test.ko}
MODNAME=${MODNAME:-of-overlay-test}
FW_DIR=${FW_DIR:-/lib/firmware/edma-test}
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR=$(mktemp -d)
DRV_DIR="/sys/bus/platform/drivers/$DRV"

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
[ -x "$DTC" ] || { echo "dtc not found: $DTC"; exit 1; }
[ -d "$DRV_DIR" ] || { echo "$DRV is not registered"; exit 1; }

CTRL=$(readlink -f "/sys/bus/platform/devices/$DEV")
[ -n "$CTRL" ] || { echo "cannot resolve $DEV"; exit 1; }

DT_CHAN=${DT_CHAN:-/sys/firmware/devicetree/base/soc/bus@42000000/dma-controller@42000000/dma-channel@42208000}
[ -d "$DT_CHAN" ] || {
	echo "channel 63 child node not found in the running DT:"
	echo "  $DT_CHAN"
	echo "Boot a DTB built from the converted imx95.dtsi"
	exit 1
}

channels()
{
	local ch path

	for ch in /sys/class/dma/dma*chan*; do
		[ -e "$ch" ] || continue
		path=$(readlink -f "$ch" 2>/dev/null || true)
		case "$path" in
		"$CTRL"/*) basename "$ch" ;;
		esac
	done
}

check_idle()
{
	local ch inuse

	for ch in $(channels); do
		inuse=$(cat "/sys/class/dma/$ch/in_use" 2>/dev/null || echo 0)
		if [ "$inuse" != 0 ]; then
			echo "FAIL: $ch is in use ($inuse clients)"
			return 1
		fi
	done
}

unbind_ctrl()
{
	echo "$DEV" > "$DRV_DIR/unbind" 2>/dev/null || true
}

bind_ctrl()
{
	echo "$DEV" > "$DRV_DIR/bind" 2>/dev/null
}

overlay_apply()
{
	insmod "$MODULE" dtbo="edma-test/$1.dtbo"
}

overlay_remove()
{
	rmmod "$MODNAME" 2>/dev/null || true
}

cleanup()
{
	unbind_ctrl
	overlay_remove
	bind_ctrl >/dev/null 2>&1 || true
	rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

build_overlays()
{
	local dtso dtbo

	for dtso in "$HERE"/*.dtso; do
		[ -e "$dtso" ] || continue
		dtbo="$BUILD_DIR/$(basename "${dtso%.dtso}").dtbo"
		echo "DTC $dtso"
		"$DTC" -@ -I dts -O dtb -o "$dtbo" "$dtso" || exit 1
	done
}

install_overlays()
{
	mkdir -p "$FW_DIR"
	cp "$BUILD_DIR"/*.dtbo "$FW_DIR"/
}

build_module()
{
	if [ -f "$MODULE" ]; then
		return
	fi

	[ -d "$KDIR" ] || {
		echo "kernel build dir not found: $KDIR"
		echo "set KDIR= or build of-overlay-test.ko and set MODULE="
		exit 1
	}

	echo "building of-overlay-test.ko"
	make -C "$KDIR" M="$HERE" modules || exit 1
}

channel_absent()
{
	channels | grep -q "chan$1$"
	[ $? -ne 0 ]
}

# run_case <name> <success|fail> <expected-dmesg> [absent-channel]
run_case()
{
	local name=$1 expect=$2 pattern=$3 absent=${4:-}
	local log bind_ok

	echo "=== $name ==="
	check_idle || exit 1

	unbind_ctrl
	sleep 0.2

	if ! overlay_apply "$name"; then
		echo "FAIL: could not apply $name overlay"
		exit 1
	fi

	dmesg -C

	if bind_ctrl; then
		bind_ok=1
	else
		bind_ok=0
	fi
	sleep 0.3
	log=$(dmesg)

	unbind_ctrl
	overlay_remove
	bind_ctrl >/dev/null 2>&1 || true
	sleep 0.2

	if [ "$expect" = success ]; then
		if [ "$bind_ok" != 1 ]; then
			echo "FAIL: expected probe success"
			echo "$log"
			exit 1
		fi
		if [ -n "$absent" ] && ! channel_absent "$absent"; then
			echo "FAIL: channel $absent should be absent"
			echo "$log"
			exit 1
		fi
	else
		if [ "$bind_ok" = 1 ]; then
			echo "FAIL: expected probe failure"
			echo "$log"
			exit 1
		fi
		if ! echo "$log" | grep -q "$pattern"; then
			echo "FAIL: missing expected message: $pattern"
			echo "$log"
			exit 1
		fi
	fi

	echo "PASS: $name"
}

build_overlays
install_overlays
build_module

run_case imx95-edma-chan63-disabled fail \
	"has no dma-channel child node"
run_case imx95-edma-chan63-no-irq fail \
	"child node has no valid interrupt"
run_case imx95-edma-chan63-unaligned fail \
	"invalid channel"
run_case imx95-edma-chan63-outside fail \
	"invalid channel"

run_case imx95-edma-chan63-disabled-masked success "" 63
run_case imx95-edma-chan63-masked success "" 63
run_case imx95-edma-dma-channels-63 success "" 63

echo "All edma overlay tests passed"
