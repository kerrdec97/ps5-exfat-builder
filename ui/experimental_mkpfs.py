"""
ui/experimental_mkpfs.py  —  Lazy_MkPFS convert-stage backend (BUNDLED).

Drives a BUNDLED Lazy_MkPFS for the image -> .ffpfsc compression stage only:

    intermediate .exfat / .ffpkg  ->  .ffpfsc      (folder builds, pass 2)
    existing     .exfat / .ffpkg  ->  .ffpfsc      (existing-image route)

It NEVER touches the folder -> intermediate stage (that stays on the hardened
builder pipeline) and does NOT use Lazy's one-shot main_folder.py.

v3.6.6 bundling: Lazy_MkPFS is vendored under vendor/lazy_mkpfs/ and shipped
INSIDE the frozen exe. The user no longer needs to download Lazy or set a
folder. Execution mirrors the stock-mkpfs worker model: instead of relying on
a system `python` (which a frozen end user may not have), the app RE-INVOKES
ITSELF — `sys.executable --__lazy_worker__ file SRC OUT ...` — and the
__main__ shim in exfat_builder.py imports the vendored Lazy package and runs
its main_file entry. This keeps:
  * only-the-exe (no external Python, no external Lazy folder),
  * crash isolation (Lazy runs in a separate process; its multiprocessing
    Pool re-execs the GUI-free worker shim, not the GUI),
  * no runtime dependency auto-install (cryptography is bundled; the vendored
    package's auto-install hook is patched out),
  * fallback to stock mkpfs on any failure (RESULT_FALLBACK),
  * no fallback on user cancel (RESULT_CANCELLED).

Lazy source resolution order (logged as source=...):
  A. override  — user-set 'experimental_mkpfs_folder', if it is a dir with
                 main_file.py (power users testing a newer Lazy build);
  B. bundled   — the vendored copy inside the frozen exe (default);
  C. dev       — vendor/lazy_mkpfs/ in the source tree when not frozen;
  otherwise source=missing -> fall back to stock mkpfs.

We never pass --no-exfat (we want .ffpfsc) and never pass --zlib-ng /
--zlib-isa (those trigger Lazy's optional-backend pip path; ISA-L/zlib-ng
bundling is a separate, deferred task). CPU: --cpu-count is OMITTED by default
so Lazy auto-selects all cores; it is passed ONLY when the user explicitly
chose a core count. The stock-mkpfs memory cap is NOT applied here.
"""

import os
import sys
import re
import subprocess


# Result codes returned by run_lazy_mkpfs(). The caller falls back to stock
# mkpfs ONLY on RESULT_FALLBACK — never on RESULT_CANCELLED.
RESULT_OK = 'ok'
RESULT_FALLBACK = 'fallback'
RESULT_CANCELLED = 'cancelled'

# Lazy's per-image entry point (the only one we drive).
_MAIN_FILE = 'main_file.py'

# Source images Lazy's main_file accepts for a .ffpfsc conversion.
_ALLOWED_SRC_EXTS = ('.exfat', '.ffpkg')

# Sentinel argv flag handled by the __main__ shim in exfat_builder.py.
LAZY_WORKER_FLAG = '--__lazy_worker__'

# Where the vendored copy lives, relative to the project root (dev) or the
# PyInstaller bundle root (frozen).
_VENDOR_REL = os.path.join('vendor', 'lazy_mkpfs')


def _is_frozen():
    return bool(getattr(sys, 'frozen', False))


def _dir_has_main_file(d):
    try:
        return bool(d) and os.path.isdir(d) and \
            os.path.isfile(os.path.join(d, _MAIN_FILE))
    except Exception:
        return False


def _bundled_candidates():
    """Yield plausible locations of the vendored Lazy dir, best first.

    Frozen (--onefile): PyInstaller unpacks data to sys._MEIPASS, and also the
    exe sits in its own dir. We try _MEIPASS first (where collected data
    lands), then next to the exe (for --onedir or side-by-side layouts).
    Dev: vendor/lazy_mkpfs/ relative to this file's project root.
    """
    cands = []
    # PyInstaller onefile extraction dir
    meipass = getattr(sys, '_MEIPASS', None)
    if meipass:
        cands.append(os.path.join(meipass, _VENDOR_REL))
        cands.append(os.path.join(meipass, 'lazy_mkpfs'))
    # Next to the exe (onedir) / side-by-side layouts
    if _is_frozen():
        exe_dir = os.path.dirname(os.path.abspath(sys.executable))
        cands.append(os.path.join(exe_dir, _VENDOR_REL))
        cands.append(os.path.join(exe_dir, 'tools', 'lazy_mkpfs'))
    # Dev tree: <project>/vendor/lazy_mkpfs  (this file is <project>/ui/)
    here = os.path.dirname(os.path.abspath(__file__))
    proj = os.path.dirname(here)
    cands.append(os.path.join(proj, _VENDOR_REL))
    return cands


def get_bundled_lazy_mkpfs_dir():
    """Return the path to the bundled vendored Lazy dir, or None."""
    for c in _bundled_candidates():
        if _dir_has_main_file(c):
            return c
    return None


def _resolve_lazy_source(app, log):
    """Resolve which Lazy_MkPFS to use and how.

    Returns (lazy_dir, source_label) where source_label is one of
    'override' / 'bundled' / 'dev' / 'missing'. Logs the decision explicitly.
    lazy_dir is None when source_label == 'missing'.
    """
    override = (getattr(app, '_settings', {}) or {}).get(
        'experimental_mkpfs_folder', '') or ''
    override = override.strip()
    if override:
        if _dir_has_main_file(override):
            log('[LAZY]   source=override (%s)\n' % override)
            return override, 'override'
        # Override set but unusable — say why, then fall through to bundled.
        if not os.path.isdir(override):
            log('[LAZY]   override folder set but does not exist: %s\n'
                % override)
        else:
            log('[LAZY]   override folder set but has no '
                f'{_MAIN_FILE!s}: {override!s}\n')
        log('[LAZY]   falling through to bundled Lazy_MkPFS\n')

    # Bundled / dev tree
    bundled = get_bundled_lazy_mkpfs_dir()
    if bundled:
        label = 'bundled' if _is_frozen() else 'dev'
        log(f'[LAZY]   source={label!s} ({bundled!s})\n')
        return bundled, label

    log('[LAZY]   source=missing (no bundled Lazy_MkPFS found)\n')
    return None, 'missing'


# ISA-L accepts compression levels 0-3 only (zlib's 1-9 do not map). When the
# user picks the ISA-L backend we clamp the requested level into that range.
# The app's default level is 9 -> clamp to 3 (ISA-L's "best") so the default
# keeps working rather than erroring out.
_ISA_L_MAX_LEVEL = 3
_ISA_L_DEFAULT_LEVEL = 3


def _clamp_isa_level(level):
    """Clamp a requested zlib level (1-9) into ISA-L's valid 0-3 range.

    None/invalid -> ISA-L default (3). Anything above 3 -> 3. This is what
    makes ISA-L usable on the app's default level-9 setting.
    """
    try:
        lvl = int(level)
    except Exception:
        return _ISA_L_DEFAULT_LEVEL
    if lvl < 0:
        return 0
    if lvl > _ISA_L_MAX_LEVEL:
        return _ISA_L_MAX_LEVEL
    return lvl


def _build_worker_cmd(lazy_dir, src_image, final_ffpfsc, *,
                      compression_level, cpu_count, zlib_backend='zlib'):
    """Build the command that runs Lazy's main_file entry in a separate
    process. Frozen: re-invoke this exe in lazy-worker mode (no system
    Python). Dev: run the vendored main_file.py with the dev interpreter.

    zlib_backend: 'zlib' (safe default) or 'isa-l' (experimental native
    acceleration). When 'isa-l' we add --zlib-isa AND clamp --zlib-level into
    ISA-L's supported 0-3 range (defaulting to 3). We never pass --zlib-ng or
    --no-exfat.

    Lazy args: positional SRC OUT, then --zlib-level; --cpu-count is OMITTED
    by default (auto) and passed ONLY when cpu_count is a positive int.
    """
    args = [src_image, final_ffpfsc]
    use_isa = str(zlib_backend or 'zlib').strip().lower() in \
        ('isa-l', 'isal', 'isa_l')
    if use_isa:
        # ISA-L: clamp into 0-3 and request the ISA-L backend.
        lvl = _clamp_isa_level(compression_level)
        args += ['--zlib-isa', '--zlib-level', str(lvl)]
    else:
        try:
            lvl = int(compression_level)
            if 1 <= lvl <= 9:
                args += ['--zlib-level', str(lvl)]
        except Exception:
            pass
    # Only pass a core count when the user explicitly chose one (>0);
    # otherwise let Lazy pick all cores (its auto behaviour).
    try:
        if cpu_count is not None and int(cpu_count) > 0:
            args += ['--cpu-count', str(int(cpu_count))]
    except Exception:
        pass
    args += ['-v']

    if _is_frozen():
        # Frozen: `exe --__lazy_worker__ file <lazy_dir> SRC OUT ...`
        # The __main__ shim in exfat_builder.py handles this before Tk is
        # ever imported, so Lazy's multiprocessing children never touch the GUI.
        return ([sys.executable, LAZY_WORKER_FLAG, 'file', lazy_dir] + args,
                'frozen-worker')

    # Dev: run the vendored main_file.py with this interpreter.
    py = sys.executable
    return [py, os.path.join(lazy_dir, _MAIN_FILE)] + args, 'dev-python'


def run_lazy_mkpfs(app, src_image, final_ffpfsc, *,
                   compression_level, cpu_count, temp_folder, log,
                   cancel_check=None, zlib_backend='zlib',
                   progress_cb=None):
    """Run bundled Lazy_MkPFS for one image -> .ffpfsc conversion.

    Args mirror the previous runner. `cpu_count` here is the USER-SELECTED
    core count or None/0 for auto; the stock-mkpfs memory cap must NOT be
    passed in. `zlib_backend` is 'zlib' (safe default) or 'isa-l'
    (experimental native acceleration); for ISA-L the level is clamped to 0-3.
    `progress_cb`, when provided, is called with each raw output FRAGMENT
    (split on CR/LF) so the caller can parse live compression progress; it is
    best-effort and must never raise. Returns RESULT_OK / RESULT_FALLBACK /
    RESULT_CANCELLED. Never raises; the caller falls back to stock mkpfs on
    FALLBACK (not on CANCEL).
    """
    _be = str(zlib_backend or 'zlib').strip().lower()
    _is_isa = _be in ('isa-l', 'isal', 'isa_l')
    log('[LAZY] Lazy_MkPFS backend (preferred, bundled) for ffpfsc convert\n')
    log('[LAZY]   Source image:      %s\n' % src_image)
    log('[LAZY]   Output image:      %s\n' % final_ffpfsc)
    if _is_isa:
        _clamped = _clamp_isa_level(compression_level)
        log('[LAZY]   Compression:       ISA-L (experimental), level '
            f'{_clamped!s} (requested {compression_level!s}; '
            'ISA-L supports 0-3)\n')
    else:
        log('[LAZY]   Compression:       zlib (safe), level %s\n'
            % compression_level)
    log('[LAZY]   CPU count:         %s\n'
        % ('auto (all cores)' if not cpu_count else cpu_count))
    if temp_folder and str(temp_folder).strip():
        log('[LAZY]   Temp folder:       %s (note: main_file.py has no temp '
            'redirect; logged only)\n' % temp_folder)

    # 1) Source extension gate (.exfat / .ffpkg only)
    src_ext = os.path.splitext(str(src_image))[1].lower()
    if src_ext not in _ALLOWED_SRC_EXTS:
        log('[LAZY] Source is %s, not .exfat/.ffpkg — falling back to stock mkpfs\n'
            % (src_ext or '(none)'))
        return RESULT_FALLBACK

    # 2) Resolve the Lazy source (override / bundled / dev / missing)
    lazy_dir, source = _resolve_lazy_source(app, log)
    if source == 'missing' or not lazy_dir:
        log('[LAZY] No usable Lazy_MkPFS — falling back to stock mkpfs\n')
        return RESULT_FALLBACK

    cmd, mode = _build_worker_cmd(
        lazy_dir, src_image, final_ffpfsc,
        compression_level=compression_level, cpu_count=cpu_count,
        zlib_backend=_be)
    log('[LAZY]   Launch mode:       %s\n' % mode)
    try:
        printable = ' '.join(('"%s"' % c) if (' ' in c or '→' in c) else c
                             for c in cmd)
    except Exception:
        printable = str(cmd)
    log('[LAZY]   Command: %s\n' % printable)

    try:
        os.makedirs(os.path.dirname(final_ffpfsc), exist_ok=True)
    except Exception:
        pass
    # Remember whether the output already existed so a cancel/fail can clean
    # up ONLY what we created.
    try:
        pre_size = (os.path.getsize(final_ffpfsc)
                    if os.path.isfile(final_ffpfsc) else -1)
    except Exception:
        pre_size = -1

    # No console window on Windows for the worker.
    popen_kwargs = {}
    if os.name == 'nt':
        try:
            popen_kwargs['creationflags'] = getattr(
                subprocess, 'CREATE_NO_WINDOW', 0x08000000)
        except Exception:
            pass
    env = dict(os.environ)
    env.setdefault('PYTHONUNBUFFERED', '1')
    env['PYTHONIOENCODING'] = 'utf-8:replace'

    # 3) Run the worker, streaming its output to the log. We read in small
    #    chunks (not lines) so Lazy's '\r' progress redraws reach the caller
    #    live via progress_cb; the log gets a line-normalised copy.
    try:
        proc = subprocess.Popen(
            cmd, cwd=lazy_dir,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            # Farewell Edition (issues #85 / #69): decode exactly what the
            # child writes (PYTHONIOENCODING=utf-8 above). Without this the
            # parent used the Windows locale codec (cp1252 / cp936 /
            # cp1256 ...); the first undecodable byte killed the reader
            # thread, the pipe stopped being drained and the worker died on
            # its next progress write with "OSError: [Errno 22] Invalid
            # argument".
            universal_newlines=True, encoding='utf-8', errors='replace',
            bufsize=1, env=env, **popen_kwargs)
    except Exception as e:
        log('[LAZY] Failed to start Lazy_MkPFS worker: %s — falling back to stock mkpfs\n'
            % e)
        return RESULT_FALLBACK

    cancelled = False

    def _terminate():
        try:
            proc.terminate()
        except Exception:
            pass
        try:
            proc.wait(timeout=5)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass

    try:
        # Reader thread: pump raw chunks into a queue so the main loop can
        # poll cancel_check between reads instead of blocking in read(). Lazy
        # redraws its progress bar with '\r', so we must NOT line-buffer here
        # (a readline() would stall until the whole stage ends). Chunks are
        # handed to progress_cb as CR/LF-split fragments and appended to a
        # line buffer for the log.
        #
        # NOTE: the previous version read with readline() and only forwarded
        # complete lines, which is why the Lazy compression stage produced
        # no progress-bar updates ("frozen at ~82%"). The caller now parses
        # each fragment through ui/progress_state.py.
        #
        # universal_newlines=True + bufsize=1 keeps the text stream decoded
        # (PYTHONIOENCODING above makes it UTF-8 on the child side); we still
        # read(N) rather than readline() so '\r' fragments arrive promptly.
        import threading as _thr
        import queue as _q
        _stream = proc.stdout
        _outq = _q.Queue()

        def _reader():
            try:
                while True:
                    _c = _stream.read(1024)
                    if not _c:
                        break
                    _outq.put(_c)
            except Exception as _re_err:
                try:
                    _outq.put('\n[LAZY] (reader error: %s)\n' % _re_err)
                except Exception:
                    pass
            finally:
                _outq.put(None)

        _thr.Thread(target=_reader, daemon=True).start()
        _log_buf = ''

        def _feed(chunk):
            nonlocal _log_buf
            # Live progress: every CR/LF-delimited fragment, as it arrives.
            if progress_cb is not None:
                try:
                    norm = chunk.replace('\r\n', '\n')
                    for frag in re.split(r'[\r\n]', norm):
                        if frag:
                            progress_cb(frag)
                except Exception:
                    pass
            # Log: normalise '\r' to '\n' and emit complete lines only.
            _log_buf += chunk.replace('\r', '\n')
            while '\n' in _log_buf:
                ln, _log_buf = _log_buf.split('\n', 1)
                ln = ln.strip()
                if ln:
                    log('[LAZY][mkpfs] ' + ln + '\n')

        _eof = False
        while not _eof:
            if cancel_check is not None:
                try:
                    if cancel_check():
                        cancelled = True
                        log('[LAZY] Cancel requested — stopping Lazy_MkPFS '
                            '(no fallback).\n')
                        _terminate()
                        break
                except Exception:
                    pass
            try:
                chunk = _outq.get(timeout=0.25)
            except _q.Empty:
                if proc.poll() is not None and _outq.empty():
                    break
                continue
            if chunk is None:
                _eof = True
                continue
            _feed(chunk)
        # Drain anything the reader queued after we stopped looping.
        if not cancelled:
            while True:
                try:
                    chunk = _outq.get_nowait()
                except _q.Empty:
                    break
                if chunk is None:
                    break
                _feed(chunk)
        if _log_buf.strip():
            log('[LAZY][mkpfs] ' + _log_buf.strip() + '\n')
        try:
            proc.stdout.close()
        except Exception:
            pass
    except Exception as e:
        log('[LAZY] Error reading Lazy_MkPFS output: %s\n' % e)

    if cancelled:
        try:
            if pre_size < 0 and os.path.isfile(final_ffpfsc):
                os.remove(final_ffpfsc)
        except Exception:
            pass
        return RESULT_CANCELLED

    rc = proc.wait()
    log('[LAZY]   Exit code: %s\n' % rc)

    if cancel_check is not None:
        try:
            if cancel_check():
                log('[LAZY] Cancel requested after run — no fallback.\n')
                try:
                    if pre_size < 0 and os.path.isfile(final_ffpfsc):
                        os.remove(final_ffpfsc)
                except Exception:
                    pass
                return RESULT_CANCELLED
        except Exception:
            pass

    if rc != 0:
        log('[LAZY] Lazy_MkPFS failed (exit %s) — falling back to stock mkpfs\n'
            % rc)
        return RESULT_FALLBACK

    if not os.path.isfile(final_ffpfsc):
        log('[LAZY] Output file not found after run — falling back to stock '
            'mkpfs\n')
        return RESULT_FALLBACK
    try:
        out_size = os.path.getsize(final_ffpfsc)
    except Exception:
        out_size = 0
    if out_size < 1024 * 1024 or (pre_size >= 0 and out_size <= pre_size):
        log('[LAZY] Output implausibly small (%d bytes) — falling back to stock mkpfs\n'
            % out_size)
        try:
            if pre_size < 0:
                os.remove(final_ffpfsc)
        except Exception:
            pass
        return RESULT_FALLBACK

    log('[LAZY] Lazy_MkPFS produced %s (%d bytes). Will run the same '
        'verification as stock output.\n'
        % (os.path.basename(final_ffpfsc), out_size))
    return RESULT_OK


def lazy_worker_main(argv):
    """Run inside the re-invoked frozen exe (or dev interpreter).

    argv is everything AFTER the LAZY_WORKER_FLAG, i.e.:
        [mode, lazy_dir, <main_file args...>]
    mode is currently always 'file' (we only use main_file.py this pass).
    Adds lazy_dir to sys.path so `import lazy_mkpfs` resolves to the vendored
    inner package, then calls the matching entry's main(). Returns an int exit
    code.
    """
    if not argv or len(argv) < 2:
        sys.stderr.write('lazy_worker: missing mode/dir\n')
        return 3
    mode = argv[0]
    lazy_dir = argv[1]
    rest = argv[2:]

    # UTF-8 stdio so Unicode paths/progress never raise in the worker.
    for _stream in (sys.stdout, sys.stderr):
        try:
            _stream.reconfigure(encoding='utf-8', errors='replace')
        except Exception:
            pass

    if lazy_dir and os.path.isdir(lazy_dir) and lazy_dir not in sys.path:
        sys.path.insert(0, lazy_dir)

    # Import the vendored inner package to make sure it resolves.
    if mode == 'file':
        entry_name = 'main_file.py'
        try:
            from lazy_mkpfs import pack_file  # noqa: F401
        except Exception as e:
            sys.stderr.write('lazy_worker: cannot import vendored lazy_mkpfs: %s\n'
                             % e)
            return 4
        # Run main_file.py as a script module (it parses sys.argv itself).
        import importlib.util
        mf = os.path.join(lazy_dir, entry_name)
        spec = importlib.util.spec_from_file_location('lazy_main_file', mf)
        if spec is None or spec.loader is None:
            sys.stderr.write('lazy_worker: cannot load %s\n' % mf)
            return 5
        module = importlib.util.module_from_spec(spec)
        sys.argv = [entry_name] + list(rest)
        try:
            spec.loader.exec_module(module)
            # main_file.py calls main() under __main__ guard; call it here.
            if hasattr(module, 'main'):
                module.main()
            return 0
        except SystemExit as e:
            return int(e.code) if isinstance(e.code, int) else 0
        except Exception as e:
            sys.stderr.write('lazy_worker: pack failed: %s\n' % e)
            return 6
    sys.stderr.write('lazy_worker: unknown mode %r\n' % mode)
    return 7


# Backwards-compatible name used by the pipeline before the bundling change.
def run_experimental_mkpfs(app, src_exfat, final_ffpfsc, *,
                           compression_level, cpu_count, temp_folder, log):
    res = run_lazy_mkpfs(app, src_exfat, final_ffpfsc,
                         compression_level=compression_level,
                         cpu_count=cpu_count, temp_folder=temp_folder,
                         log=log, cancel_check=None)
    return res == RESULT_OK


run_experimental_lazy_mkpfs = run_experimental_mkpfs
