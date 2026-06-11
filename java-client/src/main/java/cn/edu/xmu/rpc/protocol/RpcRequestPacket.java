package cn.edu.xmu.rpc.protocol;

import cn.edu.xmu.rpc.ImageProto;
import lombok.AllArgsConstructor;
import lombok.Data;

@Data
@AllArgsConstructor
public class RpcRequestPacket {
    private int type; // 业务类型：
    private ImageProto.ImageRequest data; // 实际的 Protobuf 数据
}