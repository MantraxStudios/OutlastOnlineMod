cmake_minimum_required(VERSION 3.20)
project(OutlastOnline CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # CRT estático: sin dependencias extra

# x64 -> Binaries/Win64 (DLL + launcher + herramientas); Win32 -> Binaries/Win32 (solo la DLL)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(OL_X64 ON)
    set(OL_BINDIR "Win64")
else()
    set(OL_X64 OFF)
    set(OL_BINDIR "Win32")
endif()

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
add_compile_options(/W3 /EHa /utf-8)

# DLL del mod (se instala como Binaries/Win64/dinput8.dll)
add_library(dinput8 SHARED
    src/dllmain.cpp
    src/mod.cpp
    src/ue3.cpp
    src/net.cpp
    src/hook.cpp
    src/log.cpp
    src/voice.cpp
    src/dinput8.def)
target_link_libraries(dinput8 PRIVATE ws2_32 advapi32 user32 winmm)

# Jugador simulado para probar sin un segundo PC (solo x64; más abajo)
add_executable(fakepeer tools/fakepeer.cpp)
target_include_directories(fakepeer PRIVATE src)
target_link_libraries(fakepeer PRIVATE ws2_32)

set(OUTLAST_BIN "${CMAKE_CURRENT_SOURCE_DIR}/../Binaries/${OL_BINDIR}" CACHE PATH "Carpeta Binaries/<arch> del juego")
add_custom_target(install_mod
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:dinput8> "${OUTLAST_BIN}/dinput8.dll"
    DEPENDS dinput8
    COMMENT "Copiando dinput8.dll a ${OUTLAST_BIN}")

if(NOT OL_X64)
    return()  # en 32 bits solo se compila la DLL
endif()

# DLL de 32 bits (compilada antes en build32/) para embeberla en el launcher
set(OUTLAST_DLL32 "${CMAKE_CURRENT_SOURCE_DIR}/build32/Release/dinput8.dll" CACHE FILEPATH "dinput8.dll de 32 bits")

# Launcher: busca el juego, instala la DLL (embebida como recurso) y lo lanza
if(EXISTS "${OUTLAST_DLL32}")
    set(OL_RC32 "2 RCDATA \"${OUTLAST_DLL32}\"\n")
else()
    message(WARNING "No se encontro la DLL de 32 bits (${OUTLAST_DLL32}); el launcher solo instalara 64 bits")
    set(OL_RC32 "")
endif()
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/launcher_res_$<CONFIG>.rc"
     CONTENT "1 RCDATA \"$<TARGET_FILE:dinput8>\"\n${OL_RC32}")
add_executable(OutlastOnlineLauncher WIN32 tools/launcher.cpp "${CMAKE_BINARY_DIR}/launcher_res_$<CONFIG>.rc")
add_dependencies(OutlastOnlineLauncher dinput8)
target_link_libraries(OutlastOnlineLauncher PRIVATE shell32 ole32 comctl32 advapi32 user32 gdi32)

# Zip de distribución: cmake --build build --config Release --target package_zip
set(DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist")
add_custom_target(package_zip
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${DIST_DIR}/OutlastOnline"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/OutlastOnline/manual/Win64" "${DIST_DIR}/OutlastOnline/manual/Win32" "${DIST_DIR}/OutlastOnline/tools"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:OutlastOnlineLauncher> "${DIST_DIR}/OutlastOnline/"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:dinput8> "${DIST_DIR}/OutlastOnline/manual/Win64/dinput8.dll"
    COMMAND ${CMAKE_COMMAND} -E copy "${OUTLAST_DLL32}" "${DIST_DIR}/OutlastOnline/manual/Win32/dinput8.dll"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:fakepeer> "${DIST_DIR}/OutlastOnline/tools/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LEEME.txt" "${DIST_DIR}/OutlastOnline/"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${DIST_DIR}/OutlastOnline.zip"
    COMMAND ${CMAKE_COMMAND} -E chdir "${DIST_DIR}" ${CMAKE_COMMAND} -E tar cf OutlastOnline.zip --format=zip OutlastOnline
    DEPENDS OutlastOnlineLauncher dinput8 fakepeer
    COMMENT "Generando dist/OutlastOnline.zip")

# Prueba del audio (micrófono + codificación) sin el juego
add_executable(voicetest tools/voicetest.cpp src/voice.cpp src/log.cpp)
target_include_directories(voicetest PRIVATE src)
target_link_libraries(voicetest PRIVATE winmm)
