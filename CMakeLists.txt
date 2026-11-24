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
    src/core/JLinkInstall.cpp
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
    src/core/JLinkDllApi.cpp
    src/core/JLinkDllProbe.cpp
    src/core/HssSampler.cpp
    src/core/Fft.cpp
    src/core/ImageBlob.cpp
    src/core/FirmwareImage.cpp
    src/core/FirmwareBurner.cpp
    src/core/ZipArchive.cpp
    src/core/SimModel.cpp
    src/core/PythonModel.cpp
    src/core/FmuModel.cpp
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
    src/ui/BurnDialog.cpp
    src/ui/ChipPickerDialog.cpp
    src/ui/SimulationPage.cpp
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

# ---------------------------------------------------------------------------
# zlib: the FMU loader reads ZIP members itself instead of using Qt's private
# QZipReader (whose header is explicitly unsupported/removable).
#
# find_package(ZLIB) does not find this toolchain's zlib: mingw ships it inside its
# own sysroot, which is not on CMake's default search path even though `-lz` works.
# So try the normal route first and fall back to a manual find.
# ---------------------------------------------------------------------------
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    # mingw puts zlib in <prefix>/<target-triple>/{include,lib}, which CMake does not
    # search. Derive that prefix from the compiler we are actually using.
    # CMAKE_C_COMPILER is empty in a C++-only project, so fall back to the C++ one.
    set(_mrd_compiler "${CMAKE_C_COMPILER}")
    if(NOT _mrd_compiler)
        set(_mrd_compiler "${CMAKE_CXX_COMPILER}")
    endif()
    get_filename_component(_mrd_cc_bin "${_mrd_compiler}" DIRECTORY)
    get_filename_component(_mrd_toolchain "${_mrd_cc_bin}" DIRECTORY)
    file(GLOB _mrd_sysroots "${_mrd_toolchain}/*mingw32" "${_mrd_toolchain}/*w64-mingw32")
    message(STATUS "zlib search roots: ${_mrd_sysroots}")

    find_path(ZLIB_INCLUDE_DIR zlib.h
        HINTS ${_mrd_sysroots} ${_mrd_toolchain}
        PATH_SUFFIXES include)
    find_library(ZLIB_LIBRARY NAMES z zlib zlibstatic
        HINTS ${_mrd_sysroots} ${_mrd_toolchain}
        PATH_SUFFIXES lib)

    if(ZLIB_INCLUDE_DIR AND ZLIB_LIBRARY)
        add_library(ZLIB::ZLIB UNKNOWN IMPORTED)
        set_target_properties(ZLIB::ZLIB PROPERTIES
            IMPORTED_LOCATION "${ZLIB_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ZLIB_INCLUDE_DIR}")
        message(STATUS "zlib (toolchain): ${ZLIB_LIBRARY}")
    endif()
endif()
if(NOT TARGET ZLIB::ZLIB)
    message(FATAL_ERROR
        "zlib not found. It is needed to read .fmu (ZIP) packages. "
        "Install zlib or point ZLIB_INCLUDE_DIR/ZLIB_LIBRARY at it.")
endif()

target_link_libraries(mcuRegDebugger PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    ZLIB::ZLIB
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
