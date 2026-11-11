# The Tether processes for QNX 8.0 (built only with cmake/qnx800.cmake).
# Strict -std=c11 hides the POSIX and QNX declarations these programs need.
add_compile_definitions(_QNX_SOURCE)

add_library(tether_qcommon STATIC common/qcommon.c)
target_include_directories(tether_qcommon PUBLIC common)
target_link_libraries(tether_qcommon PUBLIC tether_core slog2)

function(tether_process name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE tether_qcommon)
  install(TARGETS ${name} RUNTIME DESTINATION bin)
endfunction()

tether_process(tether-hw hw/tether-hw.c)
tether_process(tether-ctl ctl/tether-ctl.c)
tether_process(tether-arbiter arbiter/tether-arbiter.c)
tether_process(tether-ai ai/tether-ai.c)
tether_process(tetherd tetherd/tetherd.c)
tether_process(tether-bridge bridge/tether-bridge.c)
target_link_libraries(tether-bridge PRIVATE socket)
