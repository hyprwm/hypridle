
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

        else if (arg == "--version" || arg == "-V") {
            Debug::log(NONE, "hypridle v{}", HYPRIDLE_VERSION);
            return 0;
        }

        else if (arg == "--config" || arg == "-c") {
            if (i + 1 >= argc) {
                Debug::log(NONE, "After " + arg + " you should provide a path to a config file.");
                return 1;
            }

            if (!configPath.empty()) {
                Debug::log(NONE, "Multiple config files are provided.");
                return 1;
            }

            configPath = argv[++i];
            if (configPath[0] == '-') { // Should be fine, because of the null terminator
                Debug::log(NONE, "After " + arg + " you should provide a path to a config file.");
                return 1;
            }
        }

        else if (arg == "--help" || arg == "-h") {
            Debug::log(NONE,
                "Usage: hypridle [options]\n"
                "Options:\n"
                "  -v, --verbose       Enable verbose logging\n"
                "  -q, --quiet         Suppress all output except errors\n"
                "  -V, --version       Show version information\n"
                "  -c, --config <path> Specify a custom config file path\n"
                "  -h, --help          Show this help message"
            );
            return 0;
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
