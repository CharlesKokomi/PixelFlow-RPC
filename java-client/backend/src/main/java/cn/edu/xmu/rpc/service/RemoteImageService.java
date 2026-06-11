package cn.edu.xmu.rpc.service;

/**
 * 业务层接口：每个方法对应 C++ 端的一个功能
 */

public interface RemoteImageService {
    byte[] convertToGray(byte[] imageData);    // Type 1
    byte[] detectEdges(byte[] imageData);      // Type 2
    byte[] blurImage(byte[] imageData);        // Type 3
    byte[] invertImage(byte[] imageData);      // Type 4
    byte[] thresholdImage(byte[] imageData);   // Type 5
    byte[] cartoonEffect(byte[] imageData);     // Type 6
}