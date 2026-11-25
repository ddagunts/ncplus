"""The TV as a media player: volume keys, ncplus apps as sources, screen on/off."""

from __future__ import annotations

from typing import override

from homeassistant.components.media_player import (
    MediaPlayerDeviceClass,
    MediaPlayerEntity,
    MediaPlayerEntityFeature,
    MediaPlayerState,
)
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import ServiceValidationError
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .const import APP_LAUNCHER, APP_LG, DOMAIN, SOURCE_LAUNCHER, SOURCE_LG
from .coordinator import NetCastPlusConfigEntry, NetCastPlusCoordinator
from .entity import NetCastPlusEntity

PARALLEL_UPDATES = 1


def sources(coordinator: NetCastPlusCoordinator) -> dict[str, str]:
    """Source name -> launch target. Shared with the remote entity's activities."""
    out = {SOURCE_LG: "exit", SOURCE_LAUNCHER: APP_LAUNCHER}
    for app in coordinator.data.apps:
        name = app.title
        if name in out:   # two apps with one title: tell them apart by folder
            name = f"{app.title} ({app.id})"
        out[name] = app.id
    return out


def current_source(coordinator: NetCastPlusCoordinator) -> str:
    st = coordinator.data.status
    if not st.netcast or st.app == APP_LG:
        return SOURCE_LG
    if st.app == APP_LAUNCHER:
        return SOURCE_LAUNCHER
    for name, target in sources(coordinator).items():
        if target == st.app:
            return name
    return st.title


async def async_launch_source(coordinator: NetCastPlusCoordinator, source: str) -> None:
    src = sources(coordinator)
    if source not in src:
        raise ServiceValidationError(
            translation_domain=DOMAIN, translation_key="unknown_source", translation_placeholders={"source": source}
        )
    target = src[source]
    await coordinator.async_call(coordinator.client.launch(target))
    if target == "exit":
        coordinator.async_set_optimistic(netcast=False, app=APP_LG, title=SOURCE_LG)
    else:
        coordinator.async_set_optimistic(netcast=True, app=target, title=source)
    # Switching takes a second or two; starting ncplus from LG's UI up to ~10 s.
    coordinator.async_refresh_later(3, 12)


async def async_screen(coordinator: NetCastPlusCoordinator, on: bool) -> None:
    await coordinator.async_call(coordinator.client.screen(on))
    coordinator.async_set_optimistic(screen_on=on)
    coordinator.async_refresh_later(2)


async def async_setup_entry(
    hass: HomeAssistant, entry: NetCastPlusConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    async_add_entities([NetCastPlusMediaPlayer(entry.runtime_data)])


class NetCastPlusMediaPlayer(NetCastPlusEntity, MediaPlayerEntity):
    _attr_name = None
    _attr_device_class = MediaPlayerDeviceClass.TV
    _attr_supported_features = (
        MediaPlayerEntityFeature.VOLUME_STEP
        | MediaPlayerEntityFeature.VOLUME_MUTE
        | MediaPlayerEntityFeature.SELECT_SOURCE
        | MediaPlayerEntityFeature.TURN_ON
        | MediaPlayerEntityFeature.TURN_OFF
    )

    def __init__(self, coordinator: NetCastPlusCoordinator) -> None:
        super().__init__(coordinator, "media_player")

    @property
    @override
    def state(self) -> MediaPlayerState:
        # "Off" = backlight off (the TV itself stays up so the API can turn it on again)
        return MediaPlayerState.ON if self.coordinator.data.status.screen_on else MediaPlayerState.OFF

    @property
    @override
    def source_list(self) -> list[str]:
        return list(sources(self.coordinator))

    @property
    @override
    def source(self) -> str:
        return current_source(self.coordinator)

    @property
    @override
    def app_id(self) -> str:
        return self.coordinator.data.status.app

    @property
    @override
    def app_name(self) -> str:
        return self.coordinator.data.status.title

    @override
    async def async_select_source(self, source: str) -> None:
        await async_launch_source(self.coordinator, source)

    @override
    async def async_volume_up(self) -> None:
        await self.coordinator.async_call(self.coordinator.client.key("volup"))

    @override
    async def async_volume_down(self) -> None:
        await self.coordinator.async_call(self.coordinator.client.key("voldown"))

    @override
    async def async_mute_volume(self, mute: bool) -> None:
        # The API can't read the mute state, so this is the remote's Mute key: a toggle.
        await self.coordinator.async_call(self.coordinator.client.key("mute"))

    @override
    async def async_turn_on(self) -> None:
        await async_screen(self.coordinator, True)

    @override
    async def async_turn_off(self) -> None:
        await async_screen(self.coordinator, False)
