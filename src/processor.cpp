#include "processor.h"
#include <iostream>

ImageProcessor::ImageProcessor() : env(ORT_LOGGING_LEVEL_ERROR, "Inference") {
    // 1.初始化推理设置
    session_options.SetIntraOpNumThreads(0);
    session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    // 2.加载模型
    const char* model_path = "models/super_resolution.onnx";
    session = std::make_unique<Ort::Session>(env, model_path, session_options);
}

bool ImageProcessor::process(int type, const std::string& input, std::string& output) {
    try {
        // 1. 解码
        std::vector<uchar> data(input.begin(), input.end());
        cv::Mat src = cv::imdecode(data, cv::IMREAD_COLOR);
        if (src.empty()) {
            std::cerr << "[Processor] 解码失败，数据为空或格式错误" << std::endl;
            return false;
        }

        cv::Mat dst;
        // 2. 算法分发
        switch (type) {
            case 1: // 灰度化
                makeGray(src, dst);
                break;
            case 2: // Canny 边缘检测
                detectEdges(src, dst);
                break;
            case 3: // 高斯模糊 (去噪)
                blurImage(src, dst);
                break;
            case 4: // 反色处理 (底片效果)
                invertImage(src, dst);
                break;
            case 5: // 阈值分割 (二值化)
                thresholdImage(src, dst);
                break;
            case 6: // 卡通化
                applyCartoonEffect(src, dst);
                break;
            case 7: // 超分辨率重建
                runSuperResolution(src, dst);
                break;
            default:
                std::cout << "[Processor] 未知类型，原样返回" << std::endl;
                src.copyTo(dst);
        }

        // 3. 编码
        std::vector<uchar> buf;
        cv::imencode(".jpg", dst, buf);
        output.assign(buf.begin(), buf.end());
        return true;

    } catch (const cv::Exception& e) {
        std::cerr << "[Processor] OpenCV 异常: " << e.what() << std::endl;
        return false;
    }
}

// --- 算法实现 ---

void ImageProcessor::makeGray(const cv::Mat& src, cv::Mat& dst) {
    cv::cvtColor(src, dst, cv::COLOR_BGR2GRAY);
}

void ImageProcessor::detectEdges(const cv::Mat& src, cv::Mat& dst) {
    cv::Mat gray;
    cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY); // 先转灰度提高效果
    cv::Canny(gray, dst, 100, 200);
}

void ImageProcessor::blurImage(const cv::Mat& src, cv::Mat& dst) {
    // 使用 15x15 的核进行高斯模糊
    cv::GaussianBlur(src, dst, cv::Size(15, 15), 0);
}

void ImageProcessor::invertImage(const cv::Mat& src, cv::Mat& dst) {
    // 图像反色：255 - pixel_value
    cv::bitwise_not(src, dst);
}

void ImageProcessor::thresholdImage(const cv::Mat& src, cv::Mat& dst) {
    cv::Mat gray;
    cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
    // 大津法自动寻找阈值进行二值化
    cv::threshold(gray, dst, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
}
void ImageProcessor::applyCartoonEffect(const cv::Mat& src, cv::Mat& dst){
    // 1. 边缘检测：利用中值滤波和自适应阈值提取轮廓
    cv::Mat gray, blur, edges;
    cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
    cv::medianBlur(gray, blur, 7); // 中值滤波去噪
    cv::adaptiveThreshold(blur, edges, 255, cv::ADAPTIVE_THRESH_MEAN_C, cv::THRESH_BINARY, 9, 2);
    
    // 2. 色彩平滑：使用双边滤波在保留边缘的同时抹平细节，产生色块效果
    cv::Mat color;
    cv::bilateralFilter(src, color, 9, 300, 300);
    
    // 3. 融合：将黑色轮廓叠加到平滑后的彩色图像上
    dst = cv::Mat::zeros(src.size(), src.type());
    color.copyTo(dst, edges);
    
    std::cout << "[Processor] 卡通化处理完成 | 尺寸: " << src.cols << "x" << src.rows << std::endl;
}
void ImageProcessor::runSuperResolution(const cv::Mat& src, cv::Mat& dst) {
    int org_w = src.cols;
    int org_h = src.rows;

    // 1. 预处理：Resize 到模型要求的 224x224
    cv::Mat resized;
    cv::resize(src, resized, cv::Size(224, 224));

    // 2. 颜色空间转换：提取 Y 通道
    cv::Mat ycrcb;
    cv::cvtColor(resized, ycrcb, cv::COLOR_BGR2YCrCb);
    std::vector<cv::Mat> channels;
    cv::split(ycrcb, channels);

    // 3. 归一化 Y 通道 (修正：此处仅声明一次)
    cv::Mat float_y;
    channels[0].convertTo(float_y, CV_32F, 1.0 / 255.0);

    // 验证输入数据调试信息
    float debug_pixel = float_y.at<float>(112, 112);
    std::cout << "[SR_DEBUG] V2.3 Logic | Input(112,112): " << debug_pixel 
              << " | Src: " << org_w << "x" << org_h << " -> Target: 224x224" << std::endl;

    // 4. 构建张量 (完全强制重新排列内存，解决条纹/浆糊问题)
    std::vector<float> input_tensor_values;
    if (float_y.isContinuous()) {
        input_tensor_values.assign((float*)float_y.datastart, (float*)float_y.dataend);
    } else {
        for (int i = 0; i < float_y.rows; ++i) {
            input_tensor_values.insert(input_tensor_values.end(), 
                                       float_y.ptr<float>(i), 
                                       float_y.ptr<float>(i) + float_y.cols);
        }
    }

    std::vector<int64_t> input_dims = {1, 1, 224, 224};
    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);

    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info, 
        input_tensor_values.data(), 
        input_tensor_values.size(), 
        input_dims.data(), 
        input_dims.size());

    // 5. 推理执行
    const char* input_names[] = {"input"};
    const char* output_names[] = {"output"};
    auto output_tensors = session->Run(Ort::RunOptions{nullptr}, input_names, &input_tensor, 1, output_names, 1);

    // 6. 后处理还原
    float* output_data = output_tensors[0].GetTensorMutableData<float>();
    cv::Mat result_y_raw(224, 224, CV_32F, output_data);
    
    // 强制截断与反归一化：防止输出超出 [0,1] 导致像素溢出
    cv::Mat result_y_8u;
    cv::Mat result_y_clamped;
    cv::threshold(result_y_raw, result_y_clamped, 1.0, 1.0, cv::THRESH_TRUNC);
    cv::threshold(result_y_clamped, result_y_clamped, 0.0, 0.0, cv::THRESH_TOZERO);
    result_y_clamped.convertTo(result_y_8u, CV_8U, 255.0);

    // 7. 关键修复：确保 Cr 和 Cb 通道也是 224x224，防止 merge 错位
    cv::Mat cr_resized, cb_resized;
    cv::resize(channels[1], cr_resized, cv::Size(224, 224));
    cv::resize(channels[2], cb_resized, cv::Size(224, 224));

    std::vector<cv::Mat> merged_channels = { result_y_8u, cr_resized, cb_resized };
    cv::Mat merged_ycrcb;
    cv::merge(merged_channels, merged_ycrcb);
    
    cv::Mat out_bgr;
    cv::cvtColor(merged_ycrcb, out_bgr, cv::COLOR_YCrCb2BGR);

    // 8. 还原/放大输出尺寸
    cv::resize(out_bgr, dst, cv::Size(org_w * 2, org_h * 2));
}