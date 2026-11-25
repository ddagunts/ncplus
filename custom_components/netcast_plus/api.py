"""Client for the ncplus remote API (tools/ncapi on the TV).

Plain HTTP + JSON with a bearer token. See src/tools/ncapi.c for the server side.
"""

from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
from typing import Any

import aiohttp
from yarl import URL

TIMEOUT = aiohttp.ClientTimeout(total=10)


class NetCastError(Exception):
    """Base error."""


class NetCastConnectionError(NetCastError):
    """The TV did not answer (off, other network, API switched off)."""


class NetCastAuthError(NetCastError):
    """401: missing or wrong token."""


class NetCastResponseError(NetCastError):
    """The API refused the request (4xx/5xx other than 401) or sent something unexpected."""

    def __init__(self, status: int, message: str) -> None:
        super().__init__(f"{status}: {message}")
        self.status = status
        self.message = message


@dataclass
class Setting:
    """One [settings] switch."""

    key: str
    value: int
    default: int
    options: list[int]
    title: str
    description: str
    readonly: bool

    @property
    def is_bool(self) -> bool:
        return sorted(self.options) == [0, 1]


@dataclass
class App:
    """A ncplus app (apps/<id>/app.ini)."""

    id: str
    title: str
    desc: str = ""
    order: int = 50


@dataclass
class Status:
    """GET /status."""

    netcast: bool
    app: str
    title: str
    screen_on: bool
    version: str
    id: str = ""
    uptime: int = 0
    raw: dict[str, Any] = field(default_factory=dict)


def parse_status(d: dict[str, Any]) -> Status:
    try:
        return Status(
            netcast=bool(d["netcast"]),
            app=str(d["app"]),
            title=str(d.get("title") or d["app"]),
            screen_on=d.get("screen") != "off",
            version=str(d.get("version", "")),
            id=str(d.get("id") or ""),
            uptime=int(d.get("uptime", 0)),
            raw=d,
        )
    except (KeyError, TypeError, ValueError) as err:
        raise NetCastResponseError(200, f"not a ncplus status reply: {d!r:.200}") from err


def parse_settings(d: dict[str, Any]) -> dict[str, Setting]:
    out: dict[str, Setting] = {}
    try:
        for key, s in d["settings"].items():
            out[key] = Setting(
                key=key,
                value=int(s["value"]),
                default=int(s.get("default", s["value"])),
                options=[int(o) for o in s.get("options", [])],
                title=str(s.get("title") or key),
                description=str(s.get("description", "")),
                readonly=bool(s.get("readonly", False)),
            )
    except (KeyError, TypeError, ValueError, AttributeError) as err:
        raise NetCastResponseError(200, f"not a settings reply: {d!r:.200}") from err
    return out


def parse_apps(d: dict[str, Any]) -> list[App]:
    try:
        return [
            App(id=str(a["id"]), title=str(a.get("title") or a["id"]), desc=str(a.get("desc", "")), order=int(a.get("order", 50)))
            for a in d["apps"]
        ]
    except (KeyError, TypeError, ValueError) as err:
        raise NetCastResponseError(200, f"not an apps reply: {d!r:.200}") from err


class NetCastPlusClient:
    """Talks to one TV."""

    def __init__(self, session: aiohttp.ClientSession, host: str, port: int, token: str) -> None:
        self._session = session
        self._base = URL.build(scheme="http", host=host, port=port, path="/api/v1/")
        self._headers = {"Authorization": f"Bearer {token}"}

    async def _request(self, method: str, path: str, body: dict[str, Any] | None = None) -> dict[str, Any]:
        try:
            async with self._session.request(
                method, self._base.join(URL(path)), json=body, headers=self._headers, timeout=TIMEOUT
            ) as resp:
                try:
                    data = await resp.json(content_type=None)
                except (ValueError, aiohttp.ContentTypeError):
                    data = None
                if resp.status == 401:
                    raise NetCastAuthError("wrong or missing token")
                if resp.status >= 400:
                    msg = data.get("error") if isinstance(data, dict) else None
                    raise NetCastResponseError(resp.status, str(msg or resp.reason))
                if not isinstance(data, dict):
                    raise NetCastResponseError(resp.status, "reply is not a JSON object")
                return data
        except (aiohttp.ClientError, asyncio.TimeoutError, OSError) as err:
            raise NetCastConnectionError(str(err) or type(err).__name__) from err

    async def status(self) -> Status:
        return parse_status(await self._request("GET", "status"))

    async def settings(self) -> dict[str, Setting]:
        return parse_settings(await self._request("GET", "settings"))

    async def set_settings(self, values: dict[str, int]) -> dict[str, Setting]:
        return parse_settings(await self._request("PUT", "settings", values))

    async def apps(self) -> list[App]:
        return parse_apps(await self._request("GET", "apps"))

    async def keys(self) -> list[dict[str, Any]]:
        return list((await self._request("GET", "keys")).get("keys", []))

    async def key(
        self,
        key: str | int,
        *,
        repeat: int = 1,
        hold_ms: int | None = None,
        delay_ms: int | None = None,
    ) -> dict[str, Any]:
        """Press a key: a name from GET /keys ("up", "volup", ...) or an evdev code."""
        body: dict[str, Any] = {"code": key} if isinstance(key, int) else {"key": key}
        if repeat != 1:
            body["repeat"] = repeat
        if hold_ms is not None:
            body["hold_ms"] = hold_ms
        if delay_ms is not None:
            body["delay_ms"] = delay_ms
        return await self._request("POST", "key", body)

    async def launch(self, app: str) -> dict[str, Any]:
        """An app id, "launcher" or "exit" (back to LG's UI)."""
        return await self._request("POST", "launch", {"app": app})

    async def screen(self, on: bool) -> dict[str, Any]:
        return await self._request("POST", "screen", {"on": on})
