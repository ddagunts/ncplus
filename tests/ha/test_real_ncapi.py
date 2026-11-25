"""The integration against a real ncapi (skipped unless NCAPI_URL and NCAPI_TOKEN are set).

    tools/hatest.sh                     runs this against the host build of ncapi (apitest.sh --local --run)
    NCAPI_URL=http://TV:9080 NCAPI_TOKEN=... pytest test_real_ncapi.py   (in tests/ha)
                                        against the TV: read-only checks only (NCAPI_LOCAL unset)

With NCAPI_LOCAL=1 (the fake tree from apitest.sh) it also presses keys, switches apps, changes settings and
turns the screen off and on, and checks what reached the key device and netcast.conf.
"""

from __future__ import annotations

import asyncio
import os
import re
import struct

import pytest
from pytest_homeassistant_custom_component.common import MockConfigEntry
from yarl import URL

from homeassistant.const import CONF_HOST, CONF_PORT, CONF_TOKEN
from homeassistant.core import HomeAssistant
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from custom_components.netcast_plus.api import NetCastAuthError, NetCastPlusClient, NetCastResponseError
from custom_components.netcast_plus.const import DOMAIN

URL_ = os.environ.get("NCAPI_URL")
TOKEN = os.environ.get("NCAPI_TOKEN", "")
LOCAL = os.environ.get("NCAPI_LOCAL") == "1"

pytestmark = pytest.mark.skipif(not (URL_ and TOKEN), reason="NCAPI_URL / NCAPI_TOKEN not set")
local_only = pytest.mark.skipif(not LOCAL, reason="changes TV state: only against apitest.sh's fake tree")


def _addr() -> tuple[str, int]:
    u = URL(URL_)
    return u.host, u.port or 9080


@pytest.fixture
async def client(hass: HomeAssistant, socket_enabled) -> NetCastPlusClient:
    host, port = _addr()
    return NetCastPlusClient(async_get_clientsession(hass), host, port, TOKEN)


@pytest.fixture
async def real_entry(hass: HomeAssistant, socket_enabled) -> MockConfigEntry:
    host, port = _addr()
    st = await NetCastPlusClient(async_get_clientsession(hass), host, port, TOKEN).status()
    e = MockConfigEntry(domain=DOMAIN, title="LG TV", unique_id=st.id or f"{host}:{port}",
                        data={CONF_HOST: host, CONF_PORT: port, CONF_TOKEN: TOKEN})
    e.add_to_hass(hass)
    assert await hass.config_entries.async_setup(e.entry_id)
    await hass.async_block_till_done()
    return e


async def _wait(hass: HomeAssistant, entry: MockConfigEntry, entity: str, attr: str | None, want: str, secs: float = 8) -> None:
    for _ in range(int(secs * 5)):
        await entry.runtime_data.async_refresh()
        await hass.async_block_till_done()
        s = hass.states.get(entity)
        if (s.attributes.get(attr) if attr else s.state) == want:
            return
        await asyncio.sleep(0.2)
    pytest.fail(f"{entity} {attr or 'state'} != {want!r}: {s}")


def _events() -> list[tuple[int, int, int]]:
    d = open(os.environ["NCAPI_KEYDEV"], "rb").read()
    n = struct.calcsize("llHHi")
    return [struct.unpack_from("llHHi", d, i)[2:] for i in range(0, len(d) - n + 1, n)]


async def test_client_parses_every_reply(client: NetCastPlusClient) -> None:
    st = await client.status()
    assert st.app and st.title and st.version
    assert re.fullmatch(r"([0-9a-f]{2}:){5}[0-9a-f]{2}", st.id), st.raw
    settings = await client.settings()
    assert {"SAVER_MIN", "NOSLEEP", "LITE", "NETKEEP", "API"} <= set(settings)
    assert settings["API"].readonly and settings["NETKEEP"].is_bool and not settings["SAVER_MIN"].is_bool
    apps = await client.apps()
    assert apps and all(a.id and a.title for a in apps)
    assert {"up", "ok", "back", "volup", "voldown", "mute"} <= {k["name"] for k in await client.keys()}


async def test_errors(hass: HomeAssistant, client: NetCastPlusClient, socket_enabled) -> None:
    host, port = _addr()
    with pytest.raises(NetCastAuthError):
        await NetCastPlusClient(async_get_clientsession(hass), host, port, "0" * 32).status()
    with pytest.raises(NetCastResponseError, match="unknown key"):
        await client.key("nosuchkey")
    with pytest.raises(NetCastResponseError, match="403: refused"):
        await client.key(897)
    with pytest.raises(NetCastResponseError, match="400: SAVER_MIN must be one of"):
        await client.set_settings({"SAVER_MIN": 7})
    with pytest.raises(NetCastResponseError, match="403"):
        await client.set_settings({"API": 0})


async def test_entities(hass: HomeAssistant, real_entry: MockConfigEntry) -> None:
    ents = {e.entity_id for e in er.async_entries_for_config_entry(er.async_get(hass), real_entry.entry_id)}
    assert {"media_player.lg_tv", "remote.lg_tv_remote", "sensor.lg_tv_current_app", "select.lg_tv_screen_saver",
            "switch.lg_tv_keep_tv_awake", "switch.lg_tv_lite_mode", "switch.lg_tv_wi_fi_keep_alive"} == ents
    s = hass.states.get("media_player.lg_tv")
    assert s.state in ("on", "off") and s.attributes["source_list"][:2] == ["LG", "ncplus"]


@local_only
async def test_keys_reach_the_device(hass: HomeAssistant, real_entry: MockConfigEntry) -> None:
    await hass.services.async_call("media_player", "volume_up", {"entity_id": "media_player.lg_tv"}, blocking=True)
    await hass.services.async_call(
        "remote", "send_command", {"entity_id": "remote.lg_tv_remote", "command": ["ok", "412"], "num_repeats": 2, "delay_secs": 0.05},
        blocking=True,
    )
    await asyncio.sleep(0.8)
    presses = [(c, v) for t, c, v in _events() if t == 1]
    assert presses == [(115, 1), (115, 0), (28, 1), (28, 0), (28, 1), (28, 0), (412, 1), (412, 0), (412, 1), (412, 0)]


@local_only
async def test_switch_apps(hass: HomeAssistant, real_entry: MockConfigEntry) -> None:
    tv = "media_player.lg_tv"
    await hass.services.async_call("media_player", "select_source", {"entity_id": tv, "source": "Doom"}, blocking=True)
    await _wait(hass, real_entry, tv, "source", "Doom")
    await hass.services.async_call("media_player", "select_source", {"entity_id": tv, "source": "Music"}, blocking=True)
    await _wait(hass, real_entry, "sensor.lg_tv_current_app", None, "Music")
    await hass.services.async_call("media_player", "select_source", {"entity_id": tv, "source": "LG"}, blocking=True)
    await _wait(hass, real_entry, tv, "source", "LG")


@local_only
async def test_settings_and_screen(hass: HomeAssistant, real_entry: MockConfigEntry) -> None:
    await hass.services.async_call("select", "select_option", {"entity_id": "select.lg_tv_screen_saver", "option": "60"}, blocking=True)
    await hass.services.async_call("switch", "turn_on", {"entity_id": "switch.lg_tv_lite_mode"}, blocking=True)
    conf = open(os.environ["NCAPI_CONF"]).read()
    assert re.search(r"^SAVER_MIN=60\r?$", conf, re.M) and re.search(r"^LITE=1$", conf, re.M), conf
    assert hass.states.get("switch.lg_tv_lite_mode").state == "on"

    await hass.services.async_call("media_player", "turn_off", {"entity_id": "media_player.lg_tv"}, blocking=True)
    await _wait(hass, real_entry, "media_player.lg_tv", None, "off")
    await hass.services.async_call("media_player", "turn_on", {"entity_id": "media_player.lg_tv"}, blocking=True)
    await _wait(hass, real_entry, "media_player.lg_tv", None, "on")
