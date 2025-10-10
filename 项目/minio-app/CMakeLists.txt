cmake_minimum_required(VERSION 3.10)
project(MinIOStreamExample)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置vcpkg安装路径
# 写自己实际的路径，这里我只是写了我的路径
set(VCPKG_INSTALLED_DIR "/home/lqf/minio/vcpkg/installed/x64-linux")

# 查找必要的包
find_package(PkgConfig REQUIRED)

# 查找libcurl
pkg_check_modules(CURL REQUIRED libcurl)

# 设置MinIO C++ SDK的包含目录和库目录
set(MINIO_INCLUDE_DIR "${VCPKG_INSTALLED_DIR}/include")
set(MINIO_LIB_DIR "${VCPKG_INSTALLED_DIR}/lib")

# 包含目录
include_directories(${CURL_INCLUDE_DIRS})
include_directories(${MINIO_INCLUDE_DIR})

# 添加可执行文件
add_executable(minio_stream minio_stream.cpp)
add_executable(minio_basic minio_basic.cpp)

# 链接库
target_link_libraries(minio_stream 
    ${CURL_LIBRARIES}
    ${MINIO_LIB_DIR}/libminiocpp.a
    ${MINIO_LIB_DIR}/libpugixml.a
    ${MINIO_LIB_DIR}/libINIReader.a
    ${MINIO_LIB_DIR}/libinih.a
    ${MINIO_LIB_DIR}/libcurlpp.a
    ${MINIO_LIB_DIR}/libcurl.a
    ${MINIO_LIB_DIR}/libz.a
    ${MINIO_LIB_DIR}/libssl.a
    ${MINIO_LIB_DIR}/libcrypto.a
    pthread
    dl
)

target_link_libraries(minio_basic 
    ${MINIO_LIB_DIR}/libminiocpp.a
    ${MINIO_LIB_DIR}/libpugixml.a
    ${MINIO_LIB_DIR}/libINIReader.a
    ${MINIO_LIB_DIR}/libinih.a
    ${MINIO_LIB_DIR}/libcurlpp.a
    ${MINIO_LIB_DIR}/libcurl.a
    ${MINIO_LIB_DIR}/libz.a
    ${MINIO_LIB_DIR}/libssl.a
    ${MINIO_LIB_DIR}/libcrypto.a
    pthread
    dl
)

# 编译选项
target_compile_options(minio_stream PRIVATE ${CURL_CFLAGS_OTHER})

# 添加编译选项
target_compile_options(minio_stream PRIVATE -Wall -Wextra)

# 设置输出目录
set_target_properties(minio_stream PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

set_target_properties(minio_basic PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# 安装规则
install(TARGETS minio_stream minio_basic
    RUNTIME DESTINATION bin
)

# 打印配置信息
message(STATUS "VCPKG_INSTALLED_DIR: ${VCPKG_INSTALLED_DIR}")
message(STATUS "MINIO_INCLUDE_DIR: ${MINIO_INCLUDE_DIR}")
message(STATUS "MINIO_LIB_DIR: ${MINIO_LIB_DIR}")
message(STATUS "CURL_INCLUDE_DIRS: ${CURL_INCLUDE_DIRS}")
message(STATUS "CURL_LIBRARIES: ${CURL_LIBRARIES}")
message(STATUS "CURL_CFLAGS_OTHER: ${CURL_CFLAGS_OTHER}") 