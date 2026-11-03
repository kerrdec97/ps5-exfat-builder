"""Safety checks for the Auto Backport and Backport All flows.

Original code for the exFAT Image Builder (Farewell Edition). Pure
functions with no Tk dependency, so they can be unit tested on any OS.

What they guard against (all reproduced against the earlier code):

* a fakelib source with libraries in sub-folders, empty or truncated
  placeholder files, or files that are not executables at all;
* a ZIP destination inside the game folder (the backup would be packed
  into the image and picked up by the next scan), a destination that does
  not exist or cannot be written, or a ZIP path longer than Windows'
  classic MAX_PATH;
* overwriting an earlier originals ZIP (it may be the only copy of the
  real originals) and ZIPs left half-written under their final name;
* ZIP entries that would be written outside the game folder on restore
  ("zip slip"), and restores that silently skip files;
* backporting a folder that this app already backported, judged from the
  file hashes it recorded in the profile manifest.
"""
import hashlib
import os
import zipfile

# Windows MAX_PATH is 260 including the terminating NUL. Long-path support
# needs both a registry switch and a long-path-aware manifest, so the app
# cannot assume it.
MAX_WIN_PATH = 259

# A real fake library (ELF or SELF with program headers and a dynamic
# section) is far larger than this. Smaller files are placeholders, Git LFS
# pointers or truncated downloads.
MIN_FAKELIB_BYTES = 1024

LIB_EXTS = ('.sprx', '.prx')
EXEC_EXTS = ('.bin', '.elf', '.self', '.sprx', '.prx')
_OS_METADATA = ('.ds_store', 'thumbs.db', 'desktop.ini')


def _norm(path):
    return os.path.normcase(os.path.abspath(path))


def is_within(path, folder):
    """True if `path` is `folder` itself or anything below it."""
    try:
        p, f = _norm(path), _norm(folder)
        return os.path.commonpath([p, f]) == f
    except ValueError:          # different drives on Windows
        return False


def _is_metadata(name):
    low = name.lower()
    return low.startswith('._') or low in _OS_METADATA


def validate_fakelib_source(src, is_executable, game=None,
                            min_size=MIN_FAKELIB_BYTES):
    """Problems with a fakelib source folder, as a list of strings (empty
    when it is usable). `is_executable(path)` decides whether a file has a
    recognised ELF/SELF header."""
    if not src or not os.path.isdir(src):
        return ['The fakelib folder does not exist: %s' % src]
    problems = []
    if game and (is_within(src, game) or is_within(game, src)):
        problems.append('The fakelib folder must be outside the game folder '
                        '(pick a separate copy of the fakelib set).')
        return problems
    libs = 0
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d.lower() != '__macosx']
        rel_root = os.path.relpath(root, src)
        for fn in files:
            if _is_metadata(fn) or not fn.lower().endswith(LIB_EXTS):
                continue
            path = os.path.join(root, fn)
            rel = os.path.normpath(os.path.join(rel_root, fn))
            if rel_root != '.':
                problems.append(
                    '%s is in a sub-folder. Libraries must sit directly in '
                    'the chosen folder: they are copied to fakelib/<same '
                    'path> and are only looked up directly in fakelib/. '
                    'If you picked the folder above the set, pick the set '
                    'itself.' % rel)
                continue
            try:
                size = os.path.getsize(path)
            except OSError as e:
                problems.append('%s cannot be read: %s' % (rel, e))
                continue
            if size < min_size:
                problems.append('%s is only %d bytes: a placeholder or an '
                                'incomplete download, not a library.'
                                % (rel, size))
                continue
            if not is_executable(path):
                problems.append('%s is not an ELF/SELF library.' % rel)
                continue
            libs += 1
    if not libs and not problems:
        problems.append('The fakelib folder contains no .sprx/.prx '
                        'libraries.')
    return problems


def validate_zip_destination(dest, game, zip_name, max_len=MAX_WIN_PATH):
    """None if a backup ZIP named `zip_name` can be written to `dest`,
    otherwise the reason it cannot."""
    if not dest:
        return 'No folder was chosen.'
    if not os.path.isdir(dest):
        return 'The folder does not exist: %s' % dest
    if game and is_within(dest, game):
        return ('The folder is inside the game folder. The ZIP would be '
                'packed into the image and mixed with the game files; '
                'choose a folder outside the game.')
    full = os.path.abspath(os.path.join(dest, zip_name))
    # "<name> (99).zip.part" is the longest name actually written.
    if len(full) + len(' (99).part') > max_len:
        return ('The ZIP path would be about %d characters; Windows only '
                'guarantees %d. Choose a shorter folder.'
                % (len(full) + len(' (99).part'), max_len))
    probe = os.path.join(dest, '.exfat_builder_write_test.tmp')
    try:
        with open(probe, 'wb') as f:
            f.write(b'0')
    except OSError as e:
        return 'The folder cannot be written: %s' % e
    finally:
        try:
            os.remove(probe)
        except OSError:
            pass
    return None


def temp_path_problem(temp_base, game, rel_paths, max_len=MAX_WIN_PATH):
    """None, or a message when copying `rel_paths` of `game` into the
    backport's temp folder would exceed `max_len` characters."""
    # <temp>\exfat_builder_abp_xxxxxxxx\patched\<game folder name>\<rel>
    stem = os.path.join(os.path.abspath(temp_base), 'exfat_builder_abp_' +
                        'x' * 8, 'patched',
                        os.path.basename(os.path.normpath(game)))
    worst = max(rel_paths, key=len, default='')
    if worst and len(os.path.join(stem, worst)) > max_len:
        return ('%s would be copied to a %d-character path in the temp '
                'folder; Windows only guarantees %d. Choose a shorter temp '
                'folder in Settings.'
                % (worst, len(os.path.join(stem, worst)), max_len))
    return None


def unique_path(path):
    """`path` if nothing is there yet, else '<stem> (N)<ext>' for the first
    free N. An existing backup is never overwritten."""
    if not os.path.exists(path):
        return path
    stem, ext = os.path.splitext(path)
    n = 2
    while os.path.exists('%s (%d)%s' % (stem, n, ext)):
        n += 1
    return '%s (%d)%s' % (stem, n, ext)


def dir_members(src_dir, exclude_dirs=()):
    """(abs_path, arcname) for every file under `src_dir`."""
    excl = {d.lower() for d in exclude_dirs}
    out = []
    for root, dirs, files in os.walk(src_dir):
        dirs[:] = [d for d in dirs if d.lower() not in excl]
        for fn in files:
            path = os.path.join(root, fn)
            out.append((path, os.path.relpath(path, src_dir)
                        .replace(os.sep, '/')))
    return out


def write_zip_atomic(zip_path, members):
    """Write `members` [(abs_path, arcname)] to `zip_path` through a
    '.part' file, check it reads back, then give it its final name.
    Returns the number of files stored; on any error nothing is left under
    `zip_path` or `zip_path + '.part'` and the error is raised."""
    part = zip_path + '.part'
    seen = set()
    try:
        with zipfile.ZipFile(part, 'w', zipfile.ZIP_DEFLATED,
                             allowZip64=True) as zf:
            for path, arc in members:
                arc = arc.replace('\\', '/')
                if arc.lower() in seen:
                    continue
                seen.add(arc.lower())
                zf.write(path, arc)
        with zipfile.ZipFile(part, 'r') as zf:
            bad = zf.testzip()
            stored = len(zf.namelist())
        if bad is not None:
            raise OSError('CRC check failed for %s in %s' % (bad, part))
        if stored != len(seen):
            raise OSError('%s holds %d of %d files' % (part, stored, len(seen)))
        os.replace(part, zip_path)
        return stored
    except BaseException:
        try:
            os.remove(part)
        except OSError:
            pass
        raise


def safe_member_target(root, name):
    """Absolute path inside `root` for ZIP entry `name`, or None when the
    entry is absolute, has a drive letter or climbs out with '..'."""
    clean = name.replace('\\', '/')
    if not clean or clean.startswith('/') or ':' in clean.split('/')[0]:
        return None
    parts = [p for p in clean.split('/') if p not in ('', '.')]
    if not parts or '..' in parts:
        return None
    target = os.path.join(os.path.abspath(root), *parts)
    return target if is_within(target, root) else None


def restore_zip(zip_path, root):
    """Extract every file of `zip_path` into `root`. Returns
    (restored, errors); entries that would land outside `root` are refused
    and reported. Raises if the ZIP cannot be opened."""
    import shutil
    restored, errors = 0, []
    with zipfile.ZipFile(zip_path, 'r') as zf:
        for member in zf.infolist():
            if member.is_dir():
                continue
            dst = safe_member_target(root, member.filename)
            if dst is None:
                errors.append('%s: refused, it would be written outside the '
                              'game folder' % member.filename)
                continue
            try:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with zf.open(member) as src_f, open(dst, 'wb') as dst_f:
                    shutil.copyfileobj(src_f, dst_f)
                restored += 1
            except Exception as e:
                errors.append('%s: %s' % (member.filename, e))
    return restored, errors


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def tree_hashes(folder, strip_first=None, exts=EXEC_EXTS):
    """{posix rel path: sha256} of the executables under `folder`, skipping
    'decrypted'. `strip_first` drops a leading folder of that name (the
    pipeline sometimes wraps its output in the game folder's name)."""
    out = {}
    for root, dirs, files in os.walk(folder):
        dirs[:] = [d for d in dirs if d.lower() != 'decrypted']
        for fn in files:
            if not fn.lower().endswith(exts) or _is_metadata(fn):
                continue
            path = os.path.join(root, fn)
            parts = os.path.relpath(path, folder).split(os.sep)
            if strip_first and len(parts) > 1 and parts[0] == strip_first:
                parts = parts[1:]
            try:
                out['/'.join(parts)] = sha256_of(path)
            except OSError:
                pass
    return out


def backported_by_manifest(game, manifest):
    """Compare the game's executables with what a profile manifest
    recorded. Returns (verdict, files):

    'patched'   - files match the patched hashes recorded after an earlier
                  run of this app: the folder is already backported;
    'differs'   - files differ from the recorded originals without matching
                  a recorded patched hash (patched by an older version of
                  this app or another tool, or a different game version);
    'original'  - files match the recorded originals;
    'unknown'   - nothing to compare."""
    if not manifest:
        return 'unknown', []
    patched = {}
    for tgt in (manifest.get('targets') or {}).values():
        patched.update({k.lower(): v for k, v in
                        (tgt.get('post_hashes') or {}).items()})
    originals = {k.lower(): v for k, v in
                 ((manifest.get('originals') or {}).get('files') or {}).items()
                 if k.lower().endswith(EXEC_EXTS)
                 and not k.lower().startswith(('decrypted/', 'fakelib/'))}

    def _current(rel):
        path = os.path.join(game, *rel.split('/'))
        try:
            return sha256_of(path) if os.path.isfile(path) else None
        except OSError:
            return None

    hits = [rel for rel, h in sorted(patched.items())
            if not rel.startswith('fakelib/') and _current(rel) == h]
    if hits:
        return 'patched', hits
    differs = []
    same = 0
    for rel, h in sorted(originals.items()):
        cur = _current(rel)
        if cur is None:
            continue
        if cur == h:
            same += 1
        else:
            differs.append(rel)
    if differs:
        return 'differs', differs
    return ('original', []) if same else ('unknown', [])
