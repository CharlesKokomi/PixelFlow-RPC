package cn.edu.xmu.rpc.controller;

import cn.edu.xmu.rpc.client.RpcConnectionPool;
import lombok.extern.slf4j.Slf4j;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.*;
import org.springframework.web.multipart.MultipartFile;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

@Slf4j
@RestController
@RequestMapping("/rpc")
public class DemoController {

    @Autowired
    private RpcConnectionPool pool;

    @PostMapping(value = "/process", produces = MediaType.IMAGE_JPEG_VALUE)
    public CompletableFuture<ResponseEntity<byte[]>> processImage(
            @RequestParam("file") MultipartFile file,
            @RequestParam("type") int type) {

        if (type < 1 || type > 6) {
            return CompletableFuture.completedFuture(ResponseEntity.badRequest().build());
        }

        byte[] inputData;
        try {
            inputData = file.getBytes();
        } catch (Exception e) {
            return CompletableFuture.completedFuture(ResponseEntity.internalServerError().build());
        }

        log.info("接收处理请求 | 类型: {} | 文件: {}", type, file.getOriginalFilename());

        return pool.sendRequest(type, inputData)
                .orTimeout(15, TimeUnit.SECONDS)
                .thenApply(resultData -> {
                    log.info("RPC 处理成功，返回图片流，长度: {}", resultData.length);
                    return ResponseEntity.ok()
                            .contentType(MediaType.IMAGE_JPEG)
                            .body(resultData);
                })
                .exceptionally(e -> {
                    log.error("接口处理异常", e);
                    return ResponseEntity.internalServerError().build();
                });
    }
}
