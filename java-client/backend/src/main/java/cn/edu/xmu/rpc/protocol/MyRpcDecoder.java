package cn.edu.xmu.rpc.protocol;

import cn.edu.xmu.rpc.ImageProto;
import io.netty.buffer.ByteBuf;
import io.netty.channel.ChannelHandlerContext;
import io.netty.handler.codec.LengthFieldBasedFrameDecoder;

public class MyRpcDecoder extends LengthFieldBasedFrameDecoder {

    /**
     * 参数配置说明 ：
     * maxFrameLength: 10MB
     * lengthFieldOffset: 8 (跳过 Magic 4B 和 Version 4B)
     * lengthFieldLength: 4 (body_len 为 4 字节)
     * lengthAdjustment: 4  (body_len 之后还有一个 4 字节的 type 字段才到 Body)
     * initialBytesToStrip: 0
     */
    public MyRpcDecoder() {
        super(1024 * 1024 * 10, 8, 4, 4, 0);
    }

    @Override
    protected Object decode(ChannelHandlerContext ctx, ByteBuf in) throws Exception {
        // 使用父类逻辑提取完整的 Frame
        ByteBuf frame = (ByteBuf) super.decode(ctx, in);
        if (frame == null) return null;

        try {
            // 验证回包魔数
            int magic = frame.readInt();
            int version = frame.readInt();
            int length = frame.readInt();
            int type = frame.readInt();

            if (magic != 0xCAFEBABE) {
                throw new IllegalArgumentException("非法回包魔数: " + Integer.toHexString(magic));
            }

            // 读取并解析处理后的图片
            byte[] body = new byte[length];
            frame.readBytes(body);
            return ImageProto.ImageResponse.parseFrom(body);

        } finally {
            frame.release();
        }
    }
}