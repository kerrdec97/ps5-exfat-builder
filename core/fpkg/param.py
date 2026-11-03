"""sce_sys/param.json: reading, checks, normalisation and output naming."""
import json
import os
import re

CONTENT_ID_RE = re.compile(r'^[A-Z]{2}[0-9]{4}-[A-Z]{4}[0-9]{5}_[0-9]{2}-[A-Z0-9]{16}$')
VERSION_RE = re.compile(r'^(\d{2})\.(\d{3})\.(\d{3})$')

# Package size classes (attributePub): the upper bound in bytes for 0/1/2.
SIZE_CLASSES = ((0, 162514599936), (1, 195878191104), (2, 260436918272))
LV2_MAX_FILES = 500000
# kernel.addcontMountLevel limits (bytes).
MOUNT_LEVEL_2_MAX = 97945387008


class ParamError(ValueError):
    pass


def load(app_dir):
    path = os.path.join(app_dir, 'sce_sys', 'param.json')
    try:
        with open(path, encoding='utf-8-sig') as f:
            data = json.load(f)
    except FileNotFoundError:
        raise ParamError('sce_sys/param.json was not found in %s' % app_dir)
    except (OSError, ValueError) as e:
        raise ParamError('sce_sys/param.json could not be read: %s' % e)
    if not isinstance(data, dict):
        raise ParamError('sce_sys/param.json is not a JSON object')
    return data


def content_id(param):
    cid = str(param.get('contentId') or '').strip()
    if not CONTENT_ID_RE.match(cid):
        raise ParamError('contentId %r is not a valid PS5 Content ID '
                         '(e.g. UP0000-PPSA00000_00-0000000000000000)' % cid)
    return cid


def title_id(cid):
    return cid[7:16]


def version(param, key='contentVersion'):
    v = str(param.get(key) or '').strip()
    if not VERSION_RE.match(v):
        raise ParamError('%s %r is not in the form 01.000.000' % (key, v))
    return v


def version_tag(v):
    """'01.000.000' -> '0100', '01.512.000' -> '01512'."""
    m = VERSION_RE.match(v)
    if not m:
        raise ParamError('bad version %r' % v)
    return m.group(1) + '%02d' % int(m.group(2))


def output_name(param, update=False):
    cid = content_id(param)
    cv = version(param)
    try:
        av = version(param, 'masterVersion')
    except ParamError:
        av = cv
    name = '%s-A%s-V%s.pkg' % (cid, version_tag(av), version_tag(cv))
    return ('UPDATE_' + name) if update else name


def compare_versions(a, b):
    """-1/0/1 for two 'NN.NNN.NNN' versions."""
    ta = tuple(int(x) for x in a.split('.'))
    tb = tuple(int(x) for x in b.split('.'))
    return (ta > tb) - (ta < tb)


def estimate_size(payload_bytes, file_count):
    return payload_bytes + max(512 * 1024 * 1024, payload_bytes // 100) \
        + 4096 * file_count


def normalise(param, payload_bytes, file_count, full_app=True):
    """Return (new_param, notes). The input is not modified.

    * applicationDrmType is always "standard" (a "free" DRM type shows a
      lock on the console and the game refuses to start);
    * full app builds get the package size class chosen from the data
      size, and an addcont mount level the size class can carry."""
    p = json.loads(json.dumps(param))
    notes = []
    if p.get('applicationDrmType') != 'standard':
        notes.append('applicationDrmType %r -> "standard"'
                     % p.get('applicationDrmType'))
        p['applicationDrmType'] = 'standard'
    if not full_app:
        return p, notes
    try:
        category = int(p.get('applicationCategoryType', 0))
    except (TypeError, ValueError):
        category = -1
    if category != 0:
        notes.append('applicationCategoryType is %r, not a game app: package '
                     'size class left unchanged' % p.get('applicationCategoryType'))
        return p, notes
    est = estimate_size(payload_bytes, file_count)
    cls = None
    for c, limit in SIZE_CLASSES:
        if est <= limit:
            cls = c
            break
    if cls is None:
        raise ParamError('The game is too large for a PS5 package '
                         '(estimated %.1f GB, limit %.1f GB).'
                         % (est / 1e9, SIZE_CLASSES[-1][1] / 1e9))
    if cls == 2 and file_count > LV2_MAX_FILES:
        raise ParamError('Too many files for this package size (%d, limit %d).'
                         % (file_count, LV2_MAX_FILES))
    if p.get('attributePub') != cls:
        notes.append('attributePub -> %d (estimated %.1f GB)' % (cls, est / 1e9))
        p['attributePub'] = cls
    kernel = p.get('kernel')
    if isinstance(kernel, dict) and 'addcontMountLevel' in kernel:
        try:
            lvl = int(kernel['addcontMountLevel'])
        except (TypeError, ValueError):
            lvl = None
        new = lvl
        if lvl == 2 and est > MOUNT_LEVEL_2_MAX:
            new = 1
        if new == 1 and est > SIZE_CLASSES[0][1]:
            new = 0
        if new != lvl:
            kernel['addcontMountLevel'] = new
            notes.append('WARNING: kernel.addcontMountLevel %s -> %s for this '
                         'package size; separately installed DLC may not '
                         'mount.' % (lvl, new))
    return p, notes


def default_language(param):
    loc = param.get('localizedParameters')
    if isinstance(loc, dict):
        lang = loc.get('defaultLanguage')
        if isinstance(lang, str) and lang:
            return lang
    return 'en-US'


def dump(param):
    """param.json text as the Publishing Tools scripts write it (CRLF)."""
    return (json.dumps(param, indent=2, ensure_ascii=False) + '\n').replace(
        '\n', '\r\n')
