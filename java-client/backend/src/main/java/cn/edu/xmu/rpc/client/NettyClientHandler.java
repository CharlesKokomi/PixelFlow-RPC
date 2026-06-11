package cn.edu.xmu.rpc.client;

import cn.edu.xmu.rpc.ImageProto;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.SimpleChannelInboundHandler;
import lombok.extern.slf4j.Slf4j;

import java.util.concurrent.CompletableFuture;

@Slf4j
public class NettyClientHandler extends SimpleChannelInboundHandler<ImageProto.ImageResponse> {

    private final NettyClient client;

    public NettyClientHandler(NettyClient client) {
        this.client = client;
    }

    @Override
    protected void channelRead0(ChannelHandlerContext ctx, ImageProto.ImageResponse response) {
        byte[] data = response.getProcessedData().toByteArray();

        CompletableFuture<byte[]> future = client.pendingFuture;
        client.pendingFuture = null;
        if (future != null) {
            future.complete(data);
        } else {
            log.warn("连接 {} 收到响应但无匹配请求", ctx.channel().id());
        }
        // C++ 每次响应后关连接，标记 unhealthy 强制下次借出重连
        client.markUnhealthy();
        client.returnToPool();
    }

    @Override
    public void exceptionCaught(ChannelHandlerContext ctx, Throwable cause) {
        log.error("连接 {} 异常: {}", ctx.channel().id(), cause.getMessage());
        client.markUnhealthy();
        CompletableFuture<byte[]> future = client.pendingFuture;
        client.pendingFuture = null;
        if (future != null && !future.isDone()) {
            future.completeExceptionally(new RuntimeException("连接异常: " + cause.getMessage()));
        }
        client.returnToPool();
        ctx.close();
    }

    @Override
    public void channelInactive(ChannelHandlerContext ctx) {
        client.markUnhealthy();
        CompletableFuture<byte[]> future = client.pendingFuture;
        client.pendingFuture = null;
        if (future != null && !future.isDone()) {
            future.completeExceptionally(new RuntimeException("连接断开"));
        }
        client.returnToPool();
        ctx.fireChannelInactive();
    }
}
