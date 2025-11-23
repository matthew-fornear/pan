#include <iostream>
#include "pan/gui/main_window.h"
#include <csignal>
#include <atomic>
#include <sys/prctl.h>

static pan::MainWindow* g_mainWindow = nullptr;

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    if (g_mainWindow) {
        g_mainWindow->requestQuit();
    }
}

int main(int argc, char* argv[]) {
    // Set up signal handlers for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGHUP, signalHandler);   // Terminal hangup - when terminal closes
    std::signal(SIGTERM, signalHandler);
    
    // On Linux, detect when parent process (terminal) dies
    #ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);  // Send SIGTERM when parent dies
    #endif
    
    std::cout << "Pan DAW - Starting..." << std::endl;

    pan::MainWindow window;
    g_mainWindow = &window;  // Store pointer for signal handler
    
    if (!window.initialize()) {
        std::cerr << "Failed to initialize main window" << std::endl;
        return 1;
    }

    std::cout << "Pan DAW initialized successfully!" << std::endl;
    
    // Run main loop
    window.run();
    
    g_mainWindow = nullptr;  // Clear pointer
    
    window.shutdown();
    
    std::cout << "\nDone!" << std::endl;
    return 0;
}
