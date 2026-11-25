"""The TV remote: remote.send_command with key names (up, ok, back, red, volup, ...) or evdev codes."""

from __future__ import annotations

import asyncio
from collections.abc import Iterable
from typing import Any, override

from homeassistant.components.remote import (
    ATTR_ACTIVITY,
    ATTR_DELAY_SECS,
    ATTR_HOLD_SECS,
    ATTR_NUM_REPEATS,
    DEFAULT_DELAY_SECS,
    RemoteEntity,
    RemoteEntityFeature,
)
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import ServiceValidationError
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .const import DOMAIN
from .coordinator import NetCastPlusConfigEntry, NetCastPlusCoordinator
from .entity import NetCastPlusEntity
from .media_player import async_launch_source, async_screen, current_source, sources

PARALLEL_UPDATES = 1

# The API's limits (src/tools/ncapi.c h_key)
MAX_REPEAT = 100
HOLD_MS = (20, 10000)
DELAY_MS = (0, 10000)
DEFAULT_HOLD_MS = 80


async def async_setup_entry(
    hass: HomeAssistant, entry: NetCastPlusConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    async_add_entities([NetCastPlusRemote(entry.runtime_data)])


def _invalid(key: str, **ph: Any) -> ServiceValidationError:
    return ServiceValidationError(translation_domain=DOMAIN, translation_key=key, translation_placeholders=ph or None)


class NetCastPlusRemote(NetCastPlusEntity, RemoteEntity):
    _attr_translation_key = "remote"
    _attr_supported_features = RemoteEntityFeature.ACTIVITY

    def __init__(self, coordinator: NetCastPlusCoordinator) -> None:
        super().__init__(coordinator, "remote")

    @property
    @override
    def is_on(self) -> bool:
        return self.coordinator.data.status.screen_on

    @property
    @override
    def activity_list(self) -> list[str]:
        return list(sources(self.coordinator))

    @property
    @override
    def current_activity(self) -> str:
        return current_source(self.coordinator)

    @override
    async def async_turn_on(self, activity: str | None = None, **kwargs: Any) -> None:
        if not self.coordinator.data.status.screen_on:
            await async_screen(self.coordinator, True)
        if activity:
            await async_launch_source(self.coordinator, activity)

    @override
    async def async_turn_off(self, **kwargs: Any) -> None:
        await async_screen(self.coordinator, False)

    @override
    async def async_send_command(self, command: Iterable[str], **kwargs: Any) -> None:
        repeat = int(kwargs.get(ATTR_NUM_REPEATS) or 1)
        delay_ms = round(float(kwargs.get(ATTR_DELAY_SECS, DEFAULT_DELAY_SECS)) * 1000)
        hold = kwargs.get(ATTR_HOLD_SECS)
        hold_ms = round(float(hold) * 1000) if hold else None
        if not 1 <= repeat <= MAX_REPEAT:
            raise _invalid("bad_repeat", max=str(MAX_REPEAT))
        if not DELAY_MS[0] <= delay_ms <= DELAY_MS[1]:
            raise _invalid("bad_delay")
        if hold_ms is not None and not HOLD_MS[0] <= hold_ms <= HOLD_MS[1]:
            raise _invalid("bad_hold")
        keys: list[str | int] = []
        for c in command:
            c = str(c).strip()
            if not c:
                continue
            keys.append(int(c) if c.isdigit() and len(c) > 1 else c.lower())   # "0".."9" are key names
        client = self.coordinator.client
        for i, key in enumerate(keys):
            if i:
                # The TV queues presses after the ones before, but HA's delay_secs also goes between
                # commands: wait out the previous command's presses, then the delay.
                held = hold_ms if hold_ms is not None else DEFAULT_HOLD_MS
                await asyncio.sleep((repeat * held + (repeat - 1) * delay_ms + delay_ms) / 1000)
            await self.coordinator.async_call(
                client.key(key, repeat=repeat, hold_ms=hold_ms, delay_ms=delay_ms if repeat > 1 else None)
            )
