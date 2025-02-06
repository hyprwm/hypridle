#pragma once

#include <memory>
#include <vector>
#include <sdbus-c++/sdbus-c++.h>
#include <hyprutils/os/FileDescriptor.hpp>
#include <condition_variable>

#include "wayland.hpp"
#include "ext-idle-notify-v1.hpp"
#include "hyprland-lock-notify-v1.hpp"

#include "../defines.hpp"

class CHypridle {
  public:
    CHypridle();

    struct SIdleListener {
        SP<CCExtIdleNotificationV1> notification = nullptr;
        std::string                 onTimeout    = "";
        std::string                 onRestore    = "";
    };

    struct SDbusInhibitCookie {
        uint32_t    cookie = 0;
        std::string app, reason, ownerID;
    };

    void               run();

    void               onGlobal(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version);
    void               onGlobalRemoved(void* data, struct wl_registry* registry, uint32_t name);

    void               onIdled(SIdleListener*);
    void               onResumed(SIdleListener*);

    void               onInhibit(bool lock);

    void               onLocked();
    void               onUnlocked();

    SDbusInhibitCookie getDbusInhibitCookie(uint32_t cookie);
    void               registerDbusInhibitCookie(SDbusInhibitCookie& cookie);
    bool               unregisterDbusInhibitCookie(const SDbusInhibitCookie& cookie);
    bool               unregisterDbusInhibitCookies(const std::string& ownerID);

    void               handleInhibitOnDbusSleep(bool toSleep);
    void               inhibitSleep();
    void               uninhibitSleep();

    void               closeInhibitFd();

  private:
    void    setupDBUS();
    void    enterEventLoop();

    bool    m_bTerminate    = false;
    bool    isIdled         = false;
    bool    m_isLocked      = false;
    int64_t m_iInhibitLocks = 0;

    enum {
        SLEEP_INHIBIT_NONE,
        SLEEP_INHIBIT_NORMAL,
        SLEEP_INHIBIT_LOCK_NOTIFY,
    } m_inhibitSleepBehavior;

    struct {
        wl_display*                      display          = nullptr;
        SP<CCWlRegistry>                 registry         = nullptr;
        SP<CCWlSeat>                     seat             = nullptr;
        SP<CCHyprlandLockNotifierV1>     lockNotifier     = nullptr;
        SP<CCHyprlandLockNotificationV1> lockNotification = nullptr;
    } m_sWaylandState;

    struct {
        SP<CCExtIdleNotifierV1>    notifier = nullptr;

        std::vector<SIdleListener> listeners;
    } m_sWaylandIdleState;

    struct {
        std::unique_ptr<sdbus::IConnection>          connection;
        std::unique_ptr<sdbus::IConnection>          screenSaverServiceConnection;
        std::unique_ptr<sdbus::IProxy>               login;
        std::vector<std::unique_ptr<sdbus::IObject>> screenSaverObjects;
        std::vector<SDbusInhibitCookie>              inhibitCookies;
        Hyprutils::OS::CFileDescriptor               sleepInhibitFd;
    } m_sDBUSState;

    struct {
        std::condition_variable loopSignal;
        std::mutex              loopMutex;
        std::atomic<bool>       shouldProcess = false;
        std::mutex              loopRequestMutex;
        std::mutex              eventLock;
    } m_sEventLoopInternals;
};

inline std::unique_ptr<CHypridle> g_pHypridle;
