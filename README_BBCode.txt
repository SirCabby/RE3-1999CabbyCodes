[b][size=150]RE3-1999CabbyCodes[/size][/b]

A cheat panel for [b]Resident Evil 3: Nemesis (1999)[/b] - the classic PC version, as sold on Steam
(app 4249120) and GOG.

Open the game's [b]status screen[/b] - the inventory screen you already use - and a panel appears
beside it with God mode, One hit kills, Infinite ammo, Freeze countdown timer, Infinite ink ribbons,
Save without counting, Freeze play time, Heal without counting, editors for the save count, the
play-time clock, the [b]recovery total[/b] the ending ranks you on, the [b]clear count[/b] the epilogues
unlock by, your [b]reloading tool proficiency[/b] and your [b]Mercenaries money[/b], Jill's [b]outfit[/b] (any of the
game's costumes, in the middle of a game), and a full [b]inventory editor[/b]: put any of the game's items into any of
your slots, set how many, equip it. Beside the game's [b]load and save screens[/b] the panel is a [b]save file
manager[/b] instead: delete a save, or copy one into another slot. And during any cutscene or movie, [b]F8[/b] (or
[b]Start[/b] on a pad) pauses the game and offers to [b]skip it[/b].

Nothing is patched on disk. The mod is a proxy ddraw.dll that sits in front of the DirectDraw ->
Direct3D 9 wrapper the game already ships with; everything it touches in the game is found at run
time by byte signature, and every code patch is reverted when you switch the cheat off or close the
game.

This mod is open source! Check it out at [url=https://github.com/SirCabby/RE3-1999CabbyCodes]https://github.com/SirCabby/RE3-1999CabbyCodes[/url]

[b][size=125]Install[/size][/b]
[list=1]
[*]Open your Resident Evil 3 folder, then its [b]english[/b] folder - the one with
ResidentEvil3.exe in it. On Steam that is usually
[i]...\steamapps\common\4249120_Biohazard3\english[/i].
[*]Rename the ddraw.dll that is already there to [b]ddraw_orig.dll[/b].
[*]Copy this mod's ddraw.dll in beside it.
[/list]

[b]On Windows that is all.[/b]

[b]On Linux / Steam Deck (Proton)[/b] you also need one launch option. Right-click the game in Steam
-> Properties -> Launch Options, and put this in:
[code]WINEDLLOVERRIDES="ddraw=n,b" %command%[/code]
Proton loads a native ddraw.dll by itself for Resident Evil (1996), RE2 (1998), Dino Crisis and
Breath of Fire IV, but [b]not[/b] for Resident Evil 3. Without that line Wine uses its own
DirectDraw, the game's wrapper is never loaded, and neither is the mod.

[b]Uninstall:[/b] delete ddraw.dll and rename ddraw_orig.dll back to ddraw.dll.
Steam's [i]Verify integrity of game files[/i] puts the stock DLL back and removes the mod - just
install it again.

[b][size=125]Use[/size][/b]

Play as usual. Open the [b]status screen[/b] (the inventory key) and the panel is there. It is a
normal window: drag it by its title bar, and it remembers where you put it. It also comes up beside the
game's load and save screens, where it manages your save files.
[list]
[*][b]F7[/b] hides and shows the panel while that screen is up.
[*]While the panel is up the mouse is free to leave the game for another window or monitor.
[*]Typing in one of the panel's boxes takes the keyboard and the pad away from the game until you
click off it, so the game's own cursor does not move while you type.
[*]Everything above the inventory sits under a [b]Cheats[/b] heading: click it to fold the cheats away
and give the inventory and the item box the room. The heading still says how many are on, and the
panel remembers whether it was folded.
[/list]
[b]The Mercenaries[/b] - the launcher's Mercenaries button, which starts RE3_MERCE.exe from the same
english folder - loads the mod too: open the status screen during a run and the panel is there.

[b]The cheats[/b]
[list]
[*][b]God mode[/b] - no hit takes any health off you, a barrel going off at point blank included,
and your health is held at its maximum and the poison that drains it is cured, every frame. You
still flinch and get knocked about; deaths the story scripts itself are left to the game.
[*][b]One hit kills[/b] - every enemy in the room is held at 1 HP, the last value the game still
counts as alive, so your next hit kills it. Deaths, drops and scripts all run through the game's own
code. Nemesis included; boss fights written around a health bar may end sooner than they were meant
to.
[*][b]Infinite ammo[/b] - the weapon you have equipped is topped back up to a full load every frame,
so firing never runs it dry. Reloads and pickups work as usual.
[*][b]Freeze countdown timer[/b] - any countdown the game is running stops where it is, hundredths
and all, so the time never runs out: the Mercenaries' clock, and the main game's own - the
hospital's time bomb, the four-minute one late in the Dead Factory, and the incoming missile on
the radar receiver at the very end (its distance stops, so it never arrives). The Mercenaries'
time bonuses still add to it, and it runs on again from there the moment you switch this off.
[*][b]Infinite ink ribbons[/b] - saving at a typewriter stops costing a ribbon. You still need one in
the inventory to be offered the save; it simply is not used up.
[*][b]Save without counting[/b] - saving no longer adds to the save count the ending ranks you on.
[*][b]Freeze play time[/b] - the play-time clock stops where it is, and runs on again the moment you
switch this off.
[*][b]Heal without counting[/b] - healing items heal you as usual but no longer add to the recovery
total the ending ranks you on.
[*][b]Skip cutscenes[/b] - on by default; see below.
[/list]
Every switch is remembered: change one and it is written to the ini at once, so the cheats are as
you left them the next time you start the game.

[b]Skipping cutscenes[/b]

While a cutscene (the game's letterbox scenes) or a movie is playing, press [b]F8[/b] - or
[b]Start[/b] on a game pad - and the game pauses where it is, picture and sound, and asks whether to
skip it. Enter, Space, F8, A or Start skips it; Esc or B carries on exactly where it paused.
A movie ends there and then. A cutscene is run through to its end unseen and unheard rather than
cut, so every door, item, flag and room change it makes still happens; lines of speech are cut short
the way the game cuts them itself, so a talky scene skips as quickly as a silent one. A message or a
choice the scene brings up (a live selection, say) is still yours to answer: the skip shows you the
game and waits for it, then carries on. A status screen the scene opens (the Mercenaries intro ends
on one) ends the skip there, and the screen is yours. The play-time clock moves on by exactly as long
as the scene
would have taken, and nothing for the time spent paused.

[b]Saves[/b], [b]Play time[/b] and [b]Recovery[/b] also have editors: type a number and press Apply /
Set. All three are part of the save file, so they are kept when you save and come back when you load.

[b]The ending's rank[/b]

The results screen after the ending ranks you A to F (its Grade) on three scores of up to 100 points
each:
[list]
[*][b]Saves[/b]: 100 for none, 90 for one, 80 for two, 70 for three, 60 for 4-5, 50 for 6-8, 40 for
9-12, 30 for 13-17, 20 for 18-23, 10 for 24-30, and nothing for more.
[*][b]Play time[/b]: 100 up to 2:29:59, then 90 up to 2:39:59, 80 to 2:59:59, 70 to 3:29:59, 60 to
4:09:59, 50 to 4:59:59, 40 to 5:59:59, 30 to 7:09:59, 20 to 8:29:59, 10 to 9:59:59, and nothing for
longer.
[*][b]Recovery[/b]: 100 up to 1,500, then 10 less for every 200 more, and nothing over 3,300.
[/list]
Added up, 270 points or more is an A, 230 a B, 180 a C, 120 a D, 50 an E, and anything less an F.
On Easy the results show no rank at all.

Recovery is the healing you have used. Every healing item adds its full amount to it, however much
of it you actually needed: your whole health (200 for Jill) for a first aid spray or box, three green
herbs or any mix with a red herb; half of it for two green herbs and a quarter for one, blue mixed in
or not; nothing for a blue herb on its own. A new game starts it at 0. The panel shows the total and the points it is worth
beside its editor - in orange once it is costing you - and [b]Heal without counting[/b] keeps it where
it is while your items go on healing you.

[b]Clears and the epilogues[/b]

The game counts how many times you have finished it. Each of your first eight endings shows a new
[b]epilogue[/b], and the title menu's [b]EPILOGUE[/b] screen lets you read every one unlocked so far:
one for each clear, up to all eight.

The panel's [b]Clears & epilogues[/b] section shows the count for the game you are playing and lets
you set it: type a number and press Apply. Beside it the panel says what the ending will do. The
ending adds one to the count and records it in saves\data.sav, which is where the EPILOGUE screen
reads it from, so the screen changes once you have finished the game:
[list]
[*][b]7[/b] before you finish: the ending shows the eighth and last epilogue, then one more picture,
and the EPILOGUE screen has all eight from then on.
[*][b]8 or more[/b]: all eight are unlocked as well, but the ending shows none.
[*]On [b]Easy[/b] the ending shows no epilogue at all, but the clear still counts.
[/list]
The count is part of the save file, so it is kept when you save and comes back when you load. The
EPILOGUE screen always goes by the last ending's count, so finishing an older save that counts fewer
clears takes it back down. The count also decides Nemesis's seventh drop on Hard: an assault rifle on
your first playthrough, Inf. Bullets on every one after it. The Mercenaries has no ending to count,
so the section is not there.

[b]Outfits[/b]

The panel's [b]Outfit[/b] list changes Jill's costume in the middle of a game. It offers the eight the
game lets you pick on its [b]COSTUME SELECT[/b] screen before a new game, by the names that screen
gives them:
[list]
[*][b]Normal[/b] - her tube top and skirt, with the backpack once she has it
[*][b]Special 1[/b] - biker leathers
[*][b]Special 2[/b] - her S.T.A.R.S. uniform from the first game, beret and all
[*][b]Special 3[/b] - a white trouser suit
[*][b]Special 4[/b] - a police uniform
[*][b]Special 5[/b] - Regina, from Dino Crisis
[*][b]Special 6[/b] - Normal with a black top, a denim jacket and a blue skirt
[*][b]Special 7[/b] - the biker outfit with a blue top
[/list]
The game only loads Jill's model when she goes through a door (or a game starts), so she changes into
the outfit you pick [b]at the next door[/b] - until then the panel says so in orange. The outfit is
part of the save file: a save made after the change keeps it, and loading an older save brings back
that save's own outfit. While you play as Carlos, the list sets the outfit Jill wears when she is
back. The Mercenaries' characters have no outfits, so the list is not there.

[b]Reloading tool proficiency[/b]

Ammo you make with the reloading tool comes out in bigger batches the more of that kind you have
made: the game quietly counts your batches of handgun bullets, shotgun shells, magnum bullets and
grenade rounds, and each kind climbs five levels on its own - no bonus for the first 3 batches, then
+10% from the 4th, +30% from the 7th, +50% from the 12th and +70% from the 22nd (Gun Powder A goes
from 15 handgun bullets to 26). The panel's [b]Reloading tool[/b] section shows where each kind
stands and lets you pick its level; hover over a level to see what every powder makes at it, and
[b]All to level 5[/b] does every kind at once.

Once you have made more than six batches of handgun bullets or shotgun shells, the game also asks
each time whether to make [b]enhanced[/b] rounds instead, at the bonus of the level below; picking
level 3 or above for either kind turns that question on. Gun powder mixed straight into grenade rounds
counts as a grenade batch, and on Easy every batch comes out doubled. The counts belong to the
character you are playing (Carlos has his own) and are saved with the game.

[b]Mercenaries money[/b]

In the Mercenaries - and only there - the panel has a section of its own with your money, and lets
you change it. Type an amount (or use - / +) and press Set:
[list]
[*][b]Takings this run[/b] - what the run has earned so far; every kill pays into it. The results
rank you on it ($1,500 is an A) and pay it into your bank, with half the seconds left on the clock
as a bonus. It goes up to [b]$9,999[/b]: the results count that whole sum in a way that goes wrong
past $32,767, so the editor stops well short and leaves room for the time bonus and for whatever the
rest of the run earns.
[*][b]Bank[/b] - the money every run's results pay into and the shop after them spends. It goes up
to [b]$999,999[/b], the game's own maximum. The game keeps it in saves\data.sav and writes it there
after a run's results, so a change made during a run is kept once you have finished it.
[/list]
Neither amount can be set below $0 or past its maximum: a bigger number typed into the box is
brought back to the maximum before it is set.

[b]The inventory editor[/b]

The lower half of the panel is your inventory - the ten slots the game keeps, as many of them as your
character may use. Click a slot, search the item list (typing "green herb" or "herb green" both find
it), set how many, and press [b]Put in slot[/b]. Empty clears the slot, Equip puts the weapon in it
in your hands, and Fill ammo tops every gun and ammo stack in the bag up to a full load.

The item list is the game's own - all 171 of its items, with the names the game gives them, grouped
into key items (the files and maps among them), weapons, ammo, heals, and other - gun powder, ink
ribbons and Inf. Bullets. An item the game calls one
thing until you have examined it carries the other name in brackets ("Crank (Square Crank)"), so
searching for either one finds it. How many of an item one slot holds is the game's own number too,
so a Magnum takes six rounds and a stack of Handgun Bullets goes to 250.

Because the inventory lives in the save data, everything you change here is saved with the game.

[b]Save files[/b]

The game keeps all its saves on one emulated PlayStation memory card: saves\bu00.sav in the english
folder, fifteen slots in all. Whenever the game's own list of saves is on screen - LOAD GAME on the
title, the load prompt after the opening movie, or a typewriter's save - the panel lists those fifteen
slots beside it: where each save was made, its play time, its save count and its difficulty, with the
slot the game's cursor is on highlighted.
[list]
[*][b]Delete[/b] removes a save. The game's list shows No data for the slot from then on, exactly like
a slot that was never used, and your next save there fills it again.
[*][b]Copy to...[/b] puts a copy of a save into another slot, empty or not (a save already there is
replaced). The copy loads exactly like the original.
[/list]
Both ask first, and both work while the game's list is waiting for you. They change the card straight
away - the game's own copy of it and the file - and the game's list beside the panel shows the change
at once. [b]Neither can be undone[/b]: back up the saves folder first if in doubt.

[b][size=125]Settings and the log[/size][/b]

RE3-1999CabbyCodes.ini is written next to the DLL the first time the game runs, with every option
documented in it: the panel and skip keys, the cheats (as you last left them), and the diagnostics.
RE3-1999CabbyCodes.log sits beside it and says what the mod found in the game, what it hooked and
what every cheat did. If something does not work, that file is the right thing to send with a bug
report.

[b][size=125]Source[/size][/b]

GPL-3.0. Dear ImGui (MIT) is vendored. Built on Linux with mingw-w64 - no Windows or MSVC needed.

This is an unofficial fan mod. Resident Evil 3: Nemesis is Capcom's.
