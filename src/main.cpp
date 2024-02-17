
#include "config/ConfigManager.hpp"
#include "core/Hypridle.hpp"

int main(int argc, char** argv, char** envp) {

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--verbose" || arg == "-v")
            Debug::verbose = true;

        else if (arg == "--quiet" || arg == "-q")
            Debug::quiet = true;
    }

    g_pConfigManager = std::make_unique<CConfigManager>();
    g_pConfigManager->init();

    g_pHypridle = std::make_unique<CHypridle>();
    g_pHypridle->run();

    return 0;
}
