
#include "config/ConfigManager.hpp"
#include "core/Hypridle.hpp"
#include "helpers/Log.hpp"

int main(int argc, char** argv, char** envp) {
    std::string configPath;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--verbose" || arg == "-v")
            Debug::verbose = true;

        else if (arg == "--quiet" || arg == "-q")
            Debug::quiet = true;

        else if (arg == "--config" || arg == "-c") {
            configPath = argv[++i];
        }
    }

    try {
        g_pConfigManager = std::make_unique<CConfigManager>(configPath);
        g_pConfigManager->init();
    } catch (const char* err) {
        Debug::log(CRIT, "ConfigManager threw: {}", err);
        std::string strerr = err;
        if (strerr.contains("File does not exist"))
            Debug::log(NONE, "           Make sure you have a config.");
        return 1;
    }

    g_pHypridle = std::make_unique<CHypridle>();
    g_pHypridle->run();

    return 0;
}
