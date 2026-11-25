"""What is on the TV: an app's title, "ncplus" (launcher) or "LG"."""

from __future__ import annotations

from typing import override

from homeassistant.components.sensor import SensorEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .coordinator import NetCastPlusConfigEntry, NetCastPlusCoordinator
from .entity import NetCastPlusEntity
from .media_player import current_source

PARALLEL_UPDATES = 0


async def async_setup_entry(
    hass: HomeAssistant, entry: NetCastPlusConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    async_add_entities([NetCastPlusCurrentApp(entry.runtime_data)])


class NetCastPlusCurrentApp(NetCastPlusEntity, SensorEntity):
    _attr_translation_key = "current_app"

    def __init__(self, coordinator: NetCastPlusCoordinator) -> None:
        super().__init__(coordinator, "current_app")

    @property
    @override
    def native_value(self) -> str:
        return current_source(self.coordinator)

    @property
    @override
    def extra_state_attributes(self) -> dict[str, str]:
        return {"app_id": self.coordinator.data.status.app}
