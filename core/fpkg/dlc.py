"""Add-on (DLC) packages through the user's Publishing Tools.

What the Publishing Tools interface offers (from the strings of
prospero-pub-cmd.exe recorded in docs/farewell/FPKG_RESEARCH.md; the
reference FPKG builder never used it, so this path is UNTESTED with real
tools or a console):

* volume types ``prospero_app`` (game), ``prospero_ac`` (add-on with
  content) and ``prospero_al`` (entitlement-only add-on);
* an ``entitlement_key`` (32 hex characters) on the GP5 ``<package>``.

Supported here: ``prospero_ac`` - an add-on with a content folder, which
must carry its own ``sce_sys/param.json`` (as a dumped add-on does).

Not supported, by design:
* ``prospero_al`` (entitlement-only, no content): it produces a bare
  content header without a file system, which the PS5 debug installer
  refuses. Refused with that explanation.
* Generating an add-on's ``param.json``: the fields Publishing Tools
  require for ``prospero_ac`` are not documented, so the app does not
  invent them. An unlock-only add-on needs a folder with a real
  ``sce_sys/param.json`` (it may hold nothing else).
* Finding the entitlement key: the user supplies it, or it is taken from
  the game's ``dlc_emu.ini`` entry when that entry has one.
"""
import os
import re
import shutil
import tempfile
import uuid

from core.fpkg import aliases as A, gp5, param as P, runner, scan as S, toolchain, verify
from core.fpkg.backend import FpkgBuildError, FpkgResult, _free_bytes, _unique

KEY_RE = re.compile(r'^[0-9A-Fa-f]{32}$')
VOLUME_AC = 'prospero_ac'
VOLUME_AL = 'prospero_al'


class AddonOptions:
    def __init__(self, payload_dir, out_dir, tool_path=None, content_id=None,
                 entitlement_key=None, volume_type=VOLUME_AC,
                 compression_level=None, keep_intermediate=False,
                 tool_verify=True, temp_dir=None, passcode=gp5.DEFAULT_PASSCODE):
        self.payload_dir = payload_dir
        self.out_dir = out_dir
        self.tool_path = tool_path
        self.content_id = content_id
        self.entitlement_key = entitlement_key
        self.volume_type = volume_type
        self.compression_level = compression_level
        self.keep_intermediate = keep_intermediate
        self.tool_verify = tool_verify
        self.temp_dir = temp_dir
        self.passcode = passcode


def addons_from_dlc_emu(game_dir):
    """Add-on entries declared in the game's dlc_emu.ini.

    Returns a list of dicts: content_id, kind ('PSAC'/'PSAL'/...),
    payload_dir (the local mount-point folder when it exists, else None),
    entitlement_key (or None), supported (bool) and reason."""
    from ui.dlc_emu_ini import parse_ini
    path = os.path.join(game_dir, 'dlc_emu.ini')
    try:
        with open(path, encoding='utf-8-sig', errors='replace') as f:
            entries = parse_ini(f.read())
    except OSError:
        return []
    out = []
    for e in entries:
        kind = (e.section or '').upper()
        cid = (e.get('content_id') or '').strip()
        mp = (e.get('mount_point') or '').strip()
        payload = None
        if mp:
            rel = mp.split('/app0/', 1)[-1].strip('/\\')
            cand = os.path.join(game_dir, rel)
            if rel and os.path.isdir(cand):
                payload = cand
        key = (e.get('entitlement_key') or '').strip() or None
        supported, reason = True, ''
        if kind == 'PSAL':
            supported, reason = False, ('entitlement-only add-ons are refused '
                                        'by the PS5 debug installer')
        elif kind != 'PSAC':
            supported, reason = False, 'not an add-on entry'
        elif not payload:
            supported, reason = False, ('no local content folder with '
                                        'sce_sys/param.json')
        elif not os.path.isfile(os.path.join(payload, 'sce_sys', 'param.json')):
            supported, reason = False, 'its folder has no sce_sys/param.json'
        out.append({'content_id': cid, 'kind': kind, 'payload_dir': payload,
                    'entitlement_key': key, 'supported': supported,
                    'reason': reason})
    return out


def build_addon(opts, log=None, progress=None, cancel_check=None):
    """Build one add-on package. Never raises for build problems."""
    log = log or (lambda s: None)
    progress = progress or (lambda p, s: None)
    cancel_check = cancel_check or (lambda: False)
    res = FpkgResult()
    al = A.Aliases(log)
    try:
        _build_addon(opts, res, log, progress, cancel_check, al)
    except FpkgBuildError as e:
        res.status = False
        res.messages.append(str(e))
        log('[FPKG-DLC] FAILED: %s' % e)
    except _Cancelled:
        res.status = None
        res.messages.append('Cancelled')
        log('[FPKG-DLC] Cancelled.')
    except Exception as e:  # pragma: no cover
        res.status = False
        res.messages.append('Unexpected error: %r' % (e,))
        log('[FPKG-DLC] Unexpected error: %r' % (e,))
    finally:
        al.close()
    return res


class _Cancelled(Exception):
    pass


def _build_addon(opts, res, log, progress, cancel_check, al):
    if opts.volume_type == VOLUME_AL:
        raise FpkgBuildError(
            'Entitlement-only add-ons (prospero_al) are not built: they have '
            'no file system and the PS5 debug installer refuses them. Build '
            'an add-on with a content folder (prospero_ac) instead.')
    if opts.volume_type != VOLUME_AC:
        raise FpkgBuildError('Unknown add-on volume type %r' % opts.volume_type)
    key = (opts.entitlement_key or '').strip()
    if not KEY_RE.match(key):
        raise FpkgBuildError('The entitlement key must be 32 hexadecimal '
                             'characters (got %d characters).' % len(key))
    progress(0, 'Checking Publishing Tools')
    tc = toolchain.find_toolchain(opts.tool_path)
    for w in tc.warnings:
        log('[FPKG-DLC] Warning: ' + w)
    if not tc.ok:
        raise FpkgBuildError('; '.join(tc.problems))

    real_src = os.path.abspath(opts.payload_dir or '')
    real_out = os.path.abspath(opts.out_dir)
    if not os.path.isdir(real_src):
        raise FpkgBuildError('Add-on content folder not found: %s' % real_src)
    prm_path = os.path.join(real_src, 'sce_sys', 'param.json')
    if not os.path.isfile(prm_path):
        raise FpkgBuildError(
            'The add-on folder has no sce_sys/param.json. An add-on package '
            'needs the add-on\'s own param.json (as in a dumped add-on); the '
            'app does not generate one because the fields the Publishing '
            'Tools require for add-ons are not documented.')
    try:
        prm = P.load(real_src)
    except P.ParamError as e:
        raise FpkgBuildError(str(e))
    cid = (opts.content_id or prm.get('contentId') or '').strip()
    try:
        P.content_id({'contentId': cid})
    except P.ParamError as e:
        raise FpkgBuildError(str(e))
    if prm.get('contentId') and prm.get('contentId') != cid:
        raise FpkgBuildError('The Content ID %s does not match the add-on\'s '
                             'param.json (%s).' % (cid, prm.get('contentId')))
    res.content_id = cid
    try:
        name = P.output_name(dict(prm, contentId=cid))
    except P.ParamError:
        name = '%s-A0100-V0100.pkg' % cid
    stem = os.path.splitext(name)[0]
    os.makedirs(real_out, exist_ok=True)
    try:
        src = al.map(real_src)
        out = al.map(real_out)
    except OSError as e:
        raise FpkgBuildError('No ASCII alias could be made for a folder: %s' % e)

    progress(2, 'Scanning add-on files')
    try:
        sres = S.scan(src, cancel_check=cancel_check, require_eboot=False)
    except S.ScanError as e:
        if str(e) == 'cancelled':
            raise _Cancelled()
        raise FpkgBuildError(str(e))
    if sres.bad_paths:
        raise FpkgBuildError('%d file name(s) are not accepted by the Publishing '
                             'Tools, e.g. %s (%s)' % ((len(sres.bad_paths),)
                                                      + sres.bad_paths[0]))
    free = _free_bytes(out)
    if free is not None and free < int(sres.total_bytes * 1.1):
        raise FpkgBuildError('Not enough free space in the output folder.')
    if cancel_check():
        raise _Cancelled()

    logs_dir = os.path.join(out, stem + '-build-logs')
    os.makedirs(logs_dir, exist_ok=True)
    res.log_dir = os.path.join(real_out, os.path.basename(logs_dir))
    gp5_path = os.path.join(out, stem + '.gp5')
    files = [(dst, path, 0, None) for dst, path, _sz in sres.files]
    gp5.write(gp5_path, gp5.build_xml(cid, None, files, sres.empty_dirs,
                                      passcode=opts.passcode,
                                      volume_type=VOLUME_AC,
                                      entitlement_key=key.lower()))
    log('[FPKG-DLC] Add-on project written: %s (%d files)' % (gp5_path, len(files)))

    partial = os.path.join(out, stem + '.partial.pkg')
    for p in (partial, partial + '.naps_metric.json'):
        if os.path.exists(p):
            os.remove(p)
    argv = [tc.exe, 'img_create', '--oformat', 'nwonly']
    if opts.compression_level is not None:
        argv += ['--compression_level', str(int(opts.compression_level))]
    argv += [gp5_path, partial]
    tmp_base = opts.temp_dir if (opts.temp_dir and A.is_ascii(opts.temp_dir)
                                 and os.path.isdir(opts.temp_dir)) else None
    if tmp_base is None and not A.is_ascii(tempfile.gettempdir()):
        tmp_base = A.alias_root()
    sdk_tmp = tempfile.mkdtemp(prefix='sdk-tmp-%s-' % uuid.uuid4().hex[:8], dir=tmp_base)
    progress(3, 'Packing add-on')
    try:
        rr = runner.run(argv, cwd=tc.root if A.is_ascii(tc.root) else sdk_tmp,
                        env=dict(os.environ, TEMP=sdk_tmp, TMP=sdk_tmp),
                        log=lambda k, line: log('[FPKG-DLC] ' + line),
                        progress=lambda f, b: progress(3 + 90 * f, 'Packing add-on')
                        if f is not None else None,
                        cancel_check=cancel_check, watch_file=partial)
    except OSError as e:
        raise FpkgBuildError('Could not start the Publishing Tools: %s' % e)
    finally:
        shutil.rmtree(sdk_tmp, ignore_errors=True)
    with open(os.path.join(logs_dir, '02-img-create.log'), 'w', encoding='utf-8') as f:
        f.write('command=%s\nexit_code=%s\n' % (argv, 'cancelled' if rr.cancelled
                                                 else rr.returncode))
        for line in rr.lines:
            f.write(line.rstrip() + '\n')

    def drop():
        for p in (partial, partial + '.naps_metric.json'):
            try:
                if os.path.exists(p):
                    os.remove(p)
            except OSError:
                pass
    if rr.cancelled:
        drop()
        raise _Cancelled()
    if not rr.ok or not os.path.isfile(partial):
        drop()
        why = ['exit code %s' % rr.returncode] if rr.returncode else []
        why += rr.errors[:1] + runner.explain(rr.errors)
        hint = toolchain.exit_hint(rr.returncode)
        if hint:
            why.append(hint)
        raise FpkgBuildError('Packing the add-on failed: ' +
                             '; '.join(why or ['no package was written']))
    hc = verify.check_header(partial)
    if not hc.ok:
        drop()
        raise FpkgBuildError('The add-on package failed the header check: '
                             + '; '.join(hc.problems))
    final = _unique(os.path.join(out, name))
    os.replace(partial, final)
    res.out_path = os.path.join(real_out, os.path.basename(final))
    res.status = True
    log('[FPKG-DLC] Add-on package written: %s' % res.out_path)
    if opts.tool_verify:
        vr = runner.run(verify.img_verify_argv(tc.exe, final, opts.passcode),
                        cwd=tc.root, cancel_check=cancel_check,
                        log=lambda k, line: log('[FPKG-DLC] ' + line))
        if not vr.ok:
            res.status = 'warn'
            res.messages.append('img_verify reported a problem (exit %s); the '
                                'package was kept.' % vr.returncode)
    if not opts.keep_intermediate:
        try:
            os.remove(gp5_path)
        except OSError:
            pass
    progress(100, 'Done')
