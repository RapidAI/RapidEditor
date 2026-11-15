#!/usr/bin/env python3
"""Create an Automator Quick Action (.workflow bundle) for Finder.

The action appears in Finder's right-click menu (Quick Actions / Services):

    Open with RapidEditor.workflow/
      Contents/Info.plist        NSServices registration
      Contents/document.wflow    one "Run Shell Script" action

The script calls `open -a RapidEditor <files>` so LaunchServices delivers an
openFiles Apple Event; RapidEditor's Cocoa delegate queues those paths and the
editor opens them on its next frame. This works when the app is already running
too, which is not true of `open --args`.

    python3 make_quick_action.py <output .workflow path> [version]

plistlib is used instead of hand-written XML so the menu title is escaped
correctly.
"""
from __future__ import annotations

import plistlib
import sys
from pathlib import Path

MENU_TITLE = "用 RapidEditor 打开"
BUNDLE_ID = "com.rapidaire.rapideditor.quickaction"
COMMAND = 'open -a "RapidEditor" "$@"'


def info_plist(version: str) -> dict:
    return {
        "CFBundleIdentifier": BUNDLE_ID,
        "CFBundleName": MENU_TITLE,
        "CFBundleShortVersionString": version,
        "CFBundleVersion": version,
        "NSServices": [{
            "NSMenuItem": {"default": MENU_TITLE},
            "NSMessage": "runWorkflowAsService",
            "NSRequiredContext": {"NSApplicationIdentifier": "com.apple.finder"},
            "NSSendFileTypes": ["public.item"],
        }],
    }


def document_wflow() -> dict:
    param_props = {
        "COMMAND_STRING": {},
        "CheckedForUserDefaultShell": {},
        "inputMethod": {},
        "shell": {},
        "source": {},
    }
    arguments = {
        "0": {"default value": 0, "name": "inputMethod", "required": "0", "type": "0", "uuid": "0"},
        "1": {"default value": "", "name": "source", "required": "0", "type": "0", "uuid": "1"},
        "2": {"default value": False, "name": "CheckedForUserDefaultShell", "required": "0", "type": "0", "uuid": "2"},
        "3": {"default value": COMMAND, "name": "COMMAND_STRING", "required": "0", "type": "0", "uuid": "3"},
        "4": {"default value": "/bin/bash", "name": "shell", "required": "0", "type": "0", "uuid": "4"},
    }
    action = {
        "AMAccepts": {
            "Container": "List",
            "Optional": True,
            "Types": ["com.apple.cocoa.string"],
        },
        "AMActionVersion": "2.0.3",
        "AMApplication": ["Automator"],
        "AMParameterProperties": param_props,
        "AMProvides": {
            "Container": "List",
            "Types": ["com.apple.cocoa.string"],
        },
        "ActionBundlePath": "/System/Library/Automator/Run Shell Script.action",
        "ActionName": "Run Shell Script",
        "ActionParameters": {
            "COMMAND_STRING": COMMAND,
            "CheckedForUserDefaultShell": True,
            "inputMethod": 1,              # 1 = pass input as arguments ($@)
            "shell": "/bin/bash",
            "source": "",
        },
        "BundleIdentifier": "com.apple.RunShellScript",
        "CFBundleVersion": "2.0.3",
        "CanShowSelectedItemsWhenRun": False,
        "CanShowWhenRun": True,
        "Category": ["AMCategoryUtilities"],
        "Class Name": "RunShellScriptAction",
        "InputUUID": "B0A3E1F0-1111-4A2B-9C3D-000000000001",
        "Keywords": ["Shell", "Script", "Command", "Run"],
        "OutputUUID": "B0A3E1F0-1111-4A2B-9C3D-000000000002",
        "UUID": "B0A3E1F0-1111-4A2B-9C3D-000000000003",
        "UnlocalizedApplications": ["Automator"],
        "arguments": arguments,
        "isViewVisible": 1,
        "location": "309.000000:253.000000",
        "nibPath": "/System/Library/Automator/Run Shell Script.action/Contents/Resources/Base.lproj/main.nib",
    }
    return {
        "AMApplicationBuild": "523",
        "AMApplicationVersion": "2.10",
        "AMDocumentVersion": "2",
        "actions": [{"action": action, "isViewVisible": 1}],
        "connectors": {},
        "workflowMetaData": {
            "applicationBundleIDsByPath": {},
            "applicationPaths": [],
            "inputTypeIdentifier": "com.apple.Automator.fileSystemObject",
            "outputTypeIdentifier": "com.apple.Automator.nothing",
            "presentationMode": 15,        # Quick Action in the Finder context menu
            "processesInput": 0,
            "serviceApplicationBundleID": "",
            "serviceApplicationPath": "",
            "serviceInputTypeIdentifier": "com.apple.Automator.fileSystemObject",
            "serviceOutputTypeIdentifier": "com.apple.Automator.nothing",
            "serviceProcessesInput": 0,
            "systemImageName": "NSActionTemplate",
            "useAutomaticInputType": 0,
            "workflowTypeIdentifier": "com.apple.Automator.servicesMenu",
        },
    }


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    dest = Path(sys.argv[1]).resolve()
    version = sys.argv[2] if len(sys.argv) > 2 else "1.0.0"
    contents = dest / "Contents"
    contents.mkdir(parents=True, exist_ok=True)
    with (contents / "Info.plist").open("wb") as f:
        plistlib.dump(info_plist(version), f, fmt=plistlib.FMT_XML, sort_keys=True)
    with (contents / "document.wflow").open("wb") as f:
        plistlib.dump(document_wflow(), f, fmt=plistlib.FMT_XML, sort_keys=True)
    print(f"Quick Action written to {dest}")


if __name__ == "__main__":
    main()
