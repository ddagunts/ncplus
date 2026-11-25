#!/bin/sh
# panel-volume.sh <default.xml>: add the volume control (xfce4-pulseaudio-plugin) to Xfce's
# default panel, right of the systray. Besides showing the volume it is what listens for the
# volume, mute and media keys of the remote and of keyboards: without it they do nothing.
# Only new users' panels start from this file.
set -e
f=$1
grep -q '"pulseaudio"' "$f" && exit 0
sed -i -e '0,/^ *<value type="int" value="6"\/>$/s//&\n        <value type="int" value="19"\/>/' \
	-e 's|^  <property name="plugins" type="empty">$|&\
    <property name="plugin-19" type="string" value="pulseaudio">\
      <property name="enable-keyboard-shortcuts" type="bool" value="true"/>\
    </property>|' "$f"
[ "$(grep -c -e '"pulseaudio"' -e 'type="int" value="19"' "$f")" = 2 ] ||
	{ echo "panel-volume.sh: $f is not what it was: adapt this script" >&2; exit 1; }
