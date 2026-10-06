# SPDX-License-Identifier: GPL-2.0-only
"""Current package producers, build dependencies and shared project sources."""

MULTITAP_SOURCES = (
    "lib/fplinux/fplinux-multitap.c",
    "include/fplinux/fplinux-multitap.h",
)
DRM_SESSION_SOURCES = (
    "lib/fplinux/fplinux-drm-session.c",
    "include/fplinux/fplinux-drm-session.h",
)
KEYBOARD_TEXT_SOURCES = (
    "lib/fplinux/fplinux-keyboard-text.c",
    "include/fplinux/fplinux-keyboard-text.h",
)
CLI_SOURCES = (
    "lib/fplinux/fplinux-cli.c",
    "include/fplinux/fplinux-cli.h",
)
FONT_SOURCES = (
    "lib/fplinux/fplinux-font.c",
    "include/fplinux/fplinux-font.h",
)
BRIGHTNESS_CLIENT_SOURCES = (
    "lib/fplinux/fplinux-brightness-client.c",
    "include/fplinux/fplinux-brightness-client.h",
)
INPUT_DEVICE_SOURCES = ("include/fplinux/fplinux-input-device.h",)
KEYPAD_CODE_SOURCES = ("include/fplinux/fplinux-keypad.h",)
INPUT_SESSION_SOURCES = (
    "lib/fplinux/fplinux-input-session.c",
    "include/fplinux/fplinux-input-session.h",
)
SHARED_APORT_SOURCES = {
    "fplinux-base": (
        *CLI_SOURCES,
        *BRIGHTNESS_CLIENT_SOURCES,
    ),
    "fplinux-bluetooth": CLI_SOURCES,
    "fplinux-brightness-ui": (
        *FONT_SOURCES,
        *DRM_SESSION_SOURCES,
        *BRIGHTNESS_CLIENT_SOURCES,
        *INPUT_SESSION_SOURCES,
        *INPUT_DEVICE_SOURCES,
        *KEYPAD_CODE_SOURCES,
        *CLI_SOURCES,
    ),
    "fplinux-charge": CLI_SOURCES,
    "fplinux-cpuclock": CLI_SOURCES,
    "fplinux-fm": CLI_SOURCES,
    "fplinux-jack": CLI_SOURCES,
    "fplinux-jpeg": CLI_SOURCES,
    "fplinux-present": (
        *DRM_SESSION_SOURCES,
        *CLI_SOURCES,
    ),
    "fplinux-rotate": (*DRM_SESSION_SOURCES, *CLI_SOURCES),
    "fplinux-showcase": (
        *FONT_SOURCES,
        *DRM_SESSION_SOURCES,
        *CLI_SOURCES,
        *KEYPAD_CODE_SOURCES,
        *BRIGHTNESS_CLIENT_SOURCES,
    ),
    "fplinux-terminal": (
        *FONT_SOURCES,
        *CLI_SOURCES,
        *DRM_SESSION_SOURCES,
        *INPUT_DEVICE_SOURCES,
        *KEYPAD_CODE_SOURCES,
        *INPUT_SESSION_SOURCES,
        *KEYBOARD_TEXT_SOURCES,
        *MULTITAP_SOURCES,
    ),
    "fplinux-tyrquake": (
        *DRM_SESSION_SOURCES,
        *CLI_SOURCES,
        *INPUT_DEVICE_SOURCES,
        *KEYPAD_CODE_SOURCES,
        *INPUT_SESSION_SOURCES,
    ),
}
COMMON_PACKAGES = (
    "fplinux-base",
    "fplinux-bash",
    "fplinux-busybox",
    "fplinux-ncurses",
    "fplinux-openrc",
    "fplinux-readline",
    "fplinux-terminal",
    "fplinux-input",
    "fplinux-libdrm",
    "fplinux-libtsm",
    "fplinux-libudev",
)
LOCAL_BUILD_DEPENDENCIES = {
    "fplinux-apk-tools": ("fplinux-mbedtls-static-dev",),
    "fplinux-uclient": ("fplinux-mbedtls-static-dev",),
    "fplinux-audio": ("fplinux-alsa-lib",),
    "fplinux-bluealsa": (
        "fplinux-alsa-lib",
        "fplinux-bluez",
        "fplinux-glib",
        "fplinux-sbc",
    ),
    "fplinux-bash": ("fplinux-ncurses", "fplinux-readline"),
    "fplinux-bluez": ("fplinux-bluez-glib", "fplinux-ncurses", "fplinux-readline"),
    "fplinux-brightness-ui": ("fplinux-libdrm",),
    "fplinux-fm": ("fplinux-alsa-lib",),
    "fplinux-jack": ("fplinux-alsa-lib",),
    "fplinux-readline": ("fplinux-ncurses",),
    "fplinux-present": ("fplinux-libdrm",),
    "fplinux-rotate": ("fplinux-libdrm",),
    "fplinux-showcase": ("fplinux-libdrm",),
    "fplinux-terminal": (
        "fplinux-libdrm",
        "fplinux-libtsm",
        "fplinux-libxkbcommon",
    ),
    "fplinux-tyrquake": ("fplinux-libdrm",),
}
SUBPACKAGE_APORTS = {
    "fplinux-alsa-lib-card-profiles": "fplinux-alsa-lib",
    "fplinux-bash-loadables": "fplinux-bash",
    "fplinux-bluez-libs": "fplinux-bluez",
    "fplinux-glib-static-dev": "fplinux-glib",
    "fplinux-ncurses-curses": "fplinux-ncurses",
    "fplinux-font-terminus-6x12": "fplinux-font-terminus",
    "fplinux-font-terminus-8x16": "fplinux-font-terminus",
}

SHARED_APORT_SOURCE_PATHS = frozenset(
    path for paths in SHARED_APORT_SOURCES.values() for path in paths
)
