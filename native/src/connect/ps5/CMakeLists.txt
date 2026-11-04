# Qt SQL (SQLite driver) is used only by the Y2JB app.db / appinfo.db edits.
find_package(Qt6 6.4 REQUIRED COMPONENTS Network Sql)
fw_add_module(fw_connect_ps5 DEPENDS Qt6::Network Qt6::Sql fw_engine_gameinfo)
