# dmgbuild settings for the TetherKitNext disk image (used by scripts/make-dmg.sh).
#
#   dmgbuild -s scripts/dmg/settings.py -D app=dist/TetherKitNext.app \
#            -D background=… -D icon=… "TetherKitNext 1.0.0" out.dmg
#
# dmgbuild writes the Finder window layout (.DS_Store) directly, so it works on
# headless CI runners, unlike scripting Finder with AppleScript.
import os.path

app = defines["app"]  # noqa: F821 (injected by dmgbuild)
app_name = os.path.basename(app)

format = "UDZO"
compression_level = 9
filesystem = "HFS+"

files = [app]
symlinks = {"Applications": "/Applications"}
hide_extensions = [app_name]

# The volume icon: the app icon, so the mounted disk is recognisable.
icon = defines.get("icon")  # noqa: F821

background = defines["background"]  # noqa: F821
window_rect = ((200, 140), (660, 400))
default_view = "icon-view"
show_status_bar = False
show_tab_view = False
show_toolbar = False
show_pathbar = False
show_sidebar = False
sidebar_width = 0

arrange_by = None
icon_size = 128
text_size = 13
# Centres, matching the wells drawn in background.svg.
icon_locations = {
    app_name: (170, 190),
    "Applications": (490, 190),
}
