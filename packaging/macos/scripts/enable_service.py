#!/usr/bin/env python3
"""Best-effort: switch our Quick Action on in Finder's right-click menu.

macOS keeps a per-service visibility record in the user's `pbs` preference
domain. Installing a .workflow by copying the bundle does not write that record,
so a correctly registered Service can still be invisible. This helper adds the
flags and flushes the Services cache. Failure is non-fatal -- the user can
always enable it in System Settings > Keyboard > Keyboard Shortcuts > Services.
It must run as the logged-in console user, not as root.
"""
from __future__ import annotations

import os
import plistlib
import subprocess
import tempfile

MENU_TITLE = "用 RapidEditor 打开"
KEY = "(null) - %s - runWorkflowAsService" % MENU_TITLE


def main() -> None:
    fd, tmp = tempfile.mkstemp(suffix=".plist", prefix="rapideditor-pbs-")
    os.close(fd)
    try:
        data = {}
        result = subprocess.run(["/usr/bin/defaults", "export", "pbs", tmp],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if result.returncode == 0 and os.path.getsize(tmp) > 0:
            with open(tmp, "rb") as f:
                data = plistlib.load(f)
        status = data.setdefault("NSServicesStatus", {})
        entry = status.setdefault(KEY, {})
        entry["presentation_modes"] = {
            "ContextMenu": True,
            "FinderPreview": True,
            "ServicesMenu": True,
            "TouchBar": True,
        }
        entry["enabled_context_menu"] = True
        entry["enabled_services_menu"] = True
        with open(tmp, "wb") as f:
            plistlib.dump(data, f, fmt=plistlib.FMT_XML)
        subprocess.run(["/usr/bin/defaults", "import", "pbs", tmp],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception as exc:                       # noqa: BLE001 - best effort
        print("RapidEditor: could not pre-enable Quick Action: %s" % exc)
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass
    subprocess.run(["/System/Library/CoreServices/pbs", "-flush"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
