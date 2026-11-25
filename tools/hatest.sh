#!/bin/bash
# hatest.sh: run the Home Assistant integration's tests (tests/ha, for custom_components/netcast_plus).
#
#   tools/hatest.sh [pytest args]
#       1. against the in-memory fake API (tests/ha/fake_ncapi.py), and
#       2. against the real ncapi built for the host, through apitest.sh --local --run (test_real_ncapi.py).
#
# The test environment (Python 3.14 + pytest-homeassistant-custom-component, which pins one Home Assistant
# release) is built on first use in ${NETCAST_HA_ENV:-~/.cache/netcast-ha}, with a uv binary taken from its
# PyPI wheel (no ensurepip/python3-venv needed). PHCC=<version> picks another harness release.
set -uo pipefail
S=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$S/.." && pwd)
C=${NETCAST_HA_ENV:-$HOME/.cache/netcast-ha}
PHCC=${PHCC:-0.13.375}   # = Home Assistant 2026.10.4
PY=$C/venv/bin/python
export UV_PYTHON_INSTALL_DIR=$C/python UV_CACHE_DIR=$C/uv-cache

if ! "$PY" -c "import pytest_homeassistant_custom_component, importlib.metadata as m, sys; sys.exit(m.version('pytest-homeassistant-custom-component') != '$PHCC')" 2>/dev/null; then
  echo "hatest: setting up $C (pytest-homeassistant-custom-component $PHCC)"
  mkdir -p "$C/bin"
  if [ ! -x "$C/bin/uv" ]; then
    W=$(mktemp -d)
    pip download -q --no-deps --only-binary=:all: uv -d "$W" && (cd "$W" && unzip -q -o uv-*.whl) \
      && cp "$W"/uv-*.data/scripts/uv "$C/bin/uv" && chmod +x "$C/bin/uv"
    rm -rf "$W"
    [ -x "$C/bin/uv" ] || { echo "hatest: could not get uv"; exit 1; }
  fi
  [ -x "$PY" ] || "$C/bin/uv" venv -q -p 3.14 "$C/venv" || exit 1
  VIRTUAL_ENV=$C/venv "$C/bin/uv" pip install -q "pytest-homeassistant-custom-component==$PHCC" || exit 1
fi
echo "hatest: Home Assistant $("$PY" -c 'from homeassistant.const import __version__; print(__version__)')"

cd "$TOP/tests/ha" || exit 1
echo "== fake API"
"$PY" -m pytest -q -p no:cacheprovider "$@" || exit 1
echo "== real ncapi (host build)"
"$S/apitest.sh" --local --run "'$PY' -m pytest -q -p no:cacheprovider test_real_ncapi.py $*"
