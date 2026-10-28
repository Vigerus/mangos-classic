-- Scholomance (map 289): the packs at the Reliquary aggro and evade together (CREATURE_GROUP_AGGRO_TOGETHER | CREATURE_GROUP_EVADE_TOGETHER)
DELETE FROM spawn_group_spawn WHERE Id BETWEEN 2890001 AND 2890004;
DELETE FROM spawn_group WHERE Id BETWEEN 2890001 AND 2890004;

INSERT INTO spawn_group (Id, Name, Type, MaxCount, WorldState, Flags) VALUES
(2890001, 'Scholomance - Reliquary stairs Risen Guards', 0, 0, 0, 5),
(2890002, 'Scholomance - Reliquary Spectral Researcher, Neophyte, Acolytes', 0, 0, 0, 5),
(2890003, 'Scholomance - Reliquary Neophytes, Acolyte', 0, 0, 0, 5),
(2890004, 'Scholomance - Reliquary Neophyte, Acolyte', 0, 0, 0, 5);

INSERT INTO spawn_group_spawn (Id, Guid, SlotId) VALUES
(2890001, 2890296, -1),
(2890001, 2890297, -1),
(2890002, 2890344, -1),
(2890002, 2890084, -1),
(2890002, 2890095, -1),
(2890002, 2890098, -1),
(2890003, 2890078, -1),
(2890003, 2890083, -1),
(2890003, 2890103, -1),
(2890004, 2890081, -1),
(2890004, 2890093, -1);
