import tkinter as tk
from tkinter import filedialog, messagebox, ttk, simpledialog
import subprocess
import threading
import os
import sys
import ctypes
import base64
import tempfile
import shutil
import time
import re
import struct
import json

APP_VERSION = '1.3.0'

_BAT_B64 = "QGVjaG8gb2ZmDQpzZXRsb2NhbCBFbmFibGVFeHRlbnNpb25zDQoNClJFTSBtYWtlX2ltYWdlLmJhdA0KUkVNIFVzYWdlOiBtYWtlX2ltYWdlLmJhdCAiQzpcaW1hZ2VzXGRhdGEuZXhmYXQiICJDOlxwYXlsb2FkIg0KDQppZiAiJX4xIj09IiIgZ290byA6dXNhZ2UNCmlmICIlfjIiPT0iIiBnb3RvIDp1c2FnZQ0KDQpzZXQgIklNQUdFPSV+MSINCnNldCAiU1JDRElSPSV+MiINCg0KUkVNIFNjcmlwdCBpcyBleHBlY3RlZCB0byBiZSBpbiB0aGUgc2FtZSBkaXJlY3RvcnkgYXMgdGhpcyBCQVQNCnNldCAiU0NSSVBUPSV+ZHAwTmV3LU9zZkV4ZmF0SW1hZ2UucHMxIg0KDQppZiBub3QgZXhpc3QgIiVTQ1JJUFQlIiAoDQogIGVjaG8gW0VSUk9SXSBQb3dlclNoZWxsIHNjcmlwdCBub3QgZm91bmQ6ICIlU0NSSVBUJSINCiAgZWNobyBQdXQgTmV3LU9zZkV4ZmF0SW1hZ2UucHMxIG5leHQgdG8gdGhpcyAuYmF0IGZpbGUuDQogIGV4aXQgL2IgMg0KKQ0KDQppZiBub3QgZXhpc3QgIiVTUkNESVIlIiAoDQogIGVjaG8gW0VSUk9SXSBTb3VyY2UgZGlyZWN0b3J5IG5vdCBmb3VuZDogIiVTUkNESVIlIg0KICBleGl0IC9iIDMNCikNCg0KaWYgbm90IGV4aXN0ICIlU1JDRElSJVxlYm9vdC5iaW4iICgNCiAgZWNobyBbRVJST1JdIGVib290LmJpbiBub3QgZm91bmQgaW4gc291cmNlIGRpcmVjdG9yeTogIiVTUkNESVIlIg0KICBleGl0IC9iIDQNCikNCg0KUkVNIFJ1biBlbGV2YXRlZD8gVGhpcyBCQVQgZG9lcyBub3QgYXV0by1lbGV2YXRlLg0KUkVNIFJpZ2h0LWNsaWNrIC0+IFJ1biBhcyBhZG1pbmlzdHJhdG9yLCBvciBzdGFydCBjbWQgYXMgYWRtaW4uDQoNCnBvd2Vyc2hlbGwuZXhlIC1Ob1Byb2ZpbGUgLUV4ZWN1dGlvblBvbGljeSBCeXBhc3MgLUZpbGUgIiVTQ1JJUFQlIiAtSW1hZ2VQYXRoICIlSU1BR0UlIiAtU291cmNlRGlyICIlU1JDRElSJSIgLUZvcmNlT3ZlcndyaXRlDQoNCnNldCAiUkM9JUVSUk9STEVWRUwlIg0KaWYgbm90ICIlUkMlIj09IjAiICgNCiAgZWNobyBbRVJST1JdIEZhaWxlZCB3aXRoIGV4aXQgY29kZSAlUkMlLg0KICBleGl0IC9iICVSQyUNCikNCg0KZWNobyBbT0tdIERvbmU6ICIlSU1BR0UlIg0KZXhpdCAvYiAwDQoNCjp1c2FnZQ0KZWNobyBVc2FnZToNCmVjaG8gICAlfm54MCAiQzpccGF0aFx0b1xpbWFnZS5pbWciICJDOlxwYXRoXHRvXGZvbGRlciINCmVjaG8uDQplY2hvIE5vdGVzOg0KZWNobyAgIC0gUnVuIHRoaXMgQkFUIGFzIEFkbWluaXN0cmF0b3IuDQplY2hvICAgLSBJbWFnZSB3aWxsIGJlIGF1dG8tc2l6ZWQuDQpleGl0IC9iIDENCg=="
_PS1_B64 = "PCMgIE5ldy1Pc2ZFeGZhdEltYWdlLnBzMQoKICAgIFBVUlBPU0UKICAgIC0tLS0tLS0KICAgIENyZWF0ZSBhIFJBVyBpbWFnZSBmaWxlLCBtb3VudCBpdCB2aWEgT1NGTW91bnQgYXMgYSBsb2dpY2FsIHZvbHVtZSwKICAgIGZvcm1hdCBpdCBhcyBleEZBVCwgYW5kIGVpdGhlcjoKICAgICAgLSBmb3JtYXQgKyBjb3B5ICsgZGlzbW91bnQgKGRlZmF1bHQpLCBvcgogICAgICAtIGNyZWF0ZSArIG1vdW50IG9ubHkgZm9yIG1hbnVhbCBzdGVwcy4KCiAgICBVU0FHRSAocnVuIFBvd2VyU2hlbGwgYXMgQWRtaW5pc3RyYXRvcikKICAgIC0tLS0tLS0tLS0tLS0tLS0tLS0tLS0tLS0tLS0tLS0tLS0tLS0tCiAgICAxKSBBdXRvLXNpemUgKHJlY29tbWVuZGVkKToKICAgICAgIHBvd2Vyc2hlbGwuZXhlIC1FeGVjdXRpb25Qb2xpY3kgQnlwYXNzIC1GaWxlIC5cTmV3LU9zZkV4ZmF0SW1hZ2UucHMxIGAKICAgICAgICAgLUltYWdlUGF0aCAiQzpcaW1hZ2VzXGRhdGEuaW1nIiBgCiAgICAgICAgIC1Tb3VyY2VEaXIgIkM6XHBheWxvYWQiIGAKICAgICAgICAgLUxhYmVsICJEQVRBIiBgCiAgICAgICAgIC1Gb3JjZU92ZXJ3cml0ZQoKICAgIDIpIEZpeGVkIHNpemU6CiAgICAgICBwb3dlcnNoZWxsLmV4ZSAtRXhlY3V0aW9uUG9saWN5IEJ5cGFzcyAtRmlsZSAuXE5ldy1Pc2ZFeGZhdEltYWdlLnBzMSBgCiAgICAgICAgIC1JbWFnZVBhdGggIkM6XGltYWdlc1xkYXRhLmltZyIgYAogICAgICAgICAtU291cmNlRGlyICJDOlxwYXlsb2FkIiBgCiAgICAgICAgIC1TaXplIDhHIGAKICAgICAgICAgLUxhYmVsICJEQVRBIiBgCiAgICAgICAgIC1Gb3JjZU92ZXJ3cml0ZQoKICAgIDMpIENyZWF0ZSBlbXB0eSBpbWFnZSBhbmQga2VlcCBtb3VudGVkIChtYW51YWwgZm9ybWF0L2NvcHkpOgogICAgICAgcG93ZXJzaGVsbC5leGUgLUV4ZWN1dGlvblBvbGljeSBCeXBhc3MgLUZpbGUgLlxOZXctT3NmRXhmYXRJbWFnZS5wczEgYAogICAgICAgICAtSW1hZ2VQYXRoICJDOlxpbWFnZXNcZGF0YS5leGZhdCIgYAogICAgICAgICAtU291cmNlRGlyICJDOlxwYXlsb2FkXEFQUFhYWFgiIGAKICAgICAgICAgLUNyZWF0ZUVtcHR5QW5kTW91bnQgYAogICAgICAgICAtRm9yY2VPdmVyd3JpdGUKCiAgICBQQVJBTUVURVJTCiAgICAtLS0tLS0tLS0tCiAgICAtSW1hZ2VQYXRoICAgICAgIE91dHB1dCBpbWFnZSBmaWxlIHBhdGguCiAgICAtU291cmNlRGlyICAgICAgIEZvbGRlciB0byBjb3B5IGludG8gdGhlIG5ldyB2b2x1bWUuCiAgICAtU2l6ZSAgICAgICAgICAgIE9wdGlvbmFsLiBJZiBvbWl0dGVkLCBhbiBvcHRpbWFsIHNpemUgaXMgY29tcHV0ZWQgdG8gZml0IGFsbCBmaWxlcy4KICAgICAgICAgICAgICAgICAgICAgU3VmZml4ZXM6IEsvTS9HL1QgKDEwMjQpLCBrL20vZy90ICgxMDAwKSwgYiAoNTEyLWJ5dGUgYmxvY2tzKSwgb3IgYnl0ZXMuCiAgICAtTGFiZWwgICAgICAgICAgIFZvbHVtZSBsYWJlbC4KICAgIC1Gb3JjZU92ZXJ3cml0ZSAgUmVjcmVhdGUgaW1hZ2UgaWYgaXQgYWxyZWFkeSBleGlzdHMuCiAgICAtQ3JlYXRlRW1wdHlBbmRNb3VudAogICAgICAgICAgICAgICAgICAgICBDcmVhdGUgYW5kIG1vdW50IGltYWdlIG9ubHkuIFNraXAgZm9ybWF0L2NvcHkgYW5kIGxlYXZlIG1vdW50ZWQuCgogICAgTk9URVMKICAgIC0tLS0tCiAgICAtIFRoaXMgc2NyaXB0IGRvZXMgTk9UIGF1dG8tZWxldmF0ZS4gU3RhcnQgUG93ZXJTaGVsbCBhcyBBZG1pbmlzdHJhdG9yLgogICAgLSBGaWxlc3lzdGVtIGlzIGFsd2F5cyBleEZBVC4KICAgIC0gQ2x1c3RlciBzaXplIGlzIGF1dG8tc2VsZWN0ZWQ6CiAgICAgIC0gbGFyZ2UtZmlsZSBzZXRzOiA2NTUzNgogICAgICAtIHNtYWxsL21peGVkLWZpbGUgc2V0czogMzI3NjgKIz4KCltDbWRsZXRCaW5kaW5nKCldCnBhcmFtKAogIFtQYXJhbWV0ZXIoTWFuZGF0b3J5ID0gJHRydWUpXQogIFtzdHJpbmddJEltYWdlUGF0aCwKCiAgW1BhcmFtZXRlcihNYW5kYXRvcnkgPSAkdHJ1ZSldCiAgW3N0cmluZ10kU291cmNlRGlyLAoKICBbUGFyYW1ldGVyKE1hbmRhdG9yeSA9ICRmYWxzZSldCiAgW3N0cmluZ10kU2l6ZSwKCiAgW3N0cmluZ10kTGFiZWwgPSAiT1NGSU1HIiwKCiAgW3N3aXRjaF0kRm9yY2VPdmVyd3JpdGUsCgogIFtzd2l0Y2hdJENyZWF0ZUVtcHR5QW5kTW91bnQKKQoKU2V0LVN0cmljdE1vZGUgLVZlcnNpb24gTGF0ZXN0CiRFcnJvckFjdGlvblByZWZlcmVuY2UgPSAiU3RvcCIKJHNjcmlwdEN1bHR1cmUgPSBbU3lzdGVtLkdsb2JhbGl6YXRpb24uQ3VsdHVyZUluZm9dOjpHZXRDdWx0dXJlSW5mbygiZW4tVVMiKQpbU3lzdGVtLlRocmVhZGluZy5UaHJlYWRdOjpDdXJyZW50VGhyZWFkLkN1cnJlbnRDdWx0dXJlID0gJHNjcmlwdEN1bHR1cmUKW1N5c3RlbS5UaHJlYWRpbmcuVGhyZWFkXTo6Q3VycmVudFRocmVhZC5DdXJyZW50VUlDdWx0dXJlID0gJHNjcmlwdEN1bHR1cmUKW1N5c3RlbS5HbG9iYWxpemF0aW9uLkN1bHR1cmVJbmZvXTo6RGVmYXVsdFRocmVhZEN1cnJlbnRDdWx0dXJlID0gJHNjcmlwdEN1bHR1cmUKW1N5c3RlbS5HbG9iYWxpemF0aW9uLkN1bHR1cmVJbmZvXTo6RGVmYXVsdFRocmVhZEN1cnJlbnRVSUN1bHR1cmUgPSAkc2NyaXB0Q3VsdHVyZQoKZnVuY3Rpb24gVGVzdC1BZG1pbiB7CiAgJGlkID0gW1NlY3VyaXR5LlByaW5jaXBhbC5XaW5kb3dzSWRlbnRpdHldOjpHZXRDdXJyZW50KCkKICAkcCAgPSBOZXctT2JqZWN0IFNlY3VyaXR5LlByaW5jaXBhbC5XaW5kb3dzUHJpbmNpcGFsKCRpZCkKICByZXR1cm4gJHAuSXNJblJvbGUoW1NlY3VyaXR5LlByaW5jaXBhbC5XaW5kb3dzQnVpbHRJblJvbGVdOjpBZG1pbmlzdHJhdG9yKQp9CgpmdW5jdGlvbiBGaW5kLU9TRk1vdW50Q29tIHsKICAkY21kID0gR2V0LUNvbW1hbmQgIm9zZm1vdW50LmNvbSIgLUVycm9yQWN0aW9uIFNpbGVudGx5Q29udGludWUKICBpZiAoJGNtZCkgeyByZXR1cm4gJGNtZC5Tb3VyY2UgfQoKICAkY2FuZGlkYXRlcyA9IEAoCiAgICAiJHtlbnY6UHJvZ3JhbUZpbGVzfVxPU0ZNb3VudFxvc2Ztb3VudC5jb20iLAogICAgIiR7ZW52OlByb2dyYW1GaWxlcyh4ODYpfVxPU0ZNb3VudFxvc2Ztb3VudC5jb20iLAogICAgIiR7ZW52OlByb2dyYW1GaWxlc31cUGFzc01hcmtcT1NGTW91bnRcb3NmbW91bnQuY29tIiwKICAgICIke2VudjpQcm9ncmFtRmlsZXMoeDg2KX1cUGFzc01hcmtcT1NGTW91bnRcb3NmbW91bnQuY29tIgogICkgfCBXaGVyZS1PYmplY3QgeyAkXyAtYW5kIChUZXN0LVBhdGggJF8pIH0KCiAgJGNhbmRpZGF0ZXMgPSBAKCRjYW5kaWRhdGVzKQogIGlmICgkY2FuZGlkYXRlcy5Db3VudCAtZ3QgMCkgeyByZXR1cm4gJGNhbmRpZGF0ZXNbMF0gfQoKICB0aHJvdyAib3NmbW91bnQuY29tIG5vdCBmb3VuZC4gQWRkIE9TRk1vdW50IHRvIFBBVEggb3IgaW5zdGFsbCBpdCB0byBhIHN0YW5kYXJkIGxvY2F0aW9uLiIKfQoKZnVuY3Rpb24gUGFyc2UtU2l6ZVRvQnl0ZXMoW3N0cmluZ10kcykgewogICRzID0gJHMuVHJpbSgpCiAgaWYgKCRzIC1tYXRjaCAnXlxzKihcZCspXHMqKFtiQmtLbU1nR3RUXT8pXHMqJCcpIHsKICAgICRudW0gPSBbSW50NjRdJG1hdGNoZXNbMV0KICAgICR1ICAgPSAkbWF0Y2hlc1syXQogICAgc3dpdGNoICgkdSkgewogICAgICAnJyAgeyByZXR1cm4gJG51bSB9CiAgICAgICdiJyB7IHJldHVybiAkbnVtICogNTEyIH0KICAgICAgJ0InIHsgcmV0dXJuICRudW0gKiA1MTIgfQogICAgICAnSycgeyByZXR1cm4gJG51bSAqIDEwMjQgfQogICAgICAnTScgeyByZXR1cm4gJG51bSAqIDEwMjQgKiAxMDI0IH0KICAgICAgJ0cnIHsgcmV0dXJuICRudW0gKiAxMDI0ICogMTAyNCAqIDEwMjQgfQogICAgICAnVCcgeyByZXR1cm4gJG51bSAqIDEwMjQgKiAxMDI0ICogMTAyNCAqIDEwMjQgfQogICAgICAnaycgeyByZXR1cm4gJG51bSAqIDEwMDAgfQogICAgICAnbScgeyByZXR1cm4gJG51bSAqIDEwMDAgKiAxMDAwIH0KICAgICAgJ2cnIHsgcmV0dXJuICRudW0gKiAxMDAwICogMTAwMCAqIDEwMDAgfQogICAgICAndCcgeyByZXR1cm4gJG51bSAqIDEwMDAgKiAxMDAwICogMTAwMCAqIDEwMDAgfQogICAgICBkZWZhdWx0IHsgdGhyb3cgIlVua25vd24gc2l6ZSBzdWZmaXg6ICckdSciIH0KICAgIH0KICB9CiAgdGhyb3cgIkZhaWxlZCB0byBwYXJzZSBzaXplIHN0cmluZzogJyRzJyIKfQoKZnVuY3Rpb24gRm9ybWF0LUJ5dGVzKFtJbnQ2NF0kYnl0ZXMpIHsKICBpZiAoJGJ5dGVzIC1nZSAxVEIpIHsgcmV0dXJuICJ7MDpOMn0gVEIiIC1mICgkYnl0ZXMvMVRCKSB9CiAgaWYgKCRieXRlcyAtZ2UgMUdCKSB7IHJldHVybiAiezA6TjJ9IEdCIiAtZiAoJGJ5dGVzLzFHQikgfQogIGlmICgkYnl0ZXMgLWdlIDFNQikgeyByZXR1cm4gInswOk4yfSBNQiIgLWYgKCRieXRlcy8xTUIpIH0KICBpZiAoJGJ5dGVzIC1nZSAxS0IpIHsgcmV0dXJuICJ7MDpOMn0gS0IiIC1mICgkYnl0ZXMvMUtCKSB9CiAgcmV0dXJuICIkYnl0ZXMgQiIKfQoKZnVuY3Rpb24gR2V0LUZyZWVEcml2ZUxldHRlciB7CiAgJHVzZWQgPSAoR2V0LVBTRHJpdmUgLVBTUHJvdmlkZXIgRmlsZVN5c3RlbSkuTmFtZQogIGZvcmVhY2ggKCRjb2RlIGluIDY4Li45MCkgewogICAgJGxldHRlciA9IFtjaGFyXSRjb2RlCiAgICBpZiAoJHVzZWQgLW5vdGNvbnRhaW5zIFtzdHJpbmddJGxldHRlcikgeyByZXR1cm4gW3N0cmluZ10kbGV0dGVyIH0KICB9CiAgdGhyb3cgIk5vIGZyZWUgZHJpdmUgbGV0dGVycyBhdmFpbGFibGUgKEQ6Li5aOikuIgp9CgpmdW5jdGlvbiBHZXQtT3B0aW1hbEltYWdlU2l6ZUJ5dGVzKFtzdHJpbmddJGRpciwgW2ludF0kY2x1c3RlckJ5dGVzKSB7CiAgJGNsdXN0ZXIgPSBbSW50NjRdJGNsdXN0ZXJCeXRlcwogIFtJbnQ2NF0kbWV0YUZpeGVkID0gMzJNQgogIFtJbnQ2NF0kbWluU2xhY2sgPSA2NE1CCiAgW0ludDY0XSRzcGFyZU1pbiA9IDY0TUIKICBbSW50NjRdJHNwYXJlTWF4ID0gNTEyTUIKICBbSW50NjRdJGVudHJ5TWV0YUJ5dGVzID0gMjU2CgogICRmaWxlcyA9IEAoR2V0LUNoaWxkSXRlbSAtTGl0ZXJhbFBhdGggJGRpciAtUmVjdXJzZSAtRmlsZSAtRm9yY2UpCiAgJGRpcnMgPSBAKEdldC1DaGlsZEl0ZW0gLUxpdGVyYWxQYXRoICRkaXIgLVJlY3Vyc2UgLURpcmVjdG9yeSAtRm9yY2UpCgogIFtJbnQ2NF0kcmF3RmlsZUJ5dGVzID0gMAogIFtJbnQ2NF0kZGF0YUJ5dGVzID0gMAogIGZvcmVhY2ggKCRmIGluICRmaWxlcykgewogICAgJGxlbiA9IFtJbnQ2NF0kZi5MZW5ndGgKICAgICRyYXdGaWxlQnl0ZXMgKz0gJGxlbgogICAgJGRhdGFCeXRlcyArPSBbSW50NjRdKFtNYXRoXTo6Q2VpbGluZygkbGVuIC8gW2RvdWJsZV0kY2x1c3RlcikgKiAkY2x1c3RlcikKICB9CgogIFtJbnQ2NF0kZGF0YUNsdXN0ZXJzID0gW0ludDY0XShbTWF0aF06OkNlaWxpbmcoJGRhdGFCeXRlcyAvIFtkb3VibGVdJGNsdXN0ZXIpKQogIFtJbnQ2NF0kZmF0Qnl0ZXMgPSAkZGF0YUNsdXN0ZXJzICogNAogIFtJbnQ2NF0kYml0bWFwQnl0ZXMgPSBbSW50NjRdKFtNYXRoXTo6Q2VpbGluZygkZGF0YUNsdXN0ZXJzIC8gOC4wKSkKICBbSW50NjRdJGVudHJ5Qnl0ZXMgPQogICAgICAoKFtJbnQ2NF0kZmlsZXMuQ291bnQgKyBbSW50NjRdJGRpcnMuQ291bnQpICogJGVudHJ5TWV0YUJ5dGVzKQoKICBbSW50NjRdJGJhc2VUb3RhbCA9CiAgICAgICRkYXRhQnl0ZXMgKyAkZmF0Qnl0ZXMgKyAkYml0bWFwQnl0ZXMgKyAkZW50cnlCeXRlcyArICRtZXRhRml4ZWQKICBbSW50NjRdJHNwYXJlQnl0ZXMgPSBbSW50NjRdKFtNYXRoXTo6Q2VpbGluZygkYmFzZVRvdGFsIC8gMjAwLjApKQogIGlmICgkc3BhcmVCeXRlcyAtbHQgJHNwYXJlTWluKSB7ICRzcGFyZUJ5dGVzID0gJHNwYXJlTWluIH0KICBpZiAoJHNwYXJlQnl0ZXMgLWd0ICRzcGFyZU1heCkgeyAkc3BhcmVCeXRlcyA9ICRzcGFyZU1heCB9CiAgW0ludDY0XSR0b3RhbCA9ICRiYXNlVG90YWwgKyAkc3BhcmVCeXRlcwogIFtJbnQ2NF0kbWluVG90YWwgPSAkcmF3RmlsZUJ5dGVzICsgJG1pblNsYWNrCiAgaWYgKCR0b3RhbCAtbHQgJG1pblRvdGFsKSB7ICR0b3RhbCA9ICRtaW5Ub3RhbCB9CgogIFtJbnQ2NF0kYWxpZ24gPSAxTUIKICAkdG90YWwgPSBbSW50NjRdKFtNYXRoXTo6Q2VpbGluZygkdG90YWwgLyBbZG91YmxlXSRhbGlnbikgKiAkYWxpZ24pCiAgcmV0dXJuICR0b3RhbAp9CgpmdW5jdGlvbiBXYWl0LUZvckxvZ2ljYWxEcml2ZShbc3RyaW5nXSRkcml2ZUxldHRlciwgW2ludF0kdGltZW91dFNlY29uZHMgPSAyMCkgewogICR0YXJnZXQgPSAiJHtkcml2ZUxldHRlcn06IgogICRzdyA9IFtEaWFnbm9zdGljcy5TdG9wd2F0Y2hdOjpTdGFydE5ldygpCiAgd2hpbGUgKCRzdy5FbGFwc2VkLlRvdGFsU2Vjb25kcyAtbHQgJHRpbWVvdXRTZWNvbmRzKSB7CiAgICAkbG9naWNhbCA9IEdldC1DaW1JbnN0YW5jZSAtQ2xhc3NOYW1lIFdpbjMyX0xvZ2ljYWxEaXNrIC1GaWx0ZXIgIkRldmljZUlEPSckdGFyZ2V0JyIgLUVycm9yQWN0aW9uIFNpbGVudGx5Q29udGludWUKICAgIGlmICgkbG9naWNhbCkgeyByZXR1cm4gJHRydWUgfQogICAgU3RhcnQtU2xlZXAgLU1pbGxpc2Vjb25kcyAzMDAKICB9CiAgcmV0dXJuICRmYWxzZQp9CgpmdW5jdGlvbiBHZXQtTG9naWNhbERyaXZlRmlsZVN5c3RlbShbc3RyaW5nXSRkcml2ZUxldHRlcikgewogICR0YXJnZXQgPSAiJHtkcml2ZUxldHRlcn06IgogICRsb2dpY2FsID0gR2V0LUNpbUluc3RhbmNlIC1DbGFzc05hbWUgV2luMzJfTG9naWNhbERpc2sgLUZpbHRlciAiRGV2aWNlSUQ9JyR0YXJnZXQnIiAtRXJyb3JBY3Rpb24gU2lsZW50bHlDb250aW51ZQogIGlmICgkbG9naWNhbCkgeyByZXR1cm4gW3N0cmluZ10kbG9naWNhbC5GaWxlU3lzdGVtIH0KICByZXR1cm4gIiIKfQoKZnVuY3Rpb24gR2V0LU9wdGltYWxFeGZhdENsdXN0ZXJTaXplKFtzdHJpbmddJGRpcikgewogIFtJbnQ2NF0kbGFyZ2VGaWxlVGhyZXNob2xkID0gMU1CCgogICRmaWxlcyA9IEAoR2V0LUNoaWxkSXRlbSAtTGl0ZXJhbFBhdGggJGRpciAtUmVjdXJzZSAtRmlsZSAtRm9yY2UpCiAgaWYgKCRmaWxlcy5Db3VudCAtZXEgMCkgeyByZXR1cm4gMzI3NjggfQoKICBbSW50NjRdJHJhd0ZpbGVCeXRlcyA9IDAKICBmb3JlYWNoICgkZiBpbiAkZmlsZXMpIHsKICAgICRyYXdGaWxlQnl0ZXMgKz0gW0ludDY0XSRmLkxlbmd0aAogIH0KCiAgW0ludDY0XSRhdmdGaWxlQnl0ZXMgPSBbSW50NjRdKCRyYXdGaWxlQnl0ZXMgLyBbSW50NjRdJGZpbGVzLkNvdW50KQogIGlmICgkYXZnRmlsZUJ5dGVzIC1nZSAkbGFyZ2VGaWxlVGhyZXNob2xkKSB7IHJldHVybiA2NTUzNiB9CiAgcmV0dXJuIDMyNzY4Cn0KCmZ1bmN0aW9uIEZvcm1hdC1BbGxvY2F0aW9uVW5pdEFyZyhbaW50XSRjbHVzdGVyU2l6ZSkgewogIGlmICgkY2x1c3RlclNpemUgLWdlIDFNQiAtYW5kICgkY2x1c3RlclNpemUgJSAxTUIpIC1lcSAwKSB7CiAgICByZXR1cm4gInswfU0iIC1mICgkY2x1c3RlclNpemUgLyAxTUIpCiAgfQogIGlmICgkY2x1c3RlclNpemUgLWdlIDFLQiAtYW5kICgkY2x1c3RlclNpemUgJSAxS0IpIC1lcSAwKSB7CiAgICByZXR1cm4gInswfUsiIC1mICgkY2x1c3RlclNpemUgLyAxS0IpCiAgfQogIHJldHVybiAiJGNsdXN0ZXJTaXplIgp9CgpmdW5jdGlvbiBEaXNtb3VudC1Pc2ZWb2x1bWUoW3N0cmluZ10kb3NmUGF0aCwgW3N0cmluZ10kbW91bnRQb2ludCwgW2ludF0kbWF4QXR0ZW1wdHMgPSA2KSB7CiAgaWYgKFtzdHJpbmddOjpJc051bGxPcldoaXRlU3BhY2UoJG1vdW50UG9pbnQpKSB7IHJldHVybiAkZmFsc2UgfQoKICAkdGFyZ2V0cyA9IEAoJG1vdW50UG9pbnQpCiAgaWYgKC1ub3QgJG1vdW50UG9pbnQuRW5kc1dpdGgoIlwiKSkgeyAkdGFyZ2V0cyArPSAiJG1vdW50UG9pbnRcIiB9CgogIGZvciAoJGkgPSAxOyAkaSAtbGUgJG1heEF0dGVtcHRzOyAkaSsrKSB7CiAgICBmb3JlYWNoICgkdGFyZ2V0IGluICR0YXJnZXRzKSB7CiAgICAgIHRyeSB7CiAgICAgICAgJiAkb3NmUGF0aCAtZCAtbSAkdGFyZ2V0IDI+JjEgfCBPdXQtTnVsbAogICAgICAgIGlmICgkTEFTVEVYSVRDT0RFIC1lcSAwKSB7IHJldHVybiAkdHJ1ZSB9CiAgICAgIH0gY2F0Y2ggewogICAgICAgICMgUmV0cnk6IHZvbHVtZSBjYW4gcmVtYWluIGJ1c3kgZm9yIGEgc2hvcnQgdGltZSBhZnRlciBmb3JtYXQvY29weS4KICAgICAgfQogICAgfQogICAgU3RhcnQtU2xlZXAgLU1pbGxpc2Vjb25kcyA1MDAKICB9CgogIHJldHVybiAkZmFsc2UKfQoKZnVuY3Rpb24gSW52b2tlLUZvcm1hdFZvbHVtZShbc3RyaW5nXSRkcml2ZUxldHRlciwgW2ludF0kY2x1c3RlclNpemUsIFtzdHJpbmddJGxhYmVsKSB7CiAgJHRhcmdldCA9ICIke2RyaXZlTGV0dGVyfToiCiAgJGZpbGVTeXN0ZW0gPSAiZXhGQVQiCiAgJGNsdXN0ZXJBcmcgPSBGb3JtYXQtQWxsb2NhdGlvblVuaXRBcmcgLWNsdXN0ZXJTaXplICRjbHVzdGVyU2l6ZQogICRhdHRlbXB0cyA9IEAoCiAgICBAeyBOYW1lID0gIiRmaWxlU3lzdGVtIHF1aWNrIHdpdGggcmVxdWVzdGVkIGFsbG9jYXRpb24gdW5pdCI7IEFyZ3MgPSBAKCR0YXJnZXQsICIvRlM6JGZpbGVTeXN0ZW0iLCAiL0E6JGNsdXN0ZXJBcmciLCAiL1EiLCAiL1Y6JGxhYmVsIiwgIi9YIiwgIi9ZIikgfQogICkKCiAgJGxhc3RGb3JtYXRFeGl0Q29kZSA9IC0xCiAgZm9yZWFjaCAoJGF0dGVtcHQgaW4gJGF0dGVtcHRzKSB7CiAgICBXcml0ZS1Ib3N0ICJbSW5mb10gZm9ybWF0IGF0dGVtcHQ6ICQoJGF0dGVtcHQuTmFtZSkiCiAgICAkc3Rkb3V0UGF0aCA9IFtTeXN0ZW0uSU8uUGF0aF06OkdldFRlbXBGaWxlTmFtZSgpCiAgICAkc3RkZXJyUGF0aCA9IFtTeXN0ZW0uSU8uUGF0aF06OkdldFRlbXBGaWxlTmFtZSgpCiAgICB0cnkgewogICAgICAkcHJvYyA9IFN0YXJ0LVByb2Nlc3MgLUZpbGVQYXRoICJmb3JtYXQuY29tIiAtQXJndW1lbnRMaXN0ICRhdHRlbXB0LkFyZ3MgLVdhaXQgLVBhc3NUaHJ1IC1Ob05ld1dpbmRvdyBgCiAgICAgICAgLVJlZGlyZWN0U3RhbmRhcmRPdXRwdXQgJHN0ZG91dFBhdGggLVJlZGlyZWN0U3RhbmRhcmRFcnJvciAkc3RkZXJyUGF0aAogICAgICAkbGFzdEZvcm1hdEV4aXRDb2RlID0gW2ludF0kcHJvYy5FeGl0Q29kZQoKICAgICAgIyBTaG93IG5hdGl2ZSBmb3JtYXQuY29tIG91dHB1dCBpbiBjdXJyZW50IGNvbnNvbGUuCiAgICAgIGlmIChUZXN0LVBhdGggLUxpdGVyYWxQYXRoICRzdGRvdXRQYXRoKSB7CiAgICAgICAgR2V0LUNvbnRlbnQgLUxpdGVyYWxQYXRoICRzdGRvdXRQYXRoIC1FcnJvckFjdGlvbiBTaWxlbnRseUNvbnRpbnVlIHwgRm9yRWFjaC1PYmplY3QgeyBXcml0ZS1Ib3N0ICRfIH0KICAgICAgfQogICAgICBpZiAoVGVzdC1QYXRoIC1MaXRlcmFsUGF0aCAkc3RkZXJyUGF0aCkgewogICAgICAgIEdldC1Db250ZW50IC1MaXRlcmFsUGF0aCAkc3RkZXJyUGF0aCAtRXJyb3JBY3Rpb24gU2lsZW50bHlDb250aW51ZSB8IEZvckVhY2gtT2JqZWN0IHsgV3JpdGUtSG9zdCAkXyB9CiAgICAgIH0KICAgIH0gZmluYWxseSB7CiAgICAgIFJlbW92ZS1JdGVtIC1MaXRlcmFsUGF0aCAkc3Rkb3V0UGF0aCwgJHN0ZGVyclBhdGggLUZvcmNlIC1FcnJvckFjdGlvbiBTaWxlbnRseUNvbnRpbnVlCiAgICB9CgogICAgIyBTb21lIGVudmlyb25tZW50cyBtYXkgcmVwb3J0IGEgbm9uLXplcm8gZXhpdCBjb2RlIGV2ZW4gd2hlbiBmb3JtYXR0aW5nIGNvbXBsZXRlZC4KICAgICMgVmFsaWRhdGUgcmVzdWx0aW5nIGZpbGVzeXN0ZW0gYXMgdGhlIHNvdXJjZSBvZiB0cnV0aC4KICAgIGlmIChXYWl0LUZvckxvZ2ljYWxEcml2ZSAtZHJpdmVMZXR0ZXIgJGRyaXZlTGV0dGVyIC10aW1lb3V0U2Vjb25kcyA1KSB7CiAgICAgICRhY3R1YWxGcyA9IEdldC1Mb2dpY2FsRHJpdmVGaWxlU3lzdGVtIC1kcml2ZUxldHRlciAkZHJpdmVMZXR0ZXIKICAgICAgaWYgKCRhY3R1YWxGcyAtYW5kICRhY3R1YWxGcy5Ub1VwcGVySW52YXJpYW50KCkgLWVxICRmaWxlU3lzdGVtLlRvVXBwZXJJbnZhcmlhbnQoKSkgewogICAgICAgIFdyaXRlLUhvc3QgIltJbmZvXSBmb3JtYXQgcmVzdWx0OiBkZXRlY3RlZCBmaWxlc3lzdGVtICckYWN0dWFsRnMnIG9uICR0YXJnZXQuIgogICAgICAgIHJldHVybgogICAgICB9CiAgICB9CgogICAgV3JpdGUtSG9zdCAiW0luZm9dIGZvcm1hdCBhdHRlbXB0IGZhaWxlZCAoZXhpdCBjb2RlICRsYXN0Rm9ybWF0RXhpdENvZGUpLCByZXRyeWluZy4uLiIKICB9CgogIHRocm93ICJmb3JtYXQuY29tIGZhaWxlZCBmb3IgJHRhcmdldCBhZnRlciBhbGwgcmV0cnkgc3RyYXRlZ2llcy4gTGFzdCBleGl0IGNvZGU6ICRsYXN0Rm9ybWF0RXhpdENvZGUiCn0KCiMgLS0tLS0tLS0tLS0tLS0tLS0tLS0gTWFpbiAtLS0tLS0tLS0tLS0tLS0tLS0tLQoKaWYgKC1ub3QgKFRlc3QtQWRtaW4pKSB7IHRocm93ICJQbGVhc2UgcnVuIFBvd2VyU2hlbGwgYXMgQWRtaW5pc3RyYXRvci4iIH0KaWYgKC1ub3QgKFRlc3QtUGF0aCAtTGl0ZXJhbFBhdGggJFNvdXJjZURpciAtUGF0aFR5cGUgQ29udGFpbmVyKSkgeyB0aHJvdyAiU291cmNlIGRpcmVjdG9yeSBub3QgZm91bmQ6ICRTb3VyY2VEaXIiIH0KaWYgKC1ub3QgKFRlc3QtUGF0aCAtTGl0ZXJhbFBhdGggKEpvaW4tUGF0aCAkU291cmNlRGlyICJlYm9vdC5iaW4iKSAtUGF0aFR5cGUgTGVhZikpIHsgdGhyb3cgImVib290LmJpbiBub3QgZm91bmQgaW4gc291cmNlIGRpcmVjdG9yeTogJFNvdXJjZURpciIgfQoKIyBFbnN1cmUgb3V0cHV0IGRpcmVjdG9yeSBleGlzdHMKJG91dERpciA9IFNwbGl0LVBhdGggLVBhcmVudCAkSW1hZ2VQYXRoCmlmICgkb3V0RGlyIC1hbmQgLW5vdCAoVGVzdC1QYXRoIC1MaXRlcmFsUGF0aCAkb3V0RGlyKSkgewogIE5ldy1JdGVtIC1JdGVtVHlwZSBEaXJlY3RvcnkgLVBhdGggJG91dERpciB8IE91dC1OdWxsCn0KCmlmIChUZXN0LVBhdGggLUxpdGVyYWxQYXRoICRJbWFnZVBhdGgpIHsKICBpZiAoLW5vdCAkRm9yY2VPdmVyd3JpdGUpIHsgdGhyb3cgIkltYWdlIGZpbGUgYWxyZWFkeSBleGlzdHM6ICRJbWFnZVBhdGguIFVzZSAtRm9yY2VPdmVyd3JpdGUgdG8gcmVwbGFjZSBpdC4iIH0KICBSZW1vdmUtSXRlbSAtTGl0ZXJhbFBhdGggJEltYWdlUGF0aCAtRm9yY2UKfQoKW0ludDY0XSRleHBlY3RlZEJ5dGVzID0gMApbc3RyaW5nXSRvc2ZTaXplQXJnID0gJG51bGwKW2ludF0kRXhmYXRDbHVzdGVyU2l6ZSA9IEdldC1PcHRpbWFsRXhmYXRDbHVzdGVyU2l6ZSAtZGlyICRTb3VyY2VEaXIKW2Jvb2xdJHNpemVQcm92aWRlZCA9IC1ub3QgW3N0cmluZ106OklzTnVsbE9yV2hpdGVTcGFjZSgkU2l6ZSkKW2ludF0kVGFyZ2V0Q2x1c3RlclNpemUgPSAkRXhmYXRDbHVzdGVyU2l6ZQoKaWYgKC1ub3QgJHNpemVQcm92aWRlZCkgewogIFdyaXRlLUhvc3QgIltJbmZvXSBTaXplIG5vdCBwcm92aWRlZC4gQ29tcHV0aW5nIGFuIG9wdGltYWwgaW1hZ2Ugc2l6ZSBmcm9tICckU291cmNlRGlyJy4uLiIKICAkZXhwZWN0ZWRCeXRlcyA9IEdldC1PcHRpbWFsSW1hZ2VTaXplQnl0ZXMgLWRpciAkU291cmNlRGlyIC1jbHVzdGVyQnl0ZXMgJFRhcmdldENsdXN0ZXJTaXplCiAgJG9zZlNpemVBcmcgPSAiJGV4cGVjdGVkQnl0ZXMiCn0gZWxzZSB7CiAgJGV4cGVjdGVkQnl0ZXMgPSBQYXJzZS1TaXplVG9CeXRlcyAkU2l6ZQogICRvc2ZTaXplQXJnID0gJFNpemUKfQoKaWYgKC1ub3QgJHNpemVQcm92aWRlZCkgewogIFdyaXRlLUhvc3QgIltJbmZvXSBDb21wdXRlZCBpbWFnZSBzaXplOiAkKEZvcm1hdC1CeXRlcyAkZXhwZWN0ZWRCeXRlcykgKCRleHBlY3RlZEJ5dGVzIGJ5dGVzKS4iCn0KV3JpdGUtSG9zdCAiW0luZm9dIFNlbGVjdGVkIGZpbGVzeXN0ZW06IGV4RkFUIChjbHVzdGVyPSRUYXJnZXRDbHVzdGVyU2l6ZSkgZm9yIGltYWdlIHNpemUgJChGb3JtYXQtQnl0ZXMgJGV4cGVjdGVkQnl0ZXMpLiIKCiRvc2YgPSBGaW5kLU9TRk1vdW50Q29tCgpbc3RyaW5nXSREcml2ZUxldHRlciA9ICIiCltzdHJpbmddJE1vdW50UG9pbnQgPSAiIgpbYm9vbF0kTW91bnRlZCA9ICRmYWxzZQpbYm9vbF0kTGVhdmVNb3VudGVkID0gJENyZWF0ZUVtcHR5QW5kTW91bnQuSXNQcmVzZW50Cgp0cnkgewogICREcml2ZUxldHRlciA9IEdldC1GcmVlRHJpdmVMZXR0ZXIKICAkTW91bnRQb2ludCA9ICIke0RyaXZlTGV0dGVyfToiCgogIGlmICgkQ3JlYXRlRW1wdHlBbmRNb3VudCkgewogICAgV3JpdGUtSG9zdCAiWzEvMl0gQ3JlYXRpbmcgJiBtb3VudGluZyB0aGUgaW1hZ2UgdmlhIE9TRk1vdW50IGFzIGEgbG9naWNhbCB2b2x1bWUgb24gJE1vdW50UG9pbnQgLi4uIgogIH0gZWxzZSB7CiAgICBXcml0ZS1Ib3N0ICJbMS80XSBDcmVhdGluZyAmIG1vdW50aW5nIHRoZSBpbWFnZSB2aWEgT1NGTW91bnQgYXMgYSBsb2dpY2FsIHZvbHVtZSBvbiAkTW91bnRQb2ludCAuLi4iCiAgfQogICRvdXQgPSAmICRvc2YgLWEgLXQgZmlsZSAtZiAkSW1hZ2VQYXRoIC1zICRvc2ZTaXplQXJnIC1tICRNb3VudFBvaW50IC1vIHJ3LHJlbSAyPiYxCiAgV3JpdGUtSG9zdCAoJG91dCB8IE91dC1TdHJpbmcpLlRyaW0oKQogIGlmICgkTEFTVEVYSVRDT0RFIC1uZSAwKSB7IHRocm93ICJvc2Ztb3VudC5jb20gZmFpbGVkIHdpdGggZXhpdCBjb2RlICRMQVNURVhJVENPREUuIiB9CiAgJE1vdW50ZWQgPSAkdHJ1ZQogIGlmICgtbm90IChXYWl0LUZvckxvZ2ljYWxEcml2ZSAtZHJpdmVMZXR0ZXIgJERyaXZlTGV0dGVyIC10aW1lb3V0U2Vjb25kcyAyMCkpIHsKICAgIHRocm93ICJNb3VudGVkIGRyaXZlICRNb3VudFBvaW50IGRpZCBub3QgYXBwZWFyIGluIHRpbWUuIgogIH0KCiAgJGRlc3QgPSAiJHtEcml2ZUxldHRlcn06XCIKICBpZiAoJENyZWF0ZUVtcHR5QW5kTW91bnQpIHsKICAgIFdyaXRlLUhvc3QgIlsyLzJdIERvbmUuIEVtcHR5IGltYWdlIGlzIG1vdW50ZWQgYXQgJGRlc3QuIgogICAgV3JpdGUtSG9zdCAiTWFudWFsIHN0ZXBzOiIKICAgIFdyaXRlLUhvc3QgIiAgMSkgRm9ybWF0ICRkZXN0IGFzIGV4RkFUIChyZWNvbW1lbmRlZCBjbHVzdGVyOiA2NEtCIGZvciBsYXJnZS1maWxlIHNldHMsIDMyS0IgZm9yIHNtYWxsL21peGVkIHNldHMpLiIKICAgIFdyaXRlLUhvc3QgIiAgMikgQ29weSBjb250ZW50cyBvZiAnJFNvdXJjZURpcicgdG8gJGRlc3QuIgogICAgV3JpdGUtSG9zdCAiICAzKSBEaXNtb3VudDogYCIkb3NmYCIgLWQgLW0gJE1vdW50UG9pbnQiCiAgICByZXR1cm4KICB9CgogIFdyaXRlLUhvc3QgIlsyLzRdIEZvcm1hdHRpbmcgJGRlc3QgYXMgZXhGQVQgKGNsdXN0ZXI9JFRhcmdldENsdXN0ZXJTaXplLCBsYWJlbD0nJExhYmVsJykgdmlhIGZvcm1hdC5jb20gLi4uIgogIEludm9rZS1Gb3JtYXRWb2x1bWUgLWRyaXZlTGV0dGVyICREcml2ZUxldHRlciAtY2x1c3RlclNpemUgJFRhcmdldENsdXN0ZXJTaXplIC1sYWJlbCAkTGFiZWwKCiAgaWYgKC1ub3QgKFRlc3QtUGF0aCAkZGVzdCkpIHsgdGhyb3cgIkRyaXZlICRkZXN0IGlzIG5vdCBhY2Nlc3NpYmxlIGFmdGVyIGZvcm1hdHRpbmcuIiB9CgogIFdyaXRlLUhvc3QgIlszLzRdIENvcHlpbmcgY29udGVudHMgb2YgJyRTb3VyY2VEaXInIC0+ICckZGVzdCcgLi4uIgogICRyb2JvQXJncyA9IEAoCiAgICAkU291cmNlRGlyLCAkZGVzdCwKICAgICIvRSIsICIvQ09QWTpEQVQiLCAiL0RDT1BZOkRBVCIsCiAgICAiL1I6MSIsICIvVzoxIiwKICAgICIvTkZMIiwgIi9OREwiLAogICAgIi9FVEEiCiAgKQogICYgcm9ib2NvcHkuZXhlIEByb2JvQXJncwogICRyb2JvY29weUV4aXRDb2RlID0gJExBU1RFWElUQ09ERQogIGlmICgkcm9ib2NvcHlFeGl0Q29kZSAtZ3QgNykgeyB0aHJvdyAicm9ib2NvcHkgZmFpbGVkLiBFeGl0IGNvZGU6ICRyb2JvY29weUV4aXRDb2RlIiB9CgogIFdyaXRlLUhvc3QgIls0LzRdIERvbmUuIERpc21vdW50aW5nIE9TRk1vdW50IHZvbHVtZS4uLiIKfQpmaW5hbGx5IHsKICBpZiAoJE1vdW50ZWQgLWFuZCAtbm90ICRMZWF2ZU1vdW50ZWQgLWFuZCAtbm90IFtzdHJpbmddOjpJc051bGxPcldoaXRlU3BhY2UoJE1vdW50UG9pbnQpKSB7CiAgICB0cnkgewogICAgICAkY3VycmVudFBhdGggPSAoR2V0LUxvY2F0aW9uKS5QYXRoCiAgICAgIGlmICgkY3VycmVudFBhdGggLWxpa2UgIiRNb3VudFBvaW50KiIpIHsKICAgICAgICBTZXQtTG9jYXRpb24gIiRlbnY6U3lzdGVtRHJpdmVcIgogICAgICB9CiAgICB9IGNhdGNoIHsKICAgICAgIyBCZXN0IGVmZm9ydCBvbmx5LgogICAgfQoKICAgIGlmICgtbm90IChEaXNtb3VudC1Pc2ZWb2x1bWUgLW9zZlBhdGggJG9zZiAtbW91bnRQb2ludCAkTW91bnRQb2ludCAtbWF4QXR0ZW1wdHMgNikpIHsKICAgICAgV3JpdGUtV2FybmluZyAiRmFpbGVkIHRvIGRpc21vdW50IE9TRk1vdW50IHZvbHVtZSAoJE1vdW50UG9pbnQpOiBhY2Nlc3MgZGVuaWVkIG9yIHZvbHVtZSBidXN5LiIKICAgIH0KICB9Cn0KCldyaXRlLUhvc3QgIk9LOiBJbWFnZSBjcmVhdGVkIGF0OiAkSW1hZ2VQYXRoIgo="
_ICO_B64 = "AAABAAEAEBAAAAAAIADbAgAAFgAAAIlQTkcNChoKAAAADUlIRFIAAAAQAAAAEAgGAAAAH/P/YQAAAqJJREFUeJxdk81vlGUUxX/3+Xjnw3eYYtsxfEgCkY9CpEI0Nm6NrDRxS/gLdMvWNWtWrFgYojsSdEG6gDQkRGNiaq2JBggtSiHpWEtL52WYd573ea6LGSrx7u7NueecxTlivnqmTE2gCCjghxA9mAgIRMvuCCMMICaCDRjqHk1pdDURMRVIREQRGxAUEcAAVRyRCGgyaOVx2s4hjv+NJdEEgZSA5HdVzVYPjMdUBSlvoLUMksUxdopA7PWheAp7D+IALwmsUv3zguAchC4p62B6ffAWxOJ43kNQUqvBl9OLzO1foesP8/7xfXgCiFAG4eeHXaaKB/yk73B1/QNMWaFNizMTOfHZgNnaFhc/2sNOOcuBQjixfy/ORESE4mWg7O3QOXaGs6HP3fU17vsZrCrOOCEm5bMDBUVRUATHL0tLzM+v05maQFXZ2Nymmed8eu4TqmHg4yMD7m8IguLSdh/CC85OBqbe2sfTe4+4e+cWa2tPWP3rMdZa3j11EhHl9MwJ5s6cYq7a4cr6kJRlmFir0Zisc7Btyeo5335zjdWHK2SZZ3KizZvtPRiBx4/+5Lsb1xFXY9qX1GxJUnB4Sz0lOpNtVh78we2VHpuffw1SYbUJSVgNQ8yHyss7l/j9tyXanbepS6BUcCj0K4Oxnh8W5unOnkdOvwebUL3KF1C9Ad2/L7D44wLnzn9Bv7LgwFmBkiaXbi6z/OsGcvQQLC5jYoUGt5sRMQ7yQ3y/vM296ScEOYwFRC6rAuigB1mJDCxUEXFDkISGbMRgBImQIuBrGGvRvInDAAls3kJpoW6c9Vfe07hABtgssA2HZI60tQPe46TXR/MmqRoDZVw3fa1+AkTQRgZJ0RDRVg7G4BiUI1irOVJD/qsu/9trGbp7H3H/CzkmGpxJY8iIAAAAAElFTkSuQmCC"
_TEMP_DIR = None

# ── Settings persistence ─────────────────────────────────────────────────────
def _settings_path():
    return os.path.join(os.path.expanduser('~'), '.exfat_builder_settings.json')

def load_settings():
    try:
        with open(_settings_path(), 'r') as f:
            return json.load(f)
    except Exception:
        return {}

def save_settings(data):
    try:
        with open(_settings_path(), 'w') as f:
            json.dump(data, f, indent=2)
    except Exception:
        pass

# ── SFO Parser ───────────────────────────────────────────────────────────────
def parse_sfo(path):
    try:
        with open(path, 'rb') as f:
            data = f.read()
        if len(data) < 20:
            return {}
        magic, version, key_table_offset, data_table_offset, num_entries = \
            struct.unpack_from('<IIIII', data, 0)
        if magic != 0x46535000:
            return {}
        results = {}
        for i in range(num_entries):
            entry_offset = 20 + i * 16
            if entry_offset + 16 > len(data):
                break
            key_off, data_fmt, data_len, data_max_len, data_off = \
                struct.unpack_from('<HHIII', data, entry_offset)
            key_start = key_table_offset + key_off
            key_end   = data.index(b'\x00', key_start)
            key = data[key_start:key_end].decode('utf-8', errors='replace')
            val_start = data_table_offset + data_off
            if data_fmt == 0x0204:
                val = data[val_start:val_start+data_len].rstrip(b'\x00').decode('utf-8', errors='replace')
            elif data_fmt == 0x0404:
                val = struct.unpack_from('<I', data, val_start)[0]
            else:
                val = data[val_start:val_start+data_len]
            results[key] = val
        return results
    except Exception:
        return {}

def find_meta_file(folder, names):
    """Search for any of the given filenames within folder up to 3 levels deep."""
    folder = os.path.normpath(folder)
    # Fast-path: root and sce_sys
    for sub in ['', 'sce_sys']:
        for name in names:
            p = os.path.join(folder, sub, name) if sub else os.path.join(folder, name)
            if os.path.isfile(p):
                return p
    # Walk
    try:
        for root, dirs, files in os.walk(folder):
            rel = os.path.relpath(root, folder)
            depth = 0 if rel == '.' else rel.count(os.sep) + 1
            if depth > 3:
                dirs[:] = []
                continue
            lower_files = [f.lower() for f in files]
            for name in names:
                if name.lower() in lower_files:
                    idx = lower_files.index(name.lower())
                    return os.path.join(root, files[idx])
    except Exception:
        pass
    return None

def find_sfo(folder):
    return find_meta_file(folder, ['param.sfo'])

def parse_param_json(path):
    """Parse param.json used by some PS5 extraction tools instead of param.sfo."""
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            data = json.load(f)
        title_id = data.get('titleId', '')
        version  = data.get('version') or data.get('masterVersion') or ''
        # Title is nested under localizedParameters
        title = ''
        lp = data.get('localizedParameters', {})
        if lp:
            default_lang = lp.get('defaultLanguage', 'en-US')
            for lang in [default_lang, 'en-US', 'en-GB']:
                t = lp.get(lang, {}).get('titleName', '')
                if t:
                    title = t
                    break
            if not title:
                # Try first available language
                for lang, val in lp.items():
                    if isinstance(val, dict) and val.get('titleName'):
                        title = val['titleName']
                        break
        # Some formats put title at top level
        if not title:
            title = data.get('titleName', '') or data.get('name', '')
        return title.strip(), str(title_id).strip(), str(version).strip()
    except Exception:
        return '', '', ''

def parse_nptitle_dat(path):
    """nptitle.dat is plain text containing just the game title on the first line."""
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            return f.readline().strip()
    except Exception:
        return ''

def sanitize_filename(name):
    # Keep parentheses since we use them in the output format
    name = re.sub(r'[\\/:*?"<>|]', '', name)
    name = re.sub(r'\s+', ' ', name).strip()
    return name[:80]

def get_game_info(folder):
    folder = os.path.normpath(folder)
    title, title_id, version = '', '', ''

    # ── Try param.sfo first ──────────────────────────────────────────────────
    sfo_path = find_sfo(folder)
    if sfo_path:
        info = parse_sfo(sfo_path)
        if info:
            title    = info.get('TITLE') or info.get('TITLE_00') or ''
            title_id = info.get('TITLE_ID', '')
            version  = info.get('VERSION') or info.get('APP_VER') or ''
            if isinstance(title_id, int):
                title_id = ''
    sfo_version = version

    # ── pfs-version.dat — highest priority for version (real installed version)
    # This file lives in sce_sys and contains the true patch version e.g. "1.007.000"
    pfs_path = find_meta_file(folder, ['pfs-version.dat'])
    if pfs_path:
        try:
            with open(pfs_path, 'r', encoding='utf-8', errors='replace') as f:
                pfs_ver = f.read().strip().split('\n')[0].strip()
            if pfs_ver and re.match(r'[\d.]+', pfs_ver):
                version = pfs_ver  # override sfo version with real patch version
        except Exception:
            pass

    # ── Try param.json for title/id if sfo gave nothing useful ───────────────
    if not title and not title_id:
        json_path = find_meta_file(folder, ['param.json'])
        if json_path:
            t, i, v = parse_param_json(json_path)
            title    = title    or t
            title_id = title_id or i
            if not sfo_version and not version:
                version = v

    # ── Try nptitle.dat for title only ───────────────────────────────────────
    if not title:
        npt_path = find_meta_file(folder, ['nptitle.dat'])
        if npt_path:
            title = parse_nptitle_dat(npt_path)

    if isinstance(version, str):
        version = version.strip()
        # Normalise to consistent XX.XXX.XXX format
        # e.g. "01.00" -> "01.000.000"  |  "01.00.000" -> "01.000.000"  |  "01.000.000" unchanged
        if version:
            parts = version.split('.')
            if len(parts) == 2:
                # APP_VER short form e.g. "01.00"
                parts.append('000')
            # Pad each segment
            if len(parts) >= 3:
                parts[0] = parts[0].zfill(2)
                parts[1] = parts[1].zfill(3)
                parts[2] = parts[2].zfill(3)
                version = '.'.join(parts[:3])

    return (
        sanitize_filename(str(title)) if title else None,
        str(title_id).strip() if title_id else None,
        str(version) if version else None
    )


def build_exfat_name(title, title_id, version):
    # Format: "Game Title (01.000.000).exfat"
    name_part = title or title_id
    if not name_part:
        return 'game.exfat'
    if version:
        result = name_part + ' (' + version + ')'
    else:
        result = name_part
    return sanitize_filename(result) + '.exfat'

# ── Scripts / elevation ──────────────────────────────────────────────────────
def extract_scripts(custom_temp=None):
    global _TEMP_DIR
    base = custom_temp if (custom_temp and os.path.isdir(custom_temp)) else None
    _TEMP_DIR = tempfile.mkdtemp(prefix='exfat_builder_', dir=base)
    bat_path = os.path.join(_TEMP_DIR, 'make_image.bat')
    ps1_path = os.path.join(_TEMP_DIR, 'New-OsfExfatImage.ps1')
    ico_path = os.path.join(_TEMP_DIR, 'icon.ico')
    with open(bat_path, 'wb') as f:
        f.write(base64.b64decode(_BAT_B64))
    with open(ps1_path, 'wb') as f:
        f.write(base64.b64decode(_PS1_B64))
    with open(ico_path, 'wb') as f:
        f.write(base64.b64decode(_ICO_B64))
    return bat_path, ico_path

def cleanup_scripts():
    global _TEMP_DIR
    if _TEMP_DIR and os.path.isdir(_TEMP_DIR):
        shutil.rmtree(_TEMP_DIR, ignore_errors=True)

def clear_temp_folder(base_dir):
    removed, failed, size = 0, 0, 0
    try:
        for entry in os.scandir(base_dir):
            if entry.name.startswith('exfat_builder_'):
                try:
                    if entry.is_dir():
                        # sum size
                        for root, dirs, files in os.walk(entry.path):
                            for fn in files:
                                try:
                                    size += os.path.getsize(os.path.join(root, fn))
                                except Exception:
                                    pass
                        shutil.rmtree(entry.path, ignore_errors=True)
                        removed += 1
                except Exception:
                    failed += 1
    except Exception as e:
        return False, str(e), 0
    return True, removed, size

def is_admin():
    try:
        return ctypes.windll.shell32.IsUserAnAdmin()
    except Exception:
        return False

def relaunch_as_admin():
    script = os.path.abspath(sys.argv[0])
    params = ' '.join('"' + a + '"' for a in sys.argv[1:])
    try:
        ret = ctypes.windll.shell32.ShellExecuteW(
            None, 'runas', sys.executable,
            '"' + script + '" ' + params, None, 1)
        if ret <= 32:
            messagebox.showerror('Admin required',
                'This app needs Administrator rights.\n'
                'Right-click the .exe and choose Run as administrator.')
    except Exception as e:
        messagebox.showerror('Elevation failed', str(e))
    sys.exit(0)

if not is_admin():
    relaunch_as_admin()

# ── Theme ────────────────────────────────────────────────────────────────────
BG         = '#000000'
SURFACE2   = '#1c1c1c'
FIELD_BG   = '#f0f0f0'
FIELD_FG   = '#111111'
FIELD_SEL_BG = '#4a9eff'
FIELD_SEL_FG = '#ffffff'
BORDER     = '#444444'
ACCENT     = '#4a9eff'
TEXT       = '#ffffff'
MUTED      = '#aaaaaa'
SUCCESS    = '#4caf50'
WARNING    = '#ff9800'
DANGER     = '#f44336'
TRACK      = '#1e1e1e'
QUEUE_ODD  = '#141414'
QUEUE_EVEN = '#1a1a1a'
INFO_BG    = '#0d1f33'
INFO_FG    = '#7ec8ff'
SETTINGS_BG= '#0f0f0f'

STEPS = {
    '1/4': (0,  20,  'Creating & mounting image...'),
    '2/4': (20, 50,  'Formatting as exFAT...'),
    '3/4': (50, 90,  'Copying game files...'),
    '4/4': (90, 100, 'Dismounting volume...'),
}

class QueueItem:
    def __init__(self, game_folder, output_dir, output_name,
                 game_title=None, title_id=None, version=None):
        self.game_folder = game_folder
        self.output_dir  = output_dir
        self.output_name = output_name
        self.game_title  = game_title
        self.title_id    = title_id
        self.version     = version
        self.status      = 'waiting'

class ExFATBuilder(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title('exFAT Image Builder  [Administrator]')
        self.geometry('780x760')
        self.resizable(True, True)
        self.minsize(680, 640)
        self.configure(bg=BG)
        self.protocol('WM_DELETE_WINDOW', self._on_close)

        self._settings = load_settings()
        self._bat_path, ico_path = extract_scripts(
            self._settings.get('temp_dir'))
        try:
            self.iconbitmap(ico_path)
        except Exception:
            pass

        self.game_folder = tk.StringVar()
        self.output_dir  = tk.StringVar(
            value=self._settings.get('output_dir', ''))
        self.output_name = tk.StringVar(value='game.exfat')
        self.status_text = tk.StringVar(value='Running as Administrator  ✓')
        self._temp_dir_var = tk.StringVar(
            value=self._settings.get('temp_dir', ''))
        self._ftp_ip_var   = tk.StringVar(
            value=self._settings.get('ftp_ip', ''))
        self._ftp_port_var = tk.StringVar(
            value=str(self._settings.get('ftp_port', 2121)))
        self._ftp_path_var = tk.StringVar(
            value=self._settings.get('ftp_path', '/data/etaHEN/games/'))
        self._ftp_auto_var = tk.BooleanVar(
            value=self._settings.get('ftp_auto', False))
        self._discord_var  = tk.StringVar(
            value=self._settings.get('discord_webhook', ''))
        self._sound_var    = tk.BooleanVar(
            value=self._settings.get('notify_sound', True))
        self._retry_var    = tk.IntVar(
            value=self._settings.get('retry_count', 3))
        self._auto_ip_var  = tk.BooleanVar(
            value=self._settings.get('auto_ip', False))
        self._building       = False
        self._start_time     = None
        self._current_pct    = 0
        self._bar_width      = 720
        self._step_label_var = tk.StringVar(value='')
        self._pct_var        = tk.StringVar(value='')
        self._eta_var        = tk.StringVar(value='')
        self._queue          = []
        self._detected_title    = None
        self._detected_title_id = None
        self._detected_version  = None
        # Drive polling
        self._mounted_drive   = None
        self._image_total_gb  = None
        self._copy_start_free = None
        self._copy_start_time = None
        self._drive_poll_id   = None
        self._source_file_count  = 0
        self._current_build_idx  = 0
        # FTP upload cancel
        self._ftp_cancel      = False
        self._ftp_uploading   = False
        # Library
        self._lib_folders     = self._settings.get('lib_folders', [])
        self._lib_games       = []   # list of dicts
        self._lib_cards       = []   # tk widgets
        self._lib_filter      = ''
        # Recently used game folders (last 10)
        self._recent_folders  = self._settings.get('recent_folders', [])
        # Theme
        self._theme_var = tk.StringVar(value=self._settings.get('theme', 'dark'))

        self._build_ui()
        self._render_queue()
        self._refresh_temp_size()
        # Check for updates in background after 3 seconds
        self.after(3000, self._check_for_updates)
        # Check OSFMount after UI is ready
        self.after(500, self._check_osfmount_banner)

        # Restore saved window position and size
        geo = self._settings.get('window_geometry')
        if geo:
            try:
                self.geometry(geo)
                # Make sure it's on screen (handles monitor changes)
                self.update_idletasks()
                x = self.winfo_x()
                y = self.winfo_y()
                w = self.winfo_width()
                h = self.winfo_height()
                sw = self.winfo_screenwidth()
                sh = self.winfo_screenheight()
                # Clamp to screen bounds
                x = max(0, min(x, sw - 200))
                y = max(0, min(y, sh - 200))
                self.geometry('%dx%d+%d+%d' % (w, h, x, y))
            except Exception:
                pass

    def _on_close(self):
        # Save window position and size before closing
        try:
            self._settings['window_geometry'] = self.geometry()
            save_settings(self._settings)
        except Exception:
            pass
        cleanup_scripts()
        self.destroy()

    def _build_ui(self):
        # ── Header ──
        hdr = tk.Frame(self, bg=BG)
        hdr.pack(fill='x', padx=24, pady=(10, 6))
        title_row = tk.Frame(hdr, bg=BG)
        title_row.pack(fill='x')
        tk.Label(title_row, text='exFAT Image Builder',
                 font=('Segoe UI', 16, 'bold'), bg=BG, fg=TEXT).pack(side='left', anchor='w')
        tk.Label(title_row, text='v' + APP_VERSION,
                 font=('Segoe UI', 8), bg=BG, fg='#3a3a3a').pack(side='left', anchor='s', padx=(8,0), pady=(0,4))
        tk.Label(title_row, text='by DecKerr97',
                 font=('Segoe UI', 9), bg=BG, fg='#555555').pack(side='right', anchor='s', pady=(6, 0))
        tk.Label(hdr, text='Build PS5 exFAT game images  \u2014  scripts bundled, game name auto-detected',
                 font=('Segoe UI', 10), bg=BG, fg=MUTED).pack(anchor='w')
        tk.Label(hdr, text='Inspired by NookieAI \u2022 stonemodder (Porkfolio)',
                 font=('Segoe UI', 8), bg=BG, fg='#3a3a3a').pack(anchor='w')
        tk.Frame(self, bg=BORDER, height=1).pack(fill='x')

        # ── Tab bar ──
        tab_bar = tk.Frame(self, bg='#0a0a0a')
        tab_bar.pack(fill='x')
        self._active_tab = tk.StringVar(value='build')
        self._tab_build_frame  = None
        self._tab_extract_frame = None

        def _make_tab(parent, text, key):
            btn = tk.Label(parent, text=text,
                           font=('Segoe UI', 10), bg='#0a0a0a', fg=MUTED,
                           padx=20, pady=8, cursor='hand2')
            btn.pack(side='left')
            def _click(e, k=key, b=btn):
                self._switch_tab(k)
            btn.bind('<Button-1>', _click)
            return btn

        self._tab_build_btn   = _make_tab(tab_bar, '\U0001f528  Build',         'build')
        self._tab_library_btn = _make_tab(tab_bar, '\U0001f4da  Library',       'library')
        self._tab_images_btn  = _make_tab(tab_bar, '\U0001f4be  My Images',     'images')
        self._tab_ps5_btn     = _make_tab(tab_bar, '\U0001f3ae  PS5 Manager',   'ps5')
        self._tab_extract_btn = _make_tab(tab_bar, '\U0001f4e4  Extract',       'extract')
        self._tab_files_btn   = _make_tab(tab_bar, '\U0001f5c2  File Manager',  'files')
        self._tab_ftp_btn     = _make_tab(tab_bar, '\U0001f4e1  FTP Upload',    'ftp')
        self._tab_ftpbr_btn   = _make_tab(tab_bar, '\U0001f4e1  PS5 Browser',   'ftpbr')
        self._tab_help_btn    = _make_tab(tab_bar, '\u2753  Help',              'help')
        tk.Frame(self, bg=BORDER, height=1).pack(fill='x')

        # ── Content area ──
        self._tab_build_frame   = tk.Frame(self, bg=BG)
        self._tab_library_frame = tk.Frame(self, bg=BG)
        self._tab_images_frame  = tk.Frame(self, bg=BG)
        self._tab_ps5_frame     = tk.Frame(self, bg=BG)
        self._tab_extract_frame = tk.Frame(self, bg=BG)
        self._tab_files_frame   = tk.Frame(self, bg=BG)
        self._tab_ftp_frame     = tk.Frame(self, bg=BG)
        self._tab_ftpbr_frame   = tk.Frame(self, bg=BG)
        self._tab_help_frame    = tk.Frame(self, bg=BG)

        self._build_build_tab(self._tab_build_frame)
        self._build_library_tab(self._tab_library_frame)
        self._build_images_tab(self._tab_images_frame)
        self._build_ps5_tab(self._tab_ps5_frame)
        self._build_extract_tab(self._tab_extract_frame)
        self._build_files_tab(self._tab_files_frame)
        self._build_ftp_tab(self._tab_ftp_frame)
        self._build_ftpbr_tab(self._tab_ftpbr_frame)
        self._build_help_tab(self._tab_help_frame)

        self._switch_tab('build')

    def _switch_tab(self, key):
        self._active_tab.set(key)
        all_frames = [self._tab_build_frame, self._tab_library_frame,
                      self._tab_images_frame, self._tab_ps5_frame,
                      self._tab_extract_frame, self._tab_files_frame,
                      self._tab_ftp_frame, self._tab_ftpbr_frame,
                      self._tab_help_frame]
        all_btns   = [self._tab_build_btn, self._tab_library_btn,
                      self._tab_images_btn, self._tab_ps5_btn,
                      self._tab_extract_btn, self._tab_files_btn,
                      self._tab_ftp_btn, self._tab_ftpbr_btn,
                      self._tab_help_btn]
        all_keys   = ['build','library','images','ps5','extract','files',
                      'ftp','ftpbr','help']
        for frame, btn, k in zip(all_frames, all_btns, all_keys):
            frame.pack_forget()
            btn.config(fg=MUTED, bg='#0a0a0a')
        idx = all_keys.index(key)
        all_frames[idx].pack(fill='both', expand=True)
        all_btns[idx].config(fg=TEXT, bg='#1a1a1a')

    # ══════════════════════════════════════════════════════════════════════════
    # MY IMAGES TAB
    # ══════════════════════════════════════════════════════════════════════════
    def _build_images_tab(self, parent):
        self._img_scan_dirs = self._settings.get('img_scan_dirs', [])

        top = tk.Frame(parent, bg=BG)
        top.pack(fill='x', padx=24, pady=(12, 6))
        tk.Label(top, text='My Images',
                 font=('Segoe UI', 13, 'bold'), bg=BG, fg=TEXT).pack(side='left')
        tk.Button(top, text='\U0001f504 Refresh',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=TEXT,
                  activebackground=BORDER, relief='flat', bd=0,
                  padx=10, pady=4, cursor='hand2',
                  command=self._img_scan).pack(side='right')
        tk.Button(top, text='+ Add folder',
                  font=('Segoe UI', 9), bg=ACCENT, fg=TEXT,
                  activebackground='#3a8eef', relief='flat', bd=0,
                  padx=10, pady=4, cursor='hand2',
                  command=self._img_add_dir).pack(side='right', padx=(0,6))

        # Scan dirs strip
        self._img_dirs_frame = tk.Frame(parent, bg=SURFACE2,
                                         highlightbackground=BORDER,
                                         highlightthickness=1)
        self._img_dirs_frame.pack(fill='x', padx=24, pady=(0,6))
        self._img_render_dirs()

        # Status + search
        ctrl = tk.Frame(parent, bg=BG)
        ctrl.pack(fill='x', padx=24, pady=(0,6))
        self._img_status_var = tk.StringVar(value='Add a folder containing .exfat files and click Refresh')
        tk.Label(ctrl, textvariable=self._img_status_var,
                 font=('Segoe UI', 8), bg=BG, fg=MUTED).pack(side='left')
        self._img_search_var = tk.StringVar()
        self._img_search_var.trace('w', lambda *a: self._img_apply_filter())
        sf = tk.Frame(ctrl, bg=FIELD_BG,
                      highlightbackground=BORDER, highlightthickness=1)
        sf.pack(side='right')
        tk.Label(sf, text='\U0001f50d', font=('Segoe UI', 9),
                 bg=FIELD_BG, fg=MUTED).pack(side='left', padx=(6,2))
        tk.Entry(sf, textvariable=self._img_search_var,
                 font=('Segoe UI', 9), bg=FIELD_BG, fg=FIELD_FG,
                 insertbackground=FIELD_FG, relief='flat', bd=4,
                 width=22).pack(side='left')

        # List
        list_outer = tk.Frame(parent, bg=SURFACE2,
                              highlightbackground=BORDER, highlightthickness=1)
        list_outer.pack(fill='both', expand=True, padx=24, pady=(0,8))
        self._img_listbox = tk.Listbox(list_outer, font=('Consolas', 9),
                                        bg=SURFACE2, fg=TEXT,
                                        selectbackground=ACCENT,
                                        selectforeground='#ffffff',
                                        activestyle='none', relief='flat',
                                        bd=6, selectmode='extended')
        img_sb = tk.Scrollbar(list_outer, command=self._img_listbox.yview,
                              bg=SURFACE2, troughcolor=BG)
        self._img_listbox.configure(yscrollcommand=img_sb.set)
        img_sb.pack(side='right', fill='y')
        self._img_listbox.pack(fill='both', expand=True)
        self._img_listbox.bind('<Button-3>', self._img_context_menu)
        self._img_listbox.bind('<Double-Button-1>', self._img_context_menu)
        self._img_entries = []  # list of dicts

        # Batch upload bar
        batch_row = tk.Frame(parent, bg=BG)
        batch_row.pack(fill='x', padx=24, pady=(4,8))
        self._img_sel_var = tk.StringVar(value='')
        tk.Label(batch_row, textvariable=self._img_sel_var,
                 font=('Segoe UI', 8), bg=BG, fg=MUTED).pack(side='left')
        self._img_batch_btn = tk.Button(
            batch_row, text='\u2191 Upload selected to PS5',
            font=('Segoe UI', 9), bg=ACCENT, fg=TEXT,
            activebackground='#3a8eef', activeforeground=TEXT,
            relief='flat', bd=0, padx=12, pady=4,
            cursor='hand2', command=self._img_batch_upload)
        self._img_batch_btn.pack(side='right')
        self._img_listbox.bind('<<ListboxSelect>>', self._img_update_sel)

    def _img_render_dirs(self):
        for w in self._img_dirs_frame.winfo_children():
            w.destroy()
        if not self._img_scan_dirs:
            tk.Label(self._img_dirs_frame,
                     text='No folders added — click "+ Add folder"',
                     font=('Segoe UI', 8), bg=SURFACE2, fg=MUTED,
                     pady=6).pack(anchor='w', padx=10)
            return
        for i, d in enumerate(self._img_scan_dirs):
            row = tk.Frame(self._img_dirs_frame, bg=SURFACE2)
            row.pack(fill='x')
            tk.Label(row, text='\U0001f4be  ' + d,
                     font=('Segoe UI', 8), bg=SURFACE2, fg=MUTED,
                     anchor='w').pack(side='left', padx=10, pady=4,
                                      fill='x', expand=True)
            tk.Button(row, text='\u2715', font=('Segoe UI', 8),
                      bg=SURFACE2, fg=DANGER, activebackground=SURFACE2,
                      relief='flat', bd=0, padx=8, cursor='hand2',
                      command=lambda ix=i: self._img_remove_dir(ix)
                      ).pack(side='right')

    def _img_add_dir(self):
        p = filedialog.askdirectory(title='Select folder containing .exfat images')
        if not p:
            return
        p = os.path.normpath(p)
        if p not in self._img_scan_dirs:
            self._img_scan_dirs.append(p)
            self._settings['img_scan_dirs'] = self._img_scan_dirs
            save_settings(self._settings)
        self._img_render_dirs()
        self._img_scan()

    def _img_remove_dir(self, idx):
        if 0 <= idx < len(self._img_scan_dirs):
            self._img_scan_dirs.pop(idx)
            self._settings['img_scan_dirs'] = self._img_scan_dirs
            save_settings(self._settings)
            self._img_render_dirs()
            self._img_apply_filter()

    def _img_scan(self):
        self._img_status_var.set('Scanning...')
        self._img_entries = []
        self._img_listbox.delete(0, 'end')
        def worker():
            entries = []
            for d in self._img_scan_dirs:
                if not os.path.isdir(d):
                    continue
                try:
                    for fn in os.listdir(d):
                        if not fn.lower().endswith('.exfat'):
                            continue
                        fp = os.path.join(d, fn)
                        try:
                            sz  = os.path.getsize(fp)
                            mtime = os.path.getmtime(fp)
                        except Exception:
                            sz, mtime = 0, 0
                        # Try to match against build history
                        title = os.path.splitext(fn)[0]
                        entries.append({
                            'path':  fp,
                            'name':  fn,
                            'title': title,
                            'size':  sz,
                            'mtime': mtime,
                            'dir':   d,
                        })
                except Exception:
                    pass
            entries.sort(key=lambda e: e['mtime'], reverse=True)
            self.after(0, self._img_scan_done, entries)
        threading.Thread(target=worker, daemon=True).start()

    def _img_scan_done(self, entries):
        self._img_entries = entries
        total_gb = sum(e['size'] for e in entries) / 1024**3
        self._img_status_var.set(
            '%d image(s)  —  %.1f GB total' % (len(entries), total_gb))
        self._img_apply_filter()

    def _img_apply_filter(self):
        q = self._img_search_var.get().lower().strip() \
            if hasattr(self, '_img_search_var') else ''
        filtered = [e for e in self._img_entries
                    if q in e['title'].lower()] if q else self._img_entries
        self._img_listbox.delete(0, 'end')
        for e in filtered:
            sz = ('%.2f GB' % (e['size']/1024**3)
                  if e['size'] >= 1024**3 else '%d MB' % (e['size']//1024**2))
            built = time.strftime('%Y-%m-%d', time.localtime(e['mtime']))
            self._img_listbox.insert(
                'end', '\U0001f4be  %-45s %8s   %s' % (e['name'], sz, built))

    def _img_context_menu(self, event):
        sel = self._img_listbox.nearest(event.y)
        self._img_listbox.selection_clear(0, 'end')
        self._img_listbox.selection_set(sel)
        q = self._img_search_var.get().lower().strip() \
            if hasattr(self, '_img_search_var') else ''
        filtered = [e for e in self._img_entries
                    if q in e['title'].lower()] if q else self._img_entries
        if sel >= len(filtered):
            return
        entry = filtered[sel]
        menu = tk.Menu(self, tearoff=0, bg=SURFACE2, fg=TEXT,
                       activebackground=ACCENT, activeforeground=TEXT,
                       font=('Segoe UI', 9))
        menu.add_command(label=entry['name'], state='disabled',
                         font=('Segoe UI', 9, 'bold'))
        menu.add_separator()
        menu.add_command(label='\u2191  Upload to PS5',
                         command=lambda: self._ftp_upload_file(entry['path']))
        menu.add_command(label='\U0001f5c2  Open in File Manager',
                         command=lambda: self._open_image_in_fm(entry['path']))
        menu.add_command(label='\U0001f4c2  Open containing folder',
                         command=lambda: subprocess.Popen(
                             'explorer "' + entry['dir'] + '"', shell=True))
        menu.add_separator()
        menu.add_command(label='\U0001f5d1  Delete image',
                         command=lambda: self._img_delete(entry))
        menu.tk_popup(event.x_root, event.y_root)

    def _open_image_in_fm(self, path):
        self._tab_files_frame  # ensure built
        self._fm_image_var.set(path)
        self._switch_tab('files')

    def _img_delete(self, entry):
        if not messagebox.askyesno('Delete image',
                'Delete this image?\n\n' + entry['name'] +
                '\n\nThis cannot be undone.'):
            return
        try:
            os.remove(entry['path'])
            self._img_scan()
        except Exception as e:
            messagebox.showerror('Delete failed', str(e))

    def _img_update_sel(self, event=None):
        sels = self._img_listbox.curselection()
        q = self._img_search_var.get().lower().strip() \
            if hasattr(self, '_img_search_var') else ''
        filtered = [e for e in self._img_entries
                    if q in e['title'].lower()] if q else self._img_entries
        sel_entries = [filtered[i] for i in sels if i < len(filtered)]
        if sel_entries:
            total = sum(e['size'] for e in sel_entries)
            self._img_sel_var.set(
                '%d selected  —  %.2f GB' % (len(sel_entries), total/1024**3))
        else:
            self._img_sel_var.set('')

    def _img_batch_upload(self):
        sels = self._img_listbox.curselection()
        q = self._img_search_var.get().lower().strip() \
            if hasattr(self, '_img_search_var') else ''
        filtered = [e for e in self._img_entries
                    if q in e['title'].lower()] if q else self._img_entries
        paths = [filtered[i]['path'] for i in sels if i < len(filtered)]
        if not paths:
            messagebox.showwarning('Nothing selected',
                'Select one or more images to upload.\nUse Ctrl+Click for multiple.')
            return
        ip = self._ftp_ip_var.get().strip()
        if not ip:
            messagebox.showwarning('No PS5 IP',
                'Enter your PS5 IP address in Settings first.')
            return
        if not messagebox.askyesno('Batch upload',
                'Upload %d image(s) to PS5?\n\n%s' % (
                    len(paths), '\n'.join(os.path.basename(p) for p in paths[:6]) +
                    ('\n...' if len(paths) > 6 else ''))):
            return
        self._img_batch_queue = list(paths)
        self._img_batch_total = len(paths)
        self._img_batch_done  = 0
        self._img_batch_next()

    def _img_batch_next(self):
        if not self._img_batch_queue:
            self._set_status(
                'Batch upload complete: %d image(s)' % self._img_batch_total,
                SUCCESS)
            messagebox.showinfo('Batch upload complete',
                '%d image(s) uploaded to PS5 successfully!' % self._img_batch_total)
            return
        path = self._img_batch_queue.pop(0)
        self._img_batch_done += 1
        self._set_status(
            'Uploading %d/%d: %s' % (
                self._img_batch_done, self._img_batch_total,
                os.path.basename(path)), ACCENT)
        self._ftp_upload_file(path, on_done=lambda: self._img_batch_next())

    # ── Recently used folders ─────────────────────────────────────────────────
    def _update_recent_menu(self):
        if hasattr(self, '_recent_menu'):
            self._recent_menu['values'] = self._recent_folders
            self._recent_menu.set('')

    def _on_recent_select(self, event=None):
        folder = self._recent_var.get()
        if folder and os.path.isdir(folder):
            self.game_folder.set(folder)
            self._detect_game_info(folder)

    def _add_to_recent(self, folder):
        folder = os.path.normpath(folder)
        if folder in self._recent_folders:
            self._recent_folders.remove(folder)
        self._recent_folders.insert(0, folder)
        self._recent_folders = self._recent_folders[:10]
        self._settings['recent_folders'] = self._recent_folders
        save_settings(self._settings)
        self._update_recent_menu()

    # ── Queue save / load ─────────────────────────────────────────────────────
    def _queue_save(self):
        if not self._queue:
            messagebox.showwarning('Empty queue', 'Nothing in queue to save.')
            return
        path = filedialog.asksaveasfilename(
            title='Save queue',
            defaultextension='.json',
            filetypes=[('Queue files', '*.json'), ('All files', '*.*')],
            initialfile='my_queue.json')
        if not path:
            return
        data = [{'game_folder': i.game_folder,
                  'output_dir':  i.output_dir,
                  'output_name': i.output_name,
                  'game_title':  i.game_title  or '',
                  'title_id':    i.title_id    or '',
                  'version':     i.version     or ''}
                for i in self._queue]
        try:
            with open(path, 'w') as f:
                json.dump(data, f, indent=2)
            self._set_status('Queue saved: ' + os.path.basename(path), SUCCESS)
        except Exception as e:
            messagebox.showerror('Save failed', str(e))

    def _queue_load(self):
        path = filedialog.askopenfilename(
            title='Load queue',
            filetypes=[('Queue files', '*.json'), ('All files', '*.*')])
        if not path:
            return
        try:
            with open(path, 'r') as f:
                data = json.load(f)
            loaded = 0
            for entry in data:
                item = QueueItem(
                    entry.get('game_folder', ''),
                    entry.get('output_dir',  ''),
                    entry.get('output_name', 'game.exfat'),
                    entry.get('game_title',  None),
                    entry.get('title_id',    None),
                    entry.get('version',     None))
                if os.path.isdir(item.game_folder):
                    self._queue.append(item)
                    loaded += 1
            self._render_queue()
            self._set_status('Loaded %d item(s) from queue file' % loaded, SUCCESS)
            if loaded < len(data):
                messagebox.showwarning('Some items skipped',
                    '%d item(s) skipped because the game folder no longer exists.'
                    % (len(data) - loaded))
        except Exception as e:
            messagebox.showerror('Load failed', str(e))

    # ── Library Build All ─────────────────────────────────────────────────────
    def _lib_build_all(self):
        if not self._lib_games:
            messagebox.showwarning('No games',
                'Scan your library first — no games loaded yet.')
            return
        odir = self.output_dir.get().strip()
        if not odir:
            odir = filedialog.askdirectory(
                title='Select output directory for all library games')
            if not odir:
                return
            odir = odir.replace('/', '\\')
            self.output_dir.set(odir)
            self._settings['output_dir'] = odir
            save_settings(self._settings)

        added = 0
        for game in self._lib_games:
            name = build_exfat_name(
                game['title'] or None,
                game['title_id'] or None,
                game['version'] or None)
            # Skip if already in queue
            already = any(os.path.normpath(q.game_folder) ==
                          os.path.normpath(game['folder'])
                          for q in self._queue)
            if already:
                continue
            item = QueueItem(game['folder'], odir, name,
                             game['title'], game['title_id'], game['version'])
            self._queue.append(item)
            added += 1

        self._render_queue()
        self._switch_tab('build')
        self._set_status(
            'Added %d game(s) from library to queue' % added, SUCCESS)
        if added:
            messagebox.showinfo('Added to queue',
                '%d game(s) added to the Build queue.\n\n'
                'Click Build All to start building.' % added)

    # ══════════════════════════════════════════════════════════════════════════
    # PS5 MANAGER TAB
    # ══════════════════════════════════════════════════════════════════════════
    def _build_ps5_tab(self, parent):
        self._ps5mgr_local  = []  # local .exfat entries
        self._ps5mgr_remote = []  # remote .exfat entries
        self._ps5mgr_merged = []  # merged view

        top = tk.Frame(parent, bg=BG)
        top.pack(fill='x', padx=24, pady=(12,6))
        tk.Label(top, text='PS5 Game Manager',
                 font=('Segoe UI', 13, 'bold'), bg=BG, fg=TEXT).pack(side='left')
        tk.Button(top, text='\U0001f504 Refresh All',
                  font=('Segoe UI', 9), bg=ACCENT, fg=TEXT,
                  activebackground='#3a8eef', relief='flat', bd=0,
                  padx=10, pady=4, cursor='hand2',
                  command=self._ps5mgr_refresh).pack(side='right')

        # Legend
        leg = tk.Frame(parent, bg=BG)
        leg.pack(fill='x', padx=24, pady=(0,6))
        for colour, label in [(SUCCESS, 'On PS5'), (WARNING, 'Built, not uploaded'),
                               (MUTED, 'Not built yet')]:
            dot = tk.Canvas(leg, width=10, height=10, bg=BG,
                            highlightthickness=0)
            dot.pack(side='left', padx=(0,4))
            dot.create_oval(1,1,9,9, fill=colour, outline='')
            tk.Label(leg, text=label, font=('Segoe UI', 8),
                     bg=BG, fg=MUTED).pack(side='left', padx=(0,16))

        self._ps5mgr_status_var = tk.StringVar(
            value='Click "Refresh All" to load local images and PS5 games')
        tk.Label(parent, textvariable=self._ps5mgr_status_var,
                 font=('Segoe UI', 8), bg=BG, fg=MUTED,
                 anchor='w').pack(fill='x', padx=24, pady=(0,4))

        # List
        list_outer = tk.Frame(parent, bg=SURFACE2,
                              highlightbackground=BORDER, highlightthickness=1)
        list_outer.pack(fill='both', expand=True, padx=24, pady=(0,8))

        # Header row
        hdr = tk.Frame(list_outer, bg='#0f0f0f')
        hdr.pack(fill='x')
        for text, w in [('Status', 8), ('Game', 40), ('Local size', 12),
                         ('PS5 size', 12), ('Action', 14)]:
            tk.Label(hdr, text=text, font=('Segoe UI', 8, 'bold'),
                     bg='#0f0f0f', fg=MUTED, width=w, anchor='w'
                     ).pack(side='left', padx=(8,0), pady=4)

        self._ps5mgr_frame = tk.Frame(list_outer, bg=SURFACE2)
        self._ps5mgr_canvas = tk.Canvas(self._ps5mgr_frame, bg=SURFACE2,
                                         highlightthickness=0)
        ps5_sb = tk.Scrollbar(self._ps5mgr_frame,
                               command=self._ps5mgr_canvas.yview,
                               bg=SURFACE2, troughcolor=BG)
        self._ps5mgr_canvas.configure(yscrollcommand=ps5_sb.set)
        ps5_sb.pack(side='right', fill='y')
        self._ps5mgr_canvas.pack(side='left', fill='both', expand=True)
        self._ps5mgr_frame.pack(fill='both', expand=True)

        self._ps5mgr_rows = tk.Frame(self._ps5mgr_canvas, bg=SURFACE2)
        self._ps5mgr_canvas.create_window(
            (0,0), window=self._ps5mgr_rows, anchor='nw', tags='pmr')
        self._ps5mgr_rows.bind('<Configure>', lambda e:
            self._ps5mgr_canvas.configure(
                scrollregion=self._ps5mgr_canvas.bbox('all')))
        self._ps5mgr_canvas.bind('<Configure>', lambda e:
            self._ps5mgr_canvas.itemconfig('pmr', width=e.width))
        self._ps5mgr_canvas.bind('<MouseWheel>',
            lambda e: self._ps5mgr_canvas.yview_scroll(
                int(-1*(e.delta/120)), 'units'))

    def _ps5mgr_refresh(self):
        self._ps5mgr_status_var.set('Loading...')
        for w in self._ps5mgr_rows.winfo_children():
            w.destroy()

        def worker():
            # Local .exfat files from My Images scan dirs
            local = {}
            for d in self._settings.get('img_scan_dirs', []):
                if not os.path.isdir(d):
                    continue
                for fn in os.listdir(d):
                    if fn.lower().endswith('.exfat'):
                        fp = os.path.join(d, fn)
                        try:
                            sz = os.path.getsize(fp)
                        except Exception:
                            sz = 0
                        local[fn] = {'name': fn, 'path': fp,
                                     'size': sz, 'dir': d}

            # Remote files from PS5
            remote = {}
            ip = self._ftp_ip_var.get().strip()
            if ip:
                try:
                    import ftplib
                    ftp = self._ftp_connect()
                    remote_dir = self._ftp_path_var.get().strip() or '/data/etaHEN/games/'
                    lines = []
                    ftp.retrlines('LIST ' + remote_dir, lines.append)
                    ftp.quit()
                    for line in lines:
                        parts = line.split()
                        if len(parts) < 9:
                            continue
                        name = ' '.join(parts[8:])
                        if not name.lower().endswith('.exfat'):
                            continue
                        try:
                            sz = int(parts[4])
                        except Exception:
                            sz = 0
                        remote[name] = {'name': name, 'size': sz}
                except Exception:
                    pass

            # Merge
            all_names = sorted(set(list(local.keys()) + list(remote.keys())),
                                key=str.lower)
            merged = []
            for name in all_names:
                l = local.get(name)
                r = remote.get(name)
                if l and r:
                    status = 'on_ps5'
                elif l:
                    status = 'local_only'
                else:
                    status = 'remote_only'
                merged.append({
                    'name':        name,
                    'status':      status,
                    'local':       l,
                    'remote':      r,
                })
            self.after(0, self._ps5mgr_render, merged,
                       len(local), len(remote))

        threading.Thread(target=worker, daemon=True).start()

    def _ps5mgr_render(self, merged, n_local, n_remote):
        for w in self._ps5mgr_rows.winfo_children():
            w.destroy()
        self._ps5mgr_merged = merged
        self._ps5mgr_status_var.set(
            '%d local  •  %d on PS5  •  %d total unique games' % (
                n_local, n_remote, len(merged)))

        colour_map = {'on_ps5': SUCCESS, 'local_only': WARNING,
                      'remote_only': MUTED}
        label_map  = {'on_ps5': '\u2713 On PS5',
                      'local_only': '\u2191 Not uploaded',
                      'remote_only': '\u25cf PS5 only'}

        for i, item in enumerate(merged):
            row_bg = QUEUE_ODD if i % 2 == 0 else QUEUE_EVEN
            row = tk.Frame(self._ps5mgr_rows, bg=row_bg)
            row.pack(fill='x')

            colour = colour_map[item['status']]

            # Status dot + label
            dot = tk.Canvas(row, width=10, height=10, bg=row_bg,
                            highlightthickness=0)
            dot.pack(side='left', padx=(10,4), pady=10)
            dot.create_oval(1,1,9,9, fill=colour, outline='')
            tk.Label(row, text=label_map[item['status']],
                     font=('Segoe UI', 8), bg=row_bg,
                     fg=colour, width=12, anchor='w').pack(side='left')

            # Name
            name_short = item['name']
            if len(name_short) > 38:
                name_short = name_short[:36] + '…'
            tk.Label(row, text=name_short,
                     font=('Segoe UI', 8, 'bold'), bg=row_bg,
                     fg=TEXT, anchor='w').pack(side='left', fill='x', expand=True)

            # Sizes
            def fmt(sz):
                if sz is None: return '—'
                return ('%.1f GB' % (sz/1024**3) if sz >= 1024**3
                        else '%d MB' % (sz//1024**2))

            tk.Label(row, text=fmt(item['local']['size'] if item['local'] else None),
                     font=('Segoe UI', 8), bg=row_bg, fg=MUTED,
                     width=10, anchor='e').pack(side='left', padx=4)
            tk.Label(row, text=fmt(item['remote']['size'] if item['remote'] else None),
                     font=('Segoe UI', 8), bg=row_bg, fg=MUTED,
                     width=10, anchor='e').pack(side='left', padx=(0,8))

            # Action button
            if item['status'] == 'local_only' and item['local']:
                tk.Button(row, text='\u2191 Upload',
                          font=('Segoe UI', 8), bg=ACCENT, fg=TEXT,
                          activebackground='#3a8eef', relief='flat', bd=0,
                          padx=8, pady=2, cursor='hand2',
                          command=lambda p=item['local']['path']:
                              self._ftp_upload_file(p)
                          ).pack(side='right', padx=(0,8), pady=4)
            elif item['status'] == 'on_ps5' and item['local']:
                # Version check — parse version from filename
                local_ver  = self._parse_ver_from_name(item['name'])
                remote_ver = self._parse_ver_from_name(item.get('remote', {}).get('name', ''))
                if local_ver and remote_ver and local_ver != remote_ver:
                    lbl = '\u26a0 v%s local / v%s PS5' % (local_ver, remote_ver)
                    clr = WARNING
                else:
                    lbl = 'In sync \u2713'
                    clr = SUCCESS
                tk.Label(row, text=lbl, font=('Segoe UI', 8),
                         bg=row_bg, fg=clr).pack(side='right', padx=(0,12), pady=4)

    # ══════════════════════════════════════════════════════════════════════════
    # HELP TAB
    # ══════════════════════════════════════════════════════════════════════════
    def _build_help_tab(self, parent):
        # Scrollable canvas
        outer = tk.Frame(parent, bg=BG)
        outer.pack(fill='both', expand=True)
        canvas = tk.Canvas(outer, bg=BG, highlightthickness=0)
        sb = tk.Scrollbar(outer, command=canvas.yview,
                          bg=SURFACE2, troughcolor=BG)
        canvas.configure(yscrollcommand=sb.set)
        sb.pack(side='right', fill='y')
        canvas.pack(side='left', fill='both', expand=True)
        body = tk.Frame(canvas, bg=BG)
        canvas.create_window((0,0), window=body, anchor='nw', tags='hb')
        body.bind('<Configure>', lambda e:
            canvas.configure(scrollregion=canvas.bbox('all')))
        canvas.bind('<Configure>', lambda e:
            canvas.itemconfig('hb', width=e.width))
        canvas.bind('<MouseWheel>',
            lambda e: canvas.yview_scroll(int(-1*(e.delta/120)), 'units'))

        def section(title):
            tk.Label(body, text=title, font=('Segoe UI', 11, 'bold'),
                     bg=BG, fg=ACCENT, anchor='w').pack(
                         fill='x', padx=32, pady=(20,4))
            tk.Frame(body, bg=BORDER, height=1).pack(fill='x', padx=32)

        def qa(question, answer):
            tk.Label(body, text=question, font=('Segoe UI', 9, 'bold'),
                     bg=BG, fg=TEXT, anchor='w',
                     wraplength=680, justify='left').pack(
                         fill='x', padx=40, pady=(10,2))
            tk.Label(body, text=answer, font=('Segoe UI', 9),
                     bg=BG, fg=MUTED, anchor='w',
                     wraplength=680, justify='left').pack(
                         fill='x', padx=40, pady=(0,4))

        # Header
        tk.Label(body, text='Help & FAQ',
                 font=('Segoe UI', 16, 'bold'), bg=BG, fg=TEXT).pack(
                     anchor='w', padx=32, pady=(20,4))
        tk.Label(body,
                 text='exFAT Image Builder v' + APP_VERSION + '  —  by DecKerr97',
                 font=('Segoe UI', 9), bg=BG, fg='#3a3a3a').pack(anchor='w', padx=32)

        section('Getting Started')
        qa('Do my friends need Python installed?',
           'No. The .exe includes everything. The only requirement is OSFMount '
           '(free download from osforensics.com). No Python, no command line, nothing else.')
        qa('What is OSFMount and why do I need it?',
           'OSFMount is a free tool by PassMark that mounts disk image files as '
           'Windows drive letters. The app uses it to create, format and mount the '
           'exFAT image during building. Without it, builds cannot run.')
        qa('What does "must contain eboot.bin" mean?',
           'eboot.bin is the main executable of a PS5 game. It lives in the root '
           'of a properly extracted game folder. If it\'s missing the dump is '
           'incomplete or incorrectly structured.')

        section('Building')
        qa('What is the temp folder for?',
           'During a build the app creates a blank disk image the same size as your '
           'game, mounts it, formats it as exFAT, copies all game files in, then '
           'saves the finished image to your output directory. The temp folder is '
           'where this happens. You need enough free space there to hold the full '
           'game size — so if your C: drive is low, point it at an external drive.')
        qa('Why is the version showing 01.000.000 for every game?',
           'The version comes from pfs-version.dat inside sce_sys, which contains '
           'the real installed patch version. If your dump is missing this file, '
           'the app falls back to param.sfo\'s APP_VER field which is always the '
           'base version (e.g. 01.00). This is normal for some extraction tools.')
        qa('Does the game name come from the internet?',
           'No. Everything comes from files inside the game folder itself — '
           'param.sfo, pfs-version.dat, param.json, or nptitle.dat. The PPSA/CUSA '
           'ID is exactly what Sony stored in the dump, not a web lookup.')
        qa('Build failed — what do I check?',
           '1. Make sure OSFMount is installed.\n'
           '2. Make sure eboot.bin exists in the game folder root.\n'
           '3. Check the output drive has enough free space (same size as the game).\n'
           '4. Check the temp folder drive also has enough free space.\n'
           '5. Make sure the app is running as Administrator (it should prompt automatically).')

        section('FTP & PS5')
        qa('How do I find my PS5\'s IP address?',
           'On the PS5 go to Settings → Network → View Connection Status. '
           'You can also use the "Auto-detect PS5 IP" button in Settings — it scans '
           'your network for open FTP ports automatically.')
        qa('What port should I use?',
           'etaHEN typically uses port 2121. GoldHEN typically uses port 2122. '
           'Check which homebrew you\'re running if unsure.')
        qa('Where do images go on the PS5?',
           'The default path is /data/etaHEN/games/ — this is where etaHEN looks '
           'for exFAT images to load. If you\'re using a different homebrew, '
           'change the PS5 path in Settings accordingly.')

        section('File Manager')
        qa('Can I edit files inside an .exfat image without rebuilding?',
           'Yes — that\'s what the File Manager tab is for. Mount the image '
           '(read-write), navigate the folder structure, add/replace/delete files, '
           'then dismount. The image is updated immediately without needing a full rebuild.')
        qa('Is it safe to dismount without doing anything special?',
           'Yes, as long as you\'re not mid-copy. The app uses OSFMount\'s standard '
           'dismount command which flushes any pending writes before unmounting.')

        section('Common Errors')
        qa('"Source directory not found" error',
           'Your game folder name contains [ ] brackets which confuse PowerShell\'s '
           'path handling. Rename the folder to remove the brackets and try again.')
        qa('"No free drive letters" error',
           'Windows has no available drive letters (D: through Z: are all in use). '
           'Disconnect some external drives or network shares and try again.')
        qa('The Build All button is not visible',
           'Make the window taller, or collapse the Settings panel (click ▶ SETTINGS) '
           'and the Output Log to free up vertical space.')

        section('About')
        qa('Who made this?',
           'DecKerr97. Inspired by NookieAI and stonemodder\'s Porkfolio.')
        qa('Where can I get the latest version?',
           'github.com/kerrdec97/ps5-exfat-builder — check the Releases page. '
           'The app also checks for updates automatically on launch.')
        qa('How do I report a bug?',
           'Open an issue on GitHub with a description of what happened and '
           'the output from the Output Log if available.')

        # Bottom padding
        tk.Frame(body, bg=BG, height=8).pack()

        # ── Changelog ──
        section('Changelog')
        self._changelog_var = tk.StringVar(value='Loading...')
        changelog_lbl = tk.Label(body, textvariable=self._changelog_var,
                                  font=('Segoe UI', 9), bg=BG, fg=MUTED,
                                  anchor='w', justify='left',
                                  wraplength=680)
        changelog_lbl.pack(fill='x', padx=40, pady=(8, 4))
        tk.Button(body, text='\U0001f504 Refresh changelog',
                  font=('Segoe UI', 8), bg=SURFACE2, fg=MUTED,
                  activebackground=BORDER, activeforeground=TEXT,
                  relief='flat', bd=0, padx=10, pady=4,
                  cursor='hand2',
                  command=self._fetch_changelog).pack(anchor='w', padx=40)
        self.after(2000, self._fetch_changelog)

        tk.Frame(body, bg=BG, height=32).pack()

    def _build_library_tab(self, parent):
        self._lib_view_mode = tk.StringVar(value='grid')

        # ── Top bar ──
        top = tk.Frame(parent, bg=BG)
        top.pack(fill='x', padx=24, pady=(12, 6))
        tk.Label(top, text='Game Library',
                 font=('Segoe UI', 13, 'bold'), bg=BG, fg=TEXT).pack(side='left')

        tk.Button(top, text='\U0001f504 Scan',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=TEXT,
                  activebackground=BORDER, activeforeground=TEXT,
                  relief='flat', bd=0, padx=10, pady=4,
                  cursor='hand2',
                  command=self._lib_scan).pack(side='right')
        tk.Button(top, text='\U0001f528 Build All',
                  font=('Segoe UI', 9), bg=SUCCESS, fg=TEXT,
                  activebackground='#3d9140', activeforeground=TEXT,
                  relief='flat', bd=0, padx=10, pady=4,
                  cursor='hand2',
                  command=self._lib_build_all).pack(side='right', padx=(0,6))
        tk.Button(top, text='+ Add folder',
                  font=('Segoe UI', 9), bg=ACCENT, fg=TEXT,
                  activebackground='#3a8eef', activeforeground=TEXT,
                  relief='flat', bd=0, padx=10, pady=4,
                  cursor='hand2',
                  command=self._lib_add_folder).pack(side='right', padx=(0, 6))

        # ── View toggle ──
        toggle_row = tk.Frame(parent, bg=BG)
        toggle_row.pack(fill='x', padx=24, pady=(0, 4))

        def _make_view_btn(text, mode):
            btn = tk.Label(toggle_row, text=text,
                           font=('Segoe UI', 9), bg=SURFACE2, fg=MUTED,
                           padx=12, pady=4, cursor='hand2',
                           relief='flat')
            btn.pack(side='left', padx=(0, 2))
            def _click(e, m=mode, b=btn):
                self._lib_view_mode.set(m)
                for w in toggle_row.winfo_children():
                    w.config(bg=SURFACE2, fg=MUTED)
                b.config(bg=ACCENT, fg=TEXT)
                self._lib_apply_filter()
            btn.bind('<Button-1>', _click)
            return btn

        self._lib_grid_btn = _make_view_btn('\u22ee\u22ee Grid', 'grid')
        self._lib_list_btn = _make_view_btn('\u2630  List',      'list')
        self._lib_grid_btn.config(bg=ACCENT, fg=TEXT)  # default active

        # ── Folder list ──
        self._lib_folder_frame = tk.Frame(parent, bg=SURFACE2,
                                           highlightbackground=BORDER,
                                           highlightthickness=1)
        self._lib_folder_frame.pack(fill='x', padx=24, pady=(0, 6))
        self._lib_render_folders()

        # ── Search + status bar ──
        ctrl = tk.Frame(parent, bg=BG)
        ctrl.pack(fill='x', padx=24, pady=(0, 6))
        self._lib_status_var = tk.StringVar(
            value='No games scanned yet — add a folder and click Scan')
        tk.Label(ctrl, textvariable=self._lib_status_var,
                 font=('Segoe UI', 8), bg=BG, fg=MUTED).pack(side='left')

        self._lib_search_var = tk.StringVar()
        self._lib_search_var.trace('w', lambda *a: self._lib_apply_filter())
        sf = tk.Frame(ctrl, bg=FIELD_BG,
                      highlightbackground=BORDER, highlightthickness=1)
        sf.pack(side='right')
        tk.Label(sf, text='\U0001f50d', font=('Segoe UI', 9),
                 bg=FIELD_BG, fg=MUTED).pack(side='left', padx=(6, 2))
        tk.Entry(sf, textvariable=self._lib_search_var,
                 font=('Segoe UI', 9), bg=FIELD_BG, fg=FIELD_FG,
                 insertbackground=FIELD_FG, relief='flat', bd=4,
                 width=22).pack(side='left')

        # ── Canvas ──
        grid_outer = tk.Frame(parent, bg=BG)
        grid_outer.pack(fill='both', expand=True, padx=24, pady=(0, 8))

        self._lib_canvas = tk.Canvas(grid_outer, bg=BG, highlightthickness=0)
        lib_sb = tk.Scrollbar(grid_outer, orient='vertical',
                               command=self._lib_canvas.yview,
                               bg=SURFACE2, troughcolor=BG)
        self._lib_canvas.configure(yscrollcommand=lib_sb.set)
        lib_sb.pack(side='right', fill='y')
        self._lib_canvas.pack(side='left', fill='both', expand=True)

        self._lib_grid_frame = tk.Frame(self._lib_canvas, bg=BG)
        self._lib_canvas.create_window((0, 0), window=self._lib_grid_frame,
                                        anchor='nw', tags='lgf')
        self._lib_grid_frame.bind('<Configure>', lambda e:
            self._lib_canvas.configure(
                scrollregion=self._lib_canvas.bbox('all')))
        self._lib_canvas.bind('<Configure>', lambda e: (
            self._lib_canvas.itemconfig('lgf', width=e.width),
            self._lib_apply_filter()))
        self._lib_canvas.bind('<MouseWheel>',
            lambda e: self._lib_canvas.yview_scroll(
                int(-1*(e.delta/120)), 'units'))

    # ── Library helpers ───────────────────────────────────────────────────────
    def _lib_render_folders(self):
        for w in self._lib_folder_frame.winfo_children():
            w.destroy()
        if not self._lib_folders:
            tk.Label(self._lib_folder_frame,
                     text='No scan folders added yet — click "+ Add folder"',
                     font=('Segoe UI', 8), bg=SURFACE2, fg=MUTED,
                     pady=6).pack(anchor='w', padx=10)
            return
        for i, folder in enumerate(self._lib_folders):
            row = tk.Frame(self._lib_folder_frame, bg=SURFACE2)
            row.pack(fill='x')
            tk.Label(row, text='\U0001f4c1  ' + folder,
                     font=('Segoe UI', 8), bg=SURFACE2, fg=MUTED,
                     anchor='w').pack(side='left', padx=10, pady=4,
                                      fill='x', expand=True)
            idx = i
            tk.Button(row, text='\u2715',
                      font=('Segoe UI', 8), bg=SURFACE2, fg=DANGER,
                      activebackground=SURFACE2, activeforeground=DANGER,
                      relief='flat', bd=0, padx=8, cursor='hand2',
                      command=lambda ix=idx: self._lib_remove_folder(ix)
                      ).pack(side='right')

    def _lib_add_folder(self):
        p = filedialog.askdirectory(title='Select folder containing PS5 game dumps')
        if not p:
            return
        p = os.path.normpath(p)
        if p not in self._lib_folders:
            self._lib_folders.append(p)
            self._settings['lib_folders'] = self._lib_folders
            save_settings(self._settings)
        self._lib_render_folders()

    def _lib_remove_folder(self, idx):
        if 0 <= idx < len(self._lib_folders):
            self._lib_folders.pop(idx)
            self._settings['lib_folders'] = self._lib_folders
            save_settings(self._settings)
            self._lib_render_folders()
            self._lib_apply_filter()

    def _lib_scan(self):
        if not self._lib_folders:
            messagebox.showwarning('No folders',
                'Add at least one scan folder first.')
            return
        self._lib_status_var.set('Scanning...')
        self._lib_games = []
        for w in self._lib_grid_frame.winfo_children():
            w.destroy()

        def worker():
            games = []
            for base in self._lib_folders:
                if not os.path.isdir(base):
                    continue
                try:
                    for entry in os.scandir(base):
                        if not entry.is_dir():
                            continue
                        folder = entry.path
                        # Quick check for eboot.bin anywhere in first 2 levels
                        has_eboot = os.path.isfile(
                            os.path.join(folder, 'eboot.bin'))
                        if not has_eboot:
                            # check one level down
                            try:
                                for sub in os.scandir(folder):
                                    if sub.is_dir():
                                        if os.path.isfile(
                                                os.path.join(sub.path, 'eboot.bin')):
                                            has_eboot = True
                                            break
                            except Exception:
                                pass
                        if not has_eboot:
                            continue
                        title, title_id, version = get_game_info(folder)
                        if not title_id:
                            m = re.search(
                                r'((?:PPSA|CUSA|PPLH)\d{5})',
                                os.path.basename(folder), re.IGNORECASE)
                            if m:
                                title_id = m.group(1).upper()
                        cover = self._load_cover_art(folder)
                        size  = self._get_folder_size(folder)
                        games.append({
                            'folder':   folder,
                            'title':    title or title_id or os.path.basename(folder),
                            'title_id': title_id or '',
                            'version':  version or '',
                            'cover':    cover,
                            'size':     size,
                        })
                except Exception:
                    pass
            games.sort(key=lambda g: g['title'].lower())
            self.after(0, self._lib_scan_done, games)

        threading.Thread(target=worker, daemon=True).start()

    def _lib_scan_done(self, games):
        self._lib_games = games
        self._lib_status_var.set(
            str(len(games)) + ' game(s) found across ' +
            str(len(self._lib_folders)) + ' folder(s)')
        self._lib_apply_filter()

    def _lib_apply_filter(self):
        query = self._lib_search_var.get().lower().strip() \
            if hasattr(self, '_lib_search_var') else ''
        if query:
            filtered = [g for g in self._lib_games
                        if query in g['title'].lower()
                        or query in g['title_id'].lower()]
        else:
            filtered = self._lib_games
        if hasattr(self, '_lib_view_mode') and self._lib_view_mode.get() == 'list':
            self._lib_render_list(filtered)
        else:
            self._lib_render_grid(filtered)

    def _lib_render_list(self, games=None):
        if games is None:
            games = self._lib_games
        for w in self._lib_grid_frame.winfo_children():
            w.destroy()
        if not games:
            tk.Label(self._lib_grid_frame, text='No games found',
                     font=('Segoe UI', 11), bg=BG, fg=MUTED,
                     pady=40).pack()
            return
        for i, game in enumerate(games):
            row_bg = QUEUE_ODD if i % 2 == 0 else QUEUE_EVEN
            row = tk.Frame(self._lib_grid_frame, bg=row_bg, cursor='hand2')
            row.pack(fill='x')

            # Cover thumbnail (small)
            thumb_lbl = tk.Label(row, bg=row_bg, text='\U0001f3ae',
                                  font=('Segoe UI', 14), fg='#333333',
                                  width=4, height=2)
            thumb_lbl.pack(side='left', padx=(8, 6), pady=4)
            if game.get('cover'):
                def load_thumb(path=game['cover'], lbl=thumb_lbl, bg=row_bg):
                    try:
                        from PIL import Image, ImageTk
                        img = Image.open(path).resize((40, 40), Image.LANCZOS)
                        photo = ImageTk.PhotoImage(img)
                        lbl.config(image=photo, text='', width=40, height=40)
                        lbl.image = photo
                    except Exception:
                        pass
                threading.Thread(target=load_thumb, daemon=True).start()

            # Title + meta
            info_col = tk.Frame(row, bg=row_bg)
            info_col.pack(side='left', fill='x', expand=True, pady=6)
            tk.Label(info_col, text=game['title'],
                     font=('Segoe UI', 9, 'bold'), bg=row_bg, fg=TEXT,
                     anchor='w').pack(anchor='w')
            meta = game['title_id']
            if game['version']:
                meta += '  v' + game['version']
            tk.Label(info_col, text=meta,
                     font=('Segoe UI', 8), bg=row_bg, fg=INFO_FG,
                     anchor='w').pack(anchor='w')

            # Size
            if game['size'] > 0:
                sz = ('%.1f GB' % (game['size'] / 1024**3)
                      if game['size'] >= 1024**3
                      else '%d MB' % (game['size'] // 1024**2))
                tk.Label(row, text=sz,
                         font=('Segoe UI', 8), bg=row_bg, fg='#444444',
                         anchor='e').pack(side='right', padx=12)

            # Bind click / right-click to context menu
            def _bind_ctx(widgets, g=game):
                for w in widgets:
                    w.bind('<Button-1>',   lambda e, gg=g: self._lib_context_menu(e, gg))
                    w.bind('<Button-3>',   lambda e, gg=g: self._lib_context_menu(e, gg))
            _bind_ctx([row, thumb_lbl, info_col] + list(info_col.winfo_children()))

    def _lib_render_grid(self, games=None):
        if games is None:
            games = self._lib_games
        for w in self._lib_grid_frame.winfo_children():
            w.destroy()
        if not games:
            tk.Label(self._lib_grid_frame,
                     text='No games found',
                     font=('Segoe UI', 11), bg=BG, fg=MUTED,
                     pady=40).pack()
            return
        try:
            canvas_w = self._lib_canvas.winfo_width()
        except Exception:
            canvas_w = 700
        card_w = 160
        cols   = max(1, canvas_w // (card_w + 12))
        for i, game in enumerate(games):
            row = i // cols
            col = i % cols
            self._lib_make_card(self._lib_grid_frame, game, row, col)

    def _lib_make_card(self, parent, game, row, col):
        card = tk.Frame(parent, bg='#141414',
                        highlightbackground='#2a2a2a',
                        highlightthickness=1,
                        cursor='hand2')
        card.grid(row=row, column=col, padx=6, pady=6, sticky='nsew')
        parent.grid_columnconfigure(col, weight=1)

        # Cover art
        cover_frame = tk.Frame(card, bg='#0a0a0a',
                               width=148, height=148)
        cover_frame.pack(padx=6, pady=(6, 0))
        cover_frame.pack_propagate(False)

        cover_lbl = tk.Label(cover_frame, bg='#0a0a0a',
                             text='\U0001f3ae',
                             font=('Segoe UI', 28), fg='#2a2a2a')
        cover_lbl.pack(expand=True)

        # Load cover art async
        if game.get('cover'):
            def load_art(path=game['cover'], lbl=cover_lbl):
                try:
                    from PIL import Image, ImageTk
                    img = Image.open(path).resize((148, 148), Image.LANCZOS)
                    photo = ImageTk.PhotoImage(img)
                    lbl.config(image=photo, text='')
                    lbl.image = photo  # keep reference
                except Exception:
                    pass
            threading.Thread(target=load_art, daemon=True).start()

        # Title
        title_text = game['title']
        if len(title_text) > 22:
            title_text = title_text[:20] + '…'
        tk.Label(card, text=title_text,
                 font=('Segoe UI', 8, 'bold'), bg='#141414', fg=TEXT,
                 wraplength=148, justify='center').pack(padx=6, pady=(4, 0))

        # Meta
        meta = game['title_id']
        if game['version']:
            meta += '  v' + game['version']
        tk.Label(card, text=meta,
                 font=('Segoe UI', 7), bg='#141414', fg=INFO_FG).pack()

        # Size
        if game['size'] > 0:
            sz = ('%.1f GB' % (game['size'] / 1024**3)
                  if game['size'] >= 1024**3
                  else '%d MB' % (game['size'] // 1024**2))
            tk.Label(card, text=sz,
                     font=('Segoe UI', 7), bg='#141414', fg='#444444').pack(pady=(0, 6))

        # Click / right-click → context menu
        def _bind_all(widgets, g=game):
            for w in widgets:
                w.bind('<Button-1>', lambda e, gg=g: self._lib_context_menu(e, gg))
                w.bind('<Button-3>', lambda e, gg=g: self._lib_context_menu(e, gg))
        _bind_all([card, cover_lbl, cover_frame] + list(card.winfo_children()))

        # Hover highlight
        def _enter(e):
            card.config(highlightbackground=ACCENT)
        def _leave(e):
            card.config(highlightbackground='#2a2a2a')
        card.bind('<Enter>', _enter)
        card.bind('<Leave>', _leave)
        for w in card.winfo_children():
            w.bind('<Enter>', _enter)
            w.bind('<Leave>', _leave)

    def _lib_context_menu(self, event, game):
        menu = tk.Menu(self, tearoff=0, bg=SURFACE2, fg=TEXT,
                       activebackground=ACCENT, activeforeground=TEXT,
                       font=('Segoe UI', 9))
        menu.add_command(label='\U0001f3ae  ' + game['title'],
                         state='disabled', font=('Segoe UI', 9, 'bold'))
        menu.add_separator()
        menu.add_command(label='\u2795  Add to Build Queue',
                         command=lambda: self._lib_quick_add(game))
        menu.add_command(label='\U0001f4cb  View details',
                         command=lambda: self._lib_show_detail(game))
        menu.add_separator()
        menu.add_command(label='\U0001f4c2  Open folder',
                         command=lambda: subprocess.Popen(
                             'explorer "' + game['folder'] + '"', shell=True))
        menu.add_command(label='\U0001f5d1  Remove from library',
                         command=lambda: self._lib_remove_game(game))
        menu.tk_popup(event.x_root, event.y_root)

    def _lib_quick_add(self, game):
        odir = self.output_dir.get().strip()
        if not odir:
            # Ask for output dir
            odir = filedialog.askdirectory(title='Select output directory for ' + game['title'])
            if not odir:
                return
            odir = odir.replace('/', '\\')
            self.output_dir.set(odir)
            self._settings['output_dir'] = odir
            save_settings(self._settings)
        title, title_id, version = game['title'], game['title_id'], game['version']
        name = build_exfat_name(title or None, title_id or None, version or None)
        item = QueueItem(game['folder'], odir, name, title, title_id, version)
        self._queue.append(item)
        self._render_queue()
        self._switch_tab('build')
        self._set_status('Added to queue: ' + (title or title_id), SUCCESS)

    def _lib_remove_game(self, game):
        self._lib_games = [g for g in self._lib_games if g['folder'] != game['folder']]
        self._lib_apply_filter()
        self._lib_status_var.set(str(len(self._lib_games)) + ' game(s)')

    def _lib_show_detail(self, game):
        win = tk.Toplevel(self)
        win.title(game['title'])
        win.geometry('500x380')
        win.configure(bg=BG)
        win.resizable(False, False)
        win.transient(self)

        body = tk.Frame(win, bg=BG)
        body.pack(fill='both', expand=True, padx=24, pady=20)

        # Cover + info side by side
        top = tk.Frame(body, bg=BG)
        top.pack(fill='x', pady=(0, 16))

        # Cover
        cover_frame = tk.Frame(top, bg='#0a0a0a',
                               width=120, height=120)
        cover_frame.pack(side='left', padx=(0, 20))
        cover_frame.pack_propagate(False)
        cover_lbl = tk.Label(cover_frame, bg='#0a0a0a',
                             text='\U0001f3ae', font=('Segoe UI', 30),
                             fg='#2a2a2a')
        cover_lbl.pack(expand=True)

        if game.get('cover'):
            def load(path=game['cover'], lbl=cover_lbl):
                try:
                    from PIL import Image, ImageTk
                    img = Image.open(path).resize((120, 120), Image.LANCZOS)
                    photo = ImageTk.PhotoImage(img)
                    lbl.config(image=photo, text='')
                    lbl.image = photo
                except Exception:
                    pass
            threading.Thread(target=load, daemon=True).start()

        # Info
        info = tk.Frame(top, bg=BG)
        info.pack(side='left', fill='both', expand=True)

        tk.Label(info, text=game['title'],
                 font=('Segoe UI', 12, 'bold'), bg=BG, fg=TEXT,
                 wraplength=300, justify='left', anchor='w').pack(anchor='w')
        tk.Label(info, text=game['title_id'],
                 font=('Consolas', 9), bg=BG, fg=INFO_FG,
                 anchor='w').pack(anchor='w', pady=(4, 0))
        if game['version']:
            tk.Label(info, text='Version ' + game['version'],
                     font=('Segoe UI', 9), bg=BG, fg=MUTED,
                     anchor='w').pack(anchor='w')
        if game['size'] > 0:
            sz = ('%.2f GB' % (game['size'] / 1024**3)
                  if game['size'] >= 1024**3
                  else '%d MB' % (game['size'] // 1024**2))
            tk.Label(info, text=sz + ' estimated image size',
                     font=('Segoe UI', 8), bg=BG, fg='#555555',
                     anchor='w').pack(anchor='w', pady=(4, 0))

        # Folder path
        tk.Label(body, text=game['folder'],
                 font=('Consolas', 8), bg=BG, fg='#3a3a3a',
                 wraplength=440, justify='left', anchor='w').pack(anchor='w')

        tk.Frame(body, bg=BORDER, height=1).pack(fill='x', pady=16)

        # Output directory for queue
        out_row = tk.Frame(body, bg=BG)
        out_row.pack(fill='x', pady=(0, 12))
        tk.Label(out_row, text='Output directory:',
                 font=('Segoe UI', 9), bg=BG, fg=MUTED).pack(side='left')
        out_var = tk.StringVar(value=self.output_dir.get())
        out_ef = tk.Frame(out_row, bg=FIELD_BG,
                          highlightbackground=BORDER, highlightthickness=1)
        out_ef.pack(side='left', fill='x', expand=True, padx=(8, 6))
        tk.Entry(out_ef, textvariable=out_var,
                 font=('Consolas', 9), bg=FIELD_BG, fg=FIELD_FG,
                 insertbackground=FIELD_FG, relief='flat', bd=4).pack(fill='x')
        def _browse_out():
            p = filedialog.askdirectory(title='Select output directory')
            if p:
                out_var.set(p.replace('/', '\\'))
        tk.Button(out_row, text='…', font=('Segoe UI', 9),
                  bg=SURFACE2, fg=TEXT, relief='flat', bd=0,
                  padx=8, pady=3, cursor='hand2',
                  command=_browse_out).pack(side='left')

        # Action buttons
        btn_row = tk.Frame(body, bg=BG)
        btn_row.pack(fill='x')

        def _add_to_queue():
            odir = out_var.get().strip()
            if not odir:
                messagebox.showwarning('No output directory',
                    'Please select an output directory.', parent=win)
                return
            title, title_id, version = get_game_info(game['folder'])
            if not title_id:
                m = re.search(r'((?:PPSA|CUSA|PPLH)\d{5})',
                              os.path.basename(game['folder']), re.IGNORECASE)
                if m:
                    title_id = m.group(1).upper()
            name = build_exfat_name(title, title_id, version)
            item = QueueItem(game['folder'], odir, name, title, title_id, version)
            self._queue.append(item)
            self._render_queue()
            self.output_dir.set(odir)
            self._settings['output_dir'] = odir
            save_settings(self._settings)
            win.destroy()
            self._switch_tab('build')
            self._set_status('Added to queue: ' + (title or title_id or ''), SUCCESS)

        tk.Button(btn_row, text='\u2795  Add to Build Queue',
                  font=('Segoe UI', 10, 'bold'),
                  bg=SUCCESS, fg=TEXT,
                  activebackground='#3d9140', activeforeground=TEXT,
                  relief='flat', bd=0, padx=18, pady=8,
                  cursor='hand2', command=_add_to_queue).pack(side='left')

        tk.Button(btn_row, text='\U0001f4c2  Open folder',
                  font=('Segoe UI', 9),
                  bg=SURFACE2, fg=TEXT,
                  activebackground=BORDER, activeforeground=TEXT,
                  relief='flat', bd=0, padx=12, pady=8,
                  cursor='hand2',
                  command=lambda: subprocess.Popen(
                      'explorer "' + game['folder'] + '"', shell=True)
                  ).pack(side='left', padx=(8, 0))

        tk.Button(btn_row, text='Close',
                  font=('Segoe UI', 9),
                  bg=SURFACE2, fg=MUTED,
                  activebackground=BORDER, activeforeground=TEXT,
                  relief='flat', bd=0, padx=12, pady=8,
                  cursor='hand2', command=win.destroy).pack(side='right')

    def _build_ftpbr_tab(self, parent):
        """Full PS5 FTP file browser as an embedded tab."""
        # State
        self._br_path      = tk.StringVar(value=self._settings.get('ftp_path', '/data/etaHEN/games/'))
        self._br_status    = tk.StringVar(value='Not connected — click Connect')
        self._br_free      = tk.StringVar(value='')
        self._br_sel_info  = tk.StringVar(value='')
        self._br_ftp       = [None]
        self._br_entries   = []   # (name, is_dir, size)
        self._br_clipboard = [None]  # ('move', [(path, is_dir), ...])

        # ── Header row ──
        hdr = tk.Frame(parent, bg=BG)
        hdr.pack(fill='x', padx=24, pady=(12, 4))
        tk.Label(hdr, text='PS5 FTP Browser',
                 font=('Segoe UI', 13, 'bold'), bg=BG, fg=TEXT).pack(side='left')
        tk.Label(hdr, textvariable=self._br_free,
                 font=('Segoe UI', 8), bg=BG, fg=INFO_FG).pack(side='right')
        tk.Label(hdr, textvariable=self._br_status,
                 font=('Segoe UI', 8), bg=BG, fg=MUTED).pack(side='right', padx=(0,16))

        # ── Connection bar ──
        conn = tk.Frame(parent, bg=BG)
        conn.pack(fill='x', padx=24, pady=(0,4))
        tk.Button(conn, text='\U0001f517 Connect',
                  font=('Segoe UI', 9), bg=ACCENT, fg=TEXT,
                  activebackground='#3a8eef', relief='flat', bd=0,
                  padx=10, pady=4, cursor='hand2',
                  command=self._br_connect).pack(side='left')
        tk.Button(conn, text='\U0001f534 Disconnect',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=DANGER,
                  activebackground=BORDER, relief='flat', bd=0,
                  padx=10, pady=4, cursor='hand2',
                  command=self._br_disconnect).pack(side='left', padx=(6,0))
        tk.Label(conn, text='IP from Settings',
                 font=('Segoe UI', 8), bg=BG, fg='#333333').pack(side='left', padx=(12,0))

        # ── Path bar ──
        path_row = tk.Frame(parent, bg=BG)
        path_row.pack(fill='x', padx=24, pady=(0,4))
        tk.Button(path_row, text='\u2191 Up',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=TEXT,
                  relief='flat', bd=0, padx=8, pady=4, cursor='hand2',
                  command=self._br_go_up).pack(side='left', padx=(0,6))
        pf = tk.Frame(path_row, bg=FIELD_BG,
                      highlightbackground=BORDER, highlightthickness=1)
        pf.pack(side='left', fill='x', expand=True)
        pe = tk.Entry(pf, textvariable=self._br_path,
                      font=('Consolas', 9), bg=FIELD_BG, fg=FIELD_FG,
                      insertbackground=FIELD_FG, relief='flat', bd=4)
        pe.pack(fill='x')
        pe.bind('<Return>', lambda e: self._br_load(self._br_path.get()))
        tk.Button(path_row, text='Go', font=('Segoe UI', 9),
                  bg=SURFACE2, fg=TEXT, relief='flat', bd=0,
                  padx=10, pady=4, cursor='hand2',
                  command=lambda: self._br_load(self._br_path.get())
                  ).pack(side='left', padx=(6,0))
        tk.Button(path_row, text='\U0001f504', font=('Segoe UI', 9),
                  bg=SURFACE2, fg=TEXT, relief='flat', bd=0,
                  padx=8, pady=4, cursor='hand2',
                  command=lambda: self._br_load(self._br_path.get())
                  ).pack(side='left', padx=(4,0))

        # ── Toolbar ──
        toolbar = tk.Frame(parent, bg=BG)
        toolbar.pack(fill='x', padx=24, pady=(0,4))

        def tbtn(text, cmd, fg=TEXT):
            return tk.Button(toolbar, text=text, font=('Segoe UI', 8),
                      bg=SURFACE2, fg=fg, activebackground=BORDER,
                      activeforeground=TEXT, relief='flat', bd=0,
                      padx=8, pady=3, cursor='hand2', command=cmd)

        tbtn('\U0001f4c2 New folder',  self._br_new_folder).pack(side='left', padx=(0,3))
        tk.Frame(toolbar, bg=BORDER, width=1).pack(side='left', fill='y', padx=4)
        tbtn('\u2702 Rename',          self._br_rename).pack(side='left', padx=(0,3))
        tbtn('\u2702\ufe0f Cut',       self._br_cut).pack(side='left', padx=(0,3))
        tbtn('\U0001f4cb Paste',       self._br_paste).pack(side='left', padx=(0,3))
        tbtn('\u27a1 Move to...',      self._br_move_to).pack(side='left', padx=(0,3))
        tk.Frame(toolbar, bg=BORDER, width=1).pack(side='left', fill='y', padx=4)
        tbtn('\u2935 Download',        self._br_download).pack(side='left', padx=(0,3))
        tbtn('\u2191 Upload here',     self._br_upload_here).pack(side='left', padx=(0,3))
        tk.Frame(toolbar, bg=BORDER, width=1).pack(side='left', fill='y', padx=4)
        tbtn('\U0001f5d1 Delete',      self._br_delete, fg=DANGER).pack(side='left', padx=(0,3))

        # ── File list ──
        list_outer = tk.Frame(parent, bg=SURFACE2,
                              highlightbackground=BORDER, highlightthickness=1)
        list_outer.pack(fill='both', expand=True, padx=24, pady=(0,4))

        col_hdr = tk.Frame(list_outer, bg='#0f0f0f')
        col_hdr.pack(fill='x')
        for col_text, col_w in [('Name', 50), ('Size', 12), ('Type', 8)]:
            tk.Label(col_hdr, text=col_text, font=('Segoe UI', 8, 'bold'),
                     bg='#0f0f0f', fg=MUTED, anchor='w',
                     width=col_w).pack(side='left', padx=(8,0), pady=3)

        self._br_listbox = tk.Listbox(list_outer, font=('Consolas', 9),
                                       bg=SURFACE2, fg=TEXT,
                                       selectbackground=ACCENT,
                                       selectforeground='#ffffff',
                                       activestyle='none', relief='flat',
                                       bd=6, selectmode='extended')
        br_sb = tk.Scrollbar(list_outer, command=self._br_listbox.yview,
                              bg=SURFACE2, troughcolor=BG)
        self._br_listbox.configure(yscrollcommand=br_sb.set)
        br_sb.pack(side='right', fill='y')
        self._br_listbox.pack(fill='both', expand=True)
        self._br_listbox.bind('<Double-Button-1>', lambda e: self._br_nav())
        self._br_listbox.bind('<Button-3>',        lambda e: self._br_ctx(e))
        self._br_listbox.bind('<<ListboxSelect>>', lambda e: self._br_update_sel())

        # ── Status bar ──
        stat = tk.Frame(parent, bg='#0a0a0a')
        stat.pack(fill='x', padx=24, pady=(0,6))
        tk.Label(stat, textvariable=self._br_sel_info,
                 font=('Segoe UI', 8), bg='#0a0a0a', fg=MUTED,
                 anchor='w').pack(side='left', padx=8, pady=3)

        # Keyboard shortcuts
        parent.bind('<Delete>',    lambda e: self._br_delete())
        parent.bind('<F2>',        lambda e: self._br_rename())
        parent.bind('<F5>',        lambda e: self._br_load(self._br_path.get()))
        parent.bind('<BackSpace>', lambda e: self._br_go_up())

    # ── PS5 Browser methods ───────────────────────────────────────────────────
    def _br_connect(self):
        self._br_status.set('Connecting...')
        def worker():
            try:
                ftp = self._ftp_connect()
                self._br_ftp[0] = ftp
                self.after(0, self._br_status.set, 'Connected \u2713')
                self.after(0, self._br_load, self._br_path.get())
            except Exception as e:
                self.after(0, self._br_status.set, 'Failed: ' + str(e))
        threading.Thread(target=worker, daemon=True).start()

    def _br_disconnect(self):
        try:
            if self._br_ftp[0]:
                self._br_ftp[0].quit()
        except Exception:
            pass
        self._br_ftp[0] = None
        self._br_listbox.delete(0, 'end')
        self._br_entries = []
        self._br_status.set('Disconnected')
        self._br_free.set('')

    def _br_ensure(self):
        try:
            if self._br_ftp[0]:
                self._br_ftp[0].voidcmd('NOOP')
                return self._br_ftp[0]
        except Exception:
            pass
        ftp = self._ftp_connect()
        self._br_ftp[0] = ftp
        return ftp

    def _br_load(self, p):
        p = p or '/'
        self._br_status.set('Loading...')
        self._br_listbox.delete(0, 'end')
        self._br_entries = []
        def worker():
            try:
                ftp   = self._br_ensure()
                lines = []
                ftp.retrlines('LIST ' + p, lines.append)
                # Try to get free space
                free_str = ''
                try:
                    resp = ftp.sendcmd('SITE AVAIL')
                    free_str = 'Free: ' + resp
                except Exception:
                    pass
                self.after(0, self._br_show, lines, p, free_str)
            except Exception as e:
                self.after(0, self._br_status.set, 'Error: ' + str(e))
        threading.Thread(target=worker, daemon=True).start()

    def _br_show(self, lines, p, free_str):
        self._br_listbox.delete(0, 'end')
        self._br_entries = []
        self._br_path.set(p)
        if free_str:
            self._br_free.set(free_str)
        parsed = []
        for line in lines:
            parts = line.split()
            if len(parts) < 9: continue
            name   = ' '.join(parts[8:])
            is_dir = line.startswith('d')
            try: sz = int(parts[4])
            except: sz = 0
            if name not in ('.', '..'):
                parsed.append((name, is_dir, sz))
        parsed.sort(key=lambda x: (not x[1], x[0].lower()))
        if p != '/':
            self._br_entries.append(('..', True, 0))
            self._br_listbox.insert('end', '\U0001f4c2  ..')
        for name, is_dir, sz in parsed:
            self._br_entries.append((name, is_dir, sz))
            icon = '\U0001f4c1  ' if is_dir else '\U0001f4be  '
            sz_str = self._br_fmt(sz, is_dir)
            self._br_listbox.insert('end',
                '%-3s%-42s %12s' % (icon, name, sz_str))
        self._br_status.set('%d items in %s' % (len(parsed), p))
        self._br_sel_info.set('')

    def _br_fmt(self, sz, is_dir):
        if is_dir: return '<DIR>'
        if sz >= 1024**3: return '%.2f GB' % (sz/1024**3)
        if sz >= 1024**2: return '%.1f MB' % (sz/1024**2)
        if sz >= 1024:    return '%d KB'   % (sz//1024)
        return '%d B' % sz

    def _br_cur(self):
        return self._br_path.get().rstrip('/')

    def _br_selected(self):
        result = []
        for i in self._br_listbox.curselection():
            if i < len(self._br_entries):
                name, is_dir, sz = self._br_entries[i]
                if name != '..':
                    result.append((name, is_dir, sz))
        return result

    def _br_update_sel(self):
        sel = self._br_selected()
        if not sel:
            self._br_sel_info.set('')
            return
        total = sum(sz for _, is_dir, sz in sel if not is_dir)
        self._br_sel_info.set(
            '%d selected' % len(sel) +
            ('  —  ' + self._br_fmt(total, False) if total else ''))

    def _br_nav(self):
        sel = self._br_listbox.curselection()
        if not sel or sel[0] >= len(self._br_entries): return
        name, is_dir, _ = self._br_entries[sel[0]]
        if not is_dir: return
        if name == '..':
            self._br_go_up()
        else:
            self._br_load(self._br_cur() + '/' + name)

    def _br_go_up(self):
        cur = self._br_cur()
        if not cur: return
        parent = '/'.join(cur.split('/')[:-1]) or '/'
        self._br_load(parent)

    def _br_new_folder(self):
        name = simpledialog.askstring('New folder', 'Folder name:', parent=self)
        if not name: return
        remote = self._br_cur() + '/' + name
        def worker():
            try:
                ftp = self._br_ensure()
                ftp.mkd(remote)
                self.after(0, self._br_load, self._br_path.get())
            except Exception as e:
                self.after(0, messagebox.showerror, 'Error', str(e))
        threading.Thread(target=worker, daemon=True).start()

    def _br_rename(self):
        sel = self._br_selected()
        if not sel:
            messagebox.showwarning('No selection', 'Select a file or folder to rename.')
            return
        if len(sel) > 1:
            messagebox.showwarning('Too many', 'Select only one item to rename.')
            return
        name, _, _ = sel[0]
        new_name = simpledialog.askstring('Rename', 'New name:',
                                           initialvalue=name, parent=self)
        if not new_name or new_name == name: return
        old_p = self._br_cur() + '/' + name
        new_p = self._br_cur() + '/' + new_name
        def worker():
            try:
                ftp = self._br_ensure()
                ftp.rename(old_p, new_p)
                self.after(0, self._br_load, self._br_path.get())
                self.after(0, self._br_status.set, 'Renamed: ' + name + ' \u2192 ' + new_name)
            except Exception as e:
                self.after(0, messagebox.showerror, 'Rename failed', str(e))
        threading.Thread(target=worker, daemon=True).start()

    def _br_cut(self):
        sel = self._br_selected()
        if not sel:
            messagebox.showwarning('No selection', 'Select items to move.')
            return
        paths = [(self._br_cur() + '/' + name, is_dir) for name, is_dir, _ in sel]
        self._br_clipboard[0] = ('move', paths)
        self._br_status.set(
            str(len(paths)) + ' item(s) cut — navigate to destination and Paste')

    def _br_paste(self):
        if not self._br_clipboard[0]:
            messagebox.showwarning('Nothing to paste', 'Cut some files first.')
            return
        action, items = self._br_clipboard[0]
        dest = self._br_cur()
        def worker():
            errors = []
            ftp = self._br_ensure()
            for src_path, _ in items:
                name     = src_path.split('/')[-1]
                dst_path = dest + '/' + name
                try:
                    ftp.rename(src_path, dst_path)
                except Exception as e:
                    errors.append(name + ': ' + str(e))
            self._br_clipboard[0] = None
            self.after(0, self._br_load, self._br_path.get())
            if errors:
                self.after(0, messagebox.showerror, 'Move failed', '\n'.join(errors))
            else:
                self.after(0, self._br_status.set,
                           str(len(items)) + ' item(s) moved to ' + dest)
        threading.Thread(target=worker, daemon=True).start()

    def _br_move_to(self):
        """Move selected items to a typed destination path."""
        sel = self._br_selected()
        if not sel:
            messagebox.showwarning('No selection', 'Select items to move.')
            return
        dest = simpledialog.askstring(
            'Move to', 'Destination path on PS5:',
            initialvalue=self._br_cur(), parent=self)
        if not dest: return
        dest = dest.rstrip('/')
        def worker():
            errors = []
            ftp = self._br_ensure()
            # Ensure destination exists
            parts = dest.strip('/').split('/')
            cur = ''
            for part in parts:
                cur += '/' + part
                try: ftp.mkd(cur)
                except Exception: pass
            for name, is_dir, _ in sel:
                src_path = self._br_cur() + '/' + name
                dst_path = dest + '/' + name
                try:
                    ftp.rename(src_path, dst_path)
                except Exception as e:
                    errors.append(name + ': ' + str(e))
            self.after(0, self._br_load, self._br_path.get())
            if errors:
                self.after(0, messagebox.showerror, 'Move failed', '\n'.join(errors))
            else:
                self.after(0, self._br_status.set,
                           str(len(sel)) + ' item(s) moved to ' + dest)
        threading.Thread(target=worker, daemon=True).start()

    def _br_delete(self):
        sel = self._br_selected()
        if not sel:
            messagebox.showwarning('No selection', 'Select files or folders to delete.')
            return
        names = [name for name, _, _ in sel]
        if not messagebox.askyesno('Delete from PS5',
                'Permanently delete %d item(s) from PS5?\n\n%s%s' % (
                    len(names), '\n'.join(names[:8]),
                    '\n...' if len(names) > 8 else '')):
            return
        def _rmd(ftp, path):
            lines = []
            try: ftp.retrlines('LIST ' + path, lines.append)
            except Exception: pass
            for line in lines:
                parts = line.split()
                if len(parts) < 9: continue
                n = ' '.join(parts[8:])
                if n in ('.', '..'): continue
                child = path + '/' + n
                if line.startswith('d'): _rmd(ftp, child)
                else:
                    try: ftp.delete(child)
                    except Exception: pass
            try: ftp.rmd(path)
            except Exception: pass
        def worker():
            errors = []
            ftp = self._br_ensure()
            for name, is_dir, _ in sel:
                remote = self._br_cur() + '/' + name
                try:
                    if is_dir: _rmd(ftp, remote)
                    else:      ftp.delete(remote)
                except Exception as e:
                    errors.append(name + ': ' + str(e))
            self.after(0, self._br_load, self._br_path.get())
            if errors:
                self.after(0, messagebox.showerror,
                           'Some deletions failed', '\n'.join(errors))
            else:
                self.after(0, self._br_status.set,
                           'Deleted ' + str(len(sel)) + ' item(s)')
        threading.Thread(target=worker, daemon=True).start()

    def _br_download(self):
        sel = self._br_selected()
        files_only = [(n, sz) for n, is_dir, sz in sel if not is_dir]
        if not files_only:
            messagebox.showwarning('Files only', 'Select files (not folders) to download.')
            return
        save_dir = filedialog.askdirectory(title='Save downloaded files to...')
        if not save_dir: return
        self._br_status.set('Downloading %d file(s)...' % len(files_only))
        def worker():
            errors = []
            ftp = self._br_ensure()
            for name, sz in files_only:
                remote = self._br_cur() + '/' + name
                local  = os.path.join(save_dir, name)
                try:
                    with open(local, 'wb') as f:
                        ftp.retrbinary('RETR ' + remote, f.write)
                except Exception as e:
                    errors.append(name + ': ' + str(e))
            if errors:
                self.after(0, messagebox.showerror, 'Download errors', '\n'.join(errors))
                self.after(0, self._br_status.set, 'Download finished with errors')
            else:
                self.after(0, self._br_status.set,
                           '%d file(s) downloaded to %s' % (len(files_only), save_dir))
        threading.Thread(target=worker, daemon=True).start()

    def _br_upload_here(self):
        """Upload a file or folder to the current directory."""
        choice = messagebox.askyesnocancel(
            'Upload', 'What do you want to upload?\n\nYes = File\nNo = Folder\nCancel = Abort')
        if choice is None: return
        if choice:
            local = filedialog.askopenfilename(title='Select file to upload')
        else:
            local = filedialog.askdirectory(title='Select folder to upload')
        if not local: return
        # Reuse the FTP upload tab logic with current remote path
        self._ftptab_local_var.set(local.replace('/', '\\'))
        self._ftptab_remote_var.set(self._br_path.get())
        self._ftptab_is_folder.set(not choice)
        self._switch_tab('ftp')
        self._ftptab_start_upload()

    def _br_ctx(self, event):
        sel_idx = self._br_listbox.nearest(event.y)
        self._br_listbox.selection_clear(0, 'end')
        self._br_listbox.selection_set(sel_idx)
        if sel_idx >= len(self._br_entries): return
        name, is_dir, sz = self._br_entries[sel_idx]
        if name == '..': return
        remote = self._br_cur() + '/' + name
        menu = tk.Menu(self, tearoff=0, bg=SURFACE2, fg=TEXT,
                       activebackground=ACCENT, activeforeground=TEXT,
                       font=('Segoe UI', 9))
        menu.add_command(label=name, state='disabled',
                         font=('Segoe UI', 9, 'bold'))
        menu.add_separator()
        if is_dir:
            menu.add_command(label='\U0001f4c2  Open folder',
                             command=lambda: self._br_load(remote))
        else:
            menu.add_command(label='\u2935  Download',
                             command=self._br_download)
        menu.add_command(label='\u2702  Rename',       command=self._br_rename)
        menu.add_command(label='\u2702\ufe0f  Cut',    command=self._br_cut)
        menu.add_command(label='\u27a1  Move to...',   command=self._br_move_to)
        menu.add_separator()
        menu.add_command(label='\U0001f5d1  Delete',   command=self._br_delete)
        menu.tk_popup(event.x_root, event.y_root)

    def _build_files_tab(self, parent):
        self._fm_image_var   = tk.StringVar()
        self._fm_drive       = None   # mounted drive letter e.g. 'E:'
        self._fm_osf         = None   # path to osfmount.com
        self._fm_current_dir = None   # current path on mounted drive

        # ── Top bar ──
        top = tk.Frame(parent, bg=BG)
        top.pack(fill='x', padx=24, pady=(12, 6))
        tk.Label(top, text='exFAT File Manager',
                 font=('Segoe UI', 13, 'bold'), bg=BG, fg=TEXT).pack(side='left', anchor='w')

        # Mount / dismount controls
        ctrl = tk.Frame(parent, bg=BG)
        ctrl.pack(fill='x', padx=24, pady=(0, 8))

        ef_outer = tk.Frame(ctrl, bg=FIELD_BG,
                            highlightbackground=BORDER, highlightthickness=1)
        ef_outer.pack(side='left', fill='x', expand=True)
        tk.Entry(ef_outer, textvariable=self._fm_image_var,
                 font=('Consolas', 9), bg=FIELD_BG, fg=FIELD_FG,
                 insertbackground=FIELD_FG,
                 selectbackground=FIELD_SEL_BG, selectforeground=FIELD_SEL_FG,
                 relief='flat', bd=5, state='readonly').pack(fill='x')

        tk.Button(ctrl, text='Browse',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=TEXT,
                  activebackground=BORDER, activeforeground=TEXT,
                  relief='flat', bd=0, padx=12, pady=5,
                  cursor='hand2',
                  command=self._fm_browse).pack(side='left', padx=(6, 0))

        self._fm_mount_btn = tk.Button(ctrl, text='\U0001f517 Mount',
                  font=('Segoe UI', 9, 'bold'), bg=ACCENT, fg=TEXT,
                  activebackground='#3a8eef', activeforeground=TEXT,
                  relief='flat', bd=0, padx=12, pady=5,
                  cursor='hand2',
                  command=self._fm_mount)
        self._fm_mount_btn.pack(side='left', padx=(6, 0))

        self._fm_dismount_btn = tk.Button(ctrl, text='\U0001f513 Dismount',
                  font=('Segoe UI', 9, 'bold'), bg=DANGER, fg=TEXT,
                  activebackground='#c0392b', activeforeground=TEXT,
                  relief='flat', bd=0, padx=12, pady=5,
                  cursor='hand2', state='disabled',
                  command=self._fm_dismount)
        self._fm_dismount_btn.pack(side='left', padx=(6, 0))

        self._fm_status_var = tk.StringVar(value='No image mounted')
        tk.Label(ctrl, textvariable=self._fm_status_var,
                 font=('Segoe UI', 8), bg=BG, fg=MUTED).pack(side='left', padx=(12, 0))

        tk.Frame(parent, bg=BORDER, height=1).pack(fill='x', padx=24)

        # ── Path bar ──
        path_row = tk.Frame(parent, bg=BG)
        path_row.pack(fill='x', padx=24, pady=(6, 4))
        tk.Label(path_row, text='Path:', font=('Segoe UI', 9),
                 bg=BG, fg=MUTED).pack(side='left')
        self._fm_path_var = tk.StringVar(value='—')
        tk.Label(path_row, textvariable=self._fm_path_var,
                 font=('Consolas', 9), bg=BG, fg=INFO_FG,
                 anchor='w').pack(side='left', padx=(6, 0))

        # ── Toolbar ──
        toolbar = tk.Frame(parent, bg=BG)
        toolbar.pack(fill='x', padx=24, pady=(0, 4))

        def tb_btn(text, cmd, fg=TEXT):
            return tk.Button(toolbar, text=text,
                             font=('Segoe UI', 9), bg=SURFACE2, fg=fg,
                             activebackground=BORDER, activeforeground=TEXT,
                             relief='flat', bd=0, padx=10, pady=4,
                             cursor='hand2', command=cmd)

        tb_btn('\u2191 Up',            self._fm_go_up).pack(side='left', padx=(0, 4))
        tb_btn('\U0001f504 Refresh',   self._fm_refresh).pack(side='left', padx=(0, 4))
        tk.Frame(toolbar, bg=BORDER, width=1).pack(side='left', fill='y', padx=6)
        tb_btn('\u2795 Add files',     self._fm_add_file).pack(side='left', padx=(0, 4))
        tb_btn('\U0001f4c1 Add folder', self._fm_add_folder).pack(side='left', padx=(0, 4))
        tb_btn('\U0001f4c2 New folder', self._fm_new_folder).pack(side='left', padx=(0, 4))
        tk.Frame(toolbar, bg=BORDER, width=1).pack(side='left', fill='y', padx=6)
        tb_btn('\U0001f504 Replace',   self._fm_replace_file).pack(side='left', padx=(0, 4))
        tb_btn('\U0001f5d1 Delete',    self._fm_delete, fg=DANGER).pack(side='left', padx=(0, 4))

        # ── File list ──
        list_outer = tk.Frame(parent, bg=SURFACE2,
                              highlightbackground=BORDER, highlightthickness=1)
        list_outer.pack(fill='both', expand=True, padx=24, pady=(0, 8))

        self._fm_listbox = tk.Listbox(
            list_outer, font=('Consolas', 9),
            bg=SURFACE2, fg=TEXT,
            selectbackground=ACCENT, selectforeground='#ffffff',
            activestyle='none', relief='flat', bd=6,
            selectmode='extended')
        fm_sb = tk.Scrollbar(list_outer, command=self._fm_listbox.yview,
                             bg=SURFACE2, troughcolor=BG)
        self._fm_listbox.configure(yscrollcommand=fm_sb.set)
        fm_sb.pack(side='right', fill='y')
        self._fm_listbox.pack(fill='both', expand=True)
        self._fm_listbox.bind('<Double-Button-1>', lambda e: self._fm_enter())
        self._fm_listbox.bind('<Button-3>', self._fm_context_menu)

        # Store file info for selections
        self._fm_entries = []   # list of (name, is_dir, size)

    # ── File Manager helpers ──────────────────────────────────────────────────
    def _fm_find_osf(self):
        candidates = [
            r'C:\Program Files\OSFMount\osfmount.com',
            r'C:\Program Files (x86)\OSFMount\osfmount.com',
            r'C:\Program Files\PassMark\OSFMount\osfmount.com',
        ]
        for c in candidates:
            if os.path.isfile(c):
                return c
        import shutil as _sh
        return _sh.which('osfmount.com')

    def _fm_browse(self):
        p = filedialog.askopenfilename(
            title='Select exFAT image',
            filetypes=[('exFAT images', '*.exfat'), ('All files', '*.*')])
        if p:
            self._fm_image_var.set(p.replace('/', '\\'))

    def _fm_mount(self):
        img = self._fm_image_var.get().strip()
        if not img:
            messagebox.showwarning('No image', 'Please select an exFAT image first.')
            return
        if not os.path.isfile(img):
            messagebox.showerror('Not found', 'Image file not found:\n' + img)
            return
        osf = self._fm_find_osf()
        if not osf:
            messagebox.showerror('OSFMount not found',
                'Please install OSFMount from:\n'
                'https://www.osforensics.com/tools/mount-disk-images.html')
            return
        self._fm_osf = osf

        # Find free drive letter
        try:
            import ctypes as _ct
            bitmask = _ct.windll.kernel32.GetLogicalDrives()
            free = None
            for i in range(25, 3, -1):
                if not (bitmask & (1 << i)):
                    free = chr(65 + i) + ':'
                    break
            if not free:
                raise Exception('No free drive letters')
        except Exception as e:
            messagebox.showerror('Error', str(e))
            return

        self._fm_status_var.set('Mounting...')
        self.update()

        result = subprocess.run(
            [osf, '-a', '-t', 'file', '-f', img,
             '-m', free, '-o', 'rw,rem'],
            capture_output=True, text=True)

        if result.returncode != 0:
            self._fm_status_var.set('Mount failed')
            messagebox.showerror('Mount failed',
                result.stderr or result.stdout or 'Unknown error')
            return

        # Wait for drive
        import time as _t
        for _ in range(20):
            if os.path.exists(free + '\\'):
                break
            _t.sleep(0.3)
        else:
            self._fm_status_var.set('Drive did not appear')
            return

        self._fm_drive = free
        self._fm_current_dir = free + '\\'
        self._fm_status_var.set('Mounted: ' + free + '  (read-write)')
        self._fm_mount_btn.config(state='disabled')
        self._fm_dismount_btn.config(state='normal')
        self._fm_refresh()

    def _fm_dismount(self):
        if not self._fm_drive or not self._fm_osf:
            return
        if not messagebox.askyesno('Dismount',
                'Dismount the image?\n\nMake sure all file operations are complete.'):
            return
        result = subprocess.run(
            [self._fm_osf, '-d', '-m', self._fm_drive],
            capture_output=True, text=True)
        self._fm_drive = None
        self._fm_current_dir = None
        self._fm_osf = None
        self._fm_listbox.delete(0, 'end')
        self._fm_entries = []
        self._fm_path_var.set('—')
        self._fm_status_var.set('Dismounted successfully')
        self._fm_mount_btn.config(state='normal')
        self._fm_dismount_btn.config(state='disabled')

    def _fm_refresh(self):
        if not self._fm_drive or not self._fm_current_dir:
            return
        self._fm_listbox.delete(0, 'end')
        self._fm_entries = []
        self._fm_path_var.set(self._fm_current_dir)
        try:
            entries = []
            with os.scandir(self._fm_current_dir) as it:
                for e in it:
                    try:
                        is_dir = e.is_dir()
                        size   = 0 if is_dir else e.stat().st_size
                        entries.append((e.name, is_dir, size))
                    except Exception:
                        pass
            # Dirs first, then files, both alphabetical
            entries.sort(key=lambda x: (not x[1], x[0].lower()))
            # Add parent dir if not at root
            if self._fm_current_dir.rstrip('\\') != self._fm_drive:
                self._fm_entries.append(('..', True, 0))
                self._fm_listbox.insert('end', '\U0001f4c2  ..')
            for name, is_dir, size in entries:
                self._fm_entries.append((name, is_dir, size))
                if is_dir:
                    self._fm_listbox.insert('end', '\U0001f4c1  ' + name)
                else:
                    if size >= 1024**3:
                        sz = '%.2f GB' % (size / 1024**3)
                    elif size >= 1024**2:
                        sz = '%.1f MB' % (size / 1024**2)
                    else:
                        sz = '%d KB' % (size // 1024)
                    self._fm_listbox.insert('end',
                        '\U0001f4be  %-40s %s' % (name, sz))
        except Exception as e:
            messagebox.showerror('Error reading directory', str(e))

    def _fm_enter(self):
        sel = self._fm_listbox.curselection()
        if not sel:
            return
        name, is_dir, _ = self._fm_entries[sel[0]]
        if not is_dir:
            return
        if name == '..':
            self._fm_go_up()
        else:
            self._fm_current_dir = os.path.join(self._fm_current_dir, name) + '\\'
            self._fm_refresh()

    def _fm_go_up(self):
        if not self._fm_current_dir:
            return
        cur = self._fm_current_dir.rstrip('\\')
        if cur == self._fm_drive:
            return
        parent = os.path.dirname(cur) + '\\'
        self._fm_current_dir = parent
        self._fm_refresh()

    def _fm_selected_path(self):
        sel = self._fm_listbox.curselection()
        if not sel:
            return None, None
        name, is_dir, _ = self._fm_entries[sel[0]]
        if name == '..':
            return None, None
        return os.path.join(self._fm_current_dir, name), is_dir

    def _fm_add_file(self):
        if not self._fm_drive:
            messagebox.showwarning('Not mounted', 'Mount an image first.')
            return
        files = filedialog.askopenfilenames(title='Select file(s) to add')
        if not files:
            return
        added = 0
        skipped = 0
        overwrite_all = [False]
        skip_all      = [False]
        for src in files:
            dst = os.path.join(self._fm_current_dir, os.path.basename(src))
            if os.path.exists(dst) and not overwrite_all[0]:
                if skip_all[0]:
                    skipped += 1
                    continue
                choice = self._fm_overwrite_dialog(os.path.basename(src),
                                                    len(files) > 1)
                if choice == 'skip':
                    skipped += 1
                    continue
                elif choice == 'skip_all':
                    skip_all[0] = True
                    skipped += 1
                    continue
                elif choice == 'overwrite_all':
                    overwrite_all[0] = True
                # 'overwrite' or 'overwrite_all' — fall through to copy
            try:
                shutil.copy2(src, dst)
                added += 1
            except Exception as e:
                messagebox.showerror('Copy failed',
                    'Failed to copy ' + os.path.basename(src) + ':\n' + str(e))
        self._fm_refresh()
        parts = []
        if added:   parts.append(str(added) + ' file(s) added')
        if skipped: parts.append(str(skipped) + ' skipped')
        self._fm_status_var.set('  •  '.join(parts) if parts else 'Done')

    def _fm_add_folder(self):
        if not self._fm_drive:
            messagebox.showwarning('Not mounted', 'Mount an image first.')
            return
        src_folder = filedialog.askdirectory(title='Select folder to copy into image')
        if not src_folder:
            return
        folder_name = os.path.basename(src_folder.rstrip('/\\'))
        dst_root = os.path.join(self._fm_current_dir, folder_name)

        # Collect all files to copy
        all_files = []
        for root, dirs, files in os.walk(src_folder):
            for fn in files:
                src_path = os.path.join(root, fn)
                rel      = os.path.relpath(src_path, src_folder)
                dst_path = os.path.join(dst_root, rel)
                all_files.append((src_path, dst_path, fn))

        if not all_files:
            messagebox.showinfo('Empty folder', 'The selected folder contains no files.')
            return

        # Check for any conflicts upfront
        conflicts = [(s, d, n) for s, d, n in all_files if os.path.exists(d)]
        overwrite_all = False
        if conflicts:
            msg = (str(len(conflicts)) + ' file(s) already exist in the image.\n\n' +
                   '\n'.join(os.path.basename(d) for _, d, _ in conflicts[:8]) +
                   ('\n...' if len(conflicts) > 8 else '') +
                   '\n\nOverwrite all existing files?')
            ans = messagebox.askyesnocancel('Files exist', msg)
            if ans is None:
                return   # Cancel
            overwrite_all = ans

        added = 0
        skipped = 0
        errors = []
        for src_path, dst_path, fn in all_files:
            if os.path.exists(dst_path) and not overwrite_all:
                skipped += 1
                continue
            try:
                os.makedirs(os.path.dirname(dst_path), exist_ok=True)
                shutil.copy2(src_path, dst_path)
                added += 1
            except Exception as e:
                errors.append(fn + ': ' + str(e))

        self._fm_refresh()
        parts = []
        if added:   parts.append(str(added) + ' file(s) copied')
        if skipped: parts.append(str(skipped) + ' skipped')
        self._fm_status_var.set('  •  '.join(parts) if parts else 'Done')
        if errors:
            messagebox.showerror('Some files failed',
                str(len(errors)) + ' file(s) could not be copied:\n\n' +
                '\n'.join(errors[:10]))

    def _fm_overwrite_dialog(self, filename, show_all_options):
        """Ask user what to do when a file already exists. Returns:
        'overwrite', 'overwrite_all', 'skip', 'skip_all'"""
        win = tk.Toplevel(self)
        win.title('File exists')
        win.configure(bg=BG)
        win.resizable(False, False)
        win.transient(self)
        win.grab_set()

        tk.Label(win, text='\u26a0  File already exists',
                 font=('Segoe UI', 11, 'bold'), bg=BG, fg=WARNING).pack(padx=24, pady=(16, 4))
        tk.Label(win, text=filename,
                 font=('Consolas', 9), bg=BG, fg=MUTED).pack(padx=24, pady=(0, 16))

        result = [None]

        btn_frame = tk.Frame(win, bg=BG)
        btn_frame.pack(padx=24, pady=(0, 16))

        def make_btn(text, val, fg=TEXT, bg=SURFACE2):
            tk.Button(btn_frame, text=text, font=('Segoe UI', 9),
                      bg=bg, fg=fg, activebackground=BORDER,
                      activeforeground=TEXT, relief='flat', bd=0,
                      padx=12, pady=6, cursor='hand2',
                      command=lambda v=val: (result.__setitem__(0, v), win.destroy())
                      ).pack(side='left', padx=4)

        make_btn('Overwrite', 'overwrite', bg=WARNING, fg='#000000')
        if show_all_options:
            make_btn('Overwrite All', 'overwrite_all', bg=DANGER)
        make_btn('Skip', 'skip')
        if show_all_options:
            make_btn('Skip All', 'skip_all')

        win.update_idletasks()
        # Centre over parent
        x = self.winfo_x() + (self.winfo_width()  - win.winfo_width())  // 2
        y = self.winfo_y() + (self.winfo_height() - win.winfo_height()) // 2
        win.geometry('+%d+%d' % (x, y))
        win.wait_window()
        return result[0] or 'skip'

    def _fm_new_folder(self):
        if not self._fm_drive:
            messagebox.showwarning('Not mounted', 'Mount an image first.')
            return
        name = tk.simpledialog.askstring('New folder', 'Folder name:',
                                          parent=self)
        if not name:
            return
        try:
            os.makedirs(os.path.join(self._fm_current_dir, name), exist_ok=True)
            self._fm_refresh()
        except Exception as e:
            messagebox.showerror('Error', str(e))

    def _fm_replace_file(self):
        path, is_dir = self._fm_selected_path()
        if not path:
            messagebox.showwarning('No selection', 'Select a file to replace.')
            return
        if is_dir:
            messagebox.showwarning('Is a folder', 'Select a file, not a folder.')
            return
        src = filedialog.askopenfilename(
            title='Select replacement file',
            initialfile=os.path.basename(path))
        if not src:
            return
        try:
            shutil.copy2(src, path)
            self._fm_refresh()
            self._fm_status_var.set('Replaced: ' + os.path.basename(path))
        except Exception as e:
            messagebox.showerror('Replace failed', str(e))

    def _fm_delete(self):
        sels = self._fm_listbox.curselection()
        if not sels:
            messagebox.showwarning('No selection', 'Select file(s) or folder(s) to delete.')
            return
        names = [self._fm_entries[i][0] for i in sels if self._fm_entries[i][0] != '..']
        if not names:
            return
        if not messagebox.askyesno('Delete',
                'Delete ' + str(len(names)) + ' item(s)?\n\n' +
                '\n'.join(names[:10]) +
                ('\n...' if len(names) > 10 else '') +
                '\n\nThis cannot be undone.'):
            return
        errors = []
        for name in names:
            p = os.path.join(self._fm_current_dir, name)
            try:
                if os.path.isdir(p):
                    shutil.rmtree(p)
                else:
                    os.remove(p)
            except Exception as e:
                errors.append(name + ': ' + str(e))
        self._fm_refresh()
        if errors:
            messagebox.showerror('Some deletions failed', '\n'.join(errors))
        else:
            self._fm_status_var.set('Deleted ' + str(len(names)) + ' item(s)')

    def _fm_context_menu(self, event):
        if not self._fm_drive:
            return
        sel = self._fm_listbox.nearest(event.y)
        self._fm_listbox.selection_clear(0, 'end')
        self._fm_listbox.selection_set(sel)
        path, is_dir = self._fm_selected_path()
        if not path:
            return
        menu = tk.Menu(self, tearoff=0, bg=SURFACE2, fg=TEXT,
                       activebackground=ACCENT, activeforeground=TEXT,
                       font=('Segoe UI', 9))
        if not is_dir:
            menu.add_command(label='\U0001f504  Replace file',
                             command=self._fm_replace_file)
        menu.add_command(label='\u2795  Add files here',
                         command=self._fm_add_file)
        menu.add_command(label='\U0001f4c1  Add folder here',
                         command=self._fm_add_folder)
        if is_dir:
            menu.add_command(label='\U0001f4c2  Open folder',
                             command=self._fm_enter)
        menu.add_separator()
        menu.add_command(label='\U0001f5d1  Delete',
                         command=self._fm_delete)
        menu.tk_popup(event.x_root, event.y_root)

    def _build_ftp_tab(self, parent):
        self._ftptab_local_var  = tk.StringVar()
        self._ftptab_remote_var = tk.StringVar(
            value=self._settings.get('ftp_path', '/data/etaHEN/games/'))
        self._ftptab_is_folder  = tk.BooleanVar(value=False)
        self._ftptab_uploading  = False

        body = tk.Frame(parent, bg=BG)
        body.pack(fill='both', expand=True, padx=24, pady=16)

        tk.Label(body, text='FTP Upload to PS5',
                 font=('Segoe UI', 13, 'bold'), bg=BG, fg=TEXT).pack(anchor='w')
        tk.Label(body, text='Upload any file or folder directly to your PS5',
                 font=('Segoe UI', 9), bg=BG, fg=MUTED).pack(anchor='w', pady=(2, 16))

        # ── Type selector ──
        type_row = tk.Frame(body, bg=BG)
        type_row.pack(fill='x', pady=(0, 10))
        tk.Label(type_row, text='Upload type:', font=('Segoe UI', 9),
                 bg=BG, fg=MUTED).pack(side='left')
        tk.Radiobutton(type_row, text='File', variable=self._ftptab_is_folder,
                       value=False, font=('Segoe UI', 9),
                       bg=BG, fg=TEXT, activebackground=BG,
                       selectcolor=SURFACE2, cursor='hand2',
                       command=self._ftptab_update_browse).pack(side='left', padx=(12, 0))
        tk.Radiobutton(type_row, text='Folder', variable=self._ftptab_is_folder,
                       value=True, font=('Segoe UI', 9),
                       bg=BG, fg=TEXT, activebackground=BG,
                       selectcolor=SURFACE2, cursor='hand2',
                       command=self._ftptab_update_browse).pack(side='left', padx=(10, 0))

        # ── Local path ──
        local_lbl_row = tk.Frame(body, bg=BG)
        local_lbl_row.pack(fill='x')
        self._ftptab_local_lbl = tk.Label(local_lbl_row,
                 text='Local file', font=('Segoe UI', 9), bg=BG, fg=MUTED, anchor='w')
        self._ftptab_local_lbl.pack(side='left')
        local_inner = tk.Frame(body, bg=BG)
        local_inner.pack(fill='x', pady=(3, 8))
        ef = tk.Frame(local_inner, bg=FIELD_BG,
                      highlightbackground=BORDER, highlightthickness=1)
        ef.pack(side='left', fill='x', expand=True)
        tk.Entry(ef, textvariable=self._ftptab_local_var,
                 font=('Consolas', 10), bg=FIELD_BG, fg=FIELD_FG,
                 insertbackground=FIELD_FG,
                 selectbackground=FIELD_SEL_BG, selectforeground=FIELD_SEL_FG,
                 relief='flat', bd=6, state='readonly').pack(fill='x')
        self._ftptab_browse_btn = tk.Button(
                 local_inner, text='Browse', font=('Segoe UI', 9),
                 bg=SURFACE2, fg=TEXT, activebackground=BORDER,
                 activeforeground=TEXT, relief='flat', bd=0,
                 padx=14, pady=6, cursor='hand2',
                 command=self._ftptab_browse)
        self._ftptab_browse_btn.pack(side='left', padx=(6, 0))

        # ── Remote path ──
        tk.Label(body, text='PS5 remote path',
                 font=('Segoe UI', 9), bg=BG, fg=MUTED, anchor='w').pack(fill='x')
        remote_inner = tk.Frame(body, bg=BG)
        remote_inner.pack(fill='x', pady=(3, 8))
        ref = tk.Frame(remote_inner, bg=FIELD_BG,
                       highlightbackground=BORDER, highlightthickness=1)
        ref.pack(side='left', fill='x', expand=True)
        tk.Entry(ref, textvariable=self._ftptab_remote_var,
                 font=('Consolas', 10), bg=FIELD_BG, fg=FIELD_FG,
                 insertbackground=FIELD_FG,
                 selectbackground=FIELD_SEL_BG, selectforeground=FIELD_SEL_FG,
                 relief='flat', bd=6).pack(fill='x')
        tk.Button(remote_inner, text='etaHEN default',
                  font=('Segoe UI', 8), bg=SURFACE2, fg=MUTED,
                  activebackground=BORDER, activeforeground=TEXT,
                  relief='flat', bd=0, padx=8, pady=6,
                  cursor='hand2',
                  command=lambda: self._ftptab_remote_var.set(
                      '/data/etaHEN/games/')).pack(side='left', padx=(6, 0))

        # ── Upload button + cancel ──
        btn_row = tk.Frame(body, bg=BG)
        btn_row.pack(fill='x', pady=(4, 0))
        self._ftptab_upload_btn = tk.Button(
            btn_row, text='\U0001f4e1  Upload to PS5',
            font=('Segoe UI', 11, 'bold'),
            bg=ACCENT, fg=TEXT,
            activebackground='#3a8eef', activeforeground=TEXT,
            relief='flat', bd=0, padx=24, pady=9,
            cursor='hand2', command=self._ftptab_start_upload)
        self._ftptab_upload_btn.pack(side='left')
        self._ftptab_cancel_btn = tk.Button(
            btn_row, text='\u2715 Cancel',
            font=('Segoe UI', 9),
            bg=SURFACE2, fg=DANGER,
            activebackground=BORDER, activeforeground=DANGER,
            relief='flat', bd=0, padx=12, pady=9,
            cursor='hand2', command=self._ftptab_cancel)
        # Hidden until upload starts

        # ── Progress ──
        self._ftptab_status_var = tk.StringVar(value='')
        tk.Label(body, textvariable=self._ftptab_status_var,
                 font=('Segoe UI', 9), bg=BG, fg=MUTED,
                 anchor='w').pack(fill='x', pady=(12, 0))

        bar_frame = tk.Frame(body, bg=TRACK, height=18)
        bar_frame.pack(fill='x', pady=(4, 0))
        bar_frame.pack_propagate(False)
        self._ftptab_canvas = tk.Canvas(bar_frame, height=18, bg=TRACK,
                                         highlightthickness=0, bd=0)
        self._ftptab_canvas.pack(fill='both', expand=True)
        self._ftptab_bar = self._ftptab_canvas.create_rectangle(
            0, 0, 0, 18, fill=ACCENT, outline='')
        self._ftptab_canvas.bind('<Configure>',
            lambda e: self._ftptab_set_bar(self._ftptab_pct))

        self._ftptab_eta_var = tk.StringVar(value='')
        tk.Label(body, textvariable=self._ftptab_eta_var,
                 font=('Segoe UI', 8), bg=BG, fg=MUTED,
                 anchor='w').pack(fill='x', pady=(4, 0))
        self._ftptab_pct    = 0
        self._ftptab_cancel_flag = False

        # ── Log ──
        tk.Label(body, text='OUTPUT LOG', font=('Segoe UI', 8),
                 bg=BG, fg=MUTED).pack(anchor='w', pady=(14, 0))
        log_frame = tk.Frame(body, bg=SURFACE2,
                             highlightbackground=BORDER, highlightthickness=1)
        log_frame.pack(fill='both', expand=True, pady=(3, 0))
        self._ftptab_log = tk.Text(log_frame, font=('Consolas', 9),
                                    bg=SURFACE2, fg=TEXT, relief='flat',
                                    bd=6, state='disabled', wrap='word',
                                    height=6, insertbackground=TEXT)
        ftp_sb = tk.Scrollbar(log_frame, command=self._ftptab_log.yview,
                              bg=SURFACE2, troughcolor=BG)
        self._ftptab_log.configure(yscrollcommand=ftp_sb.set)
        ftp_sb.pack(side='right', fill='y')
        self._ftptab_log.pack(fill='both', expand=True)

    # ── FTP tab helpers ───────────────────────────────────────────────────────
    def _ftptab_log_write(self, text):
        self._ftptab_log.config(state='normal')
        self._ftptab_log.insert('end', text)
        self._ftptab_log.see('end')
        self._ftptab_log.config(state='disabled')

    def _ftptab_update_browse(self):
        if self._ftptab_is_folder.get():
            self._ftptab_local_lbl.config(text='Local folder')
        else:
            self._ftptab_local_lbl.config(text='Local file')

    def _ftptab_browse(self):
        if self._ftptab_is_folder.get():
            p = filedialog.askdirectory(title='Select folder to upload')
        else:
            p = filedialog.askopenfilename(title='Select file to upload')
        if p:
            self._ftptab_local_var.set(p.replace('/', '\\'))

    def _ftptab_set_bar(self, pct):
        self._ftptab_pct = pct
        try:
            w = self._ftptab_canvas.winfo_width()
            fill_w = int(w * pct / 100)
            self._ftptab_canvas.coords(self._ftptab_bar, 0, 0, fill_w, 18)
            self._ftptab_canvas.itemconfig(
                self._ftptab_bar, fill=SUCCESS if pct >= 100 else ACCENT)
        except Exception:
            pass

    def _ftptab_cancel(self):
        self._ftptab_cancel_flag = True
        self._ftptab_status_var.set('Cancelling...')

    def _ftptab_start_upload(self):
        local = self._ftptab_local_var.get().strip()
        remote_dir = self._ftptab_remote_var.get().strip() or '/data/etaHEN/games/'
        is_folder  = self._ftptab_is_folder.get()

        if not local:
            messagebox.showwarning('Missing', 'Please select a file or folder to upload.')
            return
        if not os.path.exists(local):
            messagebox.showerror('Not found', 'Path not found:\n' + local)
            return
        ip = self._ftp_ip_var.get().strip()
        if not ip:
            messagebox.showwarning('No IP',
                'Enter your PS5 IP in the Build tab Settings → PS5 FTP UPLOAD.')
            return

        self._ftptab_cancel_flag = False
        self._ftptab_upload_btn.config(state='disabled', text='Uploading...')
        self._ftptab_cancel_btn.pack(side='left', padx=(8, 0))
        self._ftptab_status_var.set('Connecting...')
        self._ftptab_set_bar(0)
        self._ftptab_eta_var.set('')
        self._ftptab_log.config(state='normal')
        self._ftptab_log.delete('1.0', 'end')
        self._ftptab_log.config(state='disabled')
        self._ftptab_log_write('[FTP] ' + ('Folder' if is_folder else 'File') +
                                ': ' + local + '\n')
        self._ftptab_log_write('[FTP] Remote: ' + remote_dir + '\n\n')

        def worker():
            try:
                import ftplib
                ftp = self._ftp_connect()
                self.after(0, self._ftptab_status_var.set, 'Connected')

                def ensure_dir(ftp, path):
                    parts = path.strip('/').split('/')
                    cur = ''
                    for part in parts:
                        cur += '/' + part
                        try:
                            ftp.mkd(cur)
                        except Exception:
                            pass

                if is_folder:
                    # Count total files first
                    all_files = []
                    for root, dirs, files in os.walk(local):
                        for fn in files:
                            all_files.append(os.path.join(root, fn))
                    total_files = len(all_files)
                    total_bytes = sum(os.path.getsize(f) for f in all_files)
                    sent_bytes  = [0]
                    sent_files  = [0]
                    start_t     = time.time()
                    folder_name = os.path.basename(local.rstrip('\\/'))

                    self.after(0, self._ftptab_log_write,
                        '[FTP] %d files, %.2f GB total\n\n' % (
                            total_files, total_bytes / 1024**3))

                    for local_path in all_files:
                        if self._ftptab_cancel_flag:
                            raise Exception('Cancelled by user')
                        rel = os.path.relpath(local_path, os.path.dirname(local))
                        remote_path = (remote_dir.rstrip('/') + '/' +
                                       folder_name + '/' +
                                       rel.replace('\\', '/'))
                        remote_parent = '/'.join(remote_path.split('/')[:-1])
                        ensure_dir(ftp, remote_parent)
                        sz = os.path.getsize(local_path)
                        self.after(0, self._ftptab_log_write,
                            'Uploading: ' + os.path.basename(local_path) + '\n')

                        window = []
                        def prog(block, _sz=sz, _lp=local_path):
                            if self._ftptab_cancel_flag:
                                raise Exception('Cancelled')
                            sent_bytes[0] += len(block)
                            now = time.time()
                            window.append((now, sent_bytes[0]))
                            cutoff = now - 5.0
                            while len(window) > 1 and window[0][0] < cutoff:
                                window.pop(0)
                            dt = window[-1][0] - window[0][0]
                            db = window[-1][1] - window[0][1]
                            speed = (db / 1024 / 1024) / dt if dt > 0 else 0
                            pct = min(100, int(sent_bytes[0] / total_bytes * 100)) if total_bytes else 0
                            elapsed = now - start_t
                            rem_gb  = max(0, (total_bytes - sent_bytes[0]) / 1024**3)
                            eta_s   = (rem_gb * 1024 / speed) if speed > 0 else 0
                            eta_str = ('Almost done' if eta_s < 5 else
                                       'ETA: %dm %02ds' % (int(eta_s//60), int(eta_s%60)))
                            el_str  = 'Elapsed: %dm %02ds' % (int(elapsed//60), int(elapsed%60))
                            status  = ('%d/%d files  \u2022  %.2f GB sent  \u2022  '
                                       '%.1f MB/s  \u2022  %s' % (
                                           sent_files[0]+1, total_files,
                                           sent_bytes[0]/1024**3, speed, eta_str))
                            self.after(0, self._ftptab_status_var.set, status)
                            self.after(0, self._ftptab_eta_var.set, el_str)
                            self.after(0, self._ftptab_set_bar, pct)

                        with open(local_path, 'rb') as f:
                            ftp.storbinary('STOR ' + remote_path, f,
                                           blocksize=65536, callback=prog)
                        sent_files[0] += 1

                else:
                    # Single file upload
                    filename = os.path.basename(local)
                    remote_path = remote_dir.rstrip('/') + '/' + filename
                    ensure_dir(ftp, remote_dir)
                    file_size = os.path.getsize(local)
                    file_size_gb = file_size / 1024**3
                    self.after(0, self._ftptab_log_write,
                        '[FTP] Size: %.2f GB\n\n' % file_size_gb)
                    uploaded = [0]
                    start_t  = [time.time()]
                    window   = []
                    def prog(block):
                        if self._ftptab_cancel_flag:
                            raise Exception('Cancelled')
                        uploaded[0] += len(block)
                        now = time.time()
                        window.append((now, uploaded[0]))
                        cutoff = now - 5.0
                        while len(window) > 1 and window[0][0] < cutoff:
                            window.pop(0)
                        dt = window[-1][0] - window[0][0]
                        db = window[-1][1] - window[0][1]
                        speed = (db / 1024 / 1024) / dt if dt > 0 else 0
                        pct   = min(100, int(uploaded[0] / file_size * 100)) if file_size else 0
                        elapsed = now - start_t[0]
                        rem_gb  = max(0, (file_size - uploaded[0]) / 1024**3)
                        eta_s   = (rem_gb * 1024 / speed) if speed > 0 else 0
                        eta_str = ('Almost done' if eta_s < 5 else
                                   'ETA: %dm %02ds' % (int(eta_s//60), int(eta_s%60)))
                        el_str  = 'Elapsed: %dm %02ds' % (int(elapsed//60), int(elapsed%60))
                        status  = ('%.2f / %.2f GB  \u2022  %.1f MB/s  \u2022  %s' % (
                            uploaded[0]/1024**3, file_size_gb, speed, eta_str))
                        self.after(0, self._ftptab_status_var.set, status)
                        self.after(0, self._ftptab_eta_var.set, el_str)
                        self.after(0, self._ftptab_set_bar, pct)

                    with open(local, 'rb') as f:
                        ftp.storbinary('STOR ' + remote_path, f,
                                       blocksize=65536, callback=prog)

                ftp.quit()
                self.after(0, self._ftptab_done, True, remote_dir)

            except Exception as e:
                self.after(0, self._ftptab_done, False, str(e))

        threading.Thread(target=worker, daemon=True).start()

    def _ftptab_done(self, ok, info):
        self._ftptab_upload_btn.config(state='normal', text='\U0001f4e1  Upload to PS5')
        self._ftptab_cancel_btn.pack_forget()
        if ok:
            self._ftptab_set_bar(100)
            self._ftptab_status_var.set('Upload complete \u2713')
            self._ftptab_log_write('\n[OK] Upload complete: ' + info + '\n')
            self._notify('FTP Upload complete', info)
        else:
            if 'Cancelled' in info:
                self._ftptab_status_var.set('Upload cancelled')
                self._ftptab_log_write('\n[CANCELLED] Upload cancelled by user\n')
            else:
                self._ftptab_status_var.set('Upload failed')
                self._ftptab_log_write('\n[ERROR] ' + info + '\n')
                messagebox.showerror('Upload failed', info)

    def _build_extract_tab(self, parent):
        # ── Extract tab UI ──
        body = tk.Frame(parent, bg=BG)
        body.pack(fill='both', expand=True, padx=24, pady=16)

        tk.Label(body, text='Extract exFAT image to folder',
                 font=('Segoe UI', 13, 'bold'), bg=BG, fg=TEXT).pack(anchor='w')
        tk.Label(body, text='Mount an .exfat image and copy its contents back to a folder',
                 font=('Segoe UI', 9), bg=BG, fg=MUTED).pack(anchor='w', pady=(2, 16))

        # Source image
        self._extract_file_var = tk.StringVar()
        self._field_extract(body, 'exFAT image file (.exfat)',
                            self._extract_file_var, self._browse_extract_file)

        # Output directory
        self._extract_outdir_var = tk.StringVar()
        self._field_extract(body, 'Output directory',
                            self._extract_outdir_var, self._browse_extract_outdir)

        # Output folder name
        fn_row = tk.Frame(body, bg=BG)
        fn_row.pack(fill='x', pady=(0, 8))
        fn_lbl_row = tk.Frame(fn_row, bg=BG)
        fn_lbl_row.pack(fill='x')
        tk.Label(fn_lbl_row, text='Output folder name',
                 font=('Segoe UI', 9), bg=BG, fg=MUTED, anchor='w').pack(side='left')
        self._extract_auto_lbl = tk.Label(fn_lbl_row,
                                           text='  \u2022 auto from filename',
                                           font=('Segoe UI', 8), bg=BG, fg=SUCCESS)
        self._extract_auto_lbl.pack(side='left')
        fn_ef = tk.Frame(fn_row, bg=FIELD_BG,
                         highlightbackground=BORDER, highlightthickness=1)
        fn_ef.pack(fill='x', pady=(3, 0))
        self._extract_name_var = tk.StringVar(value='')
        tk.Entry(fn_ef, textvariable=self._extract_name_var,
                 font=('Consolas', 10), bg=FIELD_BG, fg=FIELD_FG,
                 insertbackground=FIELD_FG,
                 selectbackground=FIELD_SEL_BG, selectforeground=FIELD_SEL_FG,
                 relief='flat', bd=6).pack(fill='x')

        # Extract button
        btn_row = tk.Frame(body, bg=BG)
        btn_row.pack(fill='x', pady=(8, 0))
        self._extract_btn = tk.Button(
            btn_row, text='\U0001f4e4  Extract Image',
            font=('Segoe UI', 11, 'bold'),
            bg=ACCENT, fg=TEXT,
            activebackground='#3a8eef', activeforeground=TEXT,
            relief='flat', bd=0, padx=24, pady=9,
            cursor='hand2', command=self._run_extract)
        self._extract_btn.pack(side='left')

        # Progress
        self._extract_status_var = tk.StringVar(value='')
        tk.Label(body, textvariable=self._extract_status_var,
                 font=('Segoe UI', 9), bg=BG, fg=MUTED,
                 anchor='w').pack(fill='x', pady=(12, 0))

        bar_frame = tk.Frame(body, bg=TRACK, height=18)
        bar_frame.pack(fill='x', pady=(6, 0))
        bar_frame.pack_propagate(False)
        self._extract_canvas = tk.Canvas(bar_frame, height=18, bg=TRACK,
                                          highlightthickness=0, bd=0)
        self._extract_canvas.pack(fill='both', expand=True)
        self._extract_bar = self._extract_canvas.create_rectangle(
            0, 0, 0, 18, fill=ACCENT, outline='')
        self._extract_canvas.bind('<Configure>',
            lambda e: self._update_extract_bar(self._extract_pct))

        self._extract_eta_var = tk.StringVar(value='')
        tk.Label(body, textvariable=self._extract_eta_var,
                 font=('Segoe UI', 8), bg=BG, fg=MUTED,
                 anchor='w').pack(fill='x', pady=(4, 0))

        self._extract_pct = 0

        # Log
        tk.Label(body, text='OUTPUT LOG', font=('Segoe UI', 8),
                 bg=BG, fg=MUTED).pack(anchor='w', pady=(16, 0))
        log_frame = tk.Frame(body, bg=SURFACE2,
                             highlightbackground=BORDER, highlightthickness=1)
        log_frame.pack(fill='both', expand=True, pady=(3, 0))
        self._extract_log = tk.Text(log_frame, font=('Consolas', 9),
                                     bg=SURFACE2, fg=TEXT, relief='flat',
                                     bd=6, state='disabled', wrap='word',
                                     height=8, insertbackground=TEXT)
        esb = tk.Scrollbar(log_frame, command=self._extract_log.yview,
                           bg=SURFACE2, troughcolor=BG)
        self._extract_log.configure(yscrollcommand=esb.set)
        esb.pack(side='right', fill='y')
        self._extract_log.pack(fill='both', expand=True)

    def _field_extract(self, parent, label, var, browse_cmd):
        row = tk.Frame(parent, bg=BG)
        row.pack(fill='x', pady=(0, 8))
        tk.Label(row, text=label, font=('Segoe UI', 9),
                 bg=BG, fg=MUTED, anchor='w').pack(fill='x')
        inner = tk.Frame(row, bg=BG)
        inner.pack(fill='x', pady=(3, 0))
        ef = tk.Frame(inner, bg=FIELD_BG,
                      highlightbackground=BORDER, highlightthickness=1)
        ef.pack(side='left', fill='x', expand=True)
        tk.Entry(ef, textvariable=var, font=('Consolas', 10),
                 bg=FIELD_BG, fg=FIELD_FG, insertbackground=FIELD_FG,
                 selectbackground=FIELD_SEL_BG, selectforeground=FIELD_SEL_FG,
                 relief='flat', bd=6, state='readonly').pack(fill='x')
        tk.Button(inner, text='Browse', font=('Segoe UI', 9),
                  bg=SURFACE2, fg=TEXT, activebackground=BORDER,
                  activeforeground=TEXT, relief='flat', bd=0,
                  padx=14, pady=6, cursor='hand2',
                  command=browse_cmd).pack(side='left', padx=(6, 0))

    def _browse_extract_file(self):
        p = filedialog.askopenfilename(
            title='Select exFAT image',
            filetypes=[('exFAT images', '*.exfat'), ('All files', '*.*')])
        if not p:
            return
        p = p.replace('/', '\\')
        self._extract_file_var.set(p)
        # Auto-fill folder name from filename (strip .exfat)
        base = os.path.splitext(os.path.basename(p))[0]
        self._extract_name_var.set(base)

    def _browse_extract_outdir(self):
        p = filedialog.askdirectory(title='Select output directory')
        if p:
            self._extract_outdir_var.set(p.replace('/', '\\'))

    def _update_extract_bar(self, pct):
        self._extract_pct = pct
        try:
            w = self._extract_canvas.winfo_width()
            fill_w = int(w * pct / 100)
            self._extract_canvas.coords(self._extract_bar, 0, 0, fill_w, 18)
            self._extract_canvas.itemconfig(
                self._extract_bar, fill=SUCCESS if pct >= 100 else ACCENT)
        except Exception:
            pass

    def _elog(self, text):
        self._extract_log.config(state='normal')
        self._extract_log.insert('end', text)
        self._extract_log.see('end')
        self._extract_log.config(state='disabled')

    def _elog_clear(self):
        self._extract_log.config(state='normal')
        self._extract_log.delete('1.0', 'end')
        self._extract_log.config(state='disabled')

    def _run_extract(self):
        img_path = self._extract_file_var.get().strip()
        out_dir  = self._extract_outdir_var.get().strip()
        out_name = self._extract_name_var.get().strip()

        if not img_path:
            messagebox.showwarning('Missing', 'Please select an exFAT image file.')
            return
        if not os.path.isfile(img_path):
            messagebox.showerror('Not found', 'Image file not found:\n' + img_path)
            return
        if not out_dir:
            messagebox.showwarning('Missing', 'Please select an output directory.')
            return
        if not out_name:
            out_name = os.path.splitext(os.path.basename(img_path))[0]
            self._extract_name_var.set(out_name)

        dest_folder = os.path.join(out_dir, out_name)
        if os.path.exists(dest_folder):
            if not messagebox.askyesno('Folder exists',
                    'Output folder already exists:\n' + dest_folder +
                    '\n\nFiles will be merged/overwritten. Continue?'):
                return

        self._extract_btn.config(state='disabled', text='Extracting...')
        self._extract_status_var.set('Starting...')
        self._update_extract_bar(0)
        self._extract_eta_var.set('')
        self._elog_clear()
        self._elog('[EXTRACT] Image: ' + img_path + '\n')
        self._elog('[EXTRACT] Destination: ' + dest_folder + '\n\n')

        # Find OSFMount
        osfmount_candidates = [
            r'C:\Program Files\OSFMount\osfmount.com',
            r'C:\Program Files (x86)\OSFMount\osfmount.com',
            r'C:\Program Files\PassMark\OSFMount\osfmount.com',
        ]
        osf = None
        for c in osfmount_candidates:
            if os.path.isfile(c):
                osf = c
                break
        if not osf:
            # Try PATH
            import shutil as _sh
            osf = _sh.which('osfmount.com')
        if not osf:
            messagebox.showerror('OSFMount not found',
                'Could not find osfmount.com.\n'
                'Please install OSFMount from:\n'
                'https://www.osforensics.com/tools/mount-disk-images.html')
            self._extract_btn.config(state='normal', text='\U0001f4e4  Extract Image')
            return

        start_time = [time.time()]
        mount_point = [None]

        def worker():
            try:
                os.makedirs(dest_folder, exist_ok=True)

                # Find free drive letter
                import ctypes as _ct
                drives_bitmask = _ct.windll.kernel32.GetLogicalDrives()
                free_letter = None
                for i in range(25, 3, -1):  # Z: down to D:
                    if not (drives_bitmask & (1 << i)):
                        free_letter = chr(65 + i) + ':'
                        break
                if not free_letter:
                    raise Exception('No free drive letters available')

                mount_point[0] = free_letter
                self.after(0, self._elog,
                    '[EXTRACT] Mounting image on ' + free_letter + '...\n')
                self.after(0, self._extract_status_var.set, 'Mounting image...')

                # Mount read-only
                result = subprocess.run(
                    [osf, '-a', '-t', 'file', '-f', img_path,
                     '-m', free_letter, '-o', 'ro,rem'],
                    capture_output=True, text=True)
                if result.returncode != 0:
                    raise Exception('Mount failed: ' + result.stderr + result.stdout)

                # Wait for drive to appear
                import time as _t
                for _ in range(20):
                    if os.path.exists(free_letter + '\\'):
                        break
                    _t.sleep(0.5)
                else:
                    raise Exception('Mounted drive did not appear in time')

                self.after(0, self._elog, '[EXTRACT] Mounted. Copying files...\n')
                self.after(0, self._extract_status_var.set, 'Copying files...')

                # Get total size for progress
                total_bytes = [0]
                for root, dirs, files in os.walk(free_letter + '\\'):
                    for fn in files:
                        try:
                            total_bytes[0] += os.path.getsize(os.path.join(root, fn))
                        except Exception:
                            pass

                self.after(0, self._elog,
                    '[EXTRACT] Total: %.2f GB to copy\n' % (total_bytes[0] / 1024**3))

                # Copy using robocopy for reliability
                robo_cmd = [
                    'robocopy.exe',
                    free_letter + '\\', dest_folder,
                    '/E', '/COPY:DAT', '/DCOPY:DAT',
                    '/R:1', '/W:1', '/NP', '/ETA'
                ]
                proc = subprocess.Popen(
                    robo_cmd,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, encoding='utf-8', errors='replace')

                copied_bytes = [0]
                for line in proc.stdout:
                    self.after(0, self._elog, line)
                    # Parse robocopy progress
                    m = re.search(r'(\d{1,3})%', line)
                    if m:
                        pct = int(m.group(1))
                        elapsed = time.time() - start_time[0]
                        remaining = max(0, (elapsed / (pct/100.0)) - elapsed) if pct > 0 else 0
                        eta = ('ETA: %dm %02ds' % (int(remaining//60), int(remaining%60))
                               if remaining > 5 else 'Almost done')
                        elapsed_str = 'Elapsed: %dm %02ds' % (int(elapsed//60), int(elapsed%60))
                        self.after(0, self._update_extract_bar, pct)
                        self.after(0, self._extract_eta_var.set,
                                   elapsed_str + '  \u2014  ' + eta)
                proc.wait()

                # Dismount
                self.after(0, self._elog, '\n[EXTRACT] Dismounting...\n')
                subprocess.run([osf, '-d', '-m', free_letter],
                               capture_output=True)
                mount_point[0] = None

                elapsed = time.time() - start_time[0]
                m, s = int(elapsed//60), int(elapsed%60)
                self.after(0, self._extract_done, True, dest_folder, m, s)

            except Exception as e:
                # Try to dismount on error
                if mount_point[0]:
                    try:
                        subprocess.run([osf, '-d', '-m', mount_point[0]],
                                       capture_output=True)
                    except Exception:
                        pass
                self.after(0, self._extract_done, False, str(e), 0, 0)

        threading.Thread(target=worker, daemon=True).start()

    def _extract_done(self, ok, info, m, s):
        self._extract_btn.config(state='normal', text='\U0001f4e4  Extract Image')
        if ok:
            self._update_extract_bar(100)
            self._extract_status_var.set('Complete! Extracted to: ' + info)
            self._extract_eta_var.set('Finished in %dm %02ds' % (m, s))
            self._elog('\n[OK] Extraction complete: ' + info + '\n')
            if messagebox.askyesno('Extraction complete',
                    'Image extracted successfully!\n\n' + info +
                    '\n\nOpen output folder?'):
                subprocess.Popen('explorer "' + info + '"', shell=True)
        else:
            self._extract_status_var.set('Extraction failed')
            self._elog('\n[ERROR] ' + info + '\n')
            messagebox.showerror('Extraction failed', info)

    def _build_build_tab(self, parent):
        # ── Settings strip — collapsible ──
        settings_outer = tk.Frame(parent, bg=BG)
        settings_outer.pack(fill='x', padx=24, pady=(8, 2))
        settings_header = tk.Frame(settings_outer, bg=BG)
        settings_header.pack(fill='x')
        self._settings_toggle_lbl = tk.Label(
            settings_header, text='\u25b6  SETTINGS',
            font=('Segoe UI', 8), bg=BG, fg=MUTED,
            cursor='hand2', anchor='w')
        self._settings_toggle_lbl.pack(side='left')
        self._settings_visible = tk.BooleanVar(value=False)
        self._settings_toggle_lbl.bind('<Button-1>', lambda e: self._toggle_settings())
        settings_header.bind('<Button-1>', lambda e: self._toggle_settings())

        self._settings_body = tk.Frame(settings_outer, bg=BG)
        self._settings_frame = tk.Frame(self._settings_body, bg=SETTINGS_BG,
                                         highlightbackground=BORDER,
                                         highlightthickness=1)
        self._settings_frame.pack(fill='x', pady=(4, 0))
        self._build_settings_strip()

        tk.Frame(parent, bg=BORDER, height=1).pack(fill='x', pady=(6, 0))

        # ── Add to queue form ──
        form = tk.Frame(parent, bg=BG)
        form.pack(fill='x', padx=24, pady=6)
        tk.Label(form, text='ADD TO QUEUE', font=('Segoe UI', 8),
                 bg=BG, fg=MUTED).pack(anchor='w', pady=(0, 4))

        self._field(form, 'Game root folder  (must contain eboot.bin)',
                    self.game_folder, self._browse_game)

        # Recently used folders dropdown
        recent_row = tk.Frame(form, bg=BG)
        recent_row.pack(fill='x', pady=(0, 6))
        tk.Label(recent_row, text='Recent:',
                 font=('Segoe UI', 8), bg=BG, fg='#444444').pack(side='left')
        self._recent_var = tk.StringVar(value='')
        self._recent_menu = ttk.Combobox(recent_row, textvariable=self._recent_var,
                                          font=('Segoe UI', 8), state='readonly',
                                          width=60)
        self._recent_menu.pack(side='left', padx=(6, 0))
        self._recent_menu.bind('<<ComboboxSelected>>', self._on_recent_select)
        self._update_recent_menu()

        # Detected game info banner
        self._info_frame = tk.Frame(form, bg=INFO_BG,
                                     highlightbackground='#1a3a5c',
                                     highlightthickness=1)
        self._info_title_var = tk.StringVar(value='')
        self._info_id_var    = tk.StringVar(value='')
        self._info_ver_var   = tk.StringVar(value='')
        self._info_size_var  = tk.StringVar(value='')
        info_inner = tk.Frame(self._info_frame, bg=INFO_BG)
        info_inner.pack(fill='x', padx=10, pady=6)
        # Cover art thumbnail
        self._cover_label = tk.Label(info_inner, bg=INFO_BG,
                                      width=5, height=3,
                                      relief='flat', bd=0)
        self._cover_label.pack(side='left', padx=(0, 10))
        self._cover_image = None  # keep reference
        info_text = tk.Frame(info_inner, bg=INFO_BG)
        info_text.pack(side='left', fill='x', expand=True)
        tk.Label(info_text, textvariable=self._info_title_var,
                 font=('Segoe UI', 10, 'bold'), bg=INFO_BG, fg=TEXT,
                 anchor='w').pack(fill='x')
        meta_row = tk.Frame(info_text, bg=INFO_BG)
        meta_row.pack(fill='x')
        tk.Label(meta_row, textvariable=self._info_id_var,
                 font=('Consolas', 9), bg=INFO_BG, fg=INFO_FG,
                 anchor='w').pack(side='left')
        tk.Label(meta_row, textvariable=self._info_ver_var,
                 font=('Consolas', 9), bg=INFO_BG, fg=MUTED,
                 anchor='w').pack(side='left', padx=(12, 0))
        tk.Label(info_text, textvariable=self._info_size_var,
                 font=('Segoe UI', 8), bg=INFO_BG, fg='#5a8fa8',
                 anchor='w').pack(fill='x')

        self._field(form, 'Output directory',
                    self.output_dir, self._browse_output)

        fn_row = tk.Frame(form, bg=BG)
        fn_row.pack(fill='x', pady=(0, 6))
        fn_label_row = tk.Frame(fn_row, bg=BG)
        fn_label_row.pack(fill='x')
        tk.Label(fn_label_row, text='Output filename',
                 font=('Segoe UI', 9), bg=BG, fg=MUTED,
                 anchor='w').pack(side='left')
        self._auto_label = tk.Label(fn_label_row,
                                     text='  \u2022 auto-detected',
                                     font=('Segoe UI', 8), bg=BG, fg=SUCCESS,
                                     anchor='w')
        self._auto_label.pack(side='left')
        self._auto_label.pack_forget()
        self._no_sfo_label = tk.Label(fn_label_row,
                                       text='  \u2022 no metadata found \u2014 will use folder name',
                                       font=('Segoe UI', 8), bg=BG, fg=WARNING,
                                       anchor='w')
        self._no_sfo_label.pack(side='left')
        self._no_sfo_label.pack_forget()
        # Read-only preview box — not editable, styled differently to show it's a preview
        fn_inner = tk.Frame(fn_row, bg='#111111',
                            highlightbackground='#333333', highlightthickness=1)
        fn_inner.pack(fill='x', pady=(3, 0))
        preview_row = tk.Frame(fn_inner, bg='#111111')
        preview_row.pack(fill='x', padx=8, pady=6)
        tk.Label(preview_row, text='\U0001f4c4', font=('Segoe UI', 10),
                 bg='#111111', fg='#555555').pack(side='left', padx=(0, 6))
        self._filename_preview = tk.Label(preview_row,
                                           textvariable=self.output_name,
                                           font=('Consolas', 10),
                                           bg='#111111', fg='#aaaaaa',
                                           anchor='w')
        self._filename_preview.pack(side='left', fill='x', expand=True)

        add_row = tk.Frame(form, bg=BG)
        add_row.pack(fill='x', pady=(4, 0))
        tk.Button(add_row, text='+ Add to Queue',
                  font=('Segoe UI', 10, 'bold'),
                  bg=ACCENT, fg=TEXT,
                  activebackground='#3a8eef', activeforeground=TEXT,
                  relief='flat', bd=0, padx=18, pady=7,
                  cursor='hand2', command=self._add_to_queue
                  ).pack(side='left')
        tk.Label(add_row, text='Add multiple games then click Build All',
                 font=('Segoe UI', 9), bg=BG, fg=MUTED).pack(side='left', padx=12)

        tk.Frame(parent, bg=BORDER, height=1).pack(fill='x')

        # ── Queue ──
        q_header = tk.Frame(parent, bg=BG)
        q_header.pack(fill='x', padx=24, pady=(6, 3))
        tk.Label(q_header, text='QUEUE', font=('Segoe UI', 8),
                 bg=BG, fg=MUTED).pack(side='left')
        self._queue_count_var = tk.StringVar(value='0 items')
        tk.Label(q_header, textvariable=self._queue_count_var,
                 font=('Segoe UI', 8), bg=BG, fg=ACCENT).pack(side='left', padx=8)
        tk.Button(q_header, text='Clear All', font=('Segoe UI', 8),
                  bg=BG, fg=DANGER, activebackground=BG,
                  activeforeground=DANGER, relief='flat', bd=0,
                  cursor='hand2', command=self._clear_queue).pack(side='right')
        tk.Button(q_header, text='\U0001f4be Save queue', font=('Segoe UI', 8),
                  bg=BG, fg=MUTED, activebackground=BG,
                  activeforeground=TEXT, relief='flat', bd=0,
                  cursor='hand2', command=self._queue_save).pack(side='right', padx=(0,8))
        tk.Button(q_header, text='\U0001f4c2 Load queue', font=('Segoe UI', 8),
                  bg=BG, fg=MUTED, activebackground=BG,
                  activeforeground=TEXT, relief='flat', bd=0,
                  cursor='hand2', command=self._queue_load).pack(side='right', padx=(0,8))

        q_outer = tk.Frame(parent, bg=SURFACE2,
                           highlightbackground=BORDER, highlightthickness=1)
        q_outer.pack(fill='x', padx=24)
        self._queue_canvas = tk.Canvas(q_outer, bg=SURFACE2,
                                        highlightthickness=0, height=100)
        q_sb = tk.Scrollbar(q_outer, orient='vertical',
                             command=self._queue_canvas.yview,
                             bg=SURFACE2, troughcolor=BG)
        self._queue_canvas.configure(yscrollcommand=q_sb.set)
        q_sb.pack(side='right', fill='y')
        self._queue_canvas.pack(side='left', fill='both', expand=True)
        self._queue_frame = tk.Frame(self._queue_canvas, bg=SURFACE2)
        self._queue_canvas.create_window((0, 0), window=self._queue_frame,
                                          anchor='nw', tags='qf')
        self._queue_frame.bind('<Configure>', lambda e:
            self._queue_canvas.configure(
                scrollregion=self._queue_canvas.bbox('all')))
        self._queue_canvas.bind('<Configure>', lambda e:
            self._queue_canvas.itemconfig('qf', width=e.width))

        # Drag and drop onto queue area
        try:
            self._queue_canvas.drop_target_register('DND_Files')
            self._queue_canvas.dnd_bind('<<Drop>>', self._on_queue_drop)
        except Exception:
            pass

        # ── Bottom bar — packed FIRST = always visible at very bottom ──
        bottom = tk.Frame(parent, bg=BG)
        bottom.pack(side='bottom', fill='x', padx=24, pady=6)
        self.status_lbl = tk.Label(bottom, textvariable=self.status_text,
                                    font=('Segoe UI', 9), bg=BG, fg=SUCCESS)
        self.status_lbl.pack(side='left')
        self.build_btn = tk.Button(
            bottom, text='Build All', font=('Segoe UI', 11, 'bold'),
            bg=SUCCESS, fg=TEXT, activebackground='#3d9140',
            activeforeground=TEXT, relief='flat', bd=0,
            padx=24, pady=8, cursor='hand2', command=self._run_queue)
        self.build_btn.pack(side='right')

        tk.Frame(parent, bg=BORDER, height=1).pack(side='bottom', fill='x')

        # ── Progress — packed second from bottom ──
        prog_outer = tk.Frame(parent, bg=BG)
        prog_outer.pack(side='bottom', fill='x', padx=24, pady=(4, 2))

        dots_frame = tk.Frame(prog_outer, bg=BG)
        dots_frame.pack(fill='x', pady=(4, 2))
        self._stage_dots = []
        for name in ['Mount', 'Format', 'Copy files', 'Dismount']:
            col = tk.Frame(dots_frame, bg=BG)
            col.pack(side='left', expand=True)
            dot = tk.Canvas(col, width=14, height=14, bg=BG, highlightthickness=0)
            dot.pack()
            dot.create_oval(1, 1, 13, 13, fill=BORDER, outline='', tags='dot')
            lbl = tk.Label(col, text=name, font=('Segoe UI', 8), bg=BG, fg=MUTED)
            lbl.pack()
            self._stage_dots.append((dot, lbl))

        eta_row = tk.Frame(prog_outer, bg=BG)
        eta_row.pack(fill='x', pady=(2, 0))
        tk.Label(eta_row, textvariable=self._eta_var,
                 font=('Segoe UI', 9), bg=BG, fg=MUTED,
                 anchor='w').pack(side='left')

        bar_frame = tk.Frame(prog_outer, bg=TRACK, height=18)
        bar_frame.pack(fill='x')
        bar_frame.pack_propagate(False)
        self._bar_canvas = tk.Canvas(bar_frame, height=18, bg=TRACK,
                                      highlightthickness=0, bd=0)
        self._bar_canvas.pack(fill='both', expand=True)
        self._bar_rect = self._bar_canvas.create_rectangle(
            0, 0, 0, 18, fill=ACCENT, outline='')
        self._bar_canvas.bind('<Configure>', self._on_bar_resize)

        top_row = tk.Frame(prog_outer, bg=BG)
        top_row.pack(fill='x', pady=(0, 4))
        tk.Label(top_row, textvariable=self._step_label_var,
                 font=('Segoe UI', 10, 'bold'), bg=BG, fg=TEXT,
                 anchor='w').pack(side='left')
        tk.Label(top_row, textvariable=self._pct_var,
                 font=('Consolas', 10), bg=BG, fg=ACCENT,
                 anchor='e').pack(side='right')

        tk.Frame(parent, bg=BORDER, height=1).pack(side='bottom', fill='x')

        # ── Collapsible log panel ──
        self._log_visible = tk.BooleanVar(value=False)
        log_outer = tk.Frame(parent, bg=BG)
        log_outer.pack(side='bottom', fill='x', padx=24, pady=(0, 2))

        # Toggle header row
        log_header = tk.Frame(log_outer, bg=BG)
        log_header.pack(fill='x')
        self._log_toggle_lbl = tk.Label(
            log_header, text='▶  OUTPUT LOG',
            font=('Segoe UI', 8), bg=BG, fg=MUTED,
            cursor='hand2', anchor='w')
        self._log_toggle_lbl.pack(side='left')
        self._log_clear_btn = tk.Button(
            log_header, text='Clear', font=('Segoe UI', 8),
            bg=BG, fg='#444444', activebackground=BG,
            activeforeground=MUTED, relief='flat', bd=0,
            cursor='hand2', command=self._log_clear)
        self._log_clear_btn.pack(side='right')

        # Collapsible body
        self._log_body = tk.Frame(log_outer, bg=BG)
        txt_frame = tk.Frame(self._log_body, bg=SURFACE2,
                             highlightbackground=BORDER, highlightthickness=1)
        txt_frame.pack(fill='x', pady=(3, 0))
        self.log_box = tk.Text(txt_frame, font=('Consolas', 9),
                               bg=SURFACE2, fg=TEXT, relief='flat',
                               bd=6, state='disabled', wrap='word',
                               height=5, insertbackground=TEXT)
        sb2 = tk.Scrollbar(txt_frame, command=self.log_box.yview,
                           bg=SURFACE2, troughcolor=BG)
        self.log_box.configure(yscrollcommand=sb2.set)
        sb2.pack(side='right', fill='y')
        self.log_box.pack(fill='both', expand=True)

        # Bind toggle
        self._log_toggle_lbl.bind('<Button-1>', lambda e: self._toggle_log())
        log_header.bind('<Button-1>', lambda e: self._toggle_log())

    # ── Settings strip ──────────────────────────────────────────────────────
    def _build_settings_strip(self):
        f = self._settings_frame
        # Title row
        title_row = tk.Frame(f, bg=SETTINGS_BG)
        title_row.pack(fill='x', padx=12, pady=(6, 2))
        tk.Label(title_row, text='SETTINGS', font=('Segoe UI', 8),
                 bg=SETTINGS_BG, fg=MUTED).pack(side='left')

        # Temp folder row
        temp_row = tk.Frame(f, bg=SETTINGS_BG)
        temp_row.pack(fill='x', padx=12, pady=(0, 4))
        tk.Label(temp_row, text='Temp folder',
                 font=('Segoe UI', 9), bg=SETTINGS_BG, fg=MUTED,
                 width=12, anchor='w').pack(side='left')
        ef = tk.Frame(temp_row, bg=FIELD_BG,
                      highlightbackground=BORDER, highlightthickness=1)
        ef.pack(side='left', fill='x', expand=True, padx=(6, 6))
        tk.Entry(ef, textvariable=self._temp_dir_var,
                 font=('Consolas', 9), bg=FIELD_BG, fg=FIELD_FG,
                 insertbackground=FIELD_FG,
                 selectbackground=FIELD_SEL_BG, selectforeground=FIELD_SEL_FG,
                 relief='flat', bd=4,
                 state='readonly').pack(fill='x')
        tk.Button(temp_row, text='Browse',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=TEXT,
                  activebackground=BORDER, activeforeground=TEXT,
                  relief='flat', bd=0, padx=10, pady=4,
                  cursor='hand2',
                  command=self._browse_temp).pack(side='left')
        tk.Button(temp_row, text='Reset',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=MUTED,
                  activebackground=BORDER, activeforeground=TEXT,
                  relief='flat', bd=0, padx=10, pady=4,
                  cursor='hand2',
                  command=self._reset_temp).pack(side='left', padx=(4, 0))

        # Temp usage + clear row
        usage_row = tk.Frame(f, bg=SETTINGS_BG)
        usage_row.pack(fill='x', padx=12, pady=(0, 6))
        tk.Label(usage_row, text='',
                 font=('Segoe UI', 9), bg=SETTINGS_BG, fg=MUTED,
                 width=12, anchor='w').pack(side='left')
        self._temp_size_var = tk.StringVar(value='')
        self._temp_size_lbl = tk.Label(usage_row,
                                        textvariable=self._temp_size_var,
                                        font=('Segoe UI', 8),
                                        bg=SETTINGS_BG, fg=MUTED,
                                        anchor='w')
        self._temp_size_lbl.pack(side='left', padx=(6, 12))
        tk.Button(usage_row, text='Clear Temp Files',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=WARNING,
                  activebackground=BORDER, activeforeground=WARNING,
                  relief='flat', bd=0, padx=10, pady=4,
                  cursor='hand2',
                  command=self._clear_temp).pack(side='left')

        # ── FTP section ──
        tk.Frame(f, bg=BORDER, height=1).pack(fill='x', padx=12, pady=(4, 0))

        ftp_title = tk.Frame(f, bg=SETTINGS_BG)
        ftp_title.pack(fill='x', padx=12, pady=(6, 4))
        tk.Label(ftp_title, text='PS5 FTP UPLOAD', font=('Segoe UI', 8),
                 bg=SETTINGS_BG, fg=MUTED).pack(side='left')
        self._ftp_auto_chk = tk.Checkbutton(
            ftp_title, text='Auto-upload after build',
            variable=self._ftp_auto_var,
            font=('Segoe UI', 8), bg=SETTINGS_BG, fg=MUTED,
            activebackground=SETTINGS_BG, activeforeground=TEXT,
            selectcolor=SURFACE2, cursor='hand2',
            command=self._save_ftp_settings)
        self._ftp_auto_chk.pack(side='right')

        # IP + Port row
        ftp_ip_row = tk.Frame(f, bg=SETTINGS_BG)
        ftp_ip_row.pack(fill='x', padx=12, pady=(0, 4))
        tk.Label(ftp_ip_row, text='PS5 IP : Port',
                 font=('Segoe UI', 9), bg=SETTINGS_BG, fg=MUTED,
                 width=12, anchor='w').pack(side='left')
        ip_ef = tk.Frame(ftp_ip_row, bg=FIELD_BG,
                         highlightbackground=BORDER, highlightthickness=1)
        ip_ef.pack(side='left', fill='x', expand=True, padx=(6, 4))
        tk.Entry(ip_ef, textvariable=self._ftp_ip_var,
                 font=('Consolas', 9), bg=FIELD_BG, fg=FIELD_FG,
                 insertbackground=FIELD_FG,
                 selectbackground=FIELD_SEL_BG, selectforeground=FIELD_SEL_FG,
                 relief='flat', bd=4).pack(fill='x')
        tk.Label(ftp_ip_row, text=':', font=('Segoe UI', 9),
                 bg=SETTINGS_BG, fg=MUTED).pack(side='left')
        port_ef = tk.Frame(ftp_ip_row, bg=FIELD_BG,
                           highlightbackground=BORDER, highlightthickness=1)
        port_ef.pack(side='left', padx=(4, 6))
        tk.Entry(port_ef, textvariable=self._ftp_port_var,
                 font=('Consolas', 9), bg=FIELD_BG, fg=FIELD_FG,
                 insertbackground=FIELD_FG,
                 selectbackground=FIELD_SEL_BG, selectforeground=FIELD_SEL_FG,
                 relief='flat', bd=4, width=6).pack()
        tk.Button(ftp_ip_row, text='Save',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=TEXT,
                  activebackground=BORDER, activeforeground=TEXT,
                  relief='flat', bd=0, padx=10, pady=4,
                  cursor='hand2',
                  command=self._save_ftp_settings).pack(side='left')

        # PS5 path row
        ftp_path_row = tk.Frame(f, bg=SETTINGS_BG)
        ftp_path_row.pack(fill='x', padx=12, pady=(0, 4))
        tk.Label(ftp_path_row, text='PS5 path',
                 font=('Segoe UI', 9), bg=SETTINGS_BG, fg=MUTED,
                 width=12, anchor='w').pack(side='left')
        path_ef = tk.Frame(ftp_path_row, bg=FIELD_BG,
                           highlightbackground=BORDER, highlightthickness=1)
        path_ef.pack(side='left', fill='x', expand=True, padx=(6, 6))
        tk.Entry(path_ef, textvariable=self._ftp_path_var,
                 font=('Consolas', 9), bg=FIELD_BG, fg=FIELD_FG,
                 insertbackground=FIELD_FG,
                 selectbackground=FIELD_SEL_BG, selectforeground=FIELD_SEL_FG,
                 relief='flat', bd=4).pack(fill='x')
        tk.Button(ftp_path_row, text='etaHEN default',
                  font=('Segoe UI', 8), bg=SURFACE2, fg=MUTED,
                  activebackground=BORDER, activeforeground=TEXT,
                  relief='flat', bd=0, padx=8, pady=4,
                  cursor='hand2',
                  command=lambda: self._ftp_path_var.set('/data/etaHEN/games/')
                  ).pack(side='left')

        # FTP status + test row
        ftp_status_row = tk.Frame(f, bg=SETTINGS_BG)
        ftp_status_row.pack(fill='x', padx=12, pady=(0, 8))
        tk.Label(ftp_status_row, text='',
                 font=('Segoe UI', 9), bg=SETTINGS_BG, fg=MUTED,
                 width=12, anchor='w').pack(side='left')
        self._ftp_status_var = tk.StringVar(value='Not connected')
        self._ftp_status_lbl = tk.Label(ftp_status_row,
                                         textvariable=self._ftp_status_var,
                                         font=('Segoe UI', 8),
                                         bg=SETTINGS_BG, fg=MUTED, anchor='w')
        self._ftp_status_lbl.pack(side='left', padx=(6, 12))
        tk.Button(ftp_status_row, text='Test Connection',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=ACCENT,
                  activebackground=BORDER, activeforeground=ACCENT,
                  relief='flat', bd=0, padx=10, pady=4,
                  cursor='hand2',
                  command=self._ftp_test).pack(side='left')
        tk.Button(ftp_status_row, text='Ping PS5',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=MUTED,
                  activebackground=BORDER, activeforeground=TEXT,
                  relief='flat', bd=0, padx=10, pady=4,
                  cursor='hand2',
                  command=lambda: self._ftp_ping(
                      lambda ok, msg: (
                          self._ftp_status_var.set(msg),
                          self._ftp_status_lbl.config(fg=SUCCESS if ok else DANGER)
                      ))).pack(side='left', padx=(6, 0))
        tk.Button(ftp_status_row, text='\U0001f4c1 Browse PS5',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=INFO_FG,
                  activebackground=BORDER, activeforeground=INFO_FG,
                  relief='flat', bd=0, padx=10, pady=4,
                  cursor='hand2',
                  command=self._show_ps5_browser).pack(side='left', padx=(6, 0))
        tk.Button(ftp_status_row, text='\U0001f3ae Games on PS5',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=SUCCESS,
                  activebackground=BORDER, activeforeground=SUCCESS,
                  relief='flat', bd=0, padx=10, pady=4,
                  cursor='hand2',
                  command=self._show_installed_games).pack(side='left', padx=(6, 0))
        self._cancel_btn = tk.Button(
                  ftp_status_row, text='\u2715 Cancel Upload',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=DANGER,
                  activebackground=BORDER, activeforeground=DANGER,
                  relief='flat', bd=0, padx=10, pady=4,
                  cursor='hand2',
                  command=self._cancel_ftp_upload)
        # Hidden until upload starts

        # ── Notifications & Discord ──
        tk.Frame(f, bg=BORDER, height=1).pack(fill='x', padx=12, pady=(4, 0))
        notif_title = tk.Frame(f, bg=SETTINGS_BG)
        notif_title.pack(fill='x', padx=12, pady=(6, 4))
        tk.Label(notif_title, text='NOTIFICATIONS', font=('Segoe UI', 8),
                 bg=SETTINGS_BG, fg=MUTED).pack(side='left')

        sound_row = tk.Frame(f, bg=SETTINGS_BG)
        sound_row.pack(fill='x', padx=12, pady=(0, 8))
        tk.Checkbutton(sound_row, text='Play sound when build / upload completes',
                       variable=self._sound_var,
                       font=('Segoe UI', 9), bg=SETTINGS_BG, fg=MUTED,
                       activebackground=SETTINGS_BG, activeforeground=TEXT,
                       selectcolor=SURFACE2, cursor='hand2',
                       command=self._save_extra_settings).pack(side='left')

        # ── Build options ──
        tk.Frame(f, bg=BORDER, height=1).pack(fill='x', padx=12, pady=(4, 0))
        build_title = tk.Frame(f, bg=SETTINGS_BG)
        build_title.pack(fill='x', padx=12, pady=(6, 4))
        tk.Label(build_title, text='BUILD OPTIONS', font=('Segoe UI', 8),
                 bg=SETTINGS_BG, fg=MUTED).pack(side='left')

        retry_row = tk.Frame(f, bg=SETTINGS_BG)
        retry_row.pack(fill='x', padx=12, pady=(0, 4))
        tk.Label(retry_row, text='Auto-retry failed builds',
                 font=('Segoe UI', 9), bg=SETTINGS_BG, fg=MUTED,
                 width=20, anchor='w').pack(side='left')
        for n in [0, 1, 2, 3, 5]:
            lbl = 'Off' if n == 0 else str(n) + 'x'
            tk.Radiobutton(retry_row, text=lbl, variable=self._retry_var, value=n,
                           font=('Segoe UI', 9), bg=SETTINGS_BG, fg=MUTED,
                           activebackground=SETTINGS_BG, activeforeground=TEXT,
                           selectcolor=SURFACE2, cursor='hand2',
                           command=self._save_extra_settings).pack(side='left', padx=(0, 8))

        # ── PS5 network extras ──
        tk.Frame(f, bg=BORDER, height=1).pack(fill='x', padx=12, pady=(4, 0))
        net_title = tk.Frame(f, bg=SETTINGS_BG)
        net_title.pack(fill='x', padx=12, pady=(6, 4))
        tk.Label(net_title, text='PS5 NETWORK', font=('Segoe UI', 8),
                 bg=SETTINGS_BG, fg=MUTED).pack(side='left')

        autoip_row = tk.Frame(f, bg=SETTINGS_BG)
        autoip_row.pack(fill='x', padx=12, pady=(0, 8))
        tk.Button(autoip_row, text='\U0001f50d Auto-detect PS5 IP',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=ACCENT,
                  activebackground=BORDER, activeforeground=ACCENT,
                  relief='flat', bd=0, padx=10, pady=4,
                  cursor='hand2',
                  command=self._auto_detect_ip).pack(side='left')
        self._autoip_status = tk.Label(autoip_row, text='',
                                        font=('Segoe UI', 8),
                                        bg=SETTINGS_BG, fg=MUTED)
        self._autoip_status.pack(side='left', padx=(10, 0))

        # ── Theme ──
        tk.Frame(f, bg=BORDER, height=1).pack(fill='x', padx=12, pady=(4, 0))
        theme_row = tk.Frame(f, bg=SETTINGS_BG)
        theme_row.pack(fill='x', padx=12, pady=(6, 8))
        tk.Label(theme_row, text='THEME',
                 font=('Segoe UI', 8), bg=SETTINGS_BG, fg=MUTED,
                 width=10, anchor='w').pack(side='left')
        tk.Radiobutton(theme_row, text='Dark', variable=self._theme_var,
                       value='dark', font=('Segoe UI', 9),
                       bg=SETTINGS_BG, fg=MUTED,
                       activebackground=SETTINGS_BG, activeforeground=TEXT,
                       selectcolor=SURFACE2, cursor='hand2',
                       command=self._toggle_theme).pack(side='left', padx=(12, 8))
        tk.Radiobutton(theme_row, text='Light', variable=self._theme_var,
                       value='light', font=('Segoe UI', 9),
                       bg=SETTINGS_BG, fg=MUTED,
                       activebackground=SETTINGS_BG, activeforeground=TEXT,
                       selectcolor=SURFACE2, cursor='hand2',
                       command=self._toggle_theme).pack(side='left')

    def _browse_temp(self):
        p = filedialog.askdirectory(title='Select temp folder for image building')
        if not p:
            return
        p = p.replace('/', '\\')
        self._temp_dir_var.set(p)
        self._settings['temp_dir'] = p
        save_settings(self._settings)
        self._refresh_temp_size()
        messagebox.showinfo('Temp folder set',
            'Temp folder set to:\n' + p +
            '\n\nThis will take effect on next launch.')

    def _reset_temp(self):
        self._temp_dir_var.set('')
        self._settings.pop('temp_dir', None)
        save_settings(self._settings)
        self._refresh_temp_size()
        self._set_status('Temp folder reset to system default', MUTED)

    def _clear_temp(self):
        base = self._settings.get('temp_dir') or tempfile.gettempdir()
        if not messagebox.askyesno('Clear temp files',
                'This will delete all exfat_builder_* folders in:\n' + base +
                '\n\nAny active build will be interrupted. Continue?'):
            return
        # Don\'t delete the currently active temp dir if building
        if self._building:
            messagebox.showwarning('Building',
                'Cannot clear temp while a build is in progress.')
            return
        ok, result, size = clear_temp_folder(base)
        if ok:
            freed = size / (1024 * 1024)
            msg = ('Removed %d folder(s), freed %.1f MB' % (result, freed)
                   if result > 0 else 'No temp folders found to remove')
            self._set_status(msg, SUCCESS)
            messagebox.showinfo('Temp cleared', msg)
        else:
            messagebox.showerror('Error', 'Failed to clear temp:\n' + str(result))
        self._refresh_temp_size()

    def _refresh_temp_size(self):
        base = self._settings.get('temp_dir') or tempfile.gettempdir()
        total_size = 0
        count = 0
        try:
            for entry in os.scandir(base):
                if entry.name.startswith('exfat_builder_') and entry.is_dir():
                    count += 1
                    for root, dirs, files in os.walk(entry.path):
                        for fn in files:
                            try:
                                total_size += os.path.getsize(os.path.join(root, fn))
                            except Exception:
                                pass
        except Exception:
            pass
        location = self._settings.get('temp_dir') or 'System temp'
        if count > 0:
            mb = total_size / (1024 * 1024)
            self._temp_size_var.set(
                location + '  —  ' + str(count) + ' folder(s), %.1f MB used' % mb)
            self._temp_size_lbl.config(fg=WARNING if mb > 500 else MUTED)
        else:
            self._temp_size_var.set(location + '  —  No temp files')
            self._temp_size_lbl.config(fg=MUTED)

    # ── Field helper ─────────────────────────────────────────────────────────
    def _field(self, parent, label, var, browse_cmd, drag_drop=False):
        row = tk.Frame(parent, bg=BG)
        row.pack(fill='x', pady=(0, 6))
        tk.Label(row, text=label, font=('Segoe UI', 9),
                 bg=BG, fg=MUTED, anchor='w').pack(fill='x')
        inner = tk.Frame(row, bg=BG)
        inner.pack(fill='x', pady=(3, 0))
        ef = tk.Frame(inner, bg=FIELD_BG,
                      highlightbackground=BORDER, highlightthickness=1)
        ef.pack(side='left', fill='x', expand=True)
        entry = tk.Entry(ef, textvariable=var, font=('Consolas', 10),
                 bg=FIELD_BG, fg=FIELD_FG, insertbackground=FIELD_FG,
                 selectbackground=FIELD_SEL_BG, selectforeground=FIELD_SEL_FG,
                 relief='flat', bd=6, state='readonly')
        entry.pack(fill='x')
        if drag_drop:
            entry.drop_target_register = lambda *a: None
            ef.bind('<Button-1>', lambda e: browse_cmd())
            # Drag and drop via tkinterdnd2 if available, else just hint
            try:
                ef.drop_target_register('DND_Files')
                ef.dnd_bind('<<Drop>>', lambda e: self._on_drop(e.data))
            except Exception:
                pass
        tk.Button(inner, text='Browse', font=('Segoe UI', 9),
                  bg=SURFACE2, fg=TEXT, activebackground=BORDER,
                  activeforeground=TEXT, relief='flat', bd=0,
                  padx=14, pady=6, cursor='hand2',
                  command=browse_cmd).pack(side='left', padx=(6, 0))
        return entry

    def _on_drop(self, data):
        # Strip braces that tkinterdnd2 adds around paths with spaces
        path = data.strip('{}').strip('"').strip()
        if os.path.isdir(path):
            self.game_folder.set(path)
            self._detect_game_info(path)

    def _browse_game(self):
        p = filedialog.askdirectory(
            title='Select game root folder (containing eboot.bin)')
        if not p:
            return
        p_display = p.replace('/', '\\')
        self.game_folder.set(p_display)
        self._detect_game_info(p)

    def _get_folder_size(self, folder):
        total = 0
        try:
            for root, dirs, files in os.walk(folder):
                for f in files:
                    try:
                        total += os.path.getsize(os.path.join(root, f))
                    except Exception:
                        pass
        except Exception:
            pass
        return total

    def _load_cover_art(self, folder):
        folder = os.path.normpath(folder)
        for p in [
            os.path.join(folder, 'sce_sys', 'icon0.png'),
            os.path.join(folder, 'icon0.png'),
        ]:
            if os.path.isfile(p):
                return p
        # Also check one level deep
        try:
            for entry in os.scandir(folder):
                if entry.is_dir():
                    for name in ['sce_sys/icon0.png', 'icon0.png']:
                        p = os.path.join(entry.path, name)
                        if os.path.isfile(p):
                            return p
        except Exception:
            pass
        return None

    def _detect_game_info(self, folder):
        folder = os.path.normpath(folder)
        title, title_id, version = get_game_info(folder)

        # Fallback: extract PPSA/CUSA from folder name
        if not title_id:
            m = re.search(r'((?:PPSA|CUSA|PPLH)\d{5})', os.path.basename(folder), re.IGNORECASE)
            if m:
                title_id = m.group(1).upper()

        self._detected_title    = title
        self._detected_title_id = title_id
        self._detected_version  = version

        # ── Cover art ──
        cover_path = self._load_cover_art(folder)
        if cover_path:
            try:
                from PIL import Image, ImageTk
                img = Image.open(cover_path).resize((56, 56), Image.LANCZOS)
                self._cover_image = ImageTk.PhotoImage(img)
                self._cover_label.config(image=self._cover_image, text='', width=56, height=56)
            except Exception:
                self._cover_label.config(image='', text='\U0001f3ae',
                                          font=('Segoe UI', 20), fg=INFO_FG)
        else:
            self._cover_label.config(image='', text='\U0001f3ae',
                                      font=('Segoe UI', 20), fg='#333333')

        # ── Folder size ──
        def calc_size():
            size_bytes = self._get_folder_size(folder)
            size_gb = size_bytes / (1024 ** 3)
            size_str = ('%.2f GB' % size_gb) if size_gb >= 1 else ('%.0f MB' % (size_bytes / (1024**2)))
            # Check output drive has enough space
            space_warn = ''
            out_dir = self.output_dir.get().strip()
            if out_dir:
                try:
                    free = shutil.disk_usage(out_dir).free
                    if free < size_bytes * 1.1:
                        space_warn = '  \u26a0 Low space on output drive!'
                except Exception:
                    pass
            self.after(0, self._info_size_var.set,
                       '\U0001f4be ' + size_str + ' estimated image size' + space_warn)
        threading.Thread(target=calc_size, daemon=True).start()

        if title or title_id:
            self._info_title_var.set(title or title_id)
            self._info_id_var.set(title_id or '')
            self._info_ver_var.set(('Version ' + version) if version else '')
            self._info_frame.pack(fill='x', pady=(0, 8))
            auto_name = build_exfat_name(title, title_id, version)
            self.output_name.set(auto_name)
            self._auto_label.pack(side='left')
            self._no_sfo_label.pack_forget()
            self._filename_preview.config(fg='#ffffff')
            src = 'param.sfo' if title else 'folder name'
            pfs_path = find_meta_file(os.path.normpath(folder), ['pfs-version.dat'])
            if pfs_path and os.path.isfile(pfs_path):
                src = 'param.sfo + pfs-version.dat' if title else 'folder name'
            self._set_status('Detected: ' + (title or title_id) + '  (from ' + src + ')', SUCCESS)
        else:
            self._info_frame.pack_forget()
            self._auto_label.pack_forget()
            self.output_name.set('game.exfat')
            self._no_sfo_label.pack(side='left')
            self._filename_preview.config(fg='#555555')
            self._info_size_var.set('')
            has_eboot = os.path.isfile(os.path.join(folder, 'eboot.bin'))
            self._set_status(
                'eboot.bin found  \u2013  no metadata detected' if has_eboot
                else 'Warning: eboot.bin not found', WARNING)

    def _browse_output(self):
        p = filedialog.askdirectory(title='Select output directory')
        if p:
            p = p.replace('/', '\\')
            self.output_dir.set(p)
            self._settings['output_dir'] = p
            save_settings(self._settings)

    # ── Queue ─────────────────────────────────────────────────────────────────
    def _add_to_queue(self):
        game = self.game_folder.get().strip()
        odir = self.output_dir.get().strip()
        name = self.output_name.get().strip() or 'game.exfat'
        if not name.lower().endswith('.exfat'):
            name += '.exfat'
        if not game:
            messagebox.showwarning('Missing', 'Please select a game folder.')
            return
        if not odir:
            messagebox.showwarning('Missing', 'Please select an output directory.')
            return
        # ── Duplicate detection ──
        for existing in self._queue:
            if os.path.normpath(existing.game_folder) == os.path.normpath(game):
                if not messagebox.askyesno('Duplicate detected',
                        'This game folder is already in the queue:\n' + game +
                        '\n\nAdd it again anyway?'):
                    return
                break
        out_path = os.path.join(odir, name)
        if os.path.isfile(out_path):
            if not messagebox.askyesno('File exists',
                    'Output file already exists:\n' + out_path +
                    '\n\nIt will be overwritten. Continue?'):
                return
        # ── Disk space check ──
        try:
            free = shutil.disk_usage(odir).free
            game_size = self._get_folder_size(game)
            if game_size > 0 and free < game_size * 1.05:
                free_gb = free / (1024**3)
                need_gb = game_size / (1024**3)
                if not messagebox.askyesno('Low disk space',
                        'Output drive may not have enough space.\n\n'
                        'Estimated needed: %.1f GB\nFree: %.1f GB\n\nContinue anyway?' % (need_gb, free_gb)):
                    return
        except Exception:
            pass
        item = QueueItem(game, odir, name,
                         self._detected_title,
                         self._detected_title_id,
                         self._detected_version)
        self._queue.append(item)
        self._render_queue()
        # Track recently used
        game = self.game_folder.get().strip()
        if game:
            self._add_to_recent(game)
        self.game_folder.set('')
        # Keep output_dir so user doesn't have to re-select it each time
        self.output_name.set('game.exfat')
        self._info_frame.pack_forget()
        self._auto_label.pack_forget()
        self._no_sfo_label.pack_forget()
        self._filename_preview.config(fg='#555555')
        self._detected_title = self._detected_title_id = self._detected_version = None
        self._set_status(str(len(self._queue)) + ' item(s) in queue', ACCENT)

    def _remove_queue_item(self, index):
        if 0 <= index < len(self._queue):
            del self._queue[index]
            self._render_queue()

    def _clear_queue(self):
        if self._building:
            messagebox.showwarning('Building', 'Cannot clear queue while building.')
            return
        self._queue.clear()
        self._render_queue()

    def _render_queue(self):
        for w in self._queue_frame.winfo_children():
            w.destroy()
        self._queue_count_var.set(str(len(self._queue)) + ' items')
        if not self._queue:
            tk.Label(self._queue_frame,
                     text='No items yet  \u2014  add a game above',
                     font=('Segoe UI', 9), bg=SURFACE2, fg=MUTED,
                     pady=14).pack()
            return
        for i, item in enumerate(self._queue):
            row_bg = QUEUE_ODD if i % 2 == 0 else QUEUE_EVEN
            row = tk.Frame(self._queue_frame, bg=row_bg)
            row.pack(fill='x')
            dot_color = {'waiting': MUTED, 'building': ACCENT,
                         'done': SUCCESS, 'failed': DANGER}.get(item.status, MUTED)
            dot = tk.Canvas(row, width=10, height=10, bg=row_bg,
                            highlightthickness=0)
            dot.pack(side='left', padx=(10, 6), pady=9)
            dot.create_oval(1, 1, 9, 9, fill=dot_color, outline='')
            display = item.game_title or os.path.basename(item.game_folder)
            if item.version:
                display += '  v' + item.version
            # Status badge for done/failed
            if item.status == 'done':
                tk.Label(row, text='\u2713', font=('Segoe UI', 9),
                         bg=row_bg, fg=SUCCESS).pack(side='left', padx=(0, 4))
            elif item.status == 'failed':
                tk.Label(row, text='\u2717', font=('Segoe UI', 9),
                         bg=row_bg, fg=DANGER).pack(side='left', padx=(0, 4))
            tk.Label(row, text=display, font=('Segoe UI', 9, 'bold'),
                     bg=row_bg, fg=TEXT, anchor='w').pack(side='left')
            tk.Label(row, text='  \u2192  ' + item.output_name,
                     font=('Consolas', 8), bg=row_bg, fg=INFO_FG,
                     anchor='w').pack(side='left')
            tk.Label(row, text=item.output_dir,
                     font=('Segoe UI', 8), bg=row_bg, fg=MUTED,
                     anchor='w').pack(side='left', padx=(6, 0), fill='x', expand=True)
            idx = i
            # Open output folder button (only when done)
            if item.status == 'done':
                out_path = os.path.join(item.output_dir, item.output_name)
                tk.Button(row, text='\U0001f4c2', font=('Segoe UI', 10),
                          bg=row_bg, fg=MUTED,
                          activebackground=row_bg, activeforeground=TEXT,
                          relief='flat', bd=0, padx=6, pady=4,
                          cursor='hand2',
                          command=lambda p=item.output_dir: subprocess.Popen(
                              'explorer "' + p + '"', shell=True)
                          ).pack(side='right', padx=(0, 2))
                # Upload to PS5 button
                tk.Button(row, text='\u2191 PS5', font=('Segoe UI', 8),
                          bg=row_bg, fg=ACCENT,
                          activebackground=row_bg, activeforeground=ACCENT,
                          relief='flat', bd=0, padx=6, pady=4,
                          cursor='hand2',
                          command=lambda p=out_path: self._ftp_upload_file(p)
                          ).pack(side='right', padx=(0, 2))
            tk.Button(row, text='\u00d7', font=('Segoe UI', 10),
                      bg=row_bg, fg=DANGER,
                      activebackground=row_bg, activeforeground=DANGER,
                      relief='flat', bd=0, padx=8, pady=4,
                      cursor='hand2',
                      command=lambda ix=idx: self._remove_queue_item(ix)
                      ).pack(side='right', padx=(0, 4))
            # Right-click context menu
            def _show_ctx(event, it=item, ix=idx):
                menu = tk.Menu(self, tearoff=0, bg=SURFACE2, fg=TEXT,
                               activebackground=ACCENT, activeforeground=TEXT,
                               font=('Segoe UI', 9))
                menu.add_command(label='\U0001f4c2  Open source folder',
                    command=lambda: subprocess.Popen(
                        'explorer "' + it.game_folder + '"', shell=True))
                menu.add_command(label='\U0001f4c2  Open output folder',
                    command=lambda: subprocess.Popen(
                        'explorer "' + it.output_dir + '"', shell=True))
                if it.status in ('waiting', 'failed'):
                    menu.add_separator()
                    menu.add_command(label='\u25b6  Build this item',
                        command=lambda: self._build_single(ix))
                if it.status == 'done':
                    menu.add_separator()
                    menu.add_command(label='\u2191  Upload to PS5',
                        command=lambda: self._ftp_upload_file(
                            os.path.join(it.output_dir, it.output_name)))
                menu.add_separator()
                menu.add_command(label='\u2717  Remove from queue',
                    command=lambda: self._remove_queue_item(ix))
                menu.tk_popup(event.x_root, event.y_root)
            row.bind('<Button-3>', _show_ctx)
            for child in row.winfo_children():
                child.bind('<Button-3>', _show_ctx)

    def _update_queue_dot(self, index, status):
        if 0 <= index < len(self._queue):
            self._queue[index].status = status
            self._render_queue()

    def _build_single(self, index):
        if self._building:
            messagebox.showwarning('Busy', 'A build is already in progress.')
            return
        if 0 <= index < len(self._queue):
            self._queue[index].status = 'waiting'
            self._building = True
            self.build_btn.config(state='disabled', text='Building...')
            self._process_next([index], 0)

    def _on_queue_drop(self, event):
        paths = event.data.strip()
        # tkinterdnd2 may wrap multiple paths in braces
        import re as _re
        items = _re.findall(r'\{([^}]+)\}|(\S+)', paths)
        folders = [a or b for a, b in items]
        if not folders:
            folders = [paths.strip('{}')]
        for folder in folders:
            folder = folder.strip()
            if os.path.isdir(folder):
                self.game_folder.set(folder)
                self._detect_game_info(folder)
                self._add_to_queue()

    # ── Build history ──────────────────────────────────────────────────────────
    def _history_path(self):
        return os.path.join(os.path.expanduser('~'), '.exfat_builder_history.json')

    def _save_history(self, item, out_path, success):
        try:
            try:
                with open(self._history_path(), 'r') as f:
                    history = json.load(f)
            except Exception:
                history = []
            history.insert(0, {
                'title':   item.game_title or os.path.basename(item.game_folder),
                'title_id': item.title_id or '',
                'version': item.version or '',
                'output':  out_path,
                'success': success,
                'time':    time.strftime('%Y-%m-%d %H:%M:%S'),
            })
            history = history[:50]  # keep last 50
            with open(self._history_path(), 'w') as f:
                json.dump(history, f, indent=2)
        except Exception:
            pass

    # ── PS5 network ───────────────────────────────────────────────────────────
    def _ftp_ping(self, callback):
        ip = self._ftp_ip_var.get().strip()
        if not ip:
            callback(False, 'No IP set')
            return
        def worker():
            try:
                import socket
                port = int(self._ftp_port_var.get().strip() or '2121')
                s = socket.create_connection((ip, port), timeout=3)
                s.close()
                self.after(0, callback, True, 'PS5 reachable at ' + ip)
            except Exception as e:
                self.after(0, callback, False, str(e))
        threading.Thread(target=worker, daemon=True).start()

    def _show_ps5_browser(self):
        ip = self._ftp_ip_var.get().strip()
        if not ip:
            messagebox.showwarning('No IP', 'Enter your PS5 IP address in Settings first.')
            return

        win = tk.Toplevel(self)
        win.title('PS5 FTP Manager — ' + ip)
        win.geometry('780x580')
        win.configure(bg=BG)
        win.transient(self)

        # ── State ──
        path_var    = tk.StringVar(value=self._ftp_path_var.get().strip() or '/')
        status_var  = tk.StringVar(value='Connecting...')
        ftp_ref     = [None]
        entries     = []   # list of (name, is_dir, size)
        clipboard   = [None]  # (action, remote_path) for cut/copy
        sel_mode    = tk.StringVar(value='extended')

        # ── Header ──
        hdr = tk.Frame(win, bg=BG)
        hdr.pack(fill='x', padx=16, pady=(12, 4))
        tk.Label(hdr, text='PS5 FTP Manager',
                 font=('Segoe UI', 13, 'bold'), bg=BG, fg=TEXT).pack(side='left')
        tk.Label(hdr, textvariable=status_var,
                 font=('Segoe UI', 8), bg=BG, fg=MUTED).pack(side='right')

        # ── Path / breadcrumb bar ──
        path_row = tk.Frame(win, bg=BG)
        path_row.pack(fill='x', padx=16, pady=(0, 4))
        tk.Button(path_row, text='\u2191 Up',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=TEXT,
                  relief='flat', bd=0, padx=8, pady=4, cursor='hand2',
                  command=lambda: _go_up()).pack(side='left', padx=(0, 6))
        pf = tk.Frame(path_row, bg=FIELD_BG,
                      highlightbackground=BORDER, highlightthickness=1)
        pf.pack(side='left', fill='x', expand=True)
        path_entry = tk.Entry(pf, textvariable=path_var,
                              font=('Consolas', 9), bg=FIELD_BG, fg=FIELD_FG,
                              insertbackground=FIELD_FG, relief='flat', bd=4)
        path_entry.pack(fill='x')
        path_entry.bind('<Return>', lambda e: _load(path_var.get()))
        tk.Button(path_row, text='Go',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=TEXT,
                  relief='flat', bd=0, padx=10, pady=4, cursor='hand2',
                  command=lambda: _load(path_var.get())).pack(side='left', padx=(6, 0))
        tk.Button(path_row, text='\U0001f504',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=TEXT,
                  relief='flat', bd=0, padx=8, pady=4, cursor='hand2',
                  command=lambda: _load(path_var.get())).pack(side='left', padx=(4, 0))

        # ── Toolbar ──
        toolbar = tk.Frame(win, bg=BG)
        toolbar.pack(fill='x', padx=16, pady=(0, 4))

        def tbtn(text, cmd, fg=TEXT, bg=SURFACE2):
            tk.Button(toolbar, text=text, font=('Segoe UI', 8),
                      bg=bg, fg=fg, activebackground=BORDER,
                      activeforeground=TEXT, relief='flat', bd=0,
                      padx=8, pady=3, cursor='hand2',
                      command=cmd).pack(side='left', padx=(0, 3))

        tbtn('\U0001f4c2 New folder', lambda: _new_folder())
        tk.Frame(toolbar, bg=BORDER, width=1).pack(side='left', fill='y', padx=4)
        tbtn('\u2702 Rename',  lambda: _rename())
        tbtn('\u2702\ufe0f Cut',    lambda: _cut())
        tbtn('\U0001f4cb Copy path', lambda: _copy_path())
        tbtn('\U0001f4cb Paste',    lambda: _paste())
        tk.Frame(toolbar, bg=BORDER, width=1).pack(side='left', fill='y', padx=4)
        tbtn('\u2935 Download', lambda: _download())
        tk.Frame(toolbar, bg=BORDER, width=1).pack(side='left', fill='y', padx=4)
        tbtn('\U0001f5d1 Delete', lambda: _delete(), fg=DANGER)

        # Storage indicator
        self._ps5_free_var = tk.StringVar(value='')
        tk.Label(toolbar, textvariable=self._ps5_free_var,
                 font=('Segoe UI', 8), bg=BG, fg=INFO_FG,
                 anchor='e').pack(side='right')

        # ── File list with columns ──
        list_outer = tk.Frame(win, bg=SURFACE2,
                              highlightbackground=BORDER, highlightthickness=1)
        list_outer.pack(fill='both', expand=True, padx=16, pady=(0, 6))

        # Column header
        col_hdr = tk.Frame(list_outer, bg='#0f0f0f')
        col_hdr.pack(fill='x')
        for col_text, col_w in [('Name', 50), ('Size', 12), ('Type', 10)]:
            tk.Label(col_hdr, text=col_text,
                     font=('Segoe UI', 8, 'bold'), bg='#0f0f0f', fg=MUTED,
                     anchor='w', width=col_w).pack(side='left', padx=(8, 0), pady=3)

        listbox = tk.Listbox(list_outer, font=('Consolas', 9),
                             bg=SURFACE2, fg=TEXT,
                             selectbackground=ACCENT, selectforeground='#ffffff',
                             activestyle='none', relief='flat', bd=6,
                             selectmode='extended')
        sb = tk.Scrollbar(list_outer, command=listbox.yview,
                          bg=SURFACE2, troughcolor=BG)
        listbox.configure(yscrollcommand=sb.set)
        sb.pack(side='right', fill='y')
        listbox.pack(fill='both', expand=True)

        # ── Status bar ──
        stat_bar = tk.Frame(win, bg='#0a0a0a')
        stat_bar.pack(fill='x', padx=16, pady=(0, 4))
        self._ps5_sel_var = tk.StringVar(value='')
        tk.Label(stat_bar, textvariable=self._ps5_sel_var,
                 font=('Segoe UI', 8), bg='#0a0a0a', fg=MUTED,
                 anchor='w').pack(side='left', padx=8, pady=3)

        # ── Bottom buttons ──
        btn_row = tk.Frame(win, bg=BG)
        btn_row.pack(fill='x', padx=16, pady=(0, 12))
        tk.Button(btn_row, text='Close', font=('Segoe UI', 9),
                  bg=SURFACE2, fg=MUTED, relief='flat', bd=0,
                  padx=10, pady=5, cursor='hand2',
                  command=lambda: (_ftp_quit(), win.destroy())
                  ).pack(side='right')

        # ── Helper: parse LIST line ──
        def _parse_line(line):
            parts = line.split()
            if len(parts) < 9:
                return None, None, 0
            name    = ' '.join(parts[8:])
            is_dir  = line.startswith('d')
            try:    sz = int(parts[4])
            except: sz = 0
            return name, is_dir, sz

        def _fmt_size(sz, is_dir):
            if is_dir: return '<DIR>'
            if sz >= 1024**3: return '%.2f GB' % (sz/1024**3)
            if sz >= 1024**2: return '%.1f MB' % (sz/1024**2)
            if sz >= 1024:    return '%d KB'   % (sz//1024)
            return '%d B' % sz

        def _ftp_quit():
            try:
                if ftp_ref[0]: ftp_ref[0].quit()
            except Exception: pass
            ftp_ref[0] = None

        def _ensure_ftp():
            try:
                if ftp_ref[0]:
                    ftp_ref[0].voidcmd('NOOP')
                    return ftp_ref[0]
            except Exception:
                pass
            ftp_ref[0] = self._ftp_connect()
            return ftp_ref[0]

        def _load(p):
            p = p or '/'
            status_var.set('Loading...')
            listbox.delete(0, 'end')
            entries.clear()

            def worker():
                try:
                    ftp  = _ensure_ftp()
                    lines = []
                    ftp.retrlines('LIST ' + p, lines.append)
                    # Try get free space via SITE commands (may not work on PS5)
                    free_str = ''
                    try:
                        resp = ftp.sendcmd('SITE AVAIL')
                        free_str = resp
                    except Exception:
                        pass
                    win.after(0, _show_items, lines, p, free_str)
                except Exception as e:
                    win.after(0, status_var.set, 'Error: ' + str(e))
            threading.Thread(target=worker, daemon=True).start()

        def _show_items(lines, p, free_str):
            listbox.delete(0, 'end')
            entries.clear()
            path_var.set(p)
            if free_str:
                self._ps5_free_var.set('Free: ' + free_str)

            parsed = []
            for line in lines:
                name, is_dir, sz = _parse_line(line)
                if name and name not in ('.', '..'):
                    parsed.append((name, is_dir, sz))
            # Dirs first
            parsed.sort(key=lambda x: (not x[1], x[0].lower()))

            if p != '/':
                entries.append(('..', True, 0))
                listbox.insert('end', '\U0001f4c2  ..')

            for name, is_dir, sz in parsed:
                entries.append((name, is_dir, sz))
                icon  = '\U0001f4c1  ' if is_dir else '\U0001f4be  '
                sz_str = _fmt_size(sz, is_dir)
                listbox.insert('end',
                    '%-3s%-42s %12s' % (icon, name, sz_str))

            status_var.set('%d items in %s' % (len(parsed), p))
            self._ps5_sel_var.set('')

        def _get_selected():
            sels = listbox.curselection()
            result = []
            for i in sels:
                if i < len(entries):
                    name, is_dir, sz = entries[i]
                    if name != '..':
                        result.append((name, is_dir, sz))
            return result

        def _current_path():
            return path_var.get().rstrip('/')

        def _go_up():
            cur = _current_path()
            if cur == '': return
            parent = '/'.join(cur.split('/')[:-1]) or '/'
            _load(parent)

        # ── Navigation ──
        def _nav():
            sel = listbox.curselection()
            if not sel: return
            if sel[0] >= len(entries): return
            name, is_dir, _ = entries[sel[0]]
            if not is_dir: return
            if name == '..':
                _go_up()
            else:
                _load(_current_path() + '/' + name)

        listbox.bind('<Double-Button-1>', lambda e: _nav())
        listbox.bind('<<ListboxSelect>>', lambda e: _update_sel_bar())

        def _update_sel_bar():
            sel = _get_selected()
            if not sel:
                self._ps5_sel_var.set('')
                return
            total_sz = sum(sz for _, _, sz in sel if not _)
            self._ps5_sel_var.set(
                '%d selected' % len(sel) +
                ('  —  ' + _fmt_size(total_sz, False) if total_sz else ''))

        # ── New folder ──
        def _new_folder():
            name = simpledialog.askstring('New folder', 'Folder name:', parent=win)
            if not name: return
            remote = _current_path() + '/' + name
            def worker():
                try:
                    ftp = _ensure_ftp()
                    ftp.mkd(remote)
                    win.after(0, _load, path_var.get())
                except Exception as e:
                    win.after(0, messagebox.showerror, 'Error', str(e))
            threading.Thread(target=worker, daemon=True).start()

        # ── Rename ──
        def _rename():
            sel = _get_selected()
            if not sel:
                messagebox.showwarning('No selection', 'Select a file or folder to rename.', parent=win)
                return
            if len(sel) > 1:
                messagebox.showwarning('Too many', 'Select only one item to rename.', parent=win)
                return
            name, is_dir, _ = sel[0]
            new_name = simpledialog.askstring('Rename', 'New name:', initialvalue=name, parent=win)
            if not new_name or new_name == name: return
            old_path = _current_path() + '/' + name
            new_path = _current_path() + '/' + new_name
            def worker():
                try:
                    ftp = _ensure_ftp()
                    ftp.rename(old_path, new_path)
                    win.after(0, _load, path_var.get())
                    win.after(0, status_var.set, 'Renamed: ' + name + ' → ' + new_name)
                except Exception as e:
                    win.after(0, messagebox.showerror, 'Rename failed', str(e))
            threading.Thread(target=worker, daemon=True).start()

        # ── Cut (move) ──
        def _cut():
            sel = _get_selected()
            if not sel:
                messagebox.showwarning('No selection', 'Select items to move.', parent=win)
                return
            paths = [(_current_path() + '/' + name, is_dir) for name, is_dir, _ in sel]
            clipboard[0] = ('move', paths)
            status_var.set(str(len(paths)) + ' item(s) cut — navigate to destination and click Paste')

        # ── Copy path to clipboard ──
        def _copy_path():
            sel = _get_selected()
            if not sel: return
            paths = '\n'.join(_current_path() + '/' + name for name, _, _ in sel)
            win.clipboard_clear()
            win.clipboard_append(paths)
            status_var.set('Path copied to clipboard')

        # ── Paste (move) ──
        def _paste():
            if not clipboard[0]:
                messagebox.showwarning('Nothing to paste', 'Cut some files first.', parent=win)
                return
            action, items = clipboard[0]
            dest = _current_path()
            if action == 'move':
                def worker():
                    errors = []
                    ftp = _ensure_ftp()
                    for src_path, is_dir in items:
                        name     = src_path.split('/')[-1]
                        dst_path = dest + '/' + name
                        try:
                            ftp.rename(src_path, dst_path)
                        except Exception as e:
                            errors.append(name + ': ' + str(e))
                    clipboard[0] = None
                    win.after(0, _load, path_var.get())
                    if errors:
                        win.after(0, messagebox.showerror, 'Some moves failed',
                                   '\n'.join(errors))
                    else:
                        win.after(0, status_var.set,
                                   str(len(items)) + ' item(s) moved to ' + dest)
                threading.Thread(target=worker, daemon=True).start()

        # ── Delete ──
        def _delete():
            sel = _get_selected()
            if not sel:
                messagebox.showwarning('No selection', 'Select files or folders to delete.', parent=win)
                return
            names = [name for name, _, _ in sel]
            if not messagebox.askyesno('Delete from PS5',
                    'Permanently delete %d item(s) from PS5?\n\n%s%s' % (
                        len(names),
                        '\n'.join(names[:8]),
                        '\n...' if len(names) > 8 else ''),
                    parent=win):
                return
            def worker():
                errors = []
                ftp = _ensure_ftp()
                for name, is_dir, _ in sel:
                    remote = _current_path() + '/' + name
                    try:
                        if is_dir:
                            _rmd_recursive(ftp, remote)
                        else:
                            ftp.delete(remote)
                    except Exception as e:
                        errors.append(name + ': ' + str(e))
                win.after(0, _load, path_var.get())
                if errors:
                    win.after(0, messagebox.showerror, 'Some deletions failed',
                               '\n'.join(errors))
                else:
                    win.after(0, status_var.set,
                               'Deleted ' + str(len(sel)) + ' item(s)')
            threading.Thread(target=worker, daemon=True).start()

        def _rmd_recursive(ftp, path):
            """Recursively delete a directory over FTP."""
            lines = []
            try:
                ftp.retrlines('LIST ' + path, lines.append)
            except Exception:
                pass
            for line in lines:
                name, is_dir, _ = _parse_line(line)
                if not name or name in ('.', '..'): continue
                child = path + '/' + name
                if is_dir:
                    _rmd_recursive(ftp, child)
                else:
                    try: ftp.delete(child)
                    except Exception: pass
            try: ftp.rmd(path)
            except Exception: pass

        # ── Download ──
        def _download():
            sel = _get_selected()
            if not sel:
                messagebox.showwarning('No selection', 'Select a file to download.', parent=win)
                return
            files_only = [(name, sz) for name, is_dir, sz in sel if not is_dir]
            if not files_only:
                messagebox.showwarning('Files only', 'Select files (not folders) to download.', parent=win)
                return
            save_dir = filedialog.askdirectory(title='Save downloaded files to...', parent=win)
            if not save_dir: return

            status_var.set('Downloading %d file(s)...' % len(files_only))
            def worker():
                errors = []
                ftp = _ensure_ftp()
                for name, sz in files_only:
                    remote = _current_path() + '/' + name
                    local  = os.path.join(save_dir, name)
                    try:
                        with open(local, 'wb') as f:
                            ftp.retrbinary('RETR ' + remote, f.write)
                    except Exception as e:
                        errors.append(name + ': ' + str(e))
                if errors:
                    win.after(0, messagebox.showerror, 'Download errors', '\n'.join(errors))
                    win.after(0, status_var.set, 'Download finished with errors')
                else:
                    win.after(0, status_var.set,
                               '%d file(s) downloaded to %s' % (len(files_only), save_dir))
            threading.Thread(target=worker, daemon=True).start()

        # ── Right-click context menu ──
        def _ctx(event):
            sel_idx = listbox.nearest(event.y)
            listbox.selection_clear(0, 'end')
            listbox.selection_set(sel_idx)
            if sel_idx >= len(entries): return
            name, is_dir, sz = entries[sel_idx]
            if name == '..': return
            remote = _current_path() + '/' + name
            menu = tk.Menu(win, tearoff=0, bg=SURFACE2, fg=TEXT,
                           activebackground=ACCENT, activeforeground=TEXT,
                           font=('Segoe UI', 9))
            menu.add_command(label=name, state='disabled',
                             font=('Segoe UI', 9, 'bold'))
            menu.add_separator()
            if is_dir:
                menu.add_command(label='\U0001f4c2  Open folder',
                                 command=lambda: _load(remote))
            else:
                menu.add_command(label='\u2935  Download',
                                 command=_download)
            menu.add_command(label='\u2702  Rename',
                             command=_rename)
            menu.add_command(label='\u2702\ufe0f  Cut (move)',
                             command=_cut)
            menu.add_separator()
            menu.add_command(label='\U0001f5d1  Delete',
                             command=_delete)
            menu.tk_popup(event.x_root, event.y_root)

        listbox.bind('<Button-3>', _ctx)

        # ── Keyboard shortcuts ──
        win.bind('<Delete>',     lambda e: _delete())
        win.bind('<F2>',         lambda e: _rename())
        win.bind('<F5>',         lambda e: _load(path_var.get()))
        win.bind('<BackSpace>',  lambda e: _go_up())
        win.bind('<Return>',     lambda e: _nav())

        # Start loading
        _load(path_var.get())

    def _on_bar_resize(self, event):
        self._bar_width = event.width
        self._update_bar_visual(self._current_pct)

    def _update_bar_visual(self, pct):
        self._current_pct = pct
        fill_w = int(self._bar_width * pct / 100)
        self._bar_canvas.coords(self._bar_rect, 0, 0, fill_w, 20)
        self._bar_canvas.itemconfig(
            self._bar_rect, fill=SUCCESS if pct >= 100 else ACCENT)

    def _set_progress(self, pct, step_text=None, eta_text=None):
        self._update_bar_visual(pct)
        self._pct_var.set(str(int(pct)) + '%')
        if step_text is not None:
            self._step_label_var.set(step_text)
        if eta_text is not None:
            self._eta_var.set(eta_text)

    def _activate_dot(self, index):
        for i, (dot, lbl) in enumerate(self._stage_dots):
            if i < index:
                dot.itemconfig('dot', fill=SUCCESS)
                lbl.config(fg=SUCCESS)
            elif i == index:
                dot.itemconfig('dot', fill=ACCENT)
                lbl.config(fg=ACCENT)
            else:
                dot.itemconfig('dot', fill=BORDER)
                lbl.config(fg=MUTED)

    def _format_eta(self, elapsed, pct):
        es = 'Elapsed: %dm %02ds' % (int(elapsed // 60), int(elapsed % 60))
        if pct <= 1:
            return es
        remaining = max(0, (elapsed / (pct / 100.0)) - elapsed)
        if remaining < 5:
            return es + '  —  Almost done'
        m, s = int(remaining // 60), int(remaining % 60)
        return es + '  —  ETA: %dm %02ds' % (m, s)

    def _parse_line(self, line):
        m = re.search(r'\[(\d)/4\]', line)
        if m:
            key = m.group(1) + '/4'
            if key in STEPS:
                lo, hi, label = STEPS[key]
                self._activate_dot(int(m.group(1)) - 1)
                return lo, label, 'step'
        m = re.search(r'(\d{1,3})%', line)
        if m:
            robo_pct = int(m.group(1))
            lo, hi, _ = STEPS['3/4']
            mapped = lo + (hi - lo) * robo_pct / 100.0
            eta_m = re.search(r'(\d+):(\d{2}):(\d{2})', line)
            if eta_m:
                h, mn, sc = int(eta_m.group(1)), int(eta_m.group(2)), int(eta_m.group(3))
                total = h * 3600 + mn * 60 + sc
                eta = 'Almost done' if total < 5 else (
                    'ETA: %dh %02dm %02ds' % (h, mn, sc) if h
                    else 'ETA: %dm %02ds' % (mn, sc))
                return mapped, eta, 'robo_eta'
            return mapped, None, 'robo'
        return None, None, None

    # ── Build ─────────────────────────────────────────────────────────────────
    def _run_queue(self):
        if self._building:
            return
        if not self._queue:
            messagebox.showwarning('Empty queue',
                'Add at least one game to the queue first.')
            return
        waiting = [i for i, q in enumerate(self._queue) if q.status == 'waiting']
        if not waiting:
            messagebox.showinfo('All done',
                'All queued items have already been built.')
            return

        # ── Pre-build checklist ──
        issues = []
        # 1. OSFMount installed?
        osf = self._find_osfmount()
        if not osf:
            issues.append('OSFMount is not installed — required for building images.')
        # 2. Per-item checks
        for i in waiting:
            item = self._queue[i]
            label = item.game_title or os.path.basename(item.game_folder)
            # eboot.bin present?
            eboot = os.path.join(item.game_folder, 'eboot.bin')
            if not os.path.isfile(eboot):
                issues.append(label + ': eboot.bin not found in source folder')
            # output drive space?
            try:
                free = shutil.disk_usage(item.output_dir).free
                needed = self._get_folder_size(item.game_folder) * 1.05
                if needed > 0 and free < needed:
                    issues.append(label + ': output drive low on space '
                        '(need %.1f GB, have %.1f GB)' % (needed/1024**3, free/1024**3))
            except Exception:
                pass
            # image already exists?
            out_path = os.path.join(item.output_dir, item.output_name)
            if os.path.isfile(out_path):
                issues.append(label + ': output file already exists — will be overwritten')

        if issues:
            msg = 'Pre-build checklist found the following:\n\n'
            msg += '\n'.join('\u2022 ' + s for s in issues)
            msg += '\n\nContinue anyway?'
            if not messagebox.askyesno('Pre-build checklist', msg):
                return

        self._building = True
        self.build_btn.config(state='disabled', text='Building...')
        self._process_next(waiting, 0)

    def _process_next(self, indices, pos):
        if pos >= len(indices):
            self._building = False
            self.build_btn.config(state='normal', text='Build All')
            self._set_status(
                'All done!  \u2713  ' + str(len(indices)) + ' image(s) built', SUCCESS)
            messagebox.showinfo('Complete',
                str(len(indices)) + ' image(s) built successfully!')
            self._refresh_temp_size()
            return
        idx  = indices[pos]
        item = self._queue[idx]
        self._update_queue_dot(idx, 'building')
        out_path = os.path.join(item.output_dir, item.output_name)
        cmd = '"' + self._bat_path + '" "' + out_path + '" "' + item.game_folder + '"'
        bat_dir = os.path.dirname(self._bat_path)
        total   = len(indices)
        display = item.game_title or os.path.basename(item.game_folder)
        label   = 'Building %d/%d: %s' % (pos + 1, total, display)
        self._set_progress(0, label, '')
        self._activate_dot(-1)
        self._start_time = time.time()
        self._set_status(label, ACCENT)
        self._log_clear()
        self._log('[' + str(pos + 1) + '/' + str(total) + '] ' + cmd + '\n\n')
        os.makedirs(os.path.dirname(out_path), exist_ok=True)

        self._current_build_idx = idx
        self._source_file_count = 0
        # Reset drive tracking for this build
        self._mounted_drive   = None   # e.g. 'E:'
        self._image_total_gb  = None   # total capacity of mounted image
        self._drive_poll_id   = None   # after() id for polling
        self._copy_start_free = None   # free space at copy start (to derive written)

        def worker():
            try:
                proc = subprocess.Popen(
                    cmd, shell=True, cwd=bat_dir,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, encoding='utf-8', errors='replace')
                for line in proc.stdout:
                    self.after(0, self._handle_line, line)
                proc.wait()
                self.after(0, self._stop_drive_poll)
                self.after(0, self._item_done,
                           proc.returncode, idx, indices, pos, out_path)
            except Exception as ex:
                self.after(0, self._stop_drive_poll)
                self.after(0, self._item_error, str(ex), idx, indices, pos)

        threading.Thread(target=worker, daemon=True).start()

    def _item_done(self, returncode, idx, indices, pos, out_path):
        elapsed = time.time() - self._start_time if self._start_time else 0
        m, s = int(elapsed // 60), int(elapsed % 60)
        if returncode == 0:
            self._update_queue_dot(idx, 'done')
            self._set_progress(100, 'Complete!', 'Finished in %dm %02ds' % (m, s))
            self._activate_dot(4)
            self._log('\n[OK] Done: ' + out_path + '\n')
            self._save_history(self._queue[idx], out_path, True)
            # ── Verify image ──
            self._set_status('Verifying image...', ACCENT)
            self._log('[VERIFY] Checking image integrity...\n')
            src_folder = self._queue[idx].game_folder
            def do_verify():
                ok, msg = self._verify_image(out_path, src_folder)
                self.after(0, self._verify_done, ok, msg, idx, indices, pos, out_path)
            threading.Thread(target=do_verify, daemon=True).start()
        else:
            self._update_queue_dot(idx, 'failed')
            self._set_progress(self._current_pct, 'Failed', '')
            self._set_status('Build failed (exit code ' + str(returncode) + ')', DANGER)
            self._save_history(self._queue[idx], out_path, False)
            # ── Auto-retry ──
            retry_max = self._retry_var.get()
            item = self._queue[idx]
            retry_count = getattr(item, '_retry_count', 0)
            if retry_max > 0 and retry_count < retry_max:
                item._retry_count = retry_count + 1
                item.status = 'waiting'
                self._log('\n[RETRY] Attempt %d/%d for %s\n' % (
                    retry_count + 1, retry_max,
                    item.game_title or os.path.basename(item.game_folder)))
                self._set_status('Retrying (%d/%d)...' % (retry_count + 1, retry_max), WARNING)
                self._render_queue()
                self.after(2000, self._process_next, indices, pos)
            else:
                if messagebox.askyesno('Item failed',
                        'Build failed for:\n' + out_path +
                        '\n\nContinue with remaining items?'):
                    self._process_next(indices, pos + 1)
                else:
                    self._building = False
                    self.build_btn.config(state='normal', text='Build All')

    def _item_error(self, msg, idx, indices, pos):
        self._update_queue_dot(idx, 'failed')
        self._set_status('Error: ' + msg, DANGER)
        if messagebox.askyesno('Error',
                'Error running make_image.bat:\n' + msg + '\n\nContinue?'):
            self._process_next(indices, pos + 1)
        else:
            self._building = False
            self.build_btn.config(state='normal', text='Build All')

    def _handle_line(self, line):
        self._log(line)
        pct, extra, kind = self._parse_line(line)
        elapsed = time.time() - self._start_time if self._start_time else 0

        # ── Detect mounted drive letter from PS5 output ──
        if not self._mounted_drive:
            m = re.search(r'logical volume on ([A-Z]:)', line)
            if m:
                self._mounted_drive = m.group(1)

        # ── When copy phase starts, snapshot free space and begin polling ──
        if '[3/4]' in line and self._mounted_drive and self._drive_poll_id is None:
            try:
                usage = shutil.disk_usage(self._mounted_drive + '\\')
                self._image_total_gb  = usage.total / (1024 ** 3)
                self._copy_start_free = usage.free
                self._copy_start_time = time.time()
            except Exception:
                pass
            # Count source files in background so we know the target
            self._source_file_count = 0
            src = self._queue[self._current_build_idx].game_folder \
                if hasattr(self, '_current_build_idx') else None
            if src:
                def _count_src(folder=src):
                    n = sum(len(files) for _, _, files in os.walk(folder))
                    self._source_file_count = n
                threading.Thread(target=_count_src, daemon=True).start()
            self._start_drive_poll()

        if pct is not None:
            if kind == 'step':
                self._set_progress(pct, extra,
                    self._format_eta(elapsed, pct) if pct > 0 else '')
            elif kind == 'robo_eta':
                es = 'Elapsed: %dm %02ds' % (int(elapsed // 60), int(elapsed % 60))
                self._set_progress(pct, None,
                    es + '  \u2014  ' + extra if extra
                    else self._format_eta(elapsed, pct))
            else:
                self._set_progress(pct, None, self._format_eta(elapsed, pct))
        elif self._current_pct > 0 and self._start_time:
            self._eta_var.set(self._format_eta(elapsed, self._current_pct))
    def _find_osfmount(self):
        candidates = [
            r'C:\Program Files\OSFMount\osfmount.com',
            r'C:\Program Files (x86)\OSFMount\osfmount.com',
            r'C:\Program Files\PassMark\OSFMount\osfmount.com',
        ]
        for c in candidates:
            if os.path.isfile(c):
                return c
        import shutil as _sh
        return _sh.which('osfmount.com')

    def _check_osfmount_banner(self):
        if self._find_osfmount():
            return  # All good, no banner needed
        # Show warning banner under header
        banner = tk.Frame(self, bg='#3a1500',
                          highlightbackground='#ff6600', highlightthickness=1)
        banner.pack(fill='x', padx=24, pady=(4, 0))
        inner = tk.Frame(banner, bg='#3a1500')
        inner.pack(fill='x', padx=12, pady=8)
        tk.Label(inner, text='\u26a0  OSFMount not detected',
                 font=('Segoe UI', 9, 'bold'),
                 bg='#3a1500', fg='#ff9944').pack(side='left')
        tk.Label(inner,
                 text='  \u2014  Required for building images.',
                 font=('Segoe UI', 9), bg='#3a1500', fg='#ffbb88').pack(side='left')
        def _open_download():
            import webbrowser
            webbrowser.open('https://www.osforensics.com/tools/mount-disk-images.html')
        tk.Button(inner, text='Download OSFMount \u2197',
                  font=('Segoe UI', 9, 'bold'),
                  bg='#ff6600', fg='#ffffff',
                  activebackground='#cc5500', activeforeground='#ffffff',
                  relief='flat', bd=0, padx=10, pady=3,
                  cursor='hand2',
                  command=_open_download).pack(side='right')

    # ── Verify image ───────────────────────────────────────────────────────────
    def _verify_image(self, out_path, src_folder):
        osf = self._find_osfmount()
        if not osf:
            return True, 'OSFMount not found — skipping verify'
        if not os.path.isfile(out_path):
            return False, 'Output file not found'

        # Count source files
        src_count = sum(len(files) for _, _, files in os.walk(src_folder))

        # Find free drive letter
        try:
            import ctypes as _ct
            bitmask = _ct.windll.kernel32.GetLogicalDrives()
            free_letter = None
            for i in range(25, 3, -1):
                if not (bitmask & (1 << i)):
                    free_letter = chr(65 + i) + ':'
                    break
            if not free_letter:
                return True, 'No free drive letter — skipping verify'
        except Exception as e:
            return True, 'Could not find drive letter: ' + str(e)

        try:
            # Mount read-only
            result = subprocess.run(
                [osf, '-a', '-t', 'file', '-f', out_path,
                 '-m', free_letter, '-o', 'ro,rem'],
                capture_output=True, text=True, timeout=30)
            if result.returncode != 0:
                return False, 'Could not mount image for verification'

            # Wait for drive
            import time as _t
            for _ in range(20):
                if os.path.exists(free_letter + '\\'):
                    break
                _t.sleep(0.3)
            else:
                subprocess.run([osf, '-d', '-m', free_letter], capture_output=True)
                return False, 'Mounted drive did not appear'

            # Check eboot.bin
            eboot_found = False
            img_count   = 0
            for root, dirs, files in os.walk(free_letter + '\\'):
                img_count += len(files)
                for fn in files:
                    if fn.lower() == 'eboot.bin':
                        eboot_found = True

            # Dismount
            subprocess.run([osf, '-d', '-m', free_letter], capture_output=True)

            if not eboot_found:
                return False, 'eboot.bin not found in built image!'
            if img_count < src_count:
                return False, ('File count mismatch: source has %d files, '
                               'image has %d' % (src_count, img_count))
            return True, ('Verified \u2713  eboot.bin present, '
                          '%d/%d files confirmed' % (img_count, src_count))

        except Exception as e:
            try:
                subprocess.run([osf, '-d', '-m', free_letter], capture_output=True)
            except Exception:
                pass
            return False, 'Verification error: ' + str(e)

    def _verify_done(self, ok, msg, idx, indices, pos, out_path):
        self._log('[VERIFY] ' + msg + '\n')

        # ── Corrupt image detection ──
        if ok and os.path.isfile(out_path):
            size = os.path.getsize(out_path)
            src_size = self._get_folder_size(self._queue[idx].game_folder)
            if src_size > 10 * 1024**2 and size < src_size * 0.05:
                ok = False
                msg = ('Image is suspiciously small (%.1f MB) for a game that '
                       'should be %.1f GB. It may be corrupt.' % (
                           size/1024**2, src_size/1024**3))
                self._log('[WARN] ' + msg + '\n')

        if ok:
            self._set_status(msg, SUCCESS)
            # Auto-export log
            self._export_log(out_path)
        else:
            self._set_status('Verify failed: ' + msg, DANGER)
            messagebox.showwarning('Verification failed',
                'Build completed but verification found an issue:\n\n' + msg +
                '\n\nThe image may be incomplete.')
            self._export_log(out_path, failed=True)

        self._notify('Build complete', (self._queue[idx].game_title or '') + '\n' + out_path)
        # Auto-upload if enabled
        if self._ftp_auto_var.get() and self._ftp_ip_var.get().strip():
            if messagebox.askyesno('FTP Upload',
                    'Build complete!\n\nUpload to PS5 now?\n' + out_path):
                self._ftp_upload_file(out_path,
                    on_done=lambda: self._process_next(indices, pos + 1))
            else:
                self._process_next(indices, pos + 1)
        else:
            self._process_next(indices, pos + 1)

    def _export_log(self, out_path, failed=False):
        """Auto-save build log alongside the output image."""
        try:
            log_text = self.log_box.get('1.0', 'end')
            suffix = '_FAILED.log' if failed else '.log'
            log_path = os.path.splitext(out_path)[0] + suffix
            with open(log_path, 'w', encoding='utf-8') as f:
                f.write('exFAT Image Builder v' + APP_VERSION + '\n')
                f.write('Build log: ' + os.path.basename(out_path) + '\n')
                f.write('Date: ' + time.strftime('%Y-%m-%d %H:%M:%S') + '\n')
                f.write('=' * 60 + '\n\n')
                f.write(log_text)
        except Exception:
            pass

    # ── Installed games list + PS5 storage ────────────────────────────────────
    def _show_installed_games(self):
        ip = self._ftp_ip_var.get().strip()
        if not ip:
            messagebox.showwarning('No IP', 'Enter your PS5 IP address in Settings first.')
            return
        remote_dir = self._ftp_path_var.get().strip() or '/data/etaHEN/games/'

        win = tk.Toplevel(self)
        win.title('Installed Games on PS5 — ' + ip)
        win.geometry('640x480')
        win.configure(bg=BG)
        win.transient(self)

        hdr = tk.Frame(win, bg=BG)
        hdr.pack(fill='x', padx=16, pady=(12, 4))
        tk.Label(hdr, text='Installed Games',
                 font=('Segoe UI', 13, 'bold'), bg=BG, fg=TEXT).pack(side='left')
        self._ps5_storage_var = tk.StringVar(value='')
        tk.Label(hdr, textvariable=self._ps5_storage_var,
                 font=('Segoe UI', 9), bg=BG, fg=INFO_FG).pack(side='right')

        status_var = tk.StringVar(value='Connecting...')
        tk.Label(win, textvariable=status_var,
                 font=('Segoe UI', 8), bg=BG, fg=MUTED).pack(anchor='w', padx=16)

        list_frame = tk.Frame(win, bg=SURFACE2,
                              highlightbackground=BORDER, highlightthickness=1)
        list_frame.pack(fill='both', expand=True, padx=16, pady=(6, 8))
        listbox = tk.Listbox(list_frame, font=('Consolas', 9),
                             bg=SURFACE2, fg=TEXT,
                             selectbackground=ACCENT, selectforeground='#ffffff',
                             relief='flat', activestyle='none', bd=6)
        sb = tk.Scrollbar(list_frame, command=listbox.yview,
                          bg=SURFACE2, troughcolor=BG)
        listbox.configure(yscrollcommand=sb.set)
        sb.pack(side='right', fill='y')
        listbox.pack(fill='both', expand=True)

        btn_row = tk.Frame(win, bg=BG)
        btn_row.pack(fill='x', padx=16, pady=(0, 12))
        tk.Button(btn_row, text='Refresh', font=('Segoe UI', 9),
                  bg=SURFACE2, fg=TEXT, relief='flat', bd=0,
                  padx=10, pady=5, cursor='hand2',
                  command=lambda: _load()).pack(side='left')
        tk.Button(btn_row, text='Close', font=('Segoe UI', 9),
                  bg=SURFACE2, fg=MUTED, relief='flat', bd=0,
                  padx=10, pady=5, cursor='hand2',
                  command=win.destroy).pack(side='right')

        def _load():
            status_var.set('Loading...')
            listbox.delete(0, 'end')
            def worker():
                try:
                    import ftplib
                    ftp = self._ftp_connect()
                    # Get storage free space
                    try:
                        lines = []
                        ftp.retrlines('LIST ' + remote_dir, lines.append)
                        # Parse .exfat files
                        games = []
                        for line in lines:
                            parts = line.split()
                            if len(parts) < 9:
                                continue
                            name = ' '.join(parts[8:])
                            if not name.lower().endswith('.exfat'):
                                continue
                            try:
                                sz = int(parts[4])
                            except Exception:
                                sz = 0
                            games.append((name, sz))
                        games.sort(key=lambda x: x[0].lower())
                        ftp.quit()
                        win.after(0, _show, games)
                    except Exception as e:
                        ftp.quit()
                        win.after(0, status_var.set, 'Error: ' + str(e))
                except Exception as e:
                    win.after(0, status_var.set, 'Connection failed: ' + str(e))
            threading.Thread(target=worker, daemon=True).start()

        def _show(games):
            listbox.delete(0, 'end')
            if not games:
                listbox.insert('end', '  No .exfat files found in ' + remote_dir)
                status_var.set('0 games found')
                return
            total_size = sum(sz for _, sz in games)
            status_var.set('%d game(s)  —  %.1f GB total' % (
                len(games), total_size / 1024**3))
            for name, sz in games:
                if sz >= 1024**3:
                    sz_str = '%.2f GB' % (sz / 1024**3)
                else:
                    sz_str = '%.0f MB' % (sz / 1024**2)
                listbox.insert('end', '\U0001f4be  %-50s %s' % (name, sz_str))

        _load()



        if pct is not None:
            if kind == 'step':
                self._set_progress(pct, extra,
                    self._format_eta(elapsed, pct) if pct > 0 else '')
            elif kind == 'robo_eta':
                es = 'Elapsed: %dm %02ds' % (int(elapsed // 60), int(elapsed % 60))
                self._set_progress(pct, None,
                    es + '  \u2014  ' + extra if extra
                    else self._format_eta(elapsed, pct))
            else:
                self._set_progress(pct, None, self._format_eta(elapsed, pct))
        elif self._current_pct > 0 and self._start_time:
            self._eta_var.set(self._format_eta(elapsed, self._current_pct))

    # ── Real-time drive polling ────────────────────────────────────────────────
    def _start_drive_poll(self):
        self._poll_drive()

    def _stop_drive_poll(self):
        if self._drive_poll_id is not None:
            try:
                self.after_cancel(self._drive_poll_id)
            except Exception:
                pass
            self._drive_poll_id = None

    def _poll_drive(self):
        if not self._building or not self._mounted_drive:
            return
        try:
            usage = shutil.disk_usage(self._mounted_drive + '\\')
            free_gb    = usage.free  / (1024 ** 3)
            total_gb   = usage.total / (1024 ** 3)
            written_gb = (self._copy_start_free - usage.free) / (1024 ** 3) \
                         if self._copy_start_free is not None else 0
            remaining_gb = free_gb

            elapsed_copy  = time.time() - self._copy_start_time \
                            if hasattr(self, '_copy_start_time') else 0
            elapsed_total = time.time() - self._start_time if self._start_time else 0

            speed_mbs = (written_gb * 1024) / elapsed_copy if elapsed_copy > 0 else 0

            if speed_mbs > 0 and remaining_gb > 0:
                eta_secs = (remaining_gb * 1024) / speed_mbs
                if eta_secs < 5:
                    eta_str = 'Almost done'
                elif eta_secs < 60:
                    eta_str = 'ETA: %ds' % int(eta_secs)
                else:
                    eta_str = 'ETA: %dm %02ds' % (int(eta_secs // 60), int(eta_secs % 60))
            else:
                eta_str = ''

            elapsed_str = 'Elapsed: %dm %02ds' % (
                int(elapsed_total // 60), int(elapsed_total % 60))

            # Count files currently on the mounted image (fast — just walk)
            file_count_str = ''
            try:
                img_files = sum(len(files)
                                for _, _, files in os.walk(self._mounted_drive + '\\'))
                src_total = self._source_file_count
                if src_total > 0:
                    file_count_str = '%d / %d files' % (img_files, src_total)
                elif img_files > 0:
                    file_count_str = '%d files copied' % img_files
            except Exception:
                pass

            parts = [elapsed_str]
            if file_count_str:
                parts.append(file_count_str)
            if written_gb > 0.01:
                parts.append('%.2f GB written' % written_gb)
            if remaining_gb > 0.01:
                parts.append('%.2f GB remaining' % remaining_gb)
            if speed_mbs > 0:
                parts.append('%.1f MB/s' % speed_mbs)
            if eta_str:
                parts.append(eta_str)

            self._eta_var.set('  \u2022  '.join(parts))

            if total_gb > 0 and self._copy_start_free is not None:
                lo, hi, _ = STEPS['3/4']
                pct = lo + (hi - lo) * min(1.0, written_gb / total_gb)
                self._update_bar_visual(pct)
                self._pct_var.set(str(int(pct)) + '%')

        except Exception:
            pass

        self._drive_poll_id = self.after(1000, self._poll_drive)

    # ── FTP ───────────────────────────────────────────────────────────────────
    def _save_ftp_settings(self):
        self._settings['ftp_ip']   = self._ftp_ip_var.get().strip()
        self._settings['ftp_port'] = int(self._ftp_port_var.get().strip() or '2121')
        self._settings['ftp_path'] = self._ftp_path_var.get().strip() or '/data/etaHEN/games/'
        self._settings['ftp_auto'] = self._ftp_auto_var.get()
        save_settings(self._settings)

    def _ftp_connect(self):
        import ftplib
        ip   = self._ftp_ip_var.get().strip()
        port = int(self._ftp_port_var.get().strip() or '2121')
        if not ip:
            raise ValueError('No PS5 IP address set. Enter it in Settings.')
        ftp = ftplib.FTP()
        ftp.connect(ip, port, timeout=10)
        ftp.login()
        return ftp

    def _ftp_test(self):
        self._save_ftp_settings()
        self._ftp_status_var.set('Connecting...')
        self._ftp_status_lbl.config(fg=ACCENT)
        def worker():
            try:
                import ftplib
                ftp = self._ftp_connect()
                # List root to confirm connection
                items = ftp.nlst()
                ftp.quit()
                self.after(0, self._ftp_test_result, True,
                           'Connected \u2713  (' + str(len(items)) + ' items at root)')
            except Exception as e:
                self.after(0, self._ftp_test_result, False, str(e))
        threading.Thread(target=worker, daemon=True).start()

    def _ftp_test_result(self, ok, msg):
        self._ftp_status_var.set(msg)
        self._ftp_status_lbl.config(fg=SUCCESS if ok else DANGER)

    def _cancel_ftp_upload(self):
        if self._ftp_uploading:
            self._ftp_cancel = True
            self._ftp_status_var.set('Cancelling...')
            self._ftp_status_lbl.config(fg=WARNING)

    def _ftp_upload_file(self, local_path, on_done=None):
        remote_dir  = self._ftp_path_var.get().strip() or '/data/etaHEN/games/'
        filename    = os.path.basename(local_path)
        remote_path = remote_dir.rstrip('/') + '/' + filename
        file_size   = os.path.getsize(local_path)
        file_size_gb = file_size / (1024 ** 3)

        self._ftp_cancel    = False
        self._ftp_uploading = True
        self._cancel_btn.pack(side='left', padx=(6, 0))  # show cancel button

        self._ftp_status_var.set('Connecting...')
        self._ftp_status_lbl.config(fg=ACCENT)
        self._set_status('Uploading to PS5: ' + filename, ACCENT)
        self._set_progress(0, 'Uploading to PS5: ' + filename, '')
        self._log('\n[FTP] Uploading ' + filename + ' -> ' + remote_path + '\n')
        self._log('[FTP] File size: %.2f GB\n' % file_size_gb)

        uploaded  = [0]
        start     = [time.time()]
        window    = []
        cancelled = [False]

        def progress(block):
            if self._ftp_cancel:
                cancelled[0] = True
                raise Exception('Upload cancelled by user')
            uploaded[0] += len(block)
            now     = time.time()
            elapsed = now - start[0]
            window.append((now, uploaded[0]))
            cutoff = now - 5.0
            while len(window) > 1 and window[0][0] < cutoff:
                window.pop(0)
            if len(window) >= 2:
                dt = window[-1][0] - window[0][0]
                db = window[-1][1] - window[0][1]
                speed_mbs = (db / (1024 * 1024)) / dt if dt > 0 else 0
            else:
                speed_mbs = uploaded[0] / (1024 * 1024) / elapsed if elapsed > 0 else 0
            sent_gb      = uploaded[0] / (1024 ** 3)
            remaining_gb = max(0, file_size_gb - sent_gb)
            pct          = min(100, int(uploaded[0] / file_size * 100)) if file_size else 0
            if speed_mbs > 0 and remaining_gb > 0:
                eta_secs = (remaining_gb * 1024) / speed_mbs
                if eta_secs < 5:
                    eta_str = 'Almost done'
                elif eta_secs < 60:
                    eta_str = 'ETA: %ds' % int(eta_secs)
                else:
                    eta_str = 'ETA: %dm %02ds' % (int(eta_secs // 60), int(eta_secs % 60))
            else:
                eta_str = ''
            elapsed_str = 'Elapsed: %dm %02ds' % (int(elapsed // 60), int(elapsed % 60))
            self.after(0, self._ftp_upload_progress,
                       pct, speed_mbs, sent_gb, remaining_gb,
                       elapsed_str, eta_str, filename)

        def worker():
            try:
                import ftplib
                ftp = self._ftp_connect()
                self.after(0, self._ftp_status_var.set, 'Uploading...')
                parts = remote_dir.strip('/').split('/')
                path = ''
                for part in parts:
                    path += '/' + part
                    try:
                        ftp.mkd(path)
                    except Exception:
                        pass
                with open(local_path, 'rb') as f:
                    ftp.storbinary('STOR ' + remote_path, f,
                                   blocksize=65536, callback=progress)
                ftp.quit()
                self.after(0, self._ftp_upload_done, True, remote_path, on_done)
            except Exception as e:
                if cancelled[0]:
                    self.after(0, self._ftp_upload_done, False, 'cancelled', on_done)
                else:
                    self.after(0, self._ftp_upload_done, False, str(e), on_done)

        threading.Thread(target=worker, daemon=True).start()

    def _ftp_upload_progress(self, pct, speed_mbs, sent_gb, remaining_gb,
                              elapsed_str, eta_str, filename):
        # Update progress bar
        self._set_progress(pct, 'Uploading to PS5: ' + filename, '')

        # Build detailed ETA string
        info_parts = [elapsed_str]
        if sent_gb > 0.001:
            info_parts.append('%.2f GB sent' % sent_gb)
        if remaining_gb > 0.001:
            info_parts.append('%.2f GB remaining' % remaining_gb)
        if speed_mbs > 0:
            info_parts.append('%.1f MB/s' % speed_mbs)
        if eta_str:
            info_parts.append(eta_str)

        eta_line = '  \u2022  '.join(info_parts)
        self._eta_var.set(eta_line)

        # Update FTP status bar with compact summary
        self._ftp_status_var.set(
            '%d%%  \u2022  %.1f MB/s  \u2022  %.2f GB remaining  \u2022  %s'
            % (pct, speed_mbs, remaining_gb, eta_str))

    def _ftp_upload_done(self, ok, info, on_done):
        self._ftp_uploading = False
        self._ftp_cancel    = False
        self._cancel_btn.pack_forget()

        if info == 'cancelled':
            self._ftp_status_var.set('Upload cancelled')
            self._ftp_status_lbl.config(fg=WARNING)
            self._set_status('FTP upload cancelled', WARNING)
            self._set_progress(0, '', '')
            self._eta_var.set('')
            self._log('[FTP] Upload cancelled by user\n')
        elif ok:
            self._ftp_status_var.set('Upload complete \u2713  ' + info)
            self._ftp_status_lbl.config(fg=SUCCESS)
            self._set_progress(100, 'Upload complete!', '')
            self._set_status('Upload complete: ' + info, SUCCESS)
            self._log('[FTP] Upload complete: ' + info + '\n')
            self._notify('Upload complete', info)
        else:
            self._ftp_status_var.set('Upload failed: ' + info)
            self._ftp_status_lbl.config(fg=DANGER)
            self._set_status('FTP upload failed', DANGER)
            self._log('[FTP] Error: ' + info + '\n')
            messagebox.showerror('FTP Upload Failed',
                'Failed to upload to PS5:\n\n' + info +
                '\n\nCheck your IP, port, and that the PS5 FTP server is running.')
        if on_done:
            on_done()

    # ── Extra settings ────────────────────────────────────────────────────────
    def _save_extra_settings(self):
        self._settings['notify_sound']     = self._sound_var.get()
        self._settings['discord_webhook']  = self._discord_var.get().strip()
        self._settings['retry_count']      = self._retry_var.get()
        save_settings(self._settings)

    # ── Notification sound ────────────────────────────────────────────────────
    def _notify(self, title, detail=''):
        if self._sound_var.get():
            try:
                import winsound
                winsound.MessageBeep(winsound.MB_ICONASTERISK)
            except Exception:
                pass
        self._discord_notify(title, detail)

    # ── Discord webhook ───────────────────────────────────────────────────────
    def _discord_notify(self, title, detail=''):
        url = self._discord_var.get().strip()
        if not url:
            return
        def worker():
            try:
                import urllib.request, urllib.parse
                colour = 0x4caf50  # green
                payload = json.dumps({
                    'embeds': [{
                        'title': '\U0001f3ae ' + title,
                        'description': detail,
                        'color': colour,
                        'footer': {'text': 'PS5 exFAT Image Builder by DecKerr97'},
                        'timestamp': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
                    }]
                }).encode('utf-8')
                req = urllib.request.Request(url, data=payload,
                    headers={'Content-Type': 'application/json'})
                urllib.request.urlopen(req, timeout=5)
            except Exception:
                pass
        threading.Thread(target=worker, daemon=True).start()

    def _test_discord(self):
        url = self._discord_var.get().strip()
        if not url:
            messagebox.showwarning('No webhook', 'Enter a Discord webhook URL first.')
            return
        self._discord_notify('Test notification',
                             'PS5 exFAT Image Builder is connected to Discord \u2713')
        self._set_status('Discord test sent', SUCCESS)

    # ── Auto-detect PS5 IP ────────────────────────────────────────────────────
    def _auto_detect_ip(self):
        self._autoip_status.config(text='Scanning network...', fg=ACCENT)
        def worker():
            import socket, concurrent.futures
            found = []
            # Get local subnet from default gateway
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s.connect(('8.8.8.8', 80))
                local_ip = s.getsockname()[0]
                s.close()
                subnet = '.'.join(local_ip.split('.')[:3])
            except Exception:
                self.after(0, self._autoip_status.config,
                           {'text': 'Could not determine subnet', 'fg': DANGER})
                return

            ports = [2121, 2122]

            def check(ip):
                for port in ports:
                    try:
                        s = socket.create_connection((ip, port), timeout=0.3)
                        s.close()
                        return ip, port
                    except Exception:
                        pass
                return None

            ips = [subnet + '.' + str(i) for i in range(1, 255)]
            with concurrent.futures.ThreadPoolExecutor(max_workers=50) as ex:
                results = list(ex.map(check, ips))

            found = [r for r in results if r]
            self.after(0, self._autoip_result, found)

        threading.Thread(target=worker, daemon=True).start()

    def _autoip_result(self, found):
        if not found:
            self._autoip_status.config(
                text='No PS5 found on network', fg=DANGER)
            return
        ip, port = found[0]
        self._ftp_ip_var.set(ip)
        self._ftp_port_var.set(str(port))
        self._save_ftp_settings()
        self._autoip_status.config(
            text='Found PS5 at ' + ip + ':' + str(port) + '  \u2713', fg=SUCCESS)

    # ── Update checker ────────────────────────────────────────────────────────
    def _check_for_updates(self):
        def worker():
            try:
                import urllib.request
                url = 'https://api.github.com/repos/kerrdec97/ps5-exfat-builder/releases/latest'
                req = urllib.request.Request(url,
                    headers={'User-Agent': 'ps5-exfat-builder'})
                with urllib.request.urlopen(req, timeout=5) as r:
                    data = json.loads(r.read().decode())
                latest = data.get('tag_name', '').lstrip('v')
                current = APP_VERSION
                if latest and latest != current:
                    self.after(0, self._show_update_notice, latest,
                               data.get('html_url', ''))
            except Exception:
                pass
        threading.Thread(target=worker, daemon=True).start()

    def _fetch_changelog(self):
        if not hasattr(self, '_changelog_var'):
            return
        def worker():
            try:
                import urllib.request
                url = 'https://api.github.com/repos/kerrdec97/ps5-exfat-builder/releases'
                req = urllib.request.Request(url,
                    headers={'User-Agent': 'ps5-exfat-builder'})
                with urllib.request.urlopen(req, timeout=5) as r:
                    releases = json.loads(r.read().decode())
                lines = []
                for rel in releases[:5]:
                    tag  = rel.get('tag_name', '')
                    name = rel.get('name', tag)
                    body = rel.get('body', '').strip()
                    if len(body) > 400:
                        body = body[:397] + '...'
                    lines.append('\u25b6 ' + tag + '  ' + name)
                    if body:
                        lines.append(body)
                    lines.append('')
                text = '\n'.join(lines).strip() or 'No releases found yet.'
                self.after(0, self._changelog_var.set, text)
            except Exception:
                self.after(0, self._changelog_var.set,
                           'Could not load — check your internet connection.')
        threading.Thread(target=worker, daemon=True).start()

    def _show_update_notice(self, version, url):
        if messagebox.askyesno('Update available',
                'Version ' + version + ' is available on GitHub.\n\n'
                'Open the releases page now?'):
            import webbrowser
            webbrowser.open(url)

    def _set_status(self, text, color=MUTED):
        self.status_text.set(text)
        self.status_lbl.config(fg=color)

    def _parse_ver_from_name(self, filename):
        """Extract version like 01.007.000 from a filename like 'Game (01.007.000).exfat'"""
        m = re.search(r'\((\d{2}\.\d{3}\.\d{3})\)', filename or '')
        return m.group(1) if m else None

    # ── Dark / Light mode ─────────────────────────────────────────────────────
    def _toggle_theme(self):
        global BG, SURFACE2, FIELD_BG, FIELD_FG, BORDER, TEXT, MUTED
        global SETTINGS_BG, QUEUE_ODD, QUEUE_EVEN, TRACK
        if self._theme_var.get() == 'light':
            BG         = '#f5f5f5'
            SURFACE2   = '#e8e8e8'
            FIELD_BG   = '#ffffff'
            FIELD_FG   = '#111111'
            BORDER     = '#cccccc'
            TEXT       = '#111111'
            MUTED      = '#666666'
            SETTINGS_BG= '#eeeeee'
            QUEUE_ODD  = '#f0f0f0'
            QUEUE_EVEN = '#e8e8e8'
            TRACK      = '#dddddd'
        else:
            BG         = '#000000'
            SURFACE2   = '#1c1c1c'
            FIELD_BG   = '#f0f0f0'
            FIELD_FG   = '#111111'
            BORDER     = '#444444'
            TEXT       = '#ffffff'
            MUTED      = '#aaaaaa'
            SETTINGS_BG= '#0f0f0f'
            QUEUE_ODD  = '#141414'
            QUEUE_EVEN = '#1a1a1a'
            TRACK      = '#1e1e1e'
        self._settings['theme'] = self._theme_var.get()
        save_settings(self._settings)
        messagebox.showinfo('Theme changed',
            'Theme will apply fully on next launch.\n'
            'Some elements update immediately.')
        self.configure(bg=BG)

    def _toggle_log(self, force_open=False):
        if force_open and self._log_visible.get():
            return
        if force_open:
            self._log_visible.set(True)
        else:
            self._log_visible.set(not self._log_visible.get())
        if self._log_visible.get():
            self._log_body.pack(fill='x', pady=(2, 0))
            self._log_toggle_lbl.config(text='\u25bc  OUTPUT LOG', fg=TEXT)
        else:
            self._log_body.pack_forget()
            self._log_toggle_lbl.config(text='\u25b6  OUTPUT LOG', fg=MUTED)

    def _toggle_settings(self):
        self._settings_visible.set(not self._settings_visible.get())
        if self._settings_visible.get():
            self._settings_body.pack(fill='x', pady=(4, 0))
            self._settings_toggle_lbl.config(text='\u25bc  SETTINGS', fg=TEXT)
        else:
            self._settings_body.pack_forget()
            self._settings_toggle_lbl.config(text='\u25b6  SETTINGS', fg=MUTED)

    def _log(self, text):
        # Auto-expand log on first output
        self._toggle_log(force_open=True)
        self.log_box.config(state='normal')
        self.log_box.insert('end', text)
        self.log_box.see('end')
        self.log_box.config(state='disabled')

    def _log_clear(self):
        self.log_box.config(state='normal')
        self.log_box.delete('1.0', 'end')
        self.log_box.config(state='disabled')

if __name__ == '__main__':
    app = ExFATBuilder()
    app.mainloop()
