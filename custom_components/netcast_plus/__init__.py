"""ncplus for LG NetCast TVs: remote keys, ncplus apps and settings through the TV's ncapi."""

from __future__ import annotations

from homeassistant.const import Platform
from homeassistant.core import HomeAssistant

from .coordinator import NetCastPlusConfigEntry, NetCastPlusCoordinator

PLATFORMS = [Platform.MEDIA_PLAYER, Platform.REMOTE, Platform.SELECT, Platform.SENSOR, Platform.SWITCH]


async def async_setup_entry(hass: HomeAssistant, entry: NetCastPlusConfigEntry) -> bool:
    coordinator = NetCastPlusCoordinator(hass, entry)
    await coordinator.async_config_entry_first_refresh()
    entry.runtime_data = coordinator
    entry.async_on_unload(coordinator.async_cancel_later)
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: NetCastPlusConfigEntry) -> bool:
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
