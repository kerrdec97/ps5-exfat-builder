"""Launch the real application, visit every page and screenshot it.

Usage:
    python tools/gui_check.py OUTDIR [--lang de] [--size 1360x860]

Runs on Windows (screenshots via PIL.ImageGrab) and on Linux under Xvfb
(PIL.ImageGrab needs an X display there). Every page in the app's tab
registry is opened; Tk callback exceptions are collected. A JSON report is
written to OUTDIR/report.json and the exit code is non-zero if any page
raised an error. The app's settings are redirected to a temporary file, so
the user's real settings are never touched.
"""
import argparse
import json
import os
import sys
import time
import traceback

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)
os.chdir(ROOT)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('outdir')
    ap.add_argument('--lang', default=None)
    ap.add_argument('--size', default='1360x860')
    ap.add_argument('--pages', default='')
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    from tests._support import load_app, temp_settings_file
    mod = load_app()
    report = {'pages': {}, 'errors': [], 'version': mod.APP_VERSION,
              'platform': sys.platform, 'lang': args.lang or 'en'}
    try:
        from PIL import ImageGrab
    except Exception as e:      # pragma: no cover
        ImageGrab = None
        report['errors'].append('PIL.ImageGrab unavailable: %r' % (e,))

    with temp_settings_file() as path:
        s = {'release_last_seen_version': mod.APP_VERSION,
             'window_geometry': args.size + '+0+0'}
        if args.lang:
            s['language'] = args.lang
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(s, f)
        app = mod.ExFATBuilder()
        errs = []
        app.report_callback_exception = (
            lambda e, v, tb: errs.append(''.join(
                traceback.format_exception(e, v, tb))))

        def pump(n=20):
            for _ in range(n):
                app.update()
                time.sleep(0.03)

        pump(40)
        try:
            app.lift()
            app.attributes('-topmost', True)
        except Exception:
            pass
        keys = [k for k in args.pages.split(',') if k] or list(app._tab_registry)
        for k in keys:
            before = len(errs)
            t0 = time.time()
            try:
                app._switch_tab(k)
            except Exception:
                errs.append('%s: %s' % (k, traceback.format_exc()))
            pump(15)
            entry = {'switch_ms': int((time.time() - t0) * 1000),
                     'errors': errs[before:]}
            if ImageGrab is not None:
                try:
                    x, y = app.winfo_rootx(), app.winfo_rooty()
                    w, h = app.winfo_width(), app.winfo_height()
                    img = ImageGrab.grab(bbox=(x, y, x + w, y + h))
                    p = os.path.join(args.outdir, '%s.png' % k)
                    img.save(p)
                    entry['screenshot'] = os.path.basename(p)
                except Exception as e:
                    entry['screenshot_error'] = repr(e)
            report['pages'][k] = entry
        app.destroy()
    report['errors'] = errs
    with open(os.path.join(args.outdir, 'report.json'), 'w',
              encoding='utf-8') as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
    print('pages: %d  errors: %d' % (len(report['pages']), len(errs)))
    for e in errs[:10]:
        print(e[-2000:])
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main())
