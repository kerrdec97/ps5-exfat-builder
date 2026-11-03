"""Pre-build release check for exFAT Image Builder.

Run by build.bat before PyInstaller (and usable on its own):

    python tools/release_check.py            # all checks + unit tests
    python tools/release_check.py --no-tests # skip the unit tests

Checks:
  * every first-party .py file compiles;
  * files the executable bundles or reads at runtime are present
    (vendored Lazy_MkPFS, icon, CHANGELOG.md, notices, licence texts);
  * the version in exfat_builder.py is the version documented at the top of
    CHANGELOG.md, so the app, the exe properties and the notes agree;
  * the unit-test suite in tests/ passes.
All checks run; the exit code is non-zero if any of them failed.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

REQUIRED_FILES = [
    'exfat_builder.py',
    'tkinter_theme.py',
    'ui/__init__.py',
    'ui/shared/__init__.py',
    'controller.ico',
    'CHANGELOG.md',
    'README.md',
    'THIRD_PARTY_NOTICES.md',
    'vendor/lazy_mkpfs/main_file.py',
    'vendor/lazy_mkpfs/lazy_mkpfs/__init__.py',
    'vendor/lazy_mkpfs/LICENSE',
    'vendor/lazy_mkpfs/UPSTREAM_COMMIT.txt',
    'LICENSES/GPL-3.0.txt',
    'LICENSES/UFS2Tool-BSD-2-Clause.txt',
]


def _first_party_sources():
    for base in ('.', 'ui', 'ui/shared', 'tools', 'tests'):
        d = os.path.join(ROOT, base)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if name.endswith('.py'):
                yield os.path.join(d, name)


def check_compile():
    bad = []
    for path in _first_party_sources():
        try:
            with open(path, 'r', encoding='utf-8') as f:
                compile(f.read(), path, 'exec', dont_inherit=True)
        except (SyntaxError, ValueError, UnicodeDecodeError) as e:
            bad.append('%s: %s' % (os.path.relpath(path, ROOT), e))
    return bad


def check_files():
    return ['missing: ' + p for p in REQUIRED_FILES
            if not os.path.exists(os.path.join(ROOT, p))]


def check_version():
    with open(os.path.join(ROOT, 'exfat_builder.py'), encoding='utf-8') as f:
        m = re.search(r"^APP_VERSION\s*=\s*'([^']+)'", f.read(), re.M)
    if not m:
        return ['APP_VERSION not found in exfat_builder.py']
    version = m.group(1)
    problems = []
    cl = os.path.join(ROOT, 'CHANGELOG.md')
    if os.path.exists(cl):
        with open(cl, encoding='utf-8') as f:
            head = f.read(4000)
        first = re.search(r'^## \[([^\]]+)\]', head, re.M)
        if not first or first.group(1) != version:
            problems.append('CHANGELOG.md top entry is %r, APP_VERSION is %r'
                            % (first.group(1) if first else None, version))
    return problems


def run_tests():
    tests = os.path.join(ROOT, 'tests')
    if not os.path.isdir(tests):
        return ['tests/ folder missing']
    rc = subprocess.call([sys.executable, '-m', 'unittest', 'discover',
                          '-s', tests, '-t', ROOT, '-q'], cwd=ROOT)
    return [] if rc == 0 else ['unit tests failed (exit %d)' % rc]


def main():
    steps = [('compile', check_compile), ('files', check_files),
             ('version', check_version)]
    if '--no-tests' not in sys.argv:
        steps.append(('tests', run_tests))
    failed = False
    for name, fn in steps:
        problems = fn()
        if problems:
            failed = True
            print('[FAIL] %s' % name)
            for p in problems:
                print('       ' + p)
        else:
            print('[ OK ] %s' % name)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
