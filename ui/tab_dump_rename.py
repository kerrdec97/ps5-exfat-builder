"""
ui/tab_dump_rename.py — compatibility shim.

Step 18 (v2.1.9): the Dump Rename tab was rebuilt as
`ui/dump_rename.py` per opus_handoff/dump_rename_v3/BRIEF.md (which
specifies the singular module name and the `DumpRenameTab` class).

The legacy carve-out call site in `exfat_builder.py` calls
`build_dump_rename_tab(parent, app)` from this module — kept here so
the main-file shim doesn't need to change.
"""

from ui.dump_rename import build_dump_rename_tab, DumpRenameTab  # noqa: F401
