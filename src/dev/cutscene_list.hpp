#pragma once

#include <string_view>

// Every cutscene the game's mission scripts name: each mission's demoList (the s*_demo.lua scripts)
// without the names that repeat a demo, then the Mother Base cutscenes in the game's queue
// (TppDefine.MB_FREEPLAY_DEMO_PRIORITY_LIST), but for the six docs/DEVELOPMENT.md leaves out under
// "Known limits". The Developer page's picker shows "code title: name (demo id)".
namespace phantom_fx::cutscenes
{
// Lua the stages and places below call, ahead of the chunk that reads them. Cluster 7 has no name in
// TppDefine.CLUSTER_DEFINE.
inline constexpr std::string_view ListHelpers = R"(
local QuarantineCluster = 7

local function motherBaseStage(packLabel, clusterId)
  return {missionPackLabelName = packLabel, locationCode = TppDefine.LOCATION_ID.MTBS,
    layoutCode = TppDefine.OFFLINE_MOHTER_BASE_LAYOUT_CODE, clusterId = clusterId}
end

local function centreOn(cluster, plant)
  return function(id)
    local position, rotation = MotherBaseStage.GetDemoCenter(cluster, plant)
    DemoDaemon.SetDemoTransform(id, rotation, position)
  end
end

local function lockAndCentreOn(cluster, plant)
  local centre = centreOn(cluster, plant)
  return function(id)
    MotherBaseStage.LockCluster(cluster)
    centre(id)
  end
end
)";

// Lua for TppMission.Reload, which takes a mission to a later stage: other packs, so other actors and
// sets, and at times another location. The game does this on its way through the mission; a cutscene
// from past that point has nothing around it on the mission's first stage.
inline constexpr const char* SameStage = "nil";
inline constexpr const char* HellboundSovietBase = R"({missionPackLabelName = "afterSahelanTestDemo"})";
inline constexpr const char* HellboundSahelan = R"({missionPackLabelName = "beforeSahelanAttackDemo"})";
inline constexpr const char* HellboundMotherBase = R"(motherBaseStage("beforeMotherBaseDemo", TppDefine.CLUSTER_DEFINE.Command))";
inline constexpr const char* PitchDarkAfterPump = R"({missionPackLabelName = "afterPumpStopDemo"})";
inline constexpr const char* VoicesVolgin = R"({missionPackLabelName = "AfterVolginDemo"})";
inline constexpr const char* CodeTalkerClear = R"({missionPackLabelName = "CodeTalkerClearDemo"})";
inline constexpr const char* MetallicArchaeaHeli = R"({missionPackLabelName = "AfterClear"})";
inline constexpr const char* MetallicArchaeaMotherBase = R"(motherBaseStage("MBdemo", QuarantineCluster))";
inline constexpr const char* SkullFaceArrival = R"({missionPackLabelName = "SkullFaceAppearance"})";
inline constexpr const char* SkullFaceSahelan = R"({missionPackLabelName = "StartingSahelan"})";
inline constexpr const char* SahelanthropusEnding = R"(motherBaseStage("OkbEnding", TppDefine.CLUSTER_DEFINE.Develop))";
inline constexpr const char* ShiningLightsInside = R"({missionPackLabelName = "InQuarantineFacility", locationCode = TppDefine.LOCATION_ID.MBQF})";
inline constexpr const char* ShiningLightsFuneral = R"(motherBaseStage("out", QuarantineCluster))";

// Lua run as a cutscene starts, for those the game's scripts place, from the demo's id. Metallic
// Archaea keeps cluster 7 locked while it plays two cutscenes on Command, so those lock nothing.
inline constexpr const char* Unplaced = "nil";
inline constexpr const char* CentreOfCluster = "centreOn()";
inline constexpr const char* CentreOfQuarantine = "lockAndCentreOn(QuarantineCluster, 0)";
inline constexpr const char* CentreOfDevelop = "lockAndCentreOn(TppDefine.CLUSTER_DEFINE.Develop)";
inline constexpr const char* CentreOfCommand = "lockAndCentreOn(TppDefine.CLUSTER_DEFINE.Command)";
inline constexpr const char* CentreOfCommandUnlocked = "centreOn(TppDefine.CLUSTER_DEFINE.Command, 0)";
inline constexpr const char* BesideTargetTruck = R"(function(id)
  local position = s10090_enemy.GetVehiclePosition(s10090_enemy.VEHICLE_NAME.TRUCK_TARGET)
  local angle = math.atan2(position:GetX() - vars.playerPosX, position:GetZ() - vars.playerPosZ)
  DemoDaemon.SetDemoTransform(id, Quat.RotationY(angle + TppMath.DegreeToRadian(45) - foxmath.PI), position)
end)";

struct Mission
{
    int code;
    const char* title;
};

inline constexpr Mission Awakening{10010, "Awakening"};
inline constexpr Mission PhantomLimbs{10020, "Phantom Limbs"};
inline constexpr Mission DiamondDogs{10030, "Diamond Dogs"};
inline constexpr Mission WhereDoTheBeesSleep{10040, "Where Do the Bees Sleep?"};
inline constexpr Mission OccupationForces{10044, "Occupation Forces"};
inline constexpr Mission CloakedInSilence{10050, "Cloaked in Silence"};
inline constexpr Mission Hellbound{10070, "Hellbound"};
inline constexpr Mission PitchDark{10080, "Pitch Dark"};
inline constexpr Mission RootCause{10081, "Root Cause"};
inline constexpr Mission TraitorsCaravan{10090, "Traitors' Caravan"};
inline constexpr Mission BloodRunsDeep{10100, "Blood Runs Deep"};
inline constexpr Mission Voices{10110, "Voices"};
inline constexpr Mission TheWhiteMamba{10120, "The White Mamba"};
inline constexpr Mission CodeTalker{10130, "Code Talker"};
inline constexpr Mission MetallicArchaea{10140, "Metallic Archaea"};
inline constexpr Mission SkullFace{10150, "Skull Face"};
inline constexpr Mission Sahelanthropus{10151, "Sahelanthropus"};
inline constexpr Mission ShiningLightsEvenInDeath{10240, "Shining Lights, Even in Death"};
inline constexpr Mission AQuietExit{10260, "A Quiet Exit"};
inline constexpr Mission AfghanistanFreeRoam{30010, "Afghanistan free roam (will not load)"};
inline constexpr Mission MotherBaseWard{30250, "Mother Base ward"};
inline constexpr Mission MotherBase{30050, "Mother Base"};

struct Cutscene
{
    Mission mission;
    const char* name; // In the mission's demoList
    const char* demoId;
    const char* stage = SameStage;
    const char* place = Unplaced;
};

inline constexpr Cutscene List[]{
    {Awakening, "beforeOpening", "p21_000100"},
    {Awakening, "opening", "p21_010000"},
    {Awakening, "openingFlag", "p21_010000_tgt_flag"},
    {Awakening, "flashback", "p21_010020"},
    {Awakening, "firstDay", "p21_010040"},
    {Awakening, "fewDaysLater0", "p21_010050"},
    {Awakening, "fewDaysLater1", "p21_010050_001"},
    {Awakening, "fewDaysLater1_NG0", "p21_010050_cmn_react000"},
    {Awakening, "fewDaysLater1_NG1", "p21_010050_cmn_react001"},
    {Awakening, "fewDaysLater1_NG2", "p21_010050_cmn_react002"},
    {Awakening, "fewDaysLater2", "p21_010050_002"},
    {Awakening, "fewDaysLater2_Truth", "p21_010055_002"},
    {Awakening, "fewDaysLater2_NG0", "p21_010050_001_react000"},
    {Awakening, "fewDaysLater3", "p21_010050_003"},
    {Awakening, "fewDaysLater3_Truth", "p21_010055_003"},
    {Awakening, "oneWeekLater", "p21_010100"},
    {Awakening, "twoWeekLater", "p21_010200"},
    {Awakening, "twoWeekLater_Truth", "p21_010205"},
    {Awakening, "twoWeekLaterReflect", "p21_010200_reflect"},
    {Awakening, "souvenirPhotograph", "p81_000001"},
    {Awakening, "quietAppear", "p21_010230"},
    {Awakening, "quietAppearTruth", "p21_010235"},
    {Awakening, "ishmaelAppear", "p21_010240"},
    {Awakening, "quietExit", "p21_010250"},
    {Awakening, "heli", "p21_010260"},
    {Awakening, "volgin", "p21_010270"},
    {Awakening, "cure", "p21_010310"},
    {Awakening, "underBed", "p21_010340"},
    {Awakening, "underBed2", "p21_010340_02"},
    {Awakening, "heliKillMob", "p21_010360"},
    {Awakening, "soldierKillMob", "p21_010370"},
    {Awakening, "stairway", "p21_010380"},
    {Awakening, "corridor0", "p21_010410"},
    {Awakening, "corridor1", "p21_010410_001"},
    {Awakening, "corridor2", "p21_010410_002"},
    {Awakening, "corridor3", "p21_010410_003"},
    {Awakening, "corridor4", "p21_010410_004"},
    {Awakening, "corridor5", "p21_010410_005"},
    {Awakening, "corridor6", "p21_010410_006"},
    {Awakening, "corridor7", "p21_010420_001"},
    {Awakening, "curtainRoom", "p21_010420"},
    {Awakening, "curtainRoom2", "p21_010420_02"},
    {Awakening, "volginVsSkullSoldier", "p21_010440"},
    {Awakening, "getGun", "p21_010490"},
    {Awakening, "beforeEntrance", "p21_010500_001"},
    {Awakening, "entrance", "p21_010500_000_final"},
    {Awakening, "ambulance", "p21_010510"},
    {Awakening, "heliRotor", "p21_010520"},
    {Awakening, "tank", "p21_010530"},
    {Awakening, "volginVsTank", "p21_010540"},
    {Awakening, "escapeFromEntrance", "p21_010550"},
    {Awakening, "escapeFromHospital", "p21_020010"},
    {Awakening, "fireWhale", "p21_020030_000"},
    {Awakening, "passportPhotograph", "p81_000010"},
    {Awakening, "fireWhaleTruth", "p61_000030"},
    {Awakening, "bridge", "p21_020040"},
    {Awakening, "bridge2", "p21_020040_001"},
    {Awakening, "twoBigBoss", "p61_000040"},

    {PhantomLimbs, "Demo_ArrivalInAfghanistan", "p31_010010"},
    {PhantomLimbs, "Demo_RescueMillerExplanation", "p31_010020"},
    {PhantomLimbs, "Demo_GetIntel", "p31_010025"},
    {PhantomLimbs, "Demo_RescueMiller", "p31_010030"},
    {PhantomLimbs, "Demo_TimeOverDeadMiller", "p31_010035"},
    {PhantomLimbs, "Demo_ParasiteAppearance", "p31_010040"},
    {PhantomLimbs, "Demo_ParasiteDiscover_upper", "p31_010045"},
    {PhantomLimbs, "Demo_EscapeWithMillerOnHeli", "p31_010050"},

    {DiamondDogs, "AfterPazResque", "p31_010060"},
    {DiamondDogs, "ArriveAtDD", "p31_010070"},
    {DiamondDogs, "OcelotGiveFulton", "p51_010500"},

    {WhereDoTheBeesSleep, "Demo_AttackHostage1", "p31_020020_000"},
    {WhereDoTheBeesSleep, "Demo_FindHoneyBee1", "p31_020025_002"},
    {WhereDoTheBeesSleep, "Demo_FindHoneyBee2", "p31_020025_003"},
    {WhereDoTheBeesSleep, "Demo_RescueHostage", "p31_020010"},
    {WhereDoTheBeesSleep, "Demo_FondHoneyBee02", "p31_020050"},
    {WhereDoTheBeesSleep, "Demo_EnemyHeli", "p31_020027"},
    {WhereDoTheBeesSleep, "Demo_Clear", "p31_020040"},
    {WhereDoTheBeesSleep, "Demo_FogStart", "p31_020029"},
    {WhereDoTheBeesSleep, "Demo_SkullFace", "p31_020030_000_final"},
    {WhereDoTheBeesSleep, "Demo_SkullFace2", "p31_020035_000_final"},

    {OccupationForces, "Demo_GetIntel", "p31_010025_001"},

    {CloakedInSilence, "Demo_ConnectKillQuietGame", "p31_030110_000_final"},
    {CloakedInSilence, "Demo_NotKillQuiet", "p31_030130_final_000"},
    {CloakedInSilence, "Demo_QuietDied", "p31_030140"},
    {CloakedInSilence, "Demo_RideHeliWithQuiet", "p31_030200_000_final"},

    {Hellbound, "Demo_SahelanTest", "p31_040010_000_final"},
    {Hellbound, "Demo_ContactHuey", "p31_040110", HellboundSovietBase},
    {Hellbound, "Demo_RideMetal", "p31_040130_000_final", HellboundSovietBase},
    {Hellbound, "Demo_SahelanHerald01", "p31_040135_000", HellboundSovietBase},
    {Hellbound, "Demo_SahelanHerald02", "p31_040135_001", HellboundSovietBase},
    {Hellbound, "Demo_SahelanAttacks", "p31_040140", HellboundSahelan},
    {Hellbound, "Demo_SahelanFalling", "p31_040160_000_final", HellboundSahelan},
    {Hellbound, "Demo_MotherBase", "p31_040200_000_final", HellboundMotherBase},

    {PitchDark, "Demo_InAfrica", "p41_030005_000_final"},
    {PitchDark, "Demo_ArrivalInAfrica", "p41_030010_000_final"},
    {PitchDark, "Demo_TeachChild", "p41_030030_000_final"},
    {PitchDark, "Demo_DeadBody", "p41_030040_000_final"},
    {PitchDark, "Demo_Reinforcement", "p41_030050_000_final", PitchDarkAfterPump},

    {RootCause, "Demo_PrisonBreak_door", "p42_010000"},
    {RootCause, "Demo_PrisonBreak_window", "p42_010010"},

    {TraitorsCaravan, "Demo_ParasiteAppearance", "p41_050010", SameStage, BesideTargetTruck},
    {TraitorsCaravan, "Demo_TargetEscape", "p41_050020_000_final"},

    {BloodRunsDeep, "Demo_Brank_30", "p81_000020_30f"},
    {BloodRunsDeep, "Demo_MeetBoySoldier", "p41_020020_000"},

    {Voices, "bedDemoName", "p41_040130_000_final"},
    {Voices, "volginDemoNameBefore", "p41_040145"},
    {Voices, "volginDemoName", "p41_040150_000_final", VoicesVolgin},

    {TheWhiteMamba, "Demo_Liquid_combatcamera", "p41_010005"},
    {TheWhiteMamba, "Demo_Liquid_combat", "p41_010010_000_final"},
    {TheWhiteMamba, "Demo_Liquid_combatEnd", "p41_010030_000_final"},
    {TheWhiteMamba, "Demo_Liquid_HeliEscape", "p41_010040_000_final"},

    {CodeTalker, "ContactCodetalkerDemo", "p41_060010"},
    {CodeTalker, "CemeteryCodetalkerDemo", "p41_060020"},
    {CodeTalker, "ClearDemo", "p41_060110", CodeTalkerClear},
    {CodeTalker, "ArrivalParasiteDemo", "p41_060005"},

    {MetallicArchaea, "Demo_Opening", "p41_060210"},
    {MetallicArchaea, "Demo_CodeTalker_Evacuate", "p41_060220"},
    {MetallicArchaea, "Demo_Inside_Heli", "p41_060250_000_final", MetallicArchaeaHeli},
    {MetallicArchaea, "Demo_TorturedHueyAtMB", "p41_060260_000_final", MetallicArchaeaMotherBase, CentreOfCommandUnlocked},
    {MetallicArchaea, "Demo_GoToLastMission", "p41_060270_000_final", MetallicArchaeaMotherBase, CentreOfCommandUnlocked},
    {MetallicArchaea, "Demo_PandemicConverge", "p41_060255", MetallicArchaeaMotherBase, CentreOfQuarantine},

    {SkullFace, "Demo_OpOKBZero", "p31_050005"},
    {SkullFace, "Demo_SkullFaceAppearance", "p31_050010_000_final", SkullFaceArrival},
    {SkullFace, "Demo_SahelanAppearance", "p31_050025_000_final", SkullFaceSahelan},

    {Sahelanthropus, "Demo_SahelanAppearance", "p31_050026_000_final"},
    {Sahelanthropus, "Demo_BreakSahelan", "p31_050040_000_final"},
    {Sahelanthropus, "Demo_DeadSkullFace", "p31_050050_000_final"},
    {Sahelanthropus, "Demo_MillerStandIdle", "p31_050050_002"},
    {Sahelanthropus, "Demo_AfterDeadSkullFace", "p31_050050_001_final"},
    {Sahelanthropus, "Demo_Ending1", "p31_050060_000_final", SahelanthropusEnding, CentreOfDevelop},
    {Sahelanthropus, "Demo_Ending2", "p31_050070_000_final", SahelanthropusEnding, CentreOfDevelop},
    {Sahelanthropus, "Demo_Ending3", "p31_050080", SahelanthropusEnding, CentreOfCommand},
    {Sahelanthropus, "Demo_SkullShot1", "p31_050050_003"},
    {Sahelanthropus, "Demo_SkullShot2", "p31_050050_004"},
    {Sahelanthropus, "Demo_SkullShot3", "p31_050050_005"},

    {ShiningLightsEvenInDeath, "Demo_Opening", "p51_070020_000_final"},
    {ShiningLightsEvenInDeath, "Demo_GoToInside", "p51_070110_000_final", SameStage, CentreOfCluster},
    {ShiningLightsEvenInDeath, "Demo_Roof_Event", "p51_070220_000_final", ShiningLightsInside},
    {ShiningLightsEvenInDeath, "Demo_Exit", "p51_070300_000_final", ShiningLightsInside},
    {ShiningLightsEvenInDeath, "Demo_Funeral", "p51_070500_000_final", ShiningLightsFuneral, CentreOfCluster},
    {ShiningLightsEvenInDeath, "Demo_EnterInMBQF", "p51_070120_000_final", ShiningLightsInside},
    {ShiningLightsEvenInDeath, "Demo_ExitWithLiveMan", "p51_070280_000_final", ShiningLightsInside},

    {AQuietExit, "Demo_EncountQuiet", "p31_070005"},
    {AQuietExit, "Demo_QuietChoked", "p31_070010"},
    {AQuietExit, "Demo_AfterTankBattle", "p31_070015_000_final"},
    {AQuietExit, "Demo_SnakeBite_01", "p31_070020_000_final"},
    {AQuietExit, "Demo_SnakeBite_02", "p31_070020_001_final"},
    {AQuietExit, "Demo_QuietSpeak", "p31_070030_000_final"},
    {AQuietExit, "Demo_QuietVanishing", "p31_070040_000_final"},
    {AQuietExit, "Demo_CameraForLetter", "p31_070050_002"},
    {AQuietExit, "Demo_LetterFromQuiet", "p31_070050_000_final"},
    {AQuietExit, "Demo_QuietDrifted", "p31_070050_001_final"},

    {AfghanistanFreeRoam, "Demo_RecoverReptilePod", "p31_060010"},
    {AfghanistanFreeRoam, "Demo_AttackedByQuiet_1", "p31_030100_000"},
    {AfghanistanFreeRoam, "Demo_AttackedByQuiet_2", "p31_030100_001"},
    {AfghanistanFreeRoam, "Demo_RescueSpecial", "p31_080010_000_final"},
    {AfghanistanFreeRoam, "Demo_RecoverVolgin", "p31_080100_000_final"},

    {MotherBaseWard, "Demo_RecoverVolgin", "p31_080110_000_final", SameStage, CentreOfQuarantine},

    {MotherBase, "AnableDevBattleGear", "p51_010140_000_final"},
    {MotherBase, "ArrivedMotherBaseAfterQuietBattle", "p31_030220_000_final"},
    {MotherBase, "ArrivedMotherBaseChildren", "p41_020050_000_final"},
    {MotherBase, "ArrivedMotherBaseFromDeathFactory", "p41_040200_000_final"},
    {MotherBase, "ArrivedMotherBaseLiquid", "p41_010050_000_final"},
    {MotherBase, "AttackedFromOtherPlayer_KnowWhereFrom", "p51_010150_000_final"},
    {MotherBase, "AttackedFromOtherPlayer_UnknowWhereFrom", "p51_010150_001_final"},
    {MotherBase, "CodeTalkerSunBath", "p51_010170_000_final"},
    {MotherBase, "DdogComeToGet", "p51_010020_000_final"},
    {MotherBase, "DecisionHuey", "p31_060030_000_final"},
    {MotherBase, "DetailsNuclearDevelop", "p51_020030_01movie"},
    {MotherBase, "EliLookSnake", "p51_010220_000_final"},
    {MotherBase, "EndingSacrificeOfNuclear", "p51_020030_02movie"},
    {MotherBase, "EntrustDdog", "p51_010010"},
    {MotherBase, "ForKeepNuclearElimination", "p51_020020_000_final"},
    {MotherBase, "GoToMotherBaseAfterQuietBattle", "p31_030210"},
    {MotherBase, "HappyBirthDay", "p51_010270_000"},
    {MotherBase, "HappyBirthDayWithQuiet", "p51_010270_001"},
    {MotherBase, "InterrogateQuiet", "p51_010210"},
    {MotherBase, "LiquidAndChildSoldier", "p51_010340_000_final"},
    {MotherBase, "LongTimeNoSee_DDSoldier", "p51_010060_000_final"},
    {MotherBase, "LongTimeNoSee_DdogHighLikability", "p51_010057_000_final"},
    {MotherBase, "LongTimeNoSee_DdogLowLikability", "p51_010053_000_final"},
    {MotherBase, "LongTimeNoSee_DdogPup", "p51_010050_000_final"},
    {MotherBase, "LongTimeNoSee_DdogSuperHighLikability", "p51_010059_000_final"},
    {MotherBase, "MoraleOfMBIsLow", "p51_010040_000_final"},
    {MotherBase, "NuclearEliminationCeremony", "p51_020010_000_final"},
    {MotherBase, "OcelotIsPupilOfSnake", "p51_010350_000_final"},
    {MotherBase, "ParasiticWormCarrierKill", "p51_070010_000_final"},
    {MotherBase, "PazPhantomPain1", "p51_080010_000_final"},
    {MotherBase, "PazPhantomPain2", "p51_080020"},
    {MotherBase, "PazPhantomPain4", "p51_080040"},
    {MotherBase, "PazPhantomPain4_jp", "p51_080041"},
    {MotherBase, "QuietHasFriendshipWithChild", "p51_010310_000"},
    {MotherBase, "QuietOnHeliInRain", "p51_010300_000_final"},
    {MotherBase, "QuietReceivesPersecution", "p51_010360_000"},
    {MotherBase, "SacrificeOfNuclearElimination", "p51_020040_000_final"},
    {MotherBase, "SnakeHasBadSmell_000", "p51_010165_000_final"},
    {MotherBase, "SnakeHasBadSmell_001", "p51_010165_001_final"},
    {MotherBase, "SnakeHasBadSmell_WithoutQuiet", "p51_010160_000_final"},
    {MotherBase, "TheGreatEscapeLiquid", "p61_010030_000_final"},
};
}
