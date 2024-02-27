#include "ConfigManager.hpp"
#include <filesystem>

static std::string getConfigDir() {
    static const char* xdgConfigHome = getenv("XDG_CONFIG_HOME");

    if (xdgConfigHome && std::filesystem::path(xdgConfigHome).is_absolute())
        return xdgConfigHome;

    return getenv("HOME") + std::string("/.config");
}

static std::string getMainConfigPath() {
    return getConfigDir() + "/hypr/hypridle.conf";
}

CConfigManager::CConfigManager() : m_config(getMainConfigPath().c_str(), Hyprlang::SConfigOptions{.throwAllErrors = true, .allowMissingConfig = false}) {
    ;
}

void CConfigManager::init() {
    m_config.addSpecialCategory("listener", Hyprlang::SSpecialCategoryOptions{.key = nullptr, .anonymousKeyBased = true});
    m_config.addSpecialConfigValue("listener", "timeout", Hyprlang::INT{-1});
    m_config.addSpecialConfigValue("listener", "on-timeout", Hyprlang::STRING{""});
    m_config.addSpecialConfigValue("listener", "on-resume", Hyprlang::STRING{""});

    m_config.addConfigValue("general:lock_cmd", Hyprlang::STRING{""});
    m_config.addConfigValue("general:unlock_cmd", Hyprlang::STRING{""});
    m_config.addConfigValue("general:before_sleep_cmd", Hyprlang::STRING{""});
    m_config.addConfigValue("general:after_sleep_cmd", Hyprlang::STRING{""});
    m_config.addConfigValue("general:ignore_dbus_inhibit", Hyprlang::INT{0});

    m_config.commence();

    auto result = m_config.parse();

    if (result.error)
        Debug::log(ERR, "Config has errors:\n{}\nProceeding ignoring faulty entries", result.getError());

    result = postParse();

    if (result.error)
        Debug::log(ERR, "Config has errors:\n{}\nProceeding ignoring faulty entries", result.getError());
}

Hyprlang::CParseResult CConfigManager::postParse() {
    const auto             KEYS = m_config.listKeysForSpecialCategory("listener");

    Hyprlang::CParseResult result;
    if (KEYS.empty()) {
        result.setError("No rules configured");
        return result;
    }

    for (auto& k : KEYS) {
        STimeoutRule  rule;

        Hyprlang::INT timeout = std::any_cast<Hyprlang::INT>(m_config.getSpecialConfigValue("listener", "timeout", k.c_str()));

        rule.timeout   = timeout;
        rule.onTimeout = std::any_cast<Hyprlang::STRING>(m_config.getSpecialConfigValue("listener", "on-timeout", k.c_str()));
        rule.onResume  = std::any_cast<Hyprlang::STRING>(m_config.getSpecialConfigValue("listener", "on-resume", k.c_str()));

        if (timeout == -1) {
            result.setError("Category has a missing timeout setting");
            continue;
        }

        m_vRules.emplace_back(rule);
    }

    for (auto& r : m_vRules) {
        Debug::log(LOG, "Registered timeout rule for {}s:\n      on-timeout: {}\n      on-resume: {}", r.timeout, r.onTimeout, r.onResume);
    }

    return result;
}

std::vector<CConfigManager::STimeoutRule> CConfigManager::getRules() {
    return m_vRules;
}

std::string CConfigManager::getOnTimeoutCommand() {
    return m_vRules.front().onTimeout;
}

void* const* CConfigManager::getValuePtr(const std::string& name) {
    return m_config.getConfigValuePtr(name.c_str())->getDataStaticPtr();
}
