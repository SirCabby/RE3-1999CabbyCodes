#pragma once

#include <cstdio>
#include <cstring>

// Resident Evil 3: Nemesis (1999) item ids, as an inventory slot stores them:
// a slot is four bytes - {id, count, flags, spare} - and ten of them sit at the
// front of the inventory block the game's slot pointer names (see game.h).
//
// The table below is the game's own, read out of the dump: the ids, the names
// and the per-slot maximum come from the item table at 0x52AC50 (four bytes an
// id: kind, maximum, the name to show once the item has been examined, the
// ammo it takes) and the name table at 0x51B7EC/0x51AF10, whose text is stored
// with every character shifted down by 0x24 and 0 for a space; 0xEA brings in
// a glyph of a second set (S, T, A, R, &), which is how the game spells
// S.T.A.R.S. An item the game shows under a plainer name until it is examined
// carries the other name in brackets, so typing either one finds it; the ids
// that share a name in the game (the four Grenade Launcher loadings, the two
// rifle modes, the six mixed herbs, the two Clock Tower and three Park keys)
// are told apart the same way. So are the two lighters, both "Lighter": 0x42 is
// the empty one (the game's "Empty Lighter" once examined) and 0x43 the one
// Lighter Oil + 0x42 makes; and the two S.T.A.R.S. Cards, Jill's (0x2F) and
// Brad's (0x38). The combination table at 0x52AF00 names what makes each herb
// mix and lighter, and the item descriptions - one an id, in id order from the
// Green Herb's at 0x519566 - agree, and say whose card is whose. The two
// Facility Keys are the one card before and after the Dead Factory's
// ultraviolet device: 0x7E as found ("coated with some kind of special
// painting"), which that device takes and swaps for 0x7F ("The plastic card is
// radiating") - the only one the sturdy door's card slot, downstairs in the
// elevator room, reads. The room scripts say so (CLAUDE.md, "The room scripts").
//
// `kind` is the game's own column, which bank() below turns into the panel's
// grouping; `maximum` is what one slot of that item holds - a gun's magazine,
// 250 for ammo and ink ribbons, 1 for everything else. game::item_max() reads
// the live table and falls back to this.
namespace re3cc::items {

// The game's own kind column, value for value.
enum Kind { kNone = 0, kWeapon, kAmmo, kHeal, kOther, kKey, kTool, kFile, kMap };

struct Def {
  int id;
  const char* name;
  Kind kind;
  int maximum;
};

inline const Def kTable[] = {
    {0x00, "(unused)",                        kNone,     0},
    {0x01, "Knife",                           kWeapon,   1},
    {0x02, "Merc's Handgun",                  kWeapon,  15},
    {0x03, "Hand Gun",                        kWeapon,  15},
    {0x04, "Shotgun",                         kWeapon,   7},
    {0x05, "Magnum",                          kWeapon,   6},
    {0x06, "G. Launcher (burst rounds)",      kWeapon, 250},
    {0x07, "G. Launcher (flame rounds)",      kWeapon, 250},
    {0x08, "G. Launcher (acid rounds)",       kWeapon, 250},
    {0x09, "G. Launcher (freeze rounds)",     kWeapon, 250},
    {0x0A, "R. Launcher",                     kWeapon,   4},
    {0x0B, "Gatling Gun",                     kWeapon, 250},
    {0x0C, "Mine Thrower",                    kWeapon,   6},
    {0x0D, "EAGLE 6.0",                       kWeapon,  15},
    {0x0E, "Assault Rifle (manual)",          kWeapon, 100},
    {0x0F, "Assault Rifle (auto)",            kWeapon, 100},
    {0x10, "Western Custom",                  kWeapon,   6},
    {0x11, "SIGPRO E",                        kWeapon,  15},
    {0x12, "M92F E",                          kWeapon,  15},
    {0x13, "Benelli M3S E",                   kWeapon,   7},
    {0x14, "M. Thrower E",                    kWeapon,   6},
    {0x15, "H. Gun Bullets",                  kAmmo,   250},
    {0x16, "Magnum Bullets",                  kAmmo,   250},
    {0x17, "Shotgun Shells",                  kAmmo,   250},
    {0x18, "Grenade Rounds",                  kAmmo,   250},
    {0x19, "Flame Rounds",                    kAmmo,   250},
    {0x1A, "Acid Rounds",                     kAmmo,   250},
    {0x1B, "Freeze Rounds",                   kAmmo,   250},
    {0x1C, "M.T. Rounds",                     kAmmo,   250},
    {0x1D, "A.R. Bullets",                    kAmmo,   100},
    {0x1E, "H.G. Bullets E",                  kAmmo,   250},
    {0x1F, "S.G. Shell E",                    kAmmo,   250},
    {0x20, "F. Aid Spray",                    kHeal,     1},
    {0x21, "Green Herb",                      kHeal,     1},
    {0x22, "Blue Herb",                       kHeal,     1},
    {0x23, "Red Herb",                        kHeal,     1},
    {0x24, "Mixed Herb (green + green)",      kHeal,     1},
    {0x25, "Mixed Herb (green + blue)",       kHeal,     1},
    {0x26, "Mixed Herb (green + red)",        kHeal,     1},
    {0x27, "Mixed Herb (green x3)",           kHeal,     1},
    {0x28, "Mixed Herb (green x2 + blue)",    kHeal,     1},
    {0x29, "Mixed Herb (green + red + blue)", kHeal,     1},
    {0x2A, "F. Aid Box",                      kHeal,     3},
    {0x2B, "Crank (Square Crank)",            kOther,    1},
    {0x2C, "BOTU",                            kOther,    1},
    {0x2D, "BOTU",                            kOther,    1},
    {0x2E, "BOTU",                            kOther,    1},
    {0x2F, "S.T.A.R.S. Card (Jill's)",        kOther,    1},
    {0x30, "BOTU",                            kOther,    1},
    {0x31, "Battery",                         kOther,    1},
    {0x32, "Fire Hook",                       kOther,    1},
    {0x33, "Power Cable",                     kOther,    1},
    {0x34, "Fuse",                            kOther,    1},
    {0x35, "BOTU",                            kOther,    1},
    {0x36, "Oil Additive",                    kOther,    1},
    {0x37, "Card Case",                       kOther,    1},
    {0x38, "S.T.A.R.S. Card (Brad's)",        kOther,    1},
    {0x39, "Machine Oil",                     kOther,    1},
    {0x3A, "Mixed Oil",                       kOther,    1},
    {0x3B, "BOTU",                            kOther,    1},
    {0x3C, "Wrench",                          kOther,    1},
    {0x3D, "Iron Pipe",                       kOther,    1},
    {0x3E, "BOTU",                            kOther,    1},
    {0x3F, "Fire Hose",                       kOther,    1},
    {0x40, "Tape Recorder",                   kOther,    1},
    {0x41, "Lighter Oil",                     kOther,    1},
    {0x42, "Lighter (empty)",                 kOther,    1},
    {0x43, "Lighter (full)",                  kOther,    1},
    {0x44, "Green Gem (Emerald)",             kOther,    1},
    {0x45, "Blue Gem (Sapphire)",             kOther,    1},
    {0x46, "Amber Ball",                      kOther,    1},
    {0x47, "Obsidian Ball",                   kOther,    1},
    {0x48, "Crystal Ball",                    kOther,    1},
    {0x49, "BOTU",                            kOther,    1},
    {0x4A, "BOTU",                            kOther,    1},
    {0x4B, "BOTU",                            kOther,    1},
    {0x4C, "Gold Gear",                       kOther,    1},
    {0x4D, "Silver Gear",                     kOther,    1},
    {0x4E, "Chronos Gear",                    kOther,    1},
    {0x4F, "Bronze Book (Book of Wisdom)",    kOther,    1},
    {0x50, "Bronze Compass (Future Compass)", kOther,    1},
    {0x51, "Vaccine Medium",                  kOther,    1},
    {0x52, "Vaccine Base",                    kOther,    1},
    {0x53, "BOTU",                            kOther,    1},
    {0x54, "BOTU",                            kOther,    1},
    {0x55, "Vaccine",                         kOther,    1},
    {0x56, "BOTU",                            kOther,    1},
    {0x57, "BOTU",                            kOther,    1},
    {0x58, "Medium Base",                     kOther,    1},
    {0x59, "EAGLE Parts A",                   kOther,    1},
    {0x5A, "EAGLE Parts B",                   kOther,    1},
    {0x5B, "M37 Parts A",                     kOther,    1},
    {0x5C, "M37 Parts B",                     kOther,    1},
    {0x5D, "BOTU",                            kOther,    1},
    {0x5E, "Chronos Chain",                   kOther,    1},
    {0x5F, "Rusted Crank (Rust Hex Crank)",   kOther,    1},
    {0x60, "Card Key",                        kOther,    1},
    {0x61, "Gun Powder A",                    kOther,    1},
    {0x62, "Gun Powder B",                    kOther,    1},
    {0x63, "Gun Powder C",                    kOther,    1},
    {0x64, "Gun Powder AA",                   kOther,    1},
    {0x65, "Gun Powder BB",                   kOther,    1},
    {0x66, "Gun Powder AC",                   kOther,    1},
    {0x67, "Gun Powder BC",                   kOther,    1},
    {0x68, "Gun Powder CC",                   kOther,    1},
    {0x69, "Gun Powder AAA",                  kOther,    1},
    {0x6A, "Gun Powder AAB",                  kOther,    1},
    {0x6B, "Gun Powder BBA",                  kOther,    1},
    {0x6C, "Gun Powder BBB",                  kOther,    1},
    {0x6D, "Gun Powder CCC",                  kOther,    1},
    {0x6E, "Inf. Bullets",                    kOther,    1},
    {0x6F, "Water Sample",                    kOther,    1},
    {0x70, "System Disk",                     kOther,    1},
    {0x71, "Dummy Key",                       kKey,      1},
    {0x72, "Lockpick",                        kKey,      1},
    {0x73, "Warehouse Key (Backdoor Key)",    kKey,      1},
    {0x74, "Sickroom Key",                    kKey,      1},
    {0x75, "Emblem Key (S.T.A.R.S. Key)",     kKey,      1},
    {0x76, "BOTU",                            kKey,      1},
    {0x77, "Clock T. Key (Bezel Key)",        kKey,      1},
    {0x78, "Clock T. Key (Winder Key)",       kKey,      1},
    {0x79, "Chronos Key",                     kKey,      1},
    {0x7A, "BOTU",                            kKey,      1},
    {0x7B, "Park Key (Main Gate Key)",        kKey,      1},
    {0x7C, "Park Key (Graveyard Key)",        kKey,      1},
    {0x7D, "Park Key (Rear Gate Key)",        kKey,      1},
    {0x7E, "Facility Key (before UV)",        kKey,      1},
    {0x7F, "Facility Key (glowing)",          kKey,      1},
    {0x80, "Boutique Key",                    kKey,      1},
    {0x81, "Ink Ribbon",                      kTool,   250},
    {0x82, "Reloading Tool",                  kTool,   250},
    {0x83, "Game Inst. A",                    kTool,     1},
    {0x84, "Game Inst. B",                    kTool,     1},
    {0x85, "Game Instructions A",             kFile,     1},
    {0x86, "Dario's Memo",                    kFile,     1},
    {0x87, "Merc's Diary",                    kFile,     1},
    {0x88, "Business Fax",                    kFile,     1},
    {0x89, "Marvin's Report",                 kFile,     1},
    {0x8A, "David's Memo",                    kFile,     1},
    {0x8B, "City Guide",                      kFile,     1},
    {0x8C, "Reporter's Memo",                 kFile,     1},
    {0x8D, "Operation Instruction",           kFile,     1},
    {0x8E, "Merc's Pocketbook",               kFile,     1},
    {0x8F, "Art Picture Postcard",            kFile,     1},
    {0x90, "Supervisor's Report",             kFile,     1},
    {0x91, "Written Order",                   kFile,     1},
    {0x92, "Director's Diary",                kFile,     1},
    {0x93, "Manager's Diary",                 kFile,     1},
    {0x94, "Security Manual",                 kFile,     1},
    {0x95, "Mechanic's Memo",                 kFile,     1},
    {0x96, "Fax from Kendo",                  kFile,     1},
    {0x97, "Manager's Report",                kFile,     1},
    {0x98, "Med. Instr. Manual",              kFile,     1},
    {0x99, "Fax from the H.Q.",               kFile,     1},
    {0x9A, "Incinerator Manual",              kFile,     1},
    {0x9B, "Photo A",                         kFile,     1},
    {0x9C, "Photo B",                         kFile,     1},
    {0x9D, "Photo C",                         kFile,     1},
    {0x9E, "Photo E",                         kFile,     1},
    {0x9F, "Photo D",                         kFile,     1},
    {0xA0, "Clock Tower Postcard",            kFile,     1},
    {0xA1, "Game Instructions B",             kFile,     1},
    {0xA2, "Classified Photo File",           kFile,     1},
    {0xA3, "Jill's Diary",                    kFile,     1},
    {0xA4, "Uptown Map",                      kMap,      1},
    {0xA5, "Downtown Map",                    kMap,      1},
    {0xA6, "Clock Tower Map",                 kMap,      1},
    {0xA7, "Park Map",                        kMap,      1},
    {0xA8, "Dead Factory Map",                kMap,      1},
    {0xA9, "Police Station Map",              kMap,      1},
    {0xAA, "Hospital Map",                    kMap,      1},
    {0xAB, "no item",                         kMap,      1},
};
inline constexpr int kCount = sizeof(kTable) / sizeof(kTable[0]);
inline constexpr int kLastId = 0xAB;         // the item table's last record ("no item")
inline constexpr int kKnifeId = 0x01;
inline constexpr int kInkRibbonId = 0x81;    // what a save costs
inline constexpr int kReloadingToolId = 0x82;  // makes ammo out of gun powder
inline constexpr int kNoItemId = 0xAB;
inline constexpr int kInfiniteBulletsId = 0x6E;
inline constexpr int kGunPowderAId = 0x61, kGunPowderCCCId = 0x6D;  // the thirteen powders, A to CCC, in a row
// The ammo the reloading tool makes, by the kinds it keeps its proficiency in
// (game.h): each kind's first member, and the enhanced rounds two of them offer.
inline constexpr int kHandgunBulletsId = 0x15, kMagnumBulletsId = 0x16, kShotgunShellsId = 0x17;
inline constexpr int kGrenadeRoundsId = 0x18;
inline constexpr int kHandgunBulletsEId = 0x1E, kShotgunShellsEId = 0x1F;

inline const Def* find(int id) {
  for (const Def& d : kTable)
    if (d.id == id) return &d;
  return nullptr;
}
inline const char* name(int id) {
  const Def* d = find(id);
  return d ? d->name : "(unknown item)";
}
inline Kind kind(int id) {
  const Def* d = find(id);
  return d ? d->kind : kOther;
}
inline int maximum(int id) {
  const Def* d = find(id);
  return d ? d->maximum : 1;
}
inline bool is_weapon(int id) { return kind(id) == kWeapon; }
// Everything that carries a count worth showing: guns hold their loaded rounds,
// ammo and ink ribbons stack, and the first aid box holds three.
inline bool counts(int id) {
  const Def* d = find(id);
  return d && d->maximum > 1;
}

// " x12" after an item's name, or nothing for an item whose count means nothing.
struct Quantity {
  char text[20];
};
inline Quantity quantity(int id, int count) {
  Quantity q;
  if (counts(id)) std::snprintf(q.text, sizeof(q.text), " x%d", count);
  else q.text[0] = '\0';
  return q;
}

// The panel's tabs, in order. A bank is only where an entry is listed - the
// inventory is the game's ten slots, kept in the order the game keeps them -
// so an item filed under the wrong bank is misplaced, never mishandled.
enum Bank { kBankKey = 0, kBankWeapons, kBankAmmo, kBankHeal, kBankOther, kBankCount };
inline const char* bank_name(int b) {
  static const char* const kNames[kBankCount] = {"Key items", "Weapons", "Ammo", "Heals", "Other"};
  return b >= 0 && b < kBankCount ? kNames[b] : kNames[kBankOther];
}
// Key items are the keys and everything else the game files under "other",
// "tool", "file" and "map": what a puzzle or a door asks for (the cranks, gems,
// gears, oils, lighter, weapon parts...), the reloading tool, the files and the
// maps. Other keeps what is used up and is neither ammo nor a heal: the gun
// powders, ink ribbons and Inf. Bullets.
inline int bank(int id) {
  const Def* d = find(id);
  switch (d ? d->kind : kNone) {
    case kWeapon: return kBankWeapons;
    case kAmmo: return kBankAmmo;
    case kHeal: return kBankHeal;
    case kOther:
    case kKey:
    case kTool:
    case kFile:
    case kMap:
      if ((id >= kGunPowderAId && id <= kGunPowderCCCId) || id == kInkRibbonId || id == kInfiniteBulletsId)
        return kBankOther;
      return kBankKey;
    default: return kBankOther;
  }
}

// Names compared the way a reader sorts them, case aside.
inline int compare_names(const char* a, const char* b) {
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + 32 : static_cast<int>(c); };
  for (;; ++a, ++b) {
    const int d = lower(*a) - lower(*b);
    if (d || !*a) return d;
  }
}

// The order the panel lists items in: bank by bank, and by name within a bank.
inline bool listed_before(int a, int b) {
  const int ka = bank(a), kb = bank(b);
  if (ka != kb) return ka < kb;
  const Def* da = find(a);
  const Def* db = find(b);
  if (!da || !db) return da ? true : db ? false : a < b;
  const int c = compare_names(da->name, db->name);
  return c ? c < 0 : a < b;
}

// An id the panel offers: the unused and placeholder records are left out.
inline bool listable(int id) {
  const Def* d = find(id);
  if (!d || !d->name[0]) return false;
  return std::strncmp(d->name, "(unused)", 8) != 0 && std::strncmp(d->name, "BOTU", 4) != 0 &&
         std::strcmp(d->name, "no item") != 0;
}

}  // namespace re3cc::items
