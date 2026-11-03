add_library(tether_sim STATIC sim.c)
target_link_libraries(tether_sim PUBLIC tether_core)
target_include_directories(tether_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(tether-datagen datagen.c)
target_link_libraries(tether-datagen PRIVATE tether_core)

add_executable(tether-sim main.c)
target_link_libraries(tether-sim PRIVATE tether_sim)
