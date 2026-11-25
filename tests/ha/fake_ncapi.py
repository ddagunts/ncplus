"""A stand-in for the TV's ncapi (src/tools/ncapi.c): same paths, replies and refusals, state in memory."""

from __future__ import annotations

import copy
from dataclasses import dataclass, field
from typing import Any

from aiohttp import web

TOKEN = "0123456789abcdef0123456789abcdef"
MAC = "aa:bb:cc:dd:ee:01"

SETTINGS = {
    "SAVER_MIN": {"value": 20, "default": 20, "options": [0, 5, 10, 20, 30, 60], "title": "Screen saver",
                  "description": "Backlight off after this many minutes without input (0 = off)", "readonly": False},
    "NOSLEEP": {"value": 1, "default": 1, "options": [0, 1], "title": "Keep TV awake", "description": "", "readonly": False},
    "LITE": {"value": 0, "default": 0, "options": [0, 1], "title": "Lite mode", "description": "", "readonly": False},
    "NETKEEP": {"value": 1, "default": 1, "options": [0, 1], "title": "Wi-Fi keep-alive", "description": "", "readonly": False},
    "API": {"value": 1, "default": 0, "options": [0, 1], "title": "Remote API", "description": "", "readonly": True},
}
APPS = [
    {"id": "doom", "title": "Doom", "desc": "Freedoom", "order": 10},
    {"id": "music", "title": "Music", "desc": "FLAC", "order": 27},
    {"id": "settings", "title": "Settings", "desc": "", "order": 90},
]
KEYS = {"up": 103, "down": 108, "left": 105, "right": 106, "ok": 28, "enter": 28, "back": 412, "exit": 174,
        "home": 773, "volup": 115, "voldown": 114, "mute": 113, "red": 398, "instart": 899,
        **{str(i): c for i, c in enumerate([11, 2, 3, 4, 5, 6, 7, 8, 9, 10])}}


@dataclass
class FakeNcapi:
    token: str = TOKEN
    mac: str = MAC
    netcast: bool = False
    app: str = "LG"
    screen: str = "on"
    settings: dict[str, Any] = field(default_factory=lambda: copy.deepcopy(SETTINGS))
    apps: list[dict[str, Any]] = field(default_factory=lambda: copy.deepcopy(APPS))
    keys: list[dict[str, Any]] = field(default_factory=list)       # POST /key bodies, in order
    launches: list[str] = field(default_factory=list)
    screens: list[bool] = field(default_factory=list)
    puts: list[dict[str, Any]] = field(default_factory=list)
    requests: list[str] = field(default_factory=list)              # "GET status", ...
    port: int = 0
    _runner: web.AppRunner | None = None

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.port}"

    def status_json(self) -> dict[str, Any]:
        title = "LG"
        if self.netcast:
            title = "ncplus" if self.app == "launcher" else next((a["title"] for a in self.apps if a["id"] == self.app), self.app)
        return {"netcast": self.netcast, "app": self.app if self.netcast else "LG", "title": title, "screen": self.screen,
                "version": "test-1", "id": self.mac, "uptime": 1234, "api_uptime": 56}

    @staticmethod
    def err(code: int, msg: str) -> web.Response:
        return web.json_response({"error": msg}, status=code)

    @web.middleware
    async def _mw(self, request: web.Request, handler):
        self.requests.append(f"{request.method} {request.path.removeprefix('/api/v1/')}")
        if request.headers.get("Authorization", "") != f"Bearer {self.token}":
            return self.err(401, "missing or wrong token")
        return await handler(request)

    async def _body(self, request: web.Request) -> dict[str, Any] | web.Response:
        try:
            b = await request.json()
        except ValueError:
            return self.err(400, "body must be a flat JSON object")
        if not isinstance(b, dict) or any(isinstance(v, (dict, list)) for v in b.values()):
            return self.err(400, "body must be a flat JSON object")
        return b

    async def h_status(self, request):
        return web.json_response(self.status_json())

    async def h_settings(self, request):
        if request.method == "GET":
            return web.json_response({"settings": self.settings})
        b = await self._body(request)
        if isinstance(b, web.Response):
            return b
        if not b:
            return self.err(400, "empty object")
        for k, v in b.items():
            s = self.settings.get(k)
            if s is None:
                return self.err(400, f"unknown setting {k}")
            if s["readonly"]:
                return self.err(403, f"{k} can only be changed on the TV (Settings)")
            if isinstance(v, bool):
                v = int(v)
            if not isinstance(v, int) or v not in s["options"]:
                return self.err(400, f"{k} must be one of " + " ".join(map(str, s["options"])))
        for k, v in b.items():
            self.settings[k]["value"] = int(v)
        self.puts.append(b)
        return web.json_response({"settings": self.settings})

    async def h_apps(self, request):
        return web.json_response({"apps": self.apps})

    async def h_keys(self, request):
        return web.json_response({"keys": [{"name": n, "code": c, "label": n} for n, c in KEYS.items()]})

    async def h_key(self, request):
        b = await self._body(request)
        if isinstance(b, web.Response):
            return b
        if isinstance(b.get("key"), str):
            code = KEYS.get(b["key"].lower())
            if code is None:
                return self.err(400, "unknown key name (GET /api/v1/keys lists them)")
        elif "code" in b:
            code = b["code"]
            if not isinstance(code, int) or isinstance(code, bool):
                return self.err(400, '"code" must be an integer')
        else:
            return self.err(400, 'need "key" (a name) or "code"')
        if code in (897, 898):
            return self.err(403, "refused: 897/898 are IN-STOP / power-only service keys")
        r = b.get("repeat", 1)
        if not 1 <= r <= 100 or not 20 <= b.get("hold_ms", 80) <= 10000 or not 0 <= b.get("delay_ms", 200) <= 10000:
            return self.err(400, "hold_ms 20..10000, repeat 1..100, delay_ms 0..10000")
        self.keys.append(b)
        if self.screen == "off":   # ncscreen off --wake: any key turns the backlight on
            self.screen = "on"
        return web.json_response({"ok": True, "code": code, "name": "", "repeat": r}, status=202)

    async def h_launch(self, request):
        b = await self._body(request)
        if isinstance(b, web.Response):
            return b
        a = b.get("app")
        if not isinstance(a, str) or not a:
            return self.err(400, 'need "app": an app id, "launcher" or "exit"')
        if a not in ("exit", "launcher") and all(x["id"] != a for x in self.apps):
            return self.err(404, "no such app (GET /api/v1/apps lists them)")
        self.launches.append(a)
        if a == "exit":
            if not self.netcast:
                return web.json_response({"ok": True, "app": a, "note": "ncplus is not running"}, status=202)
            self.netcast, self.app = False, "LG"
        else:
            self.netcast, self.app = True, a
        return web.json_response({"ok": True, "app": a, "note": "switching"}, status=202)

    async def h_screen(self, request):
        b = await self._body(request)
        if isinstance(b, web.Response):
            return b
        if not isinstance(b.get("on"), bool):
            return self.err(400, 'need {"on": true|false}')
        self.screens.append(b["on"])
        self.screen = "on" if b["on"] else "off"
        return web.json_response({"ok": True, "screen": self.screen}, status=202)

    async def start(self) -> None:
        app = web.Application(middlewares=[self._mw])
        r = app.router
        r.add_get("/api/v1/status", self.h_status)
        r.add_route("*", "/api/v1/settings", self.h_settings)
        r.add_get("/api/v1/apps", self.h_apps)
        r.add_get("/api/v1/keys", self.h_keys)
        r.add_post("/api/v1/key", self.h_key)
        r.add_post("/api/v1/launch", self.h_launch)
        r.add_post("/api/v1/screen", self.h_screen)
        self._runner = web.AppRunner(app)
        await self._runner.setup()
        site = web.TCPSite(self._runner, "127.0.0.1", self.port)
        await site.start()
        self.port = site._server.sockets[0].getsockname()[1]

    async def stop(self) -> None:
        if self._runner:
            await self._runner.cleanup()
            self._runner = None
