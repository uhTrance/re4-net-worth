// Data model shared by the poller (plugin.cpp) and the in-game window
// (window.cpp). One OutputState is one consistent snapshot of net worth.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace re4 {

constexpr int HERB_KINDS = 10; // HERB_TYPE_COUNT in plugin.cpp

struct ItemEntry {
    std::string name;
    int32_t count = 0;
    int32_t unit_price = 0;
    int32_t total_price = 0;
};

struct TreasureResult {
    std::string name;
    int32_t item_id = 0;
    // Position in the in-game treasure grid, so the page can list treasures
    // in the same order the player sees them. Essential once duplicates
    // exist: two Elegant Masks with different recommended gems are otherwise
    // indistinguishable in the plan.
    int32_t gui_order = 0;
    int32_t duplicate_ordinal = 0; // 1-based among same-named pieces, 0 = unique
    int32_t base_price = 0;
    double final_price = 0.0;
    std::vector<std::string> gems_used;   // what the optimiser wants in here
    std::vector<int32_t> gem_prices;
    std::vector<std::string> actual_gems; // what is physically socketed right now
};

// treasure item id -> one gem-name list per physical piece of that treasure.
// A vector-of-vectors rather than a flat list so two copies of the same
// treasure keep their own socket contents instead of being merged.
using ActualSockets = std::unordered_map<int32_t, std::vector<std::vector<std::string>>>;

// treasure item id -> the grid position of each physical piece, in the same
// per-piece order as ActualSockets above.
using GuiOrder = std::unordered_map<int32_t, std::vector<int32_t>>;

// One currently-purchasable Trade shop listing (Separate Ways). Spinels are a
// separate, closed currency - they cannot convert back to pesetas, only spend
// into one of these - so this is advisory only and never folds into `total`,
// the same reasoning that kept crafting out of the total.
struct TradeItemEntry {
    std::string name;
    int32_t spinel_cost = 0;
    int32_t item_count = 0;
    int32_t unit_sell_price = 0;  // 0 if the reward is not a sellable item at all
    int32_t total_sell_value = 0;
};

// A request that is completed by SELLING specific items, where the items are
// already in hand. Selling them pays pesetas (counted normally as items) AND
// awards Spinels, so the Spinels are extra budget that costs nothing to take.
struct SellQuestEntry {
    std::string requirement; // e.g. "Viper x3"
    int32_t spinels = 0;
};

// One line of the advisory ammo-crafting plan - see CRAFT_RECIPES.
struct CraftPlanEntry {
    std::string name;
    std::string local_name; // the ammo's name in the game's language (window only)
    int32_t times = 0;   // how many times to run this recipe
    int32_t rounds = 0;  // total ammo produced
    int32_t value = 0;   // pesetas that ammo is worth
};

struct OutputState {
    int32_t pesetas = 0;
    std::vector<ItemEntry> items;
    std::vector<TreasureResult> treasures;
    std::vector<ItemEntry> plain_treasures;
    std::vector<ItemEntry> leftover_gems;
    std::vector<ItemEntry> storage;   // the typewriter Storage / "Armoury"
    std::vector<ItemEntry> key_items; // only those the game currently allows selling
    int32_t spinel_count = 0;
    std::vector<TradeItemEntry> trade_items; // recievable and not yet claimed
    // The extra combined-treasure value achievable by buying specific gems/
    // treasures from the Trade shop and combining them - unlike the rest of
    // trade_items above, this DOES fold into `total`, at the user's explicit
    // request: unlike crafting, buying here is non-destructive and reversible
    // (bought gems can be un-socketed the same as any other), so the gain is
    // real and actionable, not merely hypothetical.
    double trade_shop_gain = 0.0;
    std::vector<TradeItemEntry> trade_shop_recommended;
    std::vector<TreasureResult> trade_shop_combos; // what to socket into what's bought
    // Herb combines for the plan's end state (after purchases), which can
    // differ entirely from the owned-only steps in herb_steps.
    std::vector<std::string> trade_shop_herb_steps;
    double trade_shop_herb_total = 0.0;

    // Herbs are valued at their best achievable combination rather than
    // as-is, since combining is free and strictly one-way. herb_total is the
    // optimised figure and is what feeds `total`; herb_raw_total is what the
    // same herbs would fetch unmixed, so the page can show the difference.
    std::vector<ItemEntry> herbs;      // what is held now, at as-is prices
    std::vector<std::string> herb_steps; // e.g. "Green Herb + Red Herb"
    double herb_total = 0.0;
    double herb_raw_total = 0.0;

    // Advisory only - see the CRAFT_RECIPES comment for why this never
    // touches `total`.
    std::vector<CraftPlanEntry> craft_plan;
    double craft_gain = 0.0;

    // Spinels sitting in completed-but-unclaimed requests. Confirmed against
    // the live game: getCompletedQuestOrder() returned exactly the 4 the user
    // had pending, and 0 when everything had been handed in. Spendable only
    // after turning them in, so the plan flags that rather than assuming it.
    int32_t spinel_pending = 0;
    int32_t pending_quest_count = 0;

    int32_t chapter_id = -1;
    std::string chapter_name;
    int32_t difficulty = -1;

    // Requests completable right now by selling items already held.
    std::vector<SellQuestEntry> sell_quests;
    int32_t spinel_from_sales = 0;
    double total = 0.0;
    int64_t updated_at = 0;

    // Window only, not written to net_worth.json. The JSON keeps the English
    // herb names its readers parse; the window shows the game's own names.
    std::array<std::string, HERB_KINDS> herb_english;
    std::array<std::string, HERB_KINDS> herb_local;
    int32_t ammo_bonus_pct = 0;
    int32_t heal_bonus_pct = 0;
};

} // namespace re4
