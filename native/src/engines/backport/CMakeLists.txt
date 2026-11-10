# Readers the Backport tab uses for its game list (fake-signed SELF, the SDK in
# an executable's param, param.json version fields). Self-contained: it does
# not depend on the gameinfo module. Backports themselves are made by the
# bundled PS5 Backport engine, a separate program (see src/app/backport).
fw_add_module(fw_engine_backport)
