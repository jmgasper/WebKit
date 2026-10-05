# Haiku: ANGLE's OpenGL back-end on top of the system EGL (Mesa's Haiku EGL
# platform, which Summit points at its private zink build), as the GTK and WPE
# ports use it. CMake counts Haiku as UNIX, so ANGLE's CMakeLists.txt treats it
# as Linux: the common Linux sources (system_utils_linux.cpp, which handles
# Haiku where it differs) and the EGL back-end of GL.cmake are already in the
# lists; what the Linux ports add on top is added here. No X11, Wayland or GBM.
list(APPEND ANGLE_DEFINITIONS USE_SYSTEM_EGL EGL_NO_PLATFORM_SPECIFIC_TYPES)
include(linux.cmake)

if (USE_ANGLE_EGL OR ENABLE_WEBGL)
    list(APPEND ANGLE_SOURCES
        ${gl_backend_sources}

        ${angle_dma_buf_sources}

        ${libangle_gpu_info_util_sources}
        ${libangle_gpu_info_util_linux_sources}
    )

    list(APPEND ANGLE_DEFINITIONS
        ANGLE_ENABLE_OPENGL
    )

    list(APPEND ANGLEGLESv2_LIBRARIES
        ${CMAKE_DL_LIBS}
        Threads::Threads
    )
endif ()

list(APPEND ANGLEGLESv2_LIBRARIES
    z
)
