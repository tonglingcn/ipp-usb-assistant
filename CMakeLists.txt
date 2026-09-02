cmake_minimum_required(VERSION 3.13)
if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)
endif()
project(ipp-usb-assistant VERSION 1.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)

# 同时支持 Qt6 + Dtk6（deepin/UOS 25）与 Qt5 + Dtk5（UOS 20）。
# 先静默探测 Qt6，未命中再要求 Qt5；DTK 的大版本必须与 Qt 一致。
find_package(Qt6 QUIET COMPONENTS Core)
if(Qt6_FOUND)
    set(QT_MAJOR 6)
    set(DTK_SUFFIX 6)
else()
    find_package(Qt5 REQUIRED COMPONENTS Core)
    set(QT_MAJOR 5)
    set(DTK_SUFFIX "")
endif()
message(STATUS "Building against Qt${QT_MAJOR} / Dtk${DTK_SUFFIX}")

find_package(Qt${QT_MAJOR} REQUIRED COMPONENTS Core Widgets Concurrent LinguistTools)
find_package(Dtk${DTK_SUFFIX}Core REQUIRED)
find_package(Dtk${DTK_SUFFIX}Gui REQUIRED)
find_package(Dtk${DTK_SUFFIX}Widget REQUIRED)

# CUPS 打印后端（pkg-config 在 deepin 25 上比 find_package(CUPS) 更可靠）。
# 注意：UOS 20 的 libcups2-dev 并不附带 cups.pc（只提供 /usr/include/cups 与
# libcups.so），所以 pkg-config 探测不到时必须回退到手工查找，
# 否则在 UOS 20 上会硬失败。两种写法都会填充 CUPS_INCLUDE_DIRS / CUPS_LIBRARIES。
find_package(PkgConfig REQUIRED)
pkg_check_modules(CUPS QUIET cups)
if(NOT CUPS_FOUND)
    message(STATUS "cups.pc not found, falling back to manual CUPS detection")
    find_path(CUPS_INCLUDE_DIRS NAMES cups/cups.h)
    find_library(CUPS_LIBRARIES NAMES cups)
    if(CUPS_INCLUDE_DIRS AND CUPS_LIBRARIES)
        set(CUPS_FOUND TRUE)
    endif()
endif()
if(NOT CUPS_FOUND)
    message(FATAL_ERROR "CUPS not found: install libcups2-dev (deepin/UOS) or cups-devel")
endif()
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
    Qt${QT_MAJOR}::Core
    Qt${QT_MAJOR}::Widgets
    Qt${QT_MAJOR}::Concurrent
    Dtk${DTK_SUFFIX}::Core
    Dtk${DTK_SUFFIX}::Gui
    Dtk${DTK_SUFFIX}::Widget
    ${CUPS_LIBRARIES}
    ${SANE_LIBRARIES}
)

set(IPP_USB_ASSISTANT_TS_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/ipp-usb-assistant_en.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/ipp-usb-assistant_zh_CN.ts
)

if(QT_MAJOR EQUAL 6)
    set_source_files_properties(${IPP_USB_ASSISTANT_TS_FILES}
        PROPERTIES OUTPUT_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/translations
    )
    qt_add_translations(
        TARGETS ipp-usb-assistant
        SOURCE_TARGETS ipp-usb-assistant
        TS_FILES ${IPP_USB_ASSISTANT_TS_FILES}
        QM_FILES_OUTPUT_VARIABLE IPP_USB_ASSISTANT_QM_FILES
    )
else()
    # Qt 5 没有 qt_add_translations，改用 qt5_add_translation 直接把 .ts 编译成 .qm。
    # 注意它不会回写 .ts，所以源码文案变更后仍需手动跑一次 lupdate。
    qt5_add_translation(IPP_USB_ASSISTANT_QM_FILES ${IPP_USB_ASSISTANT_TS_FILES})
endif()

# qt5_add_translation 只生成自定义命令，不会自动挂到任何目标上，
# 必须显式建一个 ALL 目标，否则 .qm 永远不会被编译出来（Qt 6 由
# qt_add_translations 自动挂载，这里重复声明一次也无害）。
add_custom_target(ipp-usb-assistant-translations ALL
    DEPENDS ${IPP_USB_ASSISTANT_QM_FILES}
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
