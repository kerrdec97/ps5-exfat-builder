"""End-to-end FPKG build: app folder -> GP5 -> img_create -> checks."""
import datetime
import os
import shutil
import tempfile
import uuid

from core.fpkg import aliases as A, gp5, param as P, playgo, runner, scan as S, toolchain, verify

KEYSTONE_SIZE = 96


class FpkgBuildError(Exception):
    pass


class FpkgOptions:
    def __init__(self, app_dir, out_dir, tool_path=None, compression_level=None,
                 chunk_count=playgo.DEFAULT_CHUNKS, keep_intermediate=False,
                 tool_verify=True, ref_pkg=None, temp_dir=None,
                 passcode=gp5.DEFAULT_PASSCODE):
        self.app_dir = app_dir
        self.out_dir = out_dir
        self.tool_path = tool_path
        self.compression_level = compression_level
        self.chunk_count = chunk_count
        self.keep_intermediate = keep_intermediate
        self.tool_verify = tool_verify
        self.ref_pkg = ref_pkg
        self.temp_dir = temp_dir
        self.passcode = passcode


class FpkgResult:
    def __init__(self):
        self.status = False          # True | 'warn' | False | None (cancelled)
        self.out_path = None
        self.companion_path = None   # update builds: the full .remastered.pkg
        self.content_id = None
        self.messages = []
        self.log_dir = None


def _ascii(path):
    try:
        path.encode('ascii')
        return True
    except UnicodeEncodeError:
        return False


def _unique(path):
    if not os.path.exists(path):
        return path
    stem, ext = os.path.splitext(path)
    n = 2
    while os.path.exists('%s (%d)%s' % (stem, n, ext)):
        n += 1
    return '%s (%d)%s' % (stem, n, ext)


def _free_bytes(path):
    try:
        return shutil.disk_usage(path).free
    except OSError:
        return None


def build_fpkg(opts, log=None, progress=None, cancel_check=None):
    """Build one package. Never raises for build problems: the outcome and
    reasons are in the returned FpkgResult.

    log(text)               one line per call
    progress(pct, step)     pct 0-100 or None, short step text
    cancel_check() -> bool  True to stop (partial output is removed)"""
    log = log or (lambda s: None)
    progress = progress or (lambda p, s: None)
    cancel_check = cancel_check or (lambda: False)
    res = FpkgResult()
    al = A.Aliases(log)
    try:
        _build(opts, res, log, progress, cancel_check, al)
    except FpkgBuildError as e:
        res.status = False
        res.messages.append(str(e))
        log('[FPKG] FAILED: %s' % e)
    except _Cancelled:
        res.status = None
        res.messages.append('Cancelled')
        log('[FPKG] Cancelled.')
    except Exception as e:  # pragma: no cover - reported, never silent
        res.status = False
        res.messages.append('Unexpected error: %r' % (e,))
        log('[FPKG] Unexpected error: %r' % (e,))
    finally:
        al.close()
    return res


class _Cancelled(Exception):
    pass


def _build(opts, res, log, progress, cancel_check, al):
    def check_cancel():
        if cancel_check():
            raise _Cancelled()

    progress(0, 'Checking Publishing Tools')
    tc = toolchain.find_toolchain(opts.tool_path)
    for w in tc.warnings:
        log('[FPKG] Warning: ' + w)
    if not tc.ok:
        raise FpkgBuildError('; '.join(tc.problems))
    log('[FPKG] Publishing Tools: ' + tc.summary())

    real_app_dir = os.path.abspath(opts.app_dir)
    real_out_dir = os.path.abspath(opts.out_dir)
    os.makedirs(real_out_dir, exist_ok=True)
    # The Publishing Tools reject non-ASCII paths: work through ASCII
    # junctions to the real folders (Windows), and report real paths.
    try:
        app_dir = al.map(real_app_dir)
        out_dir = al.map(real_out_dir)
    except OSError as e:
        raise FpkgBuildError(
            'A folder path contains characters the Publishing Tools reject '
            'and no ASCII alias could be made (%s). Move or rename the '
            'folder to a plain ASCII path.' % e)

    def real(p):
        return os.path.join(real_out_dir, os.path.basename(p)) if p else p

    progress(1, 'Reading param.json')
    try:
        prm = P.load(app_dir)
        cid = P.content_id(prm)
        cver = P.version(prm)
    except P.ParamError as e:
        raise FpkgBuildError(str(e))
    res.content_id = cid
    update = bool(opts.ref_pkg)
    if update:
        if not (os.path.isfile(opts.ref_pkg) and opts.ref_pkg.lower().endswith('.pkg')):
            raise FpkgBuildError('Base package not found: %s' % opts.ref_pkg)
        ref_abs = os.path.abspath(opts.ref_pkg)
        if not _ascii(os.path.basename(ref_abs)):
            raise FpkgBuildError('The base package file name contains '
                                 'non-ASCII characters: %s' % opts.ref_pkg)
        try:
            ref_pkg = os.path.join(al.map(os.path.dirname(ref_abs)),
                                   os.path.basename(ref_abs))
        except OSError as e:
            raise FpkgBuildError('No ASCII alias for the base package folder: %s' % e)
        log('[FPKG] Update against base package %s. The base must have been '
            'built by the Publishing Tools, with the same Content ID and a '
            'lower contentVersion.' % opts.ref_pkg)
    final_name = P.output_name(prm, update=update)
    stem = os.path.splitext(P.output_name(prm))[0]
    log('[FPKG] Content ID %s, version %s -> %s' % (cid, cver, final_name))

    ks = os.path.join(app_dir, 'sce_sys', 'keystone')
    if not os.path.isfile(ks):
        raise FpkgBuildError('sce_sys/keystone is missing; the debug '
                             'Publishing Tools need the dump\'s own keystone.')
    if os.path.getsize(ks) != KEYSTONE_SIZE:
        raise FpkgBuildError('sce_sys/keystone is %d bytes, expected %d.'
                             % (os.path.getsize(ks), KEYSTONE_SIZE))

    progress(2, 'Scanning files')
    try:
        sres = S.scan(app_dir, cancel_check=cancel_check)
    except S.ScanError as e:
        if str(e) == 'cancelled':
            raise _Cancelled()
        raise FpkgBuildError(str(e))
    if sres.bad_paths:
        lines = ['%s (%s)' % (r, why) for r, why in sres.bad_paths[:20]]
        more = len(sres.bad_paths) - len(lines)
        raise FpkgBuildError(
            '%d file name(s) are not accepted by the Publishing Tools: %s%s'
            % (len(sres.bad_paths), '; '.join(lines),
               ' ... and %d more' % more if more > 0 else ''))
    if sres.self_ps4_magic:
        log('[FPKG] Warning: %d executable(s) carry the PS4 SELF magic (e.g. '
            '%s); the package may not run.' % (len(sres.self_ps4_magic),
                                               sres.self_ps4_magic[0]))
    log('[FPKG] %d files, %.2f GB; %d left out; %d empty folder(s)'
        % (sres.file_count, sres.total_bytes / 1024**3, len(sres.excluded),
           len(sres.empty_dirs)))
    check_cancel()

    free = _free_bytes(out_dir)
    need = int(sres.total_bytes * 1.1)
    if free is not None and free < need:
        raise FpkgBuildError('Not enough free space in the output folder: '
                             '%.1f GB free, about %.1f GB needed.'
                             % (free / 1024**3, need / 1024**3))

    # ── Generated inputs ─────────────────────────────────────────────
    assets = os.path.join(out_dir, '.gp5-assets', stem)
    logs_dir = os.path.join(out_dir, stem + '-build-logs')
    os.makedirs(assets, exist_ok=True)
    os.makedirs(logs_dir, exist_ok=True)
    res.log_dir = real(logs_dir)
    gp5_path = os.path.join(out_dir, stem + '.gp5')
    scen_path = os.path.join(out_dir, stem + '.playgo-scenario.json')

    try:
        nprm, notes = P.normalise(prm, sres.total_bytes, sres.file_count,
                                  full_app=not update)
    except P.ParamError as e:
        raise FpkgBuildError(str(e))
    for n in notes:
        log('[FPKG] param.json: ' + n)
    prm_path = os.path.join(assets, 'sce_sys', 'param.json')
    os.makedirs(os.path.dirname(prm_path), exist_ok=True)
    with open(prm_path, 'w', encoding='utf-8', newline='') as f:
        f.write(P.dump(nprm))

    try:
        layout = playgo.build_layout(app_dir, P.default_language(nprm),
                                     opts.chunk_count)
    except playgo.PlayGoError as e:
        raise FpkgBuildError(str(e))
    with open(scen_path, 'w', encoding='utf-8', newline='') as f:
        f.write(playgo.scenario_text(layout))
    payloads = playgo.write_payloads(layout, assets)

    files = [('sce_sys/playgo-scenario.json', scen_path, 0, None)]
    for dst, src, _size in sres.files:
        if dst.lower() == 'sce_sys/param.json':
            src = prm_path
        files.append((dst, src, 0, None))
    for dst, src, chunk in payloads:
        files.append((dst, src, chunk, {'pfs_compression': 'disable'}))
    gp5.write(gp5_path, gp5.build_xml(cid, layout, files, sres.empty_dirs,
                                      passcode=opts.passcode))
    with open(os.path.join(logs_dir, '01-create-gp5.log'), 'w',
              encoding='utf-8') as f:
        f.write('project=%s\nfiles=%d\nbytes=%d\nchunks=%d\n'
                % (gp5_path, len(files), sres.total_bytes, layout.chunk_count))
        for n in notes:
            f.write('param: %s\n' % n)
        for rel, why in sres.excluded:
            f.write('excluded: %s  (%s)\n' % (rel, why))
        for d in sres.empty_dirs:
            f.write('empty_dir: %s\n' % d)
    log('[FPKG] Project written: %s' % gp5_path)
    check_cancel()

    # ── Pack ─────────────────────────────────────────────────────────
    partial = os.path.join(out_dir, stem + '.partial.pkg')
    partials = [partial, partial + '.remastered.pkg',
                partial + '.naps_metric.json']
    for p in partials:
        if os.path.exists(p):
            os.remove(p)
    argv = [tc.exe, 'img_create', '--oformat', 'nwonly']
    if opts.compression_level is not None:
        lvl = int(opts.compression_level)
        if not -4 <= lvl <= 9:
            raise FpkgBuildError('compression level must be -4..9')
        argv += ['--compression_level', str(lvl)]
    if update:
        argv += ['--ref_pkg_path', ref_pkg]
    argv += [gp5_path, partial]

    _tmp_base = opts.temp_dir if (opts.temp_dir and os.path.isdir(opts.temp_dir)
                                  and _ascii(opts.temp_dir)) else None
    if _tmp_base is None and not _ascii(tempfile.gettempdir()):
        _tmp_base = A.alias_root()
    sdk_tmp = tempfile.mkdtemp(prefix='sdk-tmp-%s-' % uuid.uuid4().hex[:8],
                               dir=_tmp_base)
    env = dict(os.environ, TEMP=sdk_tmp, TMP=sdk_tmp)
    cwd = tc.root if _ascii(tc.root) else sdk_tmp
    started = datetime.datetime.now(datetime.timezone.utc)

    def on_progress(frac, written):
        if frac is not None:
            progress(3 + 90 * frac, 'Packing (%d%%)' % int(frac * 100))
        elif written is not None:
            progress(None, 'Packing — %.2f GB written' % (written / 1024**3))

    def on_log(kind, line):
        log('[FPKG] %s%s' % ({'error': 'ERROR: ', 'warn': 'Warning: '}
                             .get(kind, ''), line))

    progress(3, 'Packing')
    log('[FPKG] Running: ' + ' '.join('"%s"' % a if ' ' in a else a for a in argv))
    try:
        rr = runner.run(argv, cwd=cwd, env=env, log=on_log,
                        progress=on_progress, cancel_check=cancel_check,
                        watch_file=partial)
    except OSError as e:
        raise FpkgBuildError('Could not start the Publishing Tools: %s' % e)
    finally:
        shutil.rmtree(sdk_tmp, ignore_errors=True)
    with open(os.path.join(logs_dir, '02-img-create.log'), 'w',
              encoding='utf-8') as f:
        f.write('command=%s\nstarted_utc=%s\nfinished_utc=%s\nelapsed=%.1f\n'
                % (argv, started.isoformat(),
                   datetime.datetime.now(datetime.timezone.utc).isoformat(),
                   rr.elapsed))
        f.write('exit_code=%s\n' % ('cancelled' if rr.cancelled else rr.returncode))
        for line in rr.lines:
            f.write(line.rstrip() + '\n')

    def drop_partials():
        for p in partials:
            try:
                if os.path.exists(p):
                    os.remove(p)
            except OSError as e:
                log('[FPKG] Could not remove %s: %s' % (p, e))

    if rr.cancelled:
        drop_partials()
        raise _Cancelled()
    if not rr.ok or not os.path.isfile(partial):
        drop_partials()
        why = []
        if rr.returncode not in (0, None):
            why.append('exit code %s' % rr.returncode)
            hint = toolchain.exit_hint(rr.returncode)
            if hint:
                why.append(hint)
        if rr.errors:
            why.append(rr.errors[0])
            why.extend(runner.explain(rr.errors))
        if rr.ok and not os.path.isfile(partial):
            why.append('the tool reported success but wrote no package')
        raise FpkgBuildError('Packing failed: ' + '; '.join(why or ['unknown error']))
    progress(94, 'Checking package')

    # ── Finalise ─────────────────────────────────────────────────────
    target_for_check = partial
    companion_src = partial + '.remastered.pkg'
    if update:
        if not os.path.isfile(companion_src):
            drop_partials()
            raise FpkgBuildError('The update was built but its full-package '
                                 'companion (.remastered.pkg) is missing.')
        target_for_check = companion_src
    hc = verify.check_header(target_for_check)
    if not hc.ok:
        drop_partials()
        raise FpkgBuildError('The package failed the header check: '
                             + '; '.join(hc.problems))
    final = _unique(os.path.join(out_dir, final_name))
    os.replace(partial, final)
    res.out_path = real(final)
    if update:
        comp = _unique(os.path.join(out_dir, stem + '.remastered.pkg'))
        os.replace(companion_src, comp)
        res.companion_path = real(comp)
    try:
        if os.path.exists(partial + '.naps_metric.json'):
            os.remove(partial + '.naps_metric.json')
    except OSError:
        pass
    log('[FPKG] Package written: %s (%.2f GB)' % (res.out_path, os.path.getsize(final) / 1024**3))
    res.status = True

    if opts.tool_verify and not update:
        progress(96, 'Verifying with img_verify')
        vr = runner.run(verify.img_verify_argv(tc.exe, final, opts.passcode),
                        cwd=cwd, log=on_log, cancel_check=cancel_check)
        if vr.cancelled:
            log('[FPKG] Verification cancelled; the package was kept.')
            res.status = 'warn'
        elif vr.ok:
            log('[FPKG] img_verify passed.')
        else:
            res.status = 'warn'
            msg = ('img_verify reported a problem (exit %s%s); the package '
                   'was kept, check it before use.'
                   % (vr.returncode, (': ' + vr.errors[0]) if vr.errors else ''))
            res.messages.append(msg)
            log('[FPKG] Warning: ' + msg)

    if not opts.keep_intermediate:
        for p in (gp5_path, scen_path):
            try:
                os.remove(p)
            except OSError:
                pass
        shutil.rmtree(assets, ignore_errors=True)
        try:
            os.rmdir(os.path.join(out_dir, '.gp5-assets'))
        except OSError:
            pass
    progress(100, 'Done')


def _tool_or_error(tool_path):
    tc = toolchain.find_toolchain(tool_path)
    if not tc.ok:
        raise FpkgBuildError('; '.join(tc.problems))
    return tc


def verify_package(tool_path, pkg, passcode=gp5.DEFAULT_PASSCODE, log=None,
                   cancel_check=None, integrity=True):
    """Check an existing .pkg: header checks, then the user's img_verify.
    Returns (status, messages) with status True | 'warn' | False | None."""
    log = log or (lambda s: None)
    msgs = []
    try:
        if not os.path.isfile(pkg):
            raise FpkgBuildError('Package not found: %s' % pkg)
        hc = verify.check_header(pkg)
        for p in hc.problems:
            msgs.append('Header: ' + p)
            log('[FPKG] Header: ' + p)
        if hc.ok:
            log('[FPKG] Header OK (unsigned debug package, plaintext marker).')
        tc = _tool_or_error(tool_path)
        rr = runner.run(verify.img_verify_argv(tc.exe, pkg, passcode, integrity),
                        cwd=tc.root, cancel_check=cancel_check,
                        log=lambda k, line: log('[FPKG] ' + line))
        if rr.cancelled:
            return None, msgs + ['Cancelled']
        if rr.ok:
            log('[FPKG] img_verify passed.')
            return (True if hc.ok else 'warn'), msgs
        why = 'img_verify failed (exit %s)%s' % (
            rr.returncode, (': ' + rr.errors[0]) if rr.errors else '')
        log('[FPKG] ' + why)
        return False, msgs + [why]
    except FpkgBuildError as e:
        log('[FPKG] ' + str(e))
        return False, msgs + [str(e)]


def extract_package(tool_path, pkg, dest, passcode=gp5.DEFAULT_PASSCODE,
                    log=None, cancel_check=None):
    """Extract an existing .pkg into an empty folder with img_extract.
    Returns (status, messages). The destination must be empty or absent;
    a cancelled or failed extraction leaves what was written for review."""
    log = log or (lambda s: None)
    try:
        if not os.path.isfile(pkg):
            raise FpkgBuildError('Package not found: %s' % pkg)
        if os.path.isdir(dest) and os.listdir(dest):
            raise FpkgBuildError('The destination folder is not empty: %s' % dest)
        for label, p in (('package', pkg), ('destination', dest)):
            if not _ascii(os.path.abspath(p)):
                raise FpkgBuildError('The %s path contains non-ASCII '
                                     'characters: %s' % (label, p))
        tc = _tool_or_error(tool_path)
        os.makedirs(dest, exist_ok=True)
        rr = runner.run(verify.img_extract_argv(tc.exe, pkg, dest, passcode),
                        cwd=tc.root, cancel_check=cancel_check,
                        log=lambda k, line: log('[FPKG] ' + line))
        if rr.cancelled:
            return None, ['Cancelled; partial output left in %s' % dest]
        if not rr.ok:
            why = 'img_extract failed (exit %s)%s' % (
                rr.returncode, (': ' + rr.errors[0]) if rr.errors else '')
            log('[FPKG] ' + why)
            return False, [why]
        n = sum(len(f) for _r, _d, f in os.walk(dest))
        log('[FPKG] Extracted %d file(s) to %s' % (n, dest))
        if n == 0:
            return False, ['img_extract reported success but wrote no files']
        return True, []
    except FpkgBuildError as e:
        log('[FPKG] ' + str(e))
        return False, [str(e)]
