# JSRF cutscene catalogue (all 300 e*.dat events)

Generated 2026-09-24 from mission bytecode (mssn*.bin, all 311 missions) and event files. Scripts: evinfo.py, sim.py, req.py, build.py, catalogue.py in this folder.

Classes (best route per event): A=74, B=19, C=39, C?=0, D=168

- **A** plays automatically after `RECOMP_CHAPTER_JUMP=C:M` (chapter flags cleared). Simulated, so the ordering and timing are inferred; the conditions are confirmed.
- **B** plays on load/idle but needs chapter flags (WriteStateFlag dwords given). The jump cannot write them today.
- **C** needs player action (talk, challenge region, fight, race, tags cleared, yes answer).
- **D** is never played by any mission's 0xE3 command (unused, or only loaded).

## Model notes

- Everything under 'plays' is CONFIRMED from mission bytecode: mission, block index, flag conditions, flag writes (GG-Notebook layout).
- Class A/B are the output of a forward simulation of the command blocks (sim.py); its execution model is INFERRED (GG-Notebook prose + CMission::runListenerCmds 0x56990 jump table), not traced in the engine: res flag writes -> imm in order -> nbl/auto-listeners as flags allow -> one blk at a time lowest index first -> E6 exits followed (C flags kept, M cleared, 0xFF next chapter clears C).
- Automatic listeners: only 0x20 (unconditional) and 0x00 (offscreen timer). Everything else (plane pass, talk, challenge region 0x18, fights 0x0a/0x0c/0x0d/0x0f, race 0x29, rival 0x1b, yes/no 0x16) is treated as player action.
- Simulated with the player's slot-0 G/S flags (G1,G10 + 20 S flags); re-running with G/S all clear changed no class-A result.
- Timings t_frame are from event durations (evdump) + Wait (0x57) args; talk events counted as 300 f, transmissions 240 f (guesses). Load times are not included.
- RECOMP_CHAPTER_JUMP clears ALL chapter (C) flags and sets only C2=1 for 1:0; a savetool edit of C flags is wiped by the jump. Class B therefore needs either a jump extension that writes the listed WriteStateFlag dwords after ClearStateFlags (same call the jump already makes for 0x00080011), or a save whose return mission + C flags are set with savetool and loaded with Continue (no jump).

## Class A sweep list (one jump each; events in expected order, t = frames after mission start, loads excluded)

| Jump | Missions (chain) | Events in order (start frame, length) | Total | Ends |
|---|---|---|---|---|
| `1:22` | 0122 | e024@0(1777), e025@1777(600) | 2377 f (40 s) | idle: waits for player |
| `1:96` | 0196→0100 | e200⚠@0(1268), e201⚠@1268(2640), e202@3908(790), e203@4698(1042), e204@5740(800), e205@6540(660), e206@7200(947) | 8147 f (136 s) | idle: waits for player |
| `2:30` | 0230 | e027@0(600) | 600 f (10 s) | idle: waits for player |
| `2:31` | 0231 | e028@0(930) | 930 f (16 s) | idle: waits for player |
| `2:40` | 0240 | e032@0(750) | 750 f (12 s) | idle: waits for player |
| `2:50` | 0250 | e036@0(1660) | 1660 f (28 s) | idle: waits for player |
| `2:96` | 0296→0294 | e210⚠@0(700), e211⚠@700(1050), e212@1750(1150), e213⚠@2900(1080), e214⚠@3980(704) | 4684 f (78 s) | idle: waits for player |
| `3:60` | 0360 | e040@0(470) | 470 f (8 s) | idle: waits for player |
| `3:61` | 0361 | e041@0(1000), TE203(talk)@1000(300) | 1300 f (22 s) | idle: waits for player |
| `3:96` | 0396→0394 | e220@0(830), e221@830(520), e222@1350(1080), e223@2430(620) | 3050 f (51 s) | idle: waits for player |
| `4:50` | 0450 | e048@0(700) | 700 f (12 s) | idle: waits for player |
| `4:51` | 0451 | e049@0(810) | 810 f (14 s) | idle: waits for player |
| `4:60` | 0460 | e052@0(600) | 600 f (10 s) | idle: waits for player |
| `4:61` | 0461 | e053@0(700) | 700 f (12 s) | idle: waits for player |
| `4:62` | 0462 | e054@0(500) | 500 f (8 s) | idle: waits for player |
| `4:70` | 0470 | e055@0(880) | 880 f (15 s) | idle: waits for player |
| `4:80` | 0480 | e059@0(300) | 300 f (5 s) | idle: waits for player |
| `4:96` | 0496→0494 | e230@0(1458), e231@1458(560), e232@2018(815), e233@2833(1500) | 4333 f (72 s) | idle: waits for player |
| `5:14` | 0514 | e068@0(1585) | 1585 f (26 s) | idle: waits for player |
| `5:96` | 0596→0594 | e240@0(1414), e241@1414(1114) | 2528 f (42 s) | idle: waits for player |
| `6:10` | 0610 | e071@0(600) | 600 f (10 s) | idle: waits for player |
| `6:20` | 0620 | e070@0(600) | 600 f (10 s) | idle: waits for player |
| `6:30` | 0630 | e073@0(600) | 600 f (10 s) | idle: waits for player |
| `6:41` | 0641 | e054@0(500) | 500 f (8 s) | idle: waits for player |
| `6:55` | 0655 | e072@0(600) | 600 f (10 s) | idle: waits for player |
| `6:60` | 0660 | e075@0(850) | 850 f (14 s) | idle: waits for player |
| `6:61` | 0661 | e076@0(1360) | 1360 f (23 s) | idle: waits for player |
| `6:96` | 0696→0694 | e250@0(668), e251@668(280), e252@948(824) | 1772 f (30 s) | idle: waits for player |
| `7:41` | 0741 | e054@0(500) | 500 f (8 s) | idle: waits for player |
| `7:50` | 0750 | e082@0(2050) | 2050 f (34 s) | idle: waits for player |
| `7:56` | 0756 | e080@0(840) | 840 f (14 s) | idle: waits for player |
| `7:96` | 0796→0794 | e260@0(1408), e261@1408(652), e262@2060(650), e263@2710(700), e264@3410(600), e265@4010(730), e266@4740(402) | 5142 f (86 s) | idle: waits for player |
| `8:11` | 0811→0890 | e098@0(620), e100@860(900), e101@1760(2930) | 4690 f (78 s) | idle: waits for player |
| `8:12` | 0812 | e097@0(600), e119@600(5397), e120@5997(2070) | 8067 f (134 s) | idle: waits for player |
| `8:13` | 0813→0899→0996 | e105@0(1500), e291@1500(6120), e290@7620(620) | 8240 f (137 s) | exit type 0xfe (menu/special) -> stops |
| `8:16` | 0816 | e116@0(730) | 730 f (12 s) | idle: waits for player |
| `8:41` | 0841 | e054@0(500) | 500 f (8 s) | idle: waits for player |
| `8:80` | 0880 | e095@0(1150) | 1150 f (19 s) | idle: waits for player |
| `8:90` | 0890 | e101@0(2930) | 2930 f (49 s) | idle: waits for player |
| `8:96` | 0896→0894 | e270@0(590), e271@590(760), e272@1350(1080), e273@2430(1190), e274@3620(452) | 4072 f (68 s) | idle: waits for player |
| `8:98` | 0898→0895 | e280@0(2908), e281@2908(3328) | 6236 f (104 s) | idle: waits for player |
| `8:99` | 0899→0996 | e291@0(6120), e290@6120(620) | 6740 f (112 s) | exit type 0xfe (menu/special) -> stops |
| `9:41` | 0941 | e054@0(500) | 500 f (8 s) | idle: waits for player |
| `9:56` | 0956 | e080@0(840) | 840 f (14 s) | idle: waits for player |
| `9:96` | 0996 | e290@0(620) | 620 f (10 s) | exit type 0xfe (menu/special) -> stops |

⚠ = event the player reported a problem with.

## Reported events

- **e034** (Poison Jam (player report)): class C, 400 f, full cutscene (2 models). Route: load mssn0242 (jump 2:42 + chapter flags ['C24=1']), then player: lis[1]: NPC/rival player #1 returns to neutral state (chase/race over)
- **e111** (Poison Jam (player report)): class C, 600 f, full cutscene (1 models). Route: load mssn0242 (jump 2:42 + chapter flags ['C26=1']), then player: lis[1]: NPC/rival player #1 returns to neutral state (chase/race over)
- **e200** (DJ K (player report)): class A, 1268 f, full cutscene (11 models). Route: RECOMP_CHAPTER_JUMP=1:96
- **e201** (police (player report)): class A, 2640 f, full cutscene (14 models). Route: RECOMP_CHAPTER_JUMP=1:96
- **e210** (DJ K (player report)): class A, 700 f, full cutscene (11 models). Route: RECOMP_CHAPTER_JUMP=2:96
- **e211** (Poison Jam (player report)): class A, 1050 f, full cutscene (4 models). Route: RECOMP_CHAPTER_JUMP=2:96
- **e213** (police (player report)): class A, 1080 f, full cutscene (3 models). Route: RECOMP_CHAPTER_JUMP=2:96
- **e214** (DJ K (player report)): class A, 704 f, full cutscene (11 models). Route: RECOMP_CHAPTER_JUMP=2:96
- **Rokkaku crows**: no e*.dat has a crow/karasu model or animation path. The crows are stage-side (stg20 objects), not event data. stg20 missions: 2:40, 2:41, 2:42, 2:43, 3:40, 3:41, 4:25, 4:26, 6:30, 6:31, 7:30, 7:31, 7:32, 8:30, 8:31, 8:32, 9:30, 9:31. Class-A entries into stg20: `2:40` (e032 fly-over, then live stg20) and `6:30` (e073). Any stg20 mission shows the live stage after its events.

## Every event

| Event | Frames | Type | Class | Mission / block | Condition → writes | Route / recipe |
|---|---|---|---|---|---|---|
| e000 | 520 | fly-over/caption (camera path + text/voice, no models) | B | mssn0120 imm[7] | C11=1,C20=0 → C20=1 | jump 1:20 with chapter flags ['C11=1'] WriteStateFlag: 0x00080059 |
| e001 | 1430 | fly-over/caption (camera path + text/voice, no models) | B | mssn0110 blk[1] | C2=1 → C2=0,C3=1,M100=0 | jump 1:10 with chapter flags ['C2=1'] WriteStateFlag: 0x00080011 |
| e002 | 500 | full cutscene (1 models) | C | mssn0110 blk[8] | M111=1,C4=1 → M111=0,M112=1,C4=0,C5=1 | load mssn0110 (jump 1:10 + chapter flags ['C3=1', 'C4=1']), then player: lis[2]: all tags cleared |
| e003 | 1582 | full cutscene (1 models) | B | mssn0110 blk[22] | M166=1 → M166=0,M167=1 | jump 1:10 with chapter flags ['C9=1'] WriteStateFlag: 0x00080049 |
| e004 | 590 | full cutscene (3 models) | B | mssn0110 blk[23] | M167=1,C9=1 → C9=0,C10=1 | jump 1:10 with chapter flags ['C9=1'] WriteStateFlag: 0x00080049 |
| e005 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e006 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e007 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e008 | 400 | fly-over/caption (camera path + text/voice, no models) | D | – | – | unreferenced |
| e009 | 250 | full cutscene (1 models) | D | – | – | unreferenced |
| e010 | 660 | full cutscene (1 models) | C | mssn0120 blk[3] | M113=1,C21=0 → M113=0,M114=1,C21=1 | load mssn0120 (jump 1:20), then player: lis[0]: player triggers op-0x70 object #0 (challenge/door?) |
| e011 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e012 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e013 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e014 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e015 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e016 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e017 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e018 | 870 | full cutscene (12 models) | D | – | – | unreferenced |
| e019 | 1020 | full cutscene (12 models) | D | – | – | unreferenced |
| e020 | 250 | full cutscene (1 models) | C | mssn0111 blk[3] | M200=1,M302=1 → M302=0,M303=1,M301=0 | load mssn0111 (jump 1:11 + chapter flags ['C7=1']), then player: lis[1]: NPC/rival player #1 returns to neutral state (chase/race over) |
| e021 | 400 | full cutscene (1 models) | B | mssn0120 blk[19] | M201=1 → C30=0,C31=1,M201=0,M202=1 | jump 1:20 with chapter flags ['C30=1'] WriteStateFlag: 0x000800F1 |
| e022 | 660 | full cutscene (1 models) | B | mssn0120 blk[24] | M253=1 → M253=0,M254=1 | jump 1:20 with chapter flags ['C32=1'] WriteStateFlag: 0x00080101 |
| e023 | 500 | full cutscene (1 models) | C | mssn0120 blk[5] | M122=1 → M122=0,M123=1,C22=1 | load mssn0120 (jump 1:20 + chapter flags ['C21=1']), then player: lis[2]: all tags cleared |
| e024 | 1777 | full cutscene (1 models) | A | mssn0122 imm[3] | – → – | RECOMP_CHAPTER_JUMP=1:22 |
| e025 | 600 | full cutscene (3 models) | A | mssn0122 imm[4] | – → – | RECOMP_CHAPTER_JUMP=1:22 |
| e026 | 250 | full cutscene (2 models) | C | mssn0122 blk[2] | M100=1 → M199=1 | load mssn0122 (jump 1:22), then player: lis[0]: enemy group #0 all defeated/cleared (fight; alt state check) |
| e027 | 600 | fly-over/caption (camera path + text/voice, no models) | A | mssn0230 imm[7] | C10=0 → C10=1,C11=1 | RECOMP_CHAPTER_JUMP=2:30 |
| e028 | 930 | full cutscene (2 models) | A | mssn0231 imm[4] | – → – | RECOMP_CHAPTER_JUMP=2:31 |
| e029 | 300 | full cutscene (1 models) | C | mssn0231 blk[1] | M111=1 → M111=0,M112=1 | load mssn0231 (jump 2:31), then player: lis[0]: enemy group #0 all defeated/cleared (fight; alt state check) |
| e030 | 370 | full cutscene (1 models) | B | mssn0230 blk[9] | M141=1 → M141=0,M142=1,M140=0 | jump 2:30 with chapter flags ['C14=1', 'C19=1'] WriteStateFlag: 0x00080071, 0x00080099 |
| e031 | 300 | full cutscene (1 models) | B | mssn0230 blk[13] | M161=1 → M161=0,M162=1,C15=0,C16=1,C23=1 | jump 2:30 with chapter flags ['C15=1'] WriteStateFlag: 0x00080079 |
| e032 | 750 | fly-over/caption (camera path + text/voice, no models) | A | mssn0240 imm[8] | C20=0 → C20=1,C21=1 | RECOMP_CHAPTER_JUMP=2:40 |
| e033 | 500 | full cutscene (1 models) | B | mssn0243 imm[0] | C420=1 → – | jump 2:43 with chapter flags ['C420=1'] WriteStateFlag: 0x00080D21 |
| e033 | 500 | full cutscene (1 models) | B | mssn0341 imm[0] | C420=1 → – | jump 3:41 with chapter flags ['C420=1'] WriteStateFlag: 0x00080D21 |
| e033 | 500 | full cutscene (1 models) | B | mssn0426 imm[0] | C420=1 → – | jump 4:26 with chapter flags ['C420=1'] WriteStateFlag: 0x00080D21 |
| e033 | 500 | full cutscene (1 models) | B | mssn0631 imm[2] | C420=1 → – | jump 6:31 with chapter flags ['C420=1'] WriteStateFlag: 0x00080D21 |
| e033 | 500 | full cutscene (1 models) | B | mssn0731 imm[3] | C420=1 → – | jump 7:31 with chapter flags ['C420=1'] WriteStateFlag: 0x00080D21 |
| e033 | 500 | full cutscene (1 models) | B | mssn0831 imm[3] | C420=1 → – | jump 8:31 with chapter flags ['C420=1'] WriteStateFlag: 0x00080D21 |
| e033 | 500 | full cutscene (1 models) | B | mssn0931 imm[0] | C420=1 → – | jump 9:31 with chapter flags ['C420=1'] WriteStateFlag: 0x00080D21 |
| e034 ⚠ | 400 | full cutscene (2 models) | C | mssn0242 blk[1] | C24=1,M202=1 → M202=0,M203=1 | load mssn0242 (jump 2:42 + chapter flags ['C24=1']), then player: lis[1]: NPC/rival player #1 returns to neutral state (chase/race over) |
| e035 | 600 | full cutscene (1 models) | C | mssn0241 blk[3] | M121=1 → M121=0,M122=1 | load mssn0241 (jump 2:41 + chapter flags ['C103=1']), then player: lis[2]: enemy group #2 all defeated/cleared (fight; alt state check) |
| e036 | 1660 | fly-over/caption (camera path + text/voice, no models) | A | mssn0250 imm[7] | C30=0 → C30=1,C31=1 | RECOMP_CHAPTER_JUMP=2:50 |
| e037 | 550 | full cutscene (2 models) | B | mssn0251 imm[5] | C151=1,C156=0 → – | jump 2:51 with chapter flags ['C151=1'] WriteStateFlag: 0x000804B9 |
| e038 | 300 | full cutscene (1 models) | B | mssn0251 imm[6] | C152=1,C157=0 → – | jump 2:51 with chapter flags ['C152=1'] WriteStateFlag: 0x000804C1 |
| e039 | 390 | full cutscene (1 models) | C | mssn0250 blk[8] | M153=1 → M153=0,M154=1,C36=1 | load mssn0250 (jump 2:50 + chapter flags ['C35=1']), then player: lis[7]: player triggers op-0x70 object #3 (challenge/door?) |
| e040 | 470 | fly-over/caption (camera path + text/voice, no models) | A | mssn0360 imm[3] | C1=0 → C1=1,C2=1 | RECOMP_CHAPTER_JUMP=3:60 |
| e041 | 1000 | full cutscene (2 models) | A | mssn0361 imm[2] | – → – | RECOMP_CHAPTER_JUMP=3:61 |
| e042 | 600 | full cutscene (2 models) | C | mssn0361 blk[1] | M161=1 → M161=0,M162=1 | load mssn0361 (jump 3:61), then player: lis[4]: player action type 13 x1065353216; lis[3]: player action type 14 x1077936128; lis[2]: player action type 14 x1065353216; lis[1]: player action type 13 x1077936128; lis[0]: player action type 13 x1065353216 |
| e043 | 500 | fly-over/caption (camera path + text/voice, no models) | C | mssn0360 blk[9] | M123=1 → M123=0,M124=1 | load mssn0360 (jump 3:60 + chapter flags ['C5=1']), then player: lis[5]: player passes plane region #6 |
| e044 | 450 | full cutscene (1 models) | C | mssn0360 blk[10] | M134=1 → M134=0,M135=1,C479=1 | load mssn0360 (jump 3:60 + chapter flags ['C6=1']), then player: lis[7]: enemies defeated >= 1 (fight) |
| e045 | 600 | full cutscene (1 models) | C | mssn0360 blk[11] | M138=1 → M138=0,M139=1 | load mssn0360 (jump 3:60 + chapter flags ['C6=1']), then player: lis[9]: enemy group #0 all defeated/cleared (fight; alt state check); lis[7]: enemies defeated >= 1 (fight) |
| e046 | 1690 | full cutscene (4 models) | C | mssn0370 blk[3] | M104=1 → M104=0,M105=1 | load mssn0370 (jump 3:70), then player: lis[0]: player triggers op-0x70 object #0 (challenge/door?) |
| e047 | 440 | full cutscene (1 models) | B | mssn0370 blk[6] | C14=1,M150=0 → M150=1,M151=1 | jump 3:70 with chapter flags ['C14=1'] WriteStateFlag: 0x00080071 |
| e048 | 700 | fly-over/caption (camera path + text/voice, no models) | A | mssn0450 imm[7] | C10=0 → C10=1,C11=1 | RECOMP_CHAPTER_JUMP=4:50 |
| e049 | 810 | full cutscene (5 models) | A | mssn0451 imm[4] | – → – | RECOMP_CHAPTER_JUMP=4:51 |
| e050 | 770 | full cutscene (5 models) | C | mssn0451 blk[1] | M152=1 → M152=0,M153=1 | load mssn0451 (jump 4:51), then player: lis[0]: an enemy of group #0 reaches state (chase/fight, arg 50) |
| e051 | 900 | full cutscene (9 models) | C | mssn0451 blk[2] | M155=1 → M155=0,M156=1 | load mssn0451 (jump 4:51), then player: lis[1]: enemy group #0 all defeated/cleared (fight; alt state check); lis[0]: an enemy of group #0 reaches state (chase/fight, arg 50) |
| e052 | 600 | full cutscene (1 models) | A | mssn0460 imm[7] | C20=0 → C20=1,C21=1 | RECOMP_CHAPTER_JUMP=4:60 |
| e053 | 700 | full cutscene (2 models) | A | mssn0461 imm[4] | – → M170=1 | RECOMP_CHAPTER_JUMP=4:61 |
| e054 | 500 | full cutscene (1 models) | A | mssn0462 imm[4] | – → – | RECOMP_CHAPTER_JUMP=4:62 |
| e054 | 500 | full cutscene (1 models) | A | mssn0641 imm[4] | – → – | RECOMP_CHAPTER_JUMP=6:41 |
| e054 | 500 | full cutscene (1 models) | A | mssn0741 imm[5] | – → – | RECOMP_CHAPTER_JUMP=7:41 |
| e054 | 500 | full cutscene (1 models) | A | mssn0841 imm[5] | – → – | RECOMP_CHAPTER_JUMP=8:41 |
| e054 | 500 | full cutscene (1 models) | A | mssn0941 imm[5] | – → – | RECOMP_CHAPTER_JUMP=9:41 |
| e055 | 880 | fly-over/caption (camera path + text/voice, no models) | A | mssn0470 imm[7] | C30=0 → C30=1,C31=1 | RECOMP_CHAPTER_JUMP=4:70 |
| e056 | 500 | full cutscene (1 models) | B | mssn0471 imm[4] | C201=1,C206=0 → – | jump 4:71 with chapter flags ['C201=1'] WriteStateFlag: 0x00080649 |
| e057 | 680 | full cutscene (1 models) | C | mssn0470 blk[4] | M163=1 → M163=0,M164=1 | load mssn0470 (jump 4:70 + chapter flags ['C41=1']), then player: lis[5]: player triggers op-0x70 object #2 (challenge/door?) |
| e058 | 320 | full cutscene (1 models) | B | mssn0470 blk[6] | M171=1 → M171=0,M172=1,C43=1 | jump 4:70 with chapter flags ['C42=1'] WriteStateFlag: 0x00080151 |
| e059 | 300 | fly-over/caption (camera path + text/voice, no models) | A | mssn0480 imm[6] | C50=0 → C50=1,C51=1 | RECOMP_CHAPTER_JUMP=4:80 |
| e060 | 1160 | full cutscene (3 models) | C | mssn0480 blk[1] | M103=1 → M103=0,M104=1 | load mssn0480 (jump 4:80 + chapter flags ['C51=1']), then player: lis[1]: player triggers op-0x70 object #0 (challenge/door?) |
| e061 | 600 | full cutscene (4 models) | C | mssn0480 blk[3] | M123=1 → M123=0,M124=1 | load mssn0480 (jump 4:80 + chapter flags ['C52=1']), then player: lis[3]: player triggers op-0x70 object #0 (challenge/door?) |
| e062 | 1230 | fly-over/caption (camera path + text/voice, no models) | C | mssn0510 blk[12] | M320=1 → M320=0,M321=1 | load mssn0510 (jump 5:10 + chapter flags ['C1=1']), then player: lis[10]: player answers YES; lis[9]: player talks to talk-character #4 [talk-triggered] |
| e063 | 600 | full cutscene (3 models) | B | mssn0510 imm[6] | C2=1,C19=1,C29=0 → – | jump 5:10 with chapter flags ['C2=1', 'C19=1'] WriteStateFlag: 0x00080011, 0x00080099 |
| e064 | 600 | full cutscene (4 models) | C | mssn0510 blk[17] | M330=1 → M330=0,M331=1 | load mssn0510 (jump 5:10 + chapter flags ['C2=1']), then player: lis[14]: player answers YES; lis[13]: player talks to talk-character #4 [talk-triggered] |
| e065 | 620 | full cutscene (3 models) | B | mssn0510 imm[8] | C3=1,C19=1,C29=0 → C19=0 | jump 5:10 with chapter flags ['C3=1', 'C19=1'] WriteStateFlag: 0x00080019, 0x00080099 |
| e066 | 1170 | full cutscene (3 models) | C | mssn0510 blk[21] | M340=1 → M340=0,M341=1 | load mssn0510 (jump 5:10 + chapter flags ['C3=1']), then player: lis[18]: player answers YES; lis[17]: player talks to talk-character #4 [talk-triggered] |
| e067 | 1280 | full cutscene (13 models) | C | mssn0513 blk[2] | M200=1,M201=0 → M201=1 | load mssn0513 (jump 5:13), then player: lis[0]: race result: position == 0 (arg0=1) (0=win) |
| e068 | 1585 | full cutscene (6 models) | A | mssn0514 imm[3] | – → – | RECOMP_CHAPTER_JUMP=5:14 |
| e069 | 600 | full cutscene (2 models) | C | mssn0514 blk[1] | M200=1,M201=0 → M201=1 | load mssn0514 (jump 5:14), then player: lis[0]: enemy group #0 all defeated (fight) |
| e070 | 600 | full cutscene (2 models) | A | mssn0620 imm[7] | C1=0 → C1=1 | RECOMP_CHAPTER_JUMP=6:20 |
| e071 | 600 | full cutscene (2 models) | A | mssn0610 imm[5] | C1=0 → C1=1 | RECOMP_CHAPTER_JUMP=6:10 |
| e072 | 600 | full cutscene (2 models) | A | mssn0655 imm[7] | C1=0 → C1=1 | RECOMP_CHAPTER_JUMP=6:55 |
| e073 | 600 | full cutscene (2 models) | A | mssn0630 imm[10] | C1=0 → C1=1 | RECOMP_CHAPTER_JUMP=6:30 |
| e074 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e075 | 850 | full cutscene (3 models) | A | mssn0660 imm[3] | C52=0 → C52=1 | RECOMP_CHAPTER_JUMP=6:60 |
| e076 | 1360 | full cutscene (4 models) | A | mssn0661 imm[2] | – → – | RECOMP_CHAPTER_JUMP=6:61 |
| e077 | 1680 | full cutscene (8 models) | B | mssn0660 imm[4] | C53=1 → M200=1 | jump 6:60 with chapter flags ['C53=1'] WriteStateFlag: 0x000801A9 |
| e078 | 940 | full cutscene (1 models) | C | mssn0720 blk[4] | M200=1,M201=0 → M201=1 | load mssn0720 (jump 7:20), then player: lis[0]: player triggers op-0x70 object #0 (challenge/door?) |
| e078 | 940 | full cutscene (1 models) | C | mssn0920 blk[4] | M200=1,M201=0 → M200=0,M201=1 | load mssn0920 (jump 9:20), then player: lis[0]: player triggers op-0x70 object #0 (challenge/door?) |
| e079 | 750 | full cutscene (1 models) | C | mssn0725 blk[2] | M200=1,M201=0 → M201=1 | load mssn0725 (jump 7:25), then player: lis[0]: player triggers op-0x70 object #0 (challenge/door?) |
| e079 | 750 | full cutscene (1 models) | C | mssn0925 blk[2] | M200=1,M201=0 → M201=1 | load mssn0925 (jump 9:25), then player: lis[0]: player triggers op-0x70 object #0 (challenge/door?) |
| e080 | 840 | full cutscene (2 models) | A | mssn0756 imm[4] | – → – | RECOMP_CHAPTER_JUMP=7:56 |
| e080 | 840 | full cutscene (2 models) | A | mssn0956 imm[4] | – → – | RECOMP_CHAPTER_JUMP=9:56 |
| e081 | 120 | stub (empty placeholder file) | D | – | – | loaded only: mssn0700 nbl[11] 0x32 ['M122=1'] |
| e082 | 2050 | full cutscene (2 models) | A | mssn0750 imm[2] | C51=0 → C51=1,C52=1 | RECOMP_CHAPTER_JUMP=7:50 |
| e083 | 1200 | full cutscene (1 models) | C | mssn0750 blk[6] | M107=1 → M107=0,M108=1,C52=0,C53=1,M200=0 | load mssn0750 (jump 7:50 + chapter flags ['C52=1']), then player: lis[3]: enemy group #0 all defeated (fight) |
| e084 | 450 | fly-over/caption (camera path + text/voice, no models) | C | mssn0750 blk[4] | M201=1 → M201=0,M202=1 | load mssn0750 (jump 7:50 + chapter flags ['C52=1']), then player: lis[0]: onscreen timer ran out |
| e085 | 900 | full cutscene (4 models) | C | mssn0750 blk[11] | M122=1 → M122=0,M123=1 | load mssn0750 (jump 7:50 + chapter flags ['C54=1']), then player: lis[5]: player talks to talk-character #0 [talk-triggered] |
| e085 | 900 | full cutscene (4 models) | C | mssn0752 blk[2] | M122=1 → M122=0,M123=1 | load mssn0752 (jump 7:52), then player: lis[0]: player talks to talk-character #0 [talk-triggered] |
| e086 | 500 | full cutscene (1 models) | C | mssn0751 blk[1] | M103=1 → M103=0,M104=1 | load mssn0751 (jump 7:51), then player: lis[0]: enemy group #0 all defeated (fight) |
| e087 | 250 | full cutscene (2 models) | C | mssn0815 blk[4] | M152=1 → M152=0,M153=1 | load mssn0815 (jump 8:15), then player: lis[0]: player triggers op-0x70 object #0 (challenge/door?) |
| e088 | 1100 | full cutscene (3 models) | C | mssn0816 blk[1] | M102=1 → M102=0,M103=1 | load mssn0816 (jump 8:16), then player: lis[0]: enemy group #0 all defeated/cleared (fight; alt state check) |
| e089 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e090 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e091 | 800 | full cutscene (1 models) | C | mssn0870 blk[8] | M152=1 → M152=0,M153=1 | load mssn0870 (jump 8:70), then player: lis[4]: player triggers op-0x70 object #0 (challenge/door?) |
| e092 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e093 | 450 | full cutscene (2 models) | C | mssn0872 blk[1] | M102=1 → M102=0,M103=1 | load mssn0872 (jump 8:72), then player: lis[0]: enemy group #0 all defeated/cleared (fight; alt state check) |
| e094 | 700 | full cutscene (5 models) | C | mssn0875 blk[6] | M153=1 → M153=0,C31=1 | load mssn0875 (jump 8:75 + chapter flags ['C30=1']), then player: lis[0]: player triggers op-0x70 object #0 (challenge/door?) |
| e095 | 1150 | full cutscene (11 models) | A | mssn0880 imm[3] | – → – | RECOMP_CHAPTER_JUMP=8:80 |
| e096 | 1200 | full cutscene (2 models) | C | mssn0880 blk[1] | M152=1 → M152=0,M153=1 | load mssn0880 (jump 8:80), then player: lis[0]: enemy group #0 all defeated (fight) |
| e097 | 600 | full cutscene (3 models) | A | mssn0812 imm[5] | C90=0 → – | RECOMP_CHAPTER_JUMP=8:12 |
| e098 | 620 | full cutscene (3 models) | A | mssn0811 imm[6] | – → – | RECOMP_CHAPTER_JUMP=8:11 |
| e099 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e100 | 900 | full cutscene (3 models) | A | mssn0811 blk[1] | M114=1 → M114=0,M115=1 | RECOMP_CHAPTER_JUMP=8:11 |
| e100 | 900 | full cutscene (3 models) | B | mssn0812 blk[3] | M172=1 → M172=0,M174=1 | jump 8:12 with chapter flags ['C90=1'] WriteStateFlag: 0x000802D1 |
| e100 | 900 | full cutscene (3 models) | C? | mssn0812 blk[4] | M173=1 → M173=0,M174=1 | not reached by forward sim with flags []; unresolved ['M173=1'] |
| e101 | 2930 | full cutscene (2 models) | A | mssn0890 imm[2] | – → – | RECOMP_CHAPTER_JUMP=8:90 |
| e102 | 1000 | full cutscene (5 models) | C | mssn0890 blk[1] | M102=1 → M102=0,M103=1,M109=1 | load mssn0890 (jump 8:90), then player: lis[0]: enemy group #0 all defeated/cleared (fight; alt state check) |
| e103 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e104 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e105 | 1500 | full cutscene (1 models) | A | mssn0813 imm[1] | – → M115=1 | RECOMP_CHAPTER_JUMP=8:13 |
| e106 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e107 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e108 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e109 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e110 | 300 | full cutscene (1 models) | D | – | – | unreferenced |
| e111 ⚠ | 600 | full cutscene (1 models) | C | mssn0242 blk[4] | C26=1,M202=1 → M202=0,M203=1 | load mssn0242 (jump 2:42 + chapter flags ['C26=1']), then player: lis[1]: NPC/rival player #1 returns to neutral state (chase/race over) |
| e112 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e113 | 600 | full cutscene (3 models) | C | mssn0510 blk[13] | M321=1 → M321=0,M322=1 | load mssn0510 (jump 5:10 + chapter flags ['C1=1']), then player: lis[10]: player answers YES; lis[9]: player talks to talk-character #4 [talk-triggered] |
| e114 | 600 | full cutscene (2 models) | B | mssn0510 imm[7] | C2=1,C19=1,C29=0 → C19=0 | jump 5:10 with chapter flags ['C2=1', 'C19=1'] WriteStateFlag: 0x00080011, 0x00080099 |
| e115 | 600 | full cutscene (1 models) | D | – | – | unreferenced |
| e116 | 730 | full cutscene (6 models) | A | mssn0816 imm[6] | – → – | RECOMP_CHAPTER_JUMP=8:16 |
| e117 | 600 | full cutscene (9 models) | D | – | – | unreferenced |
| e118 | 550 | full cutscene (2 models) | B | mssn0251 imm[4] | C150=1,C155=0 → – | jump 2:51 with chapter flags ['C150=1'] WriteStateFlag: 0x000804B1 |
| e119 | 5397 | full cutscene (3 models) | A | mssn0812 imm[6] | C90=0 → – | RECOMP_CHAPTER_JUMP=8:12 |
| e120 | 2070 | full cutscene (5 models) | A | mssn0812 imm[7] | C90=0 → – | RECOMP_CHAPTER_JUMP=8:12 |
| e121 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e122 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e123 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e124 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e125 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e126 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e127 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e128 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e129 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e130 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e131 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e132 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e133 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e134 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e135 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e136 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e137 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e138 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e139 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e140 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e141 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e142 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e143 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e144 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e145 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e146 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e147 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e148 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e149 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e150 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e151 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e152 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e153 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e154 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e155 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e156 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e157 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e158 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e159 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e160 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e161 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e162 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e163 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e164 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e165 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e166 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e167 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e168 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e169 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e170 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e171 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e172 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e173 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e174 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e175 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e176 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e177 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e178 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e179 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e180 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e181 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e182 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e183 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e184 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e185 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e186 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e187 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e188 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e189 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e190 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e191 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e192 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e193 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e194 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e195 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e196 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e197 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e198 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e199 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e200 ⚠ | 1268 | full cutscene (11 models) | A | mssn0196 imm[4] | M0=0 → M0=1,M1=1 | RECOMP_CHAPTER_JUMP=1:96 |
| e201 ⚠ | 2640 | full cutscene (14 models) | A | mssn0196 blk[1] | M1=1 → M1=0,M2=1 | RECOMP_CHAPTER_JUMP=1:96 |
| e202 | 790 | full cutscene (3 models) | A | mssn0196 blk[2] | M2=1 → M2=0,M3=1 | RECOMP_CHAPTER_JUMP=1:96 |
| e203 | 1042 | full cutscene (3 models) | A | mssn0196 blk[3] | M3=1 → M3=0,M4=1 | RECOMP_CHAPTER_JUMP=1:96 |
| e204 | 800 | full cutscene (1 models) | A | mssn0196 blk[4] | M4=1 → M4=0,M5=1 | RECOMP_CHAPTER_JUMP=1:96 |
| e205 | 660 | full cutscene (3 models) | A | mssn0196 blk[5] | M5=1 → M5=0,M6=1 | RECOMP_CHAPTER_JUMP=1:96 |
| e206 | 947 | full cutscene (11 models) | A | mssn0196 blk[6] | M6=1 → M6=0,M7=1,M10=1 | RECOMP_CHAPTER_JUMP=1:96 |
| e207 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e208 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e209 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e210 ⚠ | 700 | full cutscene (11 models) | A | mssn0296 imm[1] | M0=0 → M0=1,M1=1 | RECOMP_CHAPTER_JUMP=2:96 |
| e211 ⚠ | 1050 | full cutscene (4 models) | A | mssn0296 blk[1] | M1=1 → M1=0,M2=1 | RECOMP_CHAPTER_JUMP=2:96 |
| e212 | 1150 | full cutscene (1 models) | A | mssn0296 blk[2] | M2=1 → M2=0,M3=1 | RECOMP_CHAPTER_JUMP=2:96 |
| e213 ⚠ | 1080 | full cutscene (3 models) | A | mssn0296 blk[3] | M3=1 → M3=0,M4=1 | RECOMP_CHAPTER_JUMP=2:96 |
| e214 ⚠ | 704 | full cutscene (11 models) | A | mssn0296 blk[4] | M4=1 → M4=0,M5=1,M10=1 | RECOMP_CHAPTER_JUMP=2:96 |
| e215 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e216 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e217 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e218 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e219 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e220 | 830 | full cutscene (11 models) | A | mssn0396 imm[2] | M0=0 → M0=1,M1=1 | RECOMP_CHAPTER_JUMP=3:96 |
| e221 | 520 | full cutscene (1 models) | A | mssn0396 blk[1] | M1=1 → M1=0,M2=1 | RECOMP_CHAPTER_JUMP=3:96 |
| e222 | 1080 | fly-over/caption (camera path + text/voice, no models) | A | mssn0396 blk[2] | M2=1 → M2=0,M3=1 | RECOMP_CHAPTER_JUMP=3:96 |
| e223 | 620 | full cutscene (1 models) | A | mssn0396 blk[3] | M3=1 → M3=0,M4=1,M10=1 | RECOMP_CHAPTER_JUMP=3:96 |
| e224 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e225 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e226 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e227 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e228 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e229 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e230 | 1458 | full cutscene (11 models) | A | mssn0496 imm[3] | M0=0 → M0=1,M1=1 | RECOMP_CHAPTER_JUMP=4:96 |
| e231 | 560 | full cutscene (3 models) | A | mssn0496 blk[1] | M1=1 → M1=0,M2=1 | RECOMP_CHAPTER_JUMP=4:96 |
| e232 | 815 | full cutscene (1 models) | A | mssn0496 blk[2] | M2=1 → M2=0,M3=1 | RECOMP_CHAPTER_JUMP=4:96 |
| e233 | 1500 | full cutscene (1 models) | A | mssn0496 blk[3] | M3=1 → M3=0,M4=1,M10=1 | RECOMP_CHAPTER_JUMP=4:96 |
| e234 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e235 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e236 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e237 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e238 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e239 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e240 | 1414 | full cutscene (11 models) | A | mssn0596 imm[3] | M0=0 → M0=1,M1=1 | RECOMP_CHAPTER_JUMP=5:96 |
| e241 | 1114 | full cutscene (1 models) | A | mssn0596 blk[1] | M1=1 → M1=0,M2=1,M10=1 | RECOMP_CHAPTER_JUMP=5:96 |
| e242 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e243 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e244 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e245 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e246 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e247 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e248 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e249 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e250 | 668 | full cutscene (11 models) | A | mssn0696 imm[3] | M0=0 → M0=1,M1=1 | RECOMP_CHAPTER_JUMP=6:96 |
| e251 | 280 | full cutscene (1 models) | A | mssn0696 blk[1] | M1=1 → M1=0,M2=1 | RECOMP_CHAPTER_JUMP=6:96 |
| e252 | 824 | full cutscene (2 models) | A | mssn0696 blk[2] | M2=1 → M2=0,M3=1,M10=1 | RECOMP_CHAPTER_JUMP=6:96 |
| e253 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e254 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e255 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e256 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e257 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e258 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e259 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e260 | 1408 | full cutscene (11 models) | A | mssn0796 imm[1] | M0=0 → M0=1,M1=1 | RECOMP_CHAPTER_JUMP=7:96 |
| e261 | 652 | full cutscene (1 models) | A | mssn0796 blk[1] | M1=1 → M1=0,M2=1 | RECOMP_CHAPTER_JUMP=7:96 |
| e262 | 650 | full cutscene (5 models) | A | mssn0796 blk[2] | M2=1 → M2=0,M3=1 | RECOMP_CHAPTER_JUMP=7:96 |
| e263 | 700 | full cutscene (7 models) | A | mssn0796 blk[3] | M3=1 → M3=0,M4=1 | RECOMP_CHAPTER_JUMP=7:96 |
| e264 | 600 | full cutscene (4 models) | A | mssn0796 blk[4] | M4=1 → M4=0,M5=1 | RECOMP_CHAPTER_JUMP=7:96 |
| e265 | 730 | full cutscene (1 models) | A | mssn0796 blk[5] | M5=1 → M5=0,M6=1 | RECOMP_CHAPTER_JUMP=7:96 |
| e266 | 402 | full cutscene (11 models) | A | mssn0796 blk[6] | M6=1 → M6=0,M7=1,M10=1,C499=1 | RECOMP_CHAPTER_JUMP=7:96 |
| e267 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e268 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e269 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e270 | 590 | full cutscene (11 models) | A | mssn0896 imm[1] | M0=0 → M0=1,M1=1 | RECOMP_CHAPTER_JUMP=8:96 |
| e271 | 760 | full cutscene (2 models) | A | mssn0896 blk[1] | M1=1 → M1=0,M2=1 | RECOMP_CHAPTER_JUMP=8:96 |
| e272 | 1080 | full cutscene (11 models) | A | mssn0896 blk[2] | M2=1 → M2=0,M3=1 | RECOMP_CHAPTER_JUMP=8:96 |
| e273 | 1190 | full cutscene (1 models) | A | mssn0896 blk[3] | M3=1 → M3=0,M4=1 | RECOMP_CHAPTER_JUMP=8:96 |
| e274 | 452 | full cutscene (11 models) | A | mssn0896 blk[4] | M4=1 → M4=0,M5=1,M10=1 | RECOMP_CHAPTER_JUMP=8:96 |
| e275 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e276 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e277 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e278 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e279 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e280 | 2908 | full cutscene (15 models) | A | mssn0898 imm[1] | M0=0 → M0=1,M1=1 | RECOMP_CHAPTER_JUMP=8:98 |
| e281 | 3328 | full cutscene (4 models) | A | mssn0898 blk[1] | M1=1 → M1=0,M2=1,M10=1 | RECOMP_CHAPTER_JUMP=8:98 |
| e282 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e283 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e284 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e285 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e286 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e287 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e288 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e289 | 120 | stub (empty placeholder file) | D | – | – | unreferenced |
| e290 | 620 | full cutscene (11 models) | A | mssn0996 imm[0] | M0=0 → M0=1,M1=1,M10=1 | RECOMP_CHAPTER_JUMP=9:96 |
| e291 | 6120 | full cutscene (14 models) | A | mssn0899 imm[1] | M0=0 → M0=1,M1=1,M10=1 | RECOMP_CHAPTER_JUMP=8:99 |
| e292 | 600 | stub (empty placeholder file) | D | – | – | unreferenced |
| e293 | 600 | stub (empty placeholder file) | D | – | – | unreferenced |
| e294 | 600 | stub (empty placeholder file) | D | – | – | unreferenced |
| e295 | 600 | stub (empty placeholder file) | D | – | – | unreferenced |
| e296 | 600 | stub (empty placeholder file) | D | – | – | unreferenced |
| e297 | 600 | stub (empty placeholder file) | D | – | – | unreferenced |
| e298 | 600 | stub (empty placeholder file) | D | – | – | unreferenced |
| e299 | 600 | stub (empty placeholder file) | D | – | – | unreferenced |

## TalkEvents (0xE7, TE*.bin dialogue boxes, not e*.dat)

91 TalkEvent ids referenced; best class: {'C': 67, 'B/C?': 23, 'A': 1}. Class A: TE203 (3:61). Details in catalogue.json → talk_events.

