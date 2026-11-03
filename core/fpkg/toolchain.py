"""Locate and validate the user's Publishing Tools command-line tool."""
import json
import os

EXE_NAME = 'prospero-pub-cmd.exe'

# Windows NTSTATUS exit codes worth translating for the user.
EXIT_HINTS = {
    0xC0000135: ('A DLL the Publishing Tools need is missing. Install the '
                 'Microsoft Visual C++ 2015-2022 Redistributable (x64).'),
    0xC000007B: ('The Publishing Tools could not start (bad image or wrong '
                 'architecture). Use the 64-bit tools on 64-bit Windows.'),
}


class Toolchain:
    """A validated Publishing Tools location."""

    def __init__(self, exe, root, profile=None, problems=None, warnings=None):
        self.exe = exe
        self.root = root
        self.profile = profile
        self.problems = list(problems or [])
        self.warnings = list(warnings or [])

    @property
    def ok(self):
        return bool(self.exe) and not self.problems

    def summary(self):
        if not self.exe:
            return 'Publishing Tools not set'
        bits = [self.exe]
        if self.profile:
            bits.append('profile ' + self.profile)
        return '  ·  '.join(bits)


def exit_hint(code):
    """Friendly text for a Windows exit code, or ''."""
    if code is None:
        return ''
    return EXIT_HINTS.get(code & 0xFFFFFFFF, '')


def _candidates(path):
    """Accept the exe itself, its folder, or a toolkit root holding
    toolchain\\ or bin\\."""
    if not path:
        return []
    path = os.path.normpath(os.path.expandvars(os.path.expanduser(path)))
    if os.path.isfile(path):
        return [path]
    return [os.path.join(path, EXE_NAME),
            os.path.join(path, 'toolchain', EXE_NAME),
            os.path.join(path, 'bin', EXE_NAME)]


def find_toolchain(configured=None, env=None):
    """Return a Toolchain for the first existing candidate.

    `configured` is the path from the app's settings. The environment
    variable SCE_PROSPERO_PUBTOOLS may also point at the tools. Nothing is
    searched for beyond these: the user decides which tools are used."""
    env = os.environ if env is None else env
    for base in (configured, env.get('SCE_PROSPERO_PUBTOOLS')):
        for cand in _candidates(base):
            if os.path.isfile(cand):
                return validate(cand)
    return Toolchain(None, None,
                     problems=['Publishing Tools not found. Choose your '
                               'prospero-pub-cmd.exe in Settings > FPKG.'])


def validate(exe):
    root = os.path.dirname(os.path.abspath(exe))
    problems, warnings = [], []
    if os.path.basename(exe).lower() != EXE_NAME:
        problems.append('Expected %s, got %s' % (EXE_NAME, os.path.basename(exe)))
    if not os.path.isfile(os.path.join(root, 'libScePubTools.dll')):
        warnings.append('libScePubTools.dll was not found next to %s; the '
                        'tool may not start.' % EXE_NAME)
    if not os.path.isdir(os.path.join(root, 'ext')):
        warnings.append('The ext\\ folder of the Publishing Tools was not '
                        'found; texture and audio conversion may fail.')
    if not _is_ascii(root):
        warnings.append('The Publishing Tools folder path contains '
                        'non-ASCII characters; the tool may reject it.')
    profile = None
    for mf in (os.path.join(root, 'patch-manifest.json'),
               os.path.join(os.path.dirname(root), 'patch-manifest.json')):
        try:
            with open(mf, encoding='utf-8-sig') as f:
                profile = (json.load(f) or {}).get('profile')
            break
        except (OSError, ValueError, AttributeError):
            continue
    if os.name == 'nt':
        missing = vcruntime_missing()
        if missing:
            problems.append('Microsoft Visual C++ 2015-2022 x64 runtime is '
                            'missing (%s). Install the VC++ Redistributable.'
                            % ', '.join(missing))
    return Toolchain(os.path.abspath(exe), root, profile, problems, warnings)


def vcruntime_missing():
    """Names of VC++ runtime DLLs that cannot be loaded (Windows only)."""
    import ctypes
    missing = []
    for dll in ('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'):
        try:
            ctypes.WinDLL(dll)
        except OSError:
            missing.append(dll)
    return missing


def _is_ascii(s):
    try:
        s.encode('ascii')
        return True
    except UnicodeEncodeError:
        return False
