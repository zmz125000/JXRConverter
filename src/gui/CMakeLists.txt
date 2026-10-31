find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Test)
option(JXRC_USE_SYSTEM_EXIV2 "Use an installed Exiv2 instead of the pinned current release" OFF)
if(JXRC_USE_SYSTEM_EXIV2)
    find_package(exiv2 REQUIRED CONFIG)
else()
    # The GUI reads EXIF from local still images. Avoid unrelated command-line,
    # network, video and XMP dependencies; retain BMFF/JXL metadata support.
    foreach(flag PNG NLS WEBREADY CURL VIDEO XMP EXTERNAL_XMP INIH)
        set(EXIV2_ENABLE_${flag} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(EXIV2_ENABLE_BMFF ON CACHE BOOL "" FORCE)
    set(EXIV2_ENABLE_BROTLI ON CACHE BOOL "" FORCE)
    foreach(flag SAMPLES EXIV2_COMMAND UNIT_TESTS FUZZ_TESTS DOC)
        set(EXIV2_BUILD_${flag} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(BROTLI_INCLUDE_DIR "${libjxl_SOURCE_DIR}/third_party/brotli/c/include")
    set(BROTLICOMMON_LIBRARY "$<BUILD_INTERFACE:brotlicommon>")
    set(BROTLIDEC_LIBRARY "$<BUILD_INTERFACE:brotlidec>")
    jxrc_source(exiv2 https://github.com/Exiv2/exiv2.git v0.28.9)
    set(exiv2_SOURCE_DIR "${exiv2_SOURCE_DIR}" PARENT_SCOPE)
    add_subdirectory("${exiv2_SOURCE_DIR}" "${exiv2_BINARY_DIR}" EXCLUDE_FROM_ALL)
    # Exiv2 generates these public headers at the top-level binary directory.
    target_include_directories(exiv2lib PUBLIC "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}>")
endif()
if(TARGET Exiv2::exiv2lib)
    set(JXRC_EXIV2_TARGET Exiv2::exiv2lib)
elseif(TARGET exiv2lib)
    set(JXRC_EXIV2_TARGET exiv2lib)
else()
    message(FATAL_ERROR "The Exiv2 package does not export a supported CMake target")
endif()
add_library(jxrc_desktop STATIC catalog.cpp catalog.hpp queue.cpp queue.hpp views.cpp views.hpp window.cpp window.hpp style.cpp style.hpp)
set_target_properties(jxrc_desktop PROPERTIES AUTOMOC ON)
target_include_directories(jxrc_desktop PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(jxrc_desktop PUBLIC jxrc Qt6::Widgets ${JXRC_EXIV2_TARGET})
add_executable(jxrconverter WIN32 MACOSX_BUNDLE main.cpp)
qt_add_resources(jxrconverter app_icons PREFIX "/icons"
    BASE "${PROJECT_SOURCE_DIR}/assets/icons"
    FILES "${PROJECT_SOURCE_DIR}/assets/icons/jxrconverter.png")
target_link_libraries(jxrconverter PRIVATE jxrc_desktop)
set_target_properties(jxrconverter PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    MACOSX_BUNDLE_BUNDLE_NAME JXRConverter MACOSX_BUNDLE_GUI_IDENTIFIER org.jxrconverter.desktop
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}" MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
if(UNIX AND NOT APPLE)
    file(RELATIVE_PATH jxrc_runtime_libdir "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}"
        "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
    set_target_properties(jxrconverter PROPERTIES INSTALL_RPATH "$ORIGIN/${jxrc_runtime_libdir}")
endif()
if(WIN32)
    target_sources(jxrconverter PRIVATE "${PROJECT_BINARY_DIR}/generated/version.rc")
endif()
if(APPLE)
    # macdeployqt bundles Qt frameworks but preserves the executable's runpath.
    # Resolve @rpath/Qt*.framework from inside this app bundle.
    set_target_properties(jxrconverter PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
    set(jxrc_bundle_icon "${PROJECT_SOURCE_DIR}/assets/icons/jxrconverter.icns")
    target_sources(jxrconverter PRIVATE "${jxrc_bundle_icon}")
    set_source_files_properties("${jxrc_bundle_icon}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(jxrconverter PROPERTIES MACOSX_BUNDLE_ICON_FILE jxrconverter.icns)
endif()
install(FILES "${PROJECT_SOURCE_DIR}/assets/icons/jxrconverter.svg"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps" COMPONENT Runtime)
install(TARGETS jxrconverter RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime BUNDLE DESTINATION . COMPONENT Runtime)
option(JXRC_DEPLOY_QT "Bundle Qt runtime dependencies when installing the GUI" OFF)
if(JXRC_DEPLOY_QT)
    if(Qt6_VERSION VERSION_LESS 6.10)
        message(FATAL_ERROR "Runtime packaging requires Qt 6.10 or newer")
    endif()
    qt6_generate_deploy_app_script(TARGET jxrconverter OUTPUT_SCRIPT deploy_script
        NO_TRANSLATIONS INCLUDE_PLUGINS qoffscreen
        # Local file conversion needs neither network/TLS plugins nor embedded
        # framebuffer/input backends. Keep desktop platforms, themes, image
        # codecs and input methods, including Wayland and X11 on Linux.
        EXCLUDE_PLUGIN_TYPES networkinformation tls generic egldeviceintegrations
        EXCLUDE_PLUGINS qeglfs qlinuxfb qvkkhrdisplay qminimalegl)
    install(SCRIPT "${deploy_script}" COMPONENT Runtime)
endif()
if(JXRC_BUILD_TESTS)
    add_executable(jxrc_gui_tests ../../tests/gui.cpp)
    set_target_properties(jxrc_gui_tests PROPERTIES AUTOMOC ON)
    target_link_libraries(jxrc_gui_tests PRIVATE jxrc_desktop Qt6::Test)
    add_test(NAME desktop COMMAND jxrc_gui_tests)
    add_test(NAME desktop_version COMMAND jxrconverter --version)
    set_tests_properties(desktop_version PROPERTIES PASS_REGULAR_EXPRESSION "JXRConverter ${PROJECT_VERSION}")
    set_tests_properties(desktop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" FIXTURES_REQUIRED cli_input)
endif()
