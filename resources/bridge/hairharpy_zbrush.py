"""HairHarpy bridge for ZBrush 2026+ (its Python plugins).

Adds a "HairHarpy" sub-palette to the Zplugin palette (ZScript, if there is no Zplugin) with two buttons:
  Send SubTool  - the active SubTool, at its current subdivision level, with its polypaint, into HairHarpy's open scene
  Send Visible  - every visible SubTool, the same way
A SubTool sent again replaces its mesh in HairHarpy (the same name: updated in place).

HairHarpy must be running with "Receive from ZBrush" on (top bar: Bridge -> ZBrush). Nothing goes over the network: the
SubTool is exported as an OBJ into a folder on this computer, %LOCALAPPDATA%\\HairHarpy\\ZBrush\\inbox, and HairHarpy picks it
up from there (and deletes the file once it is in the scene).

How it is installed (HairHarpy: Bridge -> ZBrush -> Install the plugin): this file is copied into
%LOCALAPPDATA%\\HairHarpy\\ZBrush\\plugin, and that folder is added to the ZBRUSH_PLUGIN_PATH variable of your user's
environment - ZBrush runs every .py file found there when it starts. To remove it: delete that folder and the folder's
entry in ZBRUSH_PLUGIN_PATH (Windows: "Edit environment variables for your account").
"""
import os
import re
import sys

import zbrush.commands as zbc

ROOT = os.path.join(os.environ.get("LOCALAPPDATA") or os.path.expanduser("~"), "HairHarpy", "ZBrush")
INBOX = os.path.join(ROOT, "inbox")
TMP = os.path.join(ROOT, "tmp")


def _alive(pid):
    """Is the process with this id running?"""
    if sys.platform == "win32":
        import ctypes
        k = ctypes.windll.kernel32
        h = k.OpenProcess(0x1000, False, pid)              # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return False
        code = ctypes.c_ulong(0)
        ok = k.GetExitCodeProcess(h, ctypes.byref(code))
        k.CloseHandle(h)
        return bool(ok) and code.value == 259              # STILL_ACTIVE
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def _hairharpy_running():
    """HairHarpy, while it receives from ZBrush, keeps a file running_<its process id>.pid in ROOT."""
    if not os.path.isdir(ROOT):
        return False
    found = False
    for f in os.listdir(ROOT):
        m = re.fullmatch(r"running_(\d+)\.pid", f)
        if not m:
            continue
        if _alive(int(m.group(1))):
            found = True
        else:                                             # (left by a HairHarpy that did not close normally)
            try:
                os.remove(os.path.join(ROOT, f))
            except OSError:
                pass
    return found


def _safe(name):
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", name).strip(" .")
    return name or "SubTool"


def _export_active():
    """The active SubTool exported into the inbox -> its name ('' - not exported)."""
    path = zbc.get_active_tool_path()
    name = _safe(path.replace("\\", "/").rsplit("/", 1)[-1])
    os.makedirs(TMP, exist_ok=True)
    os.makedirs(INBOX, exist_ok=True)
    tmp = os.path.join(TMP, name + ".obj")
    if os.path.exists(tmp):
        os.remove(tmp)
    zbc.set_next_filename(tmp)
    zbc.press("Tool:Export")
    if not os.path.isfile(tmp) or os.path.getsize(tmp) == 0:
        return ""
    os.replace(tmp, os.path.join(INBOX, name + ".obj"))  # (whole at once: HairHarpy never sees half a file)
    return name


def _check():
    if _hairharpy_running():
        return True
    zbc.message_ok("HairHarpy is not open, or it is not receiving.\n\nOpen HairHarpy and turn on\nBridge -> ZBrush -> Receive from ZBrush.", "HairHarpy")
    return False


def send_subtool(*_):
    if not _check():
        return
    name = _export_active()
    if not name:
        zbc.message_ok("The SubTool could not be exported.", "HairHarpy")


def send_visible(*_):
    if not _check():
        return
    count = zbc.get_subtool_count()
    if count == 0:
        zbc.message_ok("No SubTools to send.", "HairHarpy")
        return
    was = zbc.get_active_subtool_index()
    sent = []
    for i in range(count):
        zbc.select_subtool(i)
        if zbc.get_subtool_status() & 0x01:              # (visible)
            name = _export_active()
            if name:
                sent.append(name)
    zbc.select_subtool(was)
    if not sent:
        zbc.message_ok("No visible SubTool could be exported.", "HairHarpy")


def create_palette():
    palette = "Zplugin" if zbc.exists("Zplugin") else "ZScript"
    menu = f"{palette}:HairHarpy"
    if zbc.exists(menu):
        return
    zbc.add_subpalette(menu, 0)
    zbc.add_button(f"{menu}:Send SubTool", "The active SubTool (its current subdivision level, with its polypaint) into HairHarpy's open scene. Sent again - its mesh there is replaced.", send_subtool, False, 0.5)
    zbc.add_button(f"{menu}:Send Visible", "Every visible SubTool into HairHarpy's open scene.", send_visible, False, 0.5)


# (made whichever way ZBrush runs this file — as its main script or imported as a plugin)
try:
    create_palette()
except Exception as e:                                    # (in ZBrush's Python output, if anything is off)
    print("HairHarpy plugin:", e)
