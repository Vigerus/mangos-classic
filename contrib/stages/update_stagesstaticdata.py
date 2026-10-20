"""Regenerates the <readonly> section (navmesh floors, creature spawns) of stages/*.xml from the server's mmaps and
world database. The <stages> section is left as it is.

    python update_stagesstaticdata.py                         # every file of the stages directory
    python update_stagesstaticdata.py ..\\..\\stages\\scholomance.xml
    python update_stagesstaticdata.py --conf F:\\server\\vanillanew\\mangosd.conf
"""

import argparse
import os
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stagelib  # noqa: E402


def update(server, path):
    tree = ET.parse(path, parser=ET.XMLParser(target=ET.TreeBuilder(insert_comments=True)))
    root = tree.getroot()
    map_id = int(root.get("map"))
    for old in root.findall("readonly"):
        root.remove(old)
    readonly = stagelib.readonly_element(server, map_id)
    root.append(readonly)
    stagelib.write_xml(root, path)
    floors = readonly.find("floors")
    print("%-28s map %3d: %d floors, %d links, %d mobs" % (os.path.basename(path), map_id, len(floors.findall("floor")),
                                                         len(floors.findall("link")), len(readonly.find("mobs"))))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("files", nargs="*", help="stages files (default: every file of --dir)")
    parser.add_argument("--conf", default=stagelib.DEFAULT_CONF, help="mangosd.conf (database, data dir)")
    parser.add_argument("--dir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "stages"))
    args = parser.parse_args()

    server = stagelib.ServerData(args.conf)
    for path in args.files or stagelib.stages_files(args.dir):
        update(server, path)


if __name__ == "__main__":
    main()
