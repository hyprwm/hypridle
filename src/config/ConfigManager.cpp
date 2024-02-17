#include "ConfigManager.hpp"
#include <filesystem>
#include "../helpers/VarList.hpp"

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
    m_config.addSpecialCategory("listener", Hyprlang::SSpecialCategoryOptions{.key = "timeout"});
    m_config.addSpecialConfigValue("listener", "on-timeout", Hyprlang::STRING{""});
    m_config.addSpecialConfigValue("listener", "on-resume", Hyprlang::STRING{""});

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
        STimeoutRule rule;
        uint64_t     timeout = 0;
        try {
            timeout = std::stoull(std::any_cast<Hyprlang::STRING>(m_config.getSpecialConfigValue("listener", "timeout", k.c_str())));
        } catch (std::exception& e) {
            result.setError(
                (std::string{"Faulty rule: cannot parse timeout "} + std::any_cast<Hyprlang::STRING>(m_config.getSpecialConfigValue("listener", "timeout", k.c_str()))).c_str());
            continue;
        }

        rule.timeout   = timeout;
        rule.onTimeout = std::any_cast<Hyprlang::STRING>(m_config.getSpecialConfigValue("listener", "on-timeout", k.c_str()));
        rule.onResume  = std::any_cast<Hyprlang::STRING>(m_config.getSpecialConfigValue("listener", "on-resume", k.c_str()));

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