list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
    "${WEBCORE_DIR}/platform/graphics/haiku"
)

list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
    platform/graphics/haiku/FontCascadeHaikuInlines.h
    platform/graphics/haiku/GraphicsContextHaiku.h
    platform/graphics/haiku/ImageBufferHaikuSurfaceBackend.h
    platform/graphics/haiku/PathHaiku.h
)
