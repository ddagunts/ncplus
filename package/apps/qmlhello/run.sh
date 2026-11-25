#!/bin/sh
# QML Hello World (static Qt 6 + eglfs_netcast)
D=$(cd "$(dirname "$0")" && pwd)
export QT_QPA_PLATFORM=eglfs QT_QPA_EGLFS_INTEGRATION=eglfs_netcast QSG_RENDER_LOOP=basic
export QT_QPA_FONTDIR="$D/fonts" QT_QPA_EGLFS_NO_LIBINPUT=1 XDG_RUNTIME_DIR=/tmp/ncqt-runtime
export QT_QPA_EVDEV_KEYBOARD_PARAMETERS=grab=0 QT_QPA_EVDEV_MOUSE_PARAMETERS=grab=0
mkdir -p /tmp/ncqt-runtime && chmod 700 /tmp/ncqt-runtime
exec sh "$D/../run-app.sh" "$D/qmlhello" "$@"
