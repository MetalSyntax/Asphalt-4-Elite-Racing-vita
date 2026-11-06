cmake_minimum_required(VERSION 3.14)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum policy version for legacy SDK scripts")

if (NOT DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Define VITASDK (ej. export VITASDK=$HOME/vitasdk).")
endif ()
set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Toolchain file")

project(asphalt4eliteracing C ASM)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Asphalt 4 Elite Racing")
set(VITA_TITLEID "PSVAER004")
set(VITA_VPKNAME "asphalt4eliteracing")
set(VITA_VERSION "01.00")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")
set(PSVITAIP "192.168.3.15" CACHE STRING "PSVita IP")

set(DATA_PATH "ux0:data/asphalt4eliteracing/" CACHE STRING "Datos del juego (con / final)")
add_definitions(-DDATA_PATH="${DATA_PATH}")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_definitions(-DDEBUG_LOADER)
endif()

## vitaGL vendorizada con compilación en softfp (OBLIGATORIO para evitar pantalla negra en PS Vita)
set(VITAGL_VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/vitaGL)
# No DRAW_SPEEDHACK: the GPU would read client vertex arrays from game memory at scene end; Lib3D
# reuses its CPU vertex buffers inside a frame (PoP WW logs 027-030 showed the same pattern).
set(VITAGL_MAKE_FLAGS_STR "SOFTFP_ABI=1 NO_DEBUG=1 HAVE_SHADER_CACHE=1 NO_SPLASHSCREEN=1 HAVE_GLSL_UBOS=1")
# vitasdk-softfp (--with-float=softfp) ships softfp libm/libmathneon (PoP WW 55a1dbf).
execute_process(COMMAND ${CMAKE_C_COMPILER} -v ERROR_VARIABLE VITA_GCC_V OUTPUT_QUIET)
if (VITA_GCC_V MATCHES "with-float=softfp")
  message(STATUS "vitasdk-softfp toolchain: softfp libs")
  add_definitions(-DVITASDK_SOFTFP_LIBS)
  set(VITAGL_MAKE_FLAGS_STR "${VITAGL_MAKE_FLAGS_STR} SOFTFP_LIBS=1")
endif()
separate_arguments(VITAGL_MAKE_FLAGS UNIX_COMMAND "${VITAGL_MAKE_FLAGS_STR}")
# Out-of-tree build; rsync skips macOS AppleDouble ._* files on exFAT (vitaGL globs source/*.c).
string(MD5 VITAGL_FLAGS_HASH "${VITAGL_MAKE_FLAGS_STR}")
string(SUBSTRING ${VITAGL_FLAGS_HASH} 0 8 VITAGL_FLAGS_HASH)
set(VITAGL_BUILD_DIR ${CMAKE_BINARY_DIR}/vitaGL-${VITAGL_FLAGS_HASH})
cmake_host_system_information(RESULT VITAGL_JOBS QUERY NUMBER_OF_LOGICAL_CORES)
include(ExternalProject)
ExternalProject_Add(
  vitaGL_vendored
  SOURCE_DIR ${VITAGL_VENDOR_DIR}
  BINARY_DIR ${VITAGL_BUILD_DIR}
  CONFIGURE_COMMAND ""
  BUILD_COMMAND rsync -a --delete --exclude=*.o --exclude=*.a --exclude=._* --exclude=.DS_Store
                ${VITAGL_VENDOR_DIR}/ ${VITAGL_BUILD_DIR}/
        COMMAND make -j${VITAGL_JOBS} ${VITAGL_MAKE_FLAGS}
  BUILD_ALWAYS 1
  BUILD_BYPRODUCTS ${VITAGL_BUILD_DIR}/libvitaGL.a
  INSTALL_COMMAND ""
)
add_library(vitaGL_vendored_lib STATIC IMPORTED)
set_target_properties(vitaGL_vendored_lib PROPERTIES IMPORTED_LOCATION ${VITAGL_BUILD_DIR}/libvitaGL.a)
add_dependencies(vitaGL_vendored_lib vitaGL_vendored)

include_directories(
  ${VITAGL_VENDOR_DIR}/source
  $ENV{VITASDK}/arm-vita-eabi/include/freetype2
  ${CMAKE_CURRENT_SOURCE_DIR}/source
)

# -ffixed-r9: el código de iOS trata r9 como temporal sin guardarlo. Si el loader usara r9,
# cualquier llamada a/desde el juego lo podría corromper.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -O2 -g -ffixed-r9 -mfloat-abi=softfp -DSKIP_SPLASHSCREEN -Wall -Wno-unused-function -D_GNU_SOURCE")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -ffixed-r9 -mfloat-abi=softfp")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--no-warn-mismatch")

# Explicit list (never a glob): the exFAT disk is full of AppleDouble ._*.c files.
add_executable(${CMAKE_PROJECT_NAME}
  source/main.c
  source/log.c
  source/macho.c
  source/asm.S
  source/imports.c
  source/darwin.c
  source/darwin_printf.c
  source/objc.c
  source/gles.c
  source/vitagl_no_splash.c
  source/coreaudio.c
  source/input.c
  source/cgbi.c
  source/patch.c
  source/cxx.c
  source/uikit.c
  source/video.c
)

target_link_libraries(${CMAKE_PROJECT_NAME}
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  vitaGL_vendored_lib
  openal
  vitashark
  SceShaccCgExt
  stdc++
  taihen_stub
  SceShaccCg_stub
  mathneon
  freetype
  png16
  z
  bz2
  m
  kubridge_stub
  SceAppMgr_stub
  SceCtrl_stub
  SceTouch_stub
  SceDisplay_stub
  SceSysmodule_stub
  ScePower_stub
  SceKernelDmacMgr_stub
  SceGxm_stub
  SceCommonDialog_stub
  SceAppUtil_stub
  SceAudio_stub
  SceAudioIn_stub
  SceAvPlayer_stub
  SceVideodec_stub
  -Wl,--start-group
  avcodec
  swresample
  avutil
  mp3lame
  m
  -Wl,--end-group
  SceIofilemgr_stub
  SceIme_stub
)

vita_create_self(eboot.bin ${CMAKE_PROJECT_NAME} UNSAFE NOASLR)
vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/extras/livearea/icon0.png sce_sys/icon0.png
  FILE ${CMAKE_SOURCE_DIR}/extras/livearea/pic0.png sce_sys/pic0.png
  FILE ${CMAKE_SOURCE_DIR}/extras/livearea/bg0.png sce_sys/livearea/contents/bg0.png
  FILE ${CMAKE_SOURCE_DIR}/extras/livearea/startup.png sce_sys/livearea/contents/startup.png
  FILE ${CMAKE_SOURCE_DIR}/extras/livearea/template.xml sce_sys/livearea/contents/template.xml
)
