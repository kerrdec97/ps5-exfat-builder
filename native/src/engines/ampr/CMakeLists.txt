# Game asset packs: zstd packs (ampr-emu-ng AMPRZPK1) written and read, LZ4
# packs (ampr_emu AMPRPAK4) read only, the bundled ampr-emu-ng runtime and the
# pack rules. Image sources are adapted by the integration module.
fw_add_module(fw_engine_ampr DEPENDS fw_engine_pfs fw_engine_general fw_zstd fw_lz4)
