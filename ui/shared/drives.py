"""Drive letters that are taken, including network mappings.

An elevated process does not see drive letters the user mapped to network
shares in their normal (unelevated) session: GetLogicalDrives() reports
them as free. Mounting an image on such a letter fails or hides the share
(issues #64, #42). The user's mappings are recorded in the registry under
HKEY_USERS\\<sid>\\Network\\<letter>, which the elevated process can read.
"""
import os


def network_mapped_letters():
    """Upper-case letters mapped to network shares in any loaded user hive
    (persistent mappings). Empty off Windows or on any error."""
    if os.name != 'nt':
        return set()
    letters = set()
    try:
        import winreg
    except ImportError:
        return letters
    roots = []
    try:
        roots.append((winreg.HKEY_CURRENT_USER, 'Network'))
        with winreg.OpenKey(winreg.HKEY_USERS, '') as hu:
            i = 0
            while True:
                try:
                    sid = winreg.EnumKey(hu, i)
                except OSError:
                    break
                i += 1
                if not sid.endswith('_Classes'):
                    roots.append((winreg.HKEY_USERS, sid + '\\Network'))
    except OSError:
        pass
    for hive, path in roots:
        try:
            with winreg.OpenKey(hive, path) as k:
                j = 0
                while True:
                    try:
                        name = winreg.EnumKey(k, j)
                    except OSError:
                        break
                    j += 1
                    if len(name) == 1 and name.isalpha():
                        letters.add(name.upper())
        except OSError:
            continue
    return letters


def used_letter_mask():
    """GetLogicalDrives()-style bitmask that also marks network-mapped
    letters as used, for picking a letter to mount an image on."""
    mask = 0
    try:
        import ctypes
        mask = int(ctypes.windll.kernel32.GetLogicalDrives())
    except Exception:
        mask = 0
    for ch in network_mapped_letters():
        mask |= 1 << (ord(ch) - 65)
    return mask
