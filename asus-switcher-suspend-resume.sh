#!/bin/sh
#
# When we wakeup from suspend, both devices are turned on.  This causes
# unexpected power draw and confusines switcheroo.  Turn the unused device
# back on before suspend and off after resume some everyone is in sync.

case "$1" in
	suspend|hibernate)
	if [ -e /sys/kernel/debug/vgaswitcheroo/switch ]; then
		echo ON > /sys/kernel/debug/vgaswitcheroo/switch
	fi
	exit 0
	;;
	resume|thaw)
	if [ -e /sys/kernel/debug/vgaswitcheroo/switch ]; then
		echo OFF > /sys/kernel/debug/vgaswitcheroo/switch
	fi
	exit 0
	;;
	*)
	echo "Usage $0 {suspend|hibernate|resume|thaw}"
	exit 1
esac
