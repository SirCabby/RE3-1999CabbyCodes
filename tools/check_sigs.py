#!/usr/bin/env python3
"""Run every signature src/game.cpp uses against the unpacked dump.

    python3 tools/check_sigs.py [--dump ghidra_proj/ResidentEvil3.fixed.exe] [--any-build]

The mod finds everything by byte signature at run time and reads each global out
of the matched instruction's operands. This checks, offline and before a launch,
that every signature still matches exactly once and that the operands land where
game.cpp says they do - the values below are build 21774525 (English). A
mismatch here is a mismatch in the game. Pure stdlib.

--any-build is for a dump of another exe, such as RE3_MERCE.exe (the
Mercenaries, the same engine with its code a little further on): where each
signature matched and the calls it names are printed rather than checked, and
everything else - one match each, the data operands, the patch bytes - still is.
"""
import argparse, re, struct, sys

IMAGE_BASE = 0x400000

# The ending's rank scores the recovery total in ten steps: (highest total, score).
RANK_STEPS = [(3300, 10), (3100, 20), (2900, 30), (2700, 40), (2500, 50),
              (2300, 60), (2100, 70), (1900, 80), (1700, 90), (1500, 100)]

# name -> (pattern, [(offset, "label", expected value or None)], expected VA)
# "b:" before a label reads one byte, "w:" two, "r:" a call's rel32 as the VA it
# lands on, "c:" a dword that is a code address, anything else a dword.
SIGS = {
    "Task_chain": (
        "8B 44 24 04 8B 4C 24 08 C1 E0 07 89 88 ?? ?? ?? ?? 66 C7 80 ?? ?? ?? ?? 02 00 C3",
        [(13, "task entry table", 0x00A59128), (20, "task state table", 0x00A59124),
         (10, "b:stride shift", 7)],
        0x0042AF90),
    "Task_state": (
        "8B 44 24 04 33 C9 C1 E0 07 66 8B 88 ?? ?? ?? ?? 8B C1 C3",
        [(12, "task state table", 0x00A59124)],
        0x0042B0C0),
    "Task_sleep": (
        "A1 ?? ?? ?? ?? 66 8B 4C 24 04 68 00 00 00 FF 66 89 48 02 66 C7 00 01 00 E8",
        [(1, "running task block", 0x00A592A4)],
        0x0042AFB0),
    "status screen chain": (
        "8B 0D ?? ?? ?? ?? A1 ?? ?? ?? ?? BB 00 00 00 01 83 C4 04 0B CB F6 C4 02 C6 05 ?? ?? ?? ?? 00 "
        "89 3D ?? ?? ?? ?? 89 0D ?? ?? ?? ?? 74 33 68 ?? ?? ?? ?? 6A 01 E8",
        [(2, "state flags", 0x00A5C880), (39, "state flags (again)", 0x00A5C880),
         (7, "save block flags", 0x00A61F5C), (46, "c:status screen entry", 0x0045E2F0)],
        0x0042B234),
    "status screen entry": (
        "56 68 ?? ?? ?? ?? 33 F6 E8",
        [(2, "status screen state", 0x00A35EE0)],
        0x0045E2F0),
    "player damage": (
        "66 8B 0D ?? ?? ?? ?? 66 81 25 ?? ?? ?? ?? FF E0 66 2B C8 8B 44 24 08 83 F8 01 66 89 0D ?? ?? ?? ?? "
        "75 16 66 85 C9 7D 69 66 C7 05 ?? ?? ?? ?? 00 00 80 0D ?? ?? ?? ?? 08 C3",
        [(3, "player HP", 0x00A5CCC0), (29, "player HP (again)", 0x00A5CCC0),
         (43, "player HP (again)", 0x00A5CCC0), (51, "condition word, high byte", 0x00A5CCC7)],
        0x0046384A),
    "new game HP": (
        "B8 C8 00 00 00 66 A3 ?? ?? ?? ?? 66 A3 ?? ?? ?? ?? 74 63",
        [(7, "player HP", 0x00A5CCC0), (13, "player max HP", 0x00A5CCC2)],
        0x0042BFAF),
    "play time": (
        "F6 05 ?? ?? ?? ?? 80 75 3D A1 ?? ?? ?? ?? F6 C4 40 74 0C C7 05 ?? ?? ?? ?? 00 00 00 00 EB 27 6A FF "
        "E8 ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? 8B C8 2B CA 8B 15 ?? ?? ?? ?? 83 C4 04 03 D1 89 15 ?? ?? ?? ?? "
        "A3 ?? ?? ?? ??",
        [(2, "state flags", 0x00A5C880), (10, "state flags (again)", 0x00A5C880),
         (21, "play time", 0x00A61F58), (50, "play time (again)", 0x00A61F58),
         (61, "play time (again)", 0x00A61F58), (40, "tick base", 0x00A5C944),
         (66, "tick base (again)", 0x00A5C944), (34, "r:VSync", 0x00507F60)],
        0x0044F0F0),
    "ending's play time": (
        "6A FF E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? 2B C1 6A 05 03 D0 89 15 ?? ?? ?? ??",
        [(3, "r:VSync", 0x00507F60), (9, "tick base", 0x00A5C944), (15, "play time", 0x00A61F58),
         (27, "play time (again)", 0x00A61F58)],
        0x00442CD0),
    "VSync": (
        "8B 0D ?? ?? ?? ?? 8B 54 24 04 52 8B 01 FF 50 78 C3",
        [(2, "system object pointer", 0x005349BC), (15, "b:VSync's vtable slot", 0x78)],
        0x00507F60),
    "VSync count": (
        "8B 44 24 04 83 EC 08 85 C0 56 8B F1 7D 15 8B 4E 04 8B 01 FF 50 10 8B 86 ?? ?? ?? ?? 5E 83 C4 08 C2 04 00",
        [(24, "vblank count, in the object", 0x5AC)],
        0x00420AE0),
    "vblank": (
        "8B 86 ?? ?? ?? ?? C7 44 24 0C 00 00 00 00 40 89 86 ?? ?? ?? ?? 40",
        [(2, "vblank count, in the object", 0x5AC), (17, "vblank count (again)", 0x5AC)],
        0x0042098B),
    "save": (
        "A1 ?? ?? ?? ?? F6 C4 40 75 20 A8 80 75 1C 6A 00 6A 01 6A 00 68 ?? 00 00 00 E8 ?? ?? ?? ?? 83 C4 08 50 "
        "E8 ?? ?? ?? ?? 83 C4 0C 8B 1D ?? ?? ?? ?? A0 ?? ?? ?? ?? 81 E3 FF 00 00 00 3C 63 72 0A A1 ?? ?? ?? ?? "
        "F6 C4 40 74 0F F6 05 ?? ?? ?? ?? 80 75 06 FE 05 ?? ?? ?? ??",
        [(1, "state flags", 0x00A5C880), (64, "state flags (again)", 0x00A5C880),
         (75, "state flags (again)", 0x00A5C880), (21, "b:ink ribbon item id", 0x81),
         (44, "save count", 0x00A61F70), (49, "save count (again)", 0x00A61F70),
         (84, "save count (again)", 0x00A61F70)],
        0x0044DA91),
    "inventory select": (
        "8B 44 24 04 83 F8 07 76 11 83 F8 08 75 15 B8 01 00 00 00 A3 ?? ?? ?? ?? EB 0E 33 C0 A3 ?? ?? ?? ?? "
        "EB 05 A1 ?? ?? ?? ?? 8D 04 80 C1 E0 06 05 ?? ?? ?? ?? A3 ?? ?? ?? ??",
        [(20, "inventory index", 0x00A623E8), (29, "inventory index (again)", 0x00A623E8),
         (36, "inventory index (again)", 0x00A623E8), (45, "b:block stride shift", 6),
         (47, "inventory block base", 0x00A62164), (52, "inventory pointer", 0x00A623E4)],
        0x0045EC80),
    "find_item_slot": (
        "8A 44 24 08 56 8B 35 ?? ?? ?? ?? 57 88 44 24 10 8A 8E ?? ?? 00 00 3A C1 73 1D 8A 54 24 0C 8B 7C 24 10 "
        "81 E7 FF 00 00 00 38 14 BE 74 0C FE C0 3A C1",
        [(7, "inventory pointer", 0x00A623E4), (18, "w:usable slot count offset", 0x12A)],
        0x004DE9D0),
    "typewriter": (
        "53 68 ?? 00 00 00 E8 ?? ?? ?? ?? 83 C4 08 84 C0 7C 1A 68 00 00 00 FF 6A 01 53 53 E8",
        [(2, "b:ink ribbon item id", 0x81), (7, "r:find_item_slot", 0x004DE9D0)],
        0x0045B9E7),
    "remove_item_slot": (
        "8A 0D ?? ?? ?? ?? 5E 3A C8 5B 74 24 A0 ?? ?? ?? ?? 3A C2 72 37 3C FF 74 33 FE C8 A2 ?? ?? ?? ?? "
        "A1 ?? ?? ?? ?? 0D 00 00 00 40 A3 ?? ?? ?? ??",
        [(2, "status state +0x2E (weapon put away)", 0x00A35F0E), (13, "status state +0x2F (saved slot)", 0x00A35F0F),
         (28, "status state +0x2F (again)", 0x00A35F0F), (33, "status state +0x14 (flags)", 0x00A35EF4),
         (43, "status state +0x14 (again)", 0x00A35EF4)],
        0x004DEAFA),
    "item box": (
        "2C 03 88 44 24 24 79 06 04 40 88 44 24 24 33 C9 BB ?? ?? ?? ?? 89 4C 24 18 89 5C 24 1C EB 08 8A 44 24 24 "
        "8B 4C 24 18 0F BE D0 A1 ?? ?? ?? ?? 8D 7C 90 28 8A 44 90 28 84 C0",
        [(9, "b:box slots", 0x40), (43, "inventory pointer", 0x00A623E4), (50, "b:box offset", 0x28),
         (54, "b:box offset (again)", 0x28)],
        0x004E5751),
    "item table": (
        "8B 35 ?? ?? ?? ?? 33 C9 33 D2 8A 0C 86 8A 14 8D ?? ?? ?? ?? 33 C9 8A 4C 86 01 03 CD 3B D1 73 03 40",
        [(2, "inventory pointer", 0x00A623E4), (16, "item table, second column", 0x0052AC51)],
        0x0046F212),
    "entity walk": (
        "8B 35 ?? ?? ?? ?? 81 FE ?? ?? ?? ?? C6 44 24 14 02 74 50 B8 ?? ?? ?? ?? 8B 00 8A 88 2F 01 00 00 3A CB "
        "74 20 83 38 00 74 1B",
        [(2, "entity list end", 0x00A5CE6C), (8, "entity list + 2 entries", 0x00A5CDCC),
         (20, "entity list + 2 entries (again)", 0x00A5CDCC)],
        0x0042ABFB),
    "entity list base": (
        "8D 04 8D ?? ?? ?? ?? 3B C6 75 BB",
        [(3, "entity list", 0x00A5CDC4)],
        0x0042AC4D),
    "button state stores": (
        "89 2D ?? ?? ?? ?? 89 15 ?? ?? ?? ?? 89 3D ?? ?? ?? ?? 74 47",
        [(2, "buttons held", 0x00A5C85C), (8, "buttons pressed", 0x00A5C868),
         (14, "buttons repeating", 0x00A5C860)],
        0x0042CF2A),
    "button repeat store": (
        "89 3D ?? ?? ?? ?? 5F 5E 5D 5B 59 C3",
        [(2, "buttons repeating", 0x00A5C860)],
        0x0042CF7F),
    "letterbox": (
        "A0 ?? ?? ?? ?? 56 8B 35 ?? ?? ?? ?? A8 08 A0 ?? ?? ?? ?? 74 0C 3C EF 77 04 04 10",
        [(1, "save block flags", 0x00A61F5C), (15, "letterbox level", 0x00A628BA)],
        0x004246D0),
    "movie frame": (
        "F7 05 ?? ?? ?? ?? 00 00 01 00 74 05 E8",
        [(2, "state flags", 0x00A5C880), (13, "r:movie player frame", 0x0042F7D0)],
        0x00423AC2),
    "movie end": (
        "66 3D 01 00 75 0E A2 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 00 EB 59 B3 02 EB 37 A0 ?? ?? ?? ?? B3 02 84 C3 75 0A "
        "66 83 3D ?? ?? ?? ?? 3C 76 40",
        [(7, "movie end byte", 0x00A4FD5A), (0x57, "movie end byte (again)", 0x00A4FD5A),
         (0x6E, "movie end byte (again)", 0x00A4FD5A), (13, "movie skip chain", 0x00A4FD6D),
         (0x61, "movie skip chain (again)", 0x00A4FD6D)],
        0x0042FB29),
    "message box": (
        "8A 15 ?? ?? ?? ?? 33 C0 80 FA 01 55 56 57 8B 7C 24 10 0F 94 C0 8B B7 D4 75 00 00 8B E8 33 C0 8A 87 CA 75 00 00",
        [(33, "state offset in the box", 0x75CA)],
        0x0042DB70),
    "message box call": (
        "A0 ?? ?? ?? ?? A8 40 74 10 F6 05 ?? ?? ?? ?? 80 75 07 24 BF A2 ?? ?? ?? ?? A8 80 74 17 68 ?? ?? ?? ?? E8 "
        "?? ?? ?? ?? 68 ?? ?? ?? ??",
        [(1, "message flags", 0x00A57468), (21, "message flags (again)", 0x00A57468),
         (30, "message box", 0x00A4FEA0), (40, "message box (again)", 0x00A4FEA0),
         (35, "r:message box frame", 0x0042DB70)],
        0x0042DB30),
    "voice player": (
        "68 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 78 00 00 00 C6 05 ?? ?? ?? ?? 01",
        [(1, "\"/data_a/voice/\"", 0x0051762C), (7, "voice volume", 0x00A59E48), (17, "voice state", 0x00A59E51)],
        0x00427252),
    "voice stop": (
        "A0 ?? ?? ?? ?? 84 C0 74 07 C6 05 ?? ?? ?? ?? 03 8B 0D",
        [(1, "voice state", 0x00A59E51), (11, "voice state (again)", 0x00A59E51)],
        0x0042B69C),
    "countdown tick": (
        "A1 ?? ?? ?? ?? A9 00 00 00 02 74 1D F7 05 ?? ?? ?? ?? 00 00 00 02 75 07 C6 05 ?? ?? ?? ?? 00 E8 ?? ?? ?? ?? "
        "E9 ?? ?? ?? ?? A9 00 00 00 04 0F 84 ?? ?? ?? ?? F6 05 ?? ?? ?? ?? 80 74 05 E8 ?? ?? ?? ?? 66 83 3D ?? ?? ?? ?? 1D "
        "7F 0C F7 05 ?? ?? ?? ?? 00 00 00 04 75 27 66 FF 8E ?? ?? ?? ?? A0 ?? ?? ?? ?? A8 80 66 89 3D ?? ?? ?? ?? "
        "66 C7 05 ?? ?? ?? ?? 08 00 74 07 66 FF 05 ?? ?? ?? ??",
        [(1, "save block flags", 0x00A61F5C), (14, "last frame's save flags", 0x00A58798),
         (78, "last frame's save flags (again)", 0x00A58798), (54, "state flags", 0x00A5C880),
         (96, "state flags (again)", 0x00A5C880), (69, "countdown frame count", 0x00A58792),
         (105, "countdown frame count (again)", 0x00A58792), (112, "countdown hundredths", 0x00A58794),
         (91, "countdown, inside [esi+]", 0x7868), (123, "Mercenaries seconds played", 0x00A61FC2),
         (62, "r:Mercenaries bonus read-out", 0x00455DC0), (26, "radar's state byte", 0x00A35B60),
         (32, "r:radar", 0x0045ECD0)],
        0x0042B396),
    "countdown frames": (
        "84 C0 75 2F A0 ?? ?? ?? ?? 84 C0 75 26 66 A1 ?? ?? ?? ?? 66 FF 05 ?? ?? ?? ?? 66 05 FD FF 66 3D 01 00 "
        "66 A3 ?? ?? ?? ?? 7D 09 66 C7 05 ?? ?? ?? ?? 08 00 8B 86 ?? ?? ?? ?? A3 ?? ?? ?? ??",
        [(15, "countdown hundredths", 0x00A58794), (36, "countdown hundredths (again)", 0x00A58794),
         (45, "countdown hundredths (again)", 0x00A58794), (22, "countdown frame count", 0x00A58792),
         (53, "save block flags, inside [esi+]", 0x77FC), (58, "last frame's save flags", 0x00A58798)],
        0x0042B6DE),
    "countdown bonus": (
        "8B 44 24 08 66 01 05 ?? ?? ?? ?? 66 01 05 ?? ?? ?? ?? 66 8B 44 24 04 66 01 05 ?? ?? ?? ?? A0 ?? ?? ?? ?? "
        "84 C0 75 07 C6 05 ?? ?? ?? ?? 5A C3",
        [(7, "countdown (seconds)", 0x00A61FC8), (14, "bonus seconds shown", 0x00A5C878),
         (26, "Mercenaries takings", 0x00A5C87A)],
        0x00455C40),
    "radar run": (
        "53 56 8B 74 24 0C 57 8A 5E ?? 8B C3 25 FF 00 00 00 83 F8 ?? 0F 87 ?? ?? ?? ?? FF 24 85 ?? ?? ?? ??",
        [(9, "b:the radar's own state", 1), (19, "b:its last state", 9), (29, "c:its states' table", 0x0045F398)],
        0x0045F160),
    "missile far": (
        "F7 05 ?? ?? ?? ?? 00 00 00 04 75 09 C6 46 04 00 E9 ?? ?? ?? ?? 56 C6 46 04 03 E8 ?? ?? ?? ?? 8B 7E ?? 83 C4 04 "
        "81 FF ?? ?? ?? ?? 0F 8D",
        [(2, "save block flags", 0x00A61F5C), (27, "r:missile step", 0x0045F460), (33, "b:missile's distance", 0x10),
         (39, "close in under", 0x73B90)],
        0x0045F234),
    "missile near": (
        "F7 05 ?? ?? ?? ?? 00 00 00 04 75 09 C6 46 04 00 E9 ?? ?? ?? ?? B3 03 56 88 5E 04 E8 ?? ?? ?? ?? 8B 46 ?? 33 FF "
        "83 C4 04 3B C7 7F 7F 89 7E ?? A0 ?? ?? ?? ?? 84 C0 75 73 A0 ?? ?? ?? ?? 84 C0 75 6A 57 E8 ?? ?? ?? ?? 83 C4 04 "
        "85 C0 75 5D 6A 01 E8 ?? ?? ?? ?? 83 C4 04 85 C0 75 4F A0 ?? ?? ?? ?? 84 C0 74 06 88 1D ?? ?? ?? ?? "
        "8B 0D ?? ?? ?? ?? A1 ?? ?? ?? ?? 81 E1 FF FF FF F9 80 CC 02 89 0D ?? ?? ?? ?? A3 ?? ?? ?? ??",
        [(2, "save block flags", 0x00A61F5C), (109, "save block flags (again)", 0x00A61F5C),
         (129, "save block flags (again)", 0x00A61F5C), (114, "state flags", 0x00A5C880),
         (134, "state flags (again)", 0x00A5C880), (28, "r:missile step", 0x0045F460),
         (34, "b:missile's distance", 0x10), (46, "b:missile's distance (again)", 0x10),
         (93, "voice state", 0x00A59E51), (103, "voice state (again)", 0x00A59E51), (81, "r:Task_state", 0x0042B0C0)],
        0x0045F2D0),
    "missile step": (
        "A1 ?? ?? ?? ?? F6 C4 04 75 4C F7 05 ?? ?? ?? ?? 00 00 00 10 74 19 A0 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 02 84 C0 74 09 "
        "A0 ?? ?? ?? ?? 84 C0 75 27 8B 44 24 04 8B 48 ?? 66 83 40 ?? C0 81 C1 ?? ?? ?? ?? 89 48 ??",
        [(1, "state flags", 0x00A5C880), (12, "save block flags", 0x00A61F5C), (53, "b:missile's distance", 0x10),
         (67, "b:missile's distance (again)", 0x10), (61, "the step a frame, negated", 0xFFFFFEF9),
         (57, "b:the sweep's angle", 0x0C)],
        0x0045F460),
    "results tally": (
        "0F BF 05 ?? ?? ?? ?? 99 2B C2 D1 F8 66 03 46 18 66 03 05 ?? ?? ?? ?? 66 89 46 12 75 04 C6 46 01 07 "
        "66 C7 46 06 00 00 5F 5E C3 A1 ?? ?? ?? ?? BF ?? ?? ?? ?? 40 3B C7 A3 ?? ?? ?? ?? 76 11 B9 01 00 00 00 "
        "89 3D ?? ?? ?? ??",
        [(3, "countdown (seconds)", 0x00A61FC8), (19, "Mercenaries takings", 0x00A5C87A),
         (43, "Mercenaries bank", 0x00A6261C), (56, "Mercenaries bank (again)", 0x00A6261C),
         (69, "Mercenaries bank (again)", 0x00A6261C), (48, "the bank's cap", 999999)],
        0x00444503),
    "half tally": (
        "66 A1 ?? ?? ?? ?? 66 D1 E8 66 89 46 12 75 04 C6 46 01 07 66 C7 46 06 00 00 5F 5E C3 A1 ?? ?? ?? ?? "
        "BF ?? ?? ?? ?? 40 BA 01 00 00 00 3B C7 A3 ?? ?? ?? ?? 76 0A 89 3D ?? ?? ?? ??",
        [(2, "Mercenaries takings", 0x00A5C87A), (29, "Mercenaries bank", 0x00A6261C),
         (47, "Mercenaries bank (again)", 0x00A6261C), (55, "Mercenaries bank (again)", 0x00A6261C),
         (34, "the bank's cap", 999999)],
        0x0044532B),
    "mix dispatch": (
        "8A 01 89 4C 24 24 83 E8 ?? 0F 84 ?? ?? ?? ?? 83 E8 ?? 0F 84 ?? ?? ?? ??",
        [(8, "b:the reloading tool's combination type", 2), (17, "b:grenade mix's type, less that", 3),
         (11, "r:the reloading tool's batch", 0x004DAF1C), (20, "r:powder into grenade rounds", 0x004DADFD)],
        0x004DAD2E),
    "mix yield": (
        "8A 56 6A 8A 41 04 88 54 24 18 88 46 65 8B 6C 24 18 A1 ?? ?? ?? ?? 81 E5 FF 00 00 00 C6 44 24 10 00 "
        "66 8B BC 68 ?? ?? 00 00 8D 54 AD 00 8B 44 24 10 8B D8 81 E3 FF 00 00 00 03 DA 66 0F B6 1C 9D ?? ?? ?? ?? "
        "66 3B DF 73 0E FE C0 3C ?? 88 44 24 10 72 DA 8B 44 24 10 25 FF 00 00 00 03 C2 8A 14 85 ?? ?? ?? ?? "
        "8A 41 03 3C ?? 88 54 24 10 72 2C 3C ?? 77 28 8B 44 24 10 25 FF 00 00 00 83 F8 07 77 1A FF 24 85 ?? ?? ?? ?? "
        "C6 44 24 10 ?? EB 0C C6 44 24 10 ?? EB 05 C6 44 24 10 ??",
        [(18, "inventory pointer", 0x00A623E4), (37, "batch counts, inside the block", 0x12C),
         (64, "level table", 0x0052B330), (97, "level table, bonus column", 0x0052B331),
         (76, "b:last level", 4), (105, "b:enhanced rounds, first id", 0x1E), (113, "b:enhanced rounds, last id", 0x1F),
         (133, "c:enhanced bonus switch", 0x004DB644), (141, "b:enhanced bonus, first case", 1),
         (148, "b:enhanced bonus, second case", 3), (155, "b:enhanced bonus, third case", 5)],
        0x004DAF1C),
    "mix count": (
        "6A ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 08 85 C0 74 08 8A 56 65 D0 E2 88 56 65 A1 ?? ?? ?? ?? 8B 54 24 24 "
        "66 FF 84 68 ?? ?? 00 00",
        [(1, "b:Easy's bit (from the top)", 0x17), (3, "state flags", 0x00A5C880), (8, "r:bit test", 0x00426AB0),
         (28, "inventory pointer", 0x00A623E4), (40, "batch counts, inside the block", 0x12C)],
        0x004DB003),
    "enhanced ammo offer": (
        "8A 0D ?? ?? ?? ?? 33 FF 80 F9 FF 74 1D 33 D2 8A 50 03 B8 ?? ?? ?? ?? 0F BE C9 3B CA 74 0C 8A 48 02 83 C0 02 47 "
        "80 F9 FF 75 ED 8A 04 7D ?? ?? ?? ?? 3C ?? 88 46 6A 88 44 24 10 0F 87 ?? ?? ?? ?? 8B 54 24 10 A1 ?? ?? ?? ?? "
        "81 E2 FF 00 00 00 66 83 BC 50 ?? ?? 00 00 ?? 0F 86",
        [(2, "kind table", 0x00530D08), (19, "kind table (again)", 0x00530D08), (45, "kind table, kind column", 0x00530D09),
         (50, "b:last kind with enhanced rounds", 1), (69, "inventory pointer", 0x00A623E4),
         (83, "batch counts, inside the block", 0x12C), (87, "b:enhanced rounds after this many", 6)],
        0x004DAB0A),
    "grenade mix": (
        "8A 51 04 88 56 65 A1 ?? ?? ?? ?? 32 D2 66 8B B8 ?? ?? 00 00 88 54 24 10 8B 44 24 10 8B D8 81 E3 FF 00 00 00 "
        "66 0F B6 1C 9D ?? ?? ?? ?? 66 3B DF 73 0F FE C2 80 FA ?? 88 54 24 10 72 DB 8B 44 24 10 8A 49 04 25 FF 00 00 00 "
        "33 D2 6A ?? 8A 14 85 ?? ?? ?? ?? 8B C1 25 FF 00 00 00 68 ?? ?? ?? ?? 0F AF D0 B8 67 66 66 66 F7 EA C1 FA 02 "
        "8B C2 C1 E8 1F 03 D0 02 D1 88 56 65 E8 ?? ?? ?? ?? 83 C4 08 85 C0 74 08 8A 4E 65 D0 E1 88 4E 65 A1 ?? ?? ?? ?? "
        "66 FF 80 ?? ?? 00 00",
        [(7, "inventory pointer", 0x00A623E4), (142, "inventory pointer (again)", 0x00A623E4),
         (16, "grenade batch count, in the block", 0x132), (149, "grenade batch count (again)", 0x132),
         (41, "level table, grenade row", 0x0052B36C), (80, "grenade row, bonus column", 0x0052B36D),
         (54, "b:last level", 4), (76, "b:Easy's bit (from the top)", 0x17), (92, "state flags", 0x00A5C880),
         (122, "r:bit test", 0x00426AB0)],
        0x004DADFD),
    "bit test": (
        "8B 54 24 08 B8 00 00 00 80 8B CA 56 83 E1 1F D3 E8 8B 4C 24 08 C1 EA 05 8B 34 91 23 C6 5E C3",
        [],
        0x00426AB0),
    "combine lookup": (
        "8A 0D ?? ?? ?? ?? 53 80 F9 FF B8 ?? ?? ?? ?? 74 25 8A 54 24 10 8A 5C 24 0C 3A CB 75 05 38 50 02 74 17 3A CA "
        "75 05 38 58 02 74 0E 8A 48 09 83 C0 08 80 F9 FF 75 E3 83 C8 FF 5B C3",
        [(11, "combination table", 0x0052AF00), (2, "combination table + 1", 0x0052AF01)],
        0x004DCB50),
    "healing": (
        "8A 46 36 84 C0 74 09 66 81 25 ?? ?? ?? ?? FF FD 8B 6E 18 66 C7 46 32 E0 FF 66 0F B6 4E 35 "
        "81 E5 FF FF 7F FF 89 6E 18 66 A1 ?? ?? ?? ?? 8B 2D ?? ?? ?? ?? 66 03 C1 33 C9 66 A3 ?? ?? ?? ?? "
        "8A 4E 35 03 E9 66 8B 0D ?? ?? ?? ?? 66 3B C1 89 2D ?? ?? ?? ?? 7E 07 66 89 0D ?? ?? ?? ??",
        [(10, "condition word", 0x00A5CCC6), (41, "player HP", 0x00A5CCC0), (58, "player HP (again)", 0x00A5CCC0),
         (88, "player HP (again)", 0x00A5CCC0), (70, "player max HP", 0x00A5CCC2),
         (47, "recovery total", 0x00A625FC), (79, "recovery total (again)", 0x00A625FC)],
        0x004DA817),
    "rank: recovery": (
        "8B 15 ?? ?? ?? ?? 33 C0 " + "81 FA ?? ?? ?? ?? 77 05 B8 ?? 00 00 00 " * len(RANK_STEPS) +
        "03 C1 03 F0 8B 44 24 10 83 FE ?? C6 40 04 00",
        [(2, "recovery total", 0x00A625FC)] +
        [check for i, (most, score) in enumerate(RANK_STEPS)
         for check in ((10 + 13 * i, f"step {i + 1}: a total up to", most), (17 + 13 * i, f"b:step {i + 1}: scores", score))],
        0x00446182),
    # The clear count and the epilogues.
    "ending's epilogue": (
        "A1 ?? ?? ?? ?? F6 C4 ?? 75 24 66 83 3D ?? ?? ?? ?? ?? 73 1A 6A 08 6A 01 C6 05 ?? ?? ?? ?? 05 E8 ?? ?? ?? ?? "
        "6A 01 E8 ?? ?? ?? ?? 83 C4 0C 6A 01 68 FF FF FF 00 68 FF FF FF 00 6A 02 6A 02 6A 00 6A 00 E8 ?? ?? ?? ?? "
        "E8 ?? ?? ?? ??",
        [(1, "state flags", 0x00A5C880), (7, "b:Easy, in the flags' second byte", 0x01),
         (13, "clear count", 0x00A61F6E), (17, "b:epilogues", 8), (26, "results screen", 0x00A5C875),
         (32, "r:overlay launch", 0x0042AF30), (39, "r:Task_sleep", 0x0042AFB0), (72, "r:clear count +1", 0x00442EC0)],
        0x00442D97),
    "clear count +1": (
        "A0 ?? ?? ?? ?? 68 00 00 10 80 C6 05 ?? ?? ?? ?? FF C6 05 ?? ?? ?? ?? 1C A2 ?? ?? ?? ?? E8 ?? ?? ?? ?? 50 E8 "
        "?? ?? ?? ?? 83 C4 08 66 FF 05 ?? ?? ?? ?? C3",
        [(12, "save count", 0x00A61F70), (19, "the save's location", 0x00A61F73), (46, "clear count", 0x00A61F6E)],
        0x00442EC0),
    "epilogue menu": (
        "51 66 A1 ?? ?? ?? ?? 66 89 44 24 00 66 3D ?? 00 B8 ?? 00 00 00 73 04 8B 44 24 00 8B 0D ?? ?? ?? ??",
        [(3, "clear count", 0x00A61F6E), (14, "b:epilogues", 8), (17, "b:epilogues (again)", 8),
         (29, "buttons pressed", 0x00A5C868)],
        0x004426B0),
    # Jill's outfit: the room load's model test and the load it jumps past, the
    # model loader, and the title's outfit grid.
    "player model test": (
        "A0 ?? ?? ?? ?? 66 8B 0D ?? ?? ?? ?? 66 33 D2 8A D0 66 3B D1 0F 84 ?? ?? ?? ?? 66 83 F9 ?? BF C8 00 00 00 7C ?? "
        "3C ?? 73 ?? 66 89 3D ?? ?? ?? ?? 66 89 1D ?? ?? ?? ?? 66 89 15 ?? ?? ?? ??",
        [(1, "player model's type byte", 0x00A5CC3E), (8, "character to be in play", 0x00A61FB6),
         (29, "b:first character not Jill", 8), (38, "b:first character not Jill (again)", 8),
         (44, "player HP", 0x00A5CCC0), (51, "condition word", 0x00A5CCC6),
         (58, "character kept for Jill", 0x00A61FCC), (22, "r:past the model load", 0x00468731)],
        0x00468602),
    "player model reload": (
        "0F BF 05 ?? ?? ?? ?? 8A 15 ?? ?? ?? ?? 83 C4 10 88 15 ?? ?? ?? ?? 66 89 3D ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 68 ?? ?? "
        "?? ?? E8 ?? ?? ?? ?? 83 C4 08 89 1D ?? ?? ?? ?? E8",
        [(3, "character to be in play", 0x00A61FB6), (9, "character (again)", 0x00A61FB6),
         (18, "player model's type byte", 0x00A5CC3E), (25, "player max HP", 0x00A5CCC2),
         (31, "r:inventory select", 0x0045EC80), (36, "player entity", 0x00A5CBF4),
         (41, "r:player model loader", 0x0045F880), (50, "player state", 0x00A5CBF8)],
        0x004686FB),
    "player model loader": (
        "B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 53 55 56 57 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B B4 24 ?? ?? ?? ?? B3 30 8B E8 88 1D ?? ?? "
        "?? ?? 88 1D ?? ?? ?? ?? 8A 46 4A 02 C3 83 C4 04 A2 ?? ?? ?? ?? 8A 46 4A 81 ED ?? ?? ?? ?? 3C 09 76 07 04 57 A2 ?? ?? "
        "?? ?? E8 ?? ?? ?? ?? 8A 4E 4A 89 44 24 ?? 84 C9 74 05 80 F9 01 75 ?? 8D 48 FF 83 F9 ?? 77 ?? FF 24 8D ?? ?? ?? ?? "
        "04 31 A2 ?? ?? ?? ?? EB ?? 8D 4C 24 ?? 51 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 08 88 1D ?? ?? ?? ?? EB ?? 8D 54 24 "
        "?? 52 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 08 C6 05 ?? ?? ?? ?? 32 55 68 ?? ?? ?? ?? E8",
        [(178, "model file name", 0x0052A1F0), (37, "its first digit", 0x0052A201),
         (43, "its second digit", 0x0052A202), (56, "second digit (again)", 0x0052A202),
         (76, "second digit (again)", 0x0052A202), (119, "second digit (again)", 0x0052A202),
         (145, "second digit (again)", 0x0052A202), (171, "second digit (again)", 0x0052A202),
         (81, "r:outfit getter", 0x004254D0), (106, "b:outfits less two", 6),
         (112, "c:outfit case table", 0x0045FB50), (131, "texture name, outfit 6", 0x0052A5E8),
         (157, "texture name, outfit 7", 0x0052A5CC), (136, "r:file load", 0x004264E0),
         (162, "r:file load (again)", 0x004264E0), (183, "r:file load (again)", 0x004264E0)],
        0x0045F880),
    "outfit grid": (
        "0F BE C8 8B 14 8D ?? ?? ?? ?? 52 E8 ?? ?? ?? ?? 53 56 88 5E 0F E8",
        [(6, "outfit of each cell", 0x005114C4), (12, "r:outfit setter", 0x004254C0)],
        0x0045148D),
    # The memory card screen and the memory cards (the save file manager).
    "card screen entry": (
        "6A 00 E8 ?? ?? ?? ?? 83 C4 04 83 F8 01 75 1B 6A 00 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 00 E8 ?? ?? ?? ?? 83 C4 08 E9 "
        "?? ?? ?? ??",
        [(3, "r:Task_state", 0x0042B0C0), (23, "r:card screen loop", 0x0044CEF0)],
        0x0044CEC0),
    "card screen loop": (
        "53 56 8B 35 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 83 C4 04 33 DB 88 5E 1A 53 53 68 FF 00 00 00 E8 ?? ?? ?? ?? 88 5E 11 "
        "66 A1 ?? ?? ?? ?? 66 89 46 1C 66 8B 0D ?? ?? ?? ?? 56 66 89 4E 1E E8 ?? ?? ?? ?? 83 C4 10 33 D2 56 8A 16 88 5E 16 "
        "FF 14 95 ?? ?? ?? ??",
        [(4, "card screen pointer", 0x00528DB8), (39, "buttons pressed", 0x00A5C868), (50, "buttons held", 0x00A5C85C),
         (78, "card screen state handlers", 0x00528DFC)],
        0x0044CEF0),
    "card screen init": (
        "6A 41 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 68 8C 01 00 00 68 ?? ?? ?? ?? E8 "
        "?? ?? ?? ?? 6A 16 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 28 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 68 C0 12 00 00 68 ?? ?? ?? ?? "
        "E8 ?? ?? ?? ?? 6A 50 68 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 00 00 00 00 E8 ?? ?? ?? ?? 6A 50 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
        "83 C4 40 C7 05 ?? ?? ?? ?? ?? ?? ?? ?? C3",
        [(72, "card screen state", 0x00A3FEA0), (120, "card screen pointer", 0x00528DB8),
         (124, "card screen state (again)", 0x00A3FEA0)],
        0x0044CE30),
    "card list rescan": (
        "53 56 8B 74 24 0C 33 C0 32 DB 8A 46 0E 88 5E 07 50 88 5E 13 E8 ?? ?? ?? ?? 83 C4 04 3A C3 88 46 0F 74 1E 76 0F 3C "
        "02 76 0E 3C 03 75 07 C6 06 03 C6 46 0F 01 5E 5B C3 C6 06 03 C6 46 0F 04 5E 5B C3 33 C9 8A 4E 0E 51 E8 ?? ?? ?? ?? "
        "8D 56 34 68 8C 01 00 00 52 88 46 0F E8 ?? ?? ?? ?? 8A 46 0F 83 C4 0C 3A C3 75 18 8D 46 20 50 E8 ?? ?? ?? ??",
        [(108, "r:card read", 0x0044EDD0)],
        0x0044D470),
    "card list": (
        "56 8B 74 24 08 8A 46 04 C6 46 07 0E A2 ?? ?? ?? ?? A1 ?? ?? ?? ?? F6 C4 10 75 49 8B 0D ?? ?? ?? ?? F6 C5 08 75 3E "
        "F6 C4 20 74 46",
        [(18, "buttons pressed", 0x00A5C868), (29, "buttons held", 0x00A5C85C)],
        0x0044D5A0),
    "card read": (
        "53 55 56 57 8B 3D ?? ?? ?? ?? 33 C9 68 ?? ?? ?? ?? 8A 4F 0E 8D 47 34 50 51 E8 ?? ?? ?? ?? 8B 54 24 20 52 E8 ?? ?? ?? "
        "?? 8A 87 7F 01 00 00 83 C4 10 84 C0 76 62 33 DB 8D AF ?? ?? 00 00 8B 44 24 14 8A 04 03 A8 C0 75 3C 25 FF 00 00 00 "
        "68 ?? ?? 00 00 8D B7 C0 03 00 00 68 ?? 00 00 00 8D 0C 80 8D 14 48 8D 44 57 34 50 56 E8 ?? ?? ?? ?? 83 C4 10 85 C0 "
        "75 22 68 ?? 00 00 00 56 55 E8 ?? ?? ?? ?? 83 C4 0C 43 81 C5 ?? 00 00 00 83 FB ?? 72 AD",
        [(6, "card screen pointer", 0x00528DB8), (36, "r:card slots", 0x0044EF70), (57, "w:the list's copies", 0xB40),
         (78, "w:a save's data, in its block", 0x200), (89, "b:bytes read of it", 0x80), (118, "b:...copied", 0x80),
         (135, "b:...apart", 0x80), (141, "b:slots", 15)],
        0x0044EDD0),
    "card slots": (
        "53 55 8B 6C 24 0C 56 8B 35 ?? ?? ?? ?? 8B CD B8 80 80 80 80 33 DB 89 01 89 41 04 89 41 08 66 89 41 0C 88 41 0E "
        "8A 86 7E 01 00 00 33 C9 C6 86 7F 01 00 00 00 84 C0 89 4C 24 10 76 59 57 8D 7E 40 6A 0C 8D 57 F4 68 ?? ?? ?? ?? 52 "
        "E8 ?? ?? ?? ?? 83 C4 0C 85 C0 75 25 8A 07 33 C9 8A 4F 01 8B D5 8D 04 80 03 D1 88 9C 42 F0 FD FF FF",
        [(9, "card screen pointer", 0x00528DB8), (70, "save name prefix", 0x00528EEC)],
        0x0044EF70),
    "card row": (
        "8A 44 2E ?? 83 C4 20 3C ?? 0F 83 ?? ?? ?? ?? 8B CD 6A 0A C1 E1 07 66 83 BC 31 ?? ?? 00 00 64 8D BC 31 ?? ?? 00 00 "
        "73 26 33 D2 66 8B 15 ?? ?? ?? ?? 81 C2 ?? ?? ?? ?? 52 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 0C C6 05 ?? ?? ?? ?? FE "
        "EB 1D A1 ?? ?? ?? ?? 25 FF FF 00 00 05 ?? ?? ?? ?? 50 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 0C 33 C9 B8 67 66 66 66 "
        "8A 4F ?? 6A 18 F7 E9 A0 ?? ?? ?? ?? C1 FA 02 8B CA C1 E9 1F 03 D1 B9 0A 00 00 00 02 C2 A2 ?? ?? ?? ?? 33 C0 8A 47 "
        "?? 99 F7 F9 8A 0D ?? ?? ?? ?? 33 C0 02 CA 33 D2 88 0D ?? ?? ?? ?? 8A 57 ?? 66 8B 04 55 ?? ?? ?? ?? 05 ?? ?? ?? ?? "
        "50 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C7 ?? 6A ?? 57 E8 ?? ?? ?? ??",
        [(3, "b:slot map", 0x20), (8, "b:slots", 15), (26, "w:clears, in the copies", 0xB56),
         (34, "w:the list's copies", 0xB40), (116, "b:save count", 0x18), (152, "b:save count (again)", 0x18),
         (176, "b:location", 0x1B), (181, "location names", 0x00528D62), (186, "the game's messages", 0x005287C8),
         (203, "b:flags", 0x08), (205, "b:Easy's bit", 0x1F), (208, "r:bit test", 0x00426AB0)],
        0x0044E06A),
    "card maker": (
        "68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B F8 83 C4 04 89 7C 24 10 3B FB C7 84 24 ?? ?? ?? ?? 06 00 00 00 74 4A 89 6F 04 89 "
        "5F 08 C7 07 ?? ?? ?? ?? 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? 6A ?? 8D 57 ?? 68 ?? ?? ?? ?? 52 C6 84 24 ?? ?? ?? ?? 07 E8 "
        "?? ?? ?? ?? C7 07 ?? ?? ?? ?? C7 84 24 ?? ?? ?? ?? FF FF FF FF 89 BE ?? ?? 00 00",
        [(1, "container size", 0x40AC4), (47, "c:card destructor", 0x004141D0), (52, "c:card constructor", 0x00414190),
         (57, "b:cards", 2), (60, "b:first card", 0x0C), (62, "card object size", 0x2055C),
         (82, "container vtable", 0x0050A760), (99, "w:app field", 0xFC)],
        0x00403CA4),
    "card global": (
        "8B 8E E4 00 00 00 89 0D ?? ?? ?? ?? 8B 96 FC 00 00 00 89 15 ?? ?? ?? ?? 8B 86 E8 00 00 00 A3 ?? ?? ?? ??",
        [(14, "app field", 0xFC), (20, "card container pointer", 0x005349D4)],
        0x00403EA1),
    "card constructor": (
        "56 8B F1 E8 ?? ?? ?? ?? C7 06 ?? ?? ?? ?? C6 86 ?? ?? ?? ?? 00 8B C6 5E C3",
        [(10, "card vtable", 0x0050AE34), (16, "card folder", 0x2015C)],
        0x00414190),
    "card flush": (
        "81 EC 80 04 00 00 33 C0 8D 54 24 00 53 55 8B D9 56 57 83 C9 FF 8D BB ?? ?? ?? ?? F2 AE F7 D1 2B F9 8B C1 8B F7 8B "
        "FA 8B 13 C1 E9 02 F3 A5 8B C8 83 E1 03 F3 A4 8B CB FF 52 ?? 8B F8 83 C9 FF 33 C0 8D 54 24 10 F2 AE F7 D1 2B F9 8B "
        "F7 8B E9 8B FA 83 C9 FF F2 AE 8B CD 4F C1 E9 02 F3 A5 8B CD 83 E1 03 F3 A4 8D 7C 24 10 83 C9 FF F2 AE F7 D1 49 83 "
        "F9 02 72 62 8D 7C 24 10 83 C9 FF F2 AE F7 D1 49 BF ?? ?? ?? ?? 68 ?? ?? ?? ?? 8D 54 0C 13 83 C9 FF F2 AE F7 D1 2B "
        "F9 8B C1 8B F7 8B FA C1 E9 02 F3 A5 8B C8 83 E1 03 F3 A4 8D 4C 24 14 51 E8 ?? ?? ?? ?? 8B F0 83 C4 08 85 F6 74 1A "
        "56 68 ?? ?? ?? ?? 83 C3 ?? 6A 01 53 E8 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 83 C4 14",
        [(23, "card folder", 0x2015C), (57, "b:card name's virtual", 0x10), (131, "file extension", 0x00514950),
         (136, "fopen mode", 0x00514958), (192, "card image size", 0x20000), (198, "b:card image", 0x18)],
        0x004143D0),
    "card create": (
        "B8 01 00 00 00 81 C1 ?? ?? ?? ?? 83 39 00 74 0F 40 81 C1 80 00 00 00 83 F8 10 7C EF 83 C8 FF C3 56 57 8B F1 E8 D7 "
        "FF FF FF 8B F8 85 FF 7D 08 5F 83 C8 FF 5E C2 04 00 8B 4C 24 0C 8B C7 C1 E0 07 6A 15 51 8D 74 30 ?? 8D 56 0A 52 E8 "
        "?? ?? ?? ?? 83 C4 0C 8B C7 C7 06 51 00 00 00 C7 46 04 00 20 00 00 66 C7 46 08 FF FF 5F 5E C2 04 00",
        [(7, "directory entry 1's state", 0x98), (70, "b:card image", 0x18)],
        0x00414620),
}

# Code that is checked where another signature's operand says it is (its
# pattern alone matches more than once, or the mod only ever looks there):
# (label, the signature whose operand names it, the operand's offset - "r:n"
# for a call's rel32 at n -, the pattern, [(offset, label, expected)] as in SIGS).
AT = [
    ("card destructor", "card maker", 47,
     "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 51 56 8B F1 89 74 24 04 C7 06 ?? ?? ?? ?? C7 44 24 "
     "10 00 00 00 00 E8 ?? ?? ?? ??",
     [(31, "card vtable", 0x0050AE34), (44, "r:card flush", 0x004143D0)]),
    ("radar", "countdown tick", "r:32",
     "A1 ?? ?? ?? ?? 68 ?? ?? ?? ?? 25 FF 00 00 00 FF 14 85 ?? ?? ?? ?? 59 C3",
     [(1, "radar's state byte", 0x00A35B60), (6, "radar's struct", 0x00A35B60), (18, "radar's handlers", 0x0052A1A8)]),
    ("outfit getter", "player model loader", "r:81", "A1 ?? ?? ?? ?? C3", [(1, "outfit", 0x00A6262C)]),
    ("outfit setter", "outfit grid", "r:12", "8B 44 24 04 A3 ?? ?? ?? ?? C3", [(5, "outfit", 0x00A6262C)]),
]

# Where the model loader's switch on (outfit - 1) lands, from its match: outfits
# 1..5 load another model, 6 and 7 the regular and the biker one with a texture
# over them (game.cpp's kOutfitCase*, and the order its names are in).
OUTFIT_CASES = [116, 116, 116, 116, 116, 125, 151]
OUTFITS = 8
# The outfit each cell of the title's costume grid gives: its labels (in
# data_au/etc2/titleu03.tim) are SPECIAL 1..7, then NORMAL - game.cpp's names.
OUTFIT_CELLS = [1, 2, 3, 4, 5, 6, 7, 0]

# The patch sites, as offsets from their signature's match: what must be there.
PATCHES = [
    ("save", 82, "FE 05 ?? ?? ?? ??", "save count +1"),
    ("save", 8, "75 20", "the save's ink ribbon"),
    ("typewriter", 16, "7C 1A", "the typewriter's ribbon check"),
    ("button state stores", 0, "89 2D ?? ?? ?? ?? 89 15 ?? ?? ?? ?? 89 3D ?? ?? ?? ??", "button state stores"),
    ("button repeat store", 0, "89 3D ?? ?? ?? ??", "button repeat store"),
    ("countdown tick", 88, "66 FF 8E ?? ?? ?? ??", "the countdown's second"),
    ("countdown frames", 11, "75 26", "the countdown's frame count"),
    ("missile step", 65, "89 48 ??", "the missile's step"),
    ("player damage", 16, "66 2B C8", "the player's damage"),
    ("healing", 77, "89 2D ?? ?? ?? ??", "the recovery total's store"),
    ("play time", 59, "89 15 ?? ?? ?? ??", "the save's play-time add"),
    ("ending's play time", 25, "89 15 ?? ?? ?? ??", "the ending's play-time add"),
    ("player model test", 20, "0F 84 ?? ?? ?? ??", "the room load's model test"),
]


def sections(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = pe + 24
    opt_sz = struct.unpack_from("<H", d, pe + 20)[0]
    out = []
    for i in range(nsec):
        o = opt + opt_sz + i * 40
        vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, o + 8)
        out.append((va, max(vsz, rsz), rptr))
    return out


def read_va(d, secs, va, n):
    """n bytes of the image at a VA, or None outside every section."""
    for sva, size, rptr in secs:
        o = va - IMAGE_BASE - sva
        if 0 <= o and o + n <= size:
            return d[rptr + o:rptr + o + n]
    return None


def compile_pattern(p):
    out = []
    for tok in p.split():
        out.append(None if tok.startswith("?") else int(tok, 16))
    return out


def find_all(buf, pat, limit=8):
    n = len(pat)
    hits = []
    first = pat[0]
    start = 0
    while len(hits) < limit:
        i = buf.find(bytes([first]), start)
        if i < 0 or i + n > len(buf):
            break
        start = i + 1
        for j in range(1, n):
            if pat[j] is not None and buf[i + j] != pat[j]:
                break
        else:
            hits.append(i)
    return hits


def operands(text, base, i, checks, any_build, line):
    """Read each operand of a match at text[i] into `line`; the number that are not as expected."""
    bad = 0
    for off, label, want in checks:
        if label.startswith("b:"):
            got, label = text[i + off], label[2:]
        elif label.startswith("r:"):
            got, label = (base + i + off + 4 + struct.unpack_from("<i", text, i + off)[0]), label[2:]
            if any_build:
                want = None
        elif label.startswith("w:"):
            got, label = struct.unpack_from("<H", text, i + off)[0], label[2:]
        elif label.startswith("c:"):
            got, label = struct.unpack_from("<I", text, i + off)[0], label[2:]
            if any_build:
                want = None
        else:
            got = struct.unpack_from("<I", text, i + off)[0]
        mark = ""
        if want is not None and got != want:
            mark = f"  <-- expected {want:#x}"
            bad += 1
        line.append(f"       +{off:<3d} {label:<32s} {got:#x}{mark}")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump", default="ghidra_proj/ResidentEvil3.fixed.exe")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--any-build", action="store_true",
                    help="another exe's dump: code addresses are printed, not checked")
    a = ap.parse_args()
    d = open(a.dump, "rb").read()
    secs = sections(d)
    text_va, text_sz, text_off = secs[0]
    text = d[text_off:text_off + text_sz]
    base = IMAGE_BASE + text_va

    bad = 0
    found = {}
    for name, (pattern, checks, expect_va) in SIGS.items():
        pat = compile_pattern(pattern)
        hits = find_all(text, pat)
        if len(hits) != 1:
            print(f"FAIL {name}: {len(hits)} matches (want exactly 1)")
            bad += 1
            continue
        at = base + hits[0]
        found[name] = hits[0]
        if expect_va is not None and at != expect_va and not a.any_build:
            print(f"FAIL {name}: matched at {at:#x}, expected {expect_va:#x}")
            bad += 1
            continue
        line = [f"ok   {name} @ {at:#x}"]
        n_bad = operands(text, base, hits[0], checks, a.any_build, line)
        bad += n_bad
        if not a.quiet or n_bad:
            print("\n".join(line))

    for label, sig, off, pattern, checks in AT:
        if sig not in found:
            print(f"FAIL {label}: '{sig}' did not match, so it was not looked for")
            bad += 1
            continue
        if isinstance(off, str):  # "r:n": where the rel32 at n lands
            n = found[sig] + int(off[2:])
            at = base + n + 4 + struct.unpack_from("<i", text, n)[0]
        else:
            at = struct.unpack_from("<I", text, found[sig] + off)[0]
        i = at - base
        pat = compile_pattern(pattern)
        if i < 0 or i + len(pat) > len(text) or any(b is not None and text[i + j] != b for j, b in enumerate(pat)):
            print(f"FAIL {label}: '{sig}' names {at:#x}, which does not hold it")
            bad += 1
            continue
        line = [f"ok   {label} @ {at:#x} (named by '{sig}')"]
        n_bad = operands(text, base, i, checks, a.any_build, line)
        bad += n_bad
        if not a.quiet or n_bad:
            print("\n".join(line))

    for sig, off, expect, label in PATCHES:
        if sig not in found:
            print(f"FAIL patch '{label}': its signature did not match")
            bad += 1
            continue
        pat = compile_pattern(expect)
        at = found[sig] + off
        for j, b in enumerate(pat):
            if b is not None and text[at + j] != b:
                print(f"FAIL patch '{label}' at {base + at:#x}: holds "
                      f"{' '.join('%02X' % c for c in text[at:at + len(pat)])}, expected {expect}")
                bad += 1
                break
        else:
            if not a.quiet:
                print(f"ok   patch '{label}' @ {base + at:#x}: "
                      f"{' '.join('%02X' % c for c in text[at:at + len(pat)])}")

    # Jill's outfit: the model test has to jump to where the load it skips ends,
    # the load has to call the loader, the loader's switch has to send each
    # outfit to the case game.cpp names it by, and the title's grid's cells have
    # to give the outfits their labels name.
    outfit_sigs = ("player model test", "player model reload", "player model loader", "outfit grid")
    if all(k in found for k in outfit_sigs):
        t, r, l, g = (found[k] for k in outfit_sigs)
        rel = lambda i: i + 4 + struct.unpack_from("<i", text, i)[0]
        problems = []
        if rel(t + 22) != r + 54:
            problems.append(f"the test jumps to {base + rel(t + 22):#x}, not to the load's end at {base + r + 54:#x}")
        if rel(r + 41) != l:
            problems.append(f"the load calls {base + rel(r + 41):#x}, not the loader at {base + l:#x}")
        table = struct.unpack_from("<I", text, l + 112)[0]
        raw = read_va(d, secs, table, 4 * len(OUTFIT_CASES))
        got = list(struct.unpack(f"<{len(OUTFIT_CASES)}I", raw)) if raw else []
        want = [base + l + off for off in OUTFIT_CASES]
        if got != want:
            problems.append(f"the loader's cases at {table:#x} are {[hex(x) for x in got]}, "
                            f"expected {[hex(x) for x in want]}")
        values = struct.unpack_from("<I", text, g + 6)[0]
        raw = read_va(d, secs, values, 4 * OUTFITS)
        cells = list(struct.unpack(f"<{OUTFITS}I", raw)) if raw else []
        if cells != OUTFIT_CELLS:
            problems.append(f"the grid's cells at {values:#x} give {cells}, not the outfits their labels name "
                            f"({OUTFIT_CELLS})")
        for p in problems:
            print(f"FAIL outfit: {p}")
        bad += len(problems)
        if not problems and not a.quiet:
            print(f"ok   outfit: the cases at {table:#x} and the grid's cells {cells} at {values:#x}")

    print(f"\n{len(SIGS)} signatures, {len(AT)} checked where they are named, {len(PATCHES)} patch sites: "
          f"{'all good' if not bad else str(bad) + ' PROBLEM(S)'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
