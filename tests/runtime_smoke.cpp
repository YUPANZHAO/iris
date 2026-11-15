#include "iris/iris.h"

#include <iostream>

int main() {
    iris::RuntimeConfig config;
    config.logLevel = 0;

    iris::Runtime runtime(config);
    if (runtime.version().empty()) {
        std::cerr << "libiris version is empty\n";
        return 1;
    }
    if (runtime.pjprojectVersion().empty() ||
        runtime.pjprojectVersion() == "unknown") {
        std::cerr << "pjproject version is unavailable\n";
        return 1;
    }
    if (!runtime.accounts().empty() || !runtime.calls().empty()) {
        std::cerr << "new runtime is not empty\n";
        return 1;
    }

    runtime.pollEvents();
    return 0;
}

