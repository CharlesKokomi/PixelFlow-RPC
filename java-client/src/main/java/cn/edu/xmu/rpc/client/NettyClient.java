package cn.edu.xmu.rpc.client;

import cn.edu.xmu.rpc.ImageProto;
import cn.edu.xmu.rpc.protocol.MyRpcEncoder;
import cn.edu.xmu.rpc.protocol.RpcRequestPacket;
import com.google.protobuf.ByteString;
import io.netty.bootstrap.Bootstrap;
import io.netty.channel.*;
import io.netty.channel.nio.NioEventLoopGroup;
import io.netty.channel.socket.SocketChannel;
import io.netty.channel.socket.nio.NioSocketChannel;
import io.netty.handler.codec.LengthFieldBasedFrameDecoder;
import io.netty.handler.codec.protobuf.ProtobufDecoder;
import lombok.extern.slf4j.Slf4j;

import java.nio.ByteOrder;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

@Slf4j
public class NettyClient {

    private String host;
    private int port;

    public void setHost(String host) { this.host = host; }
    public void setPort(int port) { this.port = port; }

    private static volatile EventLoopGroup sharedGroup;
    private static final Object GROUP_LOCK = new Object();

    private volatile Channel channel;
    volatile boolean healthy;

    volatile CompletableFuture<byte[]> pendingFuture;
    volatile Runnable returnCallback;

    public synchronized void start() {
        if (isHealthy()) return;
        Channel old = this.channel;
        if (old != null) {
            old.close();
            this.channel = null;
        }
        try {
            connect(host, port);
            log.info("成功连接至远程图像处理引擎: {}:{}", host, port);
        } catch (Exception e) {
            log.error("尝试连接服务器 {}:{} 失败", host, port, e);
        }
    }

    private void connect(String host, int port) throws Exception {
        if (sharedGroup == null) {
            synchronized (GROUP_LOCK) {
                if (sharedGroup == null) {
                    sharedGroup = new NioEventLoopGroup(Runtime.getRuntime().availableProcessors());
                    log.info("初始化共享 EventLoopGroup，线程数: {}", Runtime.getRuntime().availableProcessors());
                }
            }
        }

        Bootstrap b = new Bootstrap();
        b.group(sharedGroup).channel(NioSocketChannel.class)
                .option(ChannelOption.CONNECT_TIMEOUT_MILLIS, 5000)
                .option(ChannelOption.TCP_NODELAY, true)
                .option(ChannelOption.SO_KEEPALIVE, true)
                .handler(new ChannelInitializer<SocketChannel>() {
                    @Override
                    protected void initChannel(SocketChannel ch) {
                        ChannelPipeline p = ch.pipeline();
                        p.addLast(new LengthFieldBasedFrameDecoder(ByteOrder.BIG_ENDIAN, 10 * 1024 * 1024, 8, 4, 4, 16, true));
                        p.addLast(new ProtobufDecoder(ImageProto.ImageResponse.getDefaultInstance()));
                        p.addLast(new MyRpcEncoder());
                        p.addLast(new NettyClientHandler(NettyClient.this));
                    }
                });

        ChannelFuture f = b.connect(host, port).sync();
        this.channel = f.channel();
        this.healthy = true;
    }

    public CompletableFuture<byte[]> sendRequest(int type, byte[] imageData) {
        if (!isHealthy()) {
            start();
        }

        if (!isHealthy()) {
            CompletableFuture<byte[]> failed = new CompletableFuture<>();
            failed.completeExceptionally(new RuntimeException("服务器连接不可用"));
            return failed;
        }

        CompletableFuture<byte[]> future = new CompletableFuture<>();
        this.pendingFuture = future;

        ImageProto.ImageRequest protoData = ImageProto.ImageRequest.newBuilder()
                .setImageData(ByteString.copyFrom(imageData))
                .build();

        channel.writeAndFlush(new RpcRequestPacket(type, protoData))
                .addListener((ChannelFutureListener) f -> {
                    if (!f.isSuccess()) {
                        healthy = false;
                        this.pendingFuture = null;
                        future.completeExceptionally(f.cause());
                        returnToPool();
                    }
                });

        return future;
    }

    public byte[] sendRequestSync(int type, byte[] imageData) throws Exception {
        return sendRequest(type, imageData).get(15, TimeUnit.SECONDS);
    }

    public boolean isHealthy() {
        return healthy && channel != null && channel.isActive();
    }

    void markUnhealthy() {
        healthy = false;
    }

    void returnToPool() {
        Runnable cb = this.returnCallback;
        this.returnCallback = null;
        if (cb != null) {
            cb.run();
        }
    }

    public void stop() {
        healthy = false;
        CompletableFuture<byte[]> future = this.pendingFuture;
        this.pendingFuture = null;
        if (future != null && !future.isDone()) {
            future.completeExceptionally(new RuntimeException("连接已关闭"));
        }
        if (channel != null) {
            Channel ch = channel;
            channel = null;
            ch.close().syncUninterruptibly();
        }
    }

    public static void shutdownSharedGroup() {
        synchronized (GROUP_LOCK) {
            if (sharedGroup != null && !sharedGroup.isShuttingDown()) {
                sharedGroup.shutdownGracefully();
                sharedGroup = null;
                log.info("共享 EventLoopGroup 已关闭");
            }
        }
    }
}
