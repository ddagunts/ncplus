"""Config flow: host, port and the token shown in the TV's Settings > Remote API."""

from __future__ import annotations

from collections.abc import Mapping
import re
from typing import Any, override

try:  # Home Assistant 2026.x ships probatio, a drop-in voluptuous replacement
    import probatio as vol
except ImportError:  # pragma: no cover
    import voluptuous as vol

from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_HOST, CONF_PORT, CONF_TOKEN
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.selector import (
    NumberSelector,
    NumberSelectorConfig,
    NumberSelectorMode,
    TextSelector,
    TextSelectorConfig,
    TextSelectorType,
)

from .api import NetCastAuthError, NetCastConnectionError, NetCastError, NetCastPlusClient, Status
from .const import DEFAULT_PORT, DOMAIN, LOGGER

TOKEN_RE = re.compile(r"^[0-9a-fA-F]{16,64}$")
DEFAULT_TITLE = "LG TV"


def _clean(user_input: dict[str, Any]) -> dict[str, Any]:
    out = dict(user_input)
    if CONF_HOST in out:
        out[CONF_HOST] = out[CONF_HOST].strip()
    if CONF_PORT in out:
        out[CONF_PORT] = int(out[CONF_PORT])
    if CONF_TOKEN in out:   # people paste "Bearer <token>" or the token with spaces around it
        t = out[CONF_TOKEN].strip()
        out[CONF_TOKEN] = t[7:].strip() if t.lower().startswith("bearer ") else t
    return out


def _schema(defaults: Mapping[str, Any], fields: tuple[str, ...] = (CONF_HOST, CONF_PORT, CONF_TOKEN)) -> vol.Schema:
    s: dict[Any, Any] = {}
    if CONF_HOST in fields:
        s[vol.Required(CONF_HOST, default=defaults.get(CONF_HOST, ""))] = TextSelector(TextSelectorConfig(autocomplete="off"))
    if CONF_PORT in fields:
        s[vol.Required(CONF_PORT, default=defaults.get(CONF_PORT, DEFAULT_PORT))] = NumberSelector(
            NumberSelectorConfig(min=1, max=65535, step=1, mode=NumberSelectorMode.BOX)
        )
    if CONF_TOKEN in fields:
        s[vol.Required(CONF_TOKEN)] = TextSelector(TextSelectorConfig(type=TextSelectorType.PASSWORD))
    return vol.Schema(s)


class NetCastPlusConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def _validate(self, data: dict[str, Any], errors: dict[str, str]) -> Status | None:
        if not TOKEN_RE.match(data[CONF_TOKEN]):
            errors[CONF_TOKEN] = "invalid_token_format"
            return None
        client = NetCastPlusClient(async_get_clientsession(self.hass), data[CONF_HOST], data[CONF_PORT], data[CONF_TOKEN])
        try:
            return await client.status()
        except NetCastAuthError:
            errors[CONF_TOKEN] = "invalid_auth"
        except NetCastConnectionError:
            errors["base"] = "cannot_connect"
        except NetCastError:
            errors["base"] = "not_netcast"
        except Exception:  # noqa: BLE001
            LOGGER.exception("Unexpected error checking %s:%s", data[CONF_HOST], data[CONF_PORT])
            errors["base"] = "unknown"
        return None

    @staticmethod
    def _unique_id(status: Status, data: Mapping[str, Any]) -> str:
        # The MAC from /status. Older ncapi builds don't send it: fall back to the address.
        return status.id.lower() if status.id else f"{data[CONF_HOST]}:{data[CONF_PORT]}"

    @override
    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            user_input = _clean(user_input)
            if status := await self._validate(user_input, errors):
                await self.async_set_unique_id(self._unique_id(status, user_input), raise_on_progress=False)
                self._abort_if_unique_id_configured(
                    updates={CONF_HOST: user_input[CONF_HOST], CONF_PORT: user_input[CONF_PORT], CONF_TOKEN: user_input[CONF_TOKEN]}
                )
                return self.async_create_entry(title=DEFAULT_TITLE, data=user_input)
        return self.async_show_form(step_id="user", data_schema=_schema(user_input or {}), errors=errors)

    async def async_step_reauth(self, entry_data: Mapping[str, Any]) -> ConfigFlowResult:
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        entry = self._get_reauth_entry()
        errors: dict[str, str] = {}
        if user_input is not None:
            data = _clean({**entry.data, **user_input})
            if status := await self._validate(data, errors):
                await self.async_set_unique_id(self._unique_id(status, data))
                self._abort_if_unique_id_mismatch(reason="wrong_device")
                return self.async_update_reload_and_abort(entry, data_updates={CONF_TOKEN: data[CONF_TOKEN]})
        return self.async_show_form(
            step_id="reauth_confirm",
            data_schema=_schema({}, (CONF_TOKEN,)),
            description_placeholders={"host": entry.data[CONF_HOST]},
            errors=errors,
        )

    async def async_step_reconfigure(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        entry = self._get_reconfigure_entry()
        errors: dict[str, str] = {}
        if user_input is not None:
            data = _clean(user_input)
            if status := await self._validate(data, errors):
                if status.id:   # without a MAC there is nothing to compare: the new address is the point
                    await self.async_set_unique_id(self._unique_id(status, data))
                    self._abort_if_unique_id_mismatch(reason="wrong_device")
                return self.async_update_reload_and_abort(entry, data_updates=data)
        return self.async_show_form(
            step_id="reconfigure", data_schema=_schema(user_input or entry.data), errors=errors
        )
