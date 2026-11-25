"""Fixtures: a fake ncapi on 127.0.0.1 and a config entry pointing at it."""

from __future__ import annotations

from collections.abc import AsyncGenerator

import pytest
from pytest_homeassistant_custom_component.common import MockConfigEntry

from homeassistant.const import CONF_HOST, CONF_PORT, CONF_TOKEN
from homeassistant.core import HomeAssistant

from custom_components.netcast_plus.const import DOMAIN

from .fake_ncapi import FakeNcapi

pytest_plugins = ["pytest_homeassistant_custom_component"]


@pytest.fixture(autouse=True)
def auto_enable_custom_integrations(enable_custom_integrations):
    yield


@pytest.fixture
async def fake(socket_enabled) -> AsyncGenerator[FakeNcapi]:
    f = FakeNcapi()
    await f.start()
    yield f
    await f.stop()


@pytest.fixture
def entry(hass: HomeAssistant, fake: FakeNcapi) -> MockConfigEntry:
    e = MockConfigEntry(
        domain=DOMAIN,
        title="LG TV",
        unique_id=fake.mac,
        data={CONF_HOST: "127.0.0.1", CONF_PORT: fake.port, CONF_TOKEN: fake.token},
    )
    e.add_to_hass(hass)
    return e


@pytest.fixture
async def setup(hass: HomeAssistant, entry: MockConfigEntry) -> MockConfigEntry:
    assert await hass.config_entries.async_setup(entry.entry_id)
    await hass.async_block_till_done()
    return entry
