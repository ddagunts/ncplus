"""Base entity: one device per TV."""

from __future__ import annotations

from homeassistant.helpers.device_registry import CONNECTION_NETWORK_MAC, DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN
from .coordinator import NetCastPlusCoordinator


class NetCastPlusEntity(CoordinatorEntity[NetCastPlusCoordinator]):
    _attr_has_entity_name = True

    def __init__(self, coordinator: NetCastPlusCoordinator, key: str) -> None:
        super().__init__(coordinator)
        did = coordinator.device_id
        self._attr_unique_id = f"{did}_{key}"
        status = coordinator.data.status
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, did)},
            manufacturer="LG Electronics",
            model="NetCast TV with ncplus",
            name=coordinator.config_entry.title,
            sw_version=status.version,
        )
        if status.id:
            self._attr_device_info["connections"] = {(CONNECTION_NETWORK_MAC, status.id)}
