cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
endif()

project(VitaMania LANGUAGES C CXX)

option(VITAMANIA_NO_AVPLAYER "Build without Vita AvPlayer audio imports" OFF)

set(VITA_APP_NAME "VitaMania")
set(VITA_TITLEID "VTMN00002")
set(VITA_VERSION "01.16")

if(VITAMANIA_NO_AVPLAYER)
  set(VITA_APP_NAME "VitaMania Lite")
  set(VITA_TITLEID "VTMN00003")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include("$ENV{VITASDK}/share/vita.cmake")

set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1")

file(GLOB VITAMANIA_SKIN_FILES CONFIGURE_DEPENDS assets/skin/*)
set(VITAMANIA_SKIN_VPK_FILES)
foreach(SKIN_FILE ${VITAMANIA_SKIN_FILES})
  get_filename_component(SKIN_NAME ${SKIN_FILE} NAME)
  list(APPEND VITAMANIA_SKIN_VPK_FILES FILE ${SKIN_FILE} skin/${SKIN_NAME})
endforeach()

add_executable(${PROJECT_NAME}
  src/main.cpp
  third_party/stb/stb_vorbis.c
)

set_source_files_properties(third_party/stb/stb_vorbis.c PROPERTIES COMPILE_OPTIONS "-w")

target_compile_options(${PROJECT_NAME} PRIVATE
  -Wall
  -Wextra
  -Wno-psabi
)

if(VITAMANIA_NO_AVPLAYER)
  target_compile_definitions(${PROJECT_NAME} PRIVATE VITAMANIA_NO_AVPLAYER=1)
endif()

target_link_libraries(${PROJECT_NAME}
  vita2d
  SceDisplay_stub
  SceGxm_stub
  SceCtrl_stub
  ScePgf_stub
  ScePvf_stub
  SceCommonDialog_stub
  SceAppMgr_stub
  png
  jpeg
  freetype
  z
  m
)

if(NOT VITAMANIA_NO_AVPLAYER)
  target_link_libraries(${PROJECT_NAME}
    SceAudio_stub
    SceAudiodec_stub
    SceSysmodule_stub
    SceAvPlayer_stub
  )
endif()

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE assets/icon0.png sce_sys/icon0.png
  FILE assets/livearea/bg.png sce_sys/livearea/contents/bg.png
  FILE assets/livearea/startup.png sce_sys/livearea/contents/startup.png
  FILE assets/livearea/template.xml sce_sys/livearea/contents/template.xml
  ${VITAMANIA_SKIN_VPK_FILES}
)
