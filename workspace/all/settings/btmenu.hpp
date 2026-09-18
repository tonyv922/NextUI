#pragma once

#include "menu.hpp"
#include <thread>
#include <mutex>
#include <condition_variable>

namespace Bluetooth
{
    class PairingAgent;

    class Menu : public MenuList
    {
        const int &globalQuit;
        int &globalDirty;
        // bt on/off
        MenuItem *toggleItem;
        // diagnostics on/off
        MenuItem *diagItem;
        // max sample rate
        MenuItem *rateItem = nullptr;
        // "Connect Bluetooth" placeholder shown while BT is off
        MenuItem *offEntryItem = nullptr;
        
        std::thread worker;
        bool quit = false;
        bool selectionDirty = false;
        // wakes the updater right after the on/off toggle so the
        // "Connect Bluetooth" entry appears without waiting a poll cycle
        std::mutex wakeLock;
        std::condition_variable wakeCv;
        unsigned wakeGen = 0;
        
        PairingAgent* pairingAgent = nullptr;
    public:
        Menu(const int &globalQuit, int &globalDirty);
        ~Menu();

        InputReactionHint handleInput(int &dirty, int &quit) override;

    private:
        std::any getBtToggleState() const;
        void setBtToggleState(const std::any &on);
        void resetBtToggleState();

        std::any getBtDiagnosticsState() const;
        void setBtDiagnosticsState(const std::any &on);
        void resetBtDiagnosticsState();

        std::any getSamplerateMaximum() const;
        void setSamplerateMaximum(const std::any &on);
        void resetSamplerateMaximum();

        void updater();
    };

    class PairableItem : public MenuItem
    {
        BT_device dev;

    public:
        PairableItem(BT_device d, MenuList *submenu);

        void drawCustomItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected) const override;
    };

    class PairedItem : public MenuItem
    {
        BT_devicePaired dev;

    public:
        PairedItem(BT_devicePaired d, MenuList *submenu);

        void drawCustomItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected) const override;
    };

    // one row in the "Connect Bluetooth" list: A connects/disconnects directly, X forgets
    class QuickConnectItem : public MenuItem
    {
        BT_devicePaired dev;
        std::string baseName;
        bool &dirty;

        void refreshName();

    public:
        QuickConnectItem(BT_devicePaired d, bool &dirty);

        InputReactionHint handleInput(int &dirtyFlag) override;
    };

    class ConnectKnownItem : public MenuItem
    {
        BT_devicePaired dev;
    public:
        ConnectKnownItem(BT_devicePaired n, bool& dirty);
    };

    class DisconnectKnownItem : public MenuItem
    {
        BT_devicePaired dev;
    public:
        DisconnectKnownItem(BT_devicePaired n, bool& dirty);
    };

    class PairNewItem : public MenuItem
    {
        BT_device dev;

    public:
        PairNewItem(BT_device n, bool& dirty);
    };

    class UnpairItem : public MenuItem
    {
        BT_devicePaired dev;

    public:
        UnpairItem(BT_devicePaired n, bool& dirty);
    };
}
