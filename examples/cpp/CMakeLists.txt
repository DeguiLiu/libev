# C++17 examples over the libev C API. Gated by BUILD_EXAMPLES in the parent.
# SPDX-License-Identifier: MIT

find_package(Threads REQUIRED)

foreach(example IN ITEMS protocol_hsm node_manager async_proxy lwip_echo fs_hsm hsm_echo uart_hsm uart_ring_hsm)
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE ev_static)
    target_include_directories(${example} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    set_target_properties(${example} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_compile_options(${example} PRIVATE -fno-exceptions)
    add_test(NAME ${example} COMMAND ${example})
endforeach()

foreach(example IN ITEMS async_proxy lwip_echo fs_hsm hsm_echo uart_ring_hsm)
    target_link_libraries(${example} PRIVATE Threads::Threads)
endforeach()
