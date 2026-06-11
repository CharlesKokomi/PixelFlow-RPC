# PixelFlow-RPC — 跨语言云原生图像处理引擎

> \*\*A Cross-Language, Cloud-Native Image Processing Engine with Epoll-Driven I/O\*\*

\---

## 1\. 项目概述

**PixelFlow-RPC** 是一套面向生产环境的图像处理微服务系统，采用 **C++ 算力层 + Java 业务层** 的异构架构，通过自定义二进制通信协议在两层之间完成 RPC 调用。系统部署于 **K3s (Kubernetes) 集群**（2 核 4 GB × 3 节点），具备滚动更新、资源硬隔离、Prometheus/Grafana 全栈可观测性等云原生能力。

### 核心技术栈

|层级|技术选型|
|-|-|
|**语言与编译器**|C++ 17 (`-O3` 优化), Java (业务端)|
|**网络 I/O 模型**|Linux Epoll (监听 Socket) + 阻塞 Worker 线程|
|**序列化协议**|自定义 16 字节 RpcHeader（网络字节序）+ Google Protobuf 3|
|**图像处理引擎**|OpenCV 4.x（灰度化 / Canny 边缘检测 / 高斯模糊 / Otsu 二值化 / 卡通化等）|
|**并发模型**|自定义 `ThreadPool`（生产者-消费者，`std::condition\_variable` 驱动）|
|**容器编排**|K3s (Lightweight Kubernetes)，3 节点集群|
|**容器运行时**|Docker Multi-Stage Build|
|**可观测性**|kube-prometheus-stack（Prometheus + Grafana），独立调度至管理节点|

\---

## 2\. 系统架构 (System Architecture)

### 2.1 数据流全景

```
\[ Java 业务端 (Spring Boot) ]
   │  Protobuf 序列化 ImageRequest → 封装 16B RpcHeader (Big-Endian)
   │  Payload = \[RpcHeader] + \[Protobuf Body]
   ▼
\[ L4 Load Balancer ]
   │  TCP 流分发至后端 K3s Service
   ▼
\[ K3s Cluster (3 Nodes) ]
   │
   ├─ rpc-service (LoadBalancer / NodePort :9000)
   │   │
   │   ├─ Pod #1 (replica)
   │   │   ├─ epoll\_wait() 监听 listen\_fd — 仅处理 accept
   │   │   ├─ accept() → setBlocking() → pool.enqueue(conn\_fd)
   │   │   └─ ThreadPool Worker (8)
   │   │       ├─ recv() Header (阻塞) → 校验 magic\_number
   │   │       ├─ recv() Body → ParseFromArray()
   │   │       ├─ processor.process(type, raw\_bytes)
   │   │       │   ├─ cv::imdecode → cv::Mat
   │   │       │   ├─ dispatch: type 1\~6
   │   │       │   └─ cv::imencode → .jpg bytes
   │   │       └─ send() Response → close(fd)
   │   │
   │   └─ Pod #2 (replica)  \[同上]
   │
   └─ k3s-master (管理节点)
       ├─ Prometheus Server   (800Mi limit, 2d retention)
       ├─ Grafana             (nodeSelector 锁定)
       └─ kube-prometheus-stack Operator
```

### 2.2 关键设计决策

|决策点|方案|理由|
|-|-|-|
|**协议分层**|定长二进制头 + Protobuf 变长体|Protobuf 不自带长度前缀，定长头提供 `body\_len` 用于分配接收缓冲区|
|**FD 生命周期**|accept → setBlocking → 线程池全权托管 → close|conn\_fd 只被一个 Worker 持有，避免跨线程 FD 竞争|
|**Epoll 定位**|仅监听 listen\_fd，不管理 conn\_fd|主线程只做 accept 分发，Worker 线程以阻塞 I/O 串行完成读-算-写-关|
|**线程池规模**|8 worker threads|2 核节点上 8 线程覆盖 I/O 等待，避免过多上下文切换|
|**算力隔离**|监控组件通过 nodeSelector + tolerations 固定至 k3s-master|保护算力 Pod 的 CPU 配额不被监控组件挤占|

\---

## 3\. 核心技术

### 3.1 I/O 模型与线程池

#### 3.1.1 Accept-and-Handoff (`src/main.cpp`)

`main()` 中 Epoll 仅监听 `listen\_fd`，conn\_fd 由 Worker 线程全生命周期托管：

1. **accept 后立即移交**：新连接建立后 `setBlocking(conn\_fd)`，不注册 Epoll，直接 `pool.enqueue()` 投递至线程池。主线程只做 accept 分发，不触碰连接数据。
2. **Worker 阻塞读 Header**：Worker 循环 `recv()` 16 字节 `RpcHeader`。EAGAIN 防御性逻辑保留但极少触发。
3. **堆分配 + Body 上限**：`RpcHeader` 用 `std::make\_shared` 堆分配；Body 分配前检查 `body\_len > 50MB` 直接拒绝。
4. **魔术字校验**：`magic\_number != 0xCAFEBABE` 即 `close(fd)` 返回。
5. **else 分支防御性清理**：非 listen\_fd 意外出现在 Epoll 事件列表中时，`epoll\_ctl(DEL)` + `close()`。

整体效果：主线程 O(1) 完成分发，Worker 线程以阻塞方式串行完成读-算-写-关，无跨线程 FD 竞争。

#### 3.1.2 线程池 (`include/ThreadPool.h`)

基于 C++ 11 标准库：`enqueue()` 用 `std::packaged\_task` 包装任务，`condition.notify\_one()` 唤醒 Worker；析构时 `stop = true` + `notify\_all()` + `join()` 优雅关闭。

#### 3.1.3 时序

```
Epoll 主线程                         Worker 线程
────────────                         ──────────
epoll\_wait() → listen\_fd 就绪
  ├─ accept() → conn\_fd
  ├─ setBlocking(conn\_fd)
  └─ pool.enqueue(handler)  ──────→  recv() 阻塞读 16B Header
                                      ├─ 校验 magic\_number
                                      ├─ 检查 body\_len ≤ 50MB
                                      ├─ recv() 阻塞读 Body
                                      ├─ ParseFromArray()
                                      ├─ processor.process()
                                      ├─ SerializeToString()
                                      ├─ send() Response
                                      └─ close(conn\_fd)
```

\---

### 3.2 跨语言二进制协议

#### 3.2.1 协议栈

```
 0                    15 16                                           N
┌──────────────────────┬──────────────────────────────────────────────┐
│   RpcHeader (16 B)   │        Protobuf Serialized Body              │
│                      │        (ImageRequest / ImageResponse)        │
│  ┌─────────────────┐ │                                              │
│  │ magic\_number    │ │                                              │
│  │ (uint32, BE)    │ │                                              │
│  ├─────────────────┤ │                                              │
│  │ version         │ │                                              │
│  │ (uint32, BE)    │ │                                              │
│  ├─────────────────┤ │                                              │
│  │ body\_len        │ │                                              │
│  │ (uint32, BE)    │ │                                              │
│  ├─────────────────┤ │                                              │
│  │ type            │ │                                              │
│  │ (uint32, BE)    │ │                                              │
│  └─────────────────┘ │                                              │
└──────────────────────┴──────────────────────────────────────────────┘
```

#### 3.2.2 RpcHeader (`include/protocol.h`)

```cpp
#pragma pack(push, 1)
struct RpcHeader {
    uint32\_t magic\_number;   // 0xCAFEBABE
    uint32\_t version;
    uint32\_t body\_len;       // Protobuf 负载字节长度
    uint32\_t type;           // 算法类型 (1\~7)
};
#pragma pack(pop)
```

* `#pragma pack(push, 1)` 禁用对齐填充，`sizeof == 16` 跨平台一致。
* 字段经 `htonl()` / `ntohl()` 网络字节序转换；Java `ByteBuffer` 默认大端序，双端一致。
* `body\_len` 作为长度前缀：先读固定头获取 Body 长度，再精确分配缓冲区。Protobuf 不自带消息边界，依赖外层定界。

#### 3.2.3 Protobuf 契约 (`proto/message.proto`)

```protobuf
message ImageRequest  { bytes image\_data = 1; string file\_name = 2; }
message ImageResponse { bytes processed\_data = 1; bool success = 2; }
```

`image\_data` 使用 `bytes` 直接传输 JPEG 编码流，交由 `cv::imdecode` / `cv::imencode` 编解码，避免将像素矩阵展开为 Protobuf 字段的巨大序列化开销。

#### 3.2.4 Java 客户端

> Java 客户端代码（`java-client/`）尚未完成合并。双端协议对齐在设计阶段已完成：Java 端按 `RpcHeader` 结构用 `ByteBuffer` 封包/拆包；`message.proto` 通过 `protoc` 分别生成 C++ 和 Java 绑定代码。

\---

### 3.3 云原生集群部署

#### 3.3.1 算力 Pod (`k8s-deploy/core/rpc-deploy.yaml`)

```yaml
spec:
  replicas: 2
  template:
    spec:
      containers:
      - name: processor
        image: swr.cn-south-1.myhuaweicloud.com/lmy\_rpc/image-processor:v2.4
        resources:
          requests: { memory: "512Mi", cpu: "500m" }
          limits:   { memory: "2560Mi", cpu: "2000m" }
```

* `replicas: 2`：3 节点集群上 2 副本覆盖单节点宕机，调度器反亲和性分散 Pod。
* `limits.memory: 2560Mi`：2.5 Gi 为 OpenCV 图像处理留足内存余量，OOM Kill 不波及同节点其他 Pod。
* `limits.cpu: 2000m`：允许 burst 至 2 核，CFS 层面限制。

#### 3.3.2 算力隔离 (`k8s-deploy/monitoring/monitor-values.yaml`)

2 核 4 GB 节点上 Prometheus 全栈未限制时可占 600 Mi \~ 1.2 Gi。通过 `nodeSelector` + `tolerations` 将监控组件锁定至管理节点，与算力 Pod 分离：

```yaml
prometheus:
  prometheusSpec:
    nodeSelector: { kubernetes.io/hostname: k3s-master }
    tolerations:
    - key: "node-role.kubernetes.io/master"
      operator: "Exists"
      effect: "NoSchedule"
    resources:
      requests: { memory: "400Mi" }
      limits:   { memory: "800Mi" }
    retention: 2d

grafana:
  nodeSelector: { kubernetes.io/hostname: k3s-master }
  tolerations:
  - key: "node-role.kubernetes.io/master"
    operator: "Exists"
    effect: "NoSchedule"
```

K3s master 节点默认有 `NoSchedule` 污点，tolerations 允许监控组件调度至 master。2 个 Worker 节点各承载 1 个算力 Pod，监控独占 master。

#### 3.3.3 准入控制器镜像修复 (`k8s-deploy/monitoring/fix-admission.yaml`)

`kube-prometheus-stack` 的 `kube-webhook-certgen` Job 默认镜像 `registry.k8s.io/...` 在国内无法拉取，导致 Prometheus Operator 启动阻塞。修复方案：镜像替换为阿里云代理地址 `registry.cn-hangzhou.aliyuncs.com/google\_containers/kube-webhook-certgen:v1.5.1`，并通过 `fix-admission.yaml` 手动创建 Job，配置 `nodeSelector` + `tolerations` 绕过 master 污点。

\---

### 3.4 图像处理算法 (`include/processor.h`, `src/processor.cpp`)

#### 3.4.1 算法分发

`ImageProcessor::process(int type, ...)` 根据 `type` 枚举分发至 7 种算法：

|type|算法|OpenCV 核心调用|复杂度|
|-|-|-|-|
|1|灰度化|`cv::cvtColor(BGR2GRAY)`|O(W·H)|
|2|Canny 边缘检测|`cv::Canny(gray, 100, 200)`|O(W·H)|
|3|高斯模糊|`cv::GaussianBlur(Size(15,15))`|O(W·H·225)|
|4|反色处理|`cv::bitwise\_not()`|O(W·H)|
|5|Otsu 二值化|`cv::threshold(THRESH\_OTSU)`|O(W·H) + 直方图|
|6|卡通化|中值滤波 + 自适应阈值 + 双边滤波|O(W·H·(49+81))|
|7|超分辨率重建|ONNX Runtime ESPCN（效果不佳，代码保留）|—|

输入输出均为 JPEG 编码 `bytes`，内部通过 `cv::imdecode` / `cv::imencode` 编解码，中间数据以 `cv::Mat` 在内存中传递。

> type 7 的 ONNX Runtime 超分辨率重建代码（`runSuperResolution()`）因模型效果未达预期已弃用，相关代码及 `models/super\_resolution.onnx` 保留供后续参考。

\---

## 4\. 构建与部署

### 4.1 本地编译

```bash
cd CXX-engine
mkdir -p build \&\& cd build
cmake .. -DCMAKE\_BUILD\_TYPE=Release
make -j$(nproc)
```

CMake 通过 `INSTALL\_RPATH` 写入 ONNX Runtime 库路径。

### 4.2 Docker 镜像构建

```bash
docker build -t <registry>/image-processor:<tag> .
docker push <registry>/image-processor:<tag>
```

Multi-Stage Build：构建阶段安装完整工具链，运行阶段仅保留运行时 .so，减小镜像体积。

### 4.3 K3s 集群部署

```bash
kubectl apply -f k8s-deploy/core/rpc-deploy.yaml

helm upgrade --install monitor kube-prometheus-stack-60.0.0.tgz \\
  --namespace monitoring --create-namespace \\
  -f k8s-deploy/monitoring/monitor-values.yaml

kubectl apply -f k8s-deploy/monitoring/fix-admission.yaml
```

\---

## 5\. 项目目录结构

```
PixelFlow-RPC/
├── CXX-engine/
│   ├── CMakeLists.txt                # C++17, -O3
│   ├── Dockerfile                    # Multi-Stage Build
│   ├── src/
│   │   ├── main.cpp                  # Epoll accept + 线程池调度
│   │   ├── processor.cpp             # OpenCV 图像处理算法
│   │   └── message.proto             # Protobuf 契约
│   ├── include/
│   │   ├── protocol.h                # RpcHeader (1 字节对齐)
│   │   ├── processor.h               # ImageProcessor 类
│   │   ├── ThreadPool.h              # 生产者-消费者线程池
│   │   └── message.pb.h             # Protobuf 生成头文件
│   ├── third\_party/onnxruntime/      # ONNX Runtime 1.17.1 (预编译)
│   ├── models/
│   │   └── super\_resolution.onnx     # ESPCN 模型 (已弃用)
│   └── kube-prometheus-stack-60.0.0.tgz
├── proto/
│   └── message.proto                 # Single Source of Truth
├── k8s-deploy/
│   ├── core/
│   │   └── rpc-deploy.yaml           # Deployment + LoadBalancer Service
│   └── monitoring/
│       ├── monitor-values.yaml       # Prometheus/Grafana 调度策略
│       └── fix-admission.yaml        # 准入控制器镜像修复
├── java-client/
│   └── README.md                     # Java 客户端 (待合并)
└── README.md
```

\---

## 6\. 技术指标

|指标|值|
|-|-|
|**最大并发连接数**|`MAX\_EVENTS` (1024)，受系统 fd 限制|
|**线程池规模**|8 worker threads / Pod|
|**Pod 副本数**|2（可水平扩展）|
|**每 Pod CPU 限制**|2000m (2 核)|
|**每 Pod 内存限制**|2560 Mi|
|**Prometheus 指标保留**|48 小时|
|**通信协议开销**|16 字节固定头 + Protobuf 变长体|

\---

> \*C++ 17 · Linux Epoll · Protobuf · OpenCV · K3s\*

