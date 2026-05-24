#ifndef PROCESSOR_H
#define PROCESSOR_H
#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

class ImageProcessor {
public:
    ImageProcessor();
    // 统一的入口，根据 type 选择不同的算法
    bool process(int type, const std::string& input, std::string& output);

private:
    // 各种具体的算法函数
    Ort::Env env;
    std::unique_ptr<Ort::Session> session;
    Ort::SessionOptions session_options;
    // AI 推理具体的内部逻辑
    void runSuperResolution(const cv::Mat& src, cv::Mat& dst);
    static void makeGray(const cv::Mat& src, cv::Mat& dst);
    static void detectEdges(const cv::Mat& src, cv::Mat& dst);
    static void blurImage(const cv::Mat& src, cv::Mat& dst);
    static void invertImage(const cv::Mat& src, cv::Mat& dst);
    static void thresholdImage(const cv::Mat& src, cv::Mat& dst);
    static void applyCartoonEffect(const cv::Mat& src, cv::Mat& dst);
};

#endif