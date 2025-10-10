#!/bin/bash

# MinIO 流式上传程序编译脚本

echo "=== 编译 MinIO 流式上传程序 ==="

# 设置vcpkg路径
VCPKG_DIR="/home/lqf/minio/vcpkg"
INCLUDE_DIR="$VCPKG_DIR/installed/x64-linux/include"
LIB_DIR="$VCPKG_DIR/installed/x64-linux/lib"

# 检查必要文件
if [ ! -d "$VCPKG_DIR" ]; then
    echo "错误: vcpkg目录不存在: $VCPKG_DIR"
    exit 1
fi

if [ ! -d "$INCLUDE_DIR" ]; then
    echo "错误: 包含目录不存在: $INCLUDE_DIR"
    exit 1
fi

if [ ! -d "$LIB_DIR" ]; then
    echo "错误: 库目录不存在: $LIB_DIR"
    exit 1
fi

if [ ! -f "$INCLUDE_DIR/miniocpp/client.h" ]; then
    echo "错误: MinIO头文件不存在: $INCLUDE_DIR/miniocpp/client.h"
    exit 1
fi

if [ ! -f "$LIB_DIR/libminiocpp.a" ]; then
    echo "错误: MinIO库文件不存在: $LIB_DIR/libminiocpp.a"
    exit 1
fi

# 检查源文件
if [ ! -f "minio_stream.cpp" ]; then
    echo "错误: 源文件不存在: minio_stream.cpp"
    exit 1
fi

# 检查测试文件
if [ ! -f "./test-file.txt" ]; then
    echo "警告: 测试文件不存在: ./test-file.txt"
    echo "请确保test-file.txt文件存在，或者程序会提示错误"
fi

echo "开始编译..."

# 编译命令
g++ -std=c++17 \
    -I"$INCLUDE_DIR" \
    -L"$LIB_DIR" \
    -o minio_stream \
    minio_stream.cpp \
    -lminiocpp -lpugixml -lINIReader -linih -lcurlpp -lcurl -lz -lssl -lcrypto -lpthread -ldl

if [ $? -eq 0 ]; then
    echo "编译成功！"
    echo "可执行文件: ./minio_stream"
    echo ""
    echo "运行前请确保："
    echo "1. MinIO服务器正在运行 (localhost:9000)"
    echo "2. 存储桶 'video' 已创建"
    echo "3. 测试文件 './test-file.txt' 存在"
    echo ""
    echo "运行命令: ./minio_stream"
else
    echo "编译失败！"
    exit 1
fi 