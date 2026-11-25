"""Every translation key the code uses exists in translations/en.json (the HA test harness doesn't check)."""

from __future__ import annotations

import json
from pathlib import Path
import re

from custom_components.netcast_plus.const import SETTING_TRANSLATION_KEYS

PKG = Path(__file__).parents[2] / "custom_components" / "netcast_plus"
EN = json.loads((PKG / "translations" / "en.json").read_text())
CODE = {p.name: p.read_text() for p in PKG.glob("*.py")}


def _all(pattern: str) -> set[str]:
    return {m for src in CODE.values() for m in re.findall(pattern, src)}


def test_exception_keys() -> None:
    used = _all(r'translation_key="(\w+)"') | _all(r'_invalid\("(\w+)"')
    entity_keys = {k for plat in EN["entity"].values() for k in plat}
    assert used - entity_keys <= set(EN["exceptions"]), used - entity_keys - set(EN["exceptions"])


def test_config_flow_keys() -> None:
    flow = CODE["config_flow.py"]
    errors = set(re.findall(r'errors\[[^\]]+\] = "(\w+)"', flow))
    aborts = set(re.findall(r'reason="(\w+)"', flow)) | {"already_configured", "reauth_successful", "reconfigure_successful"}
    steps = set(re.findall(r'step_id="(\w+)"', flow))
    assert errors <= set(EN["config"]["error"])
    assert aborts <= set(EN["config"]["abort"])
    assert steps <= set(EN["config"]["step"])


def test_entity_keys() -> None:
    for platform, key in [("remote", "remote"), ("sensor", "current_app")]:
        assert key in EN["entity"][platform]
    for setting, key in SETTING_TRANSLATION_KEYS.items():
        platform = "select" if setting == "SAVER_MIN" else "switch"
        assert key in EN["entity"][platform], (setting, key)
    assert set(EN["entity"]["select"]["screen_saver"]["state"]) == {"0", "5", "10", "20", "30", "60"}
