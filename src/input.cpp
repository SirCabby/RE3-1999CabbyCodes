#include "input.h"

#include "config.h"
#include "game.h"
#include "log.h"
#include "mem.h"

namespace re3cc::input {
namespace {

bool g_on = false;

bool both_sites() {
  return game::site(game::kSiteInputMain) && game::site(game::kSiteInputRepeat);
}

}  // namespace

bool guarding() { return g_on; }

void tick(bool capture) {
  if (config::get().disable_input || !both_sites()) {
    if (g_on) uninstall();
    return;
  }
  if (capture == g_on) return;
  mem::Patch* a = game::site(game::kSiteInputMain);
  mem::Patch* b = game::site(game::kSiteInputRepeat);
  if (capture) {
    // Both or neither: one store left alive would keep feeding the game half a
    // frame's buttons, which is worse than not guarding at all.
    if (!a->apply()) return;
    if (!b->apply()) {
      a->revert();
      return;
    }
    game::clear_input_state();
    g_on = true;
    logf("input: the mod has the keyboard (a panel field or the cutscene skip) - the game's buttons are held up");
  } else {
    a->revert();
    b->revert();
    game::clear_input_state();
    g_on = false;
    logf("input: the keyboard is the game's again");
  }
}

void uninstall() {
  if (!both_sites()) return;
  if (mem::Patch* a = game::site(game::kSiteInputMain))
    if (a->applied) a->revert();
  if (mem::Patch* b = game::site(game::kSiteInputRepeat))
    if (b->applied) b->revert();
  if (g_on) game::clear_input_state();
  g_on = false;
}

}  // namespace re3cc::input
