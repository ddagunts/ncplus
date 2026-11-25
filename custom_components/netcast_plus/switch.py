"""On/off ncplus settings ([settings] keys whose options are 0/1): keep-awake, lite mode, Wi-Fi keep-alive."""

from __future__ import annotations

from typing import Any, override

from homeassistant.components.switch import SwitchEntity
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .api import Setting
from .const import SETTING_TRANSLATION_KEYS
from .coordinator import NetCastPlusConfigEntry, NetCastPlusCoordinator
from .entity import NetCastPlusEntity

PARALLEL_UPDATES = 1


async def async_setup_entry(
    hass: HomeAssistant, entry: NetCastPlusConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    c = entry.runtime_data
    async_add_entities(
        NetCastPlusSettingSwitch(c, s) for s in c.data.settings.values() if s.is_bool and not s.readonly
    )


class NetCastPlusSettingEntity(NetCastPlusEntity):
    """Shared by the switches and selects: one [settings] key."""

    _attr_entity_category = EntityCategory.CONFIG

    def __init__(self, coordinator: NetCastPlusCoordinator, setting: Setting) -> None:
        super().__init__(coordinator, f"setting_{setting.key.lower()}")
        self._key = setting.key
        if tk := SETTING_TRANSLATION_KEYS.get(setting.key):
            self._attr_translation_key = tk
        else:
            self._attr_name = setting.title

    @property
    def setting(self) -> Setting | None:
        return self.coordinator.data.settings.get(self._key)

    @property
    @override
    def available(self) -> bool:
        return super().available and self.setting is not None

    async def _async_put(self, value: int) -> None:
        c = self.coordinator
        c.async_set_settings(await c.async_call(c.client.set_settings({self._key: value})))


class NetCastPlusSettingSwitch(NetCastPlusSettingEntity, SwitchEntity):
    @property
    @override
    def is_on(self) -> bool | None:
        s = self.setting
        return None if s is None else bool(s.value)

    @override
    async def async_turn_on(self, **kwargs: Any) -> None:
        await self._async_put(1)

    @override
    async def async_turn_off(self, **kwargs: Any) -> None:
        await self._async_put(0)
