#pragma once

// Every cutscene the game's mission scripts name: each mission's demoList (the s*_demo.lua scripts)
// without the names that repeat a demo, then the Mother Base cutscenes in the game's queue
// (TppDefine.MB_FREEPLAY_DEMO_PRIORITY_LIST), but for the two that end the mission as the player leaves
// by helicopter. The Developer page's picker shows the labels.
namespace phantom_fx::cutscenes
{
// Lua for TppMission.Reload, which takes a mission to a later stage: other packs, so other actors and
// sets, and at times another location. The game does this on its way through the mission; a cutscene
// from past that point has nothing around it on the mission's first stage.
inline constexpr const char* SameStage = "nil";
inline constexpr const char* HellboundSovietBase = R"({missionPackLabelName = "afterSahelanTestDemo"})";
inline constexpr const char* HellboundSahelan = R"({missionPackLabelName = "beforeSahelanAttackDemo"})";
inline constexpr const char* HellboundMotherBase = R"({missionPackLabelName = "beforeMotherBaseDemo", locationCode = TppDefine.LOCATION_ID.MTBS,
    layoutCode = TppDefine.OFFLINE_MOHTER_BASE_LAYOUT_CODE, clusterId = TppDefine.CLUSTER_DEFINE.Command})";
inline constexpr const char* PitchDarkAfterPump = R"({missionPackLabelName = "afterPumpStopDemo"})";
inline constexpr const char* VoicesVolgin = R"({missionPackLabelName = "AfterVolginDemo"})";
inline constexpr const char* CodeTalkerClear = R"({missionPackLabelName = "CodeTalkerClearDemo"})";
inline constexpr const char* MetallicArchaeaHeli = R"({missionPackLabelName = "AfterClear"})";
inline constexpr const char* MetallicArchaeaMotherBase = R"({missionPackLabelName = "MBdemo", locationCode = TppDefine.LOCATION_ID.MTBS,
    layoutCode = TppDefine.OFFLINE_MOHTER_BASE_LAYOUT_CODE, clusterId = 7})";
inline constexpr const char* SkullFaceArrival = R"({missionPackLabelName = "SkullFaceAppearance"})";
inline constexpr const char* SkullFaceSahelan = R"({missionPackLabelName = "StartingSahelan"})";
inline constexpr const char* SahelanthropusEnding = R"({missionPackLabelName = "OkbEnding", locationCode = TppDefine.LOCATION_ID.MTBS,
    layoutCode = TppDefine.OFFLINE_MOHTER_BASE_LAYOUT_CODE, clusterId = TppDefine.CLUSTER_DEFINE.Develop})";
inline constexpr const char* ShiningLightsInside = R"({missionPackLabelName = "InQuarantineFacility", locationCode = TppDefine.LOCATION_ID.MBQF})";
inline constexpr const char* ShiningLightsFuneral = R"({missionPackLabelName = "out", locationCode = TppDefine.LOCATION_ID.MTBS,
    layoutCode = TppDefine.OFFLINE_MOHTER_BASE_LAYOUT_CODE, clusterId = 7})";

// Lua run as a cutscene starts, for those the game's scripts place, from the demo's id.
inline constexpr const char* Unplaced = "nil";
inline constexpr const char* CentreOfCluster = R"(function(id)
  local position, rotation = MotherBaseStage.GetDemoCenter()
  DemoDaemon.SetDemoTransform(id, rotation, position)
end)";
inline constexpr const char* CentreOfQuarantine = R"(function(id)
  MotherBaseStage.LockCluster(7)
  local position, rotation = MotherBaseStage.GetDemoCenter(7, 0)
  DemoDaemon.SetDemoTransform(id, rotation, position)
end)";
inline constexpr const char* CentreOfDevelop = R"(function(id)
  local cluster = TppDefine.CLUSTER_DEFINE.Develop
  MotherBaseStage.LockCluster(cluster)
  local position, rotation = MotherBaseStage.GetDemoCenter(cluster)
  DemoDaemon.SetDemoTransform(id, rotation, position)
end)";
inline constexpr const char* CentreOfCommand = R"(function(id)
  local cluster = TppDefine.CLUSTER_DEFINE.Command
  MotherBaseStage.LockCluster(cluster)
  local position, rotation = MotherBaseStage.GetDemoCenter(cluster)
  DemoDaemon.SetDemoTransform(id, rotation, position)
end)";

struct Cutscene
{
    int mission;
    const char* name; // In the mission's demoList
    const char* label;
    const char* stage = SameStage;
    const char* place = Unplaced;
};

inline constexpr Cutscene List[]{
    {10010, "beforeOpening", "10010 Awakening: beforeOpening (p21_000100)"},
    {10010, "opening", "10010 Awakening: opening (p21_010000)"},
    {10010, "openingFlag", "10010 Awakening: openingFlag (p21_010000_tgt_flag)"},
    {10010, "flashback", "10010 Awakening: flashback (p21_010020)"},
    {10010, "firstDay", "10010 Awakening: firstDay (p21_010040)"},
    {10010, "fewDaysLater0", "10010 Awakening: fewDaysLater0 (p21_010050)"},
    {10010, "fewDaysLater1", "10010 Awakening: fewDaysLater1 (p21_010050_001)"},
    {10010, "fewDaysLater1_NG0", "10010 Awakening: fewDaysLater1_NG0 (p21_010050_cmn_react000)"},
    {10010, "fewDaysLater1_NG1", "10010 Awakening: fewDaysLater1_NG1 (p21_010050_cmn_react001)"},
    {10010, "fewDaysLater1_NG2", "10010 Awakening: fewDaysLater1_NG2 (p21_010050_cmn_react002)"},
    {10010, "fewDaysLater2", "10010 Awakening: fewDaysLater2 (p21_010050_002)"},
    {10010, "fewDaysLater2_Truth", "10010 Awakening: fewDaysLater2_Truth (p21_010055_002)"},
    {10010, "fewDaysLater2_NG0", "10010 Awakening: fewDaysLater2_NG0 (p21_010050_001_react000)"},
    {10010, "fewDaysLater3", "10010 Awakening: fewDaysLater3 (p21_010050_003)"},
    {10010, "fewDaysLater3_Truth", "10010 Awakening: fewDaysLater3_Truth (p21_010055_003)"},
    {10010, "oneWeekLater", "10010 Awakening: oneWeekLater (p21_010100)"},
    {10010, "twoWeekLater", "10010 Awakening: twoWeekLater (p21_010200)"},
    {10010, "twoWeekLater_Truth", "10010 Awakening: twoWeekLater_Truth (p21_010205)"},
    {10010, "twoWeekLaterReflect", "10010 Awakening: twoWeekLaterReflect (p21_010200_reflect)"},
    {10010, "souvenirPhotograph", "10010 Awakening: souvenirPhotograph (p81_000001)"},
    {10010, "quietAppear", "10010 Awakening: quietAppear (p21_010230)"},
    {10010, "quietAppearTruth", "10010 Awakening: quietAppearTruth (p21_010235)"},
    {10010, "ishmaelAppear", "10010 Awakening: ishmaelAppear (p21_010240)"},
    {10010, "quietExit", "10010 Awakening: quietExit (p21_010250)"},
    {10010, "heli", "10010 Awakening: heli (p21_010260)"},
    {10010, "volgin", "10010 Awakening: volgin (p21_010270)"},
    {10010, "cure", "10010 Awakening: cure (p21_010310)"},
    {10010, "underBed", "10010 Awakening: underBed (p21_010340)"},
    {10010, "underBed2", "10010 Awakening: underBed2 (p21_010340_02)"},
    {10010, "heliKillMob", "10010 Awakening: heliKillMob (p21_010360)"},
    {10010, "soldierKillMob", "10010 Awakening: soldierKillMob (p21_010370)"},
    {10010, "stairway", "10010 Awakening: stairway (p21_010380)"},
    {10010, "corridor0", "10010 Awakening: corridor0 (p21_010410)"},
    {10010, "corridor1", "10010 Awakening: corridor1 (p21_010410_001)"},
    {10010, "corridor2", "10010 Awakening: corridor2 (p21_010410_002)"},
    {10010, "corridor3", "10010 Awakening: corridor3 (p21_010410_003)"},
    {10010, "corridor4", "10010 Awakening: corridor4 (p21_010410_004)"},
    {10010, "corridor5", "10010 Awakening: corridor5 (p21_010410_005)"},
    {10010, "corridor6", "10010 Awakening: corridor6 (p21_010410_006)"},
    {10010, "corridor7", "10010 Awakening: corridor7 (p21_010420_001)"},
    {10010, "curtainRoom", "10010 Awakening: curtainRoom (p21_010420)"},
    {10010, "curtainRoom2", "10010 Awakening: curtainRoom2 (p21_010420_02)"},
    {10010, "volginVsSkullSoldier", "10010 Awakening: volginVsSkullSoldier (p21_010440)"},
    {10010, "getGun", "10010 Awakening: getGun (p21_010490)"},
    {10010, "beforeEntrance", "10010 Awakening: beforeEntrance (p21_010500_001)"},
    {10010, "entrance", "10010 Awakening: entrance (p21_010500_000_final)"},
    {10010, "ambulance", "10010 Awakening: ambulance (p21_010510)"},
    {10010, "heliRotor", "10010 Awakening: heliRotor (p21_010520)"},
    {10010, "tank", "10010 Awakening: tank (p21_010530)"},
    {10010, "volginVsTank", "10010 Awakening: volginVsTank (p21_010540)"},
    {10010, "escapeFromEntrance", "10010 Awakening: escapeFromEntrance (p21_010550)"},
    {10010, "escapeFromHospital", "10010 Awakening: escapeFromHospital (p21_020010)"},
    {10010, "fireWhale", "10010 Awakening: fireWhale (p21_020030_000)"},
    {10010, "passportPhotograph", "10010 Awakening: passportPhotograph (p81_000010)"},
    {10010, "fireWhaleTruth", "10010 Awakening: fireWhaleTruth (p61_000030)"},
    {10010, "bridge", "10010 Awakening: bridge (p21_020040)"},
    {10010, "bridge2", "10010 Awakening: bridge2 (p21_020040_001)"},
    {10010, "twoBigBoss", "10010 Awakening: twoBigBoss (p61_000040)"},

    {10020, "Demo_ArrivalInAfghanistan", "10020 Phantom Limbs: Demo_ArrivalInAfghanistan (p31_010010)"},
    {10020, "Demo_RescueMillerExplanation", "10020 Phantom Limbs: Demo_RescueMillerExplanation (p31_010020)"},
    {10020, "Demo_GetIntel", "10020 Phantom Limbs: Demo_GetIntel (p31_010025)"},
    {10020, "Demo_RescueMiller", "10020 Phantom Limbs: Demo_RescueMiller (p31_010030)"},
    {10020, "Demo_TimeOverDeadMiller", "10020 Phantom Limbs: Demo_TimeOverDeadMiller (p31_010035)"},
    {10020, "Demo_ParasiteAppearance", "10020 Phantom Limbs: Demo_ParasiteAppearance (p31_010040)"},
    {10020, "Demo_ParasiteDiscover_upper", "10020 Phantom Limbs: Demo_ParasiteDiscover_upper (p31_010045)"},
    {10020, "Demo_EscapeWithMillerOnHeli", "10020 Phantom Limbs: Demo_EscapeWithMillerOnHeli (p31_010050)"},

    {10030, "AfterPazResque", "10030 Diamond Dogs: AfterPazResque (p31_010060)"},
    {10030, "ArriveAtDD", "10030 Diamond Dogs: ArriveAtDD (p31_010070)"},
    {10030, "OcelotGiveFulton", "10030 Diamond Dogs: OcelotGiveFulton (p51_010500)"},

    {10040, "Demo_AttackHostage1", "10040 Where Do the Bees Sleep?: Demo_AttackHostage1 (p31_020020_000)"},
    {10040, "Demo_FindHoneyBee1", "10040 Where Do the Bees Sleep?: Demo_FindHoneyBee1 (p31_020025_002)"},
    {10040, "Demo_FindHoneyBee2", "10040 Where Do the Bees Sleep?: Demo_FindHoneyBee2 (p31_020025_003)"},
    {10040, "Demo_RescueHostage", "10040 Where Do the Bees Sleep?: Demo_RescueHostage (p31_020010)"},
    {10040, "Demo_FondHoneyBee02", "10040 Where Do the Bees Sleep?: Demo_FondHoneyBee02 (p31_020050)"},
    {10040, "Demo_EnemyHeli", "10040 Where Do the Bees Sleep?: Demo_EnemyHeli (p31_020027)"},
    {10040, "Demo_Clear", "10040 Where Do the Bees Sleep?: Demo_Clear (p31_020040)"},
    {10040, "Demo_FogStart", "10040 Where Do the Bees Sleep?: Demo_FogStart (p31_020029)"},
    {10040, "Demo_SkullFace", "10040 Where Do the Bees Sleep?: Demo_SkullFace (p31_020030_000_final)"},
    {10040, "Demo_SkullFace2", "10040 Where Do the Bees Sleep?: Demo_SkullFace2 (p31_020035_000_final)"},

    {10044, "Demo_GetIntel", "10044 Occupation Forces: Demo_GetIntel (p31_010025_001)"},

    {10050, "Demo_ConnectKillQuietGame", "10050 Cloaked in Silence: Demo_ConnectKillQuietGame (p31_030110_000_final)"},
    {10050, "Demo_NotKillQuiet", "10050 Cloaked in Silence: Demo_NotKillQuiet (p31_030130_final_000)"},
    {10050, "Demo_QuietDied", "10050 Cloaked in Silence: Demo_QuietDied (p31_030140)"},
    {10050, "Demo_RideHeliWithQuiet", "10050 Cloaked in Silence: Demo_RideHeliWithQuiet (p31_030200_000_final)"},

    {10070, "Demo_SahelanTest", "10070 Hellbound: Demo_SahelanTest (p31_040010_000_final)"},
    {10070, "Demo_ContactHuey", "10070 Hellbound: Demo_ContactHuey (p31_040110)", HellboundSovietBase},
    {10070, "Demo_RideMetal", "10070 Hellbound: Demo_RideMetal (p31_040130_000_final)", HellboundSovietBase},
    {10070, "Demo_SahelanHerald01", "10070 Hellbound: Demo_SahelanHerald01 (p31_040135_000)", HellboundSovietBase},
    {10070, "Demo_SahelanHerald02", "10070 Hellbound: Demo_SahelanHerald02 (p31_040135_001)", HellboundSovietBase},
    {10070, "Demo_SahelanAttacks", "10070 Hellbound: Demo_SahelanAttacks (p31_040140)", HellboundSahelan},
    {10070, "Demo_SahelanFalling", "10070 Hellbound: Demo_SahelanFalling (p31_040160_000_final)", HellboundSahelan},
    {10070, "Demo_MotherBase", "10070 Hellbound: Demo_MotherBase (p31_040200_000_final)", HellboundMotherBase},

    {10080, "Demo_InAfrica", "10080 Pitch Dark: Demo_InAfrica (p41_030005_000_final)"},
    {10080, "Demo_ArrivalInAfrica", "10080 Pitch Dark: Demo_ArrivalInAfrica (p41_030010_000_final)"},
    {10080, "Demo_TeachChild", "10080 Pitch Dark: Demo_TeachChild (p41_030030_000_final)"},
    {10080, "Demo_DeadBody", "10080 Pitch Dark: Demo_DeadBody (p41_030040_000_final)"},
    {10080, "Demo_Reinforcement", "10080 Pitch Dark: Demo_Reinforcement (p41_030050_000_final)", PitchDarkAfterPump},

    {10081, "Demo_PrisonBreak_door", "10081 Root Cause: Demo_PrisonBreak_door (p42_010000)"},
    {10081, "Demo_PrisonBreak_window", "10081 Root Cause: Demo_PrisonBreak_window (p42_010010)"},

    {10090, "Demo_ParasiteAppearance", "10090 Traitors' Caravan: Demo_ParasiteAppearance (p41_050010)"},
    {10090, "Demo_TargetEscape", "10090 Traitors' Caravan: Demo_TargetEscape (p41_050020_000_final)"},

    {10100, "Demo_Brank_30", "10100 Blood Runs Deep: Demo_Brank_30 (p81_000020_30f)"},
    {10100, "Demo_MeetBoySoldier", "10100 Blood Runs Deep: Demo_MeetBoySoldier (p41_020020_000)"},

    {10110, "bedDemoName", "10110 Voices: bedDemoName (p41_040130_000_final)"},
    {10110, "volginDemoNameBefore", "10110 Voices: volginDemoNameBefore (p41_040145)"},
    {10110, "volginDemoName", "10110 Voices: volginDemoName (p41_040150_000_final)", VoicesVolgin},

    {10120, "Demo_Liquid_combatcamera", "10120 The White Mamba: Demo_Liquid_combatcamera (p41_010005)"},
    {10120, "Demo_Liquid_combat", "10120 The White Mamba: Demo_Liquid_combat (p41_010010_000_final)"},
    {10120, "Demo_Liquid_combatEnd", "10120 The White Mamba: Demo_Liquid_combatEnd (p41_010030_000_final)"},
    {10120, "Demo_Liquid_HeliEscape", "10120 The White Mamba: Demo_Liquid_HeliEscape (p41_010040_000_final)"},

    {10130, "ContactCodetalkerDemo", "10130 Code Talker: ContactCodetalkerDemo (p41_060010)"},
    {10130, "CemeteryCodetalkerDemo", "10130 Code Talker: CemeteryCodetalkerDemo (p41_060020)"},
    {10130, "ClearDemo", "10130 Code Talker: ClearDemo (p41_060110)", CodeTalkerClear},
    {10130, "ArrivalParasiteDemo", "10130 Code Talker: ArrivalParasiteDemo (p41_060005)"},

    {10140, "Demo_Opening", "10140 Metallic Archaea: Demo_Opening (p41_060210)"},
    {10140, "Demo_CodeTalker_Evacuate", "10140 Metallic Archaea: Demo_CodeTalker_Evacuate (p41_060220)"},
    {10140, "Demo_Inside_Heli", "10140 Metallic Archaea: Demo_Inside_Heli (p41_060250_000_final)", MetallicArchaeaHeli},
    {10140, "Demo_TorturedHueyAtMB", "10140 Metallic Archaea: Demo_TorturedHueyAtMB (p41_060260_000_final)", MetallicArchaeaMotherBase},
    {10140, "Demo_GoToLastMission", "10140 Metallic Archaea: Demo_GoToLastMission (p41_060270_000_final)", MetallicArchaeaMotherBase},
    {10140, "Demo_PandemicConverge", "10140 Metallic Archaea: Demo_PandemicConverge (p41_060255)", MetallicArchaeaMotherBase, CentreOfQuarantine},

    {10150, "Demo_OpOKBZero", "10150 Skull Face: Demo_OpOKBZero (p31_050005)"},
    {10150, "Demo_SkullFaceAppearance", "10150 Skull Face: Demo_SkullFaceAppearance (p31_050010_000_final)", SkullFaceArrival},
    {10150, "Demo_SahelanAppearance", "10150 Skull Face: Demo_SahelanAppearance (p31_050025_000_final)", SkullFaceSahelan},

    {10151, "Demo_SahelanAppearance", "10151 Sahelanthropus: Demo_SahelanAppearance (p31_050026_000_final)"},
    {10151, "Demo_BreakSahelan", "10151 Sahelanthropus: Demo_BreakSahelan (p31_050040_000_final)"},
    {10151, "Demo_DeadSkullFace", "10151 Sahelanthropus: Demo_DeadSkullFace (p31_050050_000_final)"},
    {10151, "Demo_MillerStandIdle", "10151 Sahelanthropus: Demo_MillerStandIdle (p31_050050_002)"},
    {10151, "Demo_AfterDeadSkullFace", "10151 Sahelanthropus: Demo_AfterDeadSkullFace (p31_050050_001_final)"},
    {10151, "Demo_Ending1", "10151 Sahelanthropus: Demo_Ending1 (p31_050060_000_final)", SahelanthropusEnding, CentreOfDevelop},
    {10151, "Demo_Ending2", "10151 Sahelanthropus: Demo_Ending2 (p31_050070_000_final)", SahelanthropusEnding, CentreOfDevelop},
    {10151, "Demo_Ending3", "10151 Sahelanthropus: Demo_Ending3 (p31_050080)", SahelanthropusEnding, CentreOfCommand},
    {10151, "Demo_SkullShot1", "10151 Sahelanthropus: Demo_SkullShot1 (p31_050050_003)"},
    {10151, "Demo_SkullShot2", "10151 Sahelanthropus: Demo_SkullShot2 (p31_050050_004)"},
    {10151, "Demo_SkullShot3", "10151 Sahelanthropus: Demo_SkullShot3 (p31_050050_005)"},

    {10240, "Demo_Opening", "10240 Shining Lights, Even in Death: Demo_Opening (p51_070020_000_final)"},
    {10240, "Demo_GoToInside", "10240 Shining Lights, Even in Death: Demo_GoToInside (p51_070110_000_final)", SameStage, CentreOfCluster},
    {10240, "Demo_Roof_Event", "10240 Shining Lights, Even in Death: Demo_Roof_Event (p51_070220_000_final)", ShiningLightsInside},
    {10240, "Demo_Exit", "10240 Shining Lights, Even in Death: Demo_Exit (p51_070300_000_final)", ShiningLightsInside},
    {10240, "Demo_Funeral", "10240 Shining Lights, Even in Death: Demo_Funeral (p51_070500_000_final)", ShiningLightsFuneral, CentreOfCluster},
    {10240, "Demo_EnterInMBQF", "10240 Shining Lights, Even in Death: Demo_EnterInMBQF (p51_070120_000_final)", ShiningLightsInside},
    {10240, "Demo_ExitWithLiveMan", "10240 Shining Lights, Even in Death: Demo_ExitWithLiveMan (p51_070280_000_final)", ShiningLightsInside},

    {10260, "Demo_EncountQuiet", "10260 A Quiet Exit: Demo_EncountQuiet (p31_070005)"},
    {10260, "Demo_QuietChoked", "10260 A Quiet Exit: Demo_QuietChoked (p31_070010)"},
    {10260, "Demo_AfterTankBattle", "10260 A Quiet Exit: Demo_AfterTankBattle (p31_070015_000_final)"},
    {10260, "Demo_SnakeBite_01", "10260 A Quiet Exit: Demo_SnakeBite_01 (p31_070020_000_final)"},
    {10260, "Demo_SnakeBite_02", "10260 A Quiet Exit: Demo_SnakeBite_02 (p31_070020_001_final)"},
    {10260, "Demo_QuietSpeak", "10260 A Quiet Exit: Demo_QuietSpeak (p31_070030_000_final)"},
    {10260, "Demo_QuietVanishing", "10260 A Quiet Exit: Demo_QuietVanishing (p31_070040_000_final)"},
    {10260, "Demo_CameraForLetter", "10260 A Quiet Exit: Demo_CameraForLetter (p31_070050_002)"},
    {10260, "Demo_LetterFromQuiet", "10260 A Quiet Exit: Demo_LetterFromQuiet (p31_070050_000_final)"},
    {10260, "Demo_QuietDrifted", "10260 A Quiet Exit: Demo_QuietDrifted (p31_070050_001_final)"},

    {30010, "Demo_RecoverReptilePod", "30010 Afghanistan free roam: Demo_RecoverReptilePod (p31_060010)"},
    {30010, "Demo_AttackedByQuiet_1", "30010 Afghanistan free roam: Demo_AttackedByQuiet_1 (p31_030100_000)"},
    {30010, "Demo_AttackedByQuiet_2", "30010 Afghanistan free roam: Demo_AttackedByQuiet_2 (p31_030100_001)"},
    {30010, "Demo_RescueSpecial", "30010 Afghanistan free roam: Demo_RescueSpecial (p31_080010_000_final)"},
    {30010, "Demo_RecoverVolgin", "30010 Afghanistan free roam: Demo_RecoverVolgin (p31_080100_000_final)"},

    {30250, "Demo_RecoverVolgin", "30250 Mother Base ward: Demo_RecoverVolgin (p31_080110_000_final)", SameStage, CentreOfQuarantine},

    {30050, "AnableDevBattleGear", "30050 Mother Base: AnableDevBattleGear (p51_010140_000_final)"},
    {30050, "ArrivedMotherBaseAfterQuietBattle", "30050 Mother Base: ArrivedMotherBaseAfterQuietBattle (p31_030220_000_final)"},
    {30050, "ArrivedMotherBaseChildren", "30050 Mother Base: ArrivedMotherBaseChildren (p41_020050_000_final)"},
    {30050, "ArrivedMotherBaseFromDeathFactory", "30050 Mother Base: ArrivedMotherBaseFromDeathFactory (p41_040200_000_final)"},
    {30050, "ArrivedMotherBaseLiquid", "30050 Mother Base: ArrivedMotherBaseLiquid (p41_010050_000_final)"},
    {30050, "AttackedFromOtherPlayer_KnowWhereFrom", "30050 Mother Base: AttackedFromOtherPlayer_KnowWhereFrom (p51_010150_000_final)"},
    {30050, "AttackedFromOtherPlayer_UnknowWhereFrom", "30050 Mother Base: AttackedFromOtherPlayer_UnknowWhereFrom (p51_010150_001_final)"},
    {30050, "CodeTalkerSunBath", "30050 Mother Base: CodeTalkerSunBath (p51_010170_000_final)"},
    {30050, "DdogComeToGet", "30050 Mother Base: DdogComeToGet (p51_010020_000_final)"},
    {30050, "DecisionHuey", "30050 Mother Base: DecisionHuey (p31_060030_000_final)"},
    {30050, "DetailsNuclearDevelop", "30050 Mother Base: DetailsNuclearDevelop (p51_020030_01movie)"},
    {30050, "DevelopedBattleGear1", "30050 Mother Base: DevelopedBattleGear1 (p51_010145_001)"},
    {30050, "DevelopedBattleGear2", "30050 Mother Base: DevelopedBattleGear2 (p51_010145_002)"},
    {30050, "DevelopedBattleGear4", "30050 Mother Base: DevelopedBattleGear4 (p51_010145_004)"},
    {30050, "DevelopedBattleGear5", "30050 Mother Base: DevelopedBattleGear5 (p51_010145_005)"},
    {30050, "EliLookSnake", "30050 Mother Base: EliLookSnake (p51_010220_000_final)"},
    {30050, "EndingSacrificeOfNuclear", "30050 Mother Base: EndingSacrificeOfNuclear (p51_020030_02movie)"},
    {30050, "EntrustDdog", "30050 Mother Base: EntrustDdog (p51_010010)"},
    {30050, "ForKeepNuclearElimination", "30050 Mother Base: ForKeepNuclearElimination (p51_020020_000_final)"},
    {30050, "GoToMotherBaseAfterQuietBattle", "30050 Mother Base: GoToMotherBaseAfterQuietBattle (p31_030210)"},
    {30050, "HappyBirthDay", "30050 Mother Base: HappyBirthDay (p51_010270_000)"},
    {30050, "HappyBirthDayWithQuiet", "30050 Mother Base: HappyBirthDayWithQuiet (p51_010270_001)"},
    {30050, "InterrogateQuiet", "30050 Mother Base: InterrogateQuiet (p51_010210)"},
    {30050, "LiquidAndChildSoldier", "30050 Mother Base: LiquidAndChildSoldier (p51_010340_000_final)"},
    {30050, "LongTimeNoSee_DDSoldier", "30050 Mother Base: LongTimeNoSee_DDSoldier (p51_010060_000_final)"},
    {30050, "LongTimeNoSee_DdogHighLikability", "30050 Mother Base: LongTimeNoSee_DdogHighLikability (p51_010057_000_final)"},
    {30050, "LongTimeNoSee_DdogLowLikability", "30050 Mother Base: LongTimeNoSee_DdogLowLikability (p51_010053_000_final)"},
    {30050, "LongTimeNoSee_DdogPup", "30050 Mother Base: LongTimeNoSee_DdogPup (p51_010050_000_final)"},
    {30050, "LongTimeNoSee_DdogSuperHighLikability", "30050 Mother Base: LongTimeNoSee_DdogSuperHighLikability (p51_010059_000_final)"},
    {30050, "MoraleOfMBIsLow", "30050 Mother Base: MoraleOfMBIsLow (p51_010040_000_final)"},
    {30050, "NuclearEliminationCeremony", "30050 Mother Base: NuclearEliminationCeremony (p51_020010_000_final)"},
    {30050, "OcelotIsPupilOfSnake", "30050 Mother Base: OcelotIsPupilOfSnake (p51_010350_000_final)"},
    {30050, "ParasiticWormCarrierKill", "30050 Mother Base: ParasiticWormCarrierKill (p51_070010_000_final)"},
    {30050, "PazPhantomPain1", "30050 Mother Base: PazPhantomPain1 (p51_080010_000_final)"},
    {30050, "PazPhantomPain2", "30050 Mother Base: PazPhantomPain2 (p51_080020)"},
    {30050, "PazPhantomPain4", "30050 Mother Base: PazPhantomPain4 (p51_080040)"},
    {30050, "PazPhantomPain4_jp", "30050 Mother Base: PazPhantomPain4_jp (p51_080041)"},
    {30050, "QuietHasFriendshipWithChild", "30050 Mother Base: QuietHasFriendshipWithChild (p51_010310_000)"},
    {30050, "QuietOnHeliInRain", "30050 Mother Base: QuietOnHeliInRain (p51_010300_000_final)"},
    {30050, "QuietReceivesPersecution", "30050 Mother Base: QuietReceivesPersecution (p51_010360_000)"},
    {30050, "SacrificeOfNuclearElimination", "30050 Mother Base: SacrificeOfNuclearElimination (p51_020040_000_final)"},
    {30050, "SnakeHasBadSmell_000", "30050 Mother Base: SnakeHasBadSmell_000 (p51_010165_000_final)"},
    {30050, "SnakeHasBadSmell_001", "30050 Mother Base: SnakeHasBadSmell_001 (p51_010165_001_final)"},
    {30050, "SnakeHasBadSmell_WithoutQuiet", "30050 Mother Base: SnakeHasBadSmell_WithoutQuiet (p51_010160_000_final)"},
    {30050, "TheGreatEscapeLiquid", "30050 Mother Base: TheGreatEscapeLiquid (p61_010030_000_final)"},
};
}
