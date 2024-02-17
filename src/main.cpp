
#include "config/ConfigManager.hpp"
#include "core/Hypridle.hpp"

int main(int argc, char** argv, char** envp) {

    g_pConfigManager = std::make_unique<CConfigManager>();
    g_pConfigManager->init();

    g_pHypridle = std::make_unique<CHypridle>();
    g_pHypridle->run();

    return 0;
}