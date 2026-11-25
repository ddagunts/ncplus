"""ncplus settings with a fixed set of values (screen-saver minutes: 0, 5, 10, 20, 30, 60)."""

from __future__ import annotations

from typing import override

from homeassistant.components.select import SelectEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .api import Setting
from .coordinator import NetCastPlusConfigEntry, NetCastPlusCoordinator
from .switch import NetCastPlusSettingEntity

PARALLEL_UPDATES = 1


async def async_setup_entry(
    hass: HomeAssistant, entry: NetCastPlusConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    c = entry.runtime_data
    async_add_entities(
        NetCastPlusSettingSelect(c, s) for s in c.data.settings.values() if not s.is_bool and not s.readonly and s.options
    )


class NetCastPlusSettingSelect(NetCastPlusSettingEntity, SelectEntity):
    # A select, not a number: the TV only accepts the listed values (SAVER_MIN=15 is refused).

    def __init__(self, coordinator: NetCastPlusCoordinator, setting: Setting) -> None:
        super().__init__(coordinator, setting)
        self._attr_options = [str(o) for o in setting.options]

    @property
    @override
    def current_option(self) -> str | None:
        s = self.setting
        return None if s is None else str(s.value)

    @override
    async def async_select_option(self, option: str) -> None:
        await self._async_put(int(option))
