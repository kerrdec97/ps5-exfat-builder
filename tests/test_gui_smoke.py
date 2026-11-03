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

    def test_worker_thread_after_runs_on_tk_thread(self):
        import threading
        mod = load_app()
        with temp_settings_file() as path:
            with open(path, 'w', encoding='utf-8') as f:
                f.write('{"release_last_seen_version": "%s", '
                        '"check_updates_on_boot": false}' % mod.APP_VERSION)
            app = mod.ExFATBuilder()
            ran = []
            try:
                main_id = threading.get_ident()

                def worker():
                    app.after(0, lambda: ran.append(threading.get_ident()))
                    app.after(30, lambda: ran.append('later'))
                    app.after_idle(lambda: ran.append('idle'))
                t = threading.Thread(target=worker)
                t.start()
                t.join()
                deadline = time.time() + 3
                while len(ran) < 3 and time.time() < deadline:
                    app.update()
                    time.sleep(0.01)
                self.assertIn(main_id, ran)
                self.assertIn('later', ran)
                self.assertIn('idle', ran)
            finally:
                app.destroy()
            # A worker finishing after the window closed must not raise.
            errs = []
            t = threading.Thread(target=lambda: errs.append(
                app.after(0, lambda: None)))
            t.start()
            t.join()
            self.assertEqual(errs, ['after#xthread'])


if __name__ == '__main__':
    unittest.main()
