#include <stdio.h>
#include "gui.h"
#include "app.h"

int main(int argc, char *argv[]) {
    // Initialize the application
    if (!app_initialize()) {
        fprintf(stderr, "Failed to initialize the application.\n");
        return 1;
    }

    // Initialize the GUI
    if (!gui_initialize()) {
        fprintf(stderr, "Failed to initialize the GUI.\n");
        app_cleanup();
        return 1;
    }

    // Start the main event loop
    gui_run();

    // Cleanup before exiting
    app_cleanup();
    return 0;
}