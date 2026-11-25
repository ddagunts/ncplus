"""Constants for the ncplus integration."""

from datetime import timedelta
import logging

DOMAIN = "netcast_plus"
LOGGER = logging.getLogger(__package__)

DEFAULT_PORT = 9080
SCAN_INTERVAL = timedelta(seconds=15)
# The app list only changes when apps are added to the stick, so it is fetched less often.
APPS_INTERVAL = timedelta(minutes=5)

# Sources that are not apps/<id>: LG's own UI ("exit") and the ncplus launcher.
SOURCE_LG = "LG"
SOURCE_LAUNCHER = "ncplus"
APP_LG = "LG"  # /status "app" while ncplus isn't running
APP_LAUNCHER = "launcher"

# Settings that get a translated name. Others still get an entity, named by the API's "title".
SETTING_TRANSLATION_KEYS = {
    "SAVER_MIN": "screen_saver",
    "NOSLEEP": "keep_awake",
    "LITE": "lite_mode",
    "NETKEEP": "wifi_keepalive",
}
