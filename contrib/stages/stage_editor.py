"""3D editor of a dungeon stages file (stages/<dungeon>.xml, see stagelib.py).

    python stage_editor.py ..\\..\\stages\\scholomance.xml

Shows the <readonly> data (navmesh floors colored per floor, ramps gray; creature spawns as cylinders, hover one for
its name) and the editable stage boxes. Select a box in the list or by clicking it in the view; edit its stage name,
center, half extents, clear position and canbeenteredincombatifnotcleared on the right. Boxes with the same name are
one stage; the first stage is the entrance, the order matters (a point belongs to the first stage containing it).

Camera: W / S move along the view direction (also up / down when looking down), A / D sideways, Q / E
straight down / up (Shift: faster), right drag rotates, middle drag pans, wheel zooms.
Boxes: left click selects one; the selected box shows its corners: left drag a corner resizes the box over the floor
(x / y, the opposite corner stays), Shift + left drag a corner changes its height (z).
Save writes the file (the <readonly> section is kept as it is); closing the window with X discards the changes.
"""

import colorsys
import math
import os
import sys
import xml.etree.ElementTree as ET

import numpy as np
import pyqtgraph.opengl as gl
from PyQt6 import QtCore, QtGui, QtWidgets

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stagelib  # noqa: E402

MOB_RADIUS = 0.8
MOB_HEIGHT = 2.2
HOVER_PIXELS = 12
HANDLE_PIXELS = 10
FLY_SPEED = 0.6         # camera distances per second


class StageBox:
    def __init__(self, name, box, clear, combat):
        self.name = name
        self.box = list(box)          # cx cy cz ex ey ez
        self.clear = clear            # [x, y, z] or None
        self.combat = combat

    @staticmethod
    def from_element(element):
        return StageBox(element.get("name", ""), stagelib.parse_box(element.get("box", "0 0 0 5 5 5")),
                        stagelib.parse_point(element.get("clearposition", "")),
                        element.get("canbeenteredincombatifnotcleared", "false") == "true")

    def to_element(self):
        return ET.Element("stage", name=self.name, box=" ".join(stagelib.fmt(v) for v in self.box),
                          clearposition=" ".join(stagelib.fmt(v) for v in self.clear) if self.clear else "",
                          canbeenteredincombatifnotcleared="true" if self.combat else "false")

    def bounds(self):
        cx, cy, cz, ex, ey, ez = self.box
        return np.array([cx - ex, cy - ey, cz - ez]), np.array([cx + ex, cy + ey, cz + ez])


def box_lines(stage_box):
    lo, hi = stage_box.bounds()
    corners = np.array([[x, y, z] for x in (lo[0], hi[0]) for y in (lo[1], hi[1]) for z in (lo[2], hi[2])])
    edges = [(0, 1), (2, 3), (4, 5), (6, 7), (0, 2), (1, 3), (4, 6), (5, 7), (0, 4), (1, 5), (2, 6), (3, 7)]
    return np.array([corners[i] for edge in edges for i in edge])


def stage_color(name, alpha=1.0):
    h = (sum(ord(c) * 31 ** i for i, c in enumerate(name)) % 360) / 360.0
    r, g, b = colorsys.hsv_to_rgb(h, 0.8, 1.0)
    return (r, g, b, alpha)


class View(gl.GLViewWidget):
    hovered = QtCore.pyqtSignal(str, QtCore.QPoint)
    clicked = QtCore.pyqtSignal(object, object)                 # ray origin, direction (numpy)
    cornerDragged = QtCore.pyqtSignal(int, object, object, bool)   # corner, ray origin, direction, vertical
    cornerReleased = QtCore.pyqtSignal()

    MOVE_KEYS = {
        QtCore.Qt.Key.Key_W: (1, 0, 0), QtCore.Qt.Key.Key_S: (-1, 0, 0),
        QtCore.Qt.Key.Key_D: (0, 1, 0), QtCore.Qt.Key.Key_A: (0, -1, 0),
        QtCore.Qt.Key.Key_E: (0, 0, 1), QtCore.Qt.Key.Key_Q: (0, 0, -1),
    }

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMouseTracking(True)
        self.setFocusPolicy(QtCore.Qt.FocusPolicy.StrongFocus)
        self.mob_points = np.zeros((0, 3))
        self.mob_names = []
        self.handles = np.zeros((0, 3))     # corners of the selected box (set by the editor)
        self.hovered_handle = -1
        self._press = None
        self._last = None
        self._drag_handle = -1
        self._keys = set()
        self._fly = QtCore.QTimer(self)
        self._fly.setInterval(16)
        self._fly.timeout.connect(self.fly)
        self._fly_clock = QtCore.QElapsedTimer()

    # -- math

    def mvp(self):
        m = self.projectionMatrix() * self.viewMatrix()
        return np.array(m.data(), dtype=float).reshape(4, 4).T

    def project(self, points):
        """Screen positions (logical pixels) of the points, and which are in front of the camera."""
        if not len(points):
            return np.zeros((0, 2)), np.zeros(0, dtype=bool)
        homogeneous = np.hstack([points, np.ones((len(points), 1))]) @ self.mvp().T
        w = homogeneous[:, 3]
        front = w > 1e-6
        ndc = homogeneous[:, :3] / np.where(front, w, 1.0)[:, None]
        sx = (ndc[:, 0] + 1) / 2 * self.width()
        sy = (1 - ndc[:, 1]) / 2 * self.height()
        return np.stack([sx, sy], axis=1), front

    def ray(self, pos):
        inverse = np.linalg.inv(self.mvp())
        x = pos.x() / self.width() * 2 - 1
        y = 1 - pos.y() / self.height() * 2
        near = inverse @ np.array([x, y, -1.0, 1.0])
        far = inverse @ np.array([x, y, 1.0, 1.0])
        near, far = near[:3] / near[3], far[:3] / far[3]
        direction = far - near
        return near, direction / np.linalg.norm(direction)

    def nearest(self, points, pos, pixels):
        """Index of the projected point within pixels of pos, -1 when none."""
        if not len(points):
            return -1
        screen, front = self.project(points)
        distances = np.where(front, np.linalg.norm(screen - np.array([pos.x(), pos.y()]), axis=1), np.inf)
        best = int(np.argmin(distances))
        return best if distances[best] <= pixels else -1

    # -- camera: W A S D Q E fly, right drag orbits, middle drag pans

    def keyPressEvent(self, event):
        if event.key() in self.MOVE_KEYS:
            if not event.isAutoRepeat():
                self._keys.add(event.key())
                if not self._fly.isActive():
                    self._fly_clock.start()
                    self._fly.start()
            return
        super().keyPressEvent(event)

    def keyReleaseEvent(self, event):
        if event.key() in self.MOVE_KEYS:
            if not event.isAutoRepeat():
                self._keys.discard(event.key())
            return
        super().keyReleaseEvent(event)

    def focusOutEvent(self, event):
        self._keys.clear()
        super().focusOutEvent(event)

    def fly(self):
        seconds = self._fly_clock.restart() / 1000.0
        if not self._keys:
            self._fly.stop()
            return
        # W / S along the view direction (pitch included), A / D sideways, Q / E straight down / up
        azimuth, elevation = math.radians(self.opts["azimuth"]), math.radians(self.opts["elevation"])
        forward = -np.array([math.cos(elevation) * math.cos(azimuth), math.cos(elevation) * math.sin(azimuth), math.sin(elevation)])
        right = np.array([-math.sin(azimuth), math.cos(azimuth), 0.0])
        move = np.zeros(3)
        for key in self._keys:
            f, r, u = self.MOVE_KEYS[key]
            move += forward * f + right * r + np.array([0.0, 0.0, u])
        if not move.any():
            return
        speed = max(10.0, self.opts["distance"] * FLY_SPEED)
        if QtWidgets.QApplication.keyboardModifiers() & QtCore.Qt.KeyboardModifier.ShiftModifier:
            speed *= 3
        step = move / np.linalg.norm(move) * speed * seconds
        center = self.opts["center"]
        self.opts["center"] = QtGui.QVector3D(center.x() + step[0], center.y() + step[1], center.z() + step[2])
        self.update()

    def mousePressEvent(self, event):
        self.setFocus()
        self._press = self._last = event.position()
        self._drag_handle = -1
        if event.button() == QtCore.Qt.MouseButton.LeftButton:
            self._drag_handle = self.nearest(self.handles, event.position(), HANDLE_PIXELS)

    def mouseMoveEvent(self, event):
        pos = event.position()
        buttons = event.buttons()
        if self._last is not None and buttons != QtCore.Qt.MouseButton.NoButton:
            dx, dy = pos.x() - self._last.x(), pos.y() - self._last.y()
            self._last = pos
            if buttons & QtCore.Qt.MouseButton.RightButton:
                self.orbit(-dx * 0.4, dy * 0.4)
            elif buttons & QtCore.Qt.MouseButton.MiddleButton:
                self.pan(dx, dy, 0, relative="view-upright")
            elif buttons & QtCore.Qt.MouseButton.LeftButton and self._drag_handle >= 0:
                origin, direction = self.ray(pos)
                vertical = bool(event.modifiers() & QtCore.Qt.KeyboardModifier.ShiftModifier)
                self.cornerDragged.emit(self._drag_handle, origin, direction, vertical)
            return

        # hover: corner handles, else creatures
        handle = self.nearest(self.handles, pos, HANDLE_PIXELS)
        if handle != self.hovered_handle:
            self.hovered_handle = handle
            self.setCursor(QtCore.Qt.CursorShape.SizeAllCursor if handle >= 0 else QtCore.Qt.CursorShape.ArrowCursor)
        mob = self.nearest(self.mob_points, pos, HOVER_PIXELS) if handle < 0 else -1
        self.hovered.emit(self.mob_names[mob] if mob >= 0 else "", event.globalPosition().toPoint())

    def mouseReleaseEvent(self, event):
        if event.button() == QtCore.Qt.MouseButton.LeftButton:
            if self._drag_handle >= 0:
                self.cornerReleased.emit()
            elif self._press is not None and (event.position() - self._press).manhattanLength() < 4:
                origin, direction = self.ray(event.position())
                self.clicked.emit(origin, direction)
        self._press = self._last = None
        self._drag_handle = -1


class Editor(QtWidgets.QMainWindow):
    def __init__(self, path):
        super().__init__()
        self.path = path
        self.tree = ET.parse(path, parser=ET.XMLParser(target=ET.TreeBuilder(insert_comments=True)))
        self.root = self.tree.getroot()
        stages = self.root.find("stages")
        self.boxes = [StageBox.from_element(e) for e in (stages if stages is not None else []) if e.tag == "stage"]
        self.box_items = []
        self.updating = False
        self.drag = None        # (dragged corner, opposite corner) while a corner is dragged

        self.setWindowTitle("Stage editor - %s (map %s)" % (self.root.get("name", ""), self.root.get("map", "?")))
        self.resize(1500, 900)

        self.view = View()
        self.view.setBackgroundColor((25, 25, 30))
        self.view.hovered.connect(self.on_hover)
        self.view.clicked.connect(self.on_click)
        self.view.cornerDragged.connect(self.on_corner_drag)
        self.view.cornerReleased.connect(self.on_corner_released)

        splitter = QtWidgets.QSplitter()
        splitter.addWidget(self.view)
        splitter.addWidget(self.build_panel())
        splitter.setStretchFactor(0, 4)
        splitter.setStretchFactor(1, 1)
        self.setCentralWidget(splitter)

        self.load_readonly()
        self.clear_item = gl.GLScatterPlotItem(pos=np.zeros((0, 3)), size=14, color=(1, 1, 1, 1))
        self.view.addItem(self.clear_item)
        self.handle_item = gl.GLScatterPlotItem(pos=np.zeros((0, 3)), size=12, color=(1.0, 0.9, 0.2, 1.0))
        self.handle_item.setGLOptions("translucent")
        self.handle_item.setDepthValue(10)
        self.view.addItem(self.handle_item)
        self.refresh_boxes()
        self.refresh_list()
        if self.boxes:
            self.list.setCurrentRow(0)

    # -- ui

    def build_panel(self):
        panel = QtWidgets.QWidget()
        layout = QtWidgets.QVBoxLayout(panel)

        self.list = QtWidgets.QListWidget()
        self.list.currentRowChanged.connect(self.on_select)
        layout.addWidget(self.list, 1)

        buttons = QtWidgets.QHBoxLayout()
        for text, slot in (("Add", self.on_add), ("Duplicate", self.on_duplicate), ("Delete", self.on_delete),
                           ("Up", lambda: self.on_move(-1)), ("Down", lambda: self.on_move(1))):
            button = QtWidgets.QPushButton(text)
            button.clicked.connect(slot)
            buttons.addWidget(button)
        layout.addLayout(buttons)

        form = QtWidgets.QFormLayout()
        self.name_edit = QtWidgets.QLineEdit()
        self.name_edit.editingFinished.connect(self.on_edit)
        form.addRow("Stage name", self.name_edit)

        def spin(step):
            box = QtWidgets.QDoubleSpinBox()
            box.setRange(-50000, 50000)
            box.setDecimals(1)
            box.setSingleStep(step)
            box.valueChanged.connect(self.on_edit)
            return box

        self.center = [spin(1.0) for _ in range(3)]
        self.extent = [spin(0.5) for _ in range(3)]
        for box in self.extent:
            box.setMinimum(0.5)
        self.clear = [spin(1.0) for _ in range(3)]
        for label, boxes in (("Center x y z", self.center), ("Half extent x y z", self.extent)):
            row = QtWidgets.QHBoxLayout()
            for box in boxes:
                row.addWidget(box)
            form.addRow(label, row)

        self.clear_check = QtWidgets.QCheckBox("clearposition")
        self.clear_check.toggled.connect(self.on_edit)
        row = QtWidgets.QHBoxLayout()
        for box in self.clear:
            row.addWidget(box)
        form.addRow(self.clear_check, row)
        center_button = QtWidgets.QPushButton("Clear position = box center (floor)")
        center_button.clicked.connect(self.on_clear_center)
        form.addRow("", center_button)

        self.combat_check = QtWidgets.QCheckBox("canbeenteredincombatifnotcleared")
        self.combat_check.toggled.connect(self.on_edit)
        form.addRow("", self.combat_check)
        layout.addLayout(form)

        self.info = QtWidgets.QLabel()
        self.info.setWordWrap(True)
        layout.addWidget(self.info)

        save = QtWidgets.QPushButton("Save")
        save.clicked.connect(self.on_save)
        layout.addWidget(save)
        return panel

    def load_readonly(self):
        readonly = self.root.find("readonly")
        floors = readonly.find("floors") if readonly is not None else None
        mobs = readonly.find("mobs") if readonly is not None else None

        vertices, faces, colors = [], [], []
        if floors is not None:
            for floor in floors.findall("floor"):
                fid = int(floor.get("id", "0"))
                if floor.get("kind") == "ramp":
                    color = (0.55, 0.55, 0.55, 0.85)
                else:
                    r, g, b = colorsys.hsv_to_rgb((fid * 0.618) % 1.0, 0.45, 0.75)
                    color = (r, g, b, 0.85)
                for poly in floor.findall("poly"):
                    values = [float(v) for v in poly.get("v").split()]
                    points = [values[i:i + 3] for i in range(0, len(values), 3)]
                    base = len(vertices)
                    vertices.extend(points)
                    for i in range(1, len(points) - 1):
                        faces.append((base, base + i, base + i + 1))
                        colors.append(color)
        if faces:
            mesh = gl.GLMeshItem(vertexes=np.array(vertices), faces=np.array(faces), faceColors=np.array(colors),
                                 smooth=False, drawEdges=True, edgeColor=(0, 0, 0, 0.35))
            mesh.setGLOptions("opaque")
            self.view.addItem(mesh)

        points, names = [], []
        if mobs is not None:
            for mob in mobs.findall("mob"):
                points.append((float(mob.get("x")), float(mob.get("y")), float(mob.get("z"))))
                names.append("%s (entry %s, guid %s)" % (mob.get("name"), mob.get("entry"), mob.get("guid")))
        if points:
            cylinder = gl.MeshData.cylinder(rows=1, cols=10, radius=[MOB_RADIUS, MOB_RADIUS], length=MOB_HEIGHT)
            cv, cf = cylinder.vertexes(), cylinder.faces()
            all_v = np.concatenate([cv + np.array(p) for p in points])
            all_f = np.concatenate([cf + i * len(cv) for i in range(len(points))])
            mesh = gl.GLMeshItem(vertexes=all_v, faces=all_f, color=(1.0, 0.45, 0.1, 1.0), smooth=False, shader="shaded")
            self.view.addItem(mesh)
            self.view.mob_points = np.array(points) + np.array([0, 0, MOB_HEIGHT / 2])
            self.view.mob_names = names

        # camera on the entrance, else on the mobs
        target = None
        if floors is not None and floors.get("entrance"):
            target = [float(v) for v in floors.get("entrance").split(";")[0].split()]
        elif points:
            target = list(np.mean(points, axis=0))
        if target:
            self.view.opts["center"] = QtGui.QVector3D(*target)
            spread = np.ptp(np.array(points), axis=0).max() if len(points) > 1 else 100.0
            self.view.opts["distance"] = max(80.0, float(spread) * 0.8)

    def refresh_boxes(self):
        for item in self.box_items:
            self.view.removeItem(item)
        self.box_items = []
        selected = self.list.currentRow() if hasattr(self, "list") else -1
        for index, stage_box in enumerate(self.boxes):
            is_selected = index == selected
            color = (1, 1, 1, 1) if is_selected else stage_color(stage_box.name, 0.9)
            item = gl.GLLinePlotItem(pos=box_lines(stage_box), mode="lines", color=color, width=3 if is_selected else 1.5,
                                     antialias=True)
            self.view.addItem(item)
            self.box_items.append(item)
        self.update_handles()
        clears = [b.clear for b in self.boxes if b.clear]
        colors = [(1, 1, 1, 1) if i == selected else stage_color(b.name) for i, b in enumerate(self.boxes) if b.clear]
        self.clear_item.setData(pos=np.array(clears) if clears else np.zeros((0, 3)),
                                color=np.array(colors) if colors else (1, 1, 1, 1))

    def update_handles(self):
        """Corner handles of the selected box: index bits x, y, z (0 low, 1 high); the opposite corner is 7 - index."""
        stage_box = self.current()
        if stage_box is None:
            corners = np.zeros((0, 3))
        else:
            lo, hi = stage_box.bounds()
            corners = np.array([[(lo, hi)[xi][0], (lo, hi)[yi][1], (lo, hi)[zi][2]]
                                for xi in (0, 1) for yi in (0, 1) for zi in (0, 1)])
        self.view.handles = corners
        self.handle_item.setData(pos=corners)

    def on_corner_drag(self, index, origin, direction, vertical):
        stage_box = self.current()
        if stage_box is None or not len(self.view.handles):
            return
        # the corner and its opposite at the start of the drag: the box may flip over while dragging
        if self.drag is None:
            self.drag = (self.view.handles[index].copy(), self.view.handles[7 - index].copy())
        corner, fixed = self.drag[0].copy(), self.drag[1]
        if vertical:
            # plane through the corner, upright, facing the camera: only the height changes
            normal = np.array([direction[0], direction[1], 0.0])
            if np.linalg.norm(normal) < 1e-6:
                return
            denominator = normal @ direction
            if abs(denominator) < 1e-6:
                return
            t = normal @ (corner - origin) / denominator
            corner[2] = (origin + direction * t)[2]
        else:
            # floor plane at the corner's height: x and y change
            if abs(direction[2]) < 1e-6:
                return
            t = (corner[2] - origin[2]) / direction[2]
            if t <= 0:
                return
            hit = origin + direction * t
            corner[0], corner[1] = hit[0], hit[1]
        center = (corner + fixed) / 2
        extent = np.maximum(np.abs(corner - fixed) / 2, 0.5)
        self.drag = (corner, fixed)
        stage_box.box = [round(float(v), 1) for v in center] + [round(float(v), 1) for v in extent]
        self.updating = True
        for i in range(3):
            self.center[i].setValue(stage_box.box[i])
            self.extent[i].setValue(stage_box.box[i + 3])
        self.updating = False
        row = self.list.currentRow()
        self.box_items[row].setData(pos=box_lines(stage_box))
        self.handle_item.setData(pos=np.array([corner, fixed]))

    def on_corner_released(self):
        self.drag = None
        self.on_select(self.list.currentRow())

    def refresh_list(self):
        row = self.list.currentRow()
        self.list.blockSignals(True)
        self.list.clear()
        stage_index = {}
        for b in self.boxes:
            stage_index.setdefault(b.name, len(stage_index))
        for i, b in enumerate(self.boxes):
            item = QtWidgets.QListWidgetItem("%d  %s%s" % (stage_index[b.name], b.name, "  (combat)" if b.combat else ""))
            r, g, bl, _ = stage_color(b.name)
            item.setForeground(QtGui.QColor.fromRgbF(r, g, bl))
            self.list.addItem(item)
        self.list.blockSignals(False)
        if 0 <= row < len(self.boxes):
            self.list.setCurrentRow(row)

    def current(self):
        row = self.list.currentRow()
        return self.boxes[row] if 0 <= row < len(self.boxes) else None

    # -- events

    def on_select(self, row):
        stage_box = self.current()
        self.updating = True
        if stage_box:
            self.name_edit.setText(stage_box.name)
            for i in range(3):
                self.center[i].setValue(stage_box.box[i])
                self.extent[i].setValue(stage_box.box[i + 3])
            self.clear_check.setChecked(stage_box.clear is not None)
            for i in range(3):
                self.clear[i].setValue(stage_box.clear[i] if stage_box.clear else stage_box.box[i])
            self.combat_check.setChecked(stage_box.combat)
            boxes = sum(1 for b in self.boxes if b.name == stage_box.name)
            self.info.setText("Stage '%s': %d box%s" % (stage_box.name, boxes, "es" if boxes > 1 else ""))
        self.updating = False
        self.refresh_boxes()

    def on_edit(self, *_):
        stage_box = self.current()
        if self.updating or not stage_box:
            return
        stage_box.name = self.name_edit.text().strip() or stage_box.name
        stage_box.box = [self.center[i].value() for i in range(3)] + [self.extent[i].value() for i in range(3)]
        stage_box.clear = [self.clear[i].value() for i in range(3)] if self.clear_check.isChecked() else None
        stage_box.combat = self.combat_check.isChecked()
        self.refresh_list()     # name / combat shown in the list
        self.refresh_boxes()

    def on_clear_center(self):
        stage_box = self.current()
        if not stage_box:
            return
        cx, cy, cz, _, _, ez = stage_box.box
        self.updating = True
        for i, v in enumerate((cx, cy, cz - ez + 1.0)):
            self.clear[i].setValue(v)
        self.clear_check.setChecked(True)
        self.updating = False
        self.on_edit()

    def on_add(self):
        c = self.view.opts["center"]
        self.boxes.append(StageBox("New stage %d" % (len(self.boxes) + 1), [c.x(), c.y(), c.z(), 10, 10, 5], None, False))
        self.refresh_list()
        self.list.setCurrentRow(len(self.boxes) - 1)

    def on_duplicate(self):
        stage_box = self.current()
        if not stage_box:
            return
        copy = StageBox(stage_box.name, list(stage_box.box), None, stage_box.combat)
        copy.box[0] += stage_box.box[3] * 2
        self.boxes.insert(self.list.currentRow() + 1, copy)
        self.refresh_list()
        self.list.setCurrentRow(self.list.currentRow() + 1)

    def on_delete(self):
        row = self.list.currentRow()
        if 0 <= row < len(self.boxes):
            del self.boxes[row]
            self.refresh_list()
            self.list.setCurrentRow(min(row, len(self.boxes) - 1))
            self.refresh_boxes()

    def on_move(self, delta):
        row = self.list.currentRow()
        other = row + delta
        if 0 <= row < len(self.boxes) and 0 <= other < len(self.boxes):
            self.boxes[row], self.boxes[other] = self.boxes[other], self.boxes[row]
            self.refresh_list()
            self.list.setCurrentRow(other)

    def on_click(self, origin, direction):
        best, best_t = None, None
        for index, stage_box in enumerate(self.boxes):
            lo, hi = stage_box.bounds()
            with np.errstate(divide="ignore", invalid="ignore"):
                t1, t2 = (lo - origin) / direction, (hi - origin) / direction
            t_near = np.nanmax(np.minimum(t1, t2))
            t_far = np.nanmin(np.maximum(t1, t2))
            if t_near <= t_far and t_far > 0:
                t = max(t_near, 0.0)
                # prefer the box whose surface is hit first; a box around the camera counts at its far side
                if best_t is None or t < best_t:
                    best, best_t = index, t
        if best is not None:
            self.list.setCurrentRow(best)

    def on_hover(self, text, global_pos):
        if text:
            QtWidgets.QToolTip.showText(global_pos, text, self.view)
        else:
            QtWidgets.QToolTip.hideText()

    def on_save(self):
        stages = self.root.find("stages")
        index = list(self.root).index(stages) if stages is not None else 0
        if stages is not None:
            self.root.remove(stages)
        stages = ET.Element("stages")
        for stage_box in self.boxes:
            stages.append(stage_box.to_element())
        self.root.insert(index, stages)
        stagelib.write_xml(self.root, self.path)
        self.statusBar().showMessage("saved %s" % self.path, 5000)


def main():
    app = QtWidgets.QApplication(sys.argv)
    path = sys.argv[1] if len(sys.argv) > 1 else None
    if not path:
        start = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "stages")
        path, _ = QtWidgets.QFileDialog.getOpenFileName(None, "Stages file", start, "Stages (*.xml)")
        if not path:
            return
    editor = Editor(path)
    editor.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
