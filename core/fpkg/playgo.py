"""PlayGo layout for a package built from a dump.

A dump rarely carries a usable chunk table, so the package uses a simple
layout: all game files in chunk 0, every chunk "initial" (installed before
the game can start) and every scenario listing every chunk. Chunks that
would hold no data get a 1 MiB placeholder, because an empty chunk counts
as "not installed" and the console stalls at "Installing... 0 GB".
Languages from the dump's scenario file each get a small payload in their
own chunk where there are enough chunks.
"""
import json
import os

PLACEHOLDER_SIZE = 1024 * 1024
MAX_CHUNKS = 255
DEFAULT_CHUNKS = 100

# The languages PlayGo accepts as scenario/chunk languages.
LANGUAGES = (
    'ar-AE', 'cs-CZ', 'da-DK', 'de-DE', 'el-GR', 'en-GB', 'en-US', 'es-419',
    'es-ES', 'fi-FI', 'fr-CA', 'fr-FR', 'hu-HU', 'id-ID', 'it-IT', 'ja-JP',
    'ko-KR', 'nl-NL', 'no-NO', 'pl-PL', 'pt-BR', 'pt-PT', 'ro-RO', 'ru-RU',
    'sv-SE', 'th-TH', 'tr-TR', 'uk-UA', 'vi-VN', 'zh-Hans', 'zh-Hant')


class PlayGoError(ValueError):
    pass


class Layout:
    def __init__(self, chunk_count, languages, default_language,
                 scenarios, scenario_json, payloads):
        self.chunk_count = chunk_count
        self.languages = languages            # list of language codes
        self.default_language = default_language
        self.scenarios = scenarios            # list of (id, label)
        self.scenario_json = scenario_json    # dict written as the scenario file
        self.payloads = payloads              # (dst_rel, chunk, languages|None)


def _load_source_scenario(app_dir):
    p = os.path.join(app_dir, 'sce_sys', 'playgo-scenario.json')
    try:
        with open(p, encoding='utf-8-sig') as f:
            data = json.load(f)
        return data if isinstance(data, dict) else None
    except (OSError, ValueError):
        return None


def _languages_from(src, default_language):
    langs = []
    if src:
        cand = src.get('chunkSupportedLanguages') or src.get('supportedLanguages')
        if isinstance(cand, list):
            langs = [x for x in cand if x in LANGUAGES]
    if default_language not in LANGUAGES:
        default_language = 'en-US'
    if default_language not in langs:
        langs.insert(0, default_language)
    return langs, default_language


def build_layout(app_dir, default_language='en-US', chunk_count=DEFAULT_CHUNKS):
    try:
        chunk_count = int(chunk_count)
    except (TypeError, ValueError):
        raise PlayGoError('chunk count must be a number')
    if not 1 <= chunk_count <= MAX_CHUNKS:
        raise PlayGoError('chunk count must be 1-%d' % MAX_CHUNKS)
    src = _load_source_scenario(app_dir)
    if src and src.get('chunkDefaultLanguage') in LANGUAGES:
        default_language = src['chunkDefaultLanguage']
    langs, default_language = _languages_from(src, default_language)

    # Scenarios: keep the dump's ids and labels when they are sane (1-5,
    # contiguous from 0); otherwise one scenario.
    scenarios = []
    if src and isinstance(src.get('scenarios'), list):
        ids = []
        for i, sc in enumerate(src['scenarios']):
            if not isinstance(sc, dict):
                break
            sid = sc.get('id', i)
            ids.append(sid)
            label = sc.get('label') or sc.get('name') or 'Scenario #%d' % i
            if not isinstance(label, str):
                label = 'Scenario #%d' % i
            scenarios.append((i, label))
        if not (1 <= len(scenarios) <= 5) or ids != list(range(len(ids))):
            scenarios = []
    if not scenarios:
        scenarios = [(0, 'Scenario #0')]

    payloads = []
    # Language payloads in chunks 1.. (languages beyond the last chunk
    # share it). With a single chunk everything is in chunk 0.
    if chunk_count > 1:
        for i, lang in enumerate(langs):
            chunk = min(1 + i, chunk_count - 1)
            payloads.append(('playgo-languages/%02d-%s.bin' % (i + 1, lang),
                             chunk, [lang]))
    used = {0} | {c for _d, c, _l in payloads}
    for c in range(1, chunk_count):
        if c not in used:
            payloads.append(('playgo-chunks/%03d.bin' % c, c, None))

    all_chunks = list(range(chunk_count))
    scenario_json = {
        'scenarioCount': len(scenarios),
        'scenarioDefaultId': 0,
        'scenarioDefaultLanguage': default_language,
        'chunkDefaultLanguage': default_language,
        'chunkSupportedLanguages': langs,
        'scenarios': [
            {'id': sid, 'type': 'playmode', 'initialChunkCount': chunk_count,
             'chunks': all_chunks,
             'localizedLabels': {default_language: label}}
            for sid, label in scenarios],
    }
    return Layout(chunk_count, langs, default_language, scenarios,
                  scenario_json, payloads)


def chunk_range_text(n):
    return '0' if n == 1 else '0-%d' % (n - 1)


def write_payloads(layout, assets_dir):
    """Create the placeholder payload files. Returns
    [(dst_rel, abs_path, chunk)]."""
    out = []
    zero = b'\0' * 65536
    for dst, chunk, _langs in layout.payloads:
        p = os.path.join(assets_dir, *dst.split('/'))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if not (os.path.isfile(p) and os.path.getsize(p) == PLACEHOLDER_SIZE):
            with open(p, 'wb') as f:
                for _ in range(PLACEHOLDER_SIZE // len(zero)):
                    f.write(zero)
        out.append((dst, p, chunk))
    return out


def scenario_text(layout):
    return (json.dumps(layout.scenario_json, indent=2, ensure_ascii=False)
            + '\n').replace('\n', '\r\n')
