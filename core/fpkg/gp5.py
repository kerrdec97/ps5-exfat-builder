"""Write a Publishing Tools GP5 project."""
import xml.etree.ElementTree as ET

from core.fpkg.playgo import chunk_range_text

DEFAULT_PASSCODE = '0' * 32


def build_xml(content_id, layout, files, empty_dirs=(), passcode=DEFAULT_PASSCODE,
              volume_type='prospero_app'):
    """Return the GP5 document as text (CRLF line endings).

    files: iterable of (dst_rel, abs_src, chunk, extra_attrs or None),
    already in the order the image should be laid out."""
    if len(passcode) != 32:
        raise ValueError('passcode must be 32 characters')
    root = ET.Element('psproject', {'fmt': 'gp5'})
    vol = ET.SubElement(root, 'volume')
    ET.SubElement(vol, 'volume_type').text = volume_type
    ET.SubElement(vol, 'package', {'passcode': passcode,
                                   'content_id': content_id})
    if volume_type == 'prospero_app':
        n = layout.chunk_count
        ci = ET.SubElement(vol, 'chunk_info', {
            'chunk_count': str(n), 'scenario_count': str(len(layout.scenarios))})
        all_langs = ' '.join(layout.languages)
        chunks = ET.SubElement(ci, 'chunks', {
            'supported_languages': all_langs,
            'default_language': layout.default_language})
        per_chunk = {}
        for _dst, chunk, langs in layout.payloads:
            if langs:
                per_chunk.setdefault(chunk, []).extend(langs)
        for c in range(n):
            ET.SubElement(chunks, 'chunk', {
                'id': str(c), 'label': 'Chunk #%d' % c, 'layer_no': '0',
                'languages': ' '.join(per_chunk.get(c) or layout.languages)})
        scs = ET.SubElement(ci, 'scenarios', {'default_id': '0'})
        for sid, label in layout.scenarios:
            el = ET.SubElement(scs, 'scenario', {
                'id': str(sid), 'type': 'playmode',
                'initial_chunk_count': str(n), 'label': label})
            el.text = chunk_range_text(n)
    fe = ET.SubElement(root, 'files')
    for dst, src, chunk, extra in files:
        attrs = {'dst_path': dst, 'src_path': src, 'chunk': str(chunk)}
        if extra:
            attrs.update(extra)
        ET.SubElement(fe, 'file', attrs)
    for d in empty_dirs:
        ET.SubElement(fe, 'empty_dir', {'dst_path': d})
    ET.indent(root, space='  ')
    body = ET.tostring(root, encoding='unicode')
    text = "<?xml version='1.0' encoding='utf-8'?>\n" + body + '\n'
    return text.replace('\r\n', '\n').replace('\n', '\r\n')


def write(path, text):
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)
