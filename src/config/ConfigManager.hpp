#pragma once

#include "../helpers/Log.hpp"

#include <hyprlang.hpp>

#include <vector>
#include <memory>

class CConfigManager {
  public:
    CConfigManager(std::string configPath);
    void init();

    struct STimeoutRule {
        uint64_t    timeout   = 0;
        std::string onTimeout = "";
        std::string onResume  = "";
    };

    std::vector<STimeoutRule> getRules();

    template <typename T>
    Hyprlang::CSimpleConfigValue<T> getValue(const std::string& name) {
        return Hyprlang::CSimpleConfigValue<T>(&m_config, name.c_str());
    }

  private:
    Hyprlang::CConfig         m_config;

    std::vector<STimeoutRule> m_vRules;

    Hyprlang::CParseResult    postParse();
};

inline std::unique_ptr<CConfigManager> g_pConfigManager;
