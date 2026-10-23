# English lettering for the game's Korean textures. Read by tools\texture_translate.py.
# Every coordinate is in the texture's own pixels (see the tool's docstring for Box).
import csv
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
from texture_translate import Box  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

# Song titles, one English name each, used wherever the song appears (wheel, course cards).
SONGS = {
    "피로": "Fatigue", "느낌": "Feeling", "우주미아": "Lost in Space", "하나뿐인 당신에게": "To My Only One",
    "적월애": "Red Moon Love", "천지개벽": "Genesis", "빛바랜 영혼": "Faded Soul", "빛바랜영혼": "Faded Soul",
    "주문을 외워봐": "Recite the Spell", "Harpe를 외워봐": "Recite the Harpe", "설탕음모론": "Sugar Conspiracy",
    "속삭여줘": "Whisper to Me", "비행": "Flight", "찬가 II": "Hymn II", "찬가": "Hymn", "없잖아": "You're Not Here",
    "잘가라": "Farewell", "애인을 내려주세요": "Send Me a Lover", "바람의 날개": "Wings of the Wind",
    "크리스마스의 기억": "Memories of Christmas", "유령의 축제": "Ghost Festival", "유령의 축제 2": "Ghost Festival 2",
    "시간여행": "Time Travel",
}

WHITE = (235, 235, 235)
BRIGHT = (250, 250, 250)
GOLD = (240, 176, 20)
K, MAG, RED, GRN, BLU, TEAL, PUR = (22, 22, 22), (232, 22, 160), (228, 24, 24), (24, 122, 52), (24, 110, 210), (18, 60, 60), (140, 40, 200)

SPEC = []


def add(rel, *boxes):
    SPEC.append((rel, list(boxes)))


# ---- option descriptions (512x64: icon, yellow arrow ending at x=56, text) ----------------------
for r in csv.DictReader(open(os.path.join(HERE, "translations.csv"), encoding="utf-8-sig")):
    f = r["file"]
    if f.startswith("SongSelect\\Effector\\effector") and r["h"] == "64":
        add(f, Box(58, 0, 512, 64, r["english"].replace(" / ", "\n")))
add(r"SongSelect\Effector\eff_temp.abm", Box(60, 0, 512, 64, "Notes fade out as they fall"))

# ---- option popups (EF 1 line right of the yellow arrow) -------------------------------------------
add(r"SongSelect\Effector\p_EffectPop_ruby.abm",
    Box(270, 140, 436, 170, "EF 1 |~once: |close options", size=10, colors=[[(240, 90, 90), WHITE, (240, 180, 40)]]))
for f in (r"SongSelect\Effector\p_effectpop.abm", r"SongSelect\Effector\p_effectpop_catch.abm"):
    add(f, Box(270, 130, 436, 175, "EF 1 |~once: |switch category\nEF 1 |~twice: |close options", size=10,
               colors=[[(240, 90, 90), WHITE, (240, 180, 40)], [(240, 90, 90), WHITE, (240, 180, 40)]]))

# ---- button hint bars (white text between icons; only the white text is touched) ------------------
def hint(rel, *spans):
    add(rel, *[Box(x0, y0, x1, y1, t, ink="white", colors=[WHITE], grow=2) for x0, y0, x1, y1, t in spans])


hint(r"CV2Mix\modeselect\manual.abm", (40, 20, 70, 46, "MOVE"), (87, 20, 126, 46, "SELECT"))
hint(r"Ranking\TT_rank_UseKey_Panel.abm", (124, 18, 155, 46, "MOVE"), (189, 18, 221, 46, "ENTER"),
     (248, 18, 280, 46, "DEL"), (300, 18, 352, 46, "END"))
hint(r"Ranking\Vranking\TT_Vrank_Usekey.abm", (187, 17, 219, 45, "MOVE"), (253, 17, 284, 45, "ENTER"),
     (311, 17, 343, 45, "DEL"), (363, 17, 420, 45, "END"))
hint(r"SongSelect\TT_UseKey2_panel.abm", (38, 4, 81, 27, "CATEGORY"), (129, 4, 156, 27, "OPTION"),
     (178, 4, 202, 27, "SKIN"), (216, 4, 250, 27, "EXIT"))
hint(r"SongSelect\TT_UseKey_catch_panel.abm", (45, 4, 92, 27, "CATEGORY"), (144, 4, 172, 27, "OPTION"),
     (211, 4, 250, 27, "EXIT"))
hint(r"SongSelect\TT_UseKey_ruby_panel.abm", (70, 4, 115, 27, "OPTION"), (138, 4, 181, 27, "SKIN"),
     (217, 4, 250, 27, "EXIT"))
hint(r"SongSelect\TT_course_notice1.abm", (42, 23, 96, 41, "OPTIONS"), (164, 23, 218, 41, "RANKING"))
hint(r"SongSelect\TT_course_notice2.abm", (66, 19, 123, 39, "MOVE"), (162, 19, 218, 39, "SELECT"))
hint(r"SongSelect\TT_course_notice1_org.abm", (66, 24, 144, 40, "OPTIONS"), (164, 24, 218, 40, "RANKING"))
hint(r"SongSelect\TT_course_notice2_org.abm", (66, 18, 127, 41, "MOVE"), (164, 18, 218, 41, "SELECT"))
hint(r"SongSelect\VF\ot_categoryexpl.abm", (94, 7, 250, 24, "CHANGE CATEGORY"))
add(r"CV2Mix\modeselect\tgselect_up.abm", Box(330, 58, 530, 83, "Choose a target to attack", ink="white", grow=3))
# "START를 누른 상태에서" = "while START is held": START stays, the Korean after it becomes "held".
add(r"InGameEffector\TT\14k_Panel.abm", Box(87, 60, 194, 77, "START |~held", size=10, colors=[[WHITE, (170, 170, 170)]]),
    Box(70, 126, 190, 139, "SPEED DURING PLAY", align="center", grow=2))
add(r"InGameEffector\TT\7k_panel.abm", Box(87, 69, 194, 86, "START |~held", size=10, colors=[[WHITE, (170, 170, 170)]]))

# ---- mode select -----------------------------------------------------------------------------------
MODE = {
    "basicmode_5keymix": "Play with the five keys only",
    "basicmode_rubymix": "Beginner mode: five keys, turntable and pedal",
    "basicmode_streetmix": "Standard mode: five keys, turntable and pedal",
    "basicmode_streetmix - 복사본": "Uses the turntable, the pedal and every key",
    "casual_ez2catch": "Steer the turntable to catch the falling notes",
    "casual_scratchmix": "Advanced mode: the keys and the turntable at once",
    "speicalmode_cv2mix": "HIDDEN : a special mode that tests your limits",
    "speicalmode_legacyradio": "A 7-key course mode scored on listener ratings",
}
for k, v in MODE.items():
    add("modeselect\\ModeText\\%s.abm" % k, Box(0, 36, 512, 62, v, align="center"))
add(r"modeselect\o_top_typo_one.abm", Box(20, 44, 256, 62, "Choose the mode you want to play."))
add(r"modeselect\CategoryGlow\o_top_typo_one.abm", Box(0, 33, 256, 52, "Choose the mode you want to play", align="center"))

# ---- title / attract -------------------------------------------------------------------------------
# The boot warning (warning.abm, WarningShort\) and the logo screen's insertcoin/pressstart stay in
# Korean, by the user's call (2026-09-26): they are the game's identity, not menus to be read.
add(r"BattleMode\count.abm", Box(0, 100, 256, 124, "Adjust your speed", align="center"),
    Box(0, 129, 256, 156, "The game starts soon.", align="center", weight="regular"))

# ---- how to play (dark text on white, keywords coloured) --------------------------------------------
H = "HowToPlay\\%s.abm"
add(H % "auready", Box(12, 10, 244, 40, "The game starts |now!", align="center", colors=[[K, K]]))
add(H % "bg01_txt01", Box(0, 6, 512, 28, "In |EZ2AC |you play music with the |keys|, |turntable |and |pedal|.",
                          align="center", colors=[[K, TEAL, K, BLU, K, MAG, K, GRN, K]]))
add(H % "bg01_txt02", Box(0, 10, 512, 54, "When a falling |note |meets the red |judgment line|,\n"
                                          "hit the matching |input |to play its sound.",
                          align="center", colors=[[K, BLU, K, RED, K], [K, GRN, K]]))
add(H % "bg02_txt01", Box(0, 6, 256, 28, "These are the |inputs |EZ2AC uses.", align="center", colors=[[K, GRN, K]]))
add(H % "bg02_txt02", Box(0, 10, 256, 54, "Main: white and blue |keys|, |turntable|, |pedal\n"
                                          "Extra: the |effect buttons",
                          align="center", colors=[[K, BLU, K, MAG, K, GRN], [K, RED]]))
add(H % "bg03_txt01", Box(0, 22, 256, 107, "Spin the |turntable |to pick a song,\n"
                                            "and use the |blue keys\nto change its |difficulty|.",
                          align="center", colors=[[K, MAG, K], [K, BLU], [K, GRN, K]]))
add(H % "bg03_txt02", Box(0, 23, 256, 108, "The |effect buttons |set the options,\n"
                                            "|START |changes the sort order,\nthe |pedal |changes the skin.",
                          align="center", colors=[[K, RED, K], [BLU, K], [K, GRN, K]]))
add(H % "bg03_txt03", Box(0, 5, 256, 28, "Press a |white key |to choose the song.", align="center", colors=[[K, BLU, K]]))
add(H % "bg04_txt01", Box(0, 4, 256, 30, "[ How to hit |normal notes |]", align="center", colors=[[TEAL, PUR, K]]))
add(H % "bg04_txt02", Box(0, 30, 256, 230,
    "When a |white or blue note |meets\nthe |judgment line|, press that |key|.\n"
    "When a |pink note |meets\nthe |judgment line|, spin the |turntable\nany way. Notes in a row: alternate.\n"
    "When a |green note |meets\nthe |judgment line|, step on the |pedal|.",
    align="center", colors=[[K, BLU, K], [K, RED, K, BLU, K], [K, MAG, K], [K, RED, K, MAG], [K],
                            [K, GRN, K], [K, RED, K, GRN, K]]))
add(H % "bg05_txt01", Box(0, 4, 256, 30, "[ How to play |long notes |]", align="center", colors=[[TEAL, PUR, K]]))
add(H % "bg05_txt02", Box(0, 30, 256, 230,
    "When a |white or blue note |meets\nthe |judgment line|, hold that |key |to its end.\n"
    "When a |pink note |meets\nthe |judgment line|, keep the |turntable\nturning one way to its end.\n"
    "When a |green note |meets\nthe |judgment line|, hold the |pedal |to its end.",
    align="center", colors=[[K, BLU, K], [K, RED, K, BLU, K], [K, MAG, K], [K, RED, K, MAG], [K],
                            [K, GRN, K], [K, RED, K, GRN, K]]))
add(H % "bg06_txt01", Box(0, 11, 256, 55, "Keep the |groove gauge |from emptying\n"
                                          "to play the |next stage|.", align="center", colors=[[K, RED, K], [K, BLU, K]]))
add(H % "bg06_txt02", Box(0, 5, 256, 28, "Enjoy all of |EZ2AC|'s |songs|!", align="center", colors=[[K, TEAL, K, GRN, K]]))
add(H % "ez2ac_txt0", Box(0, 6, 256, 28, "How to play |EZ2AC", align="center", colors=[[K, TEAL]]))
add(H % "skip", Box(0, 8, 256, 26, "Press |START |to |SKIP|.", align="center", colors=[[K, TEAL, K, TEAL, K]]))

# ---- patch notes -----------------------------------------------------------------------------------
add(r"patchnote\TT_ModeSelect_Notice.abm",
    Box(70, 12, 186, 27, "~Back to |MODE SELECT", size=9, colors=[[WHITE, (240, 180, 40)]]),
    Box(30, 60, 500, 365,
        "<EZ2AC : TIME TRAVELER> Ver 1.77 update\n\n"
        "1. New songs to play:\n   - Memories of Christmas\n   - Send Me a Lover\n\n"
        "2. New courses to play:\n   <5K> - Level : OMEGA -Attenuated- [?]\n   <10K> - Transin [17]\n\n"
        "3. The 5K COURSE 'Level : OMEGA' play issue is fixed and it is now\n"
        "   'Level : OMEGA -Origin-'. Its ranking had to be reset; sorry."))

# ---- course cards (Channel_info 512x256: title at y~30, songs at y 77/104/131/158 from x 337) -------
YOUR7 = "~[ Your 7 ]  |"


def course_rows(rel, title=None, name=None, rows=()):
    boxes = []
    if title is not None:
        boxes.append(Box(215, 12, 512, 46, title, colors=[[BRIGHT, BRIGHT]]))
    for k, t in rows:
        boxes.append(Box(330, 66 + 27 * k, 512, 90 + 27 * k, t, size=12.5, colors=[BRIGHT]))
    add(rel, *boxes)


CI = "Channel_info\\%s.abm"
course_rows(CI % r"10radio\10icon", rows=[(3, "Feeling")])
course_rows(CI % r"10radio\11cafe", rows=[(3, "Fatigue")])
course_rows(CI % r"10radio\11indi", rows=[(2, "Fatigue")])
course_rows(CI % r"10radio\11lab", title="10-Key Lab")
course_rows(CI % r"10radio\11lost", rows=[(2, "Lost in Space")])
course_rows(CI % r"10radio\11nostalgic", rows=[(3, "To My Only One")])
course_rows(CI % r"10radio\7two", rows=[(0, "To My Only One"), (2, "Feeling")])
for f, n, rows in [(r"10radio\y710kresearcher", "10-Key Researcher", ()), (r"10radio\y7One", "One", ()),
                   (r"10radio\y7dimcynical", "dimcynical", [(0, "Faded Soul")]), (r"10radio\y7johndoe", "John Doe", ()),
                   (r"10radio\y7lightonix", "Lightonix", ()), (r"14radio\y7chneko", "Chneko", [(3, "Genesis")]),
                   (r"14radio\y7dimcynical", "dimcynical", ()), (r"14radio\y7geodesic", "geodesic", ()),
                   (r"14radio\y7lightonix", "Lightonix", [(2, "Genesis")]), (r"14radio\y7partisan", "Partisan", ()),
                   (r"5radio\y7Luv2", "LUV2", ()), (r"5radio\y7arisa", "Arisa Minazuki", ()),
                   (r"5radio\y7flowd", "Flow.D", ()), (r"5radio\y7lightonix", "Lightonix", ()),
                   (r"radio\y7chneko", "Chneko", [(3, "Genesis")]), (r"radio\y7kien", "KIEN", ()),
                   (r"radio\y7neta", "Neta", ()), (r"radio\y7zoke", "zoke", [(1, "Faded Soul")])]:
    course_rows(CI % f, title=YOUR7 + n, rows=rows)
course_rows(CI % r"14radio\10icon", rows=[(3, "Feeling")])
course_rows(CI % r"14radio\11cafe", rows=[(3, "Fatigue")])
course_rows(CI % r"14radio\11ln", title="Wrong Encounter")
course_rows(CI % r"14radio\11nostalgic", rows=[(3, "To My Only One")])
course_rows(CI % r"14radio\11rv", rows=[(2, "Red Moon Love")])
course_rows(CI % r"5radio\11cafe", rows=[(3, "Fatigue")])
course_rows(CI % r"5radio\11indi", rows=[(2, "Fatigue")])
course_rows(CI % r"5radio\11nostalgic", rows=[(3, "To My Only One")])
course_rows(CI % r"5radio\12dlc", title="Conscience DLC")
course_rows(CI % r"5radio\7aya", rows=[(0, "Recite the Spell")])
course_rows(CI % r"radio\11lost", rows=[(2, "Lost in Space")])
course_rows(CI % r"radio\11nostalgic", rows=[(3, "To My Only One")])

# ---- course tags (Channel_Tag 256x32, right-aligned: white "[ 당신의 7 ]" + gold name) --------------
CT = "Channel_Tag\\%s.abm"
for f, n in [(r"10radio\y710kresearcher", "10-Key Researcher"), (r"10radio\y7dimcynical", "dimcynical"),
             (r"10radio\y7johndoe", "John Doe"), (r"10radio\y7lightonix", "Lightonix"), (r"10radio\y7one", "One"),
             (r"14radio\y7chneko", "Chneko"), (r"14radio\y7dimcynical", "dimcynical"),
             (r"14radio\y7geodesic", "geodesic"), (r"14radio\y7lightonix", "Lightonix"),
             (r"14radio\y7partisan", "Partisan"), (r"5radio\y7arisa", "Arisa Minazuki"), (r"5radio\y7flowd", "Flow.D"),
             (r"5radio\y7lightonix", "Lightonix"), (r"5radio\y7luv2", "LUV2"), (r"Radio\y7Neta", "Neta"),
             (r"Radio\y7chneko", "Chneko"), (r"Radio\y7kien", "KIEN"), (r"Radio\y7zoke", "zoke")]:
    add(CT % f, Box(0, 0, 256, 32, "~[ Your 7 ]  |" + n, align="right", colors=[[GOLD, GOLD]]))
add(CT % r"10radio\11lab", Box(0, 0, 256, 32, "10-Key Lab", align="right"))
add(CT % r"14radio\11ln", Box(0, 0, 256, 32, "Wrong Encounter", align="right"))
add(CT % r"5radio\12dlc", Box(0, 0, 256, 32, "Conscience DLC", align="right"))

# ---- song names in the wheel (songname 256x32, right-aligned; only the Korean line is replaced) ----
SN = r"songname\%s.abm"
for f, y0, y1, t in [
        ("10fatigue", 6, 30, "Fatigue"), ("10harpe2", 6, 30, "Recite the Harpe"), ("11spacemia", 6, 30, "Lost in Space"),
        ("11sugar", 4, 19, "Sugar Conspiracy"), ("11whisper", 6, 30, "Whisper to Me"), ("15flight", 6, 30, "Flight"),
        ("5Cheonji", 6, 30, "Genesis"), ("5bitba", 6, 30, "Faded Soul"), ("6ez2dj", 6, 30, "Hymn II"),
        ("6feelrm", 4, 19, "Feeling"), ("7nothing", 6, 30, "You're Not Here"), ("7onlyyou", 6, 30, "To My Only One"),
        ("7redmoon", 6, 30, "Red Moon Love"), ("7spell", 6, 30, "Recite the Spell"),
        ("7weep", 20, 31, "Cruelly Beautiful Tears"), ("changa", 6, 30, "Hymn"), ("feel", 6, 30, "Feeling"),
        ("o2GoHell", 5, 19, "Farewell"), ("o2aein", 6, 30, "Send Me a Lover"), ("o2baram", 6, 30, "Wings of the Wind"),
        ("o2christ", 6, 30, "Memories of Christmas"), ("o2ghost", 6, 30, "Ghost Festival"),
        ("o2ghost2", 6, 30, "Ghost Festival"), ("o2ghost3", 6, 30, "Ghost Festival 2"), ("o2timetravel", 6, 30, "Time Travel")]:
    glow = f in ("10harpe2", "11sugar", "o2aein", "o2christ", "o2ghost", "o2ghost3", "11spacemia", "15flight", "11whisper")
    add(SN % f, Box(2, y0, 252, y1, t, align="right", glow=glow, pad=3 if glow else 2))
