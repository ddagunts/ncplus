"""Diagnostics download (token redacted)."""

from __future__ import annotations

from dataclasses import asdict
from typing import Any

from homeassistant.components.diagnostics import async_redact_data
from homeassistant.const import CONF_TOKEN
from homeassistant.core import HomeAssistant

from .coordinator import NetCastPlusConfigEntry


async def async_get_config_entry_diagnostics(hass: HomeAssistant, entry: NetCastPlusConfigEntry) -> dict[str, Any]:
    d = entry.runtime_data.data
    return {
        "entry": async_redact_data(dict(entry.data), {CONF_TOKEN}),
        "status": async_redact_data(d.status.raw, {"id"}),
        "settings": {k: asdict(v) for k, v in d.settings.items()},
        "apps": [asdict(a) for a in d.apps],
    }
