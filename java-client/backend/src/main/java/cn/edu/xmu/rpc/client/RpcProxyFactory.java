package cn.edu.xmu.rpc.client;

import cn.edu.xmu.rpc.protocol.ServiceType;
import java.lang.reflect.Proxy;

public class RpcProxyFactory {
    public static <T> T create(Class<T> interfaceClass, NettyClient client) {
        return (T) Proxy.newProxyInstance(
                interfaceClass.getClassLoader(),
                new Class<?>[]{interfaceClass},
                (proxy, method, args) -> {
                    int type;
                    switch (method.getName()) {
                        case "detectEdges":    type = ServiceType.CANNY_EDGE;    break;
                        case "blurImage":      type = ServiceType.GAUSSIAN_BLUR; break;
                        case "invertImage":    type = ServiceType.INVERT;        break;
                        case "thresholdImage": type = ServiceType.THRESHOLD;     break;
                        case "cartoonEffect"   :type= ServiceType.CARTOON;         break;
                        default:               type = ServiceType.GRAYSCALE;     break;
                    }
                    // 调用 NettyClient 的同步发送逻辑
                    return client.sendRequestSync(type, (byte[]) args[0]);
                }
        );
    }
}