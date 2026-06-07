cmake_minimum_required(VERSION 3.19)
project(ctm_bridge_test VERSION 0.1.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

# --- Vendored ENet (github.com/lsalzman/enet v1.3.18) -------------------------
# Additive ENet/UDP transport. Built as a small static lib and linked into the
# bridge app. We replicate ENet's own upstream feature probes (instead of using
# its CMakeLists) so unix.c compiles correctly under the buildroot toolchain:
# socklen_t, fcntl, poll, getaddrinfo, inet_pton/ntop, msg_flags.
set(ENET_ROOT "${CMAKE_SOURCE_DIR}/third_party/enet")
if(NOT EXISTS "${ENET_ROOT}/include/enet/enet.h")
  message(FATAL_ERROR "ENet source not found at ${ENET_ROOT}. Put ENet under third_party/enet.")
endif()

include(CheckFunctionExists)
include(CheckStructHasMember)
include(CheckTypeSize)
check_function_exists("fcntl" HAS_FCNTL)
check_function_exists("poll" HAS_POLL)
check_function_exists("getaddrinfo" HAS_GETADDRINFO)
check_function_exists("getnameinfo" HAS_GETNAMEINFO)
check_function_exists("gethostbyname_r" HAS_GETHOSTBYNAME_R)
check_function_exists("gethostbyaddr_r" HAS_GETHOSTBYADDR_R)
check_function_exists("inet_pton" HAS_INET_PTON)
check_function_exists("inet_ntop" HAS_INET_NTOP)
check_struct_has_member("struct msghdr" "msg_flags" "sys/types.h;sys/socket.h" HAS_MSGHDR_FLAGS)
check_type_size("socklen_t" HAS_SOCKLEN_T BUILTIN_TYPES_ONLY)

add_library(ctm_enet STATIC
  "${ENET_ROOT}/callbacks.c"
  "${ENET_ROOT}/compress.c"
  "${ENET_ROOT}/host.c"
  "${ENET_ROOT}/list.c"
  "${ENET_ROOT}/packet.c"
  "${ENET_ROOT}/peer.c"
  "${ENET_ROOT}/protocol.c"
  "${ENET_ROOT}/unix.c"
  "${ENET_ROOT}/win32.c"
)
target_include_directories(ctm_enet PUBLIC "${ENET_ROOT}/include")
if(HAS_FCNTL)
  target_compile_definitions(ctm_enet PRIVATE HAS_FCNTL=1)
endif()
if(HAS_POLL)
  target_compile_definitions(ctm_enet PRIVATE HAS_POLL=1)
endif()
if(HAS_GETNAMEINFO)
  target_compile_definitions(ctm_enet PRIVATE HAS_GETNAMEINFO=1)
endif()
if(HAS_GETADDRINFO)
  target_compile_definitions(ctm_enet PRIVATE HAS_GETADDRINFO=1)
endif()
if(HAS_GETHOSTBYNAME_R)
  target_compile_definitions(ctm_enet PRIVATE HAS_GETHOSTBYNAME_R=1)
endif()
if(HAS_GETHOSTBYADDR_R)
  target_compile_definitions(ctm_enet PRIVATE HAS_GETHOSTBYADDR_R=1)
endif()
if(HAS_INET_PTON)
  target_compile_definitions(ctm_enet PRIVATE HAS_INET_PTON=1)
endif()
if(HAS_INET_NTOP)
  target_compile_definitions(ctm_enet PRIVATE HAS_INET_NTOP=1)
endif()
if(HAS_MSGHDR_FLAGS)
  target_compile_definitions(ctm_enet PRIVATE HAS_MSGHDR_FLAGS=1)
endif()
if(HAS_SOCKLEN_T)
  target_compile_definitions(ctm_enet PRIVATE HAS_SOCKLEN_T=1)
endif()

set(LVGL_ROOT "${CMAKE_SOURCE_DIR}/third_party/lvgl")
if(NOT EXISTS "${LVGL_ROOT}/src/lvgl.h")
  message(FATAL_ERROR "LVGL source not found at ${LVGL_ROOT}. Put LVGL under third_party/lvgl.")
endif()

set(CTM_PREBUILT_LVGL "" CACHE FILEPATH "Optional prebuilt libctm_lvgl.a to link instead of compiling LVGL")
if(CTM_PREBUILT_LVGL)
  if(NOT EXISTS "${CTM_PREBUILT_LVGL}")
    message(FATAL_ERROR "CTM_PREBUILT_LVGL does not exist: ${CTM_PREBUILT_LVGL}")
  endif()
  add_library(ctm_lvgl STATIC IMPORTED GLOBAL)
  set_target_properties(ctm_lvgl PROPERTIES IMPORTED_LOCATION "${CTM_PREBUILT_LVGL}")
  target_include_directories(ctm_lvgl INTERFACE
    "${LVGL_ROOT}"
    "${LVGL_ROOT}/src"
    "${CMAKE_SOURCE_DIR}/src/lvgl"
    ${SDL2_INCLUDE_DIRS}
  )
  target_compile_definitions(ctm_lvgl INTERFACE LV_CONF_INCLUDE_SIMPLE)
else()
  file(GLOB_RECURSE CTM_LVGL_SOURCES CONFIGURE_DEPENDS "${LVGL_ROOT}/src/*.c")
  add_library(ctm_lvgl STATIC ${CTM_LVGL_SOURCES})
  target_include_directories(ctm_lvgl PUBLIC
    "${LVGL_ROOT}"
    "${LVGL_ROOT}/src"
    "${CMAKE_SOURCE_DIR}/src/lvgl"
    ${SDL2_INCLUDE_DIRS}
  )
  target_compile_definitions(ctm_lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
  target_compile_options(ctm_lvgl PRIVATE ${SDL2_CFLAGS_OTHER})
endif()

# --- Headless bridge core (no LVGL/SDL) ---------------------------------------
# Shared state + helpers, device enumeration/classification, agent/session/plug
# orchestration, controllers, monitor and the CTMB transport. Built as a static
# lib so the same core can link into a host app (moonlight-tv) that owns its own
# LVGL/SDL. NOTE: the ENet sources are still compiled in here for now (runtime
# defaults to TCP with a NULL enet client); the ENet strip for the moonlight
# embed is a later step.
add_library(ctmcore STATIC
  src/app/ctm_state.c
  src/app/ui_devices.c
  src/app/ui_bridge.c
  src/shared/ctm_hid.c
  src/shared/ctm_transport.c
  src/shared/enet_transport.c
  src/controllers/controller_common.c
  src/controllers/controller_ds5.c
  src/controllers/controller_ds4.c
  src/controllers/controller_xbox.c
  src/controllers/controller_steam_puck.c
  src/controllers/controller_generic.c
  src/monitor/ctm_monitor.c
)
target_include_directories(ctmcore PUBLIC
  src
  src/app
  src/shared
  src/controllers
  src/monitor
  "${ENET_ROOT}/include"
)
target_link_libraries(ctmcore PUBLIC pthread m ctm_enet)

add_executable(ctm_bridge_lvgl_ui
  src/app/ui_app.c
  src/app/ui_common.c
  src/app/ui_window_common.c
  src/app/ui_window_generic.c
  src/app/ui_window_ds4.c
  src/app/ui_window_ds5.c
  src/app/ui_window_xbox.c
  src/app/ui_window_steam_puck.c
)

target_include_directories(ctm_bridge_lvgl_ui PRIVATE
  src
  src/app
  src/shared
  src/controllers
  src/monitor
  ${SDL2_INCLUDE_DIRS}
  "${LVGL_ROOT}"
  "${LVGL_ROOT}/src"
  "${ENET_ROOT}/include"
)
target_compile_options(ctm_bridge_lvgl_ui PRIVATE
  ${SDL2_CFLAGS_OTHER}
)
target_link_directories(ctm_bridge_lvgl_ui PRIVATE
  ${SDL2_LIBRARY_DIRS}
)
target_link_libraries(ctm_bridge_lvgl_ui PRIVATE
  ctmcore
  ctm_lvgl
  ctm_enet
  ${SDL2_LIBRARIES}
)

install(FILES deploy/webos/appinfo.json DESTINATION .)
install(FILES "${CMAKE_SOURCE_DIR}/icon.png" DESTINATION . OPTIONAL)
install(FILES "${CMAKE_SOURCE_DIR}/icon_large.png" DESTINATION . OPTIONAL)
install(FILES "${CMAKE_SOURCE_DIR}/assets/font.ttf" DESTINATION assets)
install(TARGETS ctm_bridge_lvgl_ui DESTINATION bin)

set(CPACK_PACKAGE_NAME "com.local.ctmbridge")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}_${PROJECT_VERSION}_arm")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_SOURCE_DIR}/dist")
set(CPACK_GENERATOR "External")
set(CPACK_EXTERNAL_ENABLE_STAGING TRUE)
set(CPACK_MONOLITHIC_INSTALL TRUE)
configure_file(cmake/AresPackage.cmake.in AresPackage.cmake @ONLY)
set(CPACK_EXTERNAL_PACKAGE_SCRIPT "${CMAKE_BINARY_DIR}/AresPackage.cmake")

include(CPack)
