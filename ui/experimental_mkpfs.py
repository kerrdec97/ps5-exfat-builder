"""
ui/experimental_mkpfs.py  —  EXPERIMENTAL, OFF BY DEFAULT.

Isolated runner for an EXTERNAL mkpfs-compatible tool, used ONLY for the
existing `.exfat` -> `.ffpfsc` route when the user explicitly opts in via
Settings. This is a generic testing hook: it can drive any external
mkpfs-compatible packer that exposes the expected CLI / launcher, not one
specific tool. This module is intentionally self-contained so the whole
experiment can be removed by:
  1. deleting this file,
  2. removing the two settings keys (experimental_mkpfs_enabled,
     experimental_mkpfs_folder), and
  3. deleting the single branch in _pipeline_convert_then_next that calls
     run_experimental_mkpfs().

Nothing here touches the stable mkpfs path, FFPKG, dump-folder builds, or
extraction. The stable code path is the fallback for every failure mode.

The external tool exposes a CLI of the form:
    main.py -i <input> -o <output.ffpfsc>
            --compression-level N --cpu N --pfs-version PS5 --temp-folder DIR
and may ship a platform launcher (a .bat on Windows / .sh on POSIX) that
builds a venv and installs its own deps. We prefer a launcher if present and
fall back to `python main.py`. Launcher detection is generic — several common
names are probed and the actual one found is logged. Any failure -> caller
uses stable mkpfs.
"""

import os
import sys
import subprocess


# Generic launcher names probed in the tool folder, in priority order.
# These cover the common conventions for external mkpfs-compatible tools
# (including a tool that happens to be named lazy_mkpfs) without the UI or
# the common-case logging being branded to any one of them.
_LAUNCHER_NAMES_WIN = ('mkpfs.bat', 'run_mkpfs.bat', 'run.bat',
                       'lazy_mkpfs.bat')
_LAUNCHER_NAMES_POSIX = ('mkpfs.sh', 'run_mkpfs.sh', 'run.sh',
                         'lazy_mkpfs.sh')


def _find_system_python():
    """Locate a system Python for the plain `python main.py` path (used only
    when no platform launcher is present). Returns a path or None."""
    import shutil
    for name in ('python3', 'python'):
        p = shutil.which(name)
        if p:
            # Avoid recursing into our own frozen exe.
            if getattr(sys, 'frozen', False) and \
                    os.path.normcase(p) == os.path.normcase(sys.executable):
                continue
            return p
    # Dev (non-frozen) interpreter is fine.
    if not getattr(sys, 'frozen', False):
        return sys.executable
    return None


def _resolve_invocation(tool_dir):
    """Decide how to launch the tool from its folder.

    Returns (argv_prefix, kind, detail) where kind is 'launcher' or 'python',
    or (None, reason, '') if it can't be launched. argv_prefix is the command
    up to (but not including) the tool arguments. `detail` is the basename of
    the detected launcher (or the python target) for logging.
    """
    if not tool_dir or not os.path.isdir(tool_dir):
        return None, 'folder-invalid', ''
    main_py = os.path.join(tool_dir, 'main.py')

    is_windows = (os.name == 'nt')
    names = _LAUNCHER_NAMES_WIN if is_windows else _LAUNCHER_NAMES_POSIX

    # Prefer a platform launcher (it typically builds the venv + installs
    # deps). Probe the common names generically and use the first that exists.
    for nm in names:
        cand = os.path.join(tool_dir, nm)
        if os.path.isfile(cand):
            if is_windows:
                return [cand], 'launcher', nm
            # POSIX: invoke via bash; the .sh may not be +x after a zip
            # extract.
            import shutil
            bash = shutil.which('bash') or '/bin/bash'
            return [bash, cand], 'launcher', nm

    # No launcher — fall back to a plain system Python running main.py.
    if not os.path.isfile(main_py):
        return None, 'main.py-missing', ''
    py = _find_system_python()
    if not py:
        return None, 'no-python', ''
    return [py, main_py], 'python', 'main.py'


def run_experimental_mkpfs(app, src_exfat, final_ffpfsc, *,
                           compression_level, cpu_count, temp_folder,
                           log):
    """Run the external mkpfs-compatible tool for one .exfat -> .ffpfsc
    conversion.

    Args:
      app             : the App (for settings); not mutated here.
      src_exfat       : ORIGINAL source .exfat path (never copied/deleted).
      final_ffpfsc    : exact final output path the tool must write.
      compression_level, cpu_count : ints from the build config.
      temp_folder     : selected temp folder (may be '' / None).
      log             : callable(str) -> None for line logging (thread-safe).

    Returns True on a plausible success (output exists and is non-trivially
    sized), False on ANY failure. Never raises; the caller falls back to the
    stable mkpfs path when this returns False.
    """
    tool_dir = (getattr(app, '_settings', {}) or {}).get(
        'experimental_mkpfs_folder', '') or ''
    tool_dir = tool_dir.strip()

    log('[EXPERIMENTAL] Using external mkpfs-compatible tool\n')
    log('[EXPERIMENTAL]   Tool folder:       %s\n' % (tool_dir or '(unset)'))
    log('[EXPERIMENTAL]   Source image:      %s\n' % src_exfat)
    log('[EXPERIMENTAL]   Output image:      %s\n' % final_ffpfsc)
    log('[EXPERIMENTAL]   Compression level: %s\n' % compression_level)
    log('[EXPERIMENTAL]   CPU count:         %s\n' % cpu_count)
    log('[EXPERIMENTAL]   Temp folder:       %s\n' % (temp_folder or '(none)'))

    prefix, kind, detail = _resolve_invocation(tool_dir)
    if prefix is None:
        log('[EXPERIMENTAL] External tool not runnable (%s) \u2014 falling '
            'back to stable mkpfs\n' % kind)
        return False
    # Report the actual launcher/target detected (generic; only names the
    # specific file because that's what was found on disk).
    if kind == 'launcher':
        log('[EXPERIMENTAL]   Detected launcher: %s\n' % detail)
    else:
        log('[EXPERIMENTAL]   Detected target:   %s (system Python)\n' % detail)
    log('[EXPERIMENTAL]   Launch mode:       %s\n' % kind)

    # Build the tool argument list (its CLI, not stock mkpfs's).
    args = ['-i', src_exfat,
            '-o', final_ffpfsc,
            '--compression-level', str(int(compression_level)),
            '--pfs-version', 'PS5']
    try:
        if cpu_count and int(cpu_count) > 0:
            args += ['--cpu', str(int(cpu_count))]
    except Exception:
        pass
    if temp_folder and str(temp_folder).strip():
        args += ['--temp-folder', str(temp_folder).strip()]

    cmd = list(prefix) + args
    try:
        printable = ' '.join(
            ('"%s"' % c if (' ' in c or '\u2192' in c) else c) for c in cmd)
    except Exception:
        printable = str(cmd)
    log('[EXPERIMENTAL]   Command: %s\n' % printable)

    # Make sure the output dir exists; the tool also does this, but be safe.
    try:
        os.makedirs(os.path.dirname(final_ffpfsc), exist_ok=True)
    except Exception:
        pass

    # Record any pre-existing output size so we can detect a real write.
    try:
        pre_size = os.path.getsize(final_ffpfsc) \
            if os.path.isfile(final_ffpfsc) else -1
    except Exception:
        pre_size = -1

    # Run it, streaming stdout/stderr (merged) to the log.
    try:
        env = dict(os.environ)
        env.setdefault('PYTHONUNBUFFERED', '1')
        proc = subprocess.Popen(
            cmd, cwd=tool_dir,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            universal_newlines=True, bufsize=1, env=env)
    except Exception as e:
        log('[EXPERIMENTAL] Failed to start external tool: %s \u2014 falling '
            'back to stable mkpfs\n' % e)
        return False

    try:
        for line in iter(proc.stdout.readline, ''):
            if line:
                log('[EXPERIMENTAL][mkpfs] ' + line.rstrip('\n') + '\n')
        proc.stdout.close()
    except Exception as e:
        log('[EXPERIMENTAL] Error reading external tool output: %s\n' % e)
    rc = proc.wait()
    log('[EXPERIMENTAL]   Exit code: %s\n' % rc)

    if rc != 0:
        log('[EXPERIMENTAL] External tool failed (exit %s) \u2014 falling '
            'back to stable mkpfs\n' % rc)
        return False

    # Plausibility checks on the produced output.
    if not os.path.isfile(final_ffpfsc):
        log('[EXPERIMENTAL] Output file not found after run \u2014 falling '
            'back to stable mkpfs\n')
        return False
    try:
        out_size = os.path.getsize(final_ffpfsc)
    except Exception:
        out_size = 0
    # "Implausibly small": a real .ffpfsc has a PFS header + data. Anything
    # under ~1 MiB (or no larger than a pre-existing stub) is suspect.
    if out_size < (1 * 1024 * 1024) or (pre_size >= 0 and out_size <= pre_size):
        log('[EXPERIMENTAL] Output implausibly small (%d bytes) \u2014 '
            'falling back to stable mkpfs\n' % out_size)
        try:
            # Don't leave a bad stub where the stable path will write.
            if pre_size < 0:
                os.remove(final_ffpfsc)
        except Exception:
            pass
        return False

    log('[EXPERIMENTAL] External tool produced %s (%d bytes). Will run the '
        'same verification as stable output.\n'
        % (os.path.basename(final_ffpfsc), out_size))
    return True


# Backwards-compatible alias: anything still importing the old name keeps
# working. (Safe to delete once no callers reference it.)
run_experimental_lazy_mkpfs = run_experimental_mkpfs
