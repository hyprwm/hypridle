#include "Hypridle.hpp"
#include "../helpers/Log.hpp"
#include "../config/ConfigManager.hpp"
#include "signal.h"
#include <sys/wait.h>

CHypridle::CHypridle() {
    m_sWaylandState.display = wl_display_connect(nullptr);
    if (!m_sWaylandState.display) {
        Debug::log(CRIT, "Couldn't connect to a wayland compositor");
        throw;
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
        throw;
    }

    const auto RULES = g_pConfigManager->getRules();
    m_sWaylandIdleState.listeners.resize(RULES.size());

    for (size_t i = 0; i < RULES.size(); ++i) {
        auto&       l  = m_sWaylandIdleState.listeners[i];
        const auto& r  = RULES[i];
        l.notification = ext_idle_notifier_v1_get_idle_notification(m_sWaylandIdleState.notifier, r.timeout * 1000 /* ms */, m_sWaylandState.seat);
        l.onRestore    = r.onResume;
        l.onTimeout    = r.onTimeout;

        ext_idle_notification_v1_add_listener(l.notification, &idleListener, &l);
    }

    while (wl_display_dispatch(m_sWaylandState.display) != -1) {
        ;
    }
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
