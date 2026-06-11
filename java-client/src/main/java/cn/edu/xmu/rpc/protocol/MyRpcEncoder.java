package cn.edu.xmu.rpc.protocol;

import cn.edu.xmu.rpc.ImageProto;
import io.netty.buffer.ByteBuf;
import io.netty.channel.ChannelHandlerContext;
import io.netty.handler.codec.MessageToByteEncoder;

import lombok.extern.slf4j.Slf4j;
@Slf4j
public class MyRpcEncoder extends MessageToByteEncoder<RpcRequestPacket> {

    @Override
    protected void encode(ChannelHandlerContext ctx, RpcRequestPacket packet, ByteBuf out) {
        // 1. 序列化 Protobuf Body
        byte[] body = packet.getData().toByteArray();

        // 2. 写入 16 字节报头
        out.writeInt(0xCAFEBABE);      // Magic Number
        out.writeInt(1);               // Version
        out.writeInt(body.length);     // Body Length
        out.writeInt(packet.getType()); // 动态写入业务类型

        // 3. 写入 Body
        out.writeBytes(body);

        log.info("RPC请求发送成功 | 业务类型: {} | 数据长度: {} 字节", packet.getType(), body.length);
    }
}