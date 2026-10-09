#include "image.h"

#include "log.h"

namespace re3cc::image {
namespace {

HMODULE g_module = nullptr;
uintptr_t g_base = 0;
mem::Range g_text, g_rdata, g_data, g_iat, g_game, g_all;
bool g_layout_ok = false;

// The loaded modules' address ranges, minus the game itself: an import slot
// holds an address inside one of them. Taken once, from a VirtualQuery walk, so
// testing thousands of candidate slots costs nothing.
struct Images {
  mem::Range r[96];
  int n = 0;
  bool contains(uintptr_t a) const {
    for (int i = 0; i < n; ++i)
      if (r[i].contains(a)) return true;
    return false;
  }
};

Images other_modules() {
  Images out;
  MEMORY_BASIC_INFORMATION mbi{};
  uintptr_t a = 0x10000;
  while (a < 0xFFFF0000u && out.n < 96 && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) {
    const uintptr_t alloc = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE && alloc && alloc != g_base) {
      if (out.n && out.r[out.n - 1].begin == alloc) out.r[out.n - 1].end = end;  // same module, next section
      else out.r[out.n++] = mem::Range{alloc, end};
    }
    if (end <= a) break;
    a = end;
  }
  return out;
}

}  // namespace

void init() {
  if (g_module) return;
  g_module = GetModuleHandleA(nullptr);
  g_base = reinterpret_cast<uintptr_t>(g_module);
  g_all = mem::module_range(g_module);
  IMAGE_NT_HEADERS* nt = mem::nt_headers(g_module);
  if (!nt || nt->FileHeader.NumberOfSections < 6) return;
  const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
  g_text = mem::section_range(g_module, s[0]);
  g_rdata = mem::section_range(g_module, s[1]);
  g_data = mem::section_range(g_module, s[2]);
  // The 1999 image ends where its last original section (5) ends; Enigma's own
  // start after it.
  g_game = mem::Range{g_base, mem::section_range(g_module, s[5]).end};
  g_layout_ok = (s[0].Characteristics & IMAGE_SCN_MEM_EXECUTE) && g_text.begin < g_rdata.begin &&
                g_rdata.begin < g_data.begin && !g_rdata.empty() && !g_data.empty();
}

HMODULE module() { return g_module; }
uintptr_t base() { return g_base; }
bool layout_ok() { return g_layout_ok; }
const mem::Range& text() { return g_text; }
const mem::Range& rdata() { return g_rdata; }
const mem::Range& data() { return g_data; }
const mem::Range& iat() { return g_iat; }
const mem::Range& game_image() { return g_game; }
const mem::Range& all() { return g_all; }
uint32_t rva(uintptr_t va) { return g_all.contains(va) ? static_cast<uint32_t>(va - g_base) : 0; }

// The import table by its shape: a stretch of .rdata whose dwords are either
// addresses inside some other loaded module (one import) or zero (the null
// terminator between two DLLs' blocks). Constants in .rdata are not addresses
// of code in kernel32, so the run the table occupies stands out; it is accepted
// only when it is long enough to be an import table and is the longest such run.
bool find_iat() {
  init();
  if (!g_iat.empty()) return true;  // found once, on the first caller
  if (g_rdata.empty()) return false;
  const Images mods = other_modules();
  if (!mods.n) return false;
  constexpr int kMinImports = 32;
  uintptr_t best_begin = 0, best_end = 0;
  int best_ptrs = 0;
  uintptr_t begin = 0, end = 0;
  int ptrs = 0;
  auto close = [&] {
    if (ptrs > best_ptrs) {
      best_ptrs = ptrs;
      best_begin = begin;
      best_end = end;
    }
    begin = end = 0;
    ptrs = 0;
  };
  for (uintptr_t a = g_rdata.begin; a + 4 <= g_rdata.end; a += 4) {
    uint32_t v = 0;
    if (!mem::read_safe(a, &v)) break;
    if (v && mods.contains(v)) {
      if (!begin) begin = a;
      end = a + 4;
      ++ptrs;
    } else if (!v && begin) {
      // A zero inside the table separates two DLLs' blocks; two in a row end it.
      uint32_t next = 0;
      if (!mem::read_safe(a + 4, &next) || (!next && a + 8 <= g_rdata.end)) close();
    } else if (begin) {
      close();
    }
  }
  close();
  if (best_ptrs < kMinImports) {
    logf("imports: no import table in .rdata (the longest run of module pointers held %d)", best_ptrs);
    return false;
  }
  g_iat = mem::Range{best_begin, best_end};
  logf("imports: the game's import table is exe+0x%X..0x%X (%d slots, %d of them filled)", rva(best_begin),
       rva(best_end), static_cast<int>(g_iat.size() / 4), best_ptrs);
  return true;
}

void log_sections() {
  IMAGE_NT_HEADERS* nt = mem::nt_headers(g_module);
  if (!nt) {
    logf("image: no PE headers at %p", static_cast<void*>(g_module));
    return;
  }
  logf("image: base %p, SizeOfImage 0x%lX, entry RVA 0x%lX, %u sections (layout %s)", static_cast<void*>(g_module),
       nt->OptionalHeader.SizeOfImage, nt->OptionalHeader.AddressOfEntryPoint, nt->FileHeader.NumberOfSections,
       g_layout_ok ? "as expected" : "NOT as expected");
  const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    char name[9] = {};
    std::memcpy(name, s[i].Name, 8);
    logf("image:   [%2u] '%s' va 0x%08lX vsize 0x%08lX raw 0x%08lX flags 0x%08lX", i, name, s[i].VirtualAddress,
         s[i].Misc.VirtualSize, s[i].SizeOfRawData, s[i].Characteristics);
  }
}

}  // namespace re3cc::image
