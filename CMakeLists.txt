cmake_minimum_required(VERSION 3.16)

project(libev VERSION 4.33 LANGUAGES C)

set(LIBEV_VERSION_CURRENT 4)
set(LIBEV_VERSION_AGE 0)
set(LIBEV_VERSION_REVISION 0)

include(CheckIncludeFile)
include(CheckSymbolExists)
include(CheckCSourceCompiles)

option(BUILD_SHARED_LIBS "Build libev as a shared library" ON)
option(BUILD_STATIC_LIBS "Build libev as a static library" ON)
option(BUILD_TESTING "Build tests" ON)
option(SKIP_INSTALL_HEADERS "Do not install header files" OFF)

check_include_file(sys/inotify.h  HAVE_SYS_INOTIFY_H)
check_include_file(sys/epoll.h    HAVE_SYS_EPOLL_H)
check_include_file(sys/event.h    HAVE_SYS_EVENT_H)
check_include_file(port.h         HAVE_PORT_H)
check_include_file(poll.h         HAVE_POLL_H)
check_include_file(sys/select.h   HAVE_SYS_SELECT_H)
check_include_file(sys/eventfd.h  HAVE_SYS_EVENTFD_H)
check_include_file(sys/signalfd.h HAVE_SYS_SIGNALFD_H)
check_include_file(sys/timerfd.h  HAVE_SYS_TIMERFD_H)
check_include_file(linux/fs.h     HAVE_LINUX_FS_H)
check_include_file(linux/aio_abi.h HAVE_LINUX_AIO_ABI_H)

if(HAVE_SYS_INOTIFY_H)
    check_symbol_exists(inotify_init sys/inotify.h HAVE_INOTIFY_INIT)
endif()
if(HAVE_SYS_EPOLL_H)
    check_symbol_exists(epoll_ctl sys/epoll.h HAVE_EPOLL_CTL)
endif()
if(HAVE_SYS_EVENT_H)
    check_symbol_exists(kqueue sys/event.h HAVE_KQUEUE)
endif()
if(HAVE_PORT_H)
    check_symbol_exists(port_create port.h HAVE_PORT_CREATE)
endif()
if(HAVE_POLL_H)
    check_symbol_exists(poll poll.h HAVE_POLL)
endif()
if(HAVE_SYS_SELECT_H)
    check_symbol_exists(select sys/select.h HAVE_SELECT)
endif()
if(HAVE_SYS_EVENTFD_H)
    check_symbol_exists(eventfd sys/eventfd.h HAVE_EVENTFD)
endif()
if(HAVE_SYS_SIGNALFD_H)
    check_symbol_exists(signalfd sys/signalfd.h HAVE_SIGNALFD)
endif()

# __kernel_rwf_t is a typedef, not a linkable symbol, so it must be
# detected by compiling rather than check_symbol_exists
check_c_source_compiles("
    #include <linux/fs.h>
    int main(void) { __kernel_rwf_t x = 0; (void)x; return 0; }
" HAVE_KERNEL_RWF_T)

check_symbol_exists(clock_gettime time.h HAVE_CLOCK_GETTIME)
if(NOT HAVE_CLOCK_GETTIME)
    if(CMAKE_SYSTEM_NAME STREQUAL Linux)
        message(STATUS "Checking for clock_gettime syscall")
        check_c_source_compiles("
            #include <unistd.h>
            #include <sys/syscall.h>
            #include <time.h>
            int main(int argc, char *argv[])
            {
                (void) argc;
                (void) argv;
                struct timespec ts;
                int status = syscall (SYS_clock_gettime, CLOCK_REALTIME, &ts);
                return status;
            }
            "
            HAVE_CLOCK_SYSCALL
        )
    endif()

    if(NOT AVOID_LIBRT AND NOT HAVE_CLOCK_SYSCALL)
        set(CMAKE_REQUIRED_LIBRARIES rt)
        check_symbol_exists(clock_gettime time.h _HAVE_CLOCK_GETTIME_2)
        unset(CMAKE_REQUIRED_LIBRARIES)
        if(_HAVE_CLOCK_GETTIME_2)
            set(LIBRT_NAME rt CACHE STRING "Name of the realtime library to explicitly link in")
            set(HAVE_CLOCK_GETTIME ${_HAVE_CLOCK_GETTIME_2})
            unset(_HAVE_CLOCK_GETTIME_2)
        endif()
    endif()
endif()

check_symbol_exists(nanosleep time.h HAVE_NANOSLEEP)
if(NOT AVOID_LIBRT AND NOT HAVE_NANOSLEEP)
    set(CMAKE_REQUIRED_LIBRARIES rt)
    check_symbol_exists(nanosleep time.h _HAVE_NANOSLEEP_2)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(_HAVE_NANOSLEEP_2)
        set(LIBRT_NAME rt CACHE STRING "Name of the realtime library to explicitly link in")
        set(HAVE_NANOSLEEP ${_HAVE_NANOSLEEP_2})
        unset(_HAVE_NANOSLEEP_2)
    endif()
endif()

check_symbol_exists(floor math.h HAVE_FLOOR)
if(NOT AVOID_LIBM AND NOT HAVE_FLOOR)
    set(CMAKE_REQUIRED_LIBRARIES m)
    check_symbol_exists(floor math.h _HAVE_FLOOR_2)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(_HAVE_FLOOR_2)
        set(LIBM_NAME m CACHE STRING "Name of the math library to explicitly link in")
        set(HAVE_FLOOR ${_HAVE_FLOOR_2})
        unset(_HAVE_FLOOR_2)
    endif()
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config.h.cmakein
    ${CMAKE_CURRENT_BINARY_DIR}/config.h
    @ONLY
)

add_library(ev_obj OBJECT src/ev.c src/event.c)

set_target_properties(ev_obj PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 99
)

target_include_directories(ev_obj
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/unix
            ${CMAKE_CURRENT_SOURCE_DIR}/src/win
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:include>
)

if(BUILD_STATIC_LIBS)
    add_library(ev_static STATIC $<TARGET_OBJECTS:ev_obj>)
    set_target_properties(ev_static PROPERTIES
        OUTPUT_NAME ev
    )
    if(LIBRT_NAME OR LIBM_NAME)
        target_link_libraries(ev_static PUBLIC ${LIBRT_NAME} ${LIBM_NAME})
    endif()
    install(TARGETS ev_static ARCHIVE DESTINATION lib)
endif()

if(BUILD_SHARED_LIBS)
    add_library(ev SHARED $<TARGET_OBJECTS:ev_obj>)
    set_target_properties(ev PROPERTIES
        OUTPUT_NAME ev
        VERSION   ${LIBEV_VERSION_CURRENT}.${LIBEV_VERSION_AGE}.${LIBEV_VERSION_REVISION}
        SOVERSION ${LIBEV_VERSION_CURRENT}
    )
    if(LIBRT_NAME OR LIBM_NAME)
        target_link_libraries(ev PUBLIC ${LIBRT_NAME} ${LIBM_NAME})
    endif()
    install(TARGETS ev LIBRARY DESTINATION lib)
endif()

if((BUILD_SHARED_LIBS OR BUILD_STATIC_LIBS) AND NOT SKIP_INSTALL_HEADERS)
    install(FILES include/ev.h include/ev++.h include/event.h DESTINATION include)
    install(FILES docs/ev.3 DESTINATION share/man/man3)
endif()

configure_file(libev.pc.in ${CMAKE_CURRENT_BINARY_DIR}/libev.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libev.pc DESTINATION lib/pkgconfig)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()
