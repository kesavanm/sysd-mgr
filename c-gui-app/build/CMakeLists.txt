project(CGuiApp)

set(CMAKE_C_STANDARD 11)

# Specify the source files
set(SOURCES
    ../src/main.c
    ../src/app.c
    ../src/gui.c
)

# Include directories
include_directories(
    ../include
    ../src
)

# Add the executable
add_executable(c-gui-app ${SOURCES})

# Link any necessary libraries (e.g., GTK, Qt, etc.)
# find_package(GTK REQUIRED)
# target_link_libraries(c-gui-app ${GTK_LIBRARIES})