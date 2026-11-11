# The dump check reads executables' headers with the backport module's SELF / ELF readers.
fw_add_module(fw_engine_tools DEPENDS fw_engine_gameinfo fw_engine_backport)
