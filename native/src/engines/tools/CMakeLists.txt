# The dump check reads executables' headers with the backport module's SELF / ELF readers,
# and compares names as exFAT does with the exFAT module's up-case table.
fw_add_module(fw_engine_tools DEPENDS fw_engine_gameinfo fw_engine_backport fw_engine_exfat)
