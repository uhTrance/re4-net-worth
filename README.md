# RE4 Net Worth

A [REFramework](https://github.com/praydog/REFramework) plugin for **Resident Evil 4 (2023)** that works out your net worth live: everything you own, valued at what the merchant would pay for it, plus the best way to combine your treasures, gems and herbs before you sell.

Nothing is bought, sold or changed. The plugin only reads the game.

## Features

- **Net worth, live:** pesetas plus the resale value of your inventory, storage, sellable key items, treasures and gems.
- **Best treasure combinations:** finds the most valuable way to socket the gems you own (loose or already socketed) into your treasures, following the game's shape and colour bonus rules. Green gems are already in place, red ones still need socketing.
- **Herbs valued combined:** herbs count at their best mix (G+R+Y sells for 10,000), with the combines to make.
- **Weapons with upgrades:** valued as the merchant pays for them, base price plus 95% of what you spent upgrading.
- **Charm bonuses:** ammo and recovery-item charm bonuses are included in the prices.
- **The plan:** one ordered list of what to do at the merchant to get the figure shown.
- **Separate Ways:** the Trade shop plan (what to buy with Spinels and combine), sell-to-complete requests and unclaimed Spinels.
- **Ammo crafting advice:** what crafting your resources would add. Shown for you to judge, never counted, since crafting cannot be undone.
- **Widget:** a small net worth panel on top of the game, rolling to each new figure with the change shown under it. Toggleable and movable.
- **Browser view:** the full breakdown as a live page for a second monitor.
- **Dark mode** (the default) and light mode. The window sizes itself to your resolution, with Size, Width and Height sliders.

## Requirements

- Resident Evil 4 (2023) on PC
- [REFramework](https://github.com/praydog/REFramework-nightly/releases) (`REFramework.zip`)

## Install

1. Install REFramework.
2. Extract the release zip into the game folder. It adds three things under `reframework`: the plugin in `plugins`, the browser page in `data\re4_net_worth` and the window's font in `fonts\re4_net_worth`. Fluffy Mod Manager can install the zip as well.
3. Start the game and load a save. The **RE4 Net Worth** window and the widget appear on their own. Toggle either from the REFramework menu (Insert key) under *RE4 Net Worth*.

The window and the widget take the mouse while the REFramework menu is open: drag the widget to move it.

Settings are saved to `reframework\data\re4_net_worth_window.json`. The plugin also writes `reframework\data\net_worth.json` with the full breakdown, for stream overlays or your own tools.

## Browser view

In the game window, click **Copy browser link** and paste it into your browser's address bar. You can also open `reframework\data\re4_net_worth\NetWorth.html` from the game folder directly. It updates live with no server or extra program, and follows the window's dark mode setting.

The plugin feeds the page by writing `net_worth.js` next to it. Untick *Browser view* in the game window to stop that.

## Building

Needs Visual Studio 2022 (or the Build Tools) with *Desktop development with C++*. Run `build.bat`; the DLL lands in `build\`. `python build.py` then makes the release zip.

The window draws into REFramework's own ImGui context, so `third_party\imgui` must be the same Dear ImGui commit REFramework is built with (currently 1.92.0, `85b2fe8`), with matching options in `src\imgui_config.hpp`. If a REFramework update moves to a newer ImGui, update both to match.

## Licence

MIT. Bundled third-party code and assets:

- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut, MIT (`third_party/imgui/LICENSE.txt`)
- REFramework plugin API headers by praydog, MIT (`include/reframework`)
- Courier Prime Bold by The Courier Prime Project Authors, SIL Open Font License 1.1 (`reframework/fonts/re4_net_worth/OFL.txt`)
