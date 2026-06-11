# PixelFlow-RPC — 跨语言云原生图像处理引擎

> **A Cross-Language, Cloud-Native Image Processing Engine with Epoll-Driven I/O and ONNX Runtime Inference**

---

##  1. 项目概述

**PixelFlow-RPC** 是一套面向生产环境的图像处理微服务系统，采用 **C++ 算力层 + Java 业务层** 的异构架构，通过自定义二进制通信协议在两层之间完成 RPC 调用。系统部署于 **华为云服务器 K3s (Kubernetes) 集群**，具备滚动更新、资源硬隔离、Prometheus/Grafana 全栈可观测性等云原生能力。

### 核心技术栈

| 层级 | 技术选型 |
|:---|:---|
| **语言与编译器** | C++ 17 (`-O3` 优化), Java (业务端) |
| **网络 I/O 模型** | Linux Epoll (监听 Socket) + 阻塞 Worker 线程模型 |
| **序列化协议** | 自定义 16 字节 RpcHeader（网络字节序）+ Google Protobuf 3 |
| **图像处理引擎** | OpenCV 4.x（灰度化 / Canny 边缘检测 / 高斯模糊 / Otsu 二值化 / 卡通化） |
| **AI 推理运行时** | ONNX Runtime 1.17.1（超分辨率重建，ESPCN 模型） |
| **并发模型** | 自定义 `ThreadPool`（生产者-消费者，`std::condition_variable` 驱动） |
| **容器编排** | K3s (Lightweight Kubernetes)，3 节点集群 |
| **容器运行时** | Docker Multi-Stage Build（构建阶段 + 极简运行阶段） |
| **服务暴露** | Kubernetes LoadBalancer Service → 华为云 ELB |
| **可观测性** | kube-prometheus-stack（Prometheus + Grafana），独立调度至管理节点 |
| **私有镜像仓库** | 华为云 SWR (Software Repository for Container) |
| **云平台** | 华为云 Flexus 云服务器（2 核 4 GB × 3 节点） |

---

## 2. 系统架构 (System Architecture)

### 2.1 数据流全景

```
[ Java 业务端 (Spring Boot) ]
   │  Protobuf 序列化 ImageRequest → 封装 16B RpcHeader (Big-Endian)
   │  Payload = [RpcHeader] + [Protobuf Body]
   ▼
[ 华为云 ELB (L4 Load Balancer) ]
   │  TCP 流分发至后端 K3s Service
   ▼
[ K3s Cluster (3 Nodes) ]
   │
   ├─ rpc-service (LoadBalancer / NodePort :9000)
   │   │
   │   ├─ Pod #1 (replica)
   │   │   ├─ epoll_wait() 监听 listen_fd — 仅处理 accept
   │   │   ├─ accept() → setBlocking() → pool.enqueue(conn_fd)
   │   │   └─ ThreadPool Worker (8)
   │   │       ├─ recv() Header (阻塞模式) → 校验 magic_number
   │   │       ├─ recv() Body → ParseFromArray()
   │   │       ├─ processor.process(type, raw_bytes)
   │   │       │   ├─ cv::imdecode → cv::Mat
   │   │       │   ├─ dispatch: type 1~7
   │   │       │   └─ cv::imencode → .jpg bytes
   │   │       └─ send() Response → close(fd)
   │   │
   │   └─ Pod #2 (replica)  [同上，镜像副本]
   │
   └─ k3s-master (管理节点 — 独立调度)
       ├─ Prometheus Server   (800Mi limit, 2d retention)
       ├─ Grafana             (nodeSelector 锁定)
       └─ kube-prometheus-stack Operator
```

### 2.2 关键设计决策

| 决策点 | 方案 | 理由 |
|:---|:---|:---|
| **协议分层** | 定长二进制头 + Protobuf 变长体 | Protobuf 不自带长度前缀，定长头提供 `body_len` 用于分配接收缓冲区 |
| **FD 生命周期** | accept → setBlocking → 线程池全权托管 → close | conn_fd 只被一个 Worker 线程持有，避免跨线程竞争和多路复用导致的 FD 重复触发 |
| **Epoll 的定位** | 仅监听 listen_fd，不管理 conn_fd | 主线程只做 accept 分发，不在事件循环中读数据；Worker 线程用阻塞 I/O 串行完成读取-处理-写回 |
| **线程池规模** | 8 worker threads | 2 核节点上 8 线程利用并发度覆盖 I/O 等待，避免过多线程的上下文切换开销 |
| **算力隔离** | Prometheus/Grafana 通过 `nodeSelector` + `tolerations` 固定至 `k3s-master` | 2 核 4 GB 节点上保护算力 Pod 的 CPU 配额不被监控组件挤占 |

---

## 3. 核心技术

### 3.1 I/O 模型与线程池

#### 3.1.1 "Accept-and-Handoff" 连接处理 (`src/main.cpp`)

`main()` 中 Epoll 仅监听 `listen_fd`，所有 `conn_fd` 由 Worker 线程全生命周期托管：

1. **`accept()` 后立即移交**：新连接建立后直接 `setBlocking(conn_fd)`，不注册 Epoll，不设置 `O_NONBLOCK`。这一步将 `conn_fd` 的所有权从主线程转移到线程池，主线程只负责 accept 分发，不在事件循环中触碰任何连接数据。

   ```cpp
   // main.cpp:79 — accept 后立即移交
   setBlocking(conn_fd);
   pool.enqueue([conn_fd, &processor]() {
       // Worker 线程完成读 Header → 读 Body → 处理 → 写回 → close 的全流程
   });
   ```

2. **Worker 阻塞读 Header**：Worker 线程在阻塞 Socket 上循环 `recv()` 读取 16 字节 `RpcHeader`。EAGAIN/EWOULDBLOCK 作为防御性代码保留（10 μs 短 sleep 后重试），在正常阻塞 Socket 上极少触发。

3. **Heap 分配 + Body 上限检查**：`RpcHeader` 通过 `std::make_shared<RpcHeader>()` 在堆上分配，避免大栈帧；Body 分配前检查 `body_len > 50 * 1024 * 1024`，拒绝异常请求。

4. **魔术字校验**：`magic_number != 0xCAFEBABE` 视为非法连接，直接 `close(fd)` + `return`。

5. **`else` 分支防御性清理**：任何意外出现在 Epoll 事件列表中的非 `listen_fd` FD，直接 `epoll_ctl(DEL)` + `close()`。

设计效果：主线程 O(1) 完成 accept 分发，不在事件循环中做任何 I/O；Worker 线程以同步阻塞方式串行完成"读-算-写-关"，无跨线程 FD 竞争。

#### 3.1.2 生产者-消费者线程池 (`include/ThreadPool.h`)

基于 C++ 11 标准库实现的无外部依赖线程池：

- **生产者侧**：`enqueue()` 用 `std::packaged_task` 包装可调用对象，入队后通过 `condition.notify_one()` 唤醒一个 Worker。
- **消费者侧**：Worker 线程在 `std::condition_variable::wait()` 上阻塞，取任务执行。`notify_one()` 避免惊群。
- **生命周期**：析构函数设 `stop = true` 并 `notify_all()`，然后 `join()` 所有 Worker。

#### 3.1.3 时序

```
Epoll 主线程                         Worker 线程
────────────                         ──────────
epoll_wait() → listen_fd 就绪
  ├─ accept() → conn_fd
  ├─ setBlocking(conn_fd)
  └─ pool.enqueue(handler)  ──────→  recv() 阻塞读 16B RpcHeader
                                      ├─ 校验 magic_number == 0xCAFEBABE
                                      ├─ 检查 body_len ≤ 50MB
                                      ├─ recv() 阻塞读 Body
                                      ├─ ParseFromArray() → ImageRequest
                                      ├─ processor.process(type, data, out)
                                      ├─ SerializeToString() → ImageResponse
                                      ├─ send() RpcHeader + Body
                                      └─ close(conn_fd)
```

---

### 3.2 跨语言二进制协议设计

#### 3.2.1 协议栈分层

通信协议采用"定长二进制帧头 + Protobuf 可变长负载"的二级封装：

```
 0                    15 16                                           N
┌──────────────────────┬──────────────────────────────────────────────┐
│   RpcHeader (16 B)   │        Protobuf Serialized Body              │
│                      │        (ImageRequest / ImageResponse)        │
│  ┌─────────────────┐ │                                              │
│  │ magic_number    │ │                                              │
│  │ (uint32, BE)    │ │                                              │
│  ├─────────────────┤ │                                              │
│  │ version         │ │                                              │
│  │ (uint32, BE)    │ │                                              │
│  ├─────────────────┤ │                                              │
│  │ body_len        │ │                                              │
│  │ (uint32, BE)    │ │                                              │
│  ├─────────────────┤ │                                              │
│  │ type            │ │                                              │
│  │ (uint32, BE)    │ │                                              │
│  └─────────────────┘ │                                              │
└──────────────────────┴──────────────────────────────────────────────┘
```

#### 3.2.2 RpcHeader 的定义 (`include/protocol.h`)

```cpp
#pragma pack(push, 1)
struct RpcHeader {
    uint32_t magic_number;   // 魔数 0xCAFEBABE
    uint32_t version;        // 协议版本号
    uint32_t body_len;       // Protobuf 负载字节长度
    uint32_t type;           // 图像处理算法类型 (1~7)
};
#pragma pack(pop)
```

- **`#pragma pack(push, 1)`**：禁用结构体对齐填充，`sizeof(RpcHeader) == 16` 在任意编译器/平台下一致。
- **网络字节序（Big-Endian）**：字段经 `htonl()` / `ntohl()` 转换。Java 端 `ByteBuffer` 默认大端序，双端语义一致。
- **`body_len` 作为长度前缀**：接收方先读 16 字节固定头获取 `body_len`，再分配缓冲区精确读取 Body。Protobuf 不自带消息边界，需要外层长度前缀。

#### 3.2.3 Protobuf 契约 (`proto/message.proto`)

```protobuf
syntax = "proto3";
package myrpc;

message ImageRequest {
    bytes image_data = 1;     // JPEG/PNG 编码的原始图像字节流
    string file_name = 2;
}

message ImageResponse {
    bytes processed_data = 1; // JPEG 编码的处理结果
    bool success = 2;
}
```

`image_data` 使用 `bytes` 而非像素矩阵：1920×1080 RGB 展开为 `repeated int32` 约 6.2M 个字段，序列化开销远大于直接传输 JPEG 字节流。将已编码图像作为不透明 `bytes` 传输，利用 `cv::imdecode` / `cv::imencode` 编解码。

#### 3.2.4 Java 客户端状态

> Java 客户端代码（`java-client/`）因项目时间限制尚未完成合并。双端协议对齐在设计阶段已完成：
> - Java 端按 `RpcHeader` 结构（4 × int，Big-Endian）用 `ByteBuffer` 封包/拆包。
> - `message.proto` 作为 Single Source of Truth，通过 `protoc --cpp_out` / `--java_out` 分别生成绑定代码。
> - 连接目标为 K3s 集群 LoadBalancer Service（`172.31.14.229` / `172.31.14.66`，端口 `30900` / `9000`）。

---

### 3.3 云原生集群部署

#### 3.3.1 算力 Pod 部署 (`k8s-deploy/core/rpc-deploy.yaml`)

```yaml
spec:
  replicas: 2
  template:
    spec:
      containers:
      - name: processor
        image: swr.cn-south-1.myhuaweicloud.com/lmy_rpc/image-processor:v2.4
        resources:
          requests:
            memory: "512Mi"
            cpu: "500m"
          limits:
            memory: "2560Mi"
            cpu: "2000m"
```

- **`replicas: 2`**：3 节点集群上 2 副本覆盖单节点宕机。K3s 调度器以 `preferredDuringScheduling` 反亲和性分散 Pod。
- **`limits.memory: 2560Mi`**：2 核 4 GB 节点上 2.5 Gi 上限同时为 OpenCV/ONNX 推理留足内存余量，OOM Kill 不波及同节点其他 Pod。
- **`limits.cpu: 2000m`**：允许 burst 至 2 核，CFS 层面防止单 Pod 耗尽节点 CPU。

#### 3.3.2 算力隔离：Prometheus & Grafana 调度策略 (`k8s-deploy/monitoring/monitor-values.yaml`)

在 2 核 4 GB 节点上，Prometheus 全栈未限制时可占 600 Mi ~ 1.2 Gi。通过 `nodeSelector` + `tolerations` 将监控组件锁定至管理节点：

```yaml
prometheus:
  prometheusSpec:
    nodeSelector:
      kubernetes.io/hostname: k3s-master
    tolerations:
    - key: "node-role.kubernetes.io/master"
      operator: "Exists"
      effect: "NoSchedule"
    resources:
      requests:
        memory: "400Mi"
      limits:
        memory: "800Mi"
    retention: 2d

grafana:
  nodeSelector:
    kubernetes.io/hostname: k3s-master
  tolerations:
  - key: "node-role.kubernetes.io/master"
    operator: "Exists"
    effect: "NoSchedule"
```

- **`tolerations`**：K3s master 节点默认有 `NoSchedule` 污点，tolerations 允许监控组件调度至 master。
- **算力 Pod 与监控分离**：2 个 Worker 节点各承载 1 个算力 Pod，监控独占 master 节点。
- **`retention: 2d`**：Flexus 40 GB 系统盘上缩短 TSDB 保留期，避免磁盘占满。

#### 3.3.3 准入控制器镜像拉取修复 (`k8s-deploy/monitoring/fix-admission.yaml`)

`kube-prometheus-stack` (v60.0.0) 的 `kube-webhook-certgen` Job 默认镜像为 `registry.k8s.io/ingress-nginx/kube-webhook-certgen:v1.5.1`，国内无法拉取（GFW 阻断 `registry.k8s.io`），导致 Job 处于 `ImagePullBackOff`，阻塞 Prometheus Operator 启动。这是一个分布式死锁：Operator 等待 Webhook 证书 → Job 无法拉取镜像 → 证书无法生成。

修复：替换为阿里云代理镜像 `registry.cn-hangzhou.aliyuncs.com/google_containers/kube-webhook-certgen:v1.5.1`。

在 `monitor-values.yaml` 中覆盖 Helm values：

```yaml
prometheusOperator:
  admissionWebhooks:
    patch:
      image:
        registry: registry.cn-hangzhou.aliyuncs.com
        repository: google_containers/kube-webhook-certgen
        tag: v1.5.1
```

`fix-admission.yaml` 手动创建 `create` Job，并声明 `nodeSelector: k3s-master` + `tolerations` 绕过 master 污点：

```yaml
spec:
  template:
    spec:
      containers:
      - name: create
        image: registry.cn-hangzhou.aliyuncs.com/google_containers/kube-webhook-certgen:v1.5.1
        args:
        - create
        - --host=monitor-kube-prometheus-st-operator,monitor-kube-prometheus-st-operator.monitoring.svc
        - --namespace=monitoring
        - --secret-name=monitor-kube-prometheus-st-admission
      nodeSelector:
        kubernetes.io/hostname: k3s-master
      tolerations:
      - key: "node-role.kubernetes.io/master"
        operator: "Exists"
        effect: "NoSchedule"
```

Admission Webhook 证书生成是 Prometheus Operator 启动的前置条件，Job 失败会使整个监控栈卡在 `Pending`。

---

### 3.4 图像处理算法与 ONNX 推理管道 (`include/processor.h`, `src/processor.cpp`)

#### 3.4.1 算法分发

`ImageProcessor::process(int type, const std::string& input, std::string& output)` 根据 `type` 枚举（1~7）分发至不同算法：

| type | 算法 | OpenCV 核心调用 | 复杂度 |
|:---|:---|:---|:---|
| 1 | 灰度化 | `cv::cvtColor(BGR2GRAY)` | O(W·H) |
| 2 | Canny 边缘检测 | `cv::Canny(gray, 100, 200)` | O(W·H) |
| 3 | 高斯模糊 | `cv::GaussianBlur(Size(15,15))` | O(W·H·225) |
| 4 | 反色处理 | `cv::bitwise_not()` | O(W·H) |
| 5 | Otsu 二值化 | `cv::threshold(THRESH_OTSU)` | O(W·H) + 直方图 |
| 6 | 卡通化 | 中值滤波 + 自适应阈值 + 双边滤波 | O(W·H·(49+81)) |
| 7 | 超分辨率重建 | ONNX Runtime ESPCN 推理 | O(卷积前向传播) |

所有算法的输入输出均为 JPEG 编码的 `bytes`，内部编解码通过 `cv::imdecode` / `cv::imencode` 完成，中间数据在内存中以 `cv::Mat` 传递。

#### 3.4.2 超分辨率重建的 ONNX 推理管道

`runSuperResolution()` 完成 BGR → YCrCb → 归一化 → 张量构建 → `session->Run()` → 后处理的完整流程：

1. **预处理**：BGR → YCrCb，提取 Y 通道，归一化至 `[0,1]`，Resize 至 224×224。
2. **张量构建**：`cv::Mat` 填充至 `std::vector<float>`，构造 NCHW 格式 `{1, 1, 224, 224}` ONNX 张量。
3. **推理**：`session->Run()` 同步前向传播。
4. **后处理**：值域 clamp → 反归一化 → 与 Cr/Cb 合并 → YCrCb → BGR → 2× 上采样输出。

---

## 4. 构建与部署

### 4.1 本地编译

```bash
cd CXX-engine
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

编译产物通过 CMake `INSTALL_RPATH` 写入 ONNX Runtime 库路径，本机运行无需额外设置 `LD_LIBRARY_PATH`。

### 4.2 Docker 镜像构建与推送

```bash
cd CXX-engine
docker build -t swr.cn-south-1.myhuaweicloud.com/lmy_rpc/image-processor:v2.x .
docker push swr.cn-south-1.myhuaweicloud.com/lmy_rpc/image-processor:v2.x
```

Dockerfile 采用 Multi-Stage Build：构建阶段安装完整编译工具链，运行阶段仅安装运行时 .so（libprotobuf, libopencv-imgcodecs, libopencv-imgproc, libopencv-core）。APT 源替换为阿里云镜像加速。

### 4.3 K3s 集群部署

```bash
# 部署算力服务
kubectl apply -f k8s-deploy/core/rpc-deploy.yaml

# 部署 Prometheus + Grafana（国内镜像修复）
helm upgrade --install monitor kube-prometheus-stack-60.0.0.tgz \
  --namespace monitoring --create-namespace \
  -f k8s-deploy/monitoring/monitor-values.yaml

# 手动创建 Admission Webhook 证书
kubectl apply -f k8s-deploy/monitoring/fix-admission.yaml
```

---

## 5. 项目目录结构

```
PixelFlow-RPC/
├── CXX-engine/                        # C++ 算力引擎
│   ├── CMakeLists.txt                 # CMake 构建 (C++17, -O3)
│   ├── Dockerfile                     # 多阶段构建 (Ubuntu 24.04)
│   ├── src/
│   │   ├── main.cpp                   # Epoll accept 分发 + 线程池调度
│   │   ├── processor.cpp              # OpenCV 算法 + ONNX Runtime 推理
│   │   └── message.proto              # Protobuf 契约
│   ├── include/
│   │   ├── protocol.h                 # RpcHeader 结构体 (1 字节对齐 + 网络字节序)
│   │   ├── processor.h                # ImageProcessor 类声明
│   │   ├── ThreadPool.h               # 生产者-消费者线程池
│   │   └── message.pb.h              # Protobuf 生成头文件
│   ├── third_party/onnxruntime/       # ONNX Runtime 1.17.1 (预编译)
│   ├── models/
│   │   └── super_resolution.onnx      # ESPCN 超分辨率模型
│   └── kube-prometheus-stack-60.0.0.tgz
├── proto/
│   └── message.proto                  # Protobuf 契约（Single Source of Truth）
├── k8s-deploy/
│   ├── core/
│   │   └── rpc-deploy.yaml            # Deployment (2 副本) + LoadBalancer Service
│   └── monitoring/
│       ├── monitor-values.yaml        # Prometheus/Grafana 调度策略
│       └── fix-admission.yaml         # 准入控制器 certgen Job 修复
├── java-client/
│   └── README.md                      # Java 客户端提交指引（代码待合并）
└── README.md
```

---

## 6. 技术指标

| 指标 | 值 |
|:---|:---|
| **最大并发连接数** | `MAX_EVENTS` (1024)，受系统 fd 限制 |
| **线程池规模** | 8 worker threads / Pod |
| **Pod 副本数** | 2（可水平扩展） |
| **每 Pod CPU 限制** | 2000m (2 核) |
| **每 Pod 内存限制** | 2560 Mi |
| **Prometheus 指标保留** | 48 小时 |
| **通信协议开销** | 16 字节固定头 + Protobuf 变长体 |

---

> *C++ 17 · Linux Epoll · Protobuf · OpenCV · ONNX Runtime · K3s · Huawei Cloud Flexus*
