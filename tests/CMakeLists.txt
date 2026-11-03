add_executable(test_core test_core.c)
target_link_libraries(test_core PRIVATE tether_sim)
add_test(NAME core COMMAND test_core ${CMAKE_SOURCE_DIR}/models/vent-v1.tmlp)
