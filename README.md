# RE3-1999CabbyCodes

A cheat panel for **Resident Evil 3: Nemesis (1999)** — the classic PC version, as sold on Steam
(app `4249120`, *Resident Evil 3 Nemesis (1999)*) and GOG.

Open the game's **status screen** — the inventory screen you already use — and a panel appears
beside it with God mode, One hit kills, Infinite ammo, Freeze countdown timer, Infinite ink ribbons,
Save without counting, Freeze play time, Heal without counting, editors for the save count, the
play-time clock, the **recovery total** the ending ranks you on, the **clear count** the epilogues
unlock by, your **reloading tool proficiency** and your **Mercenaries money**, Jill's **outfit** (any of
the game's costumes, in the middle of a game), a full **inventory editor** (put any of the game's items into any of your
slots, set how many) and the game's own **item box**. Beside the game's **load and save screens** the panel
is a **save file manager** instead: delete a save, or copy one into another slot. And during any cutscene or
movie, **F8** (or **Start** on a pad) pauses the game and offers to **skip it**.

Nothing is patched on disk. The mod is a proxy `ddraw.dll` that sits in front of the DirectDraw →
Direct3D 9 wrapper the game already ships with; everything it touches in the game is found at run
time by byte signature, and every code patch is reverted when you switch the cheat off or close the
game.


## Install

1. Open your Resident Evil 3 folder, then its **`english`** folder — the one with
   `ResidentEvil3.exe` in it. On Steam that is usually
   `.../steamapps/common/4249120_Biohazard3/english`.
2. Rename the `ddraw.dll` that is already there to **`ddraw_orig.dll`**.
3. Copy this mod's `ddraw.dll` in beside it.

**On Windows that is all.**

**On Linux / Steam Deck (Proton)** you also need one launch option. Right-click the game in Steam →
*Properties* → *Launch Options*, and put this in:

```
WINEDLLOVERRIDES="ddraw=n,b" %command%
```

Proton loads a native `ddraw.dll` by itself for Resident Evil (1996), RE2 (1998), Dino Crisis and
Breath of Fire IV, but **not** for Resident Evil 3. Without that line Wine uses its own DirectDraw,
the game's wrapper is never loaded, and neither is the mod — you get the game exactly as before,
with no panel and nothing in the log.

**Uninstall:** delete `ddraw.dll` and rename `ddraw_orig.dll` back to `ddraw.dll`.

> Steam's *Verify integrity of game files* puts the stock `ddraw.dll` back and removes the mod.
> Just install it again.

## Use

Play as usual. Open the **status screen** (whichever key your controls have it on) and the panel is
there. It is a normal window: drag it by its title bar, and it remembers where you put it. It also comes
up beside the game's load and save screens, where it manages your save files (see
[Save files](#save-files)).

- **F7** hides and shows the panel while that screen is up.
- While the panel is up the mouse is free to leave the game for another window or monitor.
- Typing in one of the panel's boxes takes the keyboard and the pad away from the game until you
  click off it, so the game's own cursor does not move while you type.
- Everything above the inventory sits under a **Cheats** heading: click it to fold the cheats away
  and give the inventory and the item box the room. The heading still says how many are on, and the
  panel remembers whether it was folded.

**The Mercenaries** — the launcher's Mercenaries button, which starts `RE3_MERCE.exe` from the same
`english` folder — loads the mod too: open the status screen during a run and the panel is there.

### The cheats

| | what it does |
| --- | --- |
| **God mode** | No hit takes any health off you — a barrel going off at point blank included — and your health is held at its maximum and the poison that drains it is cured, every frame. You still flinch and get knocked about; deaths the story scripts itself are left to the game. |
| **One hit kills** | Every enemy in the room is held at 1 HP — the last value the game still counts as alive — so your next hit kills it. Deaths, drops and scripts all run through the game's own code. Nemesis included; boss fights written around a health bar may end sooner than they were meant to. |
| **Infinite ammo** | The weapon you have equipped is topped back up to a full load every frame, so firing never runs it dry. Reloads and pickups work as usual. |
| **Freeze countdown timer** | Any countdown the game is running stops where it is, hundredths and all, so the time never runs out: the Mercenaries' clock, and the main game's own — the hospital's time bomb, the four-minute one late in the Dead Factory, and the incoming missile on the radar receiver at the very end (its distance stops, so it never arrives). The Mercenaries' time bonuses still add to it, and it runs on again from there the moment you switch this off. |
| **Infinite ink ribbons** | Saving at a typewriter never uses up a ribbon, and you can save without carrying one at all — the typewriter offers the save as if you had one. |
| **Save without counting** | Saving no longer adds to the save count the ending ranks you on. |
| **Freeze play time** | The play-time clock stops where it is, and runs on again the moment you switch this off — the time it was frozen for never counts. A save you make while it is on keeps the time the panel shows; loading a save brings back that save's own time, and it stays frozen there. |
| **Heal without counting** | Healing items heal you as usual but no longer add to the recovery total the ending ranks you on. |
| **Skip cutscenes** | On by default. See below. |

Every switch is remembered: change one and it is written to the ini at once, so the cheats are as
you left them the next time you start the game.

### Skipping cutscenes

While a cutscene (the game's letterbox scenes) or a movie is playing, a small **F8: skip cutscene**
note shows in the corner for its first few seconds. Press **F8** — or **Start** on a game pad — and
the game pauses where it is, picture and sound, and asks whether to skip it:

- **Enter**, **Space** or **F8** again, **A** or **Start** on a pad, or the *Skip* button skips it;
- **Esc**, **B** on a pad, or *Keep watching* carries on exactly where it paused.

A movie ends there and then. A cutscene is **run through to its end unseen and unheard** rather
than cut: while the paused picture stays up with *Skipping the cutscene…*, the game plays the scene
out as fast as your computer allows, so every door, item, flag and room change it makes still
happens, exactly as if you had watched it. Lines of speech are cut short the way the game cuts them
itself, so a talky scene skips as quickly as a silent one; the music and background sound come back
on their own afterwards. If the scene puts a message or a choice in front of you, the skip shows
you the game and waits (a live selection is still yours to make), then carries on; if it opens the
status screen — the Mercenaries intro ends that way — the skip ends there and the screen is yours.
Most cutscenes
are over in a fraction of a second, and the play-time clock moves on by exactly as long as the scene
would have taken — not a second more, and nothing for the time spent paused.

**Saves**, **Play time** and **Recovery** also have editors: type a number and press *Apply* / *Set*.
All three are part of the save file, so they are kept when you save and come back when you load.

### The ending's rank

The results screen after the ending ranks you **A** to **F** (its *Grade*) on three scores of up to
100 points each — your saves, your play time and your recovery:

| points | saves | play time, up to | recovery, up to |
| --- | --- | --- | --- |
| 100 | 0 | 2:29:59 | 1,500 |
| 90 | 1 | 2:39:59 | 1,700 |
| 80 | 2 | 2:59:59 | 1,900 |
| 70 | 3 | 3:29:59 | 2,100 |
| 60 | 4–5 | 4:09:59 | 2,300 |
| 50 | 6–8 | 4:59:59 | 2,500 |
| 40 | 9–12 | 5:59:59 | 2,700 |
| 30 | 13–17 | 7:09:59 | 2,900 |
| 20 | 18–23 | 8:29:59 | 3,100 |
| 10 | 24–30 | 9:59:59 | 3,300 |
| 0 | more | longer | more |

Added up, 270 points or more is an **A**, 230 a **B**, 180 a **C**, 120 a **D**, 50 an **E**, and
anything less an **F**. On Easy the results show no rank at all.

**Recovery** is the healing you have used. Every healing item adds its *full* amount to it, however
much of it you actually needed: your whole health (200 for Jill) for a first aid spray or box, three
green herbs or any mix with a red herb; half of it for two green herbs and a quarter for one, blue
mixed in or not; nothing for a blue herb on its own. A new game starts it at 0. The panel shows the total and the points it is
worth beside its editor — in orange once it is costing you — and **Heal without counting** keeps it
where it is while your items go on healing you.

### Clears and the epilogues

The game counts how many times you have finished it. Each of your first eight endings shows a new
**epilogue**, and the title menu's **EPILOGUE** screen lets you read every one unlocked so far: one
for each clear, up to all eight.

The panel's **Clears & epilogues** section shows the count for the game you are playing and lets you
set it: type a number and press *Apply*. Beside it the panel says what the ending will do. The ending
adds one to the count and records it in `saves\data.sav`, which is where the EPILOGUE screen reads it
from, so the screen changes once you have finished the game:

- **7** before you finish: the ending shows the eighth and last epilogue, then one more picture,
  and the EPILOGUE screen has all eight from then on.
- **8 or more**: all eight are unlocked as well, but the ending shows none.
- On **Easy** the ending shows no epilogue at all, but the clear still counts.

The count is part of the save file, so it is kept when you save and comes back when you load. The
EPILOGUE screen always goes by the last ending's count, so finishing an older save that counts fewer
clears takes it back down. The count also decides Nemesis's seventh drop on Hard: an assault rifle on
your first playthrough, Inf. Bullets on every one after it. The Mercenaries has no ending to count,
so the section is not there.

### Outfits

The panel's **Outfit** list changes Jill's costume in the middle of a game. It offers the eight the game
lets you pick on its **COSTUME SELECT** screen before a new game, by the names that screen gives them:

| outfit | what it is |
| --- | --- |
| **Normal** | her tube top and skirt — with the backpack, once she has it |
| **Special 1** | biker leathers |
| **Special 2** | her S.T.A.R.S. uniform from the first game, beret and all |
| **Special 3** | a white trouser suit |
| **Special 4** | a police uniform |
| **Special 5** | Regina, from Dino Crisis |
| **Special 6** | Normal with a black top, a denim jacket and a blue skirt |
| **Special 7** | the biker outfit with a blue top |

The game only loads Jill's model when she goes through a door (or a game starts), so she changes into
the outfit you pick **at the next door** — until then the panel says so in orange. The outfit is part of
the save file: a save made after the change keeps it, and loading an older save brings back that save's
own outfit. While you play as Carlos, the list sets the outfit Jill wears when she is back. The
Mercenaries' characters have no outfits, so the list is not there.

### Reloading tool proficiency

Ammo you make with the reloading tool comes out in bigger batches the more of that kind you have
made: the game quietly counts your batches of handgun bullets, shotgun shells, magnum bullets and
grenade rounds, and each kind climbs five levels on its own. The panel's **Reloading tool** section
shows where each kind stands and lets you pick its level:

| level | batches made | each new batch |
| --- | --- | --- |
| 1 | 0–2 | as the recipe says (Gun Powder A: 15 handgun bullets) |
| 2 | 3–5 | +10% (17) |
| 3 | 6–10 | +30% (20) |
| 4 | 11–20 | +50% (23) |
| 5 | 21 or more | +70% (26) |

Hover over a level to see what every powder makes at it. Picking one sets the count of batches made
to the fewest that give that level; **All to level 5** does it for every kind at once.

Once you have made more than six batches of **handgun bullets** or **shotgun shells**, the game also
asks, each time you make them, whether to make **enhanced** rounds instead, at the bonus of the level
below. Picking level 3 or above for either kind turns that question on. Gun powder mixed straight into
grenade rounds counts as a grenade batch and gets the same bonus, and on **Easy** every batch comes
out doubled.

The counts belong to the inventory of the character you are playing — Carlos has his own — and are
part of the save file, so they are kept when you save.

### Mercenaries money

In the Mercenaries — and only there — the panel has a section of its own with your money, and lets
you change it. Type an amount (or use **−** / **+**) and press *Set*:

- **Takings this run** — what the run has earned so far; every kill pays into it. The results rank
  you on it ($1,500 is an A) and pay it into your bank, with half the seconds left on the clock as a
  bonus. It goes up to **$9,999**: the results count that whole sum in a way that goes wrong past
  $32,767, so the editor stops well short and leaves room for the time bonus and for whatever the
  rest of the run earns.
- **Bank** — the money every run's results pay into and the shop after them spends. It goes up to
  **$999,999**, the game's own maximum. The game keeps it in `saves\data.sav` and writes it there
  after a run's results, so a change made during a run is kept once you have finished it.

Neither amount can be set below $0 or past its maximum: a bigger number typed into the box is
brought back to the maximum before it is set.

### The inventory and the item box

The lower half of the panel is your inventory — as many of the game's ten slots as your character
may use — and below it the item box.

Each inventory slot is a row: pick an item from its list (type to narrow it — a name, either of the
names an item goes by before and after you examine it, like `Crank (Square Crank)`, or an item
number), set how many, and press **Apply**. The panel asks before it writes anything, and **x**
throws away an edit you have not applied. The game keeps the inventory packed, and so does the
panel: something new goes into the first empty slot, and emptying or storing a slot moves the items
after it up. **E** marks the equipped weapon: putting another weapon in
its slot equips that one, and emptying or storing it unequips it — the game hands you the new
weapon when the status screen closes.

The item list is the game's own — all of its items, with the names the game gives them, grouped into
key items (the files and maps among them), weapons, ammo, heals, and other — gun powder, ink ribbons
and Inf. Bullets. How many of an item one slot
holds is the game's own number too, so a Magnum takes six rounds and a stack of Handgun Bullets goes
to 250.

**The item box** is the game's: the same 64 slots every item box in the city opens. **Store**
beside an inventory slot puts that item in it, **Take** puts a box item into your inventory, and
**x** throws one away (after asking). Ammo, ink ribbons and the reloading tool top up a stack of the
same item first, the way the game's own box screen combines them; everything else takes a free slot
of its own, and anything that does not fit stays where it was. The box is listed one bank at a time,
sorted by name, and `#` is the slot an item sits in on the game's item box screen.

The game's own status screen draws its item icons as it opens, so an inventory change shows there
the next time you open it — close and reopen it after editing (the panel reminds you). Changes are
only written while the status screen is up, and never while a typewriter has the
inventory — the save screen borrows the slots and puts them back when it closes.

Because the inventory and the box live in the save data, everything you change here is saved with
the game.

### Save files

The game keeps all its saves on one emulated PlayStation memory card: the file `saves\bu00.sav` in
the `english` folder, fifteen slots in all. Whenever the game's own list of saves is on screen —
**LOAD GAME** on the title, the load prompt after the opening movie, or a typewriter's save — the
panel lists those fifteen slots beside it: where each save was made, its play time, its save count and
its difficulty, with the slot the game's cursor is on highlighted.

- **Delete** removes a save. The game's list shows *No data* for the slot from then on, exactly like
  a slot that was never used, and your next save there fills it again.
- **Copy to...** puts a copy of a save into another slot, empty or not (a save already there is
  replaced). The copy loads exactly like the original.

Both ask first, and both work while the game's list is waiting for you — on the questions before it
(*Load the game* / *Save the game*) the buttons are greyed out. They change the card straight away,
and the game's list beside the panel shows the change at once. **Neither can be undone**: back up the
`saves` folder first if in doubt.

The game reads the card once, as it starts, keeps it in memory, and writes the whole of it back after
every save and when it closes. So the mod changes both — the file, and the game's own copy, which
would otherwise put the file back the way it was the next time the game saved or closed.

## Settings and the log

`RE3-1999CabbyCodes.ini` is written next to the DLL the first time the game runs, with every option
documented in it. The useful ones:

```ini
ToggleKey = 0x76           ; the key that hides/shows the panel (0x76 = F7)
SkipKey = 0x77             ; the key that offers to skip a cutscene (0x77 = F8)
SkipPadButton = Start      ; the pad button that does the same: Start, Back, A, B, X, Y, LB, RB, LS, RS, None
GodMode = 0                ; the cheats: the panel switches them and writes them back here
OneHitKills = 0
InfiniteAmmo = 0
InfiniteInkRibbons = 0
NoSaveCount = 0
FreezePlaytime = 0
NoRecoveryCount = 0
FreezeMercsTimer = 0
SkipCutscenes = 1
AlwaysShow = 0             ; draw the panel on every screen, not only the status screen
Trace = 1                  ; verbose diagnostics in the log
Disable =                  ; overlay,dispatch,game,gpa,input,cursor - turn a subsystem off
```

`RE3-1999CabbyCodes.log` sits beside it and says what the mod found in the game, what it hooked and
what every cheat did; the run before the current one is kept as `.prev.log`. If something does not
work, that file is the place to look — and the right thing to send with a bug report.

## Building it yourself

Linux, with `mingw-w64` (no Windows or MSVC needed):

```sh
cp config.mk.example config.mk        # then set GAME_DIR to your english folder
make                                  # -> build/ddraw.dll
make install                          # rename the stock DLL and deploy ours
make uninstall                        # put the stock DLL back
make package                          # -> dist/RE3-1999CabbyCodes_v<version>.zip
```

`CLAUDE.md` is the developer's guide: what was reverse-engineered, how, and the rules the code
follows.

## Credits and licence

Dear ImGui (MIT) is vendored under `contrib/imgui`. Everything else is GPL-3.0 — see `LICENSE`.

This is an unofficial fan mod. Resident Evil 3: Nemesis is Capcom's.
