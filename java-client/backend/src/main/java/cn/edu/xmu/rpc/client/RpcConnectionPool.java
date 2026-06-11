package cn.edu.xmu.rpc.client;

import lombok.extern.slf4j.Slf4j;

import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

@Slf4j
public class RpcConnectionPool {

    private final BlockingQueue<NettyClient> available;
    private final String host;
    private final int port;
    private final ExecutorService workerPool;

    public RpcConnectionPool(String host, int port, int poolSize) {
        this.host = host;
        this.port = port;
        this.available = new LinkedBlockingQueue<>();

        for (int i = 0; i < poolSize; i++) {
            available.offer(newClient());
        }

        AtomicInteger threadId = new AtomicInteger(1);
        this.workerPool = new ThreadPoolExecutor(
                poolSize, poolSize, 60L, TimeUnit.SECONDS,
                new LinkedBlockingQueue<>(),
                r -> {
                    Thread t = new Thread(r, "rpc-pool-" + threadId.getAndIncrement());
                    t.setDaemon(true);
                    return t;
                });

        log.info("连接池已创建: {} 个客户端就绪（懒连接模式）", available.size());
    }

    private NettyClient newClient() {
        NettyClient client = new NettyClient();
        client.setHost(host);
        client.setPort(port);
        return client;
    }

    /**
     * 全异步：调用线程立即返回 CompletableFuture，
     * 连接获取和网络 IO 都在后台工作线程执行。
     */
    public CompletableFuture<byte[]> sendRequest(int type, byte[] data) {
        return CompletableFuture.supplyAsync(() -> acquireAndSend(type, data), workerPool)
                .thenCompose(f -> f);
    }

    private CompletableFuture<byte[]> acquireAndSend(int type, byte[] data) {
        NettyClient client;
        try {
            client = available.poll(15, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return failedFuture(e);
        }

        if (client == null) {
            return failedFuture(new RuntimeException("获取连接超时（15s），池中无可用连接"));
        }

        // 懒连接
        if (!client.isHealthy()) {
            client.start();
            if (!client.isHealthy()) {
                client.stop();
                available.offer(client);
                return failedFuture(new RuntimeException("连接失败，请检查 C++ 服务是否可达"));
            }
        }

        // handler 处理完响应（或断连）后自动归还
        client.returnCallback = () -> available.offer(client);

        return client.sendRequest(type, data);
    }

    private static <T> CompletableFuture<T> failedFuture(Throwable e) {
        CompletableFuture<T> f = new CompletableFuture<>();
        f.completeExceptionally(e);
        return f;
    }

    public int availableCount() {
        return available.size();
    }

    public void shutdown() {
        workerPool.shutdown();
        NettyClient client;
        while ((client = available.poll()) != null) {
            client.stop();
        }
        NettyClient.shutdownSharedGroup();
        log.info("连接池已关闭");
    }
}
