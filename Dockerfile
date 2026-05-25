# --- 第一阶段：编译环境 ---
FROM ubuntu:24.04 AS builder

# 设置非交互模式，避免时区选择等阻塞
ENV DEBIAN_FRONTEND=noninteractive

# 安装基础编译工具和依赖库的开发版
RUN sed -i 's/archive.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list.d/ubuntu.sources && \
    apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libprotobuf-dev \
    protobuf-compiler \
    libopencv-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build_context

# 拷贝整个项目
COPY . .

# 创建编译目录
RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc)

# --- 第二阶段：运行环境 ---
FROM ubuntu:24.04

# 设置非交互模式
ENV DEBIAN_FRONTEND=noninteractive

# 仅安装运行所需的动态库（不要安装 -dev 版，减小体积）
RUN sed -i 's/archive.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list.d/ubuntu.sources && \
    apt-get update && apt-get install -y \
    libprotobuf32t64 \
    libopencv-imgcodecs406t64 \
    libopencv-imgproc406t64 \
    libopencv-core406t64 \
    curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 1. 拷贝程序
COPY --from=builder /build_context/build/server /app/server

# 2. 拷贝 ONNX Runtime 库到 /app/lib (简化路径)
COPY --from=builder /build_context/third_party/onnxruntime/lib/ /app/lib/

# 3. 设置环境变量，告诉系统去这里找 .so
ENV LD_LIBRARY_PATH=/app/lib

# 4. 拷贝模型
COPY --from=builder /build_context/models/ /app/models/

RUN chmod +x /app/server
ENTRYPOINT ["/app/server"]