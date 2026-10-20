"""Creates stages/<dungeon>.xml for every dungeon and raid (instance_template) that has no file yet: stages made by
clustering the creature spawns, plus the <readonly> navmesh / mob data (see stagelib.py).

    python generate_stages.py                       # every instance without a file
    python generate_stages.py --map 36 --force      # regenerate one (its stages are lost)
    python generate_stages.py --conf F:\\server\\vanillanew\\mangosd.conf --out ..\\..\\stages

The stages are a first guess: one per group of creatures standing together (bosses name their group), in walking
order from the entrance, boxes around the group, the clear position at a creature near its middle. Tune them with
stage_editor.py.
"""

import argparse
import math
import os
import sys
import xml.etree.ElementTree as ET
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stagelib  # noqa: E402

# map id -> file / display name
INSTANCE_NAMES = {
    33: "Shadowfang Keep", 34: "The Stockade", 36: "The Deadmines", 43: "Wailing Caverns", 47: "Razorfen Kraul",
    48: "Blackfathom Deeps", 70: "Uldaman", 90: "Gnomeregan", 109: "Sunken Temple", 129: "Razorfen Downs",
    189: "Scarlet Monastery", 209: "Zul'Farrak", 229: "Blackrock Spire", 230: "Blackrock Depths",
    249: "Onyxia's Lair", 289: "Scholomance", 309: "Zul'Gurub", 329: "Stratholme", 349: "Maraudon",
    389: "Ragefire Chasm", 409: "Molten Core", 429: "Dire Maul", 469: "Blackwing Lair", 509: "Ruins of Ahn'Qiraj",
    531: "Temple of Ahn'Qiraj", 533: "Naxxramas",
}

NEIGHBOUR_XY = 15.0     # creatures this close (2D) ...
NEIGHBOUR_Z = 6.0       # ... and this close in height stand together
MERGE_SMALL = 30.0      # groups of 1-2 creatures join a group this close
MAX_EXTENT = 80.0       # groups wider than this are split in two (along their longest side)
MARGIN_XY = 5.0
MARGIN_BELOW = 3.0
MARGIN_ABOVE = 6.0


def file_name(map_id):
    name = INSTANCE_NAMES.get(map_id, "map_%d" % map_id)
    return "".join(c for c in name.lower().replace(" ", "_") if c.isalnum() or c == "_") + ".xml"


def cluster(mobs):
    """Groups of mobs standing together (single linkage)."""
    parent = list(range(len(mobs)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for i in range(len(mobs)):
        for j in range(i + 1, len(mobs)):
            a, b = mobs[i], mobs[j]
            if abs(a["z"] - b["z"]) <= NEIGHBOUR_Z and math.hypot(a["x"] - b["x"], a["y"] - b["y"]) <= NEIGHBOUR_XY:
                parent[find(i)] = find(j)

    groups = {}
    for i in range(len(mobs)):
        groups.setdefault(find(i), []).append(mobs[i])
    groups = list(groups.values())

    # stragglers (1-2 creatures) join the closest group nearby
    big = [g for g in groups if len(g) >= 3]
    for group in [g for g in groups if len(g) < 3]:
        best, best_d = None, None
        for other in big:
            distances = [math.hypot(a["x"] - b["x"], a["y"] - b["y"]) for a in group for b in other
                         if abs(a["z"] - b["z"]) <= NEIGHBOUR_Z]
            d = min(distances) if distances else None
            if d is not None and d <= MERGE_SMALL and (best_d is None or d < best_d):
                best, best_d = other, d
        if best is not None:
            best.extend(group)
        else:
            big.append(group)
    return [part for group in big for part in split(group)]


def split(group):
    """Halves a group that is too wide (chains of creatures along a corridor or a big hall) until it fits."""
    xs, ys = [m["x"] for m in group], [m["y"] for m in group]
    width, depth = max(xs) - min(xs), max(ys) - min(ys)
    if max(width, depth) <= MAX_EXTENT or len(group) < 2:
        return [group]
    axis = "x" if width >= depth else "y"
    ordered = sorted(group, key=lambda m: m[axis])
    half = len(ordered) // 2
    return split(ordered[:half]) + split(ordered[half:])


def generate_stages(server, map_id, mesh, reach):
    stages = ET.Element("stages")
    entrance = server.entrance(map_id)
    if entrance:     # the first entrance; other wings' entrances are ordinary stages
        stage = ET.SubElement(stages, "stage", name="Entrance",
                              box=" ".join(stagelib.fmt(v) for v in (*entrance, 12.0, 12.0, 8.0)),
                              clearposition="", canbeenteredincombatifnotcleared="false")

    mobs = [m for m in server.mobs(map_id) if m["counted"]]
    bosses = server.bosses(map_id)

    # walking distance from the entrance of each mob's poly, to order the stages
    reach_polys = list(reach.keys())
    centers = {p: mesh.center(mesh.polys[p]) for p in reach_polys}

    def walk_distance(mob):
        if not reach_polys:
            return math.dist((mob["x"], mob["y"], mob["z"]), entrance) if entrance else 0.0
        best, best_d = None, None
        for poly, (x, y, z) in centers.items():
            d = (x - mob["x"]) ** 2 + (y - mob["y"]) ** 2 + ((z - mob["z"]) * 2) ** 2
            if best_d is None or d < best_d:
                best, best_d = poly, d
        return reach[best] + math.sqrt(best_d)

    groups = cluster(mobs)
    ordered = sorted(groups, key=lambda g: min(walk_distance(m) for m in g))
    used_names = Counter()
    for index, group in enumerate(ordered, 1):
        xs, ys, zs = [m["x"] for m in group], [m["y"] for m in group], [m["z"] for m in group]
        min_x, max_x = min(xs) - MARGIN_XY, max(xs) + MARGIN_XY
        min_y, max_y = min(ys) - MARGIN_XY, max(ys) + MARGIN_XY
        min_z, max_z = min(zs) - MARGIN_BELOW, max(zs) + MARGIN_ABOVE
        box = ((min_x + max_x) / 2, (min_y + max_y) / 2, (min_z + max_z) / 2,
               (max_x - min_x) / 2, (max_y - min_y) / 2, (max_z - min_z) / 2)

        boss_names = []
        for m in group:
            if m["entry"] in bosses and bosses[m["entry"]] not in boss_names:
                boss_names.append(bosses[m["entry"]])
        if boss_names:
            name = " / ".join(boss_names)
        else:
            common = Counter(m["name"] for m in group).most_common(1)[0][0]
            name = "%s (%d)" % (common, len(group))
        used_names[name] += 1
        if used_names[name] > 1:
            name = "%s #%d" % (name, used_names[name])

        cx, cy = sum(xs) / len(xs), sum(ys) / len(ys)
        clear = min(group, key=lambda m: (m["x"] - cx) ** 2 + (m["y"] - cy) ** 2)
        ET.SubElement(stages, "stage", name=name, box=" ".join(stagelib.fmt(v) for v in box),
                      clearposition=" ".join(stagelib.fmt(v) for v in (clear["x"], clear["y"], clear["z"])),
                      canbeenteredincombatifnotcleared="false")
    return stages


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--conf", default=stagelib.DEFAULT_CONF, help="mangosd.conf (database, data dir)")
    parser.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "stages"))
    parser.add_argument("--map", type=int, action="append", help="only these map ids")
    parser.add_argument("--force", action="store_true", help="overwrite the --map files (their stages are lost)")
    args = parser.parse_args()
    if args.force and not args.map:
        parser.error("--force overwrites hand-edited stages: name the maps with --map")

    server = stagelib.ServerData(args.conf)
    os.makedirs(args.out, exist_ok=True)
    for map_id, _ in server.instances():
        if args.map and map_id not in args.map:
            continue
        path = os.path.join(args.out, file_name(map_id))
        if os.path.exists(path) and not args.force:
            print("%-28s exists, skipped" % os.path.basename(path))
            continue

        cache = {}
        readonly = stagelib.readonly_element(server, map_id, cache)
        mesh, reach = cache[map_id]
        root = ET.Element("root", map=str(map_id), name=INSTANCE_NAMES.get(map_id, "map %d" % map_id))
        stages = generate_stages(server, map_id, mesh, reach)
        root.append(stages)
        root.append(readonly)
        stagelib.write_xml(root, path)
        print("%-28s %3d stages, %4d reachable polys, %d floors" % (
            os.path.basename(path), len(stages), len(reach), len(readonly.find("floors").findall("floor"))))


if __name__ == "__main__":
    main()
