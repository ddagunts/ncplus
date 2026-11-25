"""Entities against the fake ncapi."""

from __future__ import annotations

import pytest
from pytest_homeassistant_custom_component.common import MockConfigEntry

from homeassistant.components.media_player import (
    ATTR_APP_ID,
    ATTR_INPUT_SOURCE,
    ATTR_INPUT_SOURCE_LIST,
    ATTR_MEDIA_VOLUME_MUTED,
    DOMAIN as MP,
    SERVICE_SELECT_SOURCE,
)
from homeassistant.components.remote import ATTR_ACTIVITY, ATTR_COMMAND, ATTR_DELAY_SECS, ATTR_HOLD_SECS, ATTR_NUM_REPEATS
from homeassistant.config_entries import ConfigEntryState
from homeassistant.const import (
    ATTR_ENTITY_ID,
    SERVICE_TURN_OFF,
    SERVICE_TURN_ON,
    SERVICE_VOLUME_DOWN,
    SERVICE_VOLUME_MUTE,
    SERVICE_VOLUME_UP,
    STATE_OFF,
    STATE_ON,
    STATE_UNAVAILABLE,
)
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError, ServiceValidationError
from homeassistant.helpers import device_registry as dr, entity_registry as er

from custom_components.netcast_plus.const import DOMAIN
from custom_components.netcast_plus.diagnostics import async_get_config_entry_diagnostics

from .fake_ncapi import FakeNcapi

TV = "media_player.lg_tv"
REMOTE = "remote.lg_tv_remote"
APP = "sensor.lg_tv_current_app"
SAVER = "select.lg_tv_screen_saver"
NETKEEP = "switch.lg_tv_wi_fi_keep_alive"


async def _poll(hass: HomeAssistant, entry: MockConfigEntry) -> None:
    await entry.runtime_data.async_refresh()
    await hass.async_block_till_done()


async def test_entities_and_device(hass: HomeAssistant, fake: FakeNcapi, setup: MockConfigEntry) -> None:
    ents = sorted(e.entity_id for e in er.async_entries_for_config_entry(er.async_get(hass), setup.entry_id))
    assert ents == [
        TV, REMOTE, SAVER, APP,
        "switch.lg_tv_keep_tv_awake", "switch.lg_tv_lite_mode", NETKEEP,
    ]   # no entity for the read-only API switch
    dev = dr.async_get(hass).async_get_device_by_identifier((DOMAIN, fake.mac), setup.entry_id)
    assert dev.manufacturer == "LG Electronics" and dev.sw_version == "test-1"
    assert (dr.CONNECTION_NETWORK_MAC, fake.mac) in dev.connections

    s = hass.states.get(TV)
    assert s.state == STATE_ON
    assert s.attributes[ATTR_INPUT_SOURCE_LIST] == ["LG", "ncplus", "Doom", "Music", "Settings"]
    assert s.attributes[ATTR_INPUT_SOURCE] == "LG"
    assert hass.states.get(APP).state == "LG"
    assert hass.states.get(SAVER).state == "20"
    assert hass.states.get(SAVER).attributes["options"] == ["0", "5", "10", "20", "30", "60"]
    assert hass.states.get(NETKEEP).state == STATE_ON
    assert hass.states.get(REMOTE).attributes["activity_list"] == ["LG", "ncplus", "Doom", "Music", "Settings"]


async def test_volume_and_mute(hass: HomeAssistant, fake: FakeNcapi, setup) -> None:
    await hass.services.async_call(MP, SERVICE_VOLUME_UP, {ATTR_ENTITY_ID: TV}, blocking=True)
    await hass.services.async_call(MP, SERVICE_VOLUME_DOWN, {ATTR_ENTITY_ID: TV}, blocking=True)
    await hass.services.async_call(MP, SERVICE_VOLUME_MUTE, {ATTR_ENTITY_ID: TV, ATTR_MEDIA_VOLUME_MUTED: True}, blocking=True)
    assert fake.keys == [{"key": "volup"}, {"key": "voldown"}, {"key": "mute"}]


async def test_select_source(hass: HomeAssistant, fake: FakeNcapi, setup: MockConfigEntry) -> None:
    await hass.services.async_call(MP, SERVICE_SELECT_SOURCE, {ATTR_ENTITY_ID: TV, ATTR_INPUT_SOURCE: "Doom"}, blocking=True)
    assert fake.launches == ["doom"]
    s = hass.states.get(TV)   # optimistic, before any poll
    assert s.attributes[ATTR_INPUT_SOURCE] == "Doom" and s.attributes[ATTR_APP_ID] == "doom"
    await _poll(hass, setup)
    assert hass.states.get(APP).state == "Doom"

    await hass.services.async_call(MP, SERVICE_SELECT_SOURCE, {ATTR_ENTITY_ID: TV, ATTR_INPUT_SOURCE: "ncplus"}, blocking=True)
    await hass.services.async_call(MP, SERVICE_SELECT_SOURCE, {ATTR_ENTITY_ID: TV, ATTR_INPUT_SOURCE: "LG"}, blocking=True)
    assert fake.launches == ["doom", "launcher", "exit"]
    await _poll(hass, setup)
    assert hass.states.get(TV).attributes[ATTR_INPUT_SOURCE] == "LG"

    with pytest.raises(ServiceValidationError):
        await hass.services.async_call(MP, SERVICE_SELECT_SOURCE, {ATTR_ENTITY_ID: TV, ATTR_INPUT_SOURCE: "Nope"}, blocking=True)


async def test_duplicate_app_titles(hass: HomeAssistant, fake: FakeNcapi, entry: MockConfigEntry) -> None:
    fake.apps.append({"id": "doom2", "title": "Doom", "desc": "", "order": 11})
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    assert hass.states.get(TV).attributes[ATTR_INPUT_SOURCE_LIST] == ["LG", "ncplus", "Doom", "Music", "Settings", "Doom (doom2)"]
    await hass.services.async_call(MP, SERVICE_SELECT_SOURCE, {ATTR_ENTITY_ID: TV, ATTR_INPUT_SOURCE: "Doom (doom2)"}, blocking=True)
    assert fake.launches == ["doom2"]
    await _poll(hass, entry)
    assert hass.states.get(TV).attributes[ATTR_INPUT_SOURCE] == "Doom (doom2)"


async def test_new_app_is_picked_up(hass: HomeAssistant, fake: FakeNcapi, setup: MockConfigEntry, freezer) -> None:
    fake.apps.append({"id": "snes", "title": "SNES", "desc": "", "order": 30})
    fake.netcast, fake.app = True, "snes"
    freezer.tick(61)
    await _poll(hass, setup)
    assert "SNES" in hass.states.get(TV).attributes[ATTR_INPUT_SOURCE_LIST]
    assert hass.states.get(APP).state == "SNES"


async def test_turn_off_on(hass: HomeAssistant, fake: FakeNcapi, setup: MockConfigEntry) -> None:
    await hass.services.async_call(MP, SERVICE_TURN_OFF, {ATTR_ENTITY_ID: TV}, blocking=True)
    assert fake.screens == [False]
    assert hass.states.get(TV).state == STATE_OFF
    assert hass.states.get(REMOTE).state == STATE_OFF
    await _poll(hass, setup)
    assert hass.states.get(TV).state == STATE_OFF
    await hass.services.async_call(MP, SERVICE_TURN_ON, {ATTR_ENTITY_ID: TV}, blocking=True)
    assert fake.screens == [False, True]
    assert hass.states.get(TV).state == STATE_ON


async def test_remote_send_command(hass: HomeAssistant, fake: FakeNcapi, setup) -> None:
    await hass.services.async_call(
        "remote", "send_command", {ATTR_ENTITY_ID: REMOTE, ATTR_COMMAND: ["Up", "ok", "115", "5"]}, blocking=True
    )
    assert fake.keys == [{"key": "up"}, {"key": "ok"}, {"code": 115}, {"key": "5"}]
    fake.keys.clear()
    await hass.services.async_call(
        "remote", "send_command",
        {ATTR_ENTITY_ID: REMOTE, ATTR_COMMAND: "down", ATTR_NUM_REPEATS: 3, ATTR_DELAY_SECS: 0.1, ATTR_HOLD_SECS: 0.5},
        blocking=True,
    )
    assert fake.keys == [{"key": "down", "repeat": 3, "hold_ms": 500, "delay_ms": 100}]

    with pytest.raises(HomeAssistantError, match="unknown key name"):
        await hass.services.async_call("remote", "send_command", {ATTR_ENTITY_ID: REMOTE, ATTR_COMMAND: "bogus"}, blocking=True)
    with pytest.raises(HomeAssistantError, match="IN-STOP"):
        await hass.services.async_call("remote", "send_command", {ATTR_ENTITY_ID: REMOTE, ATTR_COMMAND: "897"}, blocking=True)
    with pytest.raises(ServiceValidationError):
        await hass.services.async_call(
            "remote", "send_command", {ATTR_ENTITY_ID: REMOTE, ATTR_COMMAND: "up", ATTR_NUM_REPEATS: 500}, blocking=True
        )


async def test_remote_activity(hass: HomeAssistant, fake: FakeNcapi, setup) -> None:
    fake.screen = "off"
    await setup.runtime_data.async_refresh()
    await hass.services.async_call("remote", SERVICE_TURN_ON, {ATTR_ENTITY_ID: REMOTE, ATTR_ACTIVITY: "Music"}, blocking=True)
    assert fake.screens == [True] and fake.launches == ["music"]
    assert hass.states.get(REMOTE).attributes["current_activity"] == "Music"
    await hass.services.async_call("remote", SERVICE_TURN_OFF, {ATTR_ENTITY_ID: REMOTE}, blocking=True)
    assert fake.screens == [True, False]


async def test_settings(hass: HomeAssistant, fake: FakeNcapi, setup) -> None:
    await hass.services.async_call("switch", SERVICE_TURN_OFF, {ATTR_ENTITY_ID: NETKEEP}, blocking=True)
    assert fake.puts == [{"NETKEEP": 0}]
    assert hass.states.get(NETKEEP).state == STATE_OFF
    await hass.services.async_call("select", "select_option", {ATTR_ENTITY_ID: SAVER, "option": "30"}, blocking=True)
    assert fake.puts[-1] == {"SAVER_MIN": 30}
    assert hass.states.get(SAVER).state == "30"
    # changed on the TV (Settings app): the next poll shows it
    fake.settings["LITE"]["value"] = 1
    await _poll(hass, setup)
    assert hass.states.get("switch.lg_tv_lite_mode").state == STATE_ON


async def test_tv_unreachable_then_back(hass: HomeAssistant, fake: FakeNcapi, setup: MockConfigEntry) -> None:
    await fake.stop()
    with pytest.raises(HomeAssistantError, match="No answer"):   # before the next poll notices
        await hass.services.async_call(MP, SERVICE_VOLUME_UP, {ATTR_ENTITY_ID: TV}, blocking=True)
    await _poll(hass, setup)
    assert hass.states.get(TV).state == STATE_UNAVAILABLE
    await fake.start()   # same port
    await _poll(hass, setup)
    assert hass.states.get(TV).state == STATE_ON


async def test_setup_retry_when_tv_off(hass: HomeAssistant, fake: FakeNcapi, entry: MockConfigEntry) -> None:
    await fake.stop()
    await hass.config_entries.async_setup(entry.entry_id)
    assert entry.state is ConfigEntryState.SETUP_RETRY


async def test_token_revoked_starts_reauth_on_action(hass: HomeAssistant, fake: FakeNcapi, setup) -> None:
    fake.token = "d" * 32
    with pytest.raises(HomeAssistantError):
        await hass.services.async_call(MP, SERVICE_VOLUME_UP, {ATTR_ENTITY_ID: TV}, blocking=True)
    await hass.async_block_till_done()
    assert [f["context"]["source"] for f in hass.config_entries.flow.async_progress_by_handler(DOMAIN)] == ["reauth"]


async def test_unload(hass: HomeAssistant, setup: MockConfigEntry) -> None:
    assert await hass.config_entries.async_unload(setup.entry_id)
    assert setup.state is ConfigEntryState.NOT_LOADED


async def test_diagnostics_redacts(hass: HomeAssistant, fake: FakeNcapi, setup: MockConfigEntry) -> None:
    d = await async_get_config_entry_diagnostics(hass, setup)
    assert d["entry"]["token"] == "**REDACTED**" and d["status"]["id"] == "**REDACTED**"
    assert fake.token not in repr(d) and fake.mac not in repr(d)
    assert d["settings"]["SAVER_MIN"]["value"] == 20
