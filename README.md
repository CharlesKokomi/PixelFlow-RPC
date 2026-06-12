# PixelFlow-RPC — 跨语言云原生图像处理引擎

> \*\*A Cross-Language, Cloud-Native Image Processing Engine with Epoll-Driven I/O\*\*

\---

## 1\. 项目概述

**PixelFlow-RPC** 是一套面向生产环境的图像处理微服务系统，采用 **C++ 算力层 + Java 业务层 + Web 前端** 的三层异构架构：

* **C++ 引擎层**：Epoll 驱动的 TCP 服务端，集成 OpenCV 实现图像处理算法，通过自定义二进制协议对外暴露 RPC 接口。
* **Java 中间层**：Spring Boot 提供 HTTP REST API，Netty 连接池管理到 C++ 引擎的长连接，完成 HTTP ↔ RPC 协议转换。
* **Web 前端**：纯静态页面，支持图片上传、方法选择、处理前后对比预览。

C++ 引擎部署于 **K3s (Kubernetes) 集群**（2 核 4 GB × 3 节点），具备滚动更新、资源硬隔离、Prometheus/Grafana 全栈可观测性等云原生能力。Java 中间层部署于集群同内网的一台独立服务器上，通过内网 IP 直连 K3s Service，保证 RPC 调用的低延迟。

### 核心技术栈

|层级|技术选型|
|-|-|
|**C++ 引擎**|C++17 (`-O3` 优化), Linux Epoll, OpenCV 4.x, Protobuf 3|
|**Java 中间层**|Spring Boot, Netty 4.x, Protobuf (Java Binding)|
|**前端**|HTML5 / CSS3 / Vanilla JS (Fetch API)|
|**序列化协议**|自定义 16 字节 RpcHeader（网络字节序）+ Protobuf|
|**并发模型**|C++ 自定义 ThreadPool + Java Netty NIO + 连接池|
|**容器编排**|K3s (Lightweight Kubernetes)，3 节点集群|
|**容器运行时**|Docker Multi-Stage Build|
|**可观测性**|kube-prometheus-stack（Prometheus + Grafana），独立调度至管理节点|

\---

## 2\. 系统架构

### 2.1 数据流全景

```
\[ Web 浏览器 (HTML/CSS/JS) ]
   │  HTTP POST /rpc/process (multipart/form-data: file + type)
   ▼
\[ Java 中间层 (Spring Boot :8081, 独立服务器, 与集群同内网) ]
   │  DemoController → RpcConnectionPool → NettyClient
   │  Protobuf 序列化 ImageRequest → 封装 16B RpcHeader (Big-Endian)
   │  Payload = \[RpcHeader] + \[Protobuf Body]
   │  内网 TCP → K3s Service (:9000)
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
|**三层解耦**|浏览器 HTTP → Java Netty TCP → C++ Epoll|前端无需理解 RPC 协议；Java 层隔离协议差异，可独立扩容|
|**Java 连接池**|Netty NIO 长连接 + BlockingQueue|复用 TCP 连接，避免频繁握手；池化减少 C++ 端 accept 压力|
|**FD 生命周期**|accept → setBlocking → 线程池全权托管 → close|conn\_fd 只被一个 Worker 持有，避免跨线程 FD 竞争|
|**Epoll 定位**|仅监听 listen\_fd，不管理 conn\_fd|主线程只做 accept 分发，Worker 线程以阻塞 I/O 串行完成读-算-写-关|
|**线程池规模**|8 worker threads / Pod|2 核节点上 8 线程覆盖 I/O 等待，避免过多上下文切换|
|**算力隔离**|监控组件通过 nodeSelector + tolerations 固定至 k3s-master|保护算力 Pod 的 CPU 配额不被监控组件挤占|

\---

## 3\. 核心技术

### 3.1 C++ 引擎 — I/O 模型与线程池

#### 3.1.1 Accept-and-Handoff (`src/main.cpp`)

`main()` 中 Epoll 仅监听 `listen\_fd`，conn\_fd 由 Worker 线程全生命周期托管：

1. **accept 后立即移交**：新连接建立后 `setBlocking(conn\_fd)`，不注册 Epoll，直接 `pool.enqueue()` 投递至线程池。主线程只做 accept 分发，不触碰连接数据。
2. **Worker 阻塞读 Header**：Worker 循环 `recv()` 16 字节 `RpcHeader`。EAGAIN 防御性逻辑保留但极少触发。
3. **堆分配 + Body 上限**：`RpcHeader` 用 `std::make\_shared` 堆分配；Body 分配前检查 `body\_len > 50MB` 直接拒绝。
4. **魔术字校验**：`magic\_number != 0xCAFEBABE` 即 `close(fd)` 返回。

整体效果：主线程 O(1) 完成分发，Worker 线程以阻塞方式串行完成读-算-写-关，无跨线程 FD 竞争。

#### 3.1.2 线程池 (`include/ThreadPool.h`)

基于 C++11 标准库：`enqueue()` 用 `std::packaged\_task` 包装任务，`condition.notify\_one()` 唤醒 Worker；析构时 `stop = true` + `notify\_all()` + `join()` 优雅关闭。

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
    uint32\_t type;           // 算法类型 (1\~6)
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

#### 3.2.4 Java 端协议实现

Java 端通过 Netty 的 `LengthFieldBasedFrameDecoder`（大端序，偏移 8，长度 4 字节）进行拆包，`ProtobufDecoder` 反序列化响应。`MyRpcEncoder` 将 `RpcRequestPacket`（含 type + Protobuf 体）编码为 `\[RpcHeader] + \[Protobuf Body]` 网络包。协议结构与 C++ 端完全对齐。

\---

### 3.3 Java 中间层

Java 后端不部署在 K3s 集群内，而是运行在集群同内网的一台独立服务器上。Netty 通过内网 IP 直连 C++ Service（`remote.service.host` 配置为 K3s NodePort 或 LoadBalancer 地址），避免公网绕行。

#### 3.3.1 整体架构

```
Spring Boot REST API (:8081, 独立服务器, 与 K3s 集群同内网)
   │
   ├─ DemoController      — POST /rpc/process (multipart upload → JPEG bytes)
   ├─ RpcConnectionPool   — 连接池，BlockingQueue<NettyClient>（默认 10）
   │   ├─ NettyClient     — 单连接管理（懒连接、健康检查、自动归还）
   │   └─ NettyClientHandler — 响应回调（pendingFuture.complete / 断连归还）
   ├─ RpcProxyFactory     — 动态代理，接口方法 → RPC type 枚举
   └─ RemoteImageService  — 业务接口定义（6 种图像处理方法）
```

#### 3.3.2 请求处理流程

1. 浏览器 `POST /rpc/process`，携带 `file` (MultipartFile) + `type` 参数。
2. `DemoController` 读取文件字节数组，调用 `RpcConnectionPool.sendRequest(type, data)`。
3. 连接池从 `BlockingQueue` 获取空闲 `NettyClient`，若连接未建立或已断开则懒重连。
4. `NettyClient` 用 Protobuf 序列化 `ImageRequest`，封装 `RpcHeader`，通过 Netty Channel 发送至 C++ 引擎。
5. C++ 引擎处理后返回 `RpcHeader + ImageResponse`。
6. `NettyClientHandler` 解析响应，完成 `CompletableFuture`，连接自动归还连接池。
7. `DemoController` 返回 `ResponseEntity<byte\[]>`（JPEG 流）给浏览器。

#### 3.3.3 关键设计

|设计点|实现|
|-|-|
|**全异步**|Controller 返回 `CompletableFuture<ResponseEntity<byte\[]>>`，后台线程池执行网络 I/O|
|**懒连接**|NettyClient 在首次 `sendRequest` 时才建立 TCP 连接，失败自动重试|
|**连接复用**|共享 `EventLoopGroup`，所有 Client 复用同一组 NIO 线程|
|**超时控制**|15 秒超时（连接获取 + RPC 调用），超时自动失败|

\---

### 3.4 Web 前端

纯静态单页应用（`java-client/frontend/`），通过 nginx 或直接由 Spring Boot 托管静态资源：

* **上传区**：拖拽/点击上传，支持 PNG / JPG / JPEG，客户端校验文件类型。
* **方法选择**：6 种处理算法以按钮网格展示，含编号、名称、描述。
* **对比预览**：左右分栏展示原图与处理结果，`URL.createObjectURL` 本地预览。
* **状态管理**：busy / success / error 三态状态栏，处理中禁用按钮防止重复提交。
* **API 调用**：`fetch()` POST 到 Java 后端，接收 `image/jpeg` 响应流直接渲染。

\---

### 3.5 云原生集群部署

#### 3.5.1 算力 Pod (`k8s-deploy/core/rpc-deploy.yaml`)

```yaml
spec:
  replicas: 2
  template:
    spec:
      containers:
      - name: processor
        image: <registry>/image-processor:<tag>
        resources:
          requests: { memory: "512Mi", cpu: "500m" }
          limits:   { memory: "2560Mi", cpu: "2000m" }
```

* `replicas: 2`：3 节点集群上 2 副本覆盖单节点宕机，调度器反亲和性分散 Pod。
* `limits.memory: 2560Mi`：2.5 Gi 为 OpenCV 图像处理留足内存余量，OOM Kill 不波及同节点其他 Pod。
* `limits.cpu: 2000m`：允许 burst 至 2 核，CFS 层面限制。

#### 3.5.2 算力隔离 (`k8s-deploy/monitoring/monitor-values.yaml`)

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

#### 3.5.3 准入控制器镜像修复 (`k8s-deploy/monitoring/fix-admission.yaml`)

`kube-prometheus-stack` 的准入控制器 Job 在受限网络环境下可能因镜像拉取失败导致 Operator 启动阻塞。修复方案：替换为可访问的镜像仓库地址，并通过 `fix-admission.yaml` 手动创建 Job，配置 `nodeSelector` + `tolerations` 绕过 master 污点。

\---

### 3.6 图像处理算法 (`include/processor.h`, `src/processor.cpp`)

#### 3.6.1 算法分发

`ImageProcessor::process(int type, ...)` 根据 `type` 枚举分发至 6 种算法：

|type|算法|OpenCV 核心调用|复杂度|
|-|-|-|-|
|1|灰度化|`cv::cvtColor(BGR2GRAY)`|O(W·H)|
|2|Canny 边缘检测|`cv::Canny(gray, 100, 200)`|O(W·H)|
|3|高斯模糊|`cv::GaussianBlur(Size(15,15))`|O(W·H·225)|
|4|反色处理|`cv::bitwise\_not()`|O(W·H)|
|5|Otsu 二值化|`cv::threshold(THRESH\_OTSU)`|O(W·H) + 直方图|
|6|卡通化|中值滤波 + 自适应阈值 + 双边滤波|O(W·H·(49+81))|

输入输出均为 JPEG 编码 `bytes`，内部通过 `cv::imdecode` / `cv::imencode` 编解码，中间数据以 `cv::Mat` 在内存中传递。

\---

## 4\. 构建与部署

### 4.1 C++ 引擎编译

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

### 4.4 Java 后端启动

```bash
cd java-client/backend
mvn spring-boot:run
# 或直接运行 JAR：
java -jar RPC-0.0.1-SNAPSHOT.jar
```

配置文件 `application.properties` 中可修改 C++ 引擎地址和连接池大小：

```properties
server.port=8081
remote.service.host=<C++ Engine IP>
remote.service.port=9000
rpc.connection.pool.size=10
spring.servlet.multipart.max-file-size=50MB
```

### 4.5 前端部署

前端为纯静态文件，可直接由 Spring Boot 的 `static/` 目录托管，或通过 nginx 部署：

```bash
# 放入 Spring Boot 静态资源目录
cp -r java-client/frontend/\* java-client/backend/src/main/resources/static/
```

部署后通过浏览器访问 `http://<host>:8081`，上传图片并选择处理方法即可。

\---

## 5\. 测试

### 5.1 Python 压力测试 (`java-client/test/direct\_pressure\_test.py`)

绕过 Java 中间层，直接对 C++ 引擎发起 TCP 连接进行压测：

* **模式**：长连接，每线程持有一条 TCP 连接循环发送请求。
* **默认配置**：1600 并发线程，10s 预热 + 30s 正式测试。
* **输出指标**：TPS（平均/峰值）、延迟（平均/P95/P99）、成功率。

```bash
# 在 test 目录放入测试图片 test.png，配置目标 IP 后运行
cd java-client/test
python direct\_pressure\_test.py
```

\---

## 6\. 项目目录结构

```
PixelFlow-RPC/
├── CXX-engine/
│   ├── CMakeLists.txt                    # C++17, -O3
│   ├── Dockerfile                        # Multi-Stage Build
│   ├── src/
│   │   ├── main.cpp                      # Epoll accept + 线程池调度
│   │   └── processor.cpp                 # OpenCV 图像处理算法
│   ├── include/
│   │   ├── protocol.h                    # RpcHeader (1 字节对齐)
│   │   ├── processor.h                   # ImageProcessor 类
│   │   ├── ThreadPool.h                  # 生产者-消费者线程池
│   │   └── message.pb.h                  # Protobuf 生成头文件
│   ├── third\_party/onnxruntime/          # ONNX Runtime 1.17.1 (预编译)
│   ├── models/
│   │   └── super\_resolution.onnx         # ESPCN 模型 (已弃用)
│   └── kube-prometheus-stack-60.0.0.tgz
├── proto/
│   └── message.proto                     # Protobuf 契约 (Single Source of Truth)
├── k8s-deploy/
│   ├── core/
│   │   └── rpc-deploy.yaml               # Deployment + LoadBalancer Service
│   └── monitoring/
│       ├── monitor-values.yaml           # Prometheus/Grafana 调度策略
│       └── fix-admission.yaml            # 准入控制器镜像修复
├── java-client/
│   ├── backend/
│   │   ├── RPC-0.0.1-SNAPSHOT.jar        # Spring Boot 可执行 JAR
│   │   ├── pom.xml                       # Maven 配置
│   │   └── src/main/
│   │       ├── java/cn/edu/xmu/rpc/
│   │       │   ├── RpcApplication.java   # Spring Boot 入口
│   │       │   ├── controller/           # REST 控制器
│   │       │   ├── client/               # Netty 客户端 + 连接池 + 代理工厂
│   │       │   ├── protocol/             # 编解码器 + 数据包 + 类型枚举
│   │       │   ├── config/               # RPC 配置 + CORS
│   │       │   └── service/              # 业务接口定义
│   │       ├── proto/                    # Protobuf 定义（Java 版本）
│   │       └── resources/
│   │           └── application.properties
│   ├── frontend/
│   │   ├── index.html                    # 主页面
│   │   ├── app.js                        # 交互逻辑 + API 调用
│   │   ├── styles.css                    # 样式
│   │   └── config.js                     # API 地址配置
│   └── test/
│       └── direct\_pressure\_test.py       # C++ 引擎直连压测脚本
└── README.md
```

\---

## 7\. 技术指标

|指标|值|
|-|-|
|**C++ 引擎最大并发连接**|`MAX\_EVENTS` (1024)，受系统 fd 限制|
|**C++ 线程池规模**|8 worker threads / Pod|
|**Java 连接池规模**|10（可配置）|
|**Pod 副本数**|2（可水平扩展）|
|**每 Pod CPU 限制**|2000m (2 核)|
|**每 Pod 内存限制**|2560 Mi|
|**Prometheus 指标保留**|48 小时|
|**通信协议开销**|16 字节固定头 + Protobuf 变长体|
|**Java 端超时**|15 秒（连接获取 + RPC 调用）|

\---

> \*C++ 17 · Linux Epoll · Protobuf · OpenCV · Spring Boot · Netty · K3s\*

