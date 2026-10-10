# Build tools

# Build useful tool binaries.
if(RTS_BUILD_GENERALS_TOOLS)
    add_subdirectory(GUIEdit)
    add_subdirectory(ImagePacker)
    add_subdirectory(MapCacheBuilder)
    add_subdirectory(W3DView)
    if(NOT RTS_STATIC_CRT)
        add_subdirectory(WorldBuilder)
    endif()
endif()

# Build less useful tool/test binaries.
if(RTS_BUILD_GENERALS_EXTRAS)
    add_subdirectory(Autorun)
    add_subdirectory(Launcher)
    add_subdirectory(PATCHGET)
endif()
