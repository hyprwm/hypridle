#include "ConfigManager.hpp"
#include "../helpers/Log.hpp"
#include "../helpers/MiscFunctions.hpp"
#include <hyprutils/path/Path.hpp>
#include <filesystem>
#include <glob.h>
#include <cstring>

static std::string getMainConfigPath() {
    static const auto paths = Hyprutils::Path::findConfig("hypridle");
    if (paths.first.has_value())
        return paths.first.value();
    else
        throw std::runtime_error("Could not find config in HOME, XDG_CONFIG_HOME, XDG_CONFIG_DIRS or /etc/hypr.");
}

CConfigManager::CConfigManager(std::string configPath) :
    m_config(configPath.empty() ? getMainConfigPath().c_str() : configPath.c_str(), Hyprlang::SConfigOptions{.throwAllErrors = true, .allowMissingConfig = false}) {
    ;
    configCurrentPath = configPath.empty() ? getMainConfigPath() : configPath;
}

static Hyprlang::CParseResult handleSource(const char* c, const char* v) {
    const std::string      VALUE   = v;
    const std::string      COMMAND = c;

    const auto             RESULT = g_pConfigManager->handleSource(COMMAND, VALUE);

    Hyprlang::CParseResult result;
    if (RESULT.has_value())
        result.setError(RESULT.value().c_str());
    return result;
}

void CConfigManager::init() {
    m_config.addSpecialCategory("listener", Hyprlang::SSpecialCategoryOptions{.key = nullptr, .anonymousKeyBased = true});
    m_config.addSpecialConfigValue("listener", "timeout", Hyprlang::INT{-1});
    m_config.addSpecialConfigValue("listener", "on-timeout", Hyprlang::STRING{""});
    m_config.addSpecialConfigValue("listener", "on-resume", Hyprlang::STRING{""});
    m_config.addSpecialConfigValue("listener", "ignore_inhibit", Hyprlang::INT{0});

    m_config.addConfigValue("general:lock_cmd", Hyprlang::STRING{""});
    m_config.addConfigValue("general:unlock_cmd", Hyprlang::STRING{""});
    m_config.addConfigValue("general:on_lock_cmd", Hyprlang::STRING{""});
    m_config.addConfigValue("general:on_unlock_cmd", Hyprlang::STRING{""});
    m_config.addConfigValue("general:before_sleep_cmd", Hyprlang::STRING{""});
    m_config.addConfigValue("general:after_sleep_cmd", Hyprlang::STRING{""});
    m_config.addConfigValue("general:ignore_dbus_inhibit", Hyprlang::INT{0});
    m_config.addConfigValue("general:ignore_systemd_inhibit", Hyprlang::INT{0});
    m_config.addConfigValue("general:ignore_wayland_inhibit", Hyprlang::INT{0});
    m_config.addConfigValue("general:inhibit_sleep", Hyprlang::INT{2});

    // track the file in the circular dependency chain
    const auto mainConfigPath = getMainConfigPath();
    alreadyIncludedSourceFiles.insert(std::filesystem::canonical(mainConfigPath));

    m_config.registerHandler(&::handleSource, "source", {.allowFlags = false});

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

        rule.ignoreInhibit = std::any_cast<Hyprlang::INT>(m_config.getSpecialConfigValue("listener", "ignore_inhibit", k.c_str()));

        if (timeout == -1) {
            result.setError("Category has a missing timeout setting");
            continue;
        }

        m_vRules.emplace_back(rule);
    }

    for (auto& r : m_vRules) {
        Debug::log(LOG, "Registered timeout rule for {}s:\n      on-timeout: {}\n      on-resume: {}\n      ignore_inhibit: {}", r.timeout, r.onTimeout, r.onResume, r.ignoreInhibit);
    }

    return result;
}

std::vector<CConfigManager::STimeoutRule> CConfigManager::getRules() {
    return m_vRules;
}

std::optional<std::string> CConfigManager::handleSource(const std::string& command, const std::string& rawpath) {
    if (rawpath.length() < 2) {
        return "source path " + rawpath + " bogus!";
    }
    std::unique_ptr<glob_t, void (*)(glob_t*)> glob_buf{new glob_t, [](glob_t* g) { globfree(g); }};
    memset(glob_buf.get(), 0, sizeof(glob_t));

    const auto CURRENTDIR = std::filesystem::path(configCurrentPath).parent_path().string();

    if (auto r = glob(absolutePath(rawpath, CURRENTDIR).c_str(), GLOB_TILDE, nullptr, glob_buf.get()); r != 0) {
        std::string err = std::format("source= globbing error: {}", r == GLOB_NOMATCH ? "found no match" : GLOB_ABORTED ? "read error" : "out of memory");
        Debug::log(ERR, "{}", err);
        return err;
    }

    for (size_t i = 0; i < glob_buf->gl_pathc; i++) {
        const auto PATH = absolutePath(glob_buf->gl_pathv[i], CURRENTDIR);

        if (PATH.empty() || PATH == configCurrentPath) {
            Debug::log(WARN, "source= skipping invalid path");
            continue;
        }

        if (std::find(alreadyIncludedSourceFiles.begin(), alreadyIncludedSourceFiles.end(), PATH) != alreadyIncludedSourceFiles.end()) {
            Debug::log(WARN, "source= skipping already included source file {} to prevent circular dependency", PATH);
            continue;
        }

        if (!std::filesystem::is_regular_file(PATH)) {
            if (std::filesystem::exists(PATH)) {
                Debug::log(WARN, "source= skipping non-file {}", PATH);
                continue;
            }

            Debug::log(ERR, "source= file doesnt exist");
            return "source file " + PATH + " doesn't exist!";
        }

        // track the file in the circular dependency chain
        alreadyIncludedSourceFiles.insert(PATH);

        // allow for nested config parsing
        auto backupConfigPath = configCurrentPath;
        configCurrentPath     = PATH;

        m_config.parseFile(PATH.c_str());

        configCurrentPath = backupConfigPath;
    }

    return {};
}
