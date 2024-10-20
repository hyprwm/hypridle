#include "Hypridle.hpp"
#include "../helpers/Log.hpp"
#include "../config/ConfigManager.hpp"
#include "signal.h"
#include <sys/wait.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <thread>
#include <mutex>

CHypridle::CHypridle() {
    m_sWaylandState.display = wl_display_connect(nullptr);
    if (!m_sWaylandState.display) {
        Debug::log(CRIT, "Couldn't connect to a wayland compositor");
        exit(1);
    }
}

void handleGlobal(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    g_pHypridle->onGlobal(data, registry, name, interface, version);
}

void handleGlobalRemove(void* data, struct wl_registry* registry, uint32_t name) {
    g_pHypridle->onGlobalRemoved(data, registry, name);
}

inline const wl_registry_listener registryListener = {
    .global        = handleGlobal,
    .global_remove = handleGlobalRemove,
};

void handleIdled(void* data, ext_idle_notification_v1* ext_idle_notification_v1) {
    g_pHypridle->onIdled((CHypridle::SIdleListener*)data);
}

void handleResumed(void* data, ext_idle_notification_v1* ext_idle_notification_v1) {
    g_pHypridle->onResumed((CHypridle::SIdleListener*)data);
}

inline const ext_idle_notification_v1_listener idleListener = {
    .idled   = handleIdled,
    .resumed = handleResumed,
};

void CHypridle::run() {
    m_sWaylandState.registry = wl_display_get_registry(m_sWaylandState.display);

    wl_registry_add_listener(m_sWaylandState.registry, &registryListener, nullptr);

    wl_display_roundtrip(m_sWaylandState.display);

    if (!m_sWaylandIdleState.notifier) {
        Debug::log(CRIT, "Couldn't bind to ext-idle-notifier-v1, does your compositor support it?");
        exit(1);
    }

    const auto RULES = g_pConfigManager->getRules();
    m_sWaylandIdleState.listeners.resize(RULES.size());

    Debug::log(LOG, "found {} rules", RULES.size());

    for (size_t i = 0; i < RULES.size(); ++i) {
        auto&       l  = m_sWaylandIdleState.listeners[i];
        const auto& r  = RULES[i];
        l.notification = ext_idle_notifier_v1_get_idle_notification(m_sWaylandIdleState.notifier, r.timeout * 1000 /* ms */, m_sWaylandState.seat);
        l.onRestore    = r.onResume;
        l.onTimeout    = r.onTimeout;

        ext_idle_notification_v1_add_listener(l.notification, &idleListener, &l);
    }

    wl_display_roundtrip(m_sWaylandState.display);

    Debug::log(LOG, "wayland done, registering dbus");

    try {
        m_sDBUSState.connection = sdbus::createSystemBusConnection();
    } catch (std::exception& e) {
        Debug::log(CRIT, "Couldn't create the dbus connection ({})", e.what());
        exit(1);
    }

    setupDBUS();
    enterEventLoop();
}

void CHypridle::enterEventLoop() {

    nfds_t pollfdsCount = m_sDBUSState.screenSaverServiceConnection ? 3 : 2;

    pollfd pollfds[] = {
        {
            .fd     = m_sDBUSState.connection->getEventLoopPollData().fd,
            .events = POLLIN,
        },
        {
            .fd     = wl_display_get_fd(m_sWaylandState.display),
            .events = POLLIN,
        },
        {
            .fd     = m_sDBUSState.screenSaverServiceConnection ? m_sDBUSState.screenSaverServiceConnection->getEventLoopPollData().fd : 0,
            .events = POLLIN,
        },
    };

    std::thread pollThr([this, &pollfds, &pollfdsCount]() {
        while (1) {
            int ret = poll(pollfds, pollfdsCount, 5000 /* 5 seconds, reasonable. It's because we might need to terminate */);
            if (ret < 0) {
                Debug::log(CRIT, "[core] Polling fds failed with {}", errno);
                m_bTerminate = true;
                exit(1);
            }

            for (size_t i = 0; i < pollfdsCount; ++i) {
                if (pollfds[i].revents & POLLHUP) {
                    Debug::log(CRIT, "[core] Disconnected from pollfd id {}", i);
                    m_bTerminate = true;
                    exit(1);
                }
            }

            if (m_bTerminate)
                break;

            if (ret != 0) {
                Debug::log(TRACE, "[core] got poll event");
                std::lock_guard<std::mutex> lg(m_sEventLoopInternals.loopRequestMutex);
                m_sEventLoopInternals.shouldProcess = true;
                m_sEventLoopInternals.loopSignal.notify_all();
            }
        }
    });

    while (1) { // dbus events
        // wait for being awakened
        m_sEventLoopInternals.loopRequestMutex.unlock(); // unlock, we are ready to take events

        std::unique_lock lk(m_sEventLoopInternals.loopMutex);
        if (m_sEventLoopInternals.shouldProcess == false) // avoid a lock if a thread managed to request something already since we .unlock()ed
            m_sEventLoopInternals.loopSignal.wait(lk, [this] { return m_sEventLoopInternals.shouldProcess == true; }); // wait for events

        m_sEventLoopInternals.loopRequestMutex.lock(); // lock incoming events

        if (m_bTerminate)
            break;

        m_sEventLoopInternals.shouldProcess = false;

        std::lock_guard<std::mutex> lg(m_sEventLoopInternals.eventLock);

        if (pollfds[0].revents & POLLIN /* dbus */) {
            Debug::log(TRACE, "got dbus event");
            while (m_sDBUSState.connection->processPendingRequest()) {
                ;
            }
        }

        if (pollfds[1].revents & POLLIN /* wl */) {
            Debug::log(TRACE, "got wl event");
            wl_display_flush(m_sWaylandState.display);
            if (wl_display_prepare_read(m_sWaylandState.display) == 0) {
                wl_display_read_events(m_sWaylandState.display);
                wl_display_dispatch_pending(m_sWaylandState.display);
            } else {
                wl_display_dispatch(m_sWaylandState.display);
            }
        }

        if (pollfdsCount > 2 && pollfds[2].revents & POLLIN /* dbus2 */) {
            Debug::log(TRACE, "got dbus event");
            while (m_sDBUSState.screenSaverServiceConnection->processPendingRequest()) {
                ;
            }
        }

        // finalize wayland dispatching. Dispatch pending on the queue
        int ret = 0;
        do {
            ret = wl_display_dispatch_pending(m_sWaylandState.display);
            wl_display_flush(m_sWaylandState.display);
        } while (ret > 0);
    }

    Debug::log(ERR, "[core] Terminated");
}

void CHypridle::onGlobal(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    const std::string IFACE = interface;
    Debug::log(LOG, "  | got iface: {} v{}", IFACE, version);

    if (IFACE == ext_idle_notifier_v1_interface.name) {
        m_sWaylandIdleState.notifier = (ext_idle_notifier_v1*)wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, version);
        Debug::log(LOG, "   > Bound to {} v{}", IFACE, version);
    } else if (IFACE == wl_seat_interface.name) {
        if (m_sWaylandState.seat) {
            Debug::log(WARN, "Hypridle does not support multi-seat configurations. Only binding to the first seat.");
            return;
        }

        m_sWaylandState.seat = (wl_seat*)wl_registry_bind(registry, name, &wl_seat_interface, version);
        Debug::log(LOG, "   > Bound to {} v{}", IFACE, version);
    }
}

void CHypridle::onGlobalRemoved(void* data, struct wl_registry* registry, uint32_t name) {
    ;
}

static void spawn(const std::string& args) {
    Debug::log(LOG, "Executing {}", args);

    int socket[2];
    if (pipe(socket) != 0) {
        Debug::log(LOG, "Unable to create pipe for fork");
    }

    pid_t child, grandchild;
    child = fork();
    if (child < 0) {
        close(socket[0]);
        close(socket[1]);
        Debug::log(LOG, "Fail to create the first fork");
        return;
    }
    if (child == 0) {
        // run in child

        sigset_t set;
        sigemptyset(&set);
        sigprocmask(SIG_SETMASK, &set, NULL);

        grandchild = fork();
        if (grandchild == 0) {
            // run in grandchild
            close(socket[0]);
            close(socket[1]);
            execl("/bin/sh", "/bin/sh", "-c", args.c_str(), nullptr);
            // exit grandchild
            _exit(0);
        }
        close(socket[0]);
        write(socket[1], &grandchild, sizeof(grandchild));
        close(socket[1]);
        // exit child
        _exit(0);
    }
    // run in parent
    close(socket[1]);
    read(socket[0], &grandchild, sizeof(grandchild));
    close(socket[0]);
    // clear child and leave grandchild to init
    waitpid(child, NULL, 0);
    if (grandchild < 0) {
        Debug::log(LOG, "Failed to create the second fork");
        return;
    }

    Debug::log(LOG, "Process Created with pid {}", grandchild);
}

void CHypridle::onIdled(SIdleListener* pListener) {
    Debug::log(LOG, "Idled: rule {:x}", (uintptr_t)pListener);
    isIdled = true;
    if (g_pHypridle->m_iInhibitLocks > 0) {
        Debug::log(LOG, "Ignoring from onIdled(), inhibit locks: {}", g_pHypridle->m_iInhibitLocks);
        return;
    }

    if (pListener->onTimeout.empty()) {
        Debug::log(LOG, "Ignoring, onTimeout is empty.");
        return;
    }

    Debug::log(LOG, "Running {}", pListener->onTimeout);
    spawn(pListener->onTimeout);
}

void CHypridle::onResumed(SIdleListener* pListener) {
    Debug::log(LOG, "Resumed: rule {:x}", (uintptr_t)pListener);
    isIdled = false;
    if (g_pHypridle->m_iInhibitLocks > 0) {
        Debug::log(LOG, "Ignoring from onResumed(), inhibit locks: {}", g_pHypridle->m_iInhibitLocks);
        return;
    }

    if (pListener->onRestore.empty()) {
        Debug::log(LOG, "Ignoring, onRestore is empty.");
        return;
    }

    Debug::log(LOG, "Running {}", pListener->onRestore);
    spawn(pListener->onRestore);
}

void CHypridle::onInhibit(bool lock) {
    m_iInhibitLocks += lock ? 1 : -1;

    if (m_iInhibitLocks < 0) {
        Debug::log(WARN, "BUG THIS: inhibit locks < 0: {}", m_iInhibitLocks);
        m_iInhibitLocks = 0;
    }

    if (m_iInhibitLocks == 0 && isIdled) {
        const auto RULES = g_pConfigManager->getRules();

        for (size_t i = 0; i < RULES.size(); ++i) {
            auto&       l = m_sWaylandIdleState.listeners[i];
            const auto& r = RULES[i];

            ext_idle_notification_v1_destroy(l.notification);

            l.notification = ext_idle_notifier_v1_get_idle_notification(m_sWaylandIdleState.notifier, r.timeout * 1000 /* ms */, m_sWaylandState.seat);

            ext_idle_notification_v1_add_listener(l.notification, &idleListener, &l);
        }
    }

    Debug::log(LOG, "Inhibit locks: {}", m_iInhibitLocks);
}

CHypridle::SDbusInhibitCookie CHypridle::getDbusInhibitCookie(uint32_t cookie) {
    for (auto& c : m_sDBUSState.inhibitCookies) {
        if (c.cookie == cookie)
            return c;
    }

    return {};
}

void CHypridle::registerDbusInhibitCookie(CHypridle::SDbusInhibitCookie& cookie) {
    m_sDBUSState.inhibitCookies.push_back(cookie);
}

bool CHypridle::unregisterDbusInhibitCookie(const CHypridle::SDbusInhibitCookie& cookie) {
    const auto IT = std::find_if(m_sDBUSState.inhibitCookies.begin(), m_sDBUSState.inhibitCookies.end(),
                                 [&cookie](const CHypridle::SDbusInhibitCookie& item) { return item.cookie == cookie.cookie; });

    if (IT == m_sDBUSState.inhibitCookies.end())
        return false;

    m_sDBUSState.inhibitCookies.erase(IT);
    return true;
}

bool CHypridle::unregisterDbusInhibitCookies(const std::string& ownerID) {
    const auto IT = std::remove_if(m_sDBUSState.inhibitCookies.begin(), m_sDBUSState.inhibitCookies.end(),
                                   [&ownerID](const CHypridle::SDbusInhibitCookie& item) { return item.ownerID == ownerID; });

    if (IT == m_sDBUSState.inhibitCookies.end())
        return false;

    m_sDBUSState.inhibitCookies.erase(IT, m_sDBUSState.inhibitCookies.end());
    return true;
}

void handleDbusLogin(sdbus::Message& msg) {
    // lock & unlock
    static auto* const PLOCKCMD   = (Hyprlang::STRING const*)g_pConfigManager->getValuePtr("general:lock_cmd");
    static auto* const PUNLOCKCMD = (Hyprlang::STRING const*)g_pConfigManager->getValuePtr("general:unlock_cmd");

    Debug::log(LOG, "Got dbus .Session");

    const auto MEMBER = msg.getMemberName();
    if (MEMBER == "Lock") {
        Debug::log(LOG, "Got Lock from dbus");

        if (!std::string{*PLOCKCMD}.empty()) {
            Debug::log(LOG, "Locking with {}", *PLOCKCMD);
            spawn(*PLOCKCMD);
        }
    } else if (MEMBER == "Unlock") {
        Debug::log(LOG, "Got Unlock from dbus");

        if (!std::string{*PUNLOCKCMD}.empty()) {
            Debug::log(LOG, "Locking with {}", *PUNLOCKCMD);
            spawn(*PUNLOCKCMD);
        }
    }
}

void handleDbusSleep(sdbus::Message& msg) {
    const auto MEMBER = msg.getMemberName();

    if (MEMBER != "PrepareForSleep")
        return;

    bool toSleep = true;
    msg >> toSleep;

    static auto* const PSLEEPCMD      = (Hyprlang::STRING const*)g_pConfigManager->getValuePtr("general:before_sleep_cmd");
    static auto* const PAFTERSLEEPCMD = (Hyprlang::STRING const*)g_pConfigManager->getValuePtr("general:after_sleep_cmd");

    Debug::log(LOG, "Got PrepareForSleep from dbus with sleep {}", toSleep);

    std::string cmd = toSleep ? *PSLEEPCMD : *PAFTERSLEEPCMD;

    if (cmd.empty())
        return;

    Debug::log(LOG, "Running: {}", cmd);
    spawn(cmd);
}

void handleDbusBlockInhibits(const std::string& inhibits) {
    static auto inhibited = false;
    // BlockInhibited is a colon separated list of inhibit types. Wrapping in additional colons allows for easier checking if there are active inhibits we are interested in
    auto inhibits_ = ":" + inhibits + ":";
    if (inhibits_.contains(":idle:")) {
        if (!inhibited) {
            inhibited = true;
            Debug::log(LOG, "systemd idle inhibit active");
            g_pHypridle->onInhibit(true);
        }
    } else if (inhibited) {
        inhibited = false;
        Debug::log(LOG, "systemd idle inhibit inactive");
        g_pHypridle->onInhibit(false);
    }
}

void handleDbusBlockInhibitsPropertyChanged(sdbus::Message& msg) {
    std::string                           interface;
    std::map<std::string, sdbus::Variant> changedProperties;
    msg >> interface >> changedProperties;
    if (changedProperties.contains("BlockInhibited")) {
        handleDbusBlockInhibits(changedProperties["BlockInhibited"].get<std::string>());
    }
}

void handleDbusScreensaver(sdbus::MethodCall call, bool inhibit) {
    std::string app = "?", reason = "?";
    std::string ownerID = call.getSender();

    if (inhibit) {
        call >> app;
        call >> reason;
    } else {
        uint32_t cookie = 0;
        call >> cookie;
        Debug::log(TRACE, "Read uninhibit cookie: {}", cookie);
        const auto COOKIE = g_pHypridle->getDbusInhibitCookie(cookie);
        if (COOKIE.cookie == 0) {
            Debug::log(WARN, "No cookie in uninhibit");
        } else {
            app     = COOKIE.app;
            reason  = COOKIE.reason;
            ownerID = COOKIE.ownerID;

            if (!g_pHypridle->unregisterDbusInhibitCookie(COOKIE)) {
                Debug::log(WARN, "BUG THIS: attempted to unregister unknown cookie");
            };
        }
    }

    Debug::log(LOG, "ScreenSaver inhibit: {} dbus message from {} (owner: {}) with content {}", inhibit, app, ownerID, reason);

    if (inhibit)
        g_pHypridle->onInhibit(true);
    else
        g_pHypridle->onInhibit(false);

    static int cookieID = 1337;

    if (inhibit) {
        auto cookie = CHypridle::SDbusInhibitCookie{uint32_t{cookieID}, app, reason, ownerID};

        auto reply = call.createReply();
        reply << uint32_t{cookieID++};
        reply.send();

        Debug::log(LOG, "Cookie {} sent", cookieID - 1);

        g_pHypridle->registerDbusInhibitCookie(cookie);
    } else {
        auto reply = call.createReply();
        reply.send();
        Debug::log(TRACE, "Uninhibit response sent");
    }
}

void handleDbusNameOwnerChanged(sdbus::Message& msg) {
    std::string name, oldOwner, newOwner;
    msg >> name >> oldOwner >> newOwner;

    if (!newOwner.empty())
        return;

    if (g_pHypridle->unregisterDbusInhibitCookies(oldOwner)) {
        Debug::log(LOG, "App with owner {} disconnected", oldOwner);
        g_pHypridle->onInhibit(false);
    }
}

void CHypridle::setupDBUS() {
    static auto const IGNORE_DBUS_INHIBIT    = **(Hyprlang::INT* const*)g_pConfigManager->getValuePtr("general:ignore_dbus_inhibit");
    static auto const IGNORE_SYSTEMD_INHIBIT = **(Hyprlang::INT* const*)g_pConfigManager->getValuePtr("general:ignore_systemd_inhibit");

    auto              proxy  = sdbus::createProxy("org.freedesktop.login1", "/org/freedesktop/login1");
    auto              method = proxy->createMethodCall("org.freedesktop.login1.Manager", "GetSession");
    method << "auto";
    sdbus::ObjectPath path;

    try {
        auto reply = proxy->callMethod(method);
        reply >> path;

        m_sDBUSState.connection->addMatch("type='signal',path='" + path + "',interface='org.freedesktop.login1.Session'", handleDbusLogin, sdbus::floating_slot_t{});
        m_sDBUSState.connection->addMatch("type='signal',path='/org/freedesktop/login1',interface='org.freedesktop.login1.Manager'", handleDbusSleep, sdbus::floating_slot_t{});
    } catch (std::exception& e) { Debug::log(WARN, "Couldn't connect to logind service ({})", e.what()); }

    Debug::log(LOG, "Using dbus path {}", path.c_str());

    if (!IGNORE_SYSTEMD_INHIBIT) {
        m_sDBUSState.connection->addMatch("type='signal',path='/org/freedesktop/login1',interface='org.freedesktop.DBus.Properties'", handleDbusBlockInhibitsPropertyChanged,
                                          sdbus::floating_slot_t{});

        try {
            std::string value = proxy->getProperty("BlockInhibited").onInterface("org.freedesktop.login1.Manager");
            handleDbusBlockInhibits(value);
        } catch (std::exception& e) { Debug::log(WARN, "Couldn't retrieve current systemd inhibits ({})", e.what()); }
    }

    if (!IGNORE_DBUS_INHIBIT) {
        // attempt to register as ScreenSaver
        std::string paths[] = {
            "/org/freedesktop/ScreenSaver",
            "/ScreenSaver",
        };

        try {
            m_sDBUSState.screenSaverServiceConnection = sdbus::createSessionBusConnection("org.freedesktop.ScreenSaver");

            for (const std::string& path : paths) {
                try {
                    auto obj = sdbus::createObject(*m_sDBUSState.screenSaverServiceConnection, path);
                    obj->registerMethod("org.freedesktop.ScreenSaver", "Inhibit", "ss", "u", [&](sdbus::MethodCall c) { handleDbusScreensaver(c, true); });
                    obj->registerMethod("org.freedesktop.ScreenSaver", "UnInhibit", "u", "", [&](sdbus::MethodCall c) { handleDbusScreensaver(c, false); });
                    obj->finishRegistration();

                    m_sDBUSState.screenSaverObjects.push_back(std::move(obj));
                } catch (std::exception& e) { Debug::log(ERR, "Failed registering for {}, perhaps taken?\nerr: {}", path, e.what()); }
            }

            m_sDBUSState.screenSaverServiceConnection->addMatch("type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
                                                                handleDbusNameOwnerChanged, sdbus::floating_slot_t{});
        } catch (std::exception& e) { Debug::log(ERR, "Couldn't connect to session dbus\nerr: {}", e.what()); }
    }
}
