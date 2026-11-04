# Backport engine. Self-contained: it keeps the little game-metadata parsing it
# needs (param.json / param.sfo for ZIP names and title IDs) in GameMetadata.*
# instead of depending on the gameinfo module.
fw_add_module(fw_engine_backport)
