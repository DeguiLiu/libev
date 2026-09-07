# test/CMakeLists.txt
add_executable(test_smoke test_smoke.c)
target_include_directories(test_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_smoke PRIVATE ev_static)
add_test(NAME smoke COMMAND test_smoke)
