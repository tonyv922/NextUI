#include "btmenu.hpp"
#include "keyboardprompt.hpp"
#ifdef HAS_BTAGENT
#   include "btagent.hpp"
#endif

#include <unordered_set>
#include <map>

#include <mutex>
#include <shared_mutex>
typedef std::shared_mutex Lock;
typedef std::unique_lock<Lock> WriteLock;
typedef std::shared_lock<Lock> ReadLock;

using namespace Bluetooth;
using namespace std::placeholders;

Menu::Menu(const int &globalQuit, int &globalDirty) : MenuList(MenuItemType::Fixed, "Network", {}), globalQuit(globalQuit), globalDirty(globalDirty)
{
    toggleItem = new MenuItem(ListItemType::Generic, "Bluetooth", "Enable/disable Bluetooth", {false, true}, {"Off", "On"},
                              std::bind(&Menu::getBtToggleState, this),
                              std::bind(&Menu::setBtToggleState, this, std::placeholders::_1),
                              std::bind(&Menu::resetBtToggleState, this));
    diagItem = new MenuItem(ListItemType::Generic, "Bluetooth diagnostics", "Enable/disable Bluetooth logging", {false, true}, {"Off", "On"},
                              std::bind(&Menu::getBtDiagnosticsState, this),
                              std::bind(&Menu::setBtDiagnosticsState, this, std::placeholders::_1),
                              std::bind(&Menu::resetBtDiagnosticsState, this));
    items.push_back(toggleItem);
    items.push_back(diagItem);
    rateItem = new MenuItem(ListItemType::Generic, "Maximum sampling rate", "44100 Hz: better compatibility\n48000 Hz: better quality", {44100, 48000}, {"44100 Hz", "48000 Hz"},
                              std::bind(&Menu::getSamplerateMaximum, this),
                              std::bind(&Menu::setSamplerateMaximum, this, std::placeholders::_1),
                              std::bind(&Menu::resetSamplerateMaximum, this));
    items.push_back(rateItem);

    // show the entry from the first frame, even while Bluetooth is off:
    // it says so instead of hiding, so users don't hunt for it (tonio UX note)
    offEntryItem = new MenuItem(ListItemType::Button, "Connect Bluetooth",
            "Turn Bluetooth on first.", DeferToSubmenu,
            new MenuList(MenuItemType::List, "Connect Bluetooth", {
                new MenuItem(ListItemType::Button, "Bluetooth is off",
                        "Enable Bluetooth above to connect paired devices."),
            }));
    items.push_back(offEntryItem);
    // best effort layout based on the platform defines, user should really call performLayout manually
    MenuList::performLayout((SDL_Rect){0, 0, FIXED_WIDTH, FIXED_HEIGHT});
    layout_called = false;

#ifdef HAS_BTAGENT
    // Only NoInputNoOutput for now, but this needs to interact with the UI thread if we 
    // ever want to show a PIN or passkey
    pairingAgent = new PairingAgent();
    pairingAgent->startPairingWindow();
#endif

    worker = std::thread{&Menu::updater, this};
}

Menu::~Menu()
{
    {
        std::lock_guard<std::mutex> lk(wakeLock);
        quit = true;
    }
    wakeCv.notify_all(); // don't sit out the remaining sleep on exit
    if (worker.joinable())
        worker.join();

#ifdef HAS_BTAGENT
    pairingAgent->stopPairingWindow();
    delete pairingAgent;
#endif
}

InputReactionHint Menu::handleInput(int &dirty, int &quit)
{
    auto ret = MenuList::handleInput(dirty, quit);
    if (selectionDirty)
    {
        dirty = true;
        selectionDirty = false; // handled
        //LOG_info("collected selectionDirty\n");
    }
    return ret;
}

std::any Menu::getBtToggleState() const
{
    return BT_enabled();
}

void Menu::setBtToggleState(const std::any &on)
{
    auto state = std::any_cast<bool>(on);
    ScopedOverlay overlay(state ? "Enabling Bluetooth..." : "Disabling Bluetooth...");
    BT_enable(state);
    // don't make the updater wait out its poll cycle to reflect the entry
    {
        std::lock_guard<std::mutex> lk(wakeLock);
        wakeGen++;
    }
    wakeCv.notify_all();
}

void Menu::resetBtToggleState()
{
    //
}

std::any Menu::getBtDiagnosticsState() const
{
    return BT_diagnosticsEnabled();
}

void Menu::setBtDiagnosticsState(const std::any &on)
{
    BT_diagnosticsEnable(std::any_cast<bool>(on));
}

void Menu::resetBtDiagnosticsState()
{
    //
}

std::any Menu::getSamplerateMaximum() const
{
    return CFG_getBluetoothSamplingrateLimit();
}

void Menu::setSamplerateMaximum(const std::any &value)
{
    CFG_setBluetoothSamplingrateLimit(std::any_cast<int>(value));
}

void Menu::resetSamplerateMaximum()
{
    CFG_setBluetoothSamplingrateLimit(CFG_DEFAULT_BLUETOOTH_MAXRATE);
}

template <typename Map>
bool key_compare(Map const &lhs, Map const &rhs)
{
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                                                  [](auto a, auto b)
                                                  { return a.first == b.first; });
}

void Menu::updater()
{
    int pollSecs = 15;
    std::string lastPairedSig;
    std::map<std::string, BT_device> scanMap;
    bool scanValid = false; // scanMap holds real data from the stack
    bool scanDirty = false; // scan changed, needs rebuild once submenu closes
    int scanCycles = 0;     // countdown until next slow scan refresh

    while (!quit && !globalQuit)
    {
        // snapshot before doing work: a toggle landing mid-iteration must
        // still cut the sleep short at the end of this pass
        unsigned genTop;
        {
            std::lock_guard<std::mutex> lk(wakeLock);
            genTop = wakeGen;
        }
        // TODO: pause when menu is not rendered
        // TODO: improve repaint logic in a way that remembers selection
        // Scan
        if (BT_enabled())
        {
            // FAST PATH first: the paired list is a single bluetoothctl call.
            // The discovery kick below can block for tens of seconds while
            // bluetoothd cold-starts, so it must never delay this rebuild —
            // otherwise the "Turn Bluetooth on first" placeholder from the
            // off-state lingers while the toggle already reads On.
            std::map<std::string, BT_devicePaired> pairedMap;
            std::vector<BT_devicePaired> kl(SCAN_MAX_RESULTS);
            int known = BT_pairedDevices(kl.data(), SCAN_MAX_RESULTS);
            std::string pairedSig;
            for (int i = 0; i < known; i++)
            {
                pairedMap.emplace(kl[i].remote_addr, kl[i]); // Use MAC address as key (unique)
                pairedSig += kl[i].remote_addr;
                pairedSig += ';';
            }

            // dont repopulate if any submenu is open
            bool menuOpen = false;
            for (auto i : items)
            {
                if (i->isDeferred())
                {
                    menuOpen = true;
                    break;
                }
            }

            // rebuild when paired list changed or a scan result is pending
            bool pairedChanged = pairedSig != lastPairedSig;
            if (!menuOpen && (pairedChanged || !scanValid || scanDirty))
            {
                // remember selection and restore
                std::string selectedName;
                bool selectionApplied = false;

                {
                    WriteLock w(itemLock);
                    selectedName = getSelectedItemName();
                    items.clear();
                    items.push_back(toggleItem);
                    items.push_back(diagItem);
                    if (rateItem) items.push_back(rateItem);
                    layout_called = false;

                    // "Connect Bluetooth" entry: drill into the paired device list,
                    // where A connects/disconnects a device directly.
                    std::string entryName = pairedMap.empty()
                                                ? std::string("Connect Bluetooth")
                                                : "Connect Bluetooth (" + std::to_string(pairedMap.size()) + ")";
                    MenuList *pairedOptions;
                    if (pairedMap.empty())
                    {
                        pairedOptions = new MenuList(MenuItemType::List, entryName, {
                            new MenuItem(ListItemType::Button, "No paired devices", "Pair a device from the list below first."),
                        });
                    }
                    else
                    {
                        std::vector<AbstractMenuItem *> quickItems;
                        for (auto &[s, r] : pairedMap)
                            quickItems.push_back(new QuickConnectItem(r, selectionDirty));
                        pairedOptions = new MenuList(MenuItemType::List, entryName, quickItems);
                    }
                    items.push_back(new MenuItem(ListItemType::Button, entryName, "Paired devices - press A to connect", DeferToSubmenu, pairedOptions));

                    // then scan results, skipping anything already paired (match by MAC).
                    // Empty on the very first pass; fills in once the slow scan lands.
                    for (auto &[s, r] : scanMap)
                    {
                        if (pairedMap.count(r.addr))
                            continue;
                        MenuList *options;
                        options = new MenuList(MenuItemType::List, "Options", {new PairNewItem(r, selectionDirty)});
                        auto itm = new PairableItem{r, options};
                        items.push_back(itm);
                    }

                    if (pairedChanged) lastPairedSig = pairedSig;
                    if (scanDirty) scanDirty = false;
                }
                MenuList::performLayout((SDL_Rect){0, 0, FIXED_WIDTH, FIXED_HEIGHT});

                // Attempt to restore prev selection
                selectionApplied = selectByName(selectedName);
                globalDirty |= selectionApplied;
                // If selection was restored, we already called performLayout internally
                selectionDirty |= !selectionApplied;
            }

            // SLOW PATH: full scan runs AFTER the menu above is already drawn.
            // Kick discovery here (can stall on a cold bluetoothd, but the
            // menu is already correct by now). Each pass shells out per
            // device, so only refresh every ~20s.
            if(!BT_discovering())
                BT_discovery(true);
            if (scanCycles <= 0)
            {
                std::map<std::string, BT_device> fresh;
                std::vector<BT_device> sr(SCAN_MAX_RESULTS);
                int cnt = BT_availableDevices(sr.data(), SCAN_MAX_RESULTS);
                for (int i = 0; i < cnt; i++)
                    fresh.emplace(sr[i].addr, sr[i]);

                bool first = !scanValid;
                scanValid = true;
                scanCycles = 10; // ~20s at the 2s poll below
                if (first || !key_compare(fresh, scanMap))
                {
                    scanMap = fresh;
                    scanDirty = true; // picked up by the next fast tick
                }
            }
            scanCycles--;
            pollSecs = 2;
        }
        else
        {
            WriteLock w(itemLock);
            items.clear();
            items.push_back(toggleItem);
            items.push_back(diagItem);
            if (rateItem) items.push_back(rateItem);
            if (offEntryItem) items.push_back(offEntryItem);
            layout_called = false;
            selectionDirty = true;
            pollSecs = 15;
            // stale once BT is back off, force a fresh pass on next enable
            lastPairedSig.clear();
            scanMap.clear();
            scanValid = false;
            scanDirty = false;
            scanCycles = 0;
        }

        // reset selection scope (locks internally)
        if (selectionDirty)
        {
            MenuList::performLayout((SDL_Rect){0, 0, FIXED_WIDTH, FIXED_HEIGHT});
            selectionDirty = false;
        }

        // interruptible wait: the on/off callback bumps wakeGen so a toggle
        // (or app quit) lands on screen immediately, not up to pollSecs later
        {
            std::unique_lock<std::mutex> lk(wakeLock);
            wakeCv.wait_for(lk, std::chrono::seconds(pollSecs),
                    [this, genTop] { return quit || wakeGen != genTop; });
        }
    }
}

PairNewItem::PairNewItem(BT_device d, bool& dirty)
    : MenuItem(ListItemType::Button, "Pair", "Pair this device.", 
        [&](AbstractMenuItem &item) -> InputReactionHint {
            ScopedOverlay overlay("Pairing...");
            BT_pair(dev.addr); 
            dirty = true;
            return Exit; 
        }), dev(d)
{}

UnpairItem::UnpairItem(BT_devicePaired d, bool& dirty)
    : MenuItem(ListItemType::Button, "Forget", "Forget this device.",
        [&](AbstractMenuItem &item) -> InputReactionHint {
            BT_unpair(dev.remote_addr); 
            dirty = true;
            return Exit; 
        }), dev(d)
{}

ConnectKnownItem::ConnectKnownItem(BT_devicePaired d, bool& dirty)
    : MenuItem(ListItemType::Button, "Connect", "Connect this device.",
        [&](AbstractMenuItem &item) -> InputReactionHint {
            ScopedOverlay overlay("Connecting...");
            BT_connect(dev.remote_addr); 
            dirty = true;
            return Exit; 
        }), dev(d)
{}

DisconnectKnownItem::DisconnectKnownItem(BT_devicePaired d, bool& dirty)
    : MenuItem(ListItemType::Button, "Disconnect", "Disconnect this device.",
        [&](AbstractMenuItem &item) -> InputReactionHint {
            BT_disconnect(dev.remote_addr); 
            dirty = true;
            return Exit; 
        }), dev(d)
{}

PairableItem::PairableItem(BT_device d, MenuList* submenu)
    : MenuItem(ListItemType::Custom, d.name, d.addr, DeferToSubmenu, submenu), dev(d)
{}

void PairableItem::drawCustomItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected) const
{
    SDL_Color text_color = uintToColour(THEME_COLOR4_255);
    SDL_Surface *text = TTF_RenderUTF8_Blended(font.tiny, item.getLabel().c_str(), COLOR_WHITE); // always white

    // hack - this should be correlated to max_width
    int mw = dst.w;

    if (selected)
    {
        // gray pill
        GFX_blitPillLightCPP(ASSET_BUTTON, surface, {dst.x, dst.y, mw, SCALE1(BUTTON_SIZE)});
    }

    // device icon
    if(dev.kind != BLUETOOTH_NONE) {
        auto asset = (dev.kind == BLUETOOTH_AUDIO) ? ASSET_AUDIO : ASSET_CONTROLLER;
        SDL_Rect rect = (dev.kind == BLUETOOTH_AUDIO) ? SDL_Rect{0, 0, 12, 12} : SDL_Rect{0, 0, 12, 12};
        int ix = dst.x + dst.w - SCALE1(OPTION_PADDING + rect.w);
        int y = dst.y + SCALE1(BUTTON_SIZE - rect.h) / 2;
        SDL_Rect tgt{ix, y};
        GFX_blitAssetColor(asset, NULL, surface, &tgt, THEME_COLOR6);
    }

    if (selected)
    {
        // white pill
        int w = 0;
        TTF_SizeUTF8(font.small, item.getName().c_str(), &w, NULL);
        w += SCALE1(OPTION_PADDING * 2);
        GFX_blitPillDarkCPP(ASSET_BUTTON, surface, {dst.x, dst.y, w, SCALE1(BUTTON_SIZE)});
        text_color = uintToColour(THEME_COLOR5_255);
    }

    text = TTF_RenderUTF8_Blended(font.small, item.getName().c_str(), text_color);
    SDL_BlitSurfaceCPP(text, {}, surface, {dst.x + SCALE1(OPTION_PADDING), dst.y + SCALE1(1)});
    SDL_FreeSurface(text);
}

PairedItem::PairedItem(BT_devicePaired d, MenuList* submenu)
    : MenuItem(ListItemType::Custom, d.remote_name, d.remote_addr, DeferToSubmenu, submenu), dev(d)
{}

void PairedItem::drawCustomItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected) const
{
    SDL_Color text_color = uintToColour(THEME_COLOR4_255);
    SDL_Surface *text = TTF_RenderUTF8_Blended(font.tiny, item.getLabel().c_str(), COLOR_WHITE); // always white

    // hack - this should be correlated to max_width
    int mw = dst.w;

    if (selected)
    {
        // gray pill
        GFX_blitPillLightCPP(ASSET_BUTTON, surface, {dst.x, dst.y, mw, SCALE1(BUTTON_SIZE)});
    }

    // rssi icon
    auto asset =
        dev.rssi == 0   ? ASSET_WIFI_OFF : 
        dev.rssi >= -55 ? ASSET_WIFI :
        dev.rssi >= -67 ? ASSET_WIFI_MED
                        : ASSET_WIFI_LOW;
    SDL_Rect rect = {0, 0, 12, 12};
    int ix = dst.x + dst.w - SCALE1(OPTION_PADDING + rect.w);
    int y = dst.y + SCALE1(BUTTON_SIZE - rect.h) / 2;
    SDL_Rect tgt{ix, y};
    GFX_blitAssetColor(asset, NULL, surface, &tgt, THEME_COLOR6);

    // connected
    if(dev.is_connected) {
        SDL_Rect rect = {0, 0, 12, 12};
        ix = ix - SCALE1(OPTION_PADDING + rect.w);
        int y = dst.y + SCALE1(BUTTON_SIZE - rect.h) / 2;
        SDL_Rect tgt{ix, y};
        GFX_blitAssetColor(ASSET_CHECKCIRCLE, NULL, surface, &tgt, THEME_COLOR6);
    }
    // bonded
    else if(dev.is_bonded) {
        SDL_Rect rect = {0, 0, 8, 11};
        ix = ix - SCALE1(OPTION_PADDING + rect.w + 2);
        int y = dst.y + SCALE1(BUTTON_SIZE - rect.h) / 2;
        SDL_Rect tgt{ix, y};
        GFX_blitAssetColor(ASSET_LOCK, NULL, surface, &tgt, THEME_COLOR6);
    }

    if (selected)
    {
        // white pill
        int w = 0;
        TTF_SizeUTF8(font.small, item.getName().c_str(), &w, NULL);
        w += SCALE1(OPTION_PADDING * 2);
        GFX_blitPillDarkCPP(ASSET_BUTTON, surface, {dst.x, dst.y, w, SCALE1(BUTTON_SIZE)});
        text_color = uintToColour(THEME_COLOR5_255);
    }

    text = TTF_RenderUTF8_Blended(font.small, item.getName().c_str(), text_color);
    SDL_BlitSurfaceCPP(text, {}, surface, {dst.x + SCALE1(OPTION_PADDING), dst.y + SCALE1(1)});
    SDL_FreeSurface(text);
}

///////////////////////////////////////////////////////////
// "Connect Bluetooth" submenu rows

QuickConnectItem::QuickConnectItem(BT_devicePaired d, bool &dirty)
    : MenuItem(ListItemType::Button, "", "", nullptr, nullptr), dev(d), dirty(dirty)
{
    baseName = dev.remote_name[0] ? std::string(dev.remote_name) : std::string(dev.remote_addr);
    refreshName();
}

void QuickConnectItem::refreshName()
{
    name = baseName + (dev.is_connected ? " (connected)" : "");
    desc = std::string(dev.remote_addr) + " | A: " + (dev.is_connected ? "disconnect" : "connect");
}

InputReactionHint QuickConnectItem::handleInput(int &dirtyFlag)
{
    if (PAD_justPressed(BTN_A))
    {
        if (dev.is_connected)
        {
            BT_disconnect(dev.remote_addr);
        }
        else
        {
            ScopedOverlay overlay("Connecting...");
            BT_connect(dev.remote_addr);
        }
        // re-read our own state so the row updates immediately without
        // leaving the submenu (updater() won't rebuild while it's open)
        std::vector<BT_devicePaired> kl(SCAN_MAX_RESULTS);
        int known = BT_pairedDevices(kl.data(), SCAN_MAX_RESULTS);
        for (int i = 0; i < known; i++)
        {
            if (std::string(kl[i].remote_addr) == std::string(dev.remote_addr))
            {
                dev = kl[i];
                break;
            }
        }
        refreshName();
        dirty = true;
        dirtyFlag = 1;
        return NoOp;
    }
    else if (PAD_justPressed(BTN_X))
    {
        BT_unpair(dev.remote_addr);
        dirty = true;
        dirtyFlag = 1;
        // close the submenu; the main list rebuilds and refreshes the count
        return Exit;
    }
    return MenuItem::handleInput(dirtyFlag);
}
