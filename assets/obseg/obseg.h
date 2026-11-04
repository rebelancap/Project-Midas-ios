#include <ultra64.h>

/* The asset file symbols are weak so a GE_ROMDATA_FALLBACK=OFF build can drop
 * the served asset objects from the link; their file_resource_table entries
 * then resolve to NULL and romdataFilesApply repoints each file at the ROM.
 * With the objects present (default build) each weak declaration binds to its
 * strong definition. */

extern u8 __attribute__((weak)) bg_sev_all_p_seg[];
extern u8 __attribute__((weak)) bg_silo_all_p_seg[];
extern u8 __attribute__((weak)) bg_stat_all_p_seg[];
extern u8 __attribute__((weak)) bg_arec_all_p_seg[];
extern u8 __attribute__((weak)) bg_arch_all_p_seg[];
extern u8 __attribute__((weak)) bg_tra_all_p_seg[];
extern u8 __attribute__((weak)) bg_dest_all_p_seg[];
extern u8 __attribute__((weak)) bg_sevb_all_p_seg[];
extern u8 __attribute__((weak)) bg_azt_all_p_seg[];
extern u8 __attribute__((weak)) bg_pete_all_p_seg[];
extern u8 __attribute__((weak)) bg_depo_all_p_seg[];
extern u8 __attribute__((weak)) bg_ref_all_p_seg[];
extern u8 __attribute__((weak)) bg_cryp_all_p_seg[];
extern u8 __attribute__((weak)) bg_dam_all_p_seg[];
extern u8 __attribute__((weak)) bg_ark_all_p_seg[];
extern u8 __attribute__((weak)) bg_run_all_p_seg[];
extern u8 __attribute__((weak)) bg_sevx_all_p_seg[];
extern u8 __attribute__((weak)) bg_jun_all_p_seg[];
extern u8 __attribute__((weak)) bg_dish_all_p_seg[];
extern u8 __attribute__((weak)) bg_cave_all_p_seg[];
extern u8 __attribute__((weak)) bg_cat_all_p_seg[];
extern u8 __attribute__((weak)) bg_crad_all_p_seg[];
extern u8 __attribute__((weak)) bg_sho_all_p_seg[];
extern u8 __attribute__((weak)) bg_eld_all_p_seg[];
extern u8 __attribute__((weak)) bg_imp_all_p_seg[];
extern u8 __attribute__((weak)) bg_ash_all_p_seg[];
extern u8 __attribute__((weak)) bg_lue_all_p_seg[];
extern u8 __attribute__((weak)) bg_ame_all_p_seg[];
extern u8 __attribute__((weak)) bg_rit_all_p_seg[];
extern u8 __attribute__((weak)) bg_oat_all_p_seg[];
extern u8 __attribute__((weak)) bg_ear_all_p_seg[];
extern u8 __attribute__((weak)) bg_lee_all_p_seg[];
extern u8 __attribute__((weak)) bg_lip_all_p_seg[];
extern u8 __attribute__((weak)) bg_len_all_p_seg[];
extern u8 __attribute__((weak)) bg_wax_all_p_seg[];
extern u8 __attribute__((weak)) bg_pam_all_p_seg[];

extern u8 __attribute__((weak)) CarmourguardZ[];
extern u8 __attribute__((weak)) CbaronsamediZ[];
extern u8 __attribute__((weak)) CbluecamguardZ[];
extern u8 __attribute__((weak)) CbluemanZ[];
extern u8 __attribute__((weak)) CbluewomanZ[];
extern u8 __attribute__((weak)) CboilerbondZ[];
extern u8 __attribute__((weak)) CboilertrevZ[];
extern u8 __attribute__((weak)) CborisZ[];
extern u8 __attribute__((weak)) CcamguardZ[];
extern u8 __attribute__((weak)) CcardimanZ[];
extern u8 __attribute__((weak)) CcheckmanZ[];
extern u8 __attribute__((weak)) CcommguardZ[];
extern u8 __attribute__((weak)) CdjbondZ[];
extern u8 __attribute__((weak)) CfattechwomanZ[];
extern u8 __attribute__((weak)) Cgreatguard2Z[];
extern u8 __attribute__((weak)) CgreatguardZ[];
extern u8 __attribute__((weak)) CgreyguardZ[];
extern u8 __attribute__((weak)) CgreymanZ[];
extern u8 __attribute__((weak)) CheadalanZ[];
extern u8 __attribute__((weak)) CheadbZ[];
extern u8 __attribute__((weak)) CheadbalaclavaZ[];
extern u8 __attribute__((weak)) CheadbikeZ[];
extern u8 __attribute__((weak)) CheadbrosnanZ[];
extern u8 __attribute__((weak)) CheadbrosnanboilerZ[];
extern u8 __attribute__((weak)) CheadbrosnansnowZ[];
extern u8 __attribute__((weak)) CheadbrosnansuitZ[];
extern u8 __attribute__((weak)) CheadbrosnantimberZ[];
extern u8 __attribute__((weak)) CheadchrisZ[];
extern u8 __attribute__((weak)) CheaddaveZ[];
extern u8 __attribute__((weak)) CheaddesZ[];
extern u8 __attribute__((weak)) CheadduncanZ[];
extern u8 __attribute__((weak)) CheaddwayneZ[];
extern u8 __attribute__((weak)) CheadgrahamZ[];
extern u8 __attribute__((weak)) CheadgrantZ[];
extern u8 __attribute__((weak)) CheadjimZ[];
extern u8 __attribute__((weak)) Cheadjoe2Z[];
extern u8 __attribute__((weak)) CheadjoeZ[];
extern u8 __attribute__((weak)) CheadjoelZ[];
extern u8 __attribute__((weak)) CheadkarlZ[];
extern u8 __attribute__((weak)) CheadkenZ[];
extern u8 __attribute__((weak)) CheadleeZ[];
extern u8 __attribute__((weak)) CheadmandyZ[];
extern u8 __attribute__((weak)) CheadmarionZ[];
extern u8 __attribute__((weak)) CheadmarkZ[];
extern u8 __attribute__((weak)) CheadmartinZ[];
extern u8 __attribute__((weak)) CheadmishkinZ[];
extern u8 __attribute__((weak)) CheadneilZ[];
extern u8 __attribute__((weak)) CheadpeteZ[];
extern u8 __attribute__((weak)) CheadrobinZ[];
extern u8 __attribute__((weak)) CheadsallyZ[];
extern u8 __attribute__((weak)) CheadscottZ[];
extern u8 __attribute__((weak)) CheadshaunZ[];
extern u8 __attribute__((weak)) CheadsteveeZ[];
extern u8 __attribute__((weak)) CheadstevehZ[];
extern u8 __attribute__((weak)) CheadvivienZ[];
extern u8 __attribute__((weak)) CjawsZ[];
extern u8 __attribute__((weak)) CjeanwomanZ[];
extern u8 __attribute__((weak)) CmaydayZ[];
extern u8 __attribute__((weak)) CmoonfemaleZ[];
extern u8 __attribute__((weak)) CmoonguardZ[];
extern u8 __attribute__((weak)) CnatalyaZ[];
extern u8 __attribute__((weak)) CnavyguardZ[];
extern u8 __attribute__((weak)) CoddjobZ[];
extern u8 __attribute__((weak)) ColiveguardZ[];
extern u8 __attribute__((weak)) CorumovZ[];
extern u8 __attribute__((weak)) CpilotZ[];
extern u8 __attribute__((weak)) CredmanZ[];
extern u8 __attribute__((weak)) CrusguardZ[];
extern u8 __attribute__((weak)) CsnowbondZ[];
extern u8 __attribute__((weak)) CsnowguardZ[];
extern u8 __attribute__((weak)) CspicebondZ[];
extern u8 __attribute__((weak)) Csuit_lf_handZ[];
extern u8 __attribute__((weak)) CsuitbondZ[];
extern u8 __attribute__((weak)) CtechmanZ[];
extern u8 __attribute__((weak)) CtechwomanZ[];
extern u8 __attribute__((weak)) CtimberbondZ[];
extern u8 __attribute__((weak)) CtrevelyanZ[];
extern u8 __attribute__((weak)) CtrevguardZ[];
extern u8 __attribute__((weak)) CvalentinZ[];
extern u8 __attribute__((weak)) CxeniaZ[];

extern u8 __attribute__((weak)) Gak47Z[];
extern u8 __attribute__((weak)) GaudiotapeZ[];
extern u8 __attribute__((weak)) GautoshotZ[];
extern u8 __attribute__((weak)) GblackboxZ[];
extern u8 __attribute__((weak)) GblueprintsZ[];
extern u8 __attribute__((weak)) GbombcaseZ[];
extern u8 __attribute__((weak)) GbombdefuserZ[];
extern u8 __attribute__((weak)) GbriefcaseZ[];
extern u8 __attribute__((weak)) GbugZ[];
extern u8 __attribute__((weak)) GbugdetectorZ[];
extern u8 __attribute__((weak)) GbungeeZ[];
extern u8 __attribute__((weak)) GcameraZ[];
extern u8 __attribute__((weak)) GcartblueZ[];
extern u8 __attribute__((weak)) GcartridgeZ[];
extern u8 __attribute__((weak)) GcartrifleZ[];
extern u8 __attribute__((weak)) GcartshellZ[];
extern u8 __attribute__((weak)) GcircuitboardZ[];
extern u8 __attribute__((weak)) GclipboardZ[];
extern u8 __attribute__((weak)) GcreditcardZ[];
extern u8 __attribute__((weak)) GdarkglassesZ[];
extern u8 __attribute__((weak)) GdatathiefZ[];
extern u8 __attribute__((weak)) GdattapeZ[];
extern u8 __attribute__((weak)) GdoordecoderZ[];
extern u8 __attribute__((weak)) GdoorexploderZ[];
extern u8 __attribute__((weak)) GdossierredZ[];
extern u8 __attribute__((weak)) GdynamiteZ[];
extern u8 __attribute__((weak)) GexplosivefloppyZ[];
extern u8 __attribute__((weak)) GexplosivepenZ[];
extern u8 __attribute__((weak)) GextinguisherZ[];
extern u8 __attribute__((weak)) GfingergunZ[];
extern u8 __attribute__((weak)) GfistZ[];
extern u8 __attribute__((weak)) GflarepistolZ[];
extern u8 __attribute__((weak)) Gfnp90Z[];
extern u8 __attribute__((weak)) GgaskeyringZ[];
extern u8 __attribute__((weak)) GgoldbarZ[];
extern u8 __attribute__((weak)) GgoldeneyekeyZ[];
extern u8 __attribute__((weak)) GgoldengunZ[];
extern u8 __attribute__((weak)) GgoldwppkZ[];
extern u8 __attribute__((weak)) GgrenadeZ[];
extern u8 __attribute__((weak)) GgrenadelaunchZ[];
extern u8 __attribute__((weak)) GheroinZ[];
extern u8 __attribute__((weak)) GjoypadZ[];
extern u8 __attribute__((weak)) GkeyanalysercaseZ[];
extern u8 __attribute__((weak)) GkeyboltZ[];
extern u8 __attribute__((weak)) GkeycardZ[];
extern u8 __attribute__((weak)) GkeyyaleZ[];
extern u8 __attribute__((weak)) GknifeZ[];
extern u8 __attribute__((weak)) GlaserZ[];
extern u8 __attribute__((weak)) GlectreZ[];
extern u8 __attribute__((weak)) GlockexploderZ[];
extern u8 __attribute__((weak)) Gm16Z[];
extern u8 __attribute__((weak)) GmapZ[];
extern u8 __attribute__((weak)) GmicrocameraZ[];
extern u8 __attribute__((weak)) GmicrocodeZ[];
extern u8 __attribute__((weak)) GmicrofilmZ[];
extern u8 __attribute__((weak)) GmoneyZ[];
extern u8 __attribute__((weak)) Gmp5kZ[];
extern u8 __attribute__((weak)) Gmp5ksilZ[];
extern u8 __attribute__((weak)) GpitongunZ[];
extern u8 __attribute__((weak)) GplansZ[];
extern u8 __attribute__((weak)) GplastiqueZ[];
extern u8 __attribute__((weak)) GpolarizedglassesZ[];
extern u8 __attribute__((weak)) GproximitymineZ[];
extern u8 __attribute__((weak)) GremotemineZ[];
extern u8 __attribute__((weak)) GrocketlaunchZ[];
extern u8 __attribute__((weak)) GrugerZ[];
extern u8 __attribute__((weak)) GsafecrackercaseZ[];
extern u8 __attribute__((weak)) GshotgunZ[];
extern u8 __attribute__((weak)) GsilverwppkZ[];
extern u8 __attribute__((weak)) GskorpionZ[];
extern u8 __attribute__((weak)) GsniperrifleZ[];
extern u8 __attribute__((weak)) GspectreZ[];
extern u8 __attribute__((weak)) GspooltapeZ[];
extern u8 __attribute__((weak)) GspyfileZ[];
extern u8 __attribute__((weak)) GstafflistZ[];
extern u8 __attribute__((weak)) GtaserZ[];
extern u8 __attribute__((weak)) GthrowknifeZ[];
extern u8 __attribute__((weak)) GtimedmineZ[];
extern u8 __attribute__((weak)) GtriggerZ[];
extern u8 __attribute__((weak)) Gtt33Z[];
extern u8 __attribute__((weak)) GuziZ[];
extern u8 __attribute__((weak)) GvideotapeZ[];
extern u8 __attribute__((weak)) GwatchcommunicatorZ[];
extern u8 __attribute__((weak)) GwatchgeigercounterZ[];
extern u8 __attribute__((weak)) GwatchidentifierZ[];
extern u8 __attribute__((weak)) GwatchlaserZ[];
extern u8 __attribute__((weak)) GwatchmagnetattractZ[];
extern u8 __attribute__((weak)) GwatchmagnetrepelZ[];
extern u8 __attribute__((weak)) GweaponcaseZ[];
extern u8 __attribute__((weak)) GwppkZ[];
extern u8 __attribute__((weak)) GwppksilZ[];
extern u8 __attribute__((weak)) GwristdartZ[];

extern u8 __attribute__((weak)) PICBMZ[];
extern u8 __attribute__((weak)) PICBM_noseZ[];
extern u8 __attribute__((weak)) Pak47magZ[];
extern u8 __attribute__((weak)) Palarm1Z[];
extern u8 __attribute__((weak)) Palarm2Z[];
extern u8 __attribute__((weak)) Pammo_crate1Z[];
extern u8 __attribute__((weak)) Pammo_crate2Z[];
extern u8 __attribute__((weak)) Pammo_crate3Z[];
extern u8 __attribute__((weak)) Pammo_crate4Z[];
extern u8 __attribute__((weak)) Pammo_crate5Z[];
extern u8 __attribute__((weak)) PapcZ[];
extern u8 __attribute__((weak)) Parchsecdoor1Z[];
extern u8 __attribute__((weak)) Parchsecdoor2Z[];
extern u8 __attribute__((weak)) ParticZ[];
extern u8 __attribute__((weak)) PartictrailerZ[];
extern u8 __attribute__((weak)) PbarricadeZ[];
extern u8 __attribute__((weak)) Pbin1Z[];
extern u8 __attribute__((weak)) Pblotter1Z[];
extern u8 __attribute__((weak)) PbodyarmourZ[];
extern u8 __attribute__((weak)) PbodyarmourvestZ[];
extern u8 __attribute__((weak)) PbollardZ[];
extern u8 __attribute__((weak)) PbombZ[];
extern u8 __attribute__((weak)) Pbook1Z[];
extern u8 __attribute__((weak)) Pbookshelf1Z[];
extern u8 __attribute__((weak)) Pborg_crateZ[];
extern u8 __attribute__((weak)) PboxcartridgesZ[];
extern u8 __attribute__((weak)) Pboxes2x4Z[];
extern u8 __attribute__((weak)) Pboxes3x4Z[];
extern u8 __attribute__((weak)) Pboxes4x4Z[];
extern u8 __attribute__((weak)) PbrakeunitZ[];
extern u8 __attribute__((weak)) Pbridge_console1aZ[];
extern u8 __attribute__((weak)) Pbridge_console1bZ[];
extern u8 __attribute__((weak)) Pbridge_console2aZ[];
extern u8 __attribute__((weak)) Pbridge_console2bZ[];
extern u8 __attribute__((weak)) Pbridge_console3aZ[];
extern u8 __attribute__((weak)) Pbridge_console3bZ[];
extern u8 __attribute__((weak)) PcarbmwZ[];
extern u8 __attribute__((weak)) Pcard_box1Z[];
extern u8 __attribute__((weak)) Pcard_box2Z[];
extern u8 __attribute__((weak)) Pcard_box3Z[];
extern u8 __attribute__((weak)) Pcard_box4_lgZ[];
extern u8 __attribute__((weak)) Pcard_box5_lgZ[];
extern u8 __attribute__((weak)) Pcard_box6_lgZ[];
extern u8 __attribute__((weak)) PcarescortZ[];
extern u8 __attribute__((weak)) PcargolfZ[];
extern u8 __attribute__((weak)) PcarweirdZ[];
extern u8 __attribute__((weak)) PcarzilZ[];
extern u8 __attribute__((weak)) PcctvZ[];
extern u8 __attribute__((weak)) PchraudiotapeZ[];
extern u8 __attribute__((weak)) PchrautoshotZ[];
extern u8 __attribute__((weak)) PchrblackboxZ[];
extern u8 __attribute__((weak)) PchrblueprintsZ[];
extern u8 __attribute__((weak)) PchrbombcaseZ[];
extern u8 __attribute__((weak)) PchrbombdefuserZ[];
extern u8 __attribute__((weak)) PchrbriefcaseZ[];
extern u8 __attribute__((weak)) PchrbugZ[];
extern u8 __attribute__((weak)) PchrbugdetectorZ[];
extern u8 __attribute__((weak)) PchrbungeeZ[];
extern u8 __attribute__((weak)) PchrcameraZ[];
extern u8 __attribute__((weak)) PchrcircuitboardZ[];
extern u8 __attribute__((weak)) PchrclipboardZ[];
extern u8 __attribute__((weak)) PchrcreditcardZ[];
extern u8 __attribute__((weak)) PchrdarkglassesZ[];
extern u8 __attribute__((weak)) PchrdatathiefZ[];
extern u8 __attribute__((weak)) PchrdattapeZ[];
extern u8 __attribute__((weak)) PchrdoordecoderZ[];
extern u8 __attribute__((weak)) PchrdoorexploderZ[];
extern u8 __attribute__((weak)) PchrdossierredZ[];
extern u8 __attribute__((weak)) PchrdynamiteZ[];
extern u8 __attribute__((weak)) PchrexplosivepenZ[];
extern u8 __attribute__((weak)) PchrextinguisherZ[];
extern u8 __attribute__((weak)) PchrfingergunZ[];
extern u8 __attribute__((weak)) PchrflarepistolZ[];
extern u8 __attribute__((weak)) Pchrfnp90Z[];
extern u8 __attribute__((weak)) PchrgaskeyringZ[];
extern u8 __attribute__((weak)) PchrgoldbarZ[];
extern u8 __attribute__((weak)) PchrgoldenZ[];
extern u8 __attribute__((weak)) PchrgoldeneyekeyZ[];
extern u8 __attribute__((weak)) PchrgoldwppkZ[];
extern u8 __attribute__((weak)) PchrgrenadeZ[];
extern u8 __attribute__((weak)) PchrgrenadelaunchZ[];
extern u8 __attribute__((weak)) PchrgrenaderoundZ[];
extern u8 __attribute__((weak)) PchrheroinZ[];
extern u8 __attribute__((weak)) PchrkalashZ[];
extern u8 __attribute__((weak)) PchrkeyanalysercaseZ[];
extern u8 __attribute__((weak)) PchrkeyboltZ[];
extern u8 __attribute__((weak)) PchrkeyyaleZ[];
extern u8 __attribute__((weak)) PchrknifeZ[];
extern u8 __attribute__((weak)) PchrlaserZ[];
extern u8 __attribute__((weak)) PchrlectreZ[];
extern u8 __attribute__((weak)) PchrlockexploderZ[];
extern u8 __attribute__((weak)) Pchrm16Z[];
extern u8 __attribute__((weak)) PchrmapZ[];
extern u8 __attribute__((weak)) PchrmicrocameraZ[];
extern u8 __attribute__((weak)) PchrmicrocodeZ[];
extern u8 __attribute__((weak)) PchrmicrofilmZ[];
extern u8 __attribute__((weak)) PchrmoneyZ[];
extern u8 __attribute__((weak)) Pchrmp5kZ[];
extern u8 __attribute__((weak)) Pchrmp5ksilZ[];
extern u8 __attribute__((weak)) PchrpitongunZ[];
extern u8 __attribute__((weak)) PchrplansZ[];
extern u8 __attribute__((weak)) PchrplastiqueZ[];
extern u8 __attribute__((weak)) PchrpolarizedglassesZ[];
extern u8 __attribute__((weak)) PchrproximitymineZ[];
extern u8 __attribute__((weak)) PchrremotemineZ[];
extern u8 __attribute__((weak)) PchrrocketZ[];
extern u8 __attribute__((weak)) PchrrocketlaunchZ[];
extern u8 __attribute__((weak)) PchrrugerZ[];
extern u8 __attribute__((weak)) PchrsafecrackercaseZ[];
extern u8 __attribute__((weak)) PchrshotgunZ[];
extern u8 __attribute__((weak)) PchrsilverwppkZ[];
extern u8 __attribute__((weak)) PchrskorpionZ[];
extern u8 __attribute__((weak)) PchrsniperrifleZ[];
extern u8 __attribute__((weak)) PchrspectreZ[];
extern u8 __attribute__((weak)) PchrspooltapeZ[];
extern u8 __attribute__((weak)) PchrspyfileZ[];
extern u8 __attribute__((weak)) PchrstafflistZ[];
extern u8 __attribute__((weak)) PchrtesttubeZ[];
extern u8 __attribute__((weak)) PchrthrowknifeZ[];
extern u8 __attribute__((weak)) PchrtimedmineZ[];
extern u8 __attribute__((weak)) Pchrtt33Z[];
extern u8 __attribute__((weak)) PchruziZ[];
extern u8 __attribute__((weak)) PchrvideotapeZ[];
extern u8 __attribute__((weak)) PchrweaponcaseZ[];
extern u8 __attribute__((weak)) PchrwppkZ[];
extern u8 __attribute__((weak)) PchrwppksilZ[];
extern u8 __attribute__((weak)) PchrwristdartZ[];
extern u8 __attribute__((weak)) Pconsole1Z[];
extern u8 __attribute__((weak)) Pconsole2Z[];
extern u8 __attribute__((weak)) Pconsole3Z[];
extern u8 __attribute__((weak)) Pconsole_sev2aZ[];
extern u8 __attribute__((weak)) Pconsole_sev2bZ[];
extern u8 __attribute__((weak)) Pconsole_sev2cZ[];
extern u8 __attribute__((weak)) Pconsole_sev2dZ[];
extern u8 __attribute__((weak)) Pconsole_sev_GEaZ[];
extern u8 __attribute__((weak)) Pconsole_sev_GEbZ[];
extern u8 __attribute__((weak)) Pconsole_sevaZ[];
extern u8 __attribute__((weak)) Pconsole_sevbZ[];
extern u8 __attribute__((weak)) Pconsole_sevcZ[];
extern u8 __attribute__((weak)) Pconsole_sevdZ[];
extern u8 __attribute__((weak)) Pcryptdoor1aZ[];
extern u8 __attribute__((weak)) Pcryptdoor1bZ[];
extern u8 __attribute__((weak)) Pcryptdoor2aZ[];
extern u8 __attribute__((weak)) Pcryptdoor2bZ[];
extern u8 __attribute__((weak)) Pcryptdoor3Z[];
extern u8 __attribute__((weak)) Pcryptdoor4Z[];
extern u8 __attribute__((weak)) PdamchaindoorZ[];
extern u8 __attribute__((weak)) PdamgatedoorZ[];
extern u8 __attribute__((weak)) PdamtundoorZ[];
extern u8 __attribute__((weak)) Pdepot_door_steelZ[];
extern u8 __attribute__((weak)) Pdepot_gate_entryZ[];
extern u8 __attribute__((weak)) Pdesk1Z[];
extern u8 __attribute__((weak)) Pdesk2Z[];
extern u8 __attribute__((weak)) Pdesk_arecibo1Z[];
extern u8 __attribute__((weak)) Pdesk_lamp2Z[];
extern u8 __attribute__((weak)) Pdest_engineZ[];
extern u8 __attribute__((weak)) Pdest_exocetZ[];
extern u8 __attribute__((weak)) Pdest_gunZ[];
extern u8 __attribute__((weak)) Pdest_harpoonZ[];
extern u8 __attribute__((weak)) Pdest_seawolfZ[];
extern u8 __attribute__((weak)) Pdisc_readerZ[];
extern u8 __attribute__((weak)) Pdisk_drive1Z[];
extern u8 __attribute__((weak)) Pdoor_azt_chairZ[];
extern u8 __attribute__((weak)) Pdoor_azt_deskZ[];
extern u8 __attribute__((weak)) Pdoor_azt_desk_topZ[];
extern u8 __attribute__((weak)) Pdoor_aztecZ[];
extern u8 __attribute__((weak)) Pdoor_dest1Z[];
extern u8 __attribute__((weak)) Pdoor_dest2Z[];
extern u8 __attribute__((weak)) Pdoor_eyelidZ[];
extern u8 __attribute__((weak)) Pdoor_irisZ[];
extern u8 __attribute__((weak)) Pdoor_mfZ[];
extern u8 __attribute__((weak)) Pdoor_roller1Z[];
extern u8 __attribute__((weak)) Pdoor_roller2Z[];
extern u8 __attribute__((weak)) Pdoor_roller3Z[];
extern u8 __attribute__((weak)) Pdoor_roller4Z[];
extern u8 __attribute__((weak)) Pdoor_rollertrainZ[];
extern u8 __attribute__((weak)) Pdoor_st_arec1Z[];
extern u8 __attribute__((weak)) Pdoor_st_arec2Z[];
extern u8 __attribute__((weak)) Pdoor_winZ[];
extern u8 __attribute__((weak)) PdoorconsoleZ[];
extern u8 __attribute__((weak)) PdoorpanelZ[];
extern u8 __attribute__((weak)) Pdoorprison1Z[];
extern u8 __attribute__((weak)) PdoorstatgateZ[];
extern u8 __attribute__((weak)) PexplosionbitZ[];
extern u8 __attribute__((weak)) Pfiling_cabinet1Z[];
extern u8 __attribute__((weak)) PflagZ[];
extern u8 __attribute__((weak)) PfloppyZ[];
extern u8 __attribute__((weak)) Pfnp90magZ[];
extern u8 __attribute__((weak)) Pgas_plant_met1_do1Z[];
extern u8 __attribute__((weak)) Pgas_plant_sw2_do1Z[];
extern u8 __attribute__((weak)) Pgas_plant_sw3_do1Z[];
extern u8 __attribute__((weak)) Pgas_plant_sw4_do1Z[];
extern u8 __attribute__((weak)) Pgas_plant_sw_do1Z[];
extern u8 __attribute__((weak)) Pgas_plant_wc_cub1Z[];
extern u8 __attribute__((weak)) PgasbarrelZ[];
extern u8 __attribute__((weak)) PgasbarrelsZ[];
extern u8 __attribute__((weak)) Pgasplant_clear_doorZ[];
extern u8 __attribute__((weak)) PgastankZ[];
extern u8 __attribute__((weak)) Pglassware1Z[];
extern u8 __attribute__((weak)) Pglassware2Z[];
extern u8 __attribute__((weak)) Pglassware3Z[];
extern u8 __attribute__((weak)) Pglassware4Z[];
extern u8 __attribute__((weak)) PgoldeneyelogoZ[];
extern u8 __attribute__((weak)) PgoldenshellsZ[];
extern u8 __attribute__((weak)) PgroundgunZ[];
extern u8 __attribute__((weak)) Pgun_runway1Z[];
extern u8 __attribute__((weak)) PhatberetZ[];
extern u8 __attribute__((weak)) PhatberetblueZ[];
extern u8 __attribute__((weak)) PhatberetredZ[];
extern u8 __attribute__((weak)) PhatchboltZ[];
extern u8 __attribute__((weak)) PhatchdoorZ[];
extern u8 __attribute__((weak)) PhatchsevxZ[];
extern u8 __attribute__((weak)) PhatfurryZ[];
extern u8 __attribute__((weak)) PhatfurryblackZ[];
extern u8 __attribute__((weak)) PhatfurrybrownZ[];
extern u8 __attribute__((weak)) PhathelmetZ[];
extern u8 __attribute__((weak)) PhathelmetgreyZ[];
extern u8 __attribute__((weak)) PhatmoonZ[];
extern u8 __attribute__((weak)) PhatpeakedZ[];
extern u8 __attribute__((weak)) PhattbirdZ[];
extern u8 __attribute__((weak)) PhattbirdbrownZ[];
extern u8 __attribute__((weak)) PhelicopterZ[];
extern u8 __attribute__((weak)) PhindZ[];
extern u8 __attribute__((weak)) PjeepZ[];
extern u8 __attribute__((weak)) Pjerry_can1Z[];
extern u8 __attribute__((weak)) Pjungle3_treeZ[];
extern u8 __attribute__((weak)) Pjungle5_treeZ[];
extern u8 __attribute__((weak)) Pkey_holderZ[];
extern u8 __attribute__((weak)) Pkeyboard1Z[];
extern u8 __attribute__((weak)) Pkit_units1Z[];
extern u8 __attribute__((weak)) PlabbenchZ[];
extern u8 __attribute__((weak)) PlandmineZ[];
extern u8 __attribute__((weak)) PlegalpageZ[];
extern u8 __attribute__((weak)) Pletter_tray1Z[];
extern u8 __attribute__((weak)) Plocker3Z[];
extern u8 __attribute__((weak)) Plocker4Z[];
extern u8 __attribute__((weak)) Pm16magZ[];
extern u8 __attribute__((weak)) PmagnumshellsZ[];
extern u8 __attribute__((weak)) Pmainframe1Z[];
extern u8 __attribute__((weak)) Pmainframe2Z[];
extern u8 __attribute__((weak)) Pmetal_chair1Z[];
extern u8 __attribute__((weak)) Pmetal_crate1Z[];
extern u8 __attribute__((weak)) Pmetal_crate2Z[];
extern u8 __attribute__((weak)) Pmetal_crate3Z[];
extern u8 __attribute__((weak)) Pmetal_crate4Z[];
extern u8 __attribute__((weak)) PmilcopterZ[];
extern u8 __attribute__((weak)) PmiltruckZ[];
extern u8 __attribute__((weak)) Pmissile_rack2Z[];
extern u8 __attribute__((weak)) Pmissile_rackZ[];
extern u8 __attribute__((weak)) PmodemboxZ[];
extern u8 __attribute__((weak)) PmotorbikeZ[];
extern u8 __attribute__((weak)) Pmp5kmagZ[];
extern u8 __attribute__((weak)) PnintendologoZ[];
extern u8 __attribute__((weak)) Poil_drum1Z[];
extern u8 __attribute__((weak)) Poil_drum2Z[];
extern u8 __attribute__((weak)) Poil_drum3Z[];
extern u8 __attribute__((weak)) Poil_drum5Z[];
extern u8 __attribute__((weak)) Poil_drum6Z[];
extern u8 __attribute__((weak)) Poil_drum7Z[];
extern u8 __attribute__((weak)) PpadlockZ[];
extern u8 __attribute__((weak)) PpalmZ[];
extern u8 __attribute__((weak)) PpalmtreeZ[];
extern u8 __attribute__((weak)) Pphone1Z[];
extern u8 __attribute__((weak)) PplaneZ[];
extern u8 __attribute__((weak)) Pplant11Z[];
extern u8 __attribute__((weak)) Pplant1Z[];
extern u8 __attribute__((weak)) Pplant2Z[];
extern u8 __attribute__((weak)) Pplant2bZ[];
extern u8 __attribute__((weak)) Pplant3Z[];
extern u8 __attribute__((weak)) Pradio_unit1Z[];
extern u8 __attribute__((weak)) Pradio_unit2Z[];
extern u8 __attribute__((weak)) Pradio_unit3Z[];
extern u8 __attribute__((weak)) Pradio_unit4Z[];
extern u8 __attribute__((weak)) ProofgunZ[];
extern u8 __attribute__((weak)) PsafeZ[];
extern u8 __attribute__((weak)) PsafedoorZ[];
extern u8 __attribute__((weak)) Psat1_reflectZ[];
extern u8 __attribute__((weak)) PsatboxZ[];
extern u8 __attribute__((weak)) PsatdishZ[];
extern u8 __attribute__((weak)) Psec_panelZ[];
extern u8 __attribute__((weak)) Psev_door3Z[];
extern u8 __attribute__((weak)) Psev_door3_windZ[];
extern u8 __attribute__((weak)) Psev_door4_windZ[];
extern u8 __attribute__((weak)) Psev_doorZ[];
extern u8 __attribute__((weak)) Psev_door_v1Z[];
extern u8 __attribute__((weak)) Psev_trislideZ[];
extern u8 __attribute__((weak)) PsevdishZ[];
extern u8 __attribute__((weak)) PsevdoormetslideZ[];
extern u8 __attribute__((weak)) PsevdoornowindZ[];
extern u8 __attribute__((weak)) PsevdoorwindZ[];
extern u8 __attribute__((weak)) PsevdoorwoodZ[];
extern u8 __attribute__((weak)) PshuttleZ[];
extern u8 __attribute__((weak)) Pshuttle_door_lZ[];
extern u8 __attribute__((weak)) Pshuttle_door_rZ[];
extern u8 __attribute__((weak)) PsilencerZ[];
extern u8 __attribute__((weak)) Psilo_lift_doorZ[];
extern u8 __attribute__((weak)) PsilotopdoorZ[];
extern u8 __attribute__((weak)) PskorpionmagZ[];
extern u8 __attribute__((weak)) PspectremagZ[];
extern u8 __attribute__((weak)) PspeedboatZ[];
extern u8 __attribute__((weak)) Pst_pete_room_1iZ[];
extern u8 __attribute__((weak)) Pst_pete_room_2iZ[];
extern u8 __attribute__((weak)) Pst_pete_room_3tZ[];
extern u8 __attribute__((weak)) Pst_pete_room_5cZ[];
extern u8 __attribute__((weak)) Pst_pete_room_6cZ[];
extern u8 __attribute__((weak)) Psteel_door1Z[];
extern u8 __attribute__((weak)) Psteel_door2Z[];
extern u8 __attribute__((weak)) Psteel_door2bZ[];
extern u8 __attribute__((weak)) Psteel_door3Z[];
extern u8 __attribute__((weak)) Pstool1Z[];
extern u8 __attribute__((weak)) Pswipe_card2Z[];
extern u8 __attribute__((weak)) Pswivel_chair1Z[];
extern u8 __attribute__((weak)) PtankZ[];
extern u8 __attribute__((weak)) PtigerZ[];
extern u8 __attribute__((weak)) Ptorpedo_rackZ[];
extern u8 __attribute__((weak)) Ptrain_door2Z[];
extern u8 __attribute__((weak)) Ptrain_door3Z[];
extern u8 __attribute__((weak)) Ptrain_doorZ[];
extern u8 __attribute__((weak)) PtrainextdoorZ[];
extern u8 __attribute__((weak)) Ptt33magZ[];
extern u8 __attribute__((weak)) Ptuning_console1Z[];
extern u8 __attribute__((weak)) Ptv1Z[];
extern u8 __attribute__((weak)) Ptv4screenZ[];
extern u8 __attribute__((weak)) Ptv_holderZ[];
extern u8 __attribute__((weak)) PtvscreenZ[];
extern u8 __attribute__((weak)) PuzimagZ[];
extern u8 __attribute__((weak)) PvertdoorZ[];
extern u8 __attribute__((weak)) PwalletbondZ[];
extern u8 __attribute__((weak)) PwindowZ[];
extern u8 __attribute__((weak)) Pwindow_cor11Z[];
extern u8 __attribute__((weak)) Pwindow_lib_lg1Z[];
extern u8 __attribute__((weak)) Pwindow_lib_sm1Z[];
extern u8 __attribute__((weak)) Pwood_lg_crate1Z[];
extern u8 __attribute__((weak)) Pwood_lg_crate2Z[];
extern u8 __attribute__((weak)) Pwood_md_crate3Z[];
extern u8 __attribute__((weak)) Pwood_sm_crate4Z[];
extern u8 __attribute__((weak)) Pwood_sm_crate5Z[];
extern u8 __attribute__((weak)) Pwood_sm_crate6Z[];
extern u8 __attribute__((weak)) Pwooden_table1Z[];
extern u8 __attribute__((weak)) PwppkmagZ[];

extern u8 __attribute__((weak)) Tbg_ame_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_arch_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_arec_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_ark_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_ash_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_azt_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_cat_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_cave_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_crad_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_cryp_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_dam_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_depo_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_dest_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_dish_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_imp_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_jun_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_len_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_oat_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_pete_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_ref_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_run_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_sev_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_sevb_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_sevx_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_silo_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_stat_all_p_stanZ[];
extern u8 __attribute__((weak)) Tbg_tra_all_p_stanZ[];

extern u8 __attribute__((weak)) UbriefarchZ[];
extern u8 __attribute__((weak)) UbriefarkZ[];
extern u8 __attribute__((weak)) UbriefaztZ[];
extern u8 __attribute__((weak)) UbriefcaveZ[];
extern u8 __attribute__((weak)) UbriefcontrolZ[];
extern u8 __attribute__((weak)) UbriefcradZ[];
extern u8 __attribute__((weak)) UbriefcrypZ[];
extern u8 __attribute__((weak)) UbriefdamZ[];
extern u8 __attribute__((weak)) UbriefdepoZ[];
extern u8 __attribute__((weak)) UbriefdestZ[];
extern u8 __attribute__((weak)) UbriefjunZ[];
extern u8 __attribute__((weak)) UbriefpeteZ[];
extern u8 __attribute__((weak)) UbriefrunZ[];
extern u8 __attribute__((weak)) UbriefsevbZ[];
extern u8 __attribute__((weak)) UbriefsevbunkerZ[];
extern u8 __attribute__((weak)) UbriefsevxZ[];
extern u8 __attribute__((weak)) UbriefsevxbZ[];
extern u8 __attribute__((weak)) UbriefsiloZ[];
extern u8 __attribute__((weak)) UbriefstatueZ[];
extern u8 __attribute__((weak)) UbrieftraZ[];

extern u8 __attribute__((weak)) Ump_setupameZ[];
extern u8 __attribute__((weak)) Ump_setuparchZ[];
extern u8 __attribute__((weak)) Ump_setuparkZ[];
extern u8 __attribute__((weak)) Ump_setupashZ[];
extern u8 __attribute__((weak)) Ump_setupcaveZ[];
extern u8 __attribute__((weak)) Ump_setupcradZ[];
extern u8 __attribute__((weak)) Ump_setupcrypZ[];
extern u8 __attribute__((weak)) Ump_setupdishZ[];
extern u8 __attribute__((weak)) Ump_setupimpZ[];
extern u8 __attribute__((weak)) Ump_setupoatZ[];
extern u8 __attribute__((weak)) Ump_setuprefZ[];
extern u8 __attribute__((weak)) Ump_setupsevbZ[];
extern u8 __attribute__((weak)) Ump_setupstatueZ[];

extern u8 __attribute__((weak)) UsetuparchZ[];
extern u8 __attribute__((weak)) UsetuparkZ[];
extern u8 __attribute__((weak)) UsetupaztZ[];
extern u8 __attribute__((weak)) UsetupcaveZ[];
extern u8 __attribute__((weak)) UsetupcontrolZ[];
extern u8 __attribute__((weak)) UsetupcradZ[];
extern u8 __attribute__((weak)) UsetupcrypZ[];
extern u8 __attribute__((weak)) UsetupdamZ[];
extern u8 __attribute__((weak)) UsetupdepoZ[];
extern u8 __attribute__((weak)) UsetupdestZ[];
extern u8 __attribute__((weak)) UsetupjunZ[];
extern u8 __attribute__((weak)) UsetuplenZ[];
extern u8 __attribute__((weak)) UsetuppeteZ[];
extern u8 __attribute__((weak)) UsetuprunZ[];
extern u8 __attribute__((weak)) UsetupsevbZ[];
extern u8 __attribute__((weak)) UsetupsevbunkerZ[];
extern u8 __attribute__((weak)) UsetupsevxZ[];
extern u8 __attribute__((weak)) UsetupsevxbZ[];
extern u8 __attribute__((weak)) UsetupsiloZ[];
extern u8 __attribute__((weak)) UsetupstatueZ[];
extern u8 __attribute__((weak)) UsetuptraZ[];

	extern u8 __attribute__((weak)) LameE[];
	extern u8 __attribute__((weak)) LameJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LameP[];
#endif
	extern u8 __attribute__((weak)) LarchE[];
	extern u8 __attribute__((weak)) LarchJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LarchP[];
#endif
	extern u8 __attribute__((weak)) LarecE[];
	extern u8 __attribute__((weak)) LarecJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LarecP[];
#endif
	extern u8 __attribute__((weak)) LarkE[];
	extern u8 __attribute__((weak)) LarkJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LarkP[];
#endif
	extern u8 __attribute__((weak)) LashE[];
	extern u8 __attribute__((weak)) LashJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LashP[];
#endif
	extern u8 __attribute__((weak)) LaztE[];
	extern u8 __attribute__((weak)) LaztJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LaztP[];
#endif
	extern u8 __attribute__((weak)) LcatE[];
	extern u8 __attribute__((weak)) LcatJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LcatP[];
#endif
	extern u8 __attribute__((weak)) LcaveE[];
	extern u8 __attribute__((weak)) LcaveJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LcaveP[];
#endif
	extern u8 __attribute__((weak)) LcradE[];
	extern u8 __attribute__((weak)) LcradJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LcradP[];
#endif
	extern u8 __attribute__((weak)) LcrypE[];
	extern u8 __attribute__((weak)) LcrypJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LcrypP[];
#endif
	extern u8 __attribute__((weak)) LdamE[];
	extern u8 __attribute__((weak)) LdamJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LdamP[];
#endif
	extern u8 __attribute__((weak)) LdepoE[];
	extern u8 __attribute__((weak)) LdepoJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LdepoP[];
#endif
	extern u8 __attribute__((weak)) LdestE[];
	extern u8 __attribute__((weak)) LdestJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LdestP[];
#endif
	extern u8 __attribute__((weak)) LdishE[];
	extern u8 __attribute__((weak)) LdishJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LdishP[];
#endif
	extern u8 __attribute__((weak)) LearE[];
	extern u8 __attribute__((weak)) LearJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LearP[];
#endif
	extern u8 __attribute__((weak)) LeldE[];
	extern u8 __attribute__((weak)) LeldJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LeldP[];
#endif
	extern u8 __attribute__((weak)) LgunE[];
	extern u8 __attribute__((weak)) LgunJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LgunP[];
#endif
	extern u8 __attribute__((weak)) LimpE[];
	extern u8 __attribute__((weak)) LimpJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LimpP[];
#endif
	extern u8 __attribute__((weak)) LjunE[];
	extern u8 __attribute__((weak)) LjunJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LjunP[];
#endif
	extern u8 __attribute__((weak)) LleeE[];
	extern u8 __attribute__((weak)) LleeJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LleeP[];
#endif
	extern u8 __attribute__((weak)) LlenE[];
	extern u8 __attribute__((weak)) LlenJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LlenP[];
#endif
	extern u8 __attribute__((weak)) LlipE[];
	extern u8 __attribute__((weak)) LlipJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LlipP[];
#endif
	extern u8 __attribute__((weak)) LlueE[];
	extern u8 __attribute__((weak)) LlueJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LlueP[];
#endif
	extern u8 __attribute__((weak)) LmiscE[];
	extern u8 __attribute__((weak)) LmiscJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LmiscP[];
#endif
	extern u8 __attribute__((weak)) LmpmenuE[];
	extern u8 __attribute__((weak)) LmpmenuJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LmpmenuP[];
#endif
	extern u8 __attribute__((weak)) LmpweaponsE[];
	extern u8 __attribute__((weak)) LmpweaponsJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LmpweaponsP[];
#endif
	extern u8 __attribute__((weak)) LoatE[];
	extern u8 __attribute__((weak)) LoatJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LoatP[];
#endif
	extern u8 __attribute__((weak)) LoptionsE[];
	extern u8 __attribute__((weak)) LoptionsJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LoptionsP[];
#endif
	extern u8 __attribute__((weak)) LpamE[];
	extern u8 __attribute__((weak)) LpamJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LpamP[];
#endif
	extern u8 __attribute__((weak)) LpeteE[];
	extern u8 __attribute__((weak)) LpeteJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LpeteP[];
#endif
	extern u8 __attribute__((weak)) LpropobjE[];
	extern u8 __attribute__((weak)) LpropobjJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LpropobjP[];
#endif
	extern u8 __attribute__((weak)) LrefE[];
	extern u8 __attribute__((weak)) LrefJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LrefP[];
#endif
	extern u8 __attribute__((weak)) LritE[];
	extern u8 __attribute__((weak)) LritJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LritP[];
#endif
	extern u8 __attribute__((weak)) LrunE[];
	extern u8 __attribute__((weak)) LrunJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LrunP[];
#endif
	extern u8 __attribute__((weak)) LsevE[];
	extern u8 __attribute__((weak)) LsevJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LsevP[];
#endif
	extern u8 __attribute__((weak)) LsevbE[];
	extern u8 __attribute__((weak)) LsevbJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LsevbP[];
#endif
	extern u8 __attribute__((weak)) LsevxE[];
	extern u8 __attribute__((weak)) LsevxJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LsevxP[];
#endif
	extern u8 __attribute__((weak)) LsevxbE[];
	extern u8 __attribute__((weak)) LsevxbJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LsevxbP[];
#endif
	extern u8 __attribute__((weak)) LshoE[];
	extern u8 __attribute__((weak)) LshoJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LshoP[];
#endif
	extern u8 __attribute__((weak)) LsiloE[];
	extern u8 __attribute__((weak)) LsiloJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LsiloP[];
#endif
	extern u8 __attribute__((weak)) LstatE[];
	extern u8 __attribute__((weak)) LstatJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LstatP[];
#endif
	extern u8 __attribute__((weak)) LtitleE[];
	extern u8 __attribute__((weak)) LtitleJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LtitleP[];
#endif
	extern u8 __attribute__((weak)) LtraE[];
	extern u8 __attribute__((weak)) LtraJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LtraP[];
#endif
	extern u8 __attribute__((weak)) LwaxE[];
	extern u8 __attribute__((weak)) LwaxJ[];
#ifdef VERSION_EU
	extern u8 __attribute__((weak)) LwaxP[];
#endif
extern u8 __attribute__((weak)) ob__ob_end_seg[];
