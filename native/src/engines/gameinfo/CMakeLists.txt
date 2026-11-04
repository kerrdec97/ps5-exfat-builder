# QtGui provides QImage for cover normalisation (PNG decode, smooth scaling).
# No widgets and no QGuiApplication are needed: QImage works under
# QCoreApplication, so the module stays usable from headless tests and jobs.
find_package(Qt6 6.4 REQUIRED COMPONENTS Gui)
fw_add_module(fw_engine_gameinfo DEPENDS Qt6::Gui)
