cmake_minimum_required(VERSION 3.21)

project(McuRegDebugger
    VERSION 1.1.1.0
    DESCRIPTION "MCU register-level UI debugger (J-Link + CMSIS-SVD)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network OpenGL OpenGLWidgets)
qt_standard_project_setup()

set(CORE_SOURCES
    src/core/HexUtil.h
    src/core/HexUtil.cpp
    src/core/Result.h
    src/core/Logger.h
    src/core/Logger.cpp
    src/core/SvdTypes.h
    src/core/SvdTypes.cpp
    src/core/SvdParser.h
    src/core/SvdParser.cpp
    src/core/SvdLibrary.h
    src/core/SvdLibrary.cpp
    src/core/AppSettings.h
    src/core/AppSettings.cpp
    src/core/ResourcePaths.h
    src/core/ResourcePaths.cpp
    src/core/IProbe.h
    src/core/JLinkRunner.h
    src/core/JLinkRunner.cpp
    src/core/GdbRspClient.h
    src/core/GdbRspClient.cpp
    src/core/JLinkGdbProbe.h
    src/core/JLinkGdbProbe.cpp
    src/core/ProbeController.h
    src/core/ProbeController.cpp
    src/core/DeviceIdentifier.h
    src/core/DeviceIdentifier.cpp
    src/core/DeviceTreeScanner.h
    src/core/DeviceTreeScanner.cpp
    src/core/PeripheralPresence.h
    src/core/PeripheralPresence.cpp
    src/core/PartTable.h
    src/core/PartTable.cpp
    src/core/FaultStatus.h
    src/core/FaultStatus.cpp
    src/core/ElfFile.h
    src/core/ElfFile.cpp
    src/core/DwarfReader.h
    src/core/DwarfReader.cpp
    src/core/SymbolFile.h
    src/core/SymbolFile.cpp
    src/core/SnapshotReader.cpp
    src/core/ValuePoller.cpp
    src/core/JLinkDllApi.cpp
    src/core/JLinkDllProbe.cpp
    src/core/HssSampler.cpp
    src/core/Fft.cpp
    # KFR (third_party/kfr): spectrum analysis backend. GPL v2 -- see
    # third_party/kfr/LICENSE.txt. Only the DFT module is built; audio/io/dsp are not
    # used by this program and were removed to keep the tree small.
    third_party/kfr/src/dft/bitrev-impl-f32.cpp
    third_party/kfr/src/dft/bitrev-impl-f64.cpp
    third_party/kfr/src/dft/dft.cpp
    third_party/kfr/src/dft/dft-impl-f32.cpp
    third_party/kfr/src/dft/dft-impl-f64.cpp
    third_party/kfr/src/dft/fft-impl-f32.cpp
    third_party/kfr/src/dft/fft-impl-f64.cpp
    third_party/kfr/src/dft/ngfft-impl-f32.cpp
    third_party/kfr/src/dft/ngfft-impl-f64.cpp
    third_party/kfr/src/dft/version.cpp
    src/core/SampleHistory.cpp
    src/core/MemoryProbe.h
    src/core/MemoryProbe.cpp
    src/core/PeripheralRoles.h
    src/core/PeripheralRoles.cpp
    src/core/GpioCodec.h
    src/core/GpioCodec.cpp
    src/core/JLinkDeviceName.h
    src/core/JLinkDeviceName.cpp
    src/core/RegisterSession.h
    src/core/RegisterSession.cpp
)

set(APP_SOURCES
    src/app/main.cpp
    src/app/Cli.h
    src/app/Cli.cpp
    src/app/Render.cpp
    src/app/ProbeInfo.cpp
    src/app/Symbols.cpp
)

set(UI_SOURCES
    src/ui/Theme.h
    src/ui/Theme.cpp
    src/ui/BitFieldStrip.h
    src/ui/BitFieldStrip.cpp
    src/ui/ScopePage.h
    src/ui/ScopePage.cpp
    src/ui/GlPlotWidget.cpp
    src/ui/RegisterBlockView.h
    src/ui/RegisterBlockView.cpp
    src/ui/InspectorPanel.h
    src/ui/InspectorPanel.cpp
    src/ui/PeripheralTree.h
    src/ui/PeripheralTree.cpp
    src/ui/PortGridView.h
    src/ui/PortGridView.cpp
    src/ui/PackageView.h
    src/ui/PackageView.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
)

qt_add_executable(mcuRegDebugger WIN32
    ${CORE_SOURCES}
    ${APP_SOURCES}
    ${UI_SOURCES}
)

target_include_directories(mcuRegDebugger PRIVATE src)

# KFR ships as headers plus a few translation units, so it needs the include root and
# a C++20 compile (its templates require it).
target_include_directories(mcuRegDebugger PRIVATE third_party/kfr/include)
target_compile_features(mcuRegDebugger PRIVATE cxx_std_20)

target_link_libraries(mcuRegDebugger PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::OpenGL
    Qt6::OpenGLWidgets
)

target_compile_definitions(mcuRegDebugger PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    APP_NAME="MCU Register Debugger"
)

if(MSVC)
    target_compile_options(mcuRegDebugger PRIVATE /W4 /utf-8)
else()
    target_compile_options(mcuRegDebugger PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# Install / packaging scaffolding (installer generation is a later phase)
# ---------------------------------------------------------------------------
install(TARGETS mcuRegDebugger RUNTIME DESTINATION .)
install(DIRECTORY data/ DESTINATION data OPTIONAL)

set(CPACK_PACKAGE_NAME "McuRegDebugger")
set(CPACK_PACKAGE_VENDOR "MCU Tools")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "McuRegDebugger")
include(CPack)
