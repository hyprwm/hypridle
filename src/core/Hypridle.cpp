#include "Hypridle.hpp"
#include "../helpers/Log.hpp"
#include "../config/ConfigManager.hpp"
#include "signal.h"
#include <sys/wait.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

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

    pollfd pollfds[] = {
        {
            .fd     = m_sDBUSState.connection->getEventLoopPollData().fd,
            .events = POLLIN,
        },
        {
            .fd     = wl_display_get_fd(m_sWaylandState.display),
            .events = POLLIN,
        },
    };

    std::thread pollThr([this, &pollfds]() {
        while (1) {
            int ret = poll(pollfds, 2, 5000 /* 5 seconds, reasonable. It's because we might need to terminate */);
            if (ret < 0) {
                Debug::log(CRIT, "[core] Polling fds failed with {}", errno);
                m_bTerminate = true;
                exit(1);
            }

            for (size_t i = 0; i < 3; ++i) {
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
    // clear child and leave child to init
    waitpid(child, NULL, 0);
    if (child < 0) {
        Debug::log(LOG, "Failed to create the second fork");
        return;
    }

    Debug::log(LOG, "Process Created with pid {}", grandchild);
}

void CHypridle::onIdled(SIdleListener* pListener) {
    Debug::log(LOG, "Idled: rule {:x}", (uintptr_t)pListener);

    if (pListener->onTimeout.empty()) {
        Debug::log(LOG, "Ignoring, onTimeout is empty.");
        return;
    }

    Debug::log(LOG, "Running {}", pListener->onTimeout);
    spawn(pListener->onTimeout);
}

void CHypridle::onResumed(SIdleListener* pListener) {
    Debug::log(LOG, "Resumed: rule {:x}", (uintptr_t)pListener);

    if (pListener->onRestore.empty()) {
        Debug::log(LOG, "Ignoring, onRestore is empty.");
        return;
    }

    Debug::log(LOG, "Running {}", pListener->onRestore);
    spawn(pListener->onRestore);
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

    static auto* const PSLEEPCMD = (Hyprlang::STRING const*)g_pConfigManager->getValuePtr("general:before_sleep_cmd");

    Debug::log(LOG, "Got PrepareForSleep from dbus");

    if (std::string{*PSLEEPCMD}.empty())
        return;

    Debug::log(LOG, "Running before-sleep: {}", *PSLEEPCMD);
    spawn(*PSLEEPCMD);
}

void CHypridle::setupDBUS() {
    auto proxy  = sdbus::createProxy("org.freedesktop.login1", "/org/freedesktop/login1");
    auto method = proxy->createMethodCall("org.freedesktop.login1.Manager", "GetSession");
    method << "auto";
    auto              reply = proxy->callMethod(method);

    sdbus::ObjectPath path;
    reply >> path;

    Debug::log(LOG, "Using dbus path {}", path.c_str());

    m_sDBUSState.connection->addMatch("type='signal',path='" + path + "',interface='org.freedesktop.login1.Session'", handleDbusLogin, sdbus::floating_slot_t{});
    m_sDBUSState.connection->addMatch("type='signal',path='/org/freedesktop/login1',interface='org.freedesktop.login1.Manager'", handleDbusSleep, sdbus::floating_slot_t{});
}
