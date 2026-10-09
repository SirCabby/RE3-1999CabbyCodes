#pragma once

#include <windows.h>

#include <cstdint>

#include "mem.h"

// The game executable's sections, resolved once. Enigma blanks the section
// names, so they are taken by position: the wrapper keeps the 1999 image's own
// sections first, at their original addresses (.text, .rdata, .data, .idata,
// .reloc, .rsrc), and appends its own after them. Everything that scans the
// image goes through here so the bounds are consistent and cheap to reuse.
namespace re3cc::image {

void init();
HMODULE module();
uintptr_t base();
bool layout_ok();                // the first three sections look like code, rdata and data
const mem::Range& text();        // the game's code (section 0)
const mem::Range& rdata();       // section 1: constants, and the import address table at its front
const mem::Range& data();        // section 2, .bss included (VirtualSize > SizeOfRawData)
const mem::Range& game_image();  // base .. the end of the 1999 image (before Enigma's sections)
const mem::Range& all();         // the whole module, Enigma's sections included
uint32_t rva(uintptr_t va);      // 0 when outside the image
void log_sections();

// The game's import address table: the run of function pointers at the front of
// .rdata that the code calls through (`call dword ptr [...]`). MSVC puts it
// there and the import descriptors in .idata, which Enigma clears once it has
// bound them - so the table is found by its contents, not by a directory. It is
// empty until find_iat() has run, which needs the image unpacked and bound.
const mem::Range& iat();
bool find_iat();

}  // namespace re3cc::image
