cmake_minimum_required(VERSION 3.13)
if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)
endif()
project(ipp-usb-assistant VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Concurrent LinguistTools)
find_package(Dtk6Core REQUIRED)
find_package(Dtk6Gui REQUIRED)
find_package(Dtk6Widget REQUIRED)

# CUPS 打印后端（pkg-config 在 deepin 25 上比 find_package(CUPS) 更可靠）
find_package(PkgConfig REQUIRED)
pkg_check_modules(CUPS REQUIRED cups)
# SANE 扫描后端（deepin 25 的模块名为 sane-backends，其他发行版可能为 sane）
pkg_check_modules(SANE sane-backends)
if(NOT SANE_FOUND)
    pkg_check_modules(SANE REQUIRED sane)
endif()

add_executable(ipp-usb-assistant
    src/main.cpp
    src/mainwindow.cpp
    src/privileges.cpp
    src/printmanager.cpp
    src/scannermanager.cpp
    src/envchecker.cpp
    src/advancedsettings.cpp
    src/ppdconfig.cpp
    src/printerconfigdialog.cpp
    src/printpropertiesdialog.cpp
    src/twolineitemdelegate.cpp
    src/addprinterdialog.cpp
    src/themehelper.cpp
    src/focusstyle.cpp
    src/diagnostics.cpp
    resources/icons.qrc
)

target_compile_definitions(ipp-usb-assistant PRIVATE
    IPP_USB_ASSISTANT_VERSION="${PROJECT_VERSION}"
)

target_include_directories(ipp-usb-assistant PRIVATE
    ${CUPS_INCLUDE_DIRS}
    ${SANE_INCLUDE_DIRS}
)

target_link_libraries(ipp-usb-assistant PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Concurrent
    Dtk6::Core
    Dtk6::Gui
    Dtk6::Widget
    ${CUPS_LIBRARIES}
    ${SANE_LIBRARIES}
)

set(IPP_USB_ASSISTANT_TS_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/ipp-usb-assistant_en.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/ipp-usb-assistant_zh_CN.ts
)
set_source_files_properties(${IPP_USB_ASSISTANT_TS_FILES}
    PROPERTIES OUTPUT_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/translations
)
qt_add_translations(
    TARGETS ipp-usb-assistant
    SOURCE_TARGETS ipp-usb-assistant
    TS_FILES ${IPP_USB_ASSISTANT_TS_FILES}
    QM_FILES_OUTPUT_VARIABLE IPP_USB_ASSISTANT_QM_FILES
)

install(TARGETS ipp-usb-assistant
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# 应用图标（hicolor 多尺寸）
set(IPP_USB_ASSISTANT_ICON_SIZES 16 24 32 48 64 128 256)
foreach(_size ${IPP_USB_ASSISTANT_ICON_SIZES})
    install(FILES resources/icons/hicolor/${_size}x${_size}/apps/ipp-usb-assistant.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps
    )
endforeach()

install(FILES data/org.deepin.ipp-usb.assistant.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

# 可选：放置 resources/icons/ipp-usb-assistant.dci 后取消下行注释
# install(FILES resources/icons/ipp-usb-assistant.dci
#     DESTINATION ${CMAKE_INSTALL_DATADIR}/dsg/icons
# )

install(FILES ${IPP_USB_ASSISTANT_QM_FILES}
    DESTINATION ${CMAKE_INSTALL_DATADIR}/ipp-usb-assistant/translations
)
