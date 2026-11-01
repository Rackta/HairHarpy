# HairHarpy — Blender bridge add-on.
# Install: Edit > Preferences > Add-ons > Install from Disk... > this file, then tick it. While Blender runs it listens on
# 127.0.0.1:<port> (8790 by default, set in the add-on's preferences); ⇪ Send in HairHarpy puts the hair
# meshes into the "HairMesh" collection. An object of the same name is updated in place (its mesh is swapped, its
# materials, modifiers and transform stay). The program is Y-up, Blender Z-up: the points are turned (x, -z, y).
bl_info = {
    "name": "HairHarpy Bridge",
    "author": "HairHarpy",
    "version": (1, 0, 0),
    "blender": (2, 93, 0),
    "location": "Preferences > Add-ons (port) · 3D View > Sidebar > HairMesh",
    "description": "Receives the hair meshes sent from HairHarpy (Send)",
    "category": "Import-Export",
}

import json
import queue
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import bpy

DEFAULT_PORT = 8790
COLLECTION = "HairMesh"
_server = None
_thread = None
_jobs = queue.Queue()
_state = {"msg": "Stopped", "last": ""}


class _Job:
    def __init__(self, data):
        self.data = data
        self.done = threading.Event()
        self.result = None


def build(data):
    """Make / update the meshes (main thread only)."""
    scene = bpy.context.scene
    coll = bpy.data.collections.get(COLLECTION)
    if coll is None:
        coll = bpy.data.collections.new(COLLECTION)
    stack, linked = [scene.collection], False
    while stack and not linked:
        c = stack.pop()
        linked = c == coll
        stack.extend(c.children)
    if not linked:
        scene.collection.children.link(coll)
    replace = data.get("replace", True)
    up_y = data.get("up", "y") == "y"
    names = []
    for o in data.get("objects", []):
        p = o.get("pos") or []
        if up_y:
            verts = [(p[i], -p[i + 2], p[i + 1]) for i in range(0, len(p) - 2, 3)]
        else:
            verts = [(p[i], p[i + 1], p[i + 2]) for i in range(0, len(p) - 2, 3)]
        faces = [tuple(f) for f in o.get("faces") or [] if len(f) >= 3]
        name = (o.get("name") or "Hair")[:63]
        me = bpy.data.meshes.new(name)
        me.from_pydata(verts, [], faces)
        me.validate()
        me.polygons.foreach_set("use_smooth", [True] * len(me.polygons))
        me.update()
        ob = bpy.data.objects.get(name) if replace else None
        if ob is not None and ob.type == "MESH":
            old = ob.data
            for m in old.materials:
                me.materials.append(m)
            ob.data = me
            if old.users == 0:
                bpy.data.meshes.remove(old)
            me.name = name
        else:
            ob = bpy.data.objects.new(name, me)
            coll.objects.link(ob)
        names.append(ob.name)
    _state["last"] = "Received %d mesh%s" % (len(names), "" if len(names) == 1 else "es")
    for win in getattr(bpy.context.window_manager, "windows", []):
        for area in win.screen.areas:
            area.tag_redraw()
    return {"ok": True, "program": "Blender", "objects": names}


def _pump():
    """Runs the jobs the server got, in Blender's main thread (a timer)."""
    while True:
        try:
            job = _jobs.get_nowait()
        except queue.Empty:
            break
        try:
            job.result = build(job.data)
        except Exception as e:  # noqa: BLE001 — the message goes back to the sender
            job.result = {"error": "Blender: %s" % e}
        job.done.set()
    return 0.2


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
        self._send(200, {"ok": True, "program": "Blender", "version": bpy.app.version_string})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        try:
            data = json.loads(self.rfile.read(n).decode("utf-8") or "{}")
        except Exception as e:  # noqa: BLE001
            return self._send(400, {"error": "bad request: %s" % e})
        cmd = self.path.split("?")[0].strip("/")
        if cmd == "ping":
            return self._send(200, {"ok": True, "program": "Blender", "version": bpy.app.version_string})
        if cmd != "send":
            return self._send(404, {"error": "unknown command"})
        job = _Job(data)
        _jobs.put(job)
        if not job.done.wait(60):
            return self._send(504, {"error": "Blender is busy (a modal operation?) — try again"})
        self._send(500 if "error" in job.result else 200, job.result)

    def log_message(self, *args):
        pass


def _port():
    try:
        return bpy.context.preferences.addons[__name__].preferences.port
    except Exception:  # noqa: BLE001 — not registered as an add-on (run as a script)
        return DEFAULT_PORT


def start(port=None):
    global _server, _thread
    stop()
    port = port or _port()
    try:
        _server = ThreadingHTTPServer(("127.0.0.1", port), _Handler)
    except OSError as e:
        _state["msg"] = "Port %d is busy: %s" % (port, e)
        return False
    _server.daemon_threads = True
    _thread = threading.Thread(target=_server.serve_forever, daemon=True)
    _thread.start()
    if not bpy.app.timers.is_registered(_pump):
        bpy.app.timers.register(_pump, first_interval=0.2, persistent=True)
    _state["msg"] = "Listening on port %d" % port
    return True


def stop():
    global _server, _thread
    if _server is not None:
        _server.shutdown()
        _server.server_close()
    _server = None
    _thread = None
    _state["msg"] = "Stopped"


def _port_changed(self, context):
    if _server is not None:
        start()


class HMC_Prefs(bpy.types.AddonPreferences):
    bl_idname = __name__
    port: bpy.props.IntProperty(name="Port", default=DEFAULT_PORT, min=1024, max=65535, update=_port_changed,
                                description="The same port as in HairHarpy > Bridge")

    def draw(self, context):
        col = self.layout.column()
        col.prop(self, "port")
        col.label(text=_state["msg"])


class HMC_OT_toggle(bpy.types.Operator):
    bl_idname = "hmc.bridge_toggle"
    bl_label = "Start / stop the bridge"

    def execute(self, context):
        if _server is None:
            start()
        else:
            stop()
        return {"FINISHED"}


class HMC_PT_panel(bpy.types.Panel):
    bl_label = "HairMesh Bridge"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "HairMesh"

    def draw(self, context):
        col = self.layout.column()
        col.label(text=_state["msg"])
        if _state["last"]:
            col.label(text=_state["last"])
        col.operator("hmc.bridge_toggle", text="Stop" if _server is not None else "Start")


_classes = (HMC_Prefs, HMC_OT_toggle, HMC_PT_panel)


def _start_later():
    start()
    return None


def register():
    for c in _classes:
        bpy.utils.register_class(c)
    bpy.app.timers.register(_start_later, first_interval=0.5)


def unregister():
    stop()
    if bpy.app.timers.is_registered(_pump):
        bpy.app.timers.unregister(_pump)
    for c in reversed(_classes):
        bpy.utils.unregister_class(c)


if __name__ == "__main__":
    register()
