#include <cstring>
#include <string>

#include "test_framework.h"

#ifdef _WIN32
int RunIntegration(const char* diskPath);  // integration_win.cpp
#endif

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : "";
#ifdef _WIN32
    // integration mode: df-tests --integration \\.\PhysicalDrive2
    if (argc > 2 && std::strcmp(argv[1], "--integration") == 0) return RunIntegration(argv[2]);
    if (argc > 1 && std::strcmp(argv[1], "--integration") == 0) {
        std::printf("usage: df-tests --integration \\\\.\\PhysicalDriveN\n");
        return 2;
    }
#endif
    return RunAllTests(filter);
}
