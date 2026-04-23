import tkinter as tk
from tkinter import filedialog, messagebox, ttk
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
        # FTP upload cancel
        self._ftp_cancel      = False
        self._ftp_uploading   = False

        self._build_ui()
        self._render_queue()
        self._refresh_temp_size()

    def _on_close(self):
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
        tk.Label(title_row, text='by DecKerr97',
                 font=('Segoe UI', 9), bg=BG, fg='#555555').pack(side='right', anchor='s', pady=(6, 0))
        tk.Label(hdr, text='Build PS5 exFAT game images  \u2014  scripts bundled, game name auto-detected',
                 font=('Segoe UI', 10), bg=BG, fg=MUTED).pack(anchor='w')
        tk.Frame(self, bg=BORDER, height=1).pack(fill='x')

        # ── Settings strip ──
        self._settings_frame = tk.Frame(self, bg=SETTINGS_BG,
                                         highlightbackground=BORDER,
                                         highlightthickness=1)
        self._settings_frame.pack(fill='x', padx=24, pady=(8, 2))
        self._build_settings_strip()

        tk.Frame(self, bg=BORDER, height=1).pack(fill='x', pady=(6, 0))

        # ── Add to queue form ──
        form = tk.Frame(self, bg=BG)
        form.pack(fill='x', padx=24, pady=6)
        tk.Label(form, text='ADD TO QUEUE', font=('Segoe UI', 8),
                 bg=BG, fg=MUTED).pack(anchor='w', pady=(0, 4))

        self._field(form, 'Game root folder  (must contain eboot.bin)',
                    self.game_folder, self._browse_game)

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

        tk.Frame(self, bg=BORDER, height=1).pack(fill='x')

        # ── Queue ──
        q_header = tk.Frame(self, bg=BG)
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

        q_outer = tk.Frame(self, bg=SURFACE2,
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

        # ── Bottom bar — packed FIRST = always visible at very bottom ──
        bottom = tk.Frame(self, bg=BG)
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

        tk.Frame(self, bg=BORDER, height=1).pack(side='bottom', fill='x')

        # ── Progress — packed second from bottom ──
        prog_outer = tk.Frame(self, bg=BG)
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

        tk.Frame(self, bg=BORDER, height=1).pack(side='bottom', fill='x')

        # ── Collapsible log panel ──
        self._log_visible = tk.BooleanVar(value=False)
        log_outer = tk.Frame(self, bg=BG)
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
        self._cancel_btn = tk.Button(
                  ftp_status_row, text='\u2715 Cancel Upload',
                  font=('Segoe UI', 9), bg=SURFACE2, fg=DANGER,
                  activebackground=BORDER, activeforeground=DANGER,
                  relief='flat', bd=0, padx=10, pady=4,
                  cursor='hand2',
                  command=self._cancel_ftp_upload)
        # Hidden until upload starts

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
        win.title('PS5 File Browser — ' + ip)
        win.geometry('600x500')
        win.configure(bg=BG)
        win.transient(self)

        path_var = tk.StringVar(value=self._ftp_path_var.get().strip() or '/data/etaHEN/games/')
        status_var = tk.StringVar(value='Connecting...')

        # Header
        hdr = tk.Frame(win, bg=BG)
        hdr.pack(fill='x', padx=16, pady=(12, 6))
        tk.Label(hdr, text='PS5 Files', font=('Segoe UI', 13, 'bold'),
                 bg=BG, fg=TEXT).pack(side='left')
        tk.Label(hdr, textvariable=status_var, font=('Segoe UI', 9),
                 bg=BG, fg=MUTED).pack(side='right')

        # Path bar
        path_row = tk.Frame(win, bg=BG)
        path_row.pack(fill='x', padx=16, pady=(0, 6))
        pf = tk.Frame(path_row, bg=FIELD_BG,
                      highlightbackground=BORDER, highlightthickness=1)
        pf.pack(side='left', fill='x', expand=True)
        path_entry = tk.Entry(pf, textvariable=path_var,
                              font=('Consolas', 9), bg=FIELD_BG, fg=FIELD_FG,
                              insertbackground=FIELD_FG, relief='flat', bd=4)
        path_entry.pack(fill='x')
        tk.Button(path_row, text='Go', font=('Segoe UI', 9),
                  bg=SURFACE2, fg=TEXT, relief='flat', bd=0,
                  padx=10, pady=4, cursor='hand2',
                  command=lambda: _load(path_var.get())).pack(side='left', padx=(6, 0))

        # File list
        list_frame = tk.Frame(win, bg=SURFACE2,
                              highlightbackground=BORDER, highlightthickness=1)
        list_frame.pack(fill='both', expand=True, padx=16, pady=(0, 8))
        listbox = tk.Listbox(list_frame, font=('Consolas', 9),
                             bg=SURFACE2, fg=TEXT, selectbackground=ACCENT,
                             selectforeground='#ffffff', relief='flat',
                             activestyle='none', bd=6)
        scrollbar = tk.Scrollbar(list_frame, command=listbox.yview,
                                 bg=SURFACE2, troughcolor=BG)
        listbox.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side='right', fill='y')
        listbox.pack(fill='both', expand=True)

        # Bottom buttons
        btn_row = tk.Frame(win, bg=BG)
        btn_row.pack(fill='x', padx=16, pady=(0, 12))
        tk.Button(btn_row, text='Delete selected', font=('Segoe UI', 9),
                  bg=SURFACE2, fg=DANGER, relief='flat', bd=0,
                  padx=10, pady=5, cursor='hand2',
                  command=lambda: _delete_selected()).pack(side='left')
        tk.Button(btn_row, text='Refresh', font=('Segoe UI', 9),
                  bg=SURFACE2, fg=TEXT, relief='flat', bd=0,
                  padx=10, pady=5, cursor='hand2',
                  command=lambda: _load(path_var.get())).pack(side='left', padx=(6, 0))
        tk.Button(btn_row, text='Close', font=('Segoe UI', 9),
                  bg=SURFACE2, fg=MUTED, relief='flat', bd=0,
                  padx=10, pady=5, cursor='hand2',
                  command=win.destroy).pack(side='right')

        _ftp_ref = [None]

        def _load(p):
            status_var.set('Loading...')
            listbox.delete(0, 'end')
            def worker():
                try:
                    import ftplib
                    if _ftp_ref[0]:
                        try: _ftp_ref[0].quit()
                        except: pass
                    ftp = self._ftp_connect()
                    _ftp_ref[0] = ftp
                    items = []
                    ftp.retrlines('LIST ' + p, items.append)
                    win.after(0, _show_items, items, p)
                except Exception as e:
                    win.after(0, status_var.set, 'Error: ' + str(e))
            threading.Thread(target=worker, daemon=True).start()

        def _show_items(items, p):
            listbox.delete(0, 'end')
            path_var.set(p)
            status_var.set(str(len(items)) + ' items')
            if p != '/':
                listbox.insert('end', '\U0001f4c2  ..')
            for line in items:
                parts = line.split()
                if not parts: continue
                name = ' '.join(parts[8:]) if len(parts) >= 9 else parts[-1]
                is_dir = line.startswith('d')
                icon = '\U0001f4c1  ' if is_dir else '\U0001f4be  '
                size = ''
                if not is_dir and len(parts) >= 5:
                    try:
                        sz = int(parts[4])
                        size = '  (%.1f MB)' % (sz/1024/1024) if sz > 1024*1024 else '  (%d KB)' % (sz//1024)
                    except: pass
                listbox.insert('end', icon + name + size)

        def _delete_selected():
            sel = listbox.curselection()
            if not sel: return
            name = listbox.get(sel[0]).strip()
            # Strip icon prefix
            for pfx in ['\U0001f4c1  ', '\U0001f4be  ', '\U0001f4c2  ']:
                if name.startswith(pfx):
                    name = name[len(pfx):]
                    break
            name = name.split('  (')[0]  # strip size
            if not name or name == '..': return
            remote = path_var.get().rstrip('/') + '/' + name
            if not messagebox.askyesno('Delete', 'Delete from PS5?\n\n' + remote,
                                        parent=win): return
            def worker():
                try:
                    ftp = _ftp_ref[0] or self._ftp_connect()
                    try: ftp.delete(remote)
                    except: ftp.rmd(remote)
                    win.after(0, _load, path_var.get())
                except Exception as e:
                    win.after(0, messagebox.showerror, 'Error', str(e), parent=win)
            threading.Thread(target=worker, daemon=True).start()

        listbox.bind('<Double-Button-1>', lambda e: _nav())
        def _nav():
            sel = listbox.curselection()
            if not sel: return
            name = listbox.get(sel[0]).strip()
            if name.startswith('\U0001f4c1  ') or name.startswith('\U0001f4c2  '):
                n = name[3:].split('  (')[0]
                cur = path_var.get().rstrip('/')
                new_path = '/'.join(cur.split('/')[:-1]) if n == '..' else cur + '/' + n
                _load(new_path or '/')

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
        else:
            self._update_queue_dot(idx, 'failed')
            self._set_progress(self._current_pct, 'Failed', '')
            self._set_status('Build failed (exit code ' + str(returncode) + ')', DANGER)
            self._save_history(self._queue[idx], out_path, False)
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
        # PowerShell logs: "[1/4] ... as a logical volume on E: ..."
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

            elapsed_copy = time.time() - self._copy_start_time \
                           if hasattr(self, '_copy_start_time') else 0
            elapsed_total = time.time() - self._start_time if self._start_time else 0

            # Speed in MB/s
            speed_mbs = (written_gb * 1024) / elapsed_copy if elapsed_copy > 0 else 0

            # ETA from speed + remaining
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

            parts = [elapsed_str]
            if written_gb > 0.01:
                parts.append('%.2f GB written' % written_gb)
            if remaining_gb > 0.01:
                parts.append('%.2f GB remaining' % remaining_gb)
            if speed_mbs > 0:
                parts.append('%.1f MB/s' % speed_mbs)
            if eta_str:
                parts.append(eta_str)

            self._eta_var.set('  \u2022  '.join(parts))

            # Also update progress bar from written vs total
            if total_gb > 0 and self._copy_start_free is not None:
                lo, hi, _ = STEPS['3/4']
                pct = lo + (hi - lo) * min(1.0, written_gb / total_gb)
                self._update_bar_visual(pct)
                self._pct_var.set(str(int(pct)) + '%')

        except Exception:
            pass

        # Poll every 1 second
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
        self._cancel_btn.pack_forget()  # hide cancel button

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

    def _set_status(self, text, color=MUTED):
        self.status_text.set(text)
        self.status_lbl.config(fg=color)

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
