// RE4 Net Worth - REFramework native plugin
//
// Values everything you own at what the merchant would pay, finds the best
// treasure/gem and herb combinations, and writes the result to
// reframework/data/net_worth.json. The in-game window and widget live in
// window.cpp; this file is the poller and the optimisers.
//
// WHY NATIVE: an earlier Lua version cost ~15 FPS. Two things drove that - it
// re-resolved every item's display name through the game's message system on
// every single poll, and it serialised and rewrote the JSON file on the render
// thread at the same rate. Here names are resolved once and cached forever,
// and serialisation plus disk I/O happen on a background thread.
//
// STORAGE IS INCLUDED. The Lua version could not do this and the earlier
// searches all failed, because the game does not call it "storage" anywhere -
// internally the typewriter box is the "Armoury"
// (chainsaw.ArmouryManager._ArmouryItemList). Every previous attempt guessed
// names like ItemBox/StorageManager/StockManager and missed it. It was found by
// walking the entire type database (162k types, 1.1M methods) via the C++ API,
// which the Lua binding cannot do - sdk.get_tdb is not callable there.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include "re4_common.hpp"
#include "state.hpp"
#include "window.hpp"

namespace {

using namespace re4;

constexpr auto POLL_INTERVAL = std::chrono::milliseconds(100);
constexpr uint32_t MAX_ITEMS = 4096;

// GUIs during which the game is actively mutating the very inventory and
// treasure structures this plugin walks. Reading them 10x/sec from the render
// thread while a combine or a sale is being applied risks catching them
// mid-mutation - which faults inside game code, not here, and matches the
// crashes seen while combining gems at the merchant. Polling pauses while any
// of these are open; the numbers simply refresh once you close them.
//
// Resolved by NAME from the GuiType enum at startup rather than hardcoded:
// only a couple of these ids were ever verified directly, and a wrong id here
// would silently watch the wrong screen.
// The shop, combine, and storage screens used to be listed here as crash
// mitigation. They are deliberately NOT any more: that pause was added before
// the real cause was known, crashes continued unchanged with it in place, and
// the actual fix turned out to be the missing local_frame_gc() call. Keeping
// it only meant the figures froze for the entire merchant visit - exactly when
// they are most useful - for no demonstrated safety benefit.
//
// Every read is already exception-guarded and re-fetched fresh each poll, so
// the worst case from reading mid-mutation is one stale figure for 100ms, not
// a fault. If instability ever returns around the merchant, re-adding
// "InGameShop", "InGameShopSelect", "InGameShopSaleInfo",
// "KeyTreasureInventory", "KeyTreasureCombine", "Armoury", "ArmouryTab" and
// "ArmourySelect" to this list restores the old behaviour exactly.
//
// Death stays: an exception was observed from
// TreasureInventoryController.getItems() at the exact moment of death, and
// nothing useful updates on that screen anyway.
const char* const BUSY_GUI_NAMES[] = {
    "GameOver", "GameOver_1011", "GameOver_1021", "SimpleGameOver_1021",
};
constexpr int32_t RENDER_OUTPUT_DEFAULT = 2;

// ---------------------------------------------------------------------------
// Gem / treasure rules - mirrors the Lua version
// ---------------------------------------------------------------------------

// The only real gemstones, i.e. the only things usable as combine-fuel in a
// treasure's sockets. Everything else in the treasure inventory that is not
// itself a socketed treasure is a "For selling only" piece and must simply be
// sold as-is. Matched by ItemID, so it works in every game language (display
// names are localised). The old English name match also caught Scratched
// Emerald (120865600), a Separate Ways request item sold as-is, not a gem.
// Gem shapes. A socket only accepts a gem of its own shape, so this is a hard
// constraint on which combinations are actually possible in game.
//
// HARDCODED, and deliberately so: the game exposes each SOCKET's shape (see
// TreasureCombinePositionSetting._Shape, which reads as Circle=0 / Square=1 and
// matches observed treasures exactly - Elegant Mask [0,0,0], Chalice of
// Atonement [1,1,1]), but nothing reachable exposes a GEM's shape. Their
// ItemDefinitions are byte-identical (same size, same stack max), their combine
// settings are empty and getJewelJointKind returns -1 for all of them. So the
// mapping below comes from the game itself as played, not from its data. It is
// six fixed items that will not change.
enum GemShape : int32_t { SHAPE_CIRCLE = 0, SHAPE_SQUARE = 1, SHAPE_NONE = -1 };

// Five colours across six gems - Ruby and Red Beryl are both red. This matters:
// the bonus tiers are defined by COLOUR, not by item, so two of them score as a
// Duo rather than as "two colours".
enum GemColor : int32_t { COLOR_RED = 0, COLOR_BLUE, COLOR_YELLOW, COLOR_GREEN, COLOR_PURPLE, COLOR_COUNT };

struct GemType {
    int32_t item_id;
    int32_t shape;
    int32_t color;
};

// Shape and colour are independent: each shape offers three distinct colours,
// so a same-shape treasure can still reach the all-different tiers.
const GemType GEM_TYPES[] = {
    {120830400, SHAPE_CIRCLE, COLOR_RED},    // Ruby
    {120833600, SHAPE_CIRCLE, COLOR_BLUE},   // Sapphire
    {120835200, SHAPE_CIRCLE, COLOR_YELLOW}, // Yellow Diamond
    {120857600, SHAPE_SQUARE, COLOR_RED},    // Red Beryl
    {120832000, SHAPE_SQUARE, COLOR_GREEN},  // Emerald
    {120838400, SHAPE_SQUARE, COLOR_PURPLE}, // Alexandrite
};
constexpr int GEM_TYPE_COUNT = (int)(sizeof(GEM_TYPES) / sizeof(GEM_TYPES[0]));

// Index into GEM_TYPES, or -1 if this is not one of the six real gemstones
// (i.e. it is a "For selling only" treasure piece).
int gem_type_of(int32_t item_id) {
    if (item_id <= 0) return -1;
    for (int i = 0; i < GEM_TYPE_COUNT; ++i) {
        if (GEM_TYPES[i].item_id == item_id) return i;
    }
    return -1;
}

bool is_real_gemstone(int32_t item_id) {
    return gem_type_of(item_id) >= 0;
}

// ---------------------------------------------------------------------------
// Herbs
// ---------------------------------------------------------------------------
// Mixed herbs are distinct items with their own sell prices, and combining is
// strictly one-way, so the question "what are my herbs worth" is really "what
// is the best sequence of combines available from here". Prices are read live
// from the game rather than hardcoded - only the ITEM IDS and the recipe
// graph below are fixed, since neither is derivable from the price data.
//
// Verified against the live game: G 500, R 1000, Y 3000, G+G 1200, G+G+G
// 2000, G+R 2000, G+Y 4000, R+Y 5000, G+G+Y 5000, G+R+Y 10000. G+R+Y stands
// far above everything else (10000 from parts worth 4500), which is what
// makes buying a Yellow Herb from the Trade shop worthwhile when spare green
// and red are already on hand.
enum HerbType : int {
    HERB_G = 0, HERB_R, HERB_Y,
    HERB_GG, HERB_GGG, HERB_GR, HERB_GY, HERB_RY, HERB_GRY, HERB_GGY,
    HERB_TYPE_COUNT
};

constexpr int32_t HERB_ITEM_IDS[HERB_TYPE_COUNT] = {
    114400000, // G
    114401600, // R
    114403200, // Y
    114404800, // G+G
    114406400, // G+G+G
    114408000, // G+R
    114409600, // G+Y
    114411200, // R+Y
    114412800, // G+R+Y
    114414400, // G+G+Y
};

static_assert(HERB_TYPE_COUNT == HERB_KINDS, "state.hpp's HERB_KINDS must match");

const char* const HERB_NAMES[HERB_TYPE_COUNT] = {
    "Green Herb", "Red Herb", "Yellow Herb",
    "Mixed Herb (G+G)", "Mixed Herb (G+G+G)", "Mixed Herb (G+R)",
    "Mixed Herb (G+Y)", "Mixed Herb (R+Y)", "Mixed Herb (G+R+Y)",
    "Mixed Herb (G+G+Y)",
};

// Every legal combine, as (input, input) -> output. Combining is always two
// items into one, including mixed-into-single, which is why an existing
// Mixed Herb (G+R) is not a dead end - it can still absorb a Yellow.
struct HerbRecipe { int a; int b; int result; };
constexpr HerbRecipe HERB_RECIPES[] = {
    {HERB_G,  HERB_G, HERB_GG},
    {HERB_G,  HERB_R, HERB_GR},
    {HERB_G,  HERB_Y, HERB_GY},
    {HERB_R,  HERB_Y, HERB_RY},
    {HERB_GG, HERB_G, HERB_GGG},
    {HERB_GG, HERB_Y, HERB_GGY},
    {HERB_GR, HERB_Y, HERB_GRY},
    {HERB_GY, HERB_R, HERB_GRY},
    {HERB_RY, HERB_G, HERB_GRY},
};

int herb_type_of_id(int32_t item_id) {
    for (int i = 0; i < HERB_TYPE_COUNT; ++i) {
        if (HERB_ITEM_IDS[i] == item_id) return i;
    }
    return -1;
}

using HerbCounts = std::array<int32_t, HERB_TYPE_COUNT>;

// ---------------------------------------------------------------------------
// Ammo crafting
// ---------------------------------------------------------------------------
// ADVISORY ONLY - deliberately excluded from the net worth total. Crafting is
// irreversible and the ammo has real gameplay value, so folding it in would
// silently assume every resource gets converted purely to sell, which is not
// a thing anyone actually does mid-run. Gems are reversible and herbs improve
// both sell value and healing, which is why those two DO count.
//
// Yields are the base amounts (no "Recipe:" item unlocked). Crafting also has
// a CHANCE of bonus ammo (handgun +5), deliberately ignored: a probabilistic
// upside must not inflate a figure that is meant to be a guaranteed floor.
//
// IDs and prices verified live in-game; prices are still read at runtime so
// difficulty scaling and any future balance changes are picked up.
constexpr int32_t ITEM_GUNPOWDER    = 117600000;

// Trade shop entries never worth buying purely to resell, so the buy plan
// skips them even when Spinels are left over. Gunpowder is the clearest case:
// 2 Spinels for 10 rounds worth 500 pesetas is 250/Spinel, against ~1,000 for
// a weapon and ~2,300 for a gem, and nobody spends Spinels on powder to flip
// it. Excluding beats deprioritising - a plan that suggests something you
// would never do is worse than one that leaves a couple of Spinels unspent.
//
// Note this does not affect crafting, which is advisory and values powder you
// already own; it only stops the plan recommending you BUY it.
// Items the merchant genuinely buys even though isSellable() says otherwise.
//
// The Exclusive Upgrade Ticket sells for 10,000 (confirmed against the live
// game: getSellPrice reports 10000 while isSellable returns false), so gating
// on isSellable alone silently dropped it from the total.
constexpr int32_t ITEM_EXCLUSIVE_UPGRADE_TICKET = 120867200;

constexpr int32_t SELLABLE_DESPITE_GATE[] = { ITEM_EXCLUSIVE_UPGRADE_TICKET };

bool sellable_despite_gate(int32_t item_id) {
    for (const auto id : SELLABLE_DESPITE_GATE) {
        if (id == item_id) return true;
    }
    return false;
}

constexpr int32_t BUY_PLAN_EXCLUDED[] = { ITEM_GUNPOWDER };

bool excluded_from_buy_plan(int32_t item_id) {
    for (const auto id : BUY_PLAN_EXCLUDED) {
        if (id == item_id) return true;
    }
    return false;
}
constexpr int32_t ITEM_RESOURCES_L  = 117601600;
constexpr int32_t ITEM_RESOURCES_S  = 117606400;

struct CraftRecipe {
    const char* name;
    int32_t resource_id;   // Resources (S) or (L)
    int32_t powder;
    int32_t ammo_id;
    int32_t yield;
};

constexpr CraftRecipe CRAFT_RECIPES[] = {
    {"Handgun Ammo",       ITEM_RESOURCES_S,  5, 112800000, 15},
    {"Shotgun Shells",     ITEM_RESOURCES_S, 12, 112803200,  7},
    {"Submachine Gun Ammo",ITEM_RESOURCES_L,  6, 112806400, 40},
    {"Rifle Ammo",         ITEM_RESOURCES_L, 10, 112804800,  9},
};
constexpr int CRAFT_RECIPE_COUNT = (int)(sizeof(CRAFT_RECIPES) / sizeof(CRAFT_RECIPES[0]));

// The full in-game bonus table, keyed on how the chosen gems partition by
// COLOUR. Anything not listed earns no bonus at all (x1.0) - so, for example,
// two reds plus one blue across three sockets is worth strictly less than
// either three reds or three different colours, and must never be recommended.
//
//   1+1        Two Colors  x1.1        2          Duo        x1.2
//   1+1+1      Three Colors x1.3       3          Trio       x1.4
//   2+2        Two Duos    x1.5        1+1+1+1    Four Colors x1.6
//   4          Quartet     x1.7        3+2        Duo & Trio x1.8
//   5          Quintet     x1.9        1+1+1+1+1  Five Colors x2.0
double combo_multiplier(const int32_t color_counts[COLOR_COUNT]) {
    int32_t parts[COLOR_COUNT];
    int n_parts = 0;
    int32_t total = 0;
    for (int c = 0; c < COLOR_COUNT; ++c) {
        if (color_counts[c] > 0) {
            parts[n_parts++] = color_counts[c];
            total += color_counts[c];
        }
    }
    if (total < 2) return 1.0;
    std::sort(parts, parts + n_parts, std::greater<int32_t>());

    switch (total) {
    case 2: return (n_parts == 1) ? 1.2 : 1.1;
    case 3:
        if (n_parts == 1) return 1.4;
        if (n_parts == 3) return 1.3;
        return 1.0; // 2+1 earns nothing
    case 4:
        if (n_parts == 1) return 1.7;
        if (n_parts == 4) return 1.6;
        if (n_parts == 2 && parts[0] == 2) return 1.5; // two duos
        return 1.0;
    case 5:
        if (n_parts == 1) return 1.9;
        if (n_parts == 5) return 2.0;
        if (n_parts == 2 && parts[0] == 3 && parts[1] == 2) return 1.8; // duo & trio
        return 1.0;
    default: return 1.0;
    }
}

// Value formula:  finalPrice = (treasureBase + sum(gem prices)) * multiplier
//
// Computed here rather than asked of the game. An earlier version simulated each
// candidate by creating a scratch chainsaw.TreasureCombineInfo and calling the
// game's own getBonusPrice(); that path proved unstable under repetition - even
// a small batch of correctly ref-counted create/release cycles started throwing
// native exceptions, and an uncapped version of it crashed the game outright.
//
// The formula itself was validated against real getBonusPrice() output for four
// owned treasures and matched exactly, and the multiplier table above comes
// from the game's own in-game bonus screen.

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------
// The structs live in state.hpp, shared with the window.

// ---------------------------------------------------------------------------
// Resolved game handles
// ---------------------------------------------------------------------------
struct Resolved {
    bool ready = false;
    bool disabled = false;

    reframework::API::ManagedObject* inventory_manager = nullptr;
    reframework::API::ManagedObject* shop_manager = nullptr;
    reframework::API::ManagedObject* treasure_manager = nullptr;
    reframework::API::ManagedObject* item_msg_manager = nullptr;
    reframework::API::ManagedObject* msg_manager = nullptr;
    reframework::API::ManagedObject* armoury_manager = nullptr;
    reframework::API::ManagedObject* item_manager = nullptr;

    reframework::API::Field* controller_table_field = nullptr;
    reframework::API::Field* entries_field = nullptr;
    reframework::API::Field* armoury_list_field = nullptr;

    reframework::API::Method* get_curr_ptas = nullptr;
    reframework::API::Method* sell_price_by_id = nullptr;
    reframework::API::Method* sell_price_by_item = nullptr;
    reframework::API::Method* get_combine_setting = nullptr;
    // Trade shop (Separate Ways). Both live on the same shop_manager already
    // resolved above for sell prices.
    reframework::API::Method* get_curr_spinel_count = nullptr;
    reframework::API::Method* get_rewardable_items = nullptr;

    // Current chapter + difficulty, for per-chapter profit/loss analysis.
    // ChapterID is an enum whose numeric values are sparse and non-obvious
    // (cp10_chp1_1 = 21100), so the names are resolved once at startup and
    // reported alongside the raw id rather than decoded arithmetically.
    // Charm / status-effect bonuses. Optional: absent just means no bonus is
    // applied, which is what happened before charms were handled.
    reframework::API::ManagedObject* status_effect_manager = nullptr;
    reframework::API::Method* get_curr_status_effect_value = nullptr;

    reframework::API::ManagedObject* campaign_manager = nullptr;
    reframework::API::Method* get_current_chapter = nullptr;
    reframework::API::Method* get_current_difficulty = nullptr;
    std::unordered_map<int32_t, std::string> chapter_names;

    // Weapon upgrade value. getSellPrice() returns only the BASE price - it
    // never includes upgrades, even though the merchant's sell screen does.
    // Confirmed against two weapons in-game: Red9 (base 7,000 + 15,000 spent
    // -> merchant offered 21,250) and SG-09 R (base 4,000 + 4,000 spent ->
    // 7,800). Both land exactly on base + 95% of spend, so that is the rule.
    reframework::API::ManagedObject* weapon_custom_manager = nullptr;
    reframework::API::Method* get_cost_sum = nullptr;
    reframework::API::Field* common_levels_field = nullptr;
    reframework::API::Field* individual_levels_field = nullptr;

    // Completed-but-unclaimed requests, for the spendable-Spinel figure.
    reframework::API::ManagedObject* quest_manager = nullptr;
    reframework::API::Method* get_completed_quests = nullptr;
    reframework::API::Field* quest_reward_field = nullptr;   // QuestOrder.Reward
    reframework::API::Method* quest_reward_count = nullptr;  // QuestReward.get_Count

    // Sell-to-complete requests: enumerate every order, read what it wants.
    reframework::API::Method* get_quest_orders = nullptr;
    reframework::API::Method* quest_is_cleared = nullptr;
    reframework::API::Method* quest_clear_item = nullptr;
    reframework::API::Field* dict_entries_field = nullptr; // Dictionary._entries
    reframework::API::Method* get_item_name_msg_id = nullptr;
    reframework::API::Method* get_message = nullptr;
    reframework::API::Method* is_sellable = nullptr;
    reframework::API::Field* shape_field = nullptr; // TreasureCombinePositionSetting._Shape

    reframework::API::ManagedObject* gui_manager = nullptr;
    reframework::API::Method* is_open_gui = nullptr;
    std::vector<int32_t> busy_gui_types; // see BUSY_GUI_NAMES

    // General "the world is streaming/reloading" signal - catches area
    // transitions and checkpoint reloads that never open any of the named
    // busy GUIs at all, so the name-based list above cannot see them.
    void* scene_manager = nullptr;
    reframework::API::Method* get_scene_loading = nullptr;

    // A crash was observed inside share.SaveDataManager.execSave (an autosave)
    // while polling was actively running, unguarded, mid-gameplay - no shop,
    // combine, or death screen involved. Autosave serialises the very
    // inventory/treasure structures this plugin walks with raw pointers and no
    // locking, so a concurrent read during that write is a plausible cause of
    // exactly this class of access violation. get_IsBusy() is the broadest of
    // SaveDataManager's status flags, covering save AND load alike.
    reframework::API::ManagedObject* save_data_manager = nullptr;
    reframework::API::Method* save_is_busy = nullptr;

    // For recovering gems already socketed into treasures - see
    // collect_socketed_gems().
    reframework::API::Field* treasure_inventory_field = nullptr; // Controller.<_Inventory>
    reframework::API::Field* treasure_items_field = nullptr;     // TreasureInventory._Items
    reframework::API::Field* combined_item_id_field = nullptr;   // CombineInfo.Data._CombinedItemId
};

Resolved g_res;

// itemId -> display name. Names never change, so this is resolved once per item
// and reused forever - this is the single biggest saving over the Lua version,
// which re-resolved every name on every poll.
std::unordered_map<int32_t, std::string> g_name_cache;

// itemId -> the shape of each of its sockets (empty = not a combinable
// treasure). Static per item type, so resolved once and cached.
std::unordered_map<int32_t, std::vector<int32_t>> g_socket_cache;

// TypeDefinition* -> method name -> Method*. A type's methods never change, so
// like the caches above this is resolved once and reused - unlike the caches
// above, the sites that needed this were never converted: get_Count/get_Item/
// get_ItemId/get_CurrentItemCount/getCombineInfo/get_Datas were being
// re-resolved by name via find_method() inside the per-item loop, i.e. once
// per item per controller per 100ms poll. With a modest inventory that is
// hundreds of fresh reflection lookups a second, unrelated to anything the
// player does - which fits a crash observed to recur even while standing
// completely idle, purely as a function of elapsed poll count.
std::unordered_map<void*, std::unordered_map<std::string, reframework::API::Method*>> g_method_cache;

reframework::API::Method* cached_find_method(reframework::API::TypeDefinition* type, const char* name) {
    if (type == nullptr) return nullptr;
    auto& by_name = g_method_cache[(void*)type];
    auto it = by_name.find(name);
    if (it != by_name.end()) return it->second;
    auto* m = type->find_method(name);
    by_name.emplace(name, m); // cache misses too, so a bad name never re-scans
    return m;
}

// Method::call<T>() calls the resolved function pointer directly - no
// exception handling, no argument-marshalling safety net, unlike invoke()
// (used everywhere else in this file), which goes through REFramework's own
// protected call path and reports exception_thrown. call<T>() was used
// exclusively in the hot per-item poll loop - once per item, per controller,
// per 100ms poll - for get_Count/get_Item/get_ItemId/get_CurrentItemCount. If
// any of those calls ever throws, or the raw C-ABI call doesn't match what
// the callee actually expects, call<T>() has no way to detect it: it just
// returns whatever happened to land in the return register, and any damage
// from a bad call is silent until something unrelated stumbles over it later.
// That fits a crash recurring purely from elapsed poll count, with no plugin
// frame ever appearing in the stack, far better than anything read-pattern
// related. Routing the same calls through invoke() instead costs nothing
// extra (the underlying native call is identical) and makes a thrown
// exception something the caller can actually see and skip, the same way
// every invoke() site elsewhere in this file already does.
int32_t call_int32(reframework::API::Method* m, reframework::API::ManagedObject* obj) {
    if (m == nullptr) return 0;
    auto ret = m->invoke(obj, std::vector<void*>{});
    if (ret.exception_thrown) return 0;
    return (int32_t)ret.dword;
}

std::string g_data_dir;

// ---------------------------------------------------------------------------
// Writer thread
// ---------------------------------------------------------------------------
std::mutex g_out_mutex;
std::condition_variable g_out_cv;
OutputState g_pending;
bool g_has_pending = false;
std::atomic<bool> g_writer_stop{false};

// Liveness signal. net_worth.json is write-gated on content, so a stale file
// means "nothing changed" and "plugin is dead" equally - there is no way to
// tell them apart from outside, which made a crash investigation unreadable.
// These drive a tiny status file written on a fixed cadence regardless of
// whether anything changed, so liveness is observed rather than inferred.
std::atomic<uint64_t> g_tick{0};
std::atomic<bool> g_paused{false};

void append_item_array(std::string& out, const char* key, const std::vector<ItemEntry>& items) {
    out += "\"";
    out += key;
    out += "\":[";
    bool first = true;
    for (const auto& it : items) {
        if (!first) out += ",";
        first = false;
        out += "{\"name\":\"";
        json_escape_into(out, it.name);
        out += "\",\"count\":";
        out += std::to_string(it.count);
        out += ",\"unitPrice\":";
        out += std::to_string(it.unit_price);
        out += ",\"totalPrice\":";
        out += std::to_string(it.total_price);
        out += "}";
    }
    out += "]";
}

// Bare array, no leading key - callers write their own key/colon so this can
// serve both the primary treasures array and the trade-shop combine plan.
void append_treasure_array(std::string& out, const std::vector<TreasureResult>& treasures) {
    out += "[";
    bool first = true;
    for (const auto& t : treasures) {
        if (!first) out += ",";
        first = false;
        out += "{\"name\":\"";
        json_escape_into(out, t.name);
        out += "\",\"ordinal\":";
        out += std::to_string(t.duplicate_ordinal);
        out += ",\"basePrice\":";
        out += std::to_string(t.base_price);
        out += ",\"finalPrice\":";
        out += std::to_string((long long)(t.final_price + 0.5));
        out += ",\"gemsUsed\":[";
        for (size_t i = 0; i < t.gems_used.size(); ++i) {
            if (i != 0) out += ",";
            out += "{\"name\":\"";
            json_escape_into(out, t.gems_used[i]);
            out += "\",\"unitPrice\":";
            out += std::to_string(i < t.gem_prices.size() ? t.gem_prices[i] : 0);
            out += "}";
        }
        out += "],\"actualGems\":[";
        for (size_t i = 0; i < t.actual_gems.size(); ++i) {
            if (i != 0) out += ",";
            out += "\"";
            json_escape_into(out, t.actual_gems[i]);
            out += "\"";
        }
        out += "]}";
    }
    out += "]";
}

// Defined below, next to the sell-price helpers they belong with.
double ammo_sell_bonus_pct();
double heal_sell_bonus_pct();

std::string build_json(const OutputState& st) {
    std::string out;
    out.reserve(16 * 1024);
    out += "{\"pesetas\":";
    out += std::to_string(st.pesetas);
    out += ",";

    append_item_array(out, "items", st.items);
    out += ",\"treasures\":";
    append_treasure_array(out, st.treasures);
    out += ",";

    append_item_array(out, "plainTreasures", st.plain_treasures);
    out += ",";
    append_item_array(out, "leftoverGems", st.leftover_gems);
    out += ",";
    append_item_array(out, "storage", st.storage);
    out += ",";
    append_item_array(out, "keyItems", st.key_items);

    out += ",\"spinelCount\":";
    out += std::to_string(st.spinel_count);
    out += ",\"tradeItems\":[";
    for (size_t i = 0; i < st.trade_items.size(); ++i) {
        if (i != 0) out += ",";
        const auto& t = st.trade_items[i];
        out += "{\"name\":\"";
        json_escape_into(out, t.name);
        out += "\",\"spinelCost\":";
        out += std::to_string(t.spinel_cost);
        out += ",\"count\":";
        out += std::to_string(t.item_count);
        out += ",\"unitSellPrice\":";
        out += std::to_string(t.unit_sell_price);
        out += ",\"totalSellValue\":";
        out += std::to_string(t.total_sell_value);
        out += "}";
    }
    out += "]";

    out += ",\"tradeShopGain\":";
    out += std::to_string((long long)(st.trade_shop_gain + 0.5));
    out += ",\"tradeShopRecommended\":[";
    for (size_t i = 0; i < st.trade_shop_recommended.size(); ++i) {
        if (i != 0) out += ",";
        const auto& r = st.trade_shop_recommended[i];
        out += "{\"name\":\"";
        json_escape_into(out, r.name);
        out += "\",\"spinelCost\":";
        out += std::to_string(r.spinel_cost);
        out += ",\"count\":";
        out += std::to_string(r.item_count);
        out += "}";
    }
    out += "]";

    out += ",\"tradeShopCombos\":";
    append_treasure_array(out, st.trade_shop_combos);

    out += ",\"tradeShopHerbTotal\":";
    out += std::to_string((long long)(st.trade_shop_herb_total + 0.5));
    out += ",\"tradeShopHerbSteps\":[";
    for (size_t i = 0; i < st.trade_shop_herb_steps.size(); ++i) {
        if (i != 0) out += ",";
        out += "\"";
        json_escape_into(out, st.trade_shop_herb_steps[i]);
        out += "\"";
    }
    out += "]";

    out += ",";
    append_item_array(out, "herbs", st.herbs);
    out += ",\"herbTotal\":";
    out += std::to_string((long long)(st.herb_total + 0.5));
    out += ",\"herbRawTotal\":";
    out += std::to_string((long long)(st.herb_raw_total + 0.5));
    out += ",\"herbSteps\":[";
    for (size_t i = 0; i < st.herb_steps.size(); ++i) {
        if (i != 0) out += ",";
        out += "\"";
        json_escape_into(out, st.herb_steps[i]);
        out += "\"";
    }
    out += "]";

    out += ",\"chapterId\":";
    out += std::to_string(st.chapter_id);
    out += ",\"chapter\":\"";
    json_escape_into(out, st.chapter_name);
    out += "\",\"difficulty\":";
    out += std::to_string(st.difficulty);

    // Charm bonus currently applied to ammo resale, as a percentage. Surfaced
    // so the pages can say the prices include it rather than leaving a 40%
    // swing unexplained.
    out += ",\"ammoSellBonusPct\":";
    out += std::to_string((int32_t)(ammo_sell_bonus_pct() + 0.5));
    out += ",\"healSellBonusPct\":";
    out += std::to_string((int32_t)(heal_sell_bonus_pct() + 0.5));

    // Diagnostic: tells "no charm equipped" apart from "could not read the
    // effect at all". Without it, a failed lookup and a genuinely absent bonus
    // both just report 0, which is exactly what made the first attempt at this
    // hard to diagnose.
    out += ",\"charmEffectReadable\":";
    out += (g_res.status_effect_manager != nullptr
            && g_res.get_curr_status_effect_value != nullptr) ? "true" : "false";

    out += ",\"spinelPending\":";
    out += std::to_string(st.spinel_pending);
    out += ",\"pendingQuestCount\":";
    out += std::to_string(st.pending_quest_count);
    out += ",\"spinelFromSales\":";
    out += std::to_string(st.spinel_from_sales);
    out += ",\"sellQuests\":[";
    for (size_t i = 0; i < st.sell_quests.size(); ++i) {
        if (i != 0) out += ",";
        out += "{\"requirement\":\"";
        json_escape_into(out, st.sell_quests[i].requirement);
        out += "\",\"spinels\":";
        out += std::to_string(st.sell_quests[i].spinels);
        out += "}";
    }
    out += "]";

    out += ",\"craftGain\":";
    out += std::to_string((long long)(st.craft_gain + 0.5));
    out += ",\"craftPlan\":[";
    for (size_t i = 0; i < st.craft_plan.size(); ++i) {
        if (i != 0) out += ",";
        const auto& c = st.craft_plan[i];
        out += "{\"name\":\"";
        json_escape_into(out, c.name);
        out += "\",\"times\":";
        out += std::to_string(c.times);
        out += ",\"rounds\":";
        out += std::to_string(c.rounds);
        out += ",\"value\":";
        out += std::to_string(c.value);
        out += "}";
    }
    out += "]";

    out += ",\"total\":";
    out += std::to_string((long long)(st.total + 0.5));
    out += ",\"updatedAt\":";
    out += std::to_string(st.updated_at);
    out += "}";
    return out;
}

// ~50 bytes once a second, against the ~390KB/sec the write-gating removed.
void write_heartbeat() {
    std::string out = "{\"tick\":";
    out += std::to_string(g_tick.load(std::memory_order_relaxed));
    out += ",\"paused\":";
    out += g_paused.load(std::memory_order_relaxed) ? "true" : "false";
    out += ",\"at\":";
    out += std::to_string((long long)std::time(nullptr));
    out += "}";
    write_file_atomic(g_data_dir + "net_worth_status.json", out);
}

// The browser view's feed: net_worth.json plus the time, the paused flag and
// the window's dark mode setting, which the page follows.
void write_browser_feed(const std::string& json) {
    std::string out = "RE4NW_receive({\"at\":";
    out += std::to_string((long long)std::time(nullptr));
    out += ",\"dark\":";
    out += ui_dark() ? "true" : "false";
    out += ",\"paused\":";
    out += g_paused.load(std::memory_order_relaxed) ? "true" : "false";
    out += ",\"data\":";
    out += json;
    out += "});\n";
    write_file_atomic(g_data_dir + "re4_net_worth\\net_worth.js", out);
}

void writer_thread_main() {
    std::string last_written;
    std::string last_json;
    auto last_beat = std::chrono::steady_clock::now();
    auto last_feed = last_beat;

    while (!g_writer_stop.load(std::memory_order_relaxed)) {
        OutputState st;
        bool have_state = false;
        {
            std::unique_lock<std::mutex> lock(g_out_mutex);
            // Timed wait rather than an indefinite one: the heartbeat has to
            // keep ticking while the plugin is paused, and a paused plugin
            // never signals the condvar.
            g_out_cv.wait_for(lock, std::chrono::milliseconds(500), [] {
                return g_has_pending || g_writer_stop.load(std::memory_order_relaxed);
            });
            if (g_writer_stop.load(std::memory_order_relaxed)) break;
            if (g_has_pending) {
                st = std::move(g_pending);
                g_has_pending = false;
                have_state = true;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_beat >= std::chrono::seconds(1)) {
            last_beat = now;
            write_heartbeat();
        }

        // The browser view opens straight from disk, where fetch() is blocked,
        // so it loads a script instead: RE4NW_receive({...}). Rewritten every
        // couple of seconds even when nothing changed, so the page can tell a
        // live game from a closed one by its timestamp.
        if (ui_browser_export() && !last_json.empty() && now - last_feed >= std::chrono::seconds(2)) {
            last_feed = now;
            write_browser_feed(last_json);
        }

        if (!have_state) continue;

        auto json = build_json(st);
        last_json = json;
        if (ui_browser_export()) {
            last_feed = now;
            write_browser_feed(json);
        }

        // Only touch the disk when something actually changed. updatedAt is
        // excluded from the comparison deliberately: nothing treats this file's
        // timestamp as a liveness signal (unlike the challenge tracker's), so
        // while nothing changes this stops writing entirely rather than
        // rewriting identical content once a second.
        const auto ts_pos = json.rfind(",\"updatedAt\":");
        const auto comparable = (ts_pos == std::string::npos) ? json : json.substr(0, ts_pos);

        if (comparable != last_written) {
            if (write_file_atomic(g_data_dir + "net_worth.json", json)) {
                last_written = comparable;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Game access helpers
// ---------------------------------------------------------------------------
std::string get_item_name(int32_t item_id) {
    const auto it = g_name_cache.find(item_id);
    if (it != g_name_cache.end()) return it->second;

    auto guid_ret = g_res.get_item_name_msg_id->invoke(
        g_res.item_msg_manager, {(void*)(intptr_t)item_id});
    if (guid_ret.exception_thrown) return {};

    // The GUID comes back by value in the return buffer; getMessage takes it as
    // a pointer to that data.
    auto msg_ret = g_res.get_message->invoke(
        g_res.msg_manager, {(void*)guid_ret.bytes.data(), (void*)(intptr_t)0});
    if (msg_ret.exception_thrown) return {};

    auto name = managed_string_to_utf8((reframework::API::ManagedObject*)msg_ret.ptr);
    if (!name.empty()) {
        g_name_cache[item_id] = name; // only cache real results, so a transient
                                      // failure is retried rather than stuck
    }
    return name;
}

// Items the ammo sell charm (Leon w/ shotgun, +40%) covers. The six real ammo
// types, contiguous in the ItemID enum (it_sm70_500..505) at 1,600 apart, plus
// Attachable Mines, which sit elsewhere in the enum but still take the bonus.
// Listed explicitly rather than as a range so a future id landing in a gap
// cannot silently gain it.
//
// Mines were missing until 2026-09-10. The million run sold 5 for 2,940 against
// a 2,100 base - exactly 1.4x, the same ratio as every ammo sale - while hand
// and flash grenades sold at exactly 1.0x. Held mines were valued 40% low.
constexpr int32_t AMMO_ITEM_IDS[] = {
    112800000,  // Handgun Ammo
    112801600,  // Magnum Ammo
    112803200,  // Shotgun Shells
    112804800,  // Rifle Ammo
    112806400,  // Submachine Gun Ammo
    112808000,  // Bolts
    117603200,  // Attachable Mines
};

bool is_ammo_item(int32_t item_id) {
    for (const auto id : AMMO_ITEM_IDS) {
        if (id == item_id) return true;
    }
    return false;
}

// Recovery items - what the health-resale charm covers.
//
// The main run is CONTIGUOUS in the ItemID enum, 114400000 to 114424000 in
// steps of 1600, and is treated as a range rather than a hand-written list.
// The list version silently missed Rhinoceros Beetle (114424000, the recovery
// item - not the charm of the same name at 127241600), because it had been
// built by searching item names for herb/spray/egg/bass/viper and "beetle"
// was not one of the keywords. A range cannot develop that kind of hole.
//
// Verified by walking the block: 16 entries, Green Herb through Rhinoceros
// Beetle, with nothing unrelated in between.
constexpr int32_t HEAL_RANGE_FIRST = 114400000;  // Green Herb
constexpr int32_t HEAL_RANGE_LAST  = 114424000;  // Rhinoceros Beetle (the item)
constexpr int32_t HEAL_RANGE_STEP  = 1600;

// The eggs sit in a separate block of their own, so they stay itemised.
constexpr int32_t HEAL_ITEM_IDS[] = {
    277080256,  // Chicken Egg
    277081856,  // Brown Chicken Egg
    277083456,  // Gold Chicken Egg
};

bool is_heal_item(int32_t item_id) {
    if (item_id >= HEAL_RANGE_FIRST && item_id <= HEAL_RANGE_LAST
        && ((item_id - HEAL_RANGE_FIRST) % HEAL_RANGE_STEP) == 0) {
        return true;
    }
    for (const auto id : HEAL_ITEM_IDS) {
        if (id == item_id) return true;
    }
    return false;
}

// Charm sell-price bonuses, as percentages (40 = +40%).
//
// getSellPrice() does NOT include these - it returns the base and the merchant
// composes the real figure separately, exactly like weapon upgrades. Confirmed
// against the live game: with "Leon w/ shotgun" equipped,
// getCurrStatusEffectValue(700000) reads 40.0 while getSellPrice(Handgun Ammo)
// still reports the base 120.
//
// The 700xxx family is sell-price bonuses. Read off every charm's effects:
//   700000  Leon w/ shotgun     +40   ammunition
//   700100  Rhinoceros Beetle  +100   recovery items
//   700200  Luis Sera           +20   weapons, BASE price only - NOT applied
//
// 700200 is deliberately left out. It applies to a weapon's base price only,
// not to the 95%-of-upgrade-spend component, so on an upgraded weapon it is
// worth very little - and applying it correctly would need a reliable "is this
// a weapon" test that nothing else here requires. If it is ever wanted, the
// place to add it is where the base price is read, BEFORE the upgrade value is
// added, or it will inflate the upgrade component too.
constexpr int32_t STATUS_EFFECT_AMMO_SELL_UP = 700000;
constexpr int32_t STATUS_EFFECT_HEAL_SELL_UP = 700100;

// Cached per poll rather than read per item.
//
// These were being fetched inside every single price lookup, which meant a
// managed call per item per poll - ten times a second, for no new information.
// The value only changes when a charm is equipped or removed, so one read per
// poll is plenty.
double g_ammo_bonus_pct = 0.0;
double g_heal_bonus_pct = 0.0;

double read_status_effect_pct(int32_t effect_id) {
    if (g_res.status_effect_manager == nullptr) return 0.0;
    if (g_res.get_curr_status_effect_value == nullptr) return 0.0;

    // call<float>, NOT invoke().
    //
    // getCurrStatusEffectValue returns System.Single, and a float comes back
    // in a floating-point register - invoke()'s generic byte-buffer return
    // path left InvokeRet::f reading zero, so every bonus silently evaluated
    // to "no charm equipped". Verified against the live game: with the
    // Rhinoceros Beetle equipped, Lua reads 100.0 from this exact method while
    // the invoke() path in this plugin reported 0.
    //
    // call<> takes the real function pointer, so the compiler applies the
    // normal ABI and the float arrives intact. Safe enough here despite the
    // lack of an exception guard: it is a plain getter on a singleton, called
    // once per poll rather than per item.
    auto& api = reframework::API::get();
    const float pct = g_res.get_curr_status_effect_value->call<float>(
        api->get_vm_context(), g_res.status_effect_manager, (void*)(intptr_t)effect_id);

    // Also rejects NaN - a comparison against NaN is false either way.
    if (!(pct > 0.0f) || pct > 500.0f) return 0.0;
    return (double)pct;
}

void refresh_sell_bonuses() {
    g_ammo_bonus_pct = read_status_effect_pct(STATUS_EFFECT_AMMO_SELL_UP);
    g_heal_bonus_pct = read_status_effect_pct(STATUS_EFFECT_HEAL_SELL_UP);
}

double ammo_sell_bonus_pct() { return g_ammo_bonus_pct; }
double heal_sell_bonus_pct() { return g_heal_bonus_pct; }

// Applied here, in the one place every valuation funnels through, so the items
// list, the trade-shop plan and the crafting advisory all agree without each
// having to remember charms exist.
int32_t apply_sell_bonus(int32_t item_id, int32_t base) {
    if (base <= 0) return base;
    double pct = 0.0;
    if (is_ammo_item(item_id))      pct = g_ammo_bonus_pct;
    else if (is_heal_item(item_id)) pct = g_heal_bonus_pct;
    if (pct <= 0.0) return base;
    return (int32_t)(base * (1.0 + pct / 100.0) + 0.5);
}

int32_t sell_price_by_id(int32_t item_id) {
    auto ret = g_res.sell_price_by_id->invoke(g_res.shop_manager, {(void*)(intptr_t)item_id});
    if (ret.exception_thrown) return 0;
    return apply_sell_bonus(item_id, (int32_t)ret.dword);
}

// Total pesetas sunk into a weapon's upgrades.
//
// The obvious accessors do not work: getCommonCustomLevel() and the whole
// family of get*Level() helpers returned 0 for a Red9 that demonstrably had
// AttackUp at level 1, so the levels are read straight out of the level
// structs instead. Each entry carries its category plus a spread of per-stat
// level fields (one AttackUp purchase raises four of them at once), so the
// entry's level is the highest of them. Only System.Int32 fields are read -
// one of them (_ReloadSpeedRateLevel) is a float and would be garbage if
// reinterpreted as an int.
//
// Category numbering differs between the level structs and getCostSum():
// common categories map straight across, individual ones are offset by 3
// (CriticalRate 0 -> gui 3 ... Others 11 -> gui 14).
int32_t weapon_upgrade_investment(reframework::API::ManagedObject* item, int32_t item_id) {
    if (g_res.get_cost_sum == nullptr || g_res.common_levels_field == nullptr ||
        g_res.individual_levels_field == nullptr) {
        return 0;
    }

    auto* it_type = item->get_type_definition();
    auto* get_levels = cached_find_method(it_type, "get_CustomLevelInWeapon");
    if (get_levels == nullptr) return 0;

    auto lv_ret = get_levels->invoke(item, std::vector<void*>{});
    if (lv_ret.exception_thrown || lv_ret.ptr == nullptr) return 0;
    auto* levels = (reframework::API::ManagedObject*)lv_ret.ptr;

    int32_t total = 0;

    const auto walk = [&](reframework::API::Field* list_field, const char* category_field,
                          int32_t gui_offset) {
        auto* list = *(reframework::API::ManagedObject**)list_field->get_data_raw(levels, false);
        if (list == nullptr) return;

        auto* l_type = list->get_type_definition();
        auto* l_count = cached_find_method(l_type, "get_Count");
        auto* l_item = cached_find_method(l_type, "get_Item");
        if (!l_count || !l_item) return;

        const auto n = call_int32(l_count, list);
        for (int32_t i = 0; i < n && n <= 32; ++i) {
            auto e_ret = l_item->invoke(list, {(void*)(intptr_t)i});
            if (e_ret.exception_thrown || e_ret.ptr == nullptr) continue;
            auto* entry = (reframework::API::ManagedObject*)e_ret.ptr;

            auto* e_type = entry->get_type_definition();
            if (e_type == nullptr) continue;

            auto* cat_field = e_type->find_field(category_field);
            if (cat_field == nullptr) continue;
            const auto category = *(int32_t*)cat_field->get_data_raw(entry, false);
            if (category < 0) continue;

            // Highest per-stat level in this entry is the category's level.
            int32_t level = 0;
            for (auto* fld : e_type->get_fields()) {
                if (fld == nullptr || fld == cat_field) continue;
                auto* ft = fld->get_type();
                if (ft == nullptr || ft->get_full_name() != "System.Int32") continue;
                const auto v = *(int32_t*)fld->get_data_raw(entry, false);
                if (v > level) level = v;
            }
            if (level <= 0) continue;

            auto cost = g_res.get_cost_sum->invoke(
                g_res.weapon_custom_manager,
                {(void*)(intptr_t)item_id, (void*)(intptr_t)(category + gui_offset),
                 (void*)(intptr_t)level});
            if (!cost.exception_thrown) total += (int32_t)cost.dword;
        }
    };

    walk(g_res.common_levels_field, "_CommonCustomCategory", 0);
    walk(g_res.individual_levels_field, "_IndividualCustomCategory", 3);
    return total;
}

// The item_id is needed only to apply charm bonuses - the price itself comes
// from the Item instance, so a weapon is still valued in its upgraded state.
//
// This overload has to apply the bonus too, not just sell_price_by_id(). Carried
// and stored inventory is priced through HERE, while herbs are priced through
// the by-id path; applying it in only one of them is why the Rhinoceros Beetle
// visibly doubled herbs while ammo, First Aid Spray, fish and eggs all stayed
// at base.
//
// Applied to the base BEFORE any weapon-upgrade value is added at the call
// sites, which is the correct order - though moot in practice, since
// apply_sell_bonus() only ever touches ammo and recovery items, never weapons.
int32_t sell_price_by_item(reframework::API::ManagedObject* item, int32_t item_id) {
    auto ret = g_res.sell_price_by_item->invoke(g_res.shop_manager, {(void*)item});
    if (ret.exception_thrown) return 0;
    return apply_sell_bonus(item_id, (int32_t)ret.dword);
}

// The shape of each socket on a treasure; empty if it is not combinable.
// A socket only accepts a gem of matching shape, so this drives what
// combinations are actually legal.
const std::vector<int32_t>& get_socket_shapes(int32_t item_id) {
    const auto it = g_socket_cache.find(item_id);
    if (it != g_socket_cache.end()) return it->second;

    std::vector<int32_t> shapes;

    auto ret = g_res.get_combine_setting->invoke(g_res.treasure_manager, {(void*)(intptr_t)item_id});
    if (!ret.exception_thrown && ret.ptr != nullptr) {
        auto* setting = (reframework::API::ManagedObject*)ret.ptr;
        auto* st_type = setting->get_type_definition();
        auto* is_valid = st_type ? st_type->find_method("get_IsValid") : nullptr;
        auto* get_positions = st_type ? st_type->find_method("get_PositionSettings") : nullptr;

        if (is_valid != nullptr && get_positions != nullptr) {
            auto valid_ret = is_valid->invoke(setting, std::vector<void*>{});
            if (!valid_ret.exception_thrown && valid_ret.byte != 0) {
                auto pos_ret = get_positions->invoke(setting, std::vector<void*>{});
                if (!pos_ret.exception_thrown && pos_ret.ptr != nullptr) {
                    auto* arr = (reframework::API::ManagedObject*)pos_ret.ptr;
                    const auto n = *(uint32_t*)((uintptr_t)arr + ARRAY_COUNT_OFFSET);

                    if (n > 0 && n <= 16) {
                        // Array of TreasureCombinePositionSetting references;
                        // each carries the socket's _Shape.
                        const auto* elems = (uintptr_t*)((uintptr_t)arr + ARRAY_DATA_OFFSET);

                        if (g_res.shape_field == nullptr) {
                            auto& api = reframework::API::get();
                            auto* pt = api->tdb()->find_type("chainsaw.TreasureCombinePositionSetting");
                            g_res.shape_field = pt ? pt->find_field("_Shape") : nullptr;
                        }

                        if (g_res.shape_field != nullptr) {
                            for (uint32_t i = 0; i < n; ++i) {
                                if (elems[i] == 0) continue;
                                const auto shape = *(int32_t*)g_res.shape_field->get_data_raw(
                                    (void*)elems[i], false);
                                shapes.push_back(shape);
                            }
                        }
                    }
                }
            }
        }
    }

    return g_socket_cache.emplace(item_id, std::move(shapes)).first->second;
}

// ---------------------------------------------------------------------------
// Treasure optimiser
//
// Finds the genuinely highest-paying assignment of the gems you own across the
// treasures you own, rather than a greedy approximation. The earlier greedy
// version handled biggest-socket treasures first and took whatever looked best
// locally, which could spend a gem that a later treasure needed more - so the
// advice it gave was not actually the maximum.
//
// Two things make an exact search feasible here: there are only ever a handful
// of treasures, and only six gem types. For each treasure the set of distinct
// fills is small (a multiset of up to 5 gems drawn from 3 shape-compatible
// types), so a depth-first search over treasures, memoised on the remaining gem
// pool, closes quickly. A node budget bounds the worst case - if it is ever
// exceeded the best assignment found so far is returned, which is still at
// least as good as the old greedy result.
// ---------------------------------------------------------------------------
struct TreasureCandidate {
    int32_t id;
    int32_t base_price;
    std::vector<int32_t> socket_shapes;
};

using GemCounts = std::array<int32_t, GEM_TYPE_COUNT>;

// One possible way to fill a single treasure.
struct Fill {
    GemCounts used{};   // how many of each gem type this consumes
    double value = 0.0; // resulting sell value of the treasure
};

// Every distinct fill for one treasure, given its socket shapes. Only the best
// fill per gem-usage is kept, so dominated options never enter the search.
std::vector<Fill> enumerate_fills(const TreasureCandidate& t,
                                  const GemCounts& pool,
                                  const std::array<int32_t, GEM_TYPE_COUNT>& prices) {
    int32_t sockets_of_shape[2] = {0, 0};
    for (const auto s : t.socket_shapes) {
        if (s == SHAPE_CIRCLE || s == SHAPE_SQUARE) ++sockets_of_shape[s];
    }

    std::vector<Fill> fills;
    GemCounts chosen{};

    // Walk gem types in order, choosing how many of each to use. Capacity is
    // tracked per shape so a gem can only ever occupy a socket it fits.
    std::function<void(int, int32_t, int32_t)> recurse =
        [&](int type_idx, int32_t circles_left, int32_t squares_left) {
            if (type_idx == GEM_TYPE_COUNT) {
                int32_t color_counts[COLOR_COUNT] = {0};
                int32_t total_gems = 0;
                double gem_value = 0.0;
                for (int i = 0; i < GEM_TYPE_COUNT; ++i) {
                    if (chosen[i] == 0) continue;
                    color_counts[GEM_TYPES[i].color] += chosen[i];
                    total_gems += chosen[i];
                    gem_value += (double)prices[i] * chosen[i];
                }

                Fill f;
                f.used = chosen;
                f.value = ((double)t.base_price + gem_value) * combo_multiplier(color_counts);
                fills.push_back(std::move(f));
                return;
            }

            const auto shape = GEM_TYPES[type_idx].shape;
            const int32_t capacity = (shape == SHAPE_CIRCLE) ? circles_left : squares_left;
            const int32_t max_take = std::min(pool[type_idx], capacity);

            for (int32_t take = 0; take <= max_take; ++take) {
                chosen[type_idx] = take;
                recurse(type_idx + 1,
                        shape == SHAPE_CIRCLE ? circles_left - take : circles_left,
                        shape == SHAPE_SQUARE ? squares_left - take : squares_left);
            }
            chosen[type_idx] = 0;
        };

    recurse(0, sockets_of_shape[SHAPE_CIRCLE], sockets_of_shape[SHAPE_SQUARE]);

    // Drop dominated fills: if two fills consume the SAME gems, only the more
    // valuable one can ever be right. This was described as happening but was
    // not actually implemented, so every dominated option stayed in the search
    // and inflated the branching factor for no benefit.
    std::sort(fills.begin(), fills.end(), [](const Fill& a, const Fill& b) {
        if (a.used != b.used) return a.used < b.used;
        return a.value > b.value;
    });
    fills.erase(std::unique(fills.begin(), fills.end(),
                            [](const Fill& a, const Fill& b) { return a.used == b.used; }),
                fills.end());

    // Best-first. The recursion above generates the EMPTY fill first (it starts
    // at take = 0 for every type), so a depth-first search tried "put nothing
    // in anything" before any real option. Combined with the node budget that
    // meant a large inventory could exhaust the budget having barely improved
    // on nothing - which is exactly how a Flagon sat empty while seven unused
    // Rubies that fit it were sitting in the pool.
    std::sort(fills.begin(), fills.end(),
              [](const Fill& a, const Fill& b) { return a.value > b.value; });
    return fills;
}

// Best achievable total sell value from a set of herbs, plus the combines to
// get there. Searches over "apply one legal combine, then recurse", memoised
// on the count vector - combining only ever reduces the item count, so the
// search is finite and, at realistic herb counts, tiny.
//
// Returns the value; fills out_steps with the combines to perform, in order.
double optimize_herbs(const HerbCounts& start,
                      const std::array<int32_t, HERB_TYPE_COUNT>& prices,
                      std::vector<std::pair<int, int>>* out_steps) {
    std::map<HerbCounts, double> memo;
    std::map<HerbCounts, int> best_recipe; // which recipe index won, -1 = stop

    const std::function<double(const HerbCounts&)> best =
        [&](const HerbCounts& c) -> double {
            const auto it = memo.find(c);
            if (it != memo.end()) return it->second;

            // Baseline: combine nothing further, sell everything as it is.
            double value = 0.0;
            for (int i = 0; i < HERB_TYPE_COUNT; ++i) value += (double)prices[i] * c[i];
            int chosen = -1;

            for (size_t r = 0; r < sizeof(HERB_RECIPES) / sizeof(HERB_RECIPES[0]); ++r) {
                const auto& rec = HERB_RECIPES[r];
                // Same-type recipes (G+G) need two of that type.
                if (rec.a == rec.b) {
                    if (c[rec.a] < 2) continue;
                } else {
                    if (c[rec.a] < 1 || c[rec.b] < 1) continue;
                }

                HerbCounts next = c;
                next[rec.a]--;
                next[rec.b]--;
                next[rec.result]++;

                const double candidate = best(next);
                if (candidate > value) {
                    value = candidate;
                    chosen = (int)r;
                }
            }

            memo[c] = value;
            best_recipe[c] = chosen;
            return value;
        };

    const double total = best(start);

    if (out_steps != nullptr) {
        out_steps->clear();
        HerbCounts cur = start;
        // Walk the winning chain back out, so the page can show the actual
        // sequence rather than just a number.
        for (;;) {
            const auto it = best_recipe.find(cur);
            if (it == best_recipe.end() || it->second < 0) break;
            const auto& rec = HERB_RECIPES[it->second];
            out_steps->emplace_back(rec.a, rec.b);
            cur[rec.a]--;
            cur[rec.b]--;
            cur[rec.result]++;
        }
    }
    return total;
}

// Best crafting plan given the resources on hand. Gunpowder is the shared
// constraint across all four recipes, while Smalls and Larges each feed their
// own pair, so the interesting question is how to split the powder.
//
// Exhaustive over craft counts: bounded tightly by both the resource counts
// and the powder budget, so the loop stays small at any realistic inventory.
double optimize_crafting(int32_t smalls, int32_t larges, int32_t powder,
                         int32_t price_small, int32_t price_large, int32_t price_powder,
                         const std::array<int32_t, CRAFT_RECIPE_COUNT>& ammo_prices,
                         std::vector<CraftPlanEntry>* out_plan) {
    const double baseline = (double)smalls * price_small
                          + (double)larges * price_large
                          + (double)powder * price_powder;

    double best_value = baseline;
    std::array<int32_t, CRAFT_RECIPE_COUNT> best_counts{};

    // Recipe order matches CRAFT_RECIPES: 0/1 consume Smalls, 2/3 Larges.
    const int32_t max0 = std::min(smalls, powder / CRAFT_RECIPES[0].powder);
    for (int32_t c0 = 0; c0 <= max0; ++c0) {
        const int32_t p0 = c0 * CRAFT_RECIPES[0].powder;
        const int32_t max1 = std::min(smalls - c0, (powder - p0) / CRAFT_RECIPES[1].powder);
        for (int32_t c1 = 0; c1 <= max1; ++c1) {
            const int32_t p1 = p0 + c1 * CRAFT_RECIPES[1].powder;
            const int32_t max2 = std::min(larges, (powder - p1) / CRAFT_RECIPES[2].powder);
            for (int32_t c2 = 0; c2 <= max2; ++c2) {
                const int32_t p2 = p1 + c2 * CRAFT_RECIPES[2].powder;
                const int32_t max3 = std::min(larges - c2, (powder - p2) / CRAFT_RECIPES[3].powder);
                for (int32_t c3 = 0; c3 <= max3; ++c3) {
                    const int32_t used_powder = p2 + c3 * CRAFT_RECIPES[3].powder;
                    const std::array<int32_t, CRAFT_RECIPE_COUNT> counts{c0, c1, c2, c3};

                    double value = 0.0;
                    for (int r = 0; r < CRAFT_RECIPE_COUNT; ++r) {
                        value += (double)counts[r] * CRAFT_RECIPES[r].yield * ammo_prices[r];
                    }
                    value += (double)(smalls - c0 - c1) * price_small;
                    value += (double)(larges - c2 - c3) * price_large;
                    value += (double)(powder - used_powder) * price_powder;

                    if (value > best_value) {
                        best_value = value;
                        best_counts = counts;
                    }
                }
            }
        }
    }

    if (out_plan != nullptr) {
        out_plan->clear();
        for (int r = 0; r < CRAFT_RECIPE_COUNT; ++r) {
            if (best_counts[r] <= 0) continue;
            CraftPlanEntry e;
            e.name = CRAFT_RECIPES[r].name;
            e.times = best_counts[r];
            e.rounds = best_counts[r] * CRAFT_RECIPES[r].yield;
            e.value = e.rounds * ammo_prices[r];
            out_plan->push_back(std::move(e));
        }
    }
    return best_value - baseline; // gain over just selling the raw materials
}

void optimize_treasures(std::vector<TreasureCandidate>& treasures,
                        std::unordered_map<int32_t, int32_t>& gem_pool,
                        std::vector<TreasureResult>& out) {
    // Map the id-keyed pool onto fixed gem-type slots, and grab each type's
    // price once.
    GemCounts pool{};
    std::array<int32_t, GEM_TYPE_COUNT> prices{};
    std::array<int32_t, GEM_TYPE_COUNT> id_of_type{};
    for (const auto& [gid, cnt] : gem_pool) {
        if (cnt <= 0) continue;
        const auto ti = gem_type_of(gid);
        if (ti < 0) continue;
        pool[ti] += cnt;
        id_of_type[ti] = gid;
        if (prices[ti] == 0) prices[ti] = sell_price_by_id(gid);
    }

    // Treasures with no usable sockets can never gain value; report them at
    // base and keep them out of the search entirely.
    std::vector<const TreasureCandidate*> active;
    for (const auto& t : treasures) {
        const auto n = t.socket_shapes.size();
        if (n >= 2 && n <= 5) {
            active.push_back(&t);
        } else {
            TreasureResult r;
            r.name = get_item_name(t.id);
            r.item_id = t.id;
            r.base_price = t.base_price;
            r.final_price = (double)t.base_price;
            out.push_back(std::move(r));
        }
    }
    if (active.empty()) return;

    // Precompute each treasure's options once.
    std::vector<std::vector<Fill>> options;
    options.reserve(active.size());
    for (const auto* t : active) options.push_back(enumerate_fills(*t, pool, prices));

    // Best value any single treasure from idx onwards could reach, ignoring
    // whether the gems are actually available. Summed as a suffix, this is an
    // admissible upper bound on the rest of the search - it can only ever
    // overestimate, so pruning against it never discards the true optimum.
    std::vector<double> suffix_max(active.size() + 1, 0.0);
    for (size_t i = active.size(); i-- > 0;) {
        const double best_here = options[i].empty() ? 0.0 : options[i].front().value;
        suffix_max[i] = suffix_max[i + 1] + best_here;
    }

    // Search for the best total, remembering the choice made at each step.
    std::vector<size_t> current(active.size(), 0);
    std::vector<size_t> best_choice(active.size(), 0);
    double best_total = -1.0;
    // Runs on a worker thread (see the dispatch in poll()), so a generous
    // budget costs no frames. Branch-and-bound below usually finishes long
    // before this is reached.
    long long budget = 20'000'000;

    // Leftover gems still sell. Counting only treasure value made two plans
    // that consume different numbers of gems incomparable - the gems left in
    // your pocket are worth real pesetas and belong in the objective.
    const auto loose_value = [&](const GemCounts& c) {
        double v = 0.0;
        for (int i = 0; i < GEM_TYPE_COUNT; ++i) v += (double)prices[i] * c[i];
        return v;
    };

    std::function<void(size_t, GemCounts&, double)> search =
        [&](size_t idx, GemCounts& remaining, double acc) {
            if (budget-- <= 0) return;

            if (idx == active.size()) {
                const double total = acc + loose_value(remaining);
                if (total > best_total) {
                    best_total = total;
                    best_choice = current;
                }
                return;
            }

            // Prune: even giving every remaining treasure its best conceivable
            // fill AND keeping every gem, this branch cannot beat the incumbent.
            if (acc + suffix_max[idx] + loose_value(remaining) <= best_total) return;

            for (size_t oi = 0; oi < options[idx].size(); ++oi) {
                const auto& f = options[idx][oi];

                bool affordable = true;
                for (int i = 0; i < GEM_TYPE_COUNT; ++i) {
                    if (f.used[i] > remaining[i]) { affordable = false; break; }
                }
                if (!affordable) continue;

                for (int i = 0; i < GEM_TYPE_COUNT; ++i) remaining[i] -= f.used[i];
                current[idx] = oi;
                search(idx + 1, remaining, acc + f.value);
                for (int i = 0; i < GEM_TYPE_COUNT; ++i) remaining[i] += f.used[i];
            }
        };

    GemCounts remaining = pool;
    search(0, remaining, 0.0);

    // Emit the winning assignment and deduct the gems it consumes.
    for (size_t i = 0; i < active.size(); ++i) {
        const auto* t = active[i];
        const auto& f = options[i][best_choice[i]];

        TreasureResult r;
        r.name = get_item_name(t->id);
        r.item_id = t->id;
        r.base_price = t->base_price;
        r.final_price = f.value;

        for (int ti = 0; ti < GEM_TYPE_COUNT; ++ti) {
            for (int32_t k = 0; k < f.used[ti]; ++k) {
                r.gems_used.push_back(get_item_name(id_of_type[ti]));
                r.gem_prices.push_back(prices[ti]);
            }
            if (f.used[ti] > 0) {
                auto& left = gem_pool[id_of_type[ti]];
                left -= f.used[ti];
                if (left < 0) left = 0;
            }
        }

        out.push_back(std::move(r));
    }
}

// ---------------------------------------------------------------------------
// Startup resolution
// ---------------------------------------------------------------------------
bool resolve() {
    auto& api = reframework::API::get();
    auto* tdb = api->tdb();

    g_res.inventory_manager = api->get_managed_singleton("chainsaw.InventoryManager");
    g_res.shop_manager = api->get_managed_singleton("chainsaw.InGameShopManager");
    g_res.msg_manager = api->get_managed_singleton("chainsaw.MessageManager");
    auto* item_manager = api->get_managed_singleton("chainsaw.ItemManager");

    if (!g_res.inventory_manager || !g_res.shop_manager || !g_res.msg_manager || !item_manager) {
        return false; // game data not up yet - retry next poll
    }

    g_res.item_manager = item_manager;

    auto* im_type = item_manager->get_type_definition();
    auto* get_treasure_mgr = im_type ? im_type->find_method("get_TreasureItemManager") : nullptr;
    auto* get_item_msg_mgr = im_type ? im_type->find_method("get_ItemMessageManager") : nullptr;
    if (!get_treasure_mgr || !get_item_msg_mgr) return false;

    auto tm_ret = get_treasure_mgr->invoke(item_manager, std::vector<void*>{});
    auto imm_ret = get_item_msg_mgr->invoke(item_manager, std::vector<void*>{});
    g_res.treasure_manager = (reframework::API::ManagedObject*)tm_ret.ptr;
    g_res.item_msg_manager = (reframework::API::ManagedObject*)imm_ret.ptr;
    if (!g_res.treasure_manager || !g_res.item_msg_manager) return false;

    auto* inv_type = tdb->find_type("chainsaw.InventoryManager");
    g_res.controller_table_field = inv_type ? inv_type->find_field("_ControllerTable") : nullptr;
    g_res.get_curr_ptas = inv_type ? inv_type->find_method("get_CurrPTAS") : nullptr;

    auto* shop_type = g_res.shop_manager->get_type_definition();
    g_res.sell_price_by_id = shop_type ? shop_type->find_method("getSellPrice(chainsaw.ItemID)") : nullptr;
    g_res.sell_price_by_item = shop_type ? shop_type->find_method("getSellPrice(chainsaw.Item)") : nullptr;
    // Optional: only present in Separate Ways. Absent elsewhere is normal,
    // not an error - the Trade section simply contributes nothing.
    g_res.get_curr_spinel_count = shop_type ? shop_type->find_method("get_CurrSpinelCount") : nullptr;
    g_res.get_rewardable_items = shop_type ? shop_type->find_method("getRewardableItems") : nullptr;

    auto* tm_type = g_res.treasure_manager->get_type_definition();
    g_res.get_combine_setting = tm_type ? tm_type->find_method("getCombineSetting") : nullptr;

    auto* imm_type = g_res.item_msg_manager->get_type_definition();
    g_res.get_item_name_msg_id = imm_type ? imm_type->find_method("getItemNameMsgId") : nullptr;

    // Key items become sellable only once the game decides they are spent. Note
    // this is NOT a general "can this be sold" test - it returns false for
    // ordinary sellable items like ammo and treasures, so it is only ever
    // applied to the key item inventory.
    g_res.is_sellable = im_type ? im_type->find_method("isSellable") : nullptr;

    auto* msg_type = g_res.msg_manager->get_type_definition();
    g_res.get_message = msg_type ? msg_type->find_method("getMessage(System.Guid, System.Boolean)") : nullptr;
    if (g_res.get_message == nullptr && msg_type != nullptr) {
        g_res.get_message = msg_type->find_method("getMessage");
    }

    // Storage (the typewriter box). The game calls it the "Armoury" internally,
    // which is why every earlier search for storage/box/stock style names came
    // up empty - it was found by exhaustively walking the type database rather
    // than by guessing. Optional: if it cannot be resolved, everything else
    // still works and storage simply contributes nothing.
    g_res.armoury_manager = api->get_managed_singleton("chainsaw.ArmouryManager");
    if (g_res.armoury_manager != nullptr) {
        auto* arm_type = tdb->find_type("chainsaw.ArmouryManager");
        g_res.armoury_list_field = arm_type ? arm_type->find_field("_ArmouryItemList") : nullptr;
        if (g_res.armoury_list_field == nullptr) {
            log_error("ArmouryManager found but _ArmouryItemList missing - storage will be skipped");
        }
    } else {
        log_info("ArmouryManager not available yet - storage will be skipped this session");
    }

    if (!g_res.controller_table_field || !g_res.get_curr_ptas || !g_res.sell_price_by_id ||
        !g_res.sell_price_by_item || !g_res.get_combine_setting ||
        !g_res.get_item_name_msg_id || !g_res.get_message) {
        log_error("required method/field missing - disabling");
        g_res.disabled = true;
        return false;
    }

    // Resolve the "busy" GUI ids by name, so polling can pause while the game
    // is mutating inventory (see BUSY_GUI_NAMES).
    g_res.gui_manager = api->get_managed_singleton("chainsaw.GuiManager");
    if (g_res.gui_manager != nullptr) {
        auto* gui_type = g_res.gui_manager->get_type_definition();
        g_res.is_open_gui = gui_type ? gui_type->find_method("isOpenGui") : nullptr;

        auto* enum_type = tdb->find_type("chainsaw.GuiType");
        if (enum_type != nullptr) {
            for (const auto* want : BUSY_GUI_NAMES) {
                auto* f = enum_type->find_field(want);
                if (f == nullptr) continue;
                const auto val = *(int32_t*)f->get_data_raw(nullptr, false);
                g_res.busy_gui_types.push_back(val);
            }
        }
    }
    // General streaming/reload signal - covers area transitions and checkpoint
    // reloads, which never open any of the named busy GUIs above. Optional: if
    // unavailable, the plugin still runs, just without this extra guard.
    g_res.scene_manager = api->get_native_singleton("via.SceneManager");
    if (g_res.scene_manager != nullptr) {
        auto* sm_type = tdb->find_type("via.SceneManager");
        g_res.get_scene_loading = sm_type ? sm_type->find_method("get_Loading") : nullptr;
    }

    // Chapter + difficulty. Optional - absent just means those fields report
    // empty, which is what happened before they were added.
    g_res.status_effect_manager = api->get_managed_singleton("chainsaw.StatusEffectManager");
    if (g_res.status_effect_manager != nullptr) {
        auto* se_type = g_res.status_effect_manager->get_type_definition();
        // The single-argument overload. The (ContextID, StatusEffectID) form
        // needs a player context that could not be identified - only context 0
        // is accepted and it reports nothing while the effect is demonstrably
        // active - whereas this one returns the live value directly.
        g_res.get_curr_status_effect_value =
            se_type ? se_type->find_method("getCurrStatusEffectValue(chainsaw.StatusEffectID)") : nullptr;
    }

    g_res.campaign_manager = api->get_managed_singleton("chainsaw.CampaignManager");
    if (g_res.campaign_manager != nullptr) {
        auto* cm_type = g_res.campaign_manager->get_type_definition();
        g_res.get_current_chapter = cm_type ? cm_type->find_method("get_CurrentChapter") : nullptr;
        g_res.get_current_difficulty = cm_type ? cm_type->find_method("get_CurrentDifficulty") : nullptr;
    }
    if (auto* chap_enum = tdb->find_type("chainsaw.ChapterID")) {
        for (auto* f : chap_enum->get_fields()) {
            if (f == nullptr || !f->is_static()) continue;
            const auto* nm = f->get_name();
            if (nm == nullptr) continue;
            g_res.chapter_names[*(int32_t*)f->get_data_raw(nullptr, false)] = nm;
        }
    }

    // Weapon upgrade valuation. Optional: without it weapons simply report
    // their base price, which is what happened before this was added.
    g_res.weapon_custom_manager = api->get_managed_singleton("chainsaw.WeaponCustomManager");
    if (g_res.weapon_custom_manager != nullptr) {
        auto* wcm_type = g_res.weapon_custom_manager->get_type_definition();
        g_res.get_cost_sum = wcm_type ? wcm_type->find_method("getCostSum") : nullptr;
    }
    auto* clw_type = tdb->find_type("chainsaw.CustomLevelInWeapon");
    g_res.common_levels_field = clw_type ? clw_type->find_field("_CommonLevelInWeapon") : nullptr;
    g_res.individual_levels_field = clw_type ? clw_type->find_field("_IndividualLevelInWeapon") : nullptr;

    // Optional, Separate Ways only - absent elsewhere is normal.
    g_res.quest_manager = api->get_managed_singleton("chainsaw.QuestManager");
    if (g_res.quest_manager != nullptr) {
        auto* qm_type = g_res.quest_manager->get_type_definition();
        g_res.get_completed_quests = qm_type ? qm_type->find_method("getCompletedQuestOrder") : nullptr;
        g_res.get_quest_orders = qm_type ? qm_type->find_method("get_Orders") : nullptr;
        auto* qo_type = tdb->find_type("chainsaw.QuestOrder");
        g_res.quest_is_cleared = qo_type ? qo_type->find_method("isQuestCleared") : nullptr;
        g_res.quest_clear_item = qo_type ? qo_type->find_method("getQuestClearItem") : nullptr;
        g_res.quest_reward_field = qo_type ? qo_type->find_field("Reward") : nullptr;
        auto* qr_type = tdb->find_type("chainsaw.QuestReward");
        g_res.quest_reward_count = qr_type ? qr_type->find_method("get_Count") : nullptr;
    }

    g_res.save_data_manager = api->get_managed_singleton("share.SaveDataManager");
    if (g_res.save_data_manager != nullptr) {
        auto* sdm_type = g_res.save_data_manager->get_type_definition();
        g_res.save_is_busy = sdm_type ? sdm_type->find_method("get_IsBusy") : nullptr;
    }

    // Field handles for recovering already-socketed gems.
    auto* tic_type = tdb->find_type("chainsaw.TreasureInventoryController");
    g_res.treasure_inventory_field =
        tic_type ? tic_type->find_field("<_Inventory>k__BackingField") : nullptr;
    auto* tinv_type = tdb->find_type("chainsaw.gui.treasure.TreasureInventory");
    g_res.treasure_items_field = tinv_type ? tinv_type->find_field("_Items") : nullptr;

    log_info("resolved all game handles (%zu busy-GUI ids, scene-loading guard %s, "
             "save-busy guard %s, socketed-gem read %s)",
             g_res.busy_gui_types.size(),
             g_res.get_scene_loading ? "ok" : "UNAVAILABLE",
             g_res.save_is_busy ? "ok" : "UNAVAILABLE",
             (g_res.treasure_inventory_field && g_res.treasure_items_field) ? "ok" : "UNAVAILABLE");

    g_res.ready = true;
    return true;
}

// Gems already socketed into treasures are still yours - they can be taken back
// out at will - so they belong in the pool the optimiser draws from, and the
// treasures they sit in are treated as empty canvases.
//
// Without this, following the plugin's own advice made the reported net worth
// DROP: the gems left the loose pool, but treasures are priced by item ID
// (which cannot see socket contents), so the combined treasure still reported
// its bare base price and the value of the gems vanished from the total.
//
// Counting them here means the figure is stable whether the gems are loose or
// already set, and always represents the best achievable with what you own.
void collect_socketed_gems(reframework::API::ManagedObject* treasure_ctrl,
                           std::unordered_map<int32_t, int32_t>& gem_pool,
                           ActualSockets& actual,
                           GuiOrder& gui_order) {
    if (g_res.treasure_inventory_field == nullptr || g_res.treasure_items_field == nullptr) return;

    auto* inv = *(reframework::API::ManagedObject**)
        g_res.treasure_inventory_field->get_data_raw(treasure_ctrl, false);
    if (inv == nullptr) return;

    // getInventoryItemsForGui() rather than the raw _Items list: _Items is in
    // internal slot order, while this returns the order the treasure screen
    // actually draws (verified against a screenshot - column-major, top-down
    // then right). Walking it here means socket contents and grid position
    // come from the same pass, so the plan can be listed to match what the
    // player is looking at. Falls back to _Items if unavailable.
    reframework::API::ManagedObject* items = nullptr;
    if (auto* gui_items = cached_find_method(treasure_ctrl->get_type_definition(),
                                             "getInventoryItemsForGui")) {
        auto g_ret = gui_items->invoke(treasure_ctrl, std::vector<void*>{});
        if (!g_ret.exception_thrown && g_ret.ptr != nullptr) {
            items = (reframework::API::ManagedObject*)g_ret.ptr;
        }
    }
    if (items == nullptr) {
        items = *(reframework::API::ManagedObject**)
            g_res.treasure_items_field->get_data_raw(inv, false);
    }
    if (items == nullptr) return;

    auto* lt = items->get_type_definition();
    auto* l_count = cached_find_method(lt, "get_Count");
    auto* l_item = cached_find_method(lt, "get_Item");
    if (!l_count || !l_item) return;

    const auto n = call_int32(l_count, items);
    if (n <= 0 || (uint32_t)n > MAX_ITEMS) return;

    for (int32_t i = 0; i < n; ++i) {
        auto e_ret = l_item->invoke(items, {(void*)(intptr_t)i});
        if (e_ret.exception_thrown || e_ret.ptr == nullptr) continue;
        auto* entry = (reframework::API::ManagedObject*)e_ret.ptr;

        auto* et = entry->get_type_definition();
        auto* get_ci = cached_find_method(et, "getCombineInfo");
        if (get_ci == nullptr) continue;

        // Which treasure this piece is, so its socket contents can be compared
        // against what the optimiser recommends for it. Optional: if the id is
        // unavailable the gems still count towards the pool, we just cannot say
        // which treasure they are sitting in.
        int32_t host_id = 0;
        if (auto* get_host_id = cached_find_method(et, "get_ItemId")) {
            host_id = call_int32(get_host_id, entry);
        }
        std::vector<std::string> this_piece;

        auto ci_ret = get_ci->invoke(entry, std::vector<void*>{});
        if (ci_ret.exception_thrown || ci_ret.ptr == nullptr) continue;
        auto* ci = (reframework::API::ManagedObject*)ci_ret.ptr;

        auto* cit = ci->get_type_definition();
        auto* get_datas = cached_find_method(cit, "get_Datas");
        if (get_datas == nullptr) continue;

        auto d_ret = get_datas->invoke(ci, std::vector<void*>{});
        if (d_ret.exception_thrown || d_ret.ptr == nullptr) continue;
        auto* datas = (reframework::API::ManagedObject*)d_ret.ptr;

        auto* dt = datas->get_type_definition();
        auto* d_count = cached_find_method(dt, "get_Count");
        auto* d_item = cached_find_method(dt, "get_Item");
        if (!d_count || !d_item) continue;

        const auto dn = call_int32(d_count, datas);
        for (int32_t k = 0; k < dn && dn <= 16; ++k) {
            auto e2 = d_item->invoke(datas, {(void*)(intptr_t)k});
            if (e2.exception_thrown || e2.ptr == nullptr) continue;

            if (g_res.combined_item_id_field == nullptr) {
                auto& api = reframework::API::get();
                auto* data_type = api->tdb()->find_type("chainsaw.TreasureCombineInfo.Data");
                g_res.combined_item_id_field =
                    data_type ? data_type->find_field("_CombinedItemId") : nullptr;
                if (g_res.combined_item_id_field == nullptr) return;
            }

            const auto gem_id = *(int32_t*)g_res.combined_item_id_field->get_data_raw(e2.ptr, false);
            if (gem_id <= 0) continue; // empty socket

            // Only count it if it really is one of the six gemstones.
            const auto gem_name = get_item_name(gem_id);
            if (!is_real_gemstone(gem_id)) continue;
            gem_pool[gem_id] += 1;
            this_piece.push_back(gem_name);
        }

        if (host_id != 0) {
            actual[host_id].push_back(std::move(this_piece));
            gui_order[host_id].push_back(i); // i is the grid position
        }
    }
}

// True while the game is in a state where reading is pointless or unsafe -
// loading, saving, or the death screen. Shop and combine screens no longer
// count; see BUSY_GUI_NAMES.
bool game_is_mutating_inventory() {
    if (g_res.scene_manager != nullptr && g_res.get_scene_loading != nullptr) {
        auto ret = g_res.get_scene_loading->invoke(
            (reframework::API::ManagedObject*)g_res.scene_manager, std::vector<void*>{});
        if (!ret.exception_thrown && ret.byte != 0) return true;
    }

    if (g_res.save_data_manager != nullptr && g_res.save_is_busy != nullptr) {
        auto ret = g_res.save_is_busy->invoke(g_res.save_data_manager, std::vector<void*>{});
        if (!ret.exception_thrown && ret.byte != 0) return true;
    }

    if (g_res.gui_manager == nullptr || g_res.is_open_gui == nullptr) return false;
    for (const auto gt : g_res.busy_gui_types) {
        auto ret = g_res.is_open_gui->invoke(
            g_res.gui_manager, {(void*)(intptr_t)gt, (void*)(intptr_t)RENDER_OUTPUT_DEFAULT});
        if (!ret.exception_thrown && ret.byte != 0) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Trade shop combine optimisation (Separate Ways)
// ---------------------------------------------------------------------------
// Catalog entries split into two classes, because they need different
// treatment. "Interacting" ones (gems, treasures, herbs) change what the
// combine optimisers can achieve, so their value depends on what else you
// buy and own - those need real search. Everything else (weapons, scopes,
// gunpowder...) is worth a flat resale price no matter what, which is an
// ordinary knapsack.
enum class PurchaseKind { Gem, Treasure, Herb };

struct PurchasableCandidate {
    std::string name;
    int32_t item_id = 0;
    int32_t spinel_cost = 0;
    int32_t qty = 0;
    PurchaseKind kind = PurchaseKind::Gem;
    int herb_type = -1; // index into HERB_* when kind == Herb
};

// A catalog entry whose value does not depend on anything else you buy.
struct FlatCandidate {
    std::string name;
    int32_t spinel_cost = 0;
    int32_t qty = 0;
    int32_t value = 0; // total resale pesetas
};

// Which purchasable candidates to buy interacts with which owned gems go
// where - a gem is only worth buying if there is a socket for it, and vice
// versa - so this can't be scored per-item independently. It has to actually
// try combinations. At the catalog sizes this game exposes (a handful of
// qualifying entries even by late game) an exhaustive subset search stays
// cheap: for n candidates this is 2^n full optimiser runs, capped below so it
// can never become a lot of work even if the catalog grows unexpectedly.
//
// Only called when build_json's caller detects the relevant inputs changed -
// never unconditionally every poll. Running an expensive search on a fixed
// 100ms cadence regardless of need is exactly the pattern that caused the
// crash this session spent so long chasing down; this avoids repeating it.
constexpr size_t MAX_TRADE_SUBSET_CANDIDATES = 16; // 2^16 = 65536 runs, worst case

void optimize_with_trade_shop(const std::vector<TreasureCandidate>& owned_treasures,
                               const std::unordered_map<int32_t, int32_t>& owned_gems,
                               const HerbCounts& owned_herbs,
                               const std::array<int32_t, HERB_TYPE_COUNT>& herb_prices,
                               const std::vector<PurchasableCandidate>& purchasable,
                               const std::vector<FlatCandidate>& flat,
                               int32_t spinel_budget,
                               double& out_gain,
                               std::vector<TradeItemEntry>& out_recommended,
                               std::vector<TreasureResult>& out_purchase_combos,
                               std::vector<std::string>& out_herb_steps,
                               double& out_herb_total,
                               // The OWNED-herb optimum for this same snapshot.
                               // Handed back so the caller can add a herb value
                               // that describes the same game state as the gain
                               // it is being added to - see the note at the
                               // publish site.
                               double& out_owned_herb_total,
                               std::vector<std::string>& out_owned_herb_steps) {
    out_gain = 0.0;
    out_recommended.clear();
    out_purchase_combos.clear();
    out_herb_steps.clear();
    out_herb_total = 0.0;
    out_owned_herb_total = 0.0;
    out_owned_herb_steps.clear();

    // Flat items are an ordinary 0/1 knapsack: their value never depends on
    // what else is bought. Solved once for every budget, so each subset below
    // is a table lookup rather than another solve.
    const size_t fn = flat.size();
    const int32_t cap = std::max(0, spinel_budget);
    std::vector<std::vector<int32_t>> fdp(fn + 1, std::vector<int32_t>(cap + 1, 0));
    for (size_t i = 1; i <= fn; ++i) {
        const auto& f = flat[i - 1];
        for (int32_t b = 0; b <= cap; ++b) {
            fdp[i][b] = fdp[i - 1][b];
            if (f.spinel_cost <= b) {
                const int32_t take = fdp[i - 1][b - f.spinel_cost] + f.value;
                if (take > fdp[i][b]) fdp[i][b] = take;
            }
        }
    }
    const auto flat_pick = [&](int32_t budget, std::vector<const FlatCandidate*>* picked) {
        if (budget < 0) budget = 0;
        if (budget > cap) budget = cap;
        if (picked != nullptr) {
            picked->clear();
            int32_t b = budget;
            for (size_t i = fn; i >= 1; --i) {
                if (fdp[i][b] != fdp[i - 1][b]) {
                    picked->push_back(&flat[i - 1]);
                    b -= flat[i - 1].spinel_cost;
                }
            }
        }
        return fdp[fn][budget];
    };

    // Gem prices, resolved once. optimize_treasures() consumes the pool it is
    // given, so the pool left afterwards is exactly the gems still loose.
    std::unordered_map<int32_t, int32_t> gem_prices;
    for (const auto& [gid, cnt] : owned_gems) {
        if (cnt > 0) gem_prices[gid] = sell_price_by_id(gid);
    }
    for (const auto& p : purchasable) {
        if (p.kind == PurchaseKind::Gem && gem_prices.find(p.item_id) == gem_prices.end()) {
            gem_prices[p.item_id] = sell_price_by_id(p.item_id);
        }
    }
    const auto loose_gem_value = [&](const std::unordered_map<int32_t, int32_t>& pool) {
        double v = 0.0;
        for (const auto& [gid, cnt] : pool) {
            if (cnt <= 0) continue;
            const auto it = gem_prices.find(gid);
            if (it != gem_prices.end()) v += (double)it->second * cnt;
        }
        return v;
    };

    // Both sides must value the COMPLETE end state - combined treasures plus
    // whatever gems remain loose - not just the combine total. Comparing
    // combine totals alone double-counts: a loose gem is already counted in
    // leftoverGems, and socketing it into a purchased treasure would count
    // its value a second time inside the gain. Buying a gem the plan already
    // assumed would then inflate net worth by that gem's price, when it
    // should leave the figure unchanged (spinels are not pesetas; you only
    // swapped one asset for another the plan already accounted for).
    std::vector<TreasureCandidate> baseline_treasures = owned_treasures;
    std::unordered_map<int32_t, int32_t> baseline_gems = owned_gems;
    std::vector<TreasureResult> baseline_results;
    optimize_treasures(baseline_treasures, baseline_gems, baseline_results);
    double baseline_total = 0.0;
    for (const auto& r : baseline_results) baseline_total += r.final_price;
    baseline_total += loose_gem_value(baseline_gems);
    {
        // Captured rather than discarded: this is the owned-herb figure for
        // THIS snapshot, and the caller needs it to keep the herb value it
        // reports consistent with the gain computed here.
        std::vector<std::pair<int, int>> owned_steps;
        out_owned_herb_total = optimize_herbs(owned_herbs, herb_prices, &owned_steps);
        for (const auto& [a, b] : owned_steps) {
            out_owned_herb_steps.push_back(std::string(HERB_NAMES[a]) + " + " + HERB_NAMES[b]);
        }
        baseline_total += out_owned_herb_total;
    }

    const size_t n = purchasable.size();
    if (n > MAX_TRADE_SUBSET_CANDIDATES) {
        // Previously this returned outright, leaving the gain at zero with no
        // trace - the shop quietly stopped contributing anything to net worth
        // and nothing said why. The flat knapsack does not need the subset
        // search, so fall back to it: strictly less than the true optimum,
        // never more, and the herb figures above are still valid.
        std::vector<const FlatCandidate*> picked;
        out_gain = flat_pick(spinel_budget, &picked);
        for (auto it = picked.rbegin(); it != picked.rend(); ++it) {
            TradeItemEntry e;
            e.name = (*it)->name;
            e.spinel_cost = (*it)->spinel_cost;
            e.item_count = (*it)->qty;
            e.unit_sell_price = (*it)->qty > 0 ? (*it)->value / (*it)->qty : 0;
            e.total_sell_value = (*it)->value;
            out_recommended.push_back(std::move(e));
        }
        out_herb_total = out_owned_herb_total;
        out_herb_steps = out_owned_herb_steps;
        log_error("trade shop: %zu interacting candidates exceeds the cap of %zu - "
                  "falling back to flat items only; the shop figure will UNDERSTATE",
                  n, MAX_TRADE_SUBSET_CANDIDATES);
        return;
    }

    // Spending nothing on interacting items still leaves the whole budget for
    // flat ones, so that is the real baseline to beat.
    double best_total = baseline_total + flat_pick(spinel_budget, nullptr);
    uint32_t best_mask = 0;
    std::vector<TreasureResult> best_results = baseline_results;
    HerbCounts best_herbs = owned_herbs;

    for (uint32_t mask = 0; mask < (1u << n); ++mask) {
        int32_t cost = 0;
        for (size_t i = 0; i < n; ++i) {
            if (mask & (1u << i)) cost += purchasable[i].spinel_cost;
        }
        if (cost > spinel_budget) continue;

        auto treasures = owned_treasures;
        auto gems = owned_gems;
        auto herbs = owned_herbs;
        for (size_t i = 0; i < n; ++i) {
            if (!(mask & (1u << i))) continue;
            const auto& p = purchasable[i];
            switch (p.kind) {
            case PurchaseKind::Gem:
                gems[p.item_id] += p.qty;
                break;
            case PurchaseKind::Herb:
                if (p.herb_type >= 0) herbs[p.herb_type] += p.qty;
                break;
            case PurchaseKind::Treasure: {
                const auto& shapes = get_socket_shapes(p.item_id);
                const auto base = sell_price_by_id(p.item_id);
                for (int32_t c = 0; c < p.qty; ++c) {
                    treasures.push_back({p.item_id, base, shapes});
                }
                break;
            }
            }
        }

        std::vector<TreasureResult> results;
        optimize_treasures(treasures, gems, results);
        double total = 0.0;
        for (const auto& r : results) total += r.final_price;
        total += loose_gem_value(gems); // same end-state basis as the baseline
        total += optimize_herbs(herbs, herb_prices, nullptr);
        // Whatever spinels this subset leaves over still buy flat items.
        total += flat_pick(spinel_budget - cost, nullptr);

        if (total > best_total) {
            best_total = total;
            best_mask = mask;
            best_results = std::move(results);
            best_herbs = herbs;
        }
    }

    // Herb steps for the plan's END state, not for what is owned right now.
    // Buying a Yellow Herb can change the answer completely (G+R+Y at 10,000
    // instead of G+R at 2,000), so showing the owned-only steps next to a
    // plan that buys one would tell you to make the wrong combination.
    {
        std::vector<std::pair<int, int>> steps;
        out_herb_total = optimize_herbs(best_herbs, herb_prices, &steps);
        for (const auto& [a, b] : steps) {
            out_herb_steps.push_back(std::string(HERB_NAMES[a]) + " + " + HERB_NAMES[b]);
        }
    }

    // best_results now holds the FULL combine outcome for the winning plan -
    // not just which items to buy, but exactly what to socket where. Some of
    // those entries may already be owned (e.g. you already have an Elegant
    // Mask and the plan buys a second one), so attribute only the entries
    // beyond what the baseline already had for that item id to the purchase.
    std::unordered_map<int32_t, int32_t> baseline_counts;
    for (const auto& r : baseline_results) baseline_counts[r.item_id]++;
    std::unordered_map<int32_t, int32_t> seen_counts;
    for (const auto& r : best_results) {
        const auto seen = ++seen_counts[r.item_id];
        const auto it = baseline_counts.find(r.item_id);
        const auto base_count = (it != baseline_counts.end()) ? it->second : 0;
        if (seen > base_count) out_purchase_combos.push_back(r);
    }

    out_gain = best_total - baseline_total;

    int32_t interacting_cost = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!(best_mask & (1u << i))) continue;
        interacting_cost += purchasable[i].spinel_cost;
        TradeItemEntry e;
        e.name = purchasable[i].name;
        e.spinel_cost = purchasable[i].spinel_cost;
        e.item_count = purchasable[i].qty;
        out_recommended.push_back(std::move(e));
    }

    // Flat items bought with whatever the winning subset left over. Listed
    // last, since they are pure resale rather than part of any combine.
    std::vector<const FlatCandidate*> picked;
    flat_pick(spinel_budget - interacting_cost, &picked);
    for (auto it = picked.rbegin(); it != picked.rend(); ++it) {
        TradeItemEntry e;
        e.name = (*it)->name;
        e.spinel_cost = (*it)->spinel_cost;
        e.item_count = (*it)->qty;
        e.unit_sell_price = (*it)->qty > 0 ? (*it)->value / (*it)->qty : 0;
        e.total_sell_value = (*it)->value;
        out_recommended.push_back(std::move(e));
    }
}

// ---------------------------------------------------------------------------
// Poll
// ---------------------------------------------------------------------------
std::chrono::steady_clock::time_point g_last_poll{};

void poll() {
    auto& api = reframework::API::get();

    OutputState st;
    st.updated_at = (int64_t)std::time(nullptr);

    // Carried from the trade shop block down to the herb block, so the herb
    // value that lands in st.total describes the same snapshot as the trade
    // shop gain it is added to.
    bool have_trade_snapshot = false;
    double snapshot_owned_herb_total = 0.0;
    std::vector<std::string> snapshot_owned_herb_steps;

    // Once per poll, before anything is priced. Every sell price funnels
    // through apply_sell_bonus(), which reads these cached values rather than
    // calling into the game per item.
    refresh_sell_bonuses();

    auto ptas_ret = g_res.get_curr_ptas->invoke(g_res.inventory_manager, std::vector<void*>{});
    st.pesetas = ptas_ret.exception_thrown ? 0 : (int32_t)ptas_ret.dword;

    // --- enumerate every registered inventory controller ---
    // Read straight out of the manager's private _ControllerTable dictionary:
    // there is no public API that hands back all controllers, and the
    // per-context lookups need a ContextID with no clean way to construct it.
    // The dictionary's internal layout was verified against the live game.
    auto* dict = *(reframework::API::ManagedObject**)
        g_res.controller_table_field->get_data_raw(g_res.inventory_manager, false);
    if (dict == nullptr) return;

    if (g_res.entries_field == nullptr) {
        auto* dict_type = dict->get_type_definition();
        g_res.entries_field = dict_type ? dict_type->find_field("_entries") : nullptr;
        if (g_res.entries_field == nullptr) return;
    }

    // entries/entry_count are read fresh on every loop iteration below rather
    // than cached once here - see the loop for why. This first read is only
    // used to size the loop bound.
    auto* entries0 = *(reframework::API::ManagedObject**)
        g_res.entries_field->get_data_raw(dict, false);
    if (entries0 == nullptr) return;

    const auto entry_count = *(uint32_t*)((uintptr_t)entries0 + ARRAY_COUNT_OFFSET);
    if (entry_count == 0 || entry_count > 1024) return;

    struct ItemAccum { int32_t count; int32_t unit_price; std::string name; };
    std::unordered_map<int64_t, ItemAccum> regular;        // keyed id+price
    std::unordered_map<int32_t, ItemAccum> plain_treasure; // keyed id
    std::unordered_map<int32_t, int32_t> gem_pool;         // id -> count
    std::vector<TreasureCandidate> treasure_candidates;
    ActualSockets actual_sockets; // what is really in each treasure right now
    GuiOrder treasure_gui_order;  // grid position of each piece
    HerbCounts herb_counts{};     // pooled across inventory and storage
    // Crafting inputs. Counted but NOT diverted - crafting is advisory, so
    // these stay valued as raw materials in the headline total.
    int32_t craft_smalls = 0, craft_larges = 0, craft_powder = 0;
    // Everything held, by item id, for checking sell-to-complete requests.
    std::unordered_map<int32_t, int32_t> holdings;

    for (uint32_t i = 0; i < entry_count; ++i) {
        // Re-read the backing array fresh every iteration rather than reusing
        // one pointer cached before the loop. The loop body below invokes many
        // managed methods per controller (getItems, getCombineInfo, and
        // everything collect_socketed_gems calls); if any of those causes
        // _ControllerTable to legitimately grow and reallocate its _entries
        // array - which is exactly what a Dictionary does once it outgrows its
        // capacity - a pointer computed once before the loop keeps pointing at
        // memory that is no longer the live array for every iteration after
        // that point. This was a real, unbounded stale-pointer window spanning
        // the single largest and most call-heavy loop in the plugin, and is
        // the leading suspect for a class of crash that never showed a plugin
        // frame in its stack yet stopped entirely once both plugins were
        // disabled for a clean-room test.
        auto* dict_now = *(reframework::API::ManagedObject**)
            g_res.controller_table_field->get_data_raw(g_res.inventory_manager, false);
        if (dict_now == nullptr) break;

        auto* entries = *(reframework::API::ManagedObject**)
            g_res.entries_field->get_data_raw(dict_now, false);
        if (entries == nullptr) break;

        const auto live_count = *(uint32_t*)((uintptr_t)entries + ARRAY_COUNT_OFFSET);
        if (i >= live_count) break; // array shrank/changed shape; stop rather than read past it

        const auto entries_base = (uintptr_t)entries + ARRAY_DATA_OFFSET;
        auto* controller = *(reframework::API::ManagedObject**)
            (entries_base + i * DICT_ENTRY_STRIDE + DICT_ENTRY_VALUE_OFFSET);
        if (controller == nullptr) continue;

        auto* ctrl_type = controller->get_type_definition();
        if (ctrl_type == nullptr) continue;
        const std::string type_name = ctrl_type->get_full_name();

        // Key items are handled entirely separately, because this controller
        // does NOT expose a no-arg getItems() - only getItems(Predicate). The
        // generic lookup below therefore found nothing and silently skipped the
        // whole controller, which is why key items were reporting zero despite
        // being visible in-game and on the merchant's sell screen.
        if (type_name == "chainsaw.KeyItemInventoryController") {
            auto* get_inv_items = cached_find_method(ctrl_type, "getInventoryItems");
            auto* get_count_sum = cached_find_method(ctrl_type, "getItemCountSum");
            if (get_inv_items == nullptr) continue;

            auto ki_ret = get_inv_items->invoke(controller, std::vector<void*>{});
            if (ki_ret.exception_thrown || ki_ret.ptr == nullptr) continue;
            auto* ki_list = (reframework::API::ManagedObject*)ki_ret.ptr;

            auto* kl_type = ki_list->get_type_definition();
            auto* kl_count = cached_find_method(kl_type, "get_Count");
            auto* kl_item = cached_find_method(kl_type, "get_Item");
            if (!kl_count || !kl_item) continue;

            const auto kn = call_int32(kl_count, ki_list);
            for (int32_t k = 0; k < kn && (uint32_t)kn <= MAX_ITEMS; ++k) {
                auto e_ret = kl_item->invoke(ki_list, {(void*)(intptr_t)k});
                if (e_ret.exception_thrown || e_ret.ptr == nullptr) continue;
                auto* entry = (reframework::API::ManagedObject*)e_ret.ptr;

                auto* et = entry->get_type_definition();
                auto* e_id = cached_find_method(et, "get_ItemId");
                if (e_id == nullptr) continue;
                const auto item_id = call_int32(e_id, entry);

                // Only count what the game currently permits selling. A key
                // item still needed reports a real price regardless (Cubic
                // Device 4000, Luis's Key 2500...), so pricing alone would
                // inflate the total with items that cannot actually be sold.
                //
                // isSellable() is narrower than its name suggests: it is a
                // key-item progression gate, not a general test. It returns
                // false for plainly sellable things (verified against the
                // Flagon, a treasure the merchant buys), so it can only ever
                // be used to gate key items - and even there it has
                // exceptions, hence the allow-list below.
                if (!sellable_despite_gate(item_id)) {
                    if (g_res.is_sellable == nullptr) continue;
                    auto sell_ret = g_res.is_sellable->invoke(g_res.item_manager, {(void*)(intptr_t)item_id});
                    if (sell_ret.exception_thrown || sell_ret.byte == 0) continue;
                }

                const auto unit = sell_price_by_id(item_id);
                if (unit <= 0) continue;

                // Quantity must come from the controller: a KeyItemInventoryItem
                // carries no count of its own, and duplicates (e.g. Wooden
                // Planks x2) appear as a single entry.
                int32_t qty = 1;
                if (get_count_sum != nullptr) {
                    auto c_ret = get_count_sum->invoke(controller, {(void*)(intptr_t)item_id});
                    if (!c_ret.exception_thrown && (int32_t)c_ret.dword > 0) {
                        qty = (int32_t)c_ret.dword;
                    }
                }

                ItemEntry e;
                e.name = get_item_name(item_id);
                e.count = qty;
                e.unit_price = unit;
                e.total_price = unit * qty;
                st.key_items.push_back(std::move(e));
            }
            continue;
        }

        auto* get_items = cached_find_method(ctrl_type, "getItems()");
        if (get_items == nullptr) continue;

        auto items_ret = get_items->invoke(controller, std::vector<void*>{});
        if (items_ret.exception_thrown || items_ret.ptr == nullptr) continue;
        auto* item_list = (reframework::API::ManagedObject*)items_ret.ptr;

        auto* list_type = item_list->get_type_definition();
        auto* get_count = cached_find_method(list_type, "get_Count");
        auto* get_item = cached_find_method(list_type, "get_Item");
        if (!get_count || !get_item) continue;

        const auto n = call_int32(get_count, item_list);
        if (n <= 0 || (uint32_t)n > MAX_ITEMS) continue;

        const bool is_treasure_ctrl = (type_name == "chainsaw.TreasureInventoryController");

        // DIAGNOSTIC: the no-reads test ran 22 minutes clean vs. every crash
        // with reads active landing at 5-7 minutes, confirming the bug is
        if (is_treasure_ctrl) {
            // Gems currently sitting in sockets are recoverable, so fold them
            // back into the pool before optimising.
            collect_socketed_gems(controller, gem_pool, actual_sockets, treasure_gui_order);
        }

        for (int32_t k = 0; k < n; ++k) {
            auto item_ret = get_item->invoke(item_list, {(void*)(intptr_t)k});
            if (item_ret.exception_thrown || item_ret.ptr == nullptr) continue;
            auto* item = (reframework::API::ManagedObject*)item_ret.ptr;

            auto* it_type = item->get_type_definition();
            auto* get_item_id = cached_find_method(it_type, "get_ItemId");
            auto* get_item_count = cached_find_method(it_type, "get_CurrentItemCount");
            if (!get_item_id || !get_item_count) continue;

            const auto item_id = call_int32(get_item_id, item);
            const auto item_count = call_int32(get_item_count, item);
            if (item_count <= 0) continue;
            holdings[item_id] += item_count;

            if (is_treasure_ctrl) {
                const auto& shapes = get_socket_shapes(item_id);
                if (!shapes.empty()) {
                    const auto base = sell_price_by_id(item_id);
                    // Each physical piece competes for gems independently, the
                    // same way the merchant screen treats them.
                    for (int32_t c = 0; c < item_count; ++c) {
                        treasure_candidates.push_back({item_id, base, shapes});
                    }
                } else {
                    const auto name = get_item_name(item_id);
                    if (is_real_gemstone(item_id)) {
                        gem_pool[item_id] += item_count;
                    } else {
                        auto& acc = plain_treasure[item_id];
                        acc.count += item_count;
                        acc.unit_price = sell_price_by_id(item_id);
                        acc.name = name;
                    }
                }
            } else if (herb_type_of_id(item_id) >= 0) {
                // Diverted out of the generic item flow: herbs are worth more
                // combined than summed, so they are valued by
                // optimize_herbs() rather than at their as-is price.
                herb_counts[herb_type_of_id(item_id)] += item_count;
            } else {
                if (item_id == ITEM_RESOURCES_S) craft_smalls += item_count;
                else if (item_id == ITEM_RESOURCES_L) craft_larges += item_count;
                else if (item_id == ITEM_GUNPOWDER) craft_powder += item_count;

                // getSellPrice() gives only the base price - it does NOT
                // include upgrades, despite the merchant's sell screen doing
                // so. Add 95% of what has been sunk into the weapon; see
                // weapon_upgrade_investment().
                auto unit = sell_price_by_item(item, item_id);
                if (unit <= 0) continue;
                unit += (int32_t)(0.95 * weapon_upgrade_investment(item, item_id) + 0.5);
                const int64_t key = ((int64_t)item_id << 32) ^ (uint32_t)unit;
                auto& acc = regular[key];
                acc.count += item_count;
                acc.unit_price = unit;
                if (acc.name.empty()) acc.name = get_item_name(item_id);
            }
        }
    }

    // --- Storage / Armoury ---
    // Quantities here need care: Storage "virtually stacks" identical items, so
    // a single ArmouryItem entry with CurrentItemCount=1 can represent four
    // First Aid Sprays via CurrVirtualStackCount=4. The manager's own
    // getItemCountSum() ignores that stacking (it returned 1 for those four
    // sprays), so the real quantity is count * virtual stack.
    double storage_total = 0.0;
    if (g_res.armoury_manager != nullptr && g_res.armoury_list_field != nullptr) {
        auto* arm_list = *(reframework::API::ManagedObject**)
            g_res.armoury_list_field->get_data_raw(g_res.armoury_manager, false);

        if (arm_list != nullptr) {
            auto* al_type = arm_list->get_type_definition();
            auto* al_count = cached_find_method(al_type, "get_Count");
            auto* al_item = cached_find_method(al_type, "get_Item");

            if (al_count != nullptr && al_item != nullptr) {
                const auto an = call_int32(al_count, arm_list);
                for (int32_t i = 0; i < an && (uint32_t)an <= MAX_ITEMS; ++i) {
                    auto ai_ret = al_item->invoke(arm_list, {(void*)(intptr_t)i});
                    if (ai_ret.exception_thrown || ai_ret.ptr == nullptr) continue;
                    auto* ai = (reframework::API::ManagedObject*)ai_ret.ptr;

                    auto* ai_type = ai->get_type_definition();
                    auto* m_id = cached_find_method(ai_type, "get_ItemId");
                    auto* m_cnt = cached_find_method(ai_type, "get_CurrentItemCount");
                    auto* m_vstack = cached_find_method(ai_type, "get_CurrVirtualStackCount");
                    auto* m_inner = cached_find_method(ai_type, "get_Item");
                    if (!m_id || !m_cnt) continue;

                    const auto item_id = call_int32(m_id, ai);
                    const auto raw_count = call_int32(m_cnt, ai);
                    const auto vstack = (m_vstack != nullptr) ? call_int32(m_vstack, ai) : 1;

                    const auto qty = raw_count * (vstack > 0 ? vstack : 1);
                    if (qty <= 0) continue;
                    holdings[item_id] += qty;

                    // Prefer pricing the underlying Item, so a stored weapon is
                    // valued in its current upgraded state rather than at base.
                    int32_t unit = 0;
                    if (m_inner != nullptr) {
                        auto inner_ret = m_inner->invoke(ai, std::vector<void*>{});
                        if (!inner_ret.exception_thrown && inner_ret.ptr != nullptr) {
                            auto* inner = (reframework::API::ManagedObject*)inner_ret.ptr;
                            unit = sell_price_by_item(inner, item_id);
                            // Stored weapons keep their upgrades too.
                            if (unit > 0) {
                                unit += (int32_t)(0.95 * weapon_upgrade_investment(inner, item_id) + 0.5);
                            }
                        }
                    }
                    if (unit <= 0) unit = sell_price_by_id(item_id);
                    if (unit <= 0) continue;

                    // Herbs in Storage join the same pool as carried ones -
                    // net worth is the best realisable value, and moving an
                    // item out of the box to combine it costs nothing.
                    if (herb_type_of_id(item_id) >= 0) {
                        herb_counts[herb_type_of_id(item_id)] += qty;
                        continue;
                    }
                    if (item_id == ITEM_RESOURCES_S) craft_smalls += qty;
                    else if (item_id == ITEM_RESOURCES_L) craft_larges += qty;
                    else if (item_id == ITEM_GUNPOWDER) craft_powder += qty;

                    ItemEntry e;
                    e.name = get_item_name(item_id);
                    e.count = qty;
                    e.unit_price = unit;
                    e.total_price = unit * qty;
                    storage_total += e.total_price;
                    st.storage.push_back(std::move(e));
                }
            }
        }
    }

    // --- Trade shop (Separate Ways) ---
    // trade_items itself stays advisory-only (like the crafting figures
    // discussed but never built) - Spinels cannot convert back to pesetas
    // except by spending them here, so a raw resale total would overstate
    // what you actually have. But gems/treasures bought here ARE ordinary,
    // freely re-combinable items once purchased, so a *specific, actionable
    // plan* (buy exactly these, combine like this) is real value - see
    // optimize_with_trade_shop() and trade_shop_gain below.
    if (g_res.get_current_chapter != nullptr) {
        auto ret = g_res.get_current_chapter->invoke(g_res.campaign_manager, std::vector<void*>{});
        if (!ret.exception_thrown) {
            st.chapter_id = (int32_t)ret.dword;
            const auto it = g_res.chapter_names.find(st.chapter_id);
            if (it != g_res.chapter_names.end()) st.chapter_name = it->second;
        }
    }
    if (g_res.get_current_difficulty != nullptr) {
        auto ret = g_res.get_current_difficulty->invoke(g_res.campaign_manager, std::vector<void*>{});
        if (!ret.exception_thrown) st.difficulty = (int32_t)ret.dword;
    }

    // Resolved once and shared by both the buy plan (which needs to know what
    // a purchased herb would be worth combined) and the Herbs section below.
    std::array<int32_t, HERB_TYPE_COUNT> herb_prices{};
    for (int i = 0; i < HERB_TYPE_COUNT; ++i) {
        herb_prices[i] = std::max(0, sell_price_by_id(HERB_ITEM_IDS[i]));
    }

    // Spinels waiting in completed-but-unclaimed requests. Counted towards the
    // buy plan's budget, but flagged separately so the plan can say "hand
    // these in first" rather than silently assuming money you cannot spend yet.
    if (g_res.get_completed_quests != nullptr && g_res.quest_reward_field != nullptr &&
        g_res.quest_reward_count != nullptr) {
        auto q_ret = g_res.get_completed_quests->invoke(g_res.quest_manager, std::vector<void*>{});
        if (!q_ret.exception_thrown && q_ret.ptr != nullptr) {
            auto* q_list = (reframework::API::ManagedObject*)q_ret.ptr;
            auto* ql_type = q_list->get_type_definition();
            auto* ql_count = cached_find_method(ql_type, "get_Count");
            auto* ql_item = cached_find_method(ql_type, "get_Item");
            if (ql_count != nullptr && ql_item != nullptr) {
                const auto qn = call_int32(ql_count, q_list);
                for (int32_t i = 0; i < qn && (uint32_t)qn <= MAX_ITEMS; ++i) {
                    auto o_ret = ql_item->invoke(q_list, {(void*)(intptr_t)i});
                    if (o_ret.exception_thrown || o_ret.ptr == nullptr) continue;
                    auto* order = (reframework::API::ManagedObject*)o_ret.ptr;

                    auto* reward = *(reframework::API::ManagedObject**)
                        g_res.quest_reward_field->get_data_raw(order, false);
                    if (reward == nullptr) continue;

                    st.spinel_pending += call_int32(g_res.quest_reward_count, reward);
                    st.pending_quest_count++;
                }
            }
        }
    }

    // Requests completed by SELLING items. Four exist (Viper x3, Gold Chicken
    // Egg, Lunker Bass, Scratched Emerald); each pays Spinels on top of the
    // pesetas from the sale itself. If the items are already in hand those
    // Spinels cost nothing to collect, so they belong in the buy budget.
    if (g_res.get_quest_orders != nullptr && g_res.quest_clear_item != nullptr &&
        g_res.quest_is_cleared != nullptr && g_res.quest_reward_field != nullptr &&
        g_res.quest_reward_count != nullptr) {
        auto o_ret = g_res.get_quest_orders->invoke(g_res.quest_manager, std::vector<void*>{});
        if (!o_ret.exception_thrown && o_ret.ptr != nullptr) {
            auto* orders = (reframework::API::ManagedObject*)o_ret.ptr;
            auto* ol_type = orders->get_type_definition();
            auto* ol_count = cached_find_method(ol_type, "get_Count");
            auto* ol_item = cached_find_method(ol_type, "get_Item");

            if (ol_count != nullptr && ol_item != nullptr) {
                const auto qn = call_int32(ol_count, orders);
                for (int32_t q = 0; q < qn && qn <= 128; ++q) {
                    auto q_ret = ol_item->invoke(orders, {(void*)(intptr_t)q});
                    if (q_ret.exception_thrown || q_ret.ptr == nullptr) continue;
                    auto* order = (reframework::API::ManagedObject*)q_ret.ptr;

                    auto cleared = g_res.quest_is_cleared->invoke(order, std::vector<void*>{});
                    if (cleared.exception_thrown || cleared.byte != 0) continue; // already done

                    auto ci_ret = g_res.quest_clear_item->invoke(order, std::vector<void*>{});
                    if (ci_ret.exception_thrown || ci_ret.ptr == nullptr) continue;
                    auto* arr = (reframework::API::ManagedObject*)ci_ret.ptr;

                    const auto dict_count = *(uint32_t*)((uintptr_t)arr + ARRAY_COUNT_OFFSET);
                    if (dict_count == 0 || dict_count > 8) continue;
                    const auto dicts = (uintptr_t)arr + ARRAY_DATA_OFFSET;

                    bool all_met = true;
                    bool any_requirement = false;
                    std::string desc;

                    for (uint32_t d = 0; d < dict_count && all_met; ++d) {
                        auto* req_dict = *(reframework::API::ManagedObject**)(dicts + d * sizeof(void*));
                        if (req_dict == nullptr) continue;

                        auto* dt = req_dict->get_type_definition();
                        if (g_res.dict_entries_field == nullptr) {
                            g_res.dict_entries_field = dt ? dt->find_field("_entries") : nullptr;
                            if (g_res.dict_entries_field == nullptr) break;
                        }

                        auto* d_count = cached_find_method(dt, "get_Count");
                        if (d_count == nullptr) continue;
                        const auto ec = call_int32(d_count, req_dict);
                        if (ec <= 0 || ec > 32) continue;

                        auto* ents = *(reframework::API::ManagedObject**)
                            g_res.dict_entries_field->get_data_raw(req_dict, false);
                        if (ents == nullptr) continue;
                        const auto base = (uintptr_t)ents + ARRAY_DATA_OFFSET;

                        // Entry is {int hashCode; int next; ItemID key; int value},
                        // all 4-byte, so stride 16 with key at +8 and value at +12.
                        for (int32_t e = 0; e < ec; ++e) {
                            const auto entry = base + (uintptr_t)e * 16;
                            const auto key = *(int32_t*)(entry + 8);
                            const auto need = *(int32_t*)(entry + 12);
                            if (key <= 0 || need <= 0 || need > 99) continue;

                            any_requirement = true;
                            const auto it = holdings.find(key);
                            const auto have = (it != holdings.end()) ? it->second : 0;
                            if (have < need) { all_met = false; break; }

                            if (!desc.empty()) desc += ", ";
                            desc += get_item_name(key);
                            if (need > 1) desc += " x" + std::to_string(need);
                        }
                    }

                    if (!any_requirement || !all_met) continue;

                    auto* reward = *(reframework::API::ManagedObject**)
                        g_res.quest_reward_field->get_data_raw(order, false);
                    if (reward == nullptr) continue;
                    const auto spinels = call_int32(g_res.quest_reward_count, reward);
                    if (spinels <= 0) continue;

                    SellQuestEntry e;
                    e.requirement = std::move(desc);
                    e.spinels = spinels;
                    st.sell_quests.push_back(std::move(e));
                    st.spinel_from_sales += spinels;
                }
            }
        }
    }

    std::vector<PurchasableCandidate> purchasable;
    std::vector<FlatCandidate> flat_candidates;
    if (g_res.get_curr_spinel_count != nullptr) {
        auto ret = g_res.get_curr_spinel_count->invoke(g_res.shop_manager, std::vector<void*>{});
        st.spinel_count = ret.exception_thrown ? 0 : (int32_t)ret.dword;
    }
    if (g_res.get_rewardable_items != nullptr) {
        auto list_ret = g_res.get_rewardable_items->invoke(g_res.shop_manager, std::vector<void*>{});
        if (!list_ret.exception_thrown && list_ret.ptr != nullptr) {
            auto* reward_list = (reframework::API::ManagedObject*)list_ret.ptr;
            auto* rl_type = reward_list->get_type_definition();
            auto* rl_count = cached_find_method(rl_type, "get_Count");
            auto* rl_item = cached_find_method(rl_type, "get_Item");

            if (rl_count != nullptr && rl_item != nullptr) {
                const auto rn = call_int32(rl_count, reward_list);
                for (int32_t i = 0; i < rn && (uint32_t)rn <= MAX_ITEMS; ++i) {
                    auto item_ret = rl_item->invoke(reward_list, {(void*)(intptr_t)i});
                    if (item_ret.exception_thrown || item_ret.ptr == nullptr) continue;
                    auto* item = (reframework::API::ManagedObject*)item_ret.ptr;

                    auto* it_type = item->get_type_definition();
                    auto* get_item_id = cached_find_method(it_type, "get_ItemId");
                    auto* get_spinel = cached_find_method(it_type, "get_SpinelCount");
                    auto* get_qty = cached_find_method(it_type, "get_ItemCount");
                    auto* get_recievable = cached_find_method(it_type, "get_IsRecievable");
                    auto* get_recieved = cached_find_method(it_type, "get_IsRecieved");
                    if (!get_item_id || !get_spinel || !get_qty) continue;

                    // Only what you could actually buy right now - already-
                    // claimed rewards are already in your inventory and
                    // tracked normally elsewhere, not here.
                    if (get_recievable != nullptr) {
                        auto r = get_recievable->invoke(item, std::vector<void*>{});
                        if (r.exception_thrown || r.byte == 0) continue;
                    }
                    if (get_recieved != nullptr) {
                        auto r = get_recieved->invoke(item, std::vector<void*>{});
                        if (!r.exception_thrown && r.byte != 0) continue;
                    }

                    const auto item_id = call_int32(get_item_id, item);
                    const auto spinel_cost = call_int32(get_spinel, item);
                    const auto qty = call_int32(get_qty, item);
                    if (spinel_cost <= 0) continue;

                    const auto name = get_item_name(item_id);

                    TradeItemEntry e;
                    e.name = name;
                    e.spinel_cost = spinel_cost;
                    e.item_count = qty;
                    // sell_price_by_id() returns a negative sentinel (e.g. -1)
                    // for genuinely unsellable items like the treasure map -
                    // clamp rather than show a nonsensical negative resale
                    // value on the page.
                    e.unit_sell_price = std::max(0, sell_price_by_id(item_id));
                    e.total_sell_value = e.unit_sell_price * qty;
                    st.trade_items.push_back(e);

                    // Three buckets. Gems, socketed treasures and herbs all
                    // change what the combine optimisers can reach, so they
                    // need the subset search. Everything else with a resale
                    // price is a flat knapsack item - since spinels cost no
                    // pesetas, buying any of those is pure gain, so the plan
                    // should spend the leftover budget rather than stop at
                    // the combine-relevant entries.
                    const auto& shapes = get_socket_shapes(item_id);
                    const auto ht = herb_type_of_id(item_id);
                    if (is_real_gemstone(item_id)) {
                        purchasable.push_back({name, item_id, spinel_cost, qty,
                                               PurchaseKind::Gem, -1});
                    } else if (!shapes.empty()) {
                        purchasable.push_back({name, item_id, spinel_cost, qty,
                                               PurchaseKind::Treasure, -1});
                    } else if (ht >= 0) {
                        purchasable.push_back({name, item_id, spinel_cost, qty,
                                               PurchaseKind::Herb, ht});
                    } else if (e.total_sell_value > 0 && !excluded_from_buy_plan(item_id)) {
                        flat_candidates.push_back({name, spinel_cost, qty, e.total_sell_value});
                    }
                }
            }
        }
    }

    // Recompute the trade-shop combine plan only when something it actually
    // depends on has changed - see optimize_with_trade_shop()'s comment for
    // why running an exhaustive search unconditionally every 100ms is exactly
    // the pattern this project just spent a long session fixing elsewhere.
    {
        std::string sig = std::to_string(st.spinel_count);
        for (const auto& p : purchasable) {
            sig += "|"; sig += std::to_string(p.item_id);
            sig += ":"; sig += std::to_string(p.spinel_cost);
            sig += ":"; sig += std::to_string(p.qty);
        }
        for (const auto& f : flat_candidates) {
            sig += "|f"; sig += std::to_string(f.spinel_cost);
            sig += ":"; sig += std::to_string(f.value);
        }
        for (const auto& tc : treasure_candidates) {
            sig += "|t"; sig += std::to_string(tc.id);
        }
        for (const auto& [gid, cnt] : gem_pool) {
            sig += "|g"; sig += std::to_string(gid); sig += ":"; sig += std::to_string(cnt);
        }
        for (int i = 0; i < HERB_TYPE_COUNT; ++i) {
            if (herb_counts[i] > 0) {
                sig += "|h"; sig += std::to_string(i); sig += ":"; sig += std::to_string(herb_counts[i]);
            }
        }

        // Budget includes unclaimed quest rewards - they are yours, just not
        // collected yet, and the plan surfaces that as a prerequisite step.
        const int32_t spinel_budget = st.spinel_count + st.spinel_pending + st.spinel_from_sales;
        sig += "|b"; sig += std::to_string(spinel_budget);

        // The search runs on a WORKER THREAD, not here.
        //
        // The signature cache below already stops it running every 100ms, but
        // that means it runs exactly when the inventory changes - i.e. the
        // moment you pick up a herb or a gem. The search is exhaustive over
        // 2^n subsets and calls optimize_herbs() inside every iteration, so
        // the cost is 2^n * (herb search) and it grows sharply as items
        // accumulate. Done inline, that lands as a visible frame hitch on the
        // exact frame the pickup happens.
        //
        // Moving it off-thread is safe because optimize_with_trade_shop() is
        // pure C++ over plain copies - no ManagedObject access, no invoke(),
        // nothing touching the RE Engine at all. The worker never calls into
        // the game, so none of the local_frame_gc / VM lifetime rules that
        // caused trouble elsewhere in this plugin apply to it.
        //
        // While a search is in flight the previous result keeps being
        // published, so the overlay shows a plan that is at most a beat stale
        // rather than stalling the game for a fresh one.
        static std::mutex trade_mutex;
        static std::string published_sig;   // sig the published result describes
        static std::string inflight_sig;    // sig currently being computed
        static std::thread trade_worker;

        // std::thread's destructor calls std::terminate() if the object is
        // still joinable, and a thread stays joinable after it finishes until
        // something joins or detaches it. The only join below happens when the
        // NEXT search starts, so the last worker of the session was always
        // still joinable at shutdown - meaning the process aborted on exit any
        // time the trade shop had been searched even once.
        //
        // Function-local statics are destroyed in reverse order of
        // construction, so a joiner constructed after the thread is destroyed
        // before it, and can join it while it is still a valid object. The
        // search is bounded (2^16 subsets at worst), so this cannot hang.
        struct WorkerJoiner {
            std::thread& t;
            ~WorkerJoiner() { if (t.joinable()) t.join(); }
        };
        static WorkerJoiner trade_worker_joiner{trade_worker};

        static std::atomic<bool> trade_busy{false};

        static double last_gain = 0.0;
        static std::vector<TradeItemEntry> last_recommended;
        static std::vector<TreasureResult> last_combos;
        static std::vector<std::string> last_herb_steps;
        static double last_herb_total = 0.0;
        // The owned-herb optimum for the SAME snapshot the gain describes.
        //
        // The gain published here is at most a beat stale, which is fine on its
        // own - but net worth adds it to a herb value, and that one used to be
        // recomputed fresh on the game thread every poll. For one cycle after
        // picking a herb up, a fresh herb total was being added to a gain that
        // still assumed the herb was sitting in the shop waiting to be bought,
        // so the same value landed in the total twice: net worth jumped 16,000
        // and dropped straight back a sample later. 92 of 4,242 recorded
        // samples in one run carried a spike like that, and the peak the run
        // was judged on sat on top of one, overstating it by 13,500.
        //
        // Publishing the herb figure alongside the gain makes every component
        // of the total describe one state. It is the same contract treasures
        // already follow, and it is the reason net worth now only moves when
        // something in the game actually changed.
        static double last_owned_herb_total = 0.0;
        static std::vector<std::string> last_owned_herb_steps;
        static bool have_published = false;
        // The gem-assignment plan, computed on the worker alongside the trade
        // shop rather than inline every poll - see the note at its call site.
        static std::vector<TreasureResult> last_treasures;
        // The gem pool with the planned gems REMOVED. optimize_treasures
        // consumes the pool it is given, and that remainder is what the
        // leftover-gems list must be built from - see the note where it is
        // applied below.
        static std::unordered_map<int32_t, int32_t> last_gems_remaining;

        bool needs_work = false;
        {
            std::lock_guard<std::mutex> lock(trade_mutex);
            needs_work = (sig != published_sig) && (sig != inflight_sig);
        }

        if (needs_work && !trade_busy.exchange(true)) {
            // Reap the previous worker before starting another. It has already
            // finished (trade_busy was false), so this never blocks.
            if (trade_worker.joinable()) trade_worker.join();

            {
                std::lock_guard<std::mutex> lock(trade_mutex);
                inflight_sig = sig;
            }

            // Everything the search reads is COPIED into the thread. The game
            // thread goes on mutating its own copies immediately afterwards,
            // so sharing by reference here would be a data race.
            trade_worker = std::thread(
                [tc = treasure_candidates, gp = gem_pool, hc = herb_counts,
                 hp = herb_prices, pu = purchasable, fc = flat_candidates,
                 spinel_budget, sig]() mutable {
                    double gain = 0.0;
                    std::vector<TradeItemEntry> recommended;
                    std::vector<TreasureResult> combos;
                    std::vector<std::string> steps;
                    double herb_total = 0.0;
                    double owned_herb_total = 0.0;
                    std::vector<std::string> owned_steps;

                    optimize_with_trade_shop(tc, gp, hc, hp, pu, fc, spinel_budget,
                                             gain, recommended, combos, steps, herb_total,
                                             owned_herb_total, owned_steps);

                    // The gem-assignment plan for the treasures you already
                    // own. Same inputs, same signature, so it belongs on the
                    // same worker rather than being redone on the game thread.
                    // optimize_treasures consumes the pool it is given, hence
                    // the fresh copies.
                    std::vector<TreasureResult> treasures_out;
                    auto gems_remaining = gp;   // consumed in place below
                    {
                        auto tc_copy = tc;
                        optimize_treasures(tc_copy, gems_remaining, treasures_out);
                    }

                    {
                        std::lock_guard<std::mutex> lock(trade_mutex);
                        last_gain = gain;
                        last_recommended = std::move(recommended);
                        last_combos = std::move(combos);
                        last_herb_steps = std::move(steps);
                        last_herb_total = herb_total;
                        last_owned_herb_total = owned_herb_total;
                        last_owned_herb_steps = std::move(owned_steps);
                        have_published = true;
                        last_treasures = std::move(treasures_out);
                        last_gems_remaining = std::move(gems_remaining);
                        published_sig = std::move(sig);
                        inflight_sig.clear();
                    }
                    trade_busy = false;
                });
        }

        std::lock_guard<std::mutex> lock(trade_mutex);

        st.trade_shop_gain = last_gain;
        have_trade_snapshot = have_published;
        snapshot_owned_herb_total = last_owned_herb_total;
        snapshot_owned_herb_steps = last_owned_herb_steps;
        st.trade_shop_recommended = last_recommended;
        st.trade_shop_combos = last_combos;
        st.trade_shop_herb_steps = last_herb_steps;
        st.trade_shop_herb_total = last_herb_total;
        st.treasures = last_treasures;

        // Take the CONSUMED pool back, or the gems now sitting inside those
        // treasures get counted a second time as loose stock.
        //
        // optimize_treasures() removes the gems it assigns from the pool it is
        // handed, and the leftover-gems list is built from whatever remains.
        // When this moved to the worker it was given a throwaway copy, so the
        // real pool was never decremented and every socketed gem was counted
        // twice - once inside the treasure's finalPrice and again as leftover.
        // That inflated the total by exactly the value of the socketed gems
        // (141,000 in the case that exposed it: 1,063,100 reported against
        // 922,100 actually realised).
        //
        // Only adopted once a plan has been published. Before that
        // last_treasures is empty, so no treasure value is being counted and
        // the full pool is the correct leftover set.
        if (!published_sig.empty()) {
            gem_pool = last_gems_remaining;
        }
    }

    // optimize_treasures() is NOT called here any more.
    //
    // It used to run inline on every poll - ten times a second, on the game
    // thread, unconditionally. That was survivable with a small inventory and
    // became a measurable frame-rate drop as treasures accumulated (picking up
    // a second Elegant Bangle took the game to 57fps). Worse, when the trade
    // shop search moved to a worker I raised THIS search's node budget from 2M
    // to 20M on the mistaken belief it had moved too, making the worst case
    // ten times slower on the very thread that renders the game.
    //
    // It now runs on the worker above, guarded by the same signature, so it
    // recomputes only when the treasures or gem pool actually change and never
    // blocks a frame.

    // Attach what is physically socketed, so the page can show at a glance
    // whether the current arrangement already matches the recommendation.
    // Consumed per piece, so duplicates of one treasure each get their own.
    {
        auto remaining = actual_sockets;
        auto remaining_order = treasure_gui_order;
        for (auto& r : st.treasures) {
            auto it = remaining.find(r.item_id);
            if (it != remaining.end() && !it->second.empty()) {
                r.actual_gems = std::move(it->second.front());
                it->second.erase(it->second.begin());
            }
            auto ord = remaining_order.find(r.item_id);
            if (ord != remaining_order.end() && !ord->second.empty()) {
                r.gui_order = ord->second.front();
                ord->second.erase(ord->second.begin());
            } else {
                r.gui_order = INT32_MAX; // unknown position sorts last
            }
        }

        // List in the same order the treasure screen draws, so the plan can be
        // followed by reading down the grid.
        std::sort(st.treasures.begin(), st.treasures.end(),
                  [](const TreasureResult& a, const TreasureResult& b) {
                      return a.gui_order < b.gui_order;
                  });

        // Number same-named pieces so two Elegant Masks with different
        // recommended gems can be told apart.
        std::unordered_map<std::string, int32_t> name_totals;
        for (const auto& r : st.treasures) name_totals[r.name]++;
        std::unordered_map<std::string, int32_t> seen_names;
        for (auto& r : st.treasures) {
            if (name_totals[r.name] > 1) r.duplicate_ordinal = ++seen_names[r.name];
        }
    }

    double items_total = 0.0;
    for (auto& [key, acc] : regular) {
        (void)key;
        ItemEntry e;
        e.name = acc.name;
        e.count = acc.count;
        e.unit_price = acc.unit_price;
        e.total_price = acc.unit_price * acc.count;
        items_total += e.total_price;
        st.items.push_back(std::move(e));
    }

    double plain_total = 0.0;
    for (auto& [id, acc] : plain_treasure) {
        (void)id;
        ItemEntry e;
        e.name = acc.name;
        e.count = acc.count;
        e.unit_price = acc.unit_price;
        e.total_price = acc.unit_price * acc.count;
        plain_total += e.total_price;
        st.plain_treasures.push_back(std::move(e));
    }

    double leftover_total = 0.0;
    for (const auto& [gid, cnt] : gem_pool) {
        if (cnt <= 0) continue;
        ItemEntry e;
        e.name = get_item_name(gid);
        e.count = cnt;
        e.unit_price = sell_price_by_id(gid);
        e.total_price = e.unit_price * cnt;
        leftover_total += e.total_price;
        st.leftover_gems.push_back(std::move(e));
    }

    double treasures_total = 0.0;
    for (const auto& t : st.treasures) treasures_total += t.final_price;

    double key_items_total = 0.0;
    for (const auto& e : st.key_items) key_items_total += e.total_price;

    // --- Herbs ---
    // Valued at the best reachable combination rather than as-is. The raw
    // figure is kept alongside so the page can show what combining is worth.
    {
        for (int i = 0; i < HERB_TYPE_COUNT; ++i) {
            if (herb_counts[i] <= 0) continue;
            ItemEntry e;
            e.name = HERB_NAMES[i];
            e.count = herb_counts[i];
            e.unit_price = herb_prices[i];
            e.total_price = herb_prices[i] * herb_counts[i];
            st.herb_raw_total += e.total_price;
            st.herbs.push_back(std::move(e));
        }

        // Cached on the herb counts and prices. optimize_herbs() allocates a
        // fresh memo map and recurses on every call, and this one ran
        // unconditionally at the 100ms poll rate even when nothing about the
        // herbs had changed - which is most of the time.
        static HerbCounts cached_counts{};
        static std::array<int32_t, HERB_TYPE_COUNT> cached_prices{};
        static double cached_total = 0.0;
        static std::vector<std::string> cached_steps;
        static bool cache_valid = false;

        if (!cache_valid || cached_counts != herb_counts || cached_prices != herb_prices) {
            std::vector<std::pair<int, int>> steps;
            cached_total = optimize_herbs(herb_counts, herb_prices, &steps);
            cached_steps.clear();
            for (const auto& [a, b] : steps) {
                cached_steps.push_back(std::string(HERB_NAMES[a]) + " + " + HERB_NAMES[b]);
            }
            cached_counts = herb_counts;
            cached_prices = herb_prices;
            cache_valid = true;
        }

        st.herb_total = cached_total;
        st.herb_steps = cached_steps;

        // Prefer the figure published alongside the trade shop gain, so the
        // two addends in st.total describe the same game state. The freshly
        // cached value above is the fallback for the very first polls, before
        // any worker result exists - at that point there is no gain to be
        // inconsistent with, so it is safe.
        if (have_trade_snapshot) {
            st.herb_total = snapshot_owned_herb_total;
            st.herb_steps = snapshot_owned_herb_steps;
        }
    }

    // --- Ammo crafting (advisory) ---
    if (craft_smalls > 0 || craft_larges > 0) {
        std::array<int32_t, CRAFT_RECIPE_COUNT> ammo_prices{};
        for (int r = 0; r < CRAFT_RECIPE_COUNT; ++r) {
            ammo_prices[r] = std::max(0, sell_price_by_id(CRAFT_RECIPES[r].ammo_id));
        }
        st.craft_gain = optimize_crafting(
            craft_smalls, craft_larges, craft_powder,
            std::max(0, sell_price_by_id(ITEM_RESOURCES_S)),
            std::max(0, sell_price_by_id(ITEM_RESOURCES_L)),
            std::max(0, sell_price_by_id(ITEM_GUNPOWDER)),
            ammo_prices, &st.craft_plan);
    }

    for (auto& c : st.craft_plan) {
        for (const auto& r : CRAFT_RECIPES) {
            if (c.name == r.name) c.local_name = get_item_name(r.ammo_id);
        }
        if (c.local_name.empty()) c.local_name = c.name;
    }
    for (int i = 0; i < HERB_TYPE_COUNT; ++i) {
        st.herb_english[i] = HERB_NAMES[i];
        st.herb_local[i] = get_item_name(HERB_ITEM_IDS[i]);
        if (st.herb_local[i].empty()) st.herb_local[i] = HERB_NAMES[i];
    }
    st.ammo_bonus_pct = (int32_t)(g_ammo_bonus_pct + 0.5);
    st.heal_bonus_pct = (int32_t)(g_heal_bonus_pct + 0.5);

    st.total = st.pesetas + items_total + treasures_total + plain_total
             + leftover_total + storage_total + key_items_total + st.trade_shop_gain
             + st.herb_total;

    // --- Completeness gate ---
    //
    // Loading a save does not hand the inventory over all at once: for a
    // stretch of polls the controllers are still filling, so the figure climbs
    // through partial states before settling. Published raw, that reads as the
    // run gaining hundreds of thousands in a second - one such partial put a
    // 250,616 low into chapter 5 (items 17 -> 0 -> 17, treasures 4 -> 0 -> 4,
    // pesetas untouched), and the chapter summary then reported a gain of
    // 465,410 that never happened.
    //
    // The gate is PERSISTENCE, and deliberately nothing else.
    //
    // The obvious rule - reject any reading with an empty inventory while the
    // player clearly has one - is wrong here, and the game says so at the
    // start of chapter 2, where the inventory is confiscated outright. Items
    // go 11 -> 0 while pesetas sit untouched at 7,300. That is a real state
    // the run genuinely passed through, and a content rule would have refused
    // to publish it, freezing the overlay on the pre-theft figure for the rest
    // of the chapter.
    //
    // A mid-load partial and a post-theft inventory are indistinguishable from
    // content alone: both are an empty bag with the money intact. What tells
    // them apart is that the theft STAYS and the partial does not. So the only
    // test is whether a figure survives to the next poll. A genuine change
    // costs one poll (100ms) before it shows; a staged load never settles
    // long enough to be published at all.
    //
    // The timeout is the safety valve: anything that legitimately changes on
    // every single poll must still get through, or the overlay would freeze.
    {
        static double last_seen_total = -1.0;
        static int    stable_polls = 0;
        static auto   last_publish = std::chrono::steady_clock::now();
        constexpr auto MAX_HOLD = std::chrono::milliseconds(3000);

        if (st.total == last_seen_total) ++stable_polls; else stable_polls = 0;
        last_seen_total = st.total;

        const auto now = std::chrono::steady_clock::now();
        const bool timed_out = (now - last_publish) >= MAX_HOLD;

        if (stable_polls >= 1 || timed_out) {
            last_publish = now;
            ui_publish(st);
            std::lock_guard<std::mutex> lock(g_out_mutex);
            g_pending = std::move(st);
            g_has_pending = true;
        }
    }
    g_out_cv.notify_one();

    // Bisection isolated the bug to getItems() specifically - 51+ minutes
    // clean with it skipped entirely vs. every test that called it crashing
    // at 5-7 minutes, regardless of what was done with the result. getItems()
    // almost certainly allocates a fresh list on every call rather than
    // returning an existing reference; at 3 calls/poll * 10 polls/sec that is
    // thousands of allocations a minute from native-code-initiated calls.
    // local_frame_gc() exists on VMContext specifically to release objects
    // rooted by such calls (the same local-reference-frame model JNI uses,
    // where an unreleased frame is a well-known source of exactly this kind
    // of delayed, unrelated-looking corruption) and was never being called
    // anywhere in this plugin. Running it once per poll, after this poll's
    // own invoke() calls are done, keeps that frame from ever accumulating.
    //
    // Confirmed by A/B test: with this call disabled, the game crashed with
    // the identical signature (RIP 7ff6a1337307, RCX 0xffffffff, RAX 0) after
    // 8m09s of real gameplay. With it enabled, the previous session ran a
    // full 20m17s of real gameplay and closed clean. Same code otherwise,
    // one variable flipped - this is the fix.
    api->get_vm_context()->local_frame_gc();
}

void on_present_body() {
    if (g_res.disabled) return;

    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_poll < POLL_INTERVAL) return;
    g_last_poll = now;

    if (!g_res.ready) {
        resolve();
        return;
    }

    g_tick.fetch_add(1, std::memory_order_relaxed);

    // Leave the game alone while it is rewriting inventory - see
    // game_is_mutating_inventory().
    //
    // The steady state was already guarded, but the *transition* was not: the
    // frame isOpenGui goes false, the game is still tearing the shop down and
    // finalising the mutation, and resuming on that exact frame is the worst
    // possible moment to start walking those structures. So hold off until the
    // screen has been shut for SETTLE_AFTER_GUI before reading anything.
    static auto last_busy = std::chrono::steady_clock::time_point{};
    constexpr auto SETTLE_AFTER_GUI = std::chrono::milliseconds(750);

    if (game_is_mutating_inventory()) {
        last_busy = now;
        g_paused.store(true, std::memory_order_relaxed);
        ui_set_paused(true);
        return;
    }
    if (last_busy.time_since_epoch().count() != 0 && now - last_busy < SETTLE_AFTER_GUI) {
        g_paused.store(true, std::memory_order_relaxed);
        ui_set_paused(true);
        return;
    }
    g_paused.store(false, std::memory_order_relaxed);
    ui_set_paused(false);

    poll();
}

// Runs on the render thread: an escaping exception here is a hard crash, so the
// body is contained and any failure disables the plugin rather than recurring
// every frame.
void on_present() {
    try {
        on_present_body();
    } catch (const std::exception& e) {
        log_error("exception in on_present: %s - disabling", e.what());
        g_res.disabled = true;
    } catch (...) {
        log_error("unknown exception in on_present - disabling");
        g_res.disabled = true;
    }
}

} // namespace

extern "C" __declspec(dllexport) void
reframework_plugin_required_version(REFrameworkPluginVersion* version) {
    version->major = REFRAMEWORK_PLUGIN_VERSION_MAJOR;
    version->minor = REFRAMEWORK_PLUGIN_VERSION_MINOR;
    version->patch = REFRAMEWORK_PLUGIN_VERSION_PATCH;
}

extern "C" __declspec(dllexport) bool
reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param) {
    if (param == nullptr) return false;

    re4::set_functions(param->functions, "re4_net_worth");
    reframework::API::initialize(param);

    g_data_dir = resolve_data_dir();
    log_info("plugin loaded, data dir = %s", g_data_dir.c_str());

    // The in-game window and widget. Optional: without them the plugin still
    // writes net_worth.json exactly as before. Before the writer starts, so
    // the browser view's folder and settings exist by its first write.
    if (!ui_initialize(param->functions, g_data_dir)) {
        log_error("could not register the window callbacks - window disabled");
    }

    std::thread(writer_thread_main).detach();

    if (param->functions->on_present == nullptr || !param->functions->on_present(on_present)) {
        log_error("FAILED to register on_present callback");
        return false;
    }
    return true;
}
