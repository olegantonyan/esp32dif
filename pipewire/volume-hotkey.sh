#!/bin/sh
set -eu

DEVICE=esp32dif-rtp

STEP=${1:-}

case "$STEP" in
	[+-][0-9]*%) ;;
	*) echo "usage: ${0##*/} [+N%|-N%]" >&2; exit 1 ;;
esac

volume() {
	pactl get-sink-volume "$DEVICE" | cut -d/ -f2 -s | tr -d ' %' | head -n1
}

pactl set-sink-volume "$DEVICE" "$STEP"

VOLUME=$(volume)
if [ "$VOLUME" -gt 100 ]; then
	pactl set-sink-volume "$DEVICE" 100%
	VOLUME=100
fi

if [ "$(pactl get-sink-mute "$DEVICE")" = "Mute: yes" ]; then
	VOLUME=0
fi

if ! qdbus6 org.kde.plasmashell /org/kde/osdService org.kde.osdService.volumeChanged "$VOLUME" >/dev/null 2>&1; then
	notify-send -i audio-volume-high -t 1500 -e "Volume ${VOLUME}%" || true
fi
