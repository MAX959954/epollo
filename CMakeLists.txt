cmake_minimum_required(VERSION 3.25)
project(epollo VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(EPOLLO_WERROR "Treat warnings as errors" ON)
option(EPOLLO_SANITIZE "Enable ASan + UBSan (GCC/Clang only)" OFF)

# --- Предупреждения: INTERFACE-библиотека, сама ничего не компилирует,
#     только раздаёт флаги тем, кто на неё слинкован.
add_library(epollo_warnings INTERFACE)
if(MSVC)
    target_compile_options(epollo_warnings INTERFACE /W4 /permissive-)
    if(EPOLLO_WERROR)
        target_compile_options(epollo_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(epollo_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
    if(EPOLLO_WERROR)
        target_compile_options(epollo_warnings INTERFACE -Werror)
    endif()
endif()

# --- Санитайзеры: глобально, чтобы и код, и тесты были инструментированы одинаково.
#     Должны стоять до объявления целей: add_compile_options влияет только на цели ниже.
if(EPOLLO_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# --- Ядро
add_library(epollo_core STATIC src/version.cpp)
target_include_directories(epollo_core PUBLIC include)
target_link_libraries(epollo_core PRIVATE epollo_warnings)

# --- Приложение
add_executable(epollo apps/epollo/main.cpp)
target_link_libraries(epollo PRIVATE epollo_core epollo_warnings)

# --- Тесты
enable_testing()
find_package(GTest CONFIG REQUIRED)

add_executable(epollo_tests tests/version_test.cpp)
target_link_libraries(epollo_tests PRIVATE epollo_core epollo_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(epollo_tests)
