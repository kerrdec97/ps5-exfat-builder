# FPKG packages through the user's own Sony Publishing Tools. Depends only on
# the app-wide settings module; image sources plug in at run time through
# fw::fpkg::registerSourceStager() (see SourceStager.h).
fw_add_module(fw_engine_fpkg DEPENDS fw_engine_general)
