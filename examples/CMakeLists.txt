set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples)

if (UNIX)
    file(COPY res DESTINATION .)
elseif (WIN32)
    file(COPY res DESTINATION ${CMAKE_BUILD_TYPE})
endif()

set(LVN_SOURCES
    antiAliasing.cpp
    bindlessTexture.cpp
    colorBlending.cpp
    cubemap.cpp
    entityComponentSystem.cpp
    events.cpp
    framebuffer.cpp
    loadingModel.cpp
    loadingShader.cpp
    logging.cpp
    loggingToFile.cpp
    memoryPool.cpp
    pbrScene.cpp
    pbrSpheres.cpp
    pong.cpp
    renderer2d.cpp
    simpleMatrix.cpp
    simpleSound.cpp
    simpleText.cpp
    simpleTexture.cpp
    simpleTriangle.cpp
    simpleWindow.cpp
    twoTextures.cpp
    twoWindows.cpp
)

foreach(LVN_SRC ${LVN_SOURCES})
    get_filename_component(LVN_SRC_NAME ${LVN_SRC} NAME)
    string(REPLACE ".cpp" "" LVN_SRC_NAME ${LVN_SRC_NAME})

    add_executable(${LVN_SRC_NAME} ${LVN_SRC})
    target_include_directories(${LVN_SRC_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(${LVN_SRC_NAME} PRIVATE levikno)
endforeach()
