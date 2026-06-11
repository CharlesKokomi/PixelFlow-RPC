package cn.edu.xmu.rpc.protocol;

public class ServiceType {
    public static final int GRAYSCALE = 1;       // 灰度化
    public static final int CANNY_EDGE = 2;      // Canny 边缘检测
    public static final int GAUSSIAN_BLUR = 3;   // 高斯模糊 (去噪)
    public static final int INVERT = 4;          // 反色处理 (底片效果)
    public static final int THRESHOLD = 5;       // 阈值分割 (二值化)
    public static final int CARTOON = 6;           // 卡通化
}