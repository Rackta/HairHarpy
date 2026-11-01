# -*- coding: utf-8 -*-
# HairHarpy — Maya bridge (Maya 2018+, Python 2 or 3).
# Install: drag this file into Maya's viewport. It copies itself into your scripts folder, adds a line to userSetup.py
# (so it starts with Maya from then on) and starts. While Maya runs it listens on 127.0.0.1:PORT; ⇪ Send in HairHarpy
# puts the hair meshes under the "HairMesh" group. An object of the same name is replaced in place
# (the same parent and shading group). By hand: import hmc_bridge_maya; hmc_bridge_maya.start() / .stop()
from __future__ import print_function

import json
import os
import re
import shutil
import threading

try:
    from http.server import BaseHTTPRequestHandler, HTTPServer
    from socketserver import ThreadingMixIn
except ImportError:  # Python 2 (Maya 2018 – 2021)
    from BaseHTTPServer import BaseHTTPRequestHandler, HTTPServer
    from SocketServer import ThreadingMixIn

import maya.api.OpenMaya as om
import maya.cmds as cmds
import maya.utils

PORT = 8791
GROUP = "HairMesh"
_server = None


def _name(s):
    s = re.sub(r"[^A-Za-z0-9_]", "_", s or "Hair")
    return s if re.match(r"[A-Za-z_]", s) else "_" + s


def build(data):
    """Make / replace the meshes (main thread only)."""
    if not cmds.objExists(GROUP):
        cmds.group(empty=True, name=GROUP)
    replace = data.get("replace", True)
    names = []
    for o in data.get("objects", []):
        p = o.get("pos") or []
        pts = om.MPointArray()
        for i in range(0, len(p) - 2, 3):
            pts.append(om.MPoint(p[i], p[i + 1], p[i + 2]))
        faces = [f for f in o.get("faces") or [] if len(f) >= 3]
        counts = [len(f) for f in faces]
        conn = [i for f in faces for i in f]
        tr = om.MFnMesh().create(pts, counts, conn)
        new = om.MFnDagNode(tr).fullPathName()
        name = _name(o.get("name"))
        parent, sg = GROUP, None
        old = cmds.ls(name, type="transform", long=True) if replace else []
        if old:
            old = old[0]
            shapes = cmds.listRelatives(old, shapes=True, fullPath=True) or []
            sgs = cmds.listConnections(shapes, type="shadingEngine") or [] if shapes else []
            sg = sgs[0] if sgs else None
            up = cmds.listRelatives(old, parent=True, fullPath=True)
            parent = up[0] if up else None
            cmds.delete(old)
        new = (cmds.parent(new, parent) if parent else cmds.parent(new, world=True))[0]
        new = cmds.rename(new, name)
        cmds.sets(new, edit=True, forceElement=sg or "initialShadingGroup")
        names.append(new)
    return {"ok": True, "program": "Maya", "objects": names}


def _run(data):
    """build() in Maya's main thread (the server's threads must not touch the scene)."""
    return maya.utils.executeInMainThreadWithResult(build, data)


class _Handler(BaseHTTPRequestHandler):
    def _head(self, code, n=0):
        self.send_response(code)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Allow-Private-Network", "true")
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(n))
        self.end_headers()

    def _send(self, code, obj):
        b = json.dumps(obj).encode("utf-8")
        self._head(code, len(b))
        self.wfile.write(b)

    def do_OPTIONS(self):
        self._head(204)

    def do_GET(self):
        self._send(200, {"ok": True, "program": "Maya", "version": cmds.about(version=True)})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        try:
            data = json.loads(self.rfile.read(n).decode("utf-8") or "{}")
        except Exception as e:
            return self._send(400, {"error": "bad request: %s" % e})
        cmd = self.path.split("?")[0].strip("/")
        if cmd == "ping":
            return self._send(200, {"ok": True, "program": "Maya", "version": maya.utils.executeInMainThreadWithResult(cmds.about, version=True)})
        if cmd != "send":
            return self._send(404, {"error": "unknown command"})
        try:
            self._send(200, _run(data))
        except Exception as e:
            self._send(500, {"error": "Maya: %s" % e})

    def log_message(self, *args):
        pass


class _Server(ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = False


def start(port=None):
    global _server
    stop()
    port = port or PORT
    try:
        _server = _Server(("127.0.0.1", port), _Handler)
    except Exception as e:
        print("HairMesh bridge: port %d is busy (%s)" % (port, e))
        return False
    t = threading.Thread(target=_server.serve_forever)
    t.daemon = True
    t.start()
    print("HairMesh bridge: listening on port %d" % port)
    return True


def stop():
    global _server
    if _server is not None:
        _server.shutdown()
        _server.server_close()
        _server = None


# ---- drag & drop install ----
_SETUP_LINE = "import maya.utils; maya.utils.executeDeferred('import hmc_bridge_maya; hmc_bridge_maya.start()')  # HairHarpy bridge\n"


def install(src):
    d = cmds.internalVar(userScriptDir=True)
    if not os.path.isdir(d):
        os.makedirs(d)
    dst = os.path.join(d, "hmc_bridge_maya.py")
    if os.path.abspath(src) != os.path.abspath(dst):
        shutil.copyfile(src, dst)
    us = os.path.join(d, "userSetup.py")
    text = open(us).read() if os.path.exists(us) else ""
    if "hmc_bridge_maya" not in text:
        with open(us, "a") as f:
            f.write(("\n" if text and not text.endswith("\n") else "") + _SETUP_LINE)
    return dst


def onMayaDroppedPythonFile(*args):
    import inspect
    import sys
    src = globals().get("__file__") or inspect.getsourcefile(onMayaDroppedPythonFile)
    dst = install(src)
    d = os.path.dirname(dst)
    if d not in sys.path:
        sys.path.append(d)
    for m in [k for k in sys.modules if k == "hmc_bridge_maya"]:
        try:
            sys.modules[m].stop()
        except Exception:
            pass
        del sys.modules[m]
    import hmc_bridge_maya
    ok = hmc_bridge_maya.start()
    cmds.confirmDialog(title="HairMesh bridge", button=["OK"],
                       message=("Installed and listening on port %d.\nIt starts with Maya from now on." % hmc_bridge_maya.PORT) if ok
                       else "Installed, but port %d is busy — set PORT in\n%s" % (hmc_bridge_maya.PORT, dst))
