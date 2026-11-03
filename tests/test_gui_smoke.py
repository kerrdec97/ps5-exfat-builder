"""GUI smoke test: build the real main window and visit every page.

Opt-in because it opens a window (and the app starts its usual background
checks): set EXFAT_GUI_SMOKE=1. On Linux run it under Xvfb, e.g.
    EXFAT_GUI_SMOKE=1 xvfb-run -a python -m unittest tests.test_gui_smoke
No OSFMount, robocopy, PS5 or network access is needed for it to pass.
"""
import os
import time
import unittest

from tests._support import load_app, temp_settings_file


@unittest.skipUnless(os.environ.get('EXFAT_GUI_SMOKE') == '1',
                     'set EXFAT_GUI_SMOKE=1 to run the GUI smoke test')
class GuiSmoke(unittest.TestCase):
    def test_every_page_builds_and_legacy_routes_resolve(self):
        mod = load_app()
        with temp_settings_file() as path:
            with open(path, 'w', encoding='utf-8') as f:
                # Skip the first-run release notes modal.
                f.write('{"release_last_seen_version": "%s"}' % mod.APP_VERSION)
            app = mod.ExFATBuilder()
            errors = []
            app.report_callback_exception = (
                lambda exc, val, tb: errors.append('%s: %s' % (exc.__name__, val)))
            try:
                for _i in range(20):
                    app.update()
                    time.sleep(0.05)
                keys = list(app._tab_registry)
                self.assertGreaterEqual(len(keys), 15)
                for key in keys:
                    app._switch_tab(key)
                    app.update()
                # Pages that became sub-tabs must land on their parent.
                for legacy, parent in (('ftp', 'ps5'), ('language', 'settings'),
                                       ('images', 'library'), ('klog', 'ps5')):
                    app._switch_tab(legacy)
                    app.update()
                    self.assertEqual(app._active_tab.get(), parent, legacy)
                self.assertEqual(errors, [])
                self.assertIn(mod.APP_EDITION, app.title())
            finally:
                app.destroy()


if __name__ == '__main__':
    unittest.main()
