cmake_minimum_required(VERSION 3.10.0)
project(AndroidNative VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include(CheckSymbolExists)

check_symbol_exists (kqueue "sys/event.h" HAVE_KQUEUE)
check_symbol_exists (epoll_create "sys/epoll.h" HAVE_EPOLL)
check_symbol_exists (eventfd "sys/eventfd.h" HAVE_EVENTFD)

configure_file(
   ${PROJECT_SOURCE_DIR}/config.h.in
   ${PROJECT_BINARY_DIR}/include/config.h
   @ONLY
)

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR}/include)

add_compile_options(-fexperimental-library)

set(${PROJECT_NAME}_SOURCES 
    native/android/looper.cpp
    libutils/Looper.cpp
    libutils/Timers.cpp
    libutils/VectorImpl.cpp
    libutils/SharedBuffer.cpp
    libutils/StrongPointer.cpp
    libutils/RefBase.cpp
)
if (NOT HAVE_EVENTFD)
    set(${PROJECT_NAME}_SOURCES ${${PROJECT_NAME}_SOURCES} macport/eventfd.c)
endif()

add_library(${PROJECT_NAME} ${${PROJECT_NAME}_SOURCES})

set(${PROJECT_NAME}_INCLUDES
        ${PROJECT_SOURCE_DIR}/native/include
        ${PROJECT_SOURCE_DIR}/liblog/include
        ${PROJECT_SOURCE_DIR}/libutils/include
)
if (NOT HAVE_EVENTFD)
    set(${PROJECT_NAME}_INCLUDES ${${PROJECT_NAME}_INCLUDES} ${PROJECT_SOURCE_DIR}/macport/include)
endif()

target_include_directories(${PROJECT_NAME} SYSTEM PUBLIC ${${PROJECT_NAME}_INCLUDES})

target_precompile_headers(${PROJECT_NAME}
    PUBLIC
        ${PROJECT_BINARY_DIR}/include/config.h
)

target_compile_definitions(${PROJECT_NAME}
    PRIVATE
        _DEBUG=1
        DEBUG_POLL_AND_WAKE=1
        DEBUG_CALLBACKS=1
    )

target_compile_options(${PROJECT_NAME}
    PRIVATE
        -fsanitize=address
        -fno-omit-frame-pointer
        -fsanitize-recover=address
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        -fsanitize=address
)