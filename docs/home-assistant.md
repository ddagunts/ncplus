# ncplus for Home Assistant

A custom integration (`netcast_plus`) for an LG NetCast TV running ncplus. It talks only to the TV's
ncplus Remote API (`ncapi`, see the [ncplus docs](ncplus.md#remote-api-and-home-assistant)). It
polls every 15 s and works without cloud access or extra Python packages. It is written for Home
Assistant 2026.10 and tested against 2026.10.4.

## What you get

One device per TV, with:

| Entity | What it does |
|---|---|
| `media_player.lg_tv` | **On/off** = backlight on/off (the TV keeps running, so it can be turned on again over Wi-Fi). **Volume up/down** and **mute** send the remote's keys. **Source** = `LG` (LG's own UI), `ncplus` (the launcher) or any ncplus app. |
| `remote.lg_tv_remote` | `remote.send_command` with key names or evdev codes. `num_repeats`, `delay_secs` and `hold_secs` work. Activities = the same list as the sources, so `remote.turn_on` with `activity: Doom` starts Doom. |
| `switch.lg_tv_keep_tv_awake`, `switch.lg_tv_lite_mode`, `switch.lg_tv_wi_fi_keep_alive` | The Settings app's switches (`NOSLEEP`, `LITE`, `NETKEEP`), applied on the TV straight away. |
| `select.lg_tv_screen_saver` | Screen-saver minutes: Off, 5, 10, 20, 30, 60. |
| `sensor.lg_tv_current_app` | `LG`, `ncplus` or the app's title (`app_id` attribute = its folder). |

Key names for `send_command`: `up down left right ok back exit home menu info guide fav volup voldown
mute chup chdown red green yellow blue 0`–`9 dash flashback instart`. `GET /api/v1/keys` lists them.
A number with two or more digits is sent as an evdev code (`"412"` = Back). Codes 897/898 are always
refused, because they reset the TV to its shipping state.

```yaml
action: remote.send_command
target: { entity_id: remote.lg_tv_remote }
data: { command: [down, down, ok], delay_secs: 0.3 }
```

Known limits:
- **Mute is a toggle.** The API can't read LG's mute state, so the mute button always presses Mute.
  The volume level isn't readable either, so there is no volume slider.
- **"Off" only turns the backlight off.** Real standby isn't offered, because the TV can't be woken over Wi-Fi.
  Any key, including one sent from Home Assistant, turns the backlight on again.
- The `API` setting itself can only be changed on the TV, so it has no entity.

## Install

1. On the TV, open ncplus **Settings** and turn on **Remote API**. Note the address and token shown
   under it.
2. Copy this repository's `custom_components/netcast_plus/` into your Home Assistant config folder as
   `config/custom_components/netcast_plus/`, then restart Home Assistant.
3. Go to Settings > Devices & services > Add integration > **ncplus**, and enter the TV's address,
   port 9080 and the token.

If you press "New token" on the TV, Home Assistant asks for the new token (re-authenticate). If the TV
gets a new IP address, use **Reconfigure** on the integration. The TV is identified by its MAC address,
which `/status` reports.

## Tests

```sh
tools/hatest.sh
```

The first run builds a Python 3.14 environment with `pytest-homeassistant-custom-component` (Home
Assistant 2026.10.4) in `~/.cache/netcast-ha`. The tests live in `tests/ha/`. The script runs:
1. `test_config_flow.py`, `test_entities.py` and `test_translations.py`, against an in-memory fake of
   the API (`fake_ncapi.py`).
2. `test_real_ncapi.py`, against the real `ncapi` built for the host, on the fake netcast tree from
   `tools/apitest.sh --local`. That run checks that keys reach the input device, apps switch through
   the session loop, settings are written to `netcast.conf` and the screen turns off and on.

To check against the TV itself (read-only: status, settings, apps, keys and entity setup):

```sh
cd tests/ha
NCAPI_URL=http://<tv-ip>:9080 NCAPI_TOKEN=<token> ~/.cache/netcast-ha/venv/bin/python -m pytest test_real_ncapi.py
```
