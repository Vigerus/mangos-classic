# Dungeon stage tools

Tools for `stages/<dungeon>.xml`, the rooms (stages) of each dungeon and raid that the server's `InstanceSubsystem`
loads when a dungeon's first instance is created (config `Instance.StagesDir`, default `stages`). Bots wait outside
stages that are not cleared, stand at a stage's clear position, and the DungeonStage addon lists them.

```xml
<root map="289" name="Scholomance">
  <stages>
    <stage name="Reliquary" box="202.5 18 117 52.5 48 7" clearposition="209.4 12.4 115.8" canbeenteredincombatifnotcleared="false"/>
  </stages>
  <readonly>
    <floors entrance="190.8 126.3 137.2"> <floor id="0" kind="floor" z="83.8" area="4320"> <poly v="x y z ..."/> </floor> <link a="0" b="5" .../> </floors>
    <mobs> <mob name="Rattlegore" entry="11622" guid="..." x="" y="" z=""/> </mobs>
  </readonly>
</root>
```

- `<stages>`: edited by hand / with the editor. `box` is the center and the half extents (no rotation); stages with
  the same name are one stage of several boxes; the first stage is the entrance (always cleared); a point belongs to
  the first stage containing it. `clearposition` (optional): where bots stand once the stage is cleared (their own rtsc
  spot wins). `canbeenteredincombatifnotcleared`: bots may chase into the box in combat before it is cleared.
- `<readonly>`: generated, ignored by the server (it counts the creatures of each box from the world database itself,
  and a stage with an encounter creature of `instance_encounters` is a boss stage). `floors`: the navmesh polygons
  reachable from the entrances, grouped into flat floors and ramps / stairs (`kind`), `link`s where they meet; `mobs`:
  every creature spawn.

All scripts read the database and data paths from the server's `mangosd.conf` (`--conf`, default
`F:\server\vanillanew\mangosd.conf`) and need `mysql-connector-python`; the editor also `PyQt6`, `pyqtgraph`,
`PyOpenGL` and `numpy`.

| script | does |
|--------|------|
| `generate_stages.py` | creates the file of every dungeon / raid of `instance_template` that has none: stages from groups of creatures standing together (named after their boss, else their most common creature), in walking order from the entrance, plus the readonly data. `--map <id> --force` regenerates one (its stages are lost) |
| `update_stagesstaticdata.py` | regenerates only the `<readonly>` sections (all files, or the files given); `<stages>` are kept |
| `stage_editor.py <file>` | 3D editor: navmesh floors, creatures as cylinders (hover for the name), stage boxes; select a box in the list or click it, drag its corners (Shift: height) or edit name, center, half extents, clear position, canbeenteredincombatifnotcleared; add / duplicate / delete / reorder boxes. Camera: W / S fly along the view direction, A / D sideways, Q / E down / up (Shift faster), right drag rotates, middle drag pans, wheel zooms. Save writes the file, closing the window discards the changes |
| `stagelib.py` | shared code: database, dbc, mmaps tile reader, floors, xml |

`deploy_watch.py` copies `stages` next to `statetree` into the server directory. In game, `.instance stages` lists the
stages of your instance and `.instance stages show [n]` marks a stage's boxes.
