# DLC packages (the ppr-pkg Companion's DLC Converter): DLC INIs and DLC
# .pkg files -> PS5 DLC packages built by fpkg-cli (LibProsperoPkg), plus the
# optional online region lookup (Qt Network).
fw_add_module(fw_engine_dlc DEPENDS fw_engine_fpkg Qt6::Network)
