cmake_minimum_required(VERSION 3.10)
project(ImageRpcServer)

set(CMAKE_CXX_STANDARD 17)
#开启 O3 优化，提升图像处理速度
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")

# 1. 查找依赖库
find_package(Protobuf REQUIRED)
find_package(OpenCV REQUIRED)

# 指定 ONNX Runtime 路径
set(ONNXRUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")

# 2. 包含头文件目录
include_directories(
    ${PROTOBUF_INCLUDE_DIRS} 
    ${OpenCV_INCLUDE_DIRS}
    "${ONNXRUNTIME_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# 3. 找到 ONNX Runtime 动态库
find_library(ONNXRUNTIME_LIB onnxruntime
    PATHS "${ONNXRUNTIME_DIR}/lib"
    REQUIRED
)

# 4. 指定源文件
set(SOURCES 
    src/main.cpp 
    src/message.pb.cc
    src/processor.cpp
)

# 5. 生成可执行程序
add_executable(server ${SOURCES})

# 6. 链接库
target_link_libraries(server 
    ${PROTOBUF_LIBRARIES} 
    ${OpenCV_LIBS}
    ${ONNXRUNTIME_LIB}
    pthread
)

# 7.设置运行时库搜索路径 (RPATH)
set_target_properties(server PROPERTIES 
    INSTALL_RPATH "${ONNXRUNTIME_DIR}/lib"
    BUILD_WITH_INSTALL_RPATH TRUE
)