#pragma once

#include <memory>
#include <vector>
#include <wayland-client.h>
#include <sdbus-c++/sdbus-c++.h>
#include <condition_variable>

#include "ext-idle-notify-v1-protocol.h"
#include "hyprland-lock-notify-v1-protocol.h"

class CHypridle {
  public:
    CHypridle();

    struct SIdleListener {
        ext_idle_notification_v1* notification = nullptr;
        std::string               onTimeout    = "";
        std::string               onRestore    = "";
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

    void               onLocked();
    void               onUnlocked();

    void               onInhibit(bool lock);

    SDbusInhibitCookie getDbusInhibitCookie(uint32_t cookie);
    void               registerDbusInhibitCookie(SDbusInhibitCookie& cookie);
    bool               unregisterDbusInhibitCookie(const SDbusInhibitCookie& cookie);
    bool               unregisterDbusInhibitCookies(const std::string& ownerID);

  private:
    void    setupDBUS();
    void    enterEventLoop();

    bool    m_bTerminate    = false;
    bool    isIdled         = false;
    bool    m_isLocked      = false;
    int64_t m_iInhibitLocks = 0;

    struct {
        wl_display*                    display          = nullptr;
        wl_registry*                   registry         = nullptr;
        wl_seat*                       seat             = nullptr;
        hyprland_lock_notifier_v1*     lockNotifier     = nullptr;
        hyprland_lock_notification_v1* lockNotification = nullptr;
    } m_sWaylandState;

    struct {
        ext_idle_notifier_v1*      notifier = nullptr;

        std::vector<SIdleListener> listeners;
    } m_sWaylandIdleState;

    struct {
        std::unique_ptr<sdbus::IConnection>          connection;
        std::unique_ptr<sdbus::IConnection>          screenSaverServiceConnection;
        std::vector<std::unique_ptr<sdbus::IObject>> screenSaverObjects;
        std::vector<SDbusInhibitCookie>              inhibitCookies;
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
