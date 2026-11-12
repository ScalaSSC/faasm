include(FindGit)
find_package(Git)
include (ExternalProject)
include (FetchContent)

include_directories(${CMAKE_INSTALL_PREFIX}/include)

# Find conan-generated package descriptions
list(PREPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_BINARY_DIR})
list(PREPEND CMAKE_PREFIX_PATH ${CMAKE_CURRENT_BINARY_DIR})

find_program(CONAN_CMD conan)
if(NOT CONAN_CMD)
    message(FATAL_ERROR "Please install Conan 2.x and add it to your PATH.")
endif()

set(CONANFILE ${CMAKE_CURRENT_LIST_DIR}/../conanfile.txt)
set(CONAN_PROFILE_HOST ${CMAKE_CURRENT_LIST_DIR}/../faabric/conan-profile.txt)
set(CONAN_PROFILE_BUILD ${CONAN_PROFILE_HOST}) 
set(CONAN_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR})

execute_process(COMMAND ${CONAN_CMD} remote add conancenter https://center2.conan.io --force)

execute_process(
    COMMAND ${CONAN_CMD} install ${CONANFILE} 
    --output-folder=${CONAN_OUTPUT_DIR} 
    --build=missing 
    --profile:host=${CONAN_PROFILE_HOST}
    --profile:build=${CONAN_PROFILE_BUILD}
    -s build_type=${CMAKE_BUILD_TYPE} 
    -s compiler.cppstd=20
    RESULT_VARIABLE CONAN_RESULT
)

if(NOT CONAN_RESULT EQUAL 0)
    message(FATAL_ERROR "Conan install failed with exit code: ${CONAN_RESULT}")
endif()

include(${CMAKE_CURRENT_BINARY_DIR}/conan_toolchain.cmake)

find_package(Catch2 REQUIRED)
find_package(cppcodec REQUIRED)
find_package(cpprestsdk REQUIRED)
find_package(jwt-cpp REQUIRED)
find_package(picojson REQUIRED)
find_package(RapidJSON REQUIRED)

# 22/12/2021 - WARNING: we don't install AWS through Conan as the recipe proved
# very unstable and failed frequently.

# There are some AWS docs on using the cpp sdk as an external project:
# https://github.com/aws/aws-sdk-cpp/blob/main/Docs/CMake_External_Project.md
# but they don't specify how to link the libraries, which required adding an
# extra couple of CMake targets.
set(AWS_CORE_LIBRARY ${CMAKE_INSTALL_PREFIX}/lib/libaws-cpp-sdk-core.so)
set(AWS_S3_LIBRARY ${CMAKE_INSTALL_PREFIX}/lib/libaws-cpp-sdk-s3.so)
ExternalProject_Add(aws_ext
    GIT_REPOSITORY   "https://github.com/aws/aws-sdk-cpp.git"
    GIT_TAG          "a47c163630a4d4e62cd3c42e9c391c954be80664"
    BUILD_ALWAYS     0
    TEST_COMMAND     ""
    UPDATE_COMMAND   ""
    BUILD_BYPRODUCTS ${AWS_S3_LIBRARY} ${AWS_CORE_LIBRARY}
    CMAKE_CACHE_ARGS "-DCMAKE_INSTALL_PREFIX:STRING=${CMAKE_INSTALL_PREFIX}"
    LIST_SEPARATOR    "|"
    CMAKE_ARGS       -DBUILD_SHARED_LIBS=ON
                     -DBUILD_ONLY=s3|sts
                     -DAUTORUN_UNIT_TESTS=OFF
                     -DENABLE_TESTING=OFF
                     -DCMAKE_BUILD_TYPE=Release
    LOG_CONFIGURE ON
    LOG_INSTALL ON
    LOG_BUILD ON
    LOG_OUTPUT_ON_FAILURE ON
)

add_library(aws_ext_core SHARED IMPORTED)
add_library(aws_ext_s3 SHARED IMPORTED)
set_target_properties(aws_ext_core
    PROPERTIES IMPORTED_LOCATION
    ${AWS_CORE_LIBRARY})
set_target_properties(aws_ext_s3
    PROPERTIES IMPORTED_LOCATION
    ${AWS_S3_LIBRARY})
add_dependencies(aws_ext_core aws_ext)
add_dependencies(aws_ext_s3 aws_ext)
# Merge the two libraries in one aliased interface
add_library(aws_ext_s3_lib INTERFACE)
target_link_libraries(aws_ext_s3_lib INTERFACE aws_ext_s3 aws_ext_core)
add_library(AWS::s3 ALIAS aws_ext_s3_lib)

# Tightly-coupled dependencies
set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(wavm_ext
    GIT_REPOSITORY "https://github.com/faasm/WAVM.git"
    GIT_TAG "6f4a663826f41d87d43203c9747253f8ecb3a1c0"
    CMAKE_ARGS "-DDLL_EXPORT= \
        -DDLL_IMPORT="
)

# Runtime-only patch (no struct layout changes, so AOT files built for this
# commit keep working): growing linear memory over pages that are already
# readable and writable skips mprotect. Applied once; a tree that already has
# it is left as it is.
set(FAASM_WAMR_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/wamr-skip-rw-mprotect.patch")
FetchContent_Declare(wamr_ext
    GIT_REPOSITORY "https://github.com/faasm/wasm-micro-runtime"
    GIT_TAG "c3a833acb51ba1c8d98aad7fceb69829c96c4eee"
    PATCH_COMMAND sh -c "git apply --reverse --check ${FAASM_WAMR_PATCH} 2>/dev/null || git apply ${FAASM_WAMR_PATCH}"
)

# WAMR and WAVM both link to LLVM
# If WAVM is not linked statically like WAMR, there are some obscure
# static constructor errors in LLVM due to double-registration
set(WAVM_ENABLE_STATIC_LINKING ON CACHE INTERNAL "")

FetchContent_MakeAvailable(wavm_ext wamr_ext)

# Allow access to headers nested in other projects
FetchContent_GetProperties(wavm_ext SOURCE_DIR FAASM_WAVM_SOURCE_DIR)
message(STATUS FAASM_WAVM_SOURCE_DIR ${FAASM_WAVM_SOURCE_DIR})

FetchContent_GetProperties(wamr_ext SOURCE_DIR WAMR_ROOT_DIR)
message(STATUS WAMR_ROOT_DIR ${WAMR_ROOT_DIR})
