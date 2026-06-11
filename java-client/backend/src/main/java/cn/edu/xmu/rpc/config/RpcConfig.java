package cn.edu.xmu.rpc.config;

import cn.edu.xmu.rpc.client.RpcConnectionPool;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;

@Configuration
public class RpcConfig {

    @Value("${remote.service.host}")
    private String remoteHost;

    @Value("${remote.service.port}")
    private int remotePort;

    @Value("${rpc.connection.pool.size:20}")
    private int poolSize;

    @Bean
    public RpcConnectionPool rpcConnectionPool() {
        return new RpcConnectionPool(remoteHost, remotePort, poolSize);
    }
}
