# ncconf.sh: the NetCast+ settings file (sourced by start.sh and the apps' run.sh; set N=<netcast dir> first).
#
# All settings, servers and URLs live in ONE file on the USB stick, next to the netcast/ folder:
#     <stick>/netcast.conf
# The zip only contains netcast/, so unzipping an upgrade (or deleting the old folder first) never
# touches it. Nothing is written to the TV's internal flash.
#
#   ncconf_init                 create netcast.conf if missing, migrating the old per-app files
#   ncconf_section NAME         print the [NAME] section's lines (no comments/blank lines, CRLF stripped)
#   ncconf_get NAME KEY         print KEY's value from [NAME] (last one wins)

NETCAST_CONF=${NETCAST_CONF:-$(cd "$N/.." && pwd)/netcast.conf}
export NETCAST_CONF

ncconf_section() {
  [ -f "$NETCAST_CONF" ] || return 0
  awk -v s="[$1]" '
    { sub(/\r$/, ""); t = $0; gsub(/^[ \t]+|[ \t]+$/, "", t) }
    t ~ /^\[.*\]$/ { on = (tolower(t) == tolower(s)); next }
    on && t != "" && t !~ /^#/ { print }
  ' "$NETCAST_CONF"
}

ncconf_get() {
  ncconf_section "$1" | sed -n "s/^[ \t]*$2[ \t]*=[ \t]*//p" | tail -1
}

# lines of an old per-app file that aren't comments or blank
_ncconf_old() {
  [ -f "$1" ] && grep -v '^[[:space:]]*#' "$1" | grep -v '^[[:space:]]*$' | tr -d '\r'
}

ncconf_init() {
  if [ -f "$NETCAST_CONF" ]; then
    # sections added in later versions: append them (comments only) to an existing file
    grep -qi '^[[:space:]]*\[browser\]' "$NETCAST_CONF" || { printf '\n'; _ncconf_browser; } >> "$NETCAST_CONF"
    return 0
  fi
  {
    cat <<'EOF'
# NetCast+ settings: servers, URLs and options for all apps.
# This file sits next to the netcast/ folder on the USB stick, so upgrades never overwrite it.
# Edit it on a PC (any text editor) or on the TV. Lines starting with # are comments.

[settings]
# SAVER_MIN = screen saver after N minutes without input (0 = off)
# NOSLEEP   = 1: keep the TV on (LG's Auto Off / No-signal / No-operation timers off until power off)
# LITE      = 1: stop optional LG services (browsers, voice, Skype, ...) for more free RAM
# NETKEEP   = 1: Wi-Fi keep-alive (the TV can't hear ARP broadcasts; without it the router loses
#             the TV for ~5 min every ~20 min)
# The Settings app updates this section.
EOF
    S=$(_ncconf_old "$N/home/settings.conf")
    if [ -n "$S" ]; then echo "$S"; else printf 'SAVER_MIN=20\nNOSLEEP=1\nLITE=0\nNETKEEP=1\n'; fi

    cat <<'EOF'

[photos]
# Photo server: any HTTP server with directory listings, e.g. on a PC:
#     cd ~/Pictures && python3 -m http.server 8000
# then set (use the PC's LAN address):
#   url=http://192.168.1.10:8000/
# The line below shows the test photos in netcast/test/photos/ (tools/webtest.sh serves them).
EOF
    S=$(_ncconf_old "$N/apps/photos/photos.conf")
    if [ -n "$S" ]; then echo "$S"; else echo 'url=http://127.0.0.1:8090/photos/'; fi

    cat <<'EOF'

[player]
# One stream per line:  Name=URI
#   Movie  = file:///mnt/usb/usb2/Drive1/videos/movie.mkv
#   PC     = http://192.168.1.10:8000/stream.ts      (HTTP progressive MP4/MKV/TS)
#   Camera = rtsp://192.168.1.20:554/live            (IP cameras, RTSP servers)
#   Multicast = udp://239.0.0.1:1234                 (MPEG-TS, e.g. from VLC/ffmpeg)
# Hardware decoding: H.264 video with MP3 audio in MPEG-TS works reliably; MP4 files, AAC/AC-3 audio and
# MPEG-2 video don't in this player. For those (and VP9 etc.): Network Shares, YELLOW = ffplay.
# The test clips below are in netcast/test/video/ (tools/webtest.sh serves them).
EOF
    S=$(_ncconf_old "$N/apps/player/streams.conf")
    if [ -n "$S" ]; then echo "$S"; else
      echo 'Test H.264 720p = http://127.0.0.1:8090/video/test-h264-mp3-720p.ts'
    fi

    cat <<'EOF'

[shares]
# Network Shares: one server or share per line.
#   Name = smb://[DOMAIN;]user[:password]@host[:port]/[share[/folder]]
# Leave out the share to list all shares of a server. No user = guest.
# Escape special characters in the password: @ -> %40, : -> %3A, space -> %20, % -> %25
# SMB2/3 servers only (Windows 7+, Samba 3.6+, any current NAS).
# The USB stick is always listed too (its netcast/test/ folder has test videos and music).
#   NAS music  = smb://me:secret@192.168.1.20/music
#   Windows PC = smb://me:secret@192.168.1.30/
#   Guest      = smb://192.168.1.20/public
#   Movies     = smb://me:secret@nas.lan/media/Movies
#   Work       = smb://CORP;me:secret@fileserver/home
EOF
    _ncconf_old "$N/apps/smb/shares.conf"

    cat <<'EOF'

[rdp]
# Remote Desktop: one line of FreeRDP arguments (the first line is used). Missing user/password
# are asked for on screen (USB keyboard).
#   /v:192.168.1.20 /u:Administrator
#   /v:myhost.lan:3389 /u:me /d:WORKGROUP /p:secret
EOF
    _ncconf_old "$N/apps/rdp/rdp.conf" | head -1
    echo
    _ncconf_browser
  } > "$NETCAST_CONF.tmp" && mv "$NETCAST_CONF.tmp" "$NETCAST_CONF" && echo "ncconf: created $NETCAST_CONF"
}

_ncconf_browser() {
  cat <<'EOF'
[browser]
# Web Browser start page. Without home= it opens the local test pages (tools/webtest.sh, netcast/test/),
# since the TV may have no internet access.
#   home=http://192.168.1.10:8000/
#   home=https://www.netsurf-browser.org/
EOF
}
