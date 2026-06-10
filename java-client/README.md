\# PixelFlow-RPC: Java Client \& Backend Component



本目录用于存放 \*\*PixelFlow-RPC\*\* 的 Java 业务后端与微服务客户端代码。由于本项目采用异构架构，Java 端的核心职责是\*\*承载高并发业务、封包自定义协议头并与 C++ 算力集群进行高吞吐 Socket 通信\*\*。



为了统一仓库规范并进行简历/项目成果的最终对齐，请团队同学按照以下指引，将相关的 Java 后端代码提交至本目录。



\---



\## 🛠️ 提交文件范围清单 (Scope of Submission)



请各位队友将你负责的 Java 前后端、微服务模块中，\*\*最核心、最能体现工程量\*\*的以下文件复制并归类提交至本目录（可保持原有包结构，如 `src/main/java/...`）：



1\. \*\*协议封包与 Socket 通信核心\*\*

&#x20;  - 负责构建 16 字节 `RpcHeader` 二进制包头的代码（包含 Magic Number 校验、动态数据长度计算）。

&#x20;  - 负责建立 Socket 连接、无阻塞/异步读取 C++ 集群返回数据的网络输入输出流（InputStream/OutputStream）处理类。

2\. \*\*Protobuf 业务序列化层\*\*

&#x20;  - 将原始图片字节流（`bytes`）以及滤镜类型参数（`filter\\\_type`）封装进 Protobuf `ImageRequest` 对象的业务代码。

&#x20;  - 解析 `ImageResponse` 并将处理后的图片数据流向前端分发的控制器（Controller）或服务层（Service）。

3\. \*\*配置文件与路由（可选）\*\*

&#x20;  - 连接 K3s 集群内网负载均衡虚拟 IP（`172.31.14.229` / `172.31.14.66`）及端口（`30900`）的 `application.yml` 或配置类。



> ⚠️ \\\*\\\*注意\\\*\\\*：不需要提交完整的 Spring Boot 骨架或无关的网页前端 UI 代码，\\\*\\\*仅保留与 RPC 通信、图片流处理、高并发控制相关的核心 Java 类与 `pom.xml` 依赖即可\\\*\\\*。



\---



\## 📂 推荐提交后的目录结构 (Target Structure)



```text

java-client/

├── src/

│   └── main/

│       └── java/

│           └── 你的包名

│               ├── config/      # K3s 虚拟 IP 与集群连接配置

│               ├── controller/  # 接收前端图片请求、调用 RPC 的 Gateway/Controller

│               ├── protocol/    # 16字节 RpcHeader 封包与二进制协议对齐逻辑 (核心)

│               └── service/     # Socket 管道管理与 Protobuf 序列化交互服务

└── pom.xml                      # 包含 protobuf-java, netty 或 standard socket 依赖


