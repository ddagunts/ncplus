"""Config flow: user, reauth, reconfigure."""

from __future__ import annotations

import socket

from pytest_homeassistant_custom_component.common import MockConfigEntry

from homeassistant import config_entries
from homeassistant.const import CONF_HOST, CONF_PORT, CONF_TOKEN
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType

from custom_components.netcast_plus.const import DOMAIN

from .fake_ncapi import FakeNcapi


def _free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


async def _start(hass: HomeAssistant):
    r = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})
    assert r["type"] is FlowResultType.FORM and r["step_id"] == "user"
    return r


async def test_user_flow(hass: HomeAssistant, fake: FakeNcapi) -> None:
    r = await _start(hass)
    r = await hass.config_entries.flow.async_configure(
        r["flow_id"], {CONF_HOST: " 127.0.0.1 ", CONF_PORT: float(fake.port), CONF_TOKEN: f"Bearer {fake.token} "}
    )
    assert r["type"] is FlowResultType.CREATE_ENTRY
    assert r["title"] == "LG TV"
    assert r["data"] == {CONF_HOST: "127.0.0.1", CONF_PORT: fake.port, CONF_TOKEN: fake.token}
    assert r["result"].unique_id == fake.mac


async def test_user_flow_errors_then_success(hass: HomeAssistant, fake: FakeNcapi) -> None:
    r = await _start(hass)
    good = {CONF_HOST: "127.0.0.1", CONF_PORT: fake.port, CONF_TOKEN: fake.token}

    r = await hass.config_entries.flow.async_configure(r["flow_id"], {**good, CONF_TOKEN: "not hex"})
    assert r["type"] is FlowResultType.FORM and r["errors"] == {CONF_TOKEN: "invalid_token_format"}

    r = await hass.config_entries.flow.async_configure(r["flow_id"], {**good, CONF_TOKEN: "f" * 32})
    assert r["errors"] == {CONF_TOKEN: "invalid_auth"}

    r = await hass.config_entries.flow.async_configure(r["flow_id"], {**good, CONF_PORT: _free_port()})
    assert r["errors"] == {"base": "cannot_connect"}

    r = await hass.config_entries.flow.async_configure(r["flow_id"], good)
    assert r["type"] is FlowResultType.CREATE_ENTRY


async def test_not_netcast(hass: HomeAssistant, fake: FakeNcapi) -> None:
    fake.status_json = lambda: {"hello": "world"}
    r = await _start(hass)
    r = await hass.config_entries.flow.async_configure(
        r["flow_id"], {CONF_HOST: "127.0.0.1", CONF_PORT: fake.port, CONF_TOKEN: fake.token}
    )
    assert r["errors"] == {"base": "not_netcast"}


async def test_already_configured_updates_address(hass: HomeAssistant, fake: FakeNcapi) -> None:
    old = MockConfigEntry(domain=DOMAIN, unique_id=fake.mac, data={CONF_HOST: "10.0.0.9", CONF_PORT: 9080, CONF_TOKEN: "a" * 32})
    old.add_to_hass(hass)
    r = await _start(hass)
    r = await hass.config_entries.flow.async_configure(
        r["flow_id"], {CONF_HOST: "127.0.0.1", CONF_PORT: fake.port, CONF_TOKEN: fake.token}
    )
    assert r["type"] is FlowResultType.ABORT and r["reason"] == "already_configured"
    assert old.data == {CONF_HOST: "127.0.0.1", CONF_PORT: fake.port, CONF_TOKEN: fake.token}


async def test_no_mac_falls_back_to_address(hass: HomeAssistant, fake: FakeNcapi) -> None:
    fake.mac = ""
    r = await _start(hass)
    r = await hass.config_entries.flow.async_configure(
        r["flow_id"], {CONF_HOST: "127.0.0.1", CONF_PORT: fake.port, CONF_TOKEN: fake.token}
    )
    assert r["result"].unique_id == f"127.0.0.1:{fake.port}"


async def test_reauth(hass: HomeAssistant, fake: FakeNcapi, setup: MockConfigEntry) -> None:
    fake.token = "b" * 32   # "New token" on the TV
    await setup.runtime_data.async_refresh()
    await hass.async_block_till_done()
    flows = hass.config_entries.flow.async_progress_by_handler(DOMAIN)
    assert [f["context"]["source"] for f in flows] == [config_entries.SOURCE_REAUTH]
    fid = flows[0]["flow_id"]

    r = await hass.config_entries.flow.async_configure(fid, {CONF_TOKEN: "c" * 32})
    assert r["type"] is FlowResultType.FORM and r["errors"] == {CONF_TOKEN: "invalid_auth"}
    r = await hass.config_entries.flow.async_configure(fid, {CONF_TOKEN: "b" * 32})
    assert r["type"] is FlowResultType.ABORT and r["reason"] == "reauth_successful"
    assert setup.data[CONF_TOKEN] == "b" * 32
    await hass.async_block_till_done()
    assert setup.state is config_entries.ConfigEntryState.LOADED


async def test_reconfigure(hass: HomeAssistant, fake: FakeNcapi, setup: MockConfigEntry) -> None:
    old = fake.port
    await fake.stop()   # the TV moves to another port ([api] PORT)
    fake.port = 0
    await fake.start()
    r = await setup.start_reconfigure_flow(hass)
    assert r["step_id"] == "reconfigure"
    r = await hass.config_entries.flow.async_configure(
        r["flow_id"], {CONF_HOST: "127.0.0.1", CONF_PORT: fake.port, CONF_TOKEN: fake.token}
    )
    assert r["type"] is FlowResultType.ABORT and r["reason"] == "reconfigure_successful"
    assert setup.data[CONF_PORT] == fake.port != old
    await hass.async_block_till_done()
    assert setup.state is config_entries.ConfigEntryState.LOADED


async def test_reconfigure_other_tv(hass: HomeAssistant, fake: FakeNcapi, setup: MockConfigEntry) -> None:
    fake.mac = "aa:bb:cc:dd:ee:02"
    r = await setup.start_reconfigure_flow(hass)
    r = await hass.config_entries.flow.async_configure(
        r["flow_id"], {CONF_HOST: "127.0.0.1", CONF_PORT: fake.port, CONF_TOKEN: fake.token}
    )
    assert r["type"] is FlowResultType.ABORT and r["reason"] == "wrong_device"
