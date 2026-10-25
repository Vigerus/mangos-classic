-- Scholomance Dark Summoner: Summon Frail Skeleton (16531) puts the skeleton at the caster's position
-- (spell_summon_frail_skeleton), and Summon Risen Lackey (17618) raises the lackey only at the caster's own Frail
-- Skeleton (spell_summon_risen_lackey), not at the nearest skeleton of another Dark Summoner's pack
DELETE FROM spell_scripts WHERE Id IN (16531, 17618);
INSERT INTO spell_scripts (Id, ScriptName) VALUES
(16531, 'spell_summon_frail_skeleton'),
(17618, 'spell_summon_risen_lackey');
