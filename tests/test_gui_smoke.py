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


    @unittest.skipIf(os.name == 'nt', 'the fake Publishing Tools is a script')
    def test_fpkg_job_through_build_queue(self):
        import shutil
        import stat
        import sys
        import tempfile
        from tests.test_fpkg import FAKE, CID, make_dump
        mod = load_app()
        tmp = tempfile.mkdtemp(prefix='gui_fpkg_')
        self.addCleanup(shutil.rmtree, tmp, True)
        tools = os.path.join(tmp, 'tools')
        os.makedirs(os.path.join(tools, 'ext'))
        open(os.path.join(tools, 'libScePubTools.dll'), 'wb').close()
        exe = os.path.join(tools, 'prospero-pub-cmd.exe')
        with open(exe, 'w') as f:
            f.write('#!%s\nimport runpy, sys\nsys.argv[0] = %r\n'
                    'runpy.run_path(%r, run_name="__main__")\n'
                    % (sys.executable, FAKE, FAKE))
        os.chmod(exe, os.stat(exe).st_mode | stat.S_IEXEC)
        dump = make_dump(tmp)
        out = os.path.join(tmp, 'out')
        os.makedirs(out)
        with temp_settings_file() as path:
            with open(path, 'w', encoding='utf-8') as f:
                f.write('{"release_last_seen_version": "%s", '
                        '"check_updates_on_boot": false}' % mod.APP_VERSION)
            app = mod.ExFATBuilder()
            errors = []
            app.report_callback_exception = (
                lambda exc, val, tb: errors.append('%s: %s' % (exc.__name__, val)))
            try:
                app._switch_tab('unibuild')
                app.update()
                api = app._uq_test_api
                api['pick_type']('fpkg')
                api['fpkg_tool_var'].set(exe)
                api['fpkg_chunks_var'].set('3')
                api['outdir_var'].set(out)
                api['src_var'].set(dump)
                app.update()
                api['add']()
                self.assertEqual(len(api['queue']), 1)
                job = api['queue'][0]
                self.assertEqual(job['type'], 'fpkg')
                self.assertEqual(job['name'], CID + '-A0100-V0100.pkg')
                api['build_all']()
                deadline = time.time() + 60
                while api['state'].get('running') and time.time() < deadline:
                    app.update()
                    time.sleep(0.02)
                self.assertFalse(api['state'].get('running'))
                self.assertEqual(job['status'], 'done', job.get('error'))
                self.assertTrue(os.path.isfile(job['out_path']))
                self.assertEqual(errors, [])
            finally:
                app.destroy()


if __name__ == '__main__':
    unittest.main()
