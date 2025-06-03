
#include "Hypridle.hpp"
#include "../helpers/Log.hpp"
#include "../config/ConfigManager.hpp"
#include "csignal"
#include <sys/wait.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <thread>
#include <mutex>
#include <hyprutils/os/Process.hpp>

CHypridle::CHypridle() {
    m_sWaylandState.display = wl_display_connect(nullptr);
    if (!m_sWaylandState.display) {
        Debug::log(CRIT, "Couldn't connect to a wayland compositor");
        exit(1);
    }
}

void CHypridle::run() {
    m_sWaylandState.registry = makeShared<CCWlRegistry>((wl_proxy*)wl_display_get_registry(m_sWaylandState.display));
    m_sWaylandState.registry->setGlobal([this](CCWlRegistry* r, uint32_t name, const char* interface, uint32_t version) {
        const std::string IFACE = interface;
        Debug::log(LOG, "  | got iface: {} v{}", IFACE, version);

        if (IFACE == ext_idle_notifier_v1_interface.name) {
            m_sWaylandIdleState.notifier =
                makeShared<CCExtIdleNotifierV1>((wl_proxy*)wl_registry_bind((wl_registry*)r->resource(), name, &ext_idle_notifier_v1_interface, version));
            Debug::log(LOG, "   > Bound to {} v{}", IFACE, version);
        } else if (IFACE == hyprland_lock_notifier_v1_interface.name) {
            m_sWaylandState.lockNotifier =
                makeShared<CCHyprlandLockNotifierV1>((wl_proxy*)wl_registry_bind((wl_registry*)r->resource(), name, &hyprland_lock_notifier_v1_interface, version));
            Debug::log(LOG, "   > Bound to {} v{}", IFACE, version);
        } else if (IFACE == wl_seat_interface.name) {
            if (m_sWaylandState.seat) {
                Debug::log(WARN, "Hypridle does not support multi-seat configurations. Only binding to the first seat.");
                return;
            }

            m_sWaylandState.seat = makeShared<CCWlSeat>((wl_proxy*)wl_registry_bind((wl_registry*)r->resource(), name, &wl_seat_interface, version));
            Debug::log(LOG, "   > Bound to {} v{}", IFACE, version);
        }
    });

    m_sWaylandState.registry->setGlobalRemove([](CCWlRegistry* r, uint32_t name) { Debug::log(LOG, "  | removed iface {}", name); });

    wl_display_roundtrip(m_sWaylandState.display);

    if (!m_sWaylandIdleState.notifier) {
        Debug::log(CRIT, "Couldn't bind to ext-idle-notifier-v1, does your compositor support it?");
        exit(1);
    }

    static const auto IGNOREWAYLANDINHIBIT = g_pConfigManager->getValue<Hyprlang::INT>("general:ignore_wayland_inhibit");

    const auto        RULES = g_pConfigManager->getRules();
    m_sWaylandIdleState.listeners.resize(RULES.size());

    Debug::log(LOG, "found {} rules", RULES.size());

    for (size_t i = 0; i < RULES.size(); ++i) {
        auto&       l   = m_sWaylandIdleState.listeners[i];
        const auto& r   = RULES[i];
        l.onRestore     = r.onResume;
        l.onTimeout     = r.onTimeout;
        l.ignoreInhibit = r.ignoreInhibit;

        if (*IGNOREWAYLANDINHIBIT || r.ignoreInhibit)
            l.notification =
                makeShared<CCExtIdleNotificationV1>(m_sWaylandIdleState.notifier->sendGetInputIdleNotification(r.timeout * 1000 /* ms */, m_sWaylandState.seat->resource()));
        else
            l.notification =
                makeShared<CCExtIdleNotificationV1>(m_sWaylandIdleState.notifier->sendGetIdleNotification(r.timeout * 1000 /* ms */, m_sWaylandState.seat->resource()));

        l.notification->setData(&m_sWaylandIdleState.listeners[i]);

        l.notification->setIdled([this](CCExtIdleNotificationV1* n) { onIdled((CHypridle::SIdleListener*)n->data()); });
        l.notification->setResumed([this](CCExtIdleNotificationV1* n) { onResumed((CHypridle::SIdleListener*)n->data()); });
    }

    wl_display_roundtrip(m_sWaylandState.display);

    if (m_sWaylandState.lockNotifier) {
        m_sWaylandState.lockNotification = makeShared<CCHyprlandLockNotificationV1>(m_sWaylandState.lockNotifier->sendGetLockNotification());
        m_sWaylandState.lockNotification->setLocked([this](CCHyprlandLockNotificationV1* n) { onLocked(); });
        m_sWaylandState.lockNotification->setUnlocked([this](CCHyprlandLockNotificationV1* n) { onUnlocked(); });
    }

    Debug::log(LOG, "wayland done, registering dbus");

    try {
        m_sDBUSState.connection = sdbus::createSystemBusConnection();
    } catch (std::exception& e) {
        Debug::log(CRIT, "Couldn't create the dbus connection ({})", e.what());
        exit(1);
    }

    if (!m_sWaylandState.lockNotifier)
        Debug::log(WARN,
                   "Compositor is missing hyprland-lock-notify-v1!\n"
                   "general:inhibit_sleep=3, general:on_lock_cmd and general:on_unlock_cmd will not work.");

    static const auto INHIBIT  = g_pConfigManager->getValue<Hyprlang::INT>("general:inhibit_sleep");
    static const auto SLEEPCMD = g_pConfigManager->getValue<Hyprlang::STRING>("general:before_sleep_cmd");
    static const auto LOCKCMD  = g_pConfigManager->getValue<Hyprlang::STRING>("general:lock_cmd");

    switch (*INHIBIT) {
        case 0: // disabled
            m_inhibitSleepBehavior = SLEEP_INHIBIT_NONE;
            break;
        case 1: // enabled
            m_inhibitSleepBehavior = SLEEP_INHIBIT_NORMAL;
            break;
        case 2: { // auto (enable, but wait until locked if before_sleep_cmd contains hyprlock, or loginctl lock-session and lock_cmd contains hyprlock.)
            if (m_sWaylandState.lockNotifier && std::string{*SLEEPCMD}.contains("hyprlock"))
                m_inhibitSleepBehavior = SLEEP_INHIBIT_LOCK_NOTIFY;
            else if (m_sWaylandState.lockNotifier && std::string{*LOCKCMD}.contains("hyprlock") && std::string{*SLEEPCMD}.contains("lock-session"))
                m_inhibitSleepBehavior = SLEEP_INHIBIT_LOCK_NOTIFY;
            else
                m_inhibitSleepBehavior = SLEEP_INHIBIT_NORMAL;
        } break;
        case 3: // wait until locked
            if (m_sWaylandState.lockNotifier)
                m_inhibitSleepBehavior = SLEEP_INHIBIT_LOCK_NOTIFY;
            break;
        default: Debug::log(ERR, "Invalid inhibit_sleep value: {}", *INHIBIT); break;
    }

    switch (m_inhibitSleepBehavior) {
        case SLEEP_INHIBIT_NONE: Debug::log(LOG, "Sleep inhibition disabled"); break;
        case SLEEP_INHIBIT_NORMAL: Debug::log(LOG, "Sleep inhibition enabled"); break;
        case SLEEP_INHIBIT_LOCK_NOTIFY: Debug::log(LOG, "Sleep inhibition enabled - inhibiting until the wayland session gets locked"); break;
    }

    setupDBUS();
    if (m_inhibitSleepBehavior != SLEEP_INHIBIT_NONE)
        inhibitSleep();
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
        if (!m_sEventLoopInternals.shouldProcess) // avoid a lock if a thread managed to request something already since we .unlock()ed
            m_sEventLoopInternals.loopSignal.wait(lk, [this] { return m_sEventLoopInternals.shouldProcess == true; }); // wait for events

        m_sEventLoopInternals.loopRequestMutex.lock(); // lock incoming events

        if (m_bTerminate)
            break;

        m_sEventLoopInternals.shouldProcess = false;

        std::lock_guard<std::mutex> lg(m_sEventLoopInternals.eventLock);

        if (pollfds[0].revents & POLLIN /* dbus */) {
            Debug::log(TRACE, "got dbus event");
            while (m_sDBUSState.connection->processPendingEvent()) {
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
            while (m_sDBUSState.screenSaverServiceConnection->processPendingEvent()) {
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

static void spawn(const std::string& args) {
    Debug::log(LOG, "Executing {}", args);

    Hyprutils::OS::CProcess proc("/bin/sh", {"-c", args});
    if (!proc.runAsync()) {
        Debug::log(ERR, "Failed run \"{}\"", args);
        return;
    }

    Debug::log(LOG, "Process Created with pid {}", proc.pid());
}

void CHypridle::onIdled(SIdleListener* pListener) {
    Debug::log(LOG, "Idled: rule {:x}", (uintptr_t)pListener);
    isIdled = true;
    if (g_pHypridle->m_iInhibitLocks > 0 && !pListener->ignoreInhibit) {
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
    if (g_pHypridle->m_iInhibitLocks > 0 && !pListener->ignoreInhibit) {
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
        static const auto IGNOREWAYLANDINHIBIT = g_pConfigManager->getValue<Hyprlang::INT>("general:ignore_wayland_inhibit");

        const auto        RULES = g_pConfigManager->getRules();
        for (size_t i = 0; i < RULES.size(); ++i) {
            auto&       l = m_sWaylandIdleState.listeners[i];
            const auto& r = RULES[i];

            l.notification->sendDestroy();

            if (*IGNOREWAYLANDINHIBIT || r.ignoreInhibit)
                l.notification =
                    makeShared<CCExtIdleNotificationV1>(m_sWaylandIdleState.notifier->sendGetInputIdleNotification(r.timeout * 1000 /* ms */, m_sWaylandState.seat->resource()));
            else
                l.notification =
                    makeShared<CCExtIdleNotificationV1>(m_sWaylandIdleState.notifier->sendGetIdleNotification(r.timeout * 1000 /* ms */, m_sWaylandState.seat->resource()));

            l.notification->setData(&m_sWaylandIdleState.listeners[i]);

            l.notification->setIdled([this](CCExtIdleNotificationV1* n) { onIdled((CHypridle::SIdleListener*)n->data()); });
            l.notification->setResumed([this](CCExtIdleNotificationV1* n) { onResumed((CHypridle::SIdleListener*)n->data()); });
        }
    }

    Debug::log(LOG, "Inhibit locks: {}", m_iInhibitLocks);
}

void CHypridle::onLocked() {
    Debug::log(LOG, "Wayland session got locked");
    m_isLocked = true;

    static const auto LOCKCMD = g_pConfigManager->getValue<Hyprlang::STRING>("general:on_lock_cmd");
    if (!std::string{*LOCKCMD}.empty())
        spawn(*LOCKCMD);

    if (m_inhibitSleepBehavior == SLEEP_INHIBIT_LOCK_NOTIFY)
        uninhibitSleep();
}

void CHypridle::onUnlocked() {
    Debug::log(LOG, "Wayland session got unlocked");
    m_isLocked = false;

    if (m_inhibitSleepBehavior == SLEEP_INHIBIT_LOCK_NOTIFY)
        inhibitSleep();

    static const auto UNLOCKCMD = g_pConfigManager->getValue<Hyprlang::STRING>("general:on_unlock_cmd");
    if (!std::string{*UNLOCKCMD}.empty())
        spawn(*UNLOCKCMD);
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
    const auto IT = std::ranges::find_if(m_sDBUSState.inhibitCookies, [&cookie](const CHypridle::SDbusInhibitCookie& item) { return item.cookie == cookie.cookie; });

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

static void handleDbusLogin(sdbus::Message msg) {
    // lock & unlock
    static const auto LOCKCMD   = g_pConfigManager->getValue<Hyprlang::STRING>("general:lock_cmd");
    static const auto UNLOCKCMD = g_pConfigManager->getValue<Hyprlang::STRING>("general:unlock_cmd");

    Debug::log(LOG, "Got dbus .Session");

    const std::string MEMBER = msg.getMemberName();
    if (MEMBER == "Lock") {
        Debug::log(LOG, "Got Lock from dbus");

        if (!std::string{*LOCKCMD}.empty()) {
            Debug::log(LOG, "Locking with {}", *LOCKCMD);
            spawn(*LOCKCMD);
        }
    } else if (MEMBER == "Unlock") {
        Debug::log(LOG, "Got Unlock from dbus");

        if (!std::string{*UNLOCKCMD}.empty()) {
            Debug::log(LOG, "Unlocking with {}", *UNLOCKCMD);
            spawn(*UNLOCKCMD);
        }
    }
}

static void handleDbusSleep(sdbus::Message msg) {
    const std::string MEMBER = msg.getMemberName();

    if (MEMBER != "PrepareForSleep")
        return;

    bool toSleep = true;
    msg >> toSleep;

    static const auto SLEEPCMD      = g_pConfigManager->getValue<Hyprlang::STRING>("general:before_sleep_cmd");
    static const auto AFTERSLEEPCMD = g_pConfigManager->getValue<Hyprlang::STRING>("general:after_sleep_cmd");

    Debug::log(LOG, "Got PrepareForSleep from dbus with sleep {}", toSleep);

    std::string cmd = toSleep ? *SLEEPCMD : *AFTERSLEEPCMD;

    if (!toSleep)
        g_pHypridle->handleInhibitOnDbusSleep(toSleep);

    if (!cmd.empty())
        spawn(cmd);

    if (toSleep)
        g_pHypridle->handleInhibitOnDbusSleep(toSleep);
}

static void handleDbusBlockInhibits(const std::string& inhibits) {
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

static void handleDbusBlockInhibitsPropertyChanged(sdbus::Message msg) {
    std::string                           interface;
    std::map<std::string, sdbus::Variant> changedProperties;
    msg >> interface >> changedProperties;
    if (changedProperties.contains("BlockInhibited")) {
        handleDbusBlockInhibits(changedProperties["BlockInhibited"].get<std::string>());
    }
}

static uint32_t handleDbusScreensaver(std::string app, std::string reason, uint32_t cookie, bool inhibit, const char* sender) {
    std::string ownerID = sender;

    if (!inhibit) {
        Debug::log(TRACE, "Read uninhibit cookie: {}", cookie);
        const auto COOKIE = g_pHypridle->getDbusInhibitCookie(cookie);
        if (COOKIE.cookie == 0) {
            Debug::log(WARN, "No cookie in uninhibit");
        } else {
            app     = COOKIE.app;
            reason  = COOKIE.reason;
            ownerID = COOKIE.ownerID;

            if (!g_pHypridle->unregisterDbusInhibitCookie(COOKIE))
                Debug::log(WARN, "BUG THIS: attempted to unregister unknown cookie");
        }
    }

    Debug::log(LOG, "ScreenSaver inhibit: {} dbus message from {} (owner: {}) with content {}", inhibit, app, ownerID, reason);

    if (inhibit)
        g_pHypridle->onInhibit(true);
    else
        g_pHypridle->onInhibit(false);

    static uint32_t cookieID = 1337;

    if (inhibit) {
        auto cookie = CHypridle::SDbusInhibitCookie{.cookie = cookieID, .app = app, .reason = reason, .ownerID = ownerID};

        Debug::log(LOG, "Cookie {} sent", cookieID);

        g_pHypridle->registerDbusInhibitCookie(cookie);

        return cookieID++;
    }

    return 0;
}

static void handleDbusNameOwnerChanged(sdbus::Message msg) {
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
    static const auto IGNOREDBUSINHIBIT    = g_pConfigManager->getValue<Hyprlang::INT>("general:ignore_dbus_inhibit");
    static const auto IGNORESYSTEMDINHIBIT = g_pConfigManager->getValue<Hyprlang::INT>("general:ignore_systemd_inhibit");

    auto              systemConnection = sdbus::createSystemBusConnection();
    auto              proxy            = sdbus::createProxy(*systemConnection, sdbus::ServiceName{"org.freedesktop.login1"}, sdbus::ObjectPath{"/org/freedesktop/login1"});
    sdbus::ObjectPath path;

    try {
        proxy->callMethod("GetSession").onInterface("org.freedesktop.login1.Manager").withArguments(std::string{"auto"}).storeResultsTo(path);

        m_sDBUSState.connection->addMatch("type='signal',path='" + path + "',interface='org.freedesktop.login1.Session'", ::handleDbusLogin);
        m_sDBUSState.connection->addMatch("type='signal',path='/org/freedesktop/login1',interface='org.freedesktop.login1.Manager'", ::handleDbusSleep);
        m_sDBUSState.login = sdbus::createProxy(*m_sDBUSState.connection, sdbus::ServiceName{"org.freedesktop.login1"}, sdbus::ObjectPath{"/org/freedesktop/login1"});
    } catch (std::exception& e) { Debug::log(WARN, "Couldn't connect to logind service ({})", e.what()); }

    Debug::log(LOG, "Using dbus path {}", path.c_str());

    if (!*IGNORESYSTEMDINHIBIT) {
        m_sDBUSState.connection->addMatch("type='signal',path='/org/freedesktop/login1',interface='org.freedesktop.DBus.Properties'", ::handleDbusBlockInhibitsPropertyChanged);

        try {
            std::string value = (proxy->getProperty("BlockInhibited").onInterface("org.freedesktop.login1.Manager")).get<std::string>();
            handleDbusBlockInhibits(value);
        } catch (std::exception& e) { Debug::log(WARN, "Couldn't retrieve current systemd inhibits ({})", e.what()); }
    }

    if (!*IGNOREDBUSINHIBIT) {
        // attempt to register as ScreenSaver
        std::string paths[] = {
            "/org/freedesktop/ScreenSaver",
            "/ScreenSaver",
        };

        try {
            m_sDBUSState.screenSaverServiceConnection = sdbus::createSessionBusConnection(sdbus::ServiceName{"org.freedesktop.ScreenSaver"});

            for (const std::string& path : paths) {
                try {
                    auto obj = sdbus::createObject(*m_sDBUSState.screenSaverServiceConnection, sdbus::ObjectPath{path});

                    obj->addVTable(sdbus::registerMethod("Inhibit").implementedAs([object = obj.get()](std::string s1, std::string s2) {
                           return handleDbusScreensaver(s1, s2, 0, true, object->getCurrentlyProcessedMessage().getSender());
                       }),
                                   sdbus::registerMethod("UnInhibit").implementedAs([object = obj.get()](uint32_t c) {
                                       handleDbusScreensaver("", "", c, false, object->getCurrentlyProcessedMessage().getSender());
                                   }))
                        .forInterface(sdbus::InterfaceName{"org.freedesktop.ScreenSaver"});

                    m_sDBUSState.screenSaverObjects.push_back(std::move(obj));
                } catch (std::exception& e) { Debug::log(ERR, "Failed registering for {}, perhaps taken?\nerr: {}", path, e.what()); }
            }

            m_sDBUSState.screenSaverServiceConnection->addMatch("type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
                                                                ::handleDbusNameOwnerChanged);
        } catch (sdbus::Error& e) {
            if (e.getName() == sdbus::Error::Name{"org.freedesktop.DBus.Error.FileExists"}) {
                Debug::log(ERR, "Another service is already providing the org.freedesktop.ScreenSaver interface");
                Debug::log(ERR, "Is hypridle already running?");
            } else
                Debug::log(ERR, "Failed to connect to ScreenSaver service\nerr: {}", e.what());
        }
    }

    systemConnection.reset();
}

void CHypridle::handleInhibitOnDbusSleep(bool toSleep) {
    if (m_inhibitSleepBehavior == SLEEP_INHIBIT_NONE ||     //
        m_inhibitSleepBehavior == SLEEP_INHIBIT_LOCK_NOTIFY // Sleep inhibition handled via onLocked/onUnlocked
    )
        return;

    if (!toSleep)
        inhibitSleep();
    else
        uninhibitSleep();
}

void CHypridle::inhibitSleep() {
    if (!m_sDBUSState.login) {
        Debug::log(WARN, "Can't inhibit sleep. Dbus logind interface is not available.");
        return;
    }

    if (m_sDBUSState.sleepInhibitFd.isValid()) {
        Debug::log(WARN, "Called inhibitSleep, but previous sleep inhibitor is still active!");
        m_sDBUSState.sleepInhibitFd.reset();
    }

    auto method = m_sDBUSState.login->createMethodCall(sdbus::InterfaceName{"org.freedesktop.login1.Manager"}, sdbus::MethodName{"Inhibit"});
    method << "sleep";
    method << "hypridle";
    method << "Hypridle wants to delay sleep until it's before_sleep handling is done.";
    method << "delay";

    try {
        auto reply = m_sDBUSState.login->callMethod(method);

        if (!reply || !reply.isValid()) {
            Debug::log(ERR, "Failed to inhibit sleep");
            return;
        }

        if (reply.isEmpty()) {
            Debug::log(ERR, "Failed to inhibit sleep, empty reply");
            return;
        }

        sdbus::UnixFd fd;
        // This calls dup on the fd, no F_DUPFD_CLOEXEC :(
        // There seems to be no way to get the file descriptor out of the reply other than that.
        reply >> fd;

        // Setting the O_CLOEXEC flag does not work for some reason. Instead we make our own dupe and close the one from UnixFd.
        auto immidiateFD            = Hyprutils::OS::CFileDescriptor(fd.release());
        m_sDBUSState.sleepInhibitFd = immidiateFD.duplicate(F_DUPFD_CLOEXEC);
        immidiateFD.reset(); // close the fd that was opened with dup

        Debug::log(LOG, "Inhibited sleep with fd {}", m_sDBUSState.sleepInhibitFd.get());
    } catch (const std::exception& e) { Debug::log(ERR, "Failed to inhibit sleep ({})", e.what()); }
}

void CHypridle::uninhibitSleep() {
    if (!m_sDBUSState.sleepInhibitFd.isValid()) {
        Debug::log(ERR, "No sleep inhibitor fd to release");
        return;
    }

    Debug::log(LOG, "Releasing the sleep inhibitor!");
    m_sDBUSState.sleepInhibitFd.reset();
}
