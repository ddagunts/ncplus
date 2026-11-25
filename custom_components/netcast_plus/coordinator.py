"""Polls the TV's /status and /settings (and /apps now and then)."""

from __future__ import annotations

from dataclasses import dataclass, field, replace
import time
from typing import override

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST, CONF_PORT, CONF_TOKEN
from homeassistant.core import HomeAssistant, callback
from homeassistant.exceptions import ConfigEntryAuthFailed, HomeAssistantError
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.event import async_call_later
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import (
    App,
    NetCastAuthError,
    NetCastConnectionError,
    NetCastError,
    NetCastPlusClient,
    NetCastResponseError,
    Setting,
    Status,
)
from .const import APP_LAUNCHER, APPS_INTERVAL, DOMAIN, LOGGER, SCAN_INTERVAL

type NetCastPlusConfigEntry = ConfigEntry[NetCastPlusCoordinator]


@dataclass
class NetCastPlusData:
    status: Status
    settings: dict[str, Setting]
    apps: list[App] = field(default_factory=list)


class NetCastPlusCoordinator(DataUpdateCoordinator[NetCastPlusData]):
    """One per TV."""

    config_entry: NetCastPlusConfigEntry

    def __init__(self, hass: HomeAssistant, entry: NetCastPlusConfigEntry) -> None:
        self.client = NetCastPlusClient(
            async_get_clientsession(hass), entry.data[CONF_HOST], entry.data[CONF_PORT], entry.data[CONF_TOKEN]
        )
        self._apps: list[App] = []
        self._apps_at = 0.0
        self._later: list = []
        super().__init__(
            hass,
            LOGGER,
            config_entry=entry,
            name=f"{DOMAIN}_{entry.data[CONF_HOST]}",
            update_interval=SCAN_INTERVAL,
        )

    @property
    def device_id(self) -> str:
        """The TV's stable id: config entry unique id (the MAC from /status), else the entry id."""
        return self.config_entry.unique_id or self.config_entry.entry_id

    @override
    async def _async_update_data(self) -> NetCastPlusData:
        try:
            status = await self.client.status()
            settings = await self.client.settings()
            unknown_app = status.netcast and status.app != APP_LAUNCHER and all(a.id != status.app for a in self._apps)
            age = time.monotonic() - self._apps_at
            if not self._apps_at or age > APPS_INTERVAL.total_seconds() or (unknown_app and age > 60):
                self._apps = await self.client.apps()
                self._apps_at = time.monotonic()
        except NetCastAuthError as err:
            raise ConfigEntryAuthFailed(translation_domain=DOMAIN, translation_key="invalid_auth") from err
        except NetCastConnectionError as err:
            raise UpdateFailed(
                translation_domain=DOMAIN, translation_key="cannot_connect", translation_placeholders={"error": str(err)}
            ) from err
        except NetCastResponseError as err:
            raise UpdateFailed(
                translation_domain=DOMAIN, translation_key="api_error", translation_placeholders={"error": str(err)}
            ) from err
        return NetCastPlusData(status=status, settings=settings, apps=self._apps)

    async def async_call(self, coro):
        """Await an API call made for a user action; turn its errors into HomeAssistantError."""
        try:
            return await coro
        except NetCastAuthError as err:
            self.config_entry.async_start_reauth(self.hass)
            raise HomeAssistantError(translation_domain=DOMAIN, translation_key="invalid_auth") from err
        except NetCastConnectionError as err:
            raise HomeAssistantError(
                translation_domain=DOMAIN, translation_key="cannot_connect", translation_placeholders={"error": str(err)}
            ) from err
        except NetCastError as err:
            raise HomeAssistantError(
                translation_domain=DOMAIN, translation_key="api_error", translation_placeholders={"error": str(err)}
            ) from err

    @callback
    def async_set_optimistic(self, **status_changes) -> None:
        """Show a change at once (screen off, new app) until the next poll confirms it."""
        if self.data is None:
            return
        self.async_set_updated_data(replace(self.data, status=replace(self.data.status, **status_changes)))

    @callback
    def async_set_settings(self, settings: dict[str, Setting]) -> None:
        """PUT /settings answers with every setting: use it, no extra poll needed."""
        if self.data is not None:
            self.async_set_updated_data(replace(self.data, settings=settings))

    @callback
    def async_refresh_later(self, *delays: float) -> None:
        """Poll again after an action that takes a moment on the TV (app switch, screen).

        A new action replaces the timers of the one before it."""
        self.async_cancel_later()
        for d in delays:
            self._later.append(async_call_later(self.hass, d, self._refresh_now))

    async def _refresh_now(self, _now) -> None:
        await self.async_request_refresh()

    @callback
    def async_cancel_later(self) -> None:
        for cancel in self._later:
            cancel()
        self._later.clear()
