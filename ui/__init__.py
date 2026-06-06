"""ui/ — per-tab UI modules.

Each tab is a module under this package exporting a `build_<name>_tab(parent, app)`
function. To avoid circular-import risk during the Step 1 carve-out, this package
deliberately does NOT eager-import the tab modules. Callers (the shims in
exfat_builder.py) import each tab module lazily by name.

Step 2+ may add explicit re-exports here once the theme module replaces the
star-import dependency on the main module.
"""

# Public surface — these names exist as importable submodules:
__all__ = [
    "tab_exfat",
    "tab_ffpkg",
    "tab_library",
    "tab_images",
    "tab_ps5_mgr",
    "tab_extract",
    "tab_dump_rename",
    "tab_files",
    "tab_ftp",
    "tab_backports",
    "tab_payloads",
    "tab_klog",
    "tab_advanced",
    "tab_settings",
    "tab_language",
    "tab_history",
    "tab_y2jb",
    "tab_help",
    "shared",
]
