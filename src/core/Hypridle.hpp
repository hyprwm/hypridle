#pragma once

#include <memory>
#include <vector>
#include <wayland-client.h>
#include <sdbus-c++/sdbus-c++.h>
#include <condition_variable>

#include "ext-idle-notify-v1-protocol.h"

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
        std::string app, reason;
    };

    void               run();

    void               onGlobal(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version);
    void               onGlobalRemoved(void* data, struct wl_registry* registry, uint32_t name);

    void               onIdled(SIdleListener*);
    void               onResumed(SIdleListener*);

    void               onInhibit(bool lock);

    SDbusInhibitCookie getDbusInhibitCookie(uint32_t cookie);
    void               registerDbusInhibitCookie(SDbusInhibitCookie& cookie);
    void               unregisterDbusInhibitCookie(const SDbusInhibitCookie& cookie);

  private:
    void    setupDBUS();
    void    enterEventLoop();

    bool    m_bTerminate    = false;
    bool    isIdled         = false;
    int64_t m_iInhibitLocks = 0;

    struct {
        wl_display*  display  = nullptr;
        wl_registry* registry = nullptr;
        wl_seat*     seat     = nullptr;
    } m_sWaylandState;

    struct {
        ext_idle_notifier_v1*      notifier = nullptr;

        std::vector<SIdleListener> listeners;
    } m_sWaylandIdleState;

    struct {
        std::unique_ptr<sdbus::IConnection> connection;
        std::unique_ptr<sdbus::IConnection> screenSaverServiceConnection;
        std::unique_ptr<sdbus::IObject>     screenSaverObject;
        std::vector<SDbusInhibitCookie>     inhibitCookies;
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
