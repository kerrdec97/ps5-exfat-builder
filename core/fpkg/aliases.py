"""ASCII aliases for folders whose path the Publishing Tools reject.

The Publishing Tools refuse non-ASCII paths, and many game folders have
names like "Ghost of Yōtei". On Windows an NTFS junction with a plain ASCII
path pointing at the real folder lets the tools read and write it without
renaming anything. Junctions are removed afterwards; removing a junction
never touches the folder it points to.
"""
import os
import subprocess
import uuid


def is_ascii(path):
    try:
        path.encode('ascii')
        return True
    except UnicodeEncodeError:
        return False


def alias_root():
    """An ASCII folder to hold junctions, or None."""
    for base in (os.environ.get('TEMP'), os.environ.get('TMP'),
                 os.path.join(os.environ.get('ProgramData', r'C:\ProgramData'),
                              'exFATImageBuilder')):
        if base and is_ascii(base):
            root = os.path.join(base, 'fpkg-aliases')
            try:
                os.makedirs(root, exist_ok=True)
                return root
            except OSError:
                continue
    return None


class Aliases:
    """Context manager: map(path) returns an ASCII path for a directory."""

    def __init__(self, log=None):
        self.created = []
        self.log = log or (lambda s: None)

    def map(self, path):
        path = os.path.abspath(path)
        if is_ascii(path):
            return path
        if os.name != 'nt':
            raise OSError('non-ASCII path and no junction support: %s' % path)
        root = alias_root()
        if not root:
            raise OSError('no ASCII folder is available for a junction')
        alias = os.path.join(root, 'a%d-%s' % (os.getpid(), uuid.uuid4().hex[:8]))
        r = subprocess.run(['cmd', '/d', '/c', 'mklink', '/J', alias, path],
                           capture_output=True,
                           creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        if r.returncode != 0 or not os.path.isdir(alias):
            raise OSError('could not create a junction for %s (%s)'
                          % (path, (r.stdout + r.stderr).decode('mbcs', 'replace').strip()))
        self.created.append(alias)
        self.log('[FPKG] Using ASCII alias %s for %s' % (alias, path))
        return alias

    def close(self):
        for a in reversed(self.created):
            try:
                os.rmdir(a)      # removes the junction only
            except OSError as e:
                self.log('[FPKG] Could not remove alias %s: %s' % (a, e))
        self.created = []

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False
