# Xbox D3D8 Compatibility Layer
#   _WIN32 -> backed by Direct3D 11 (d3d8_*.c, d3d8_combiners.c, d3d8_vsh.c)
#   POSIX  -> backed by OpenGL 3.3 core via SDL2 + libepoxy (d3d8_gl.c)

if(WIN32)
    add_library(xbox_d3d8 STATIC
        d3d8_device.c
        d3d8_formats.c
        d3d8_gamma.c
        d3d8_present.c
        d3d8_resources.c
        d3d8_shaders.c
        d3d8_states.c
        d3d8_combiners.c
        d3d8_vsh.c
        d3d8_extvs.c
    )
    target_link_libraries(xbox_d3d8 PUBLIC platform d3d11 dxgi dxguid d3dcompiler)

    # Offline smoke test for the pure format layer (no D3D11 device needed).
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/d3d8_smoke)
else()
    # Off Windows the device is Vulkan (MoltenVK on macOS) when the Vulkan
    # loader and shaderc are installed, and the older OpenGL one otherwise.
    # Only the Vulkan device draws what the push-buffer executor asks for.
    find_package(Vulkan QUIET)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SHADERC QUIET IMPORTED_TARGET shaderc)
    option(XBOXRECOMP_VULKAN "Build the Vulkan Direct3D 8 device" ON)
    if(XBOXRECOMP_VULKAN AND Vulkan_FOUND AND SHADERC_FOUND)
        add_library(xbox_d3d8 STATIC
            d3d8_vk.c
            d3d8_formats.c
            d3d8_combiners.c
        )
        target_link_libraries(xbox_d3d8 PUBLIC platform Vulkan::Vulkan PkgConfig::SHADERC)
        target_compile_definitions(xbox_d3d8 PUBLIC XBOXRECOMP_HAVE_VULKAN=1)
        message(STATUS "xbox_d3d8: Vulkan device (shaderc ${SHADERC_VERSION})")
    elseif(XBOXRECOMP_SDL3)
        message(FATAL_ERROR "XBOXRECOMP_SDL3 off Windows needs the Vulkan device "
                            "(the Vulkan loader and shaderc); the OpenGL one is built on SDL2.")
    else()
        add_library(xbox_d3d8 STATIC
            d3d8_gl.c
            d3d8_formats.c
            d3d8_nv2a_pending.c
        )
        find_package(SDL2 REQUIRED)
        pkg_check_modules(EPOXY REQUIRED epoxy)
        target_include_directories(xbox_d3d8 PRIVATE ${EPOXY_INCLUDE_DIRS})
        target_link_libraries(xbox_d3d8 PUBLIC platform SDL2::SDL2 ${EPOXY_LINK_LIBRARIES})
        message(STATUS "xbox_d3d8: OpenGL device (Vulkan or shaderc not found)")
    endif()
endif()
target_include_directories(xbox_d3d8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(xbox_d3d8 PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_d3d8 PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
