#include "processor.h"
#include <iostream>

ImageProcessor::ImageProcessor() : env(ORT_LOGGING_LEVEL_ERROR, "Inference") {
    //初始化推理设置
    session_options.SetIntraOpNumThreads(0);
    session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    //模型效果不好，java那边移除调用，加载模型的代码暂时保留
    const char* model_path = "models/super_resolution.onnx";
    session = std::make_unique<Ort::Session>(env, model_path, session_options);
}

bool ImageProcessor::process(int type, const std::string& input, std::string& output) {
    try {
        //解码
        std::vector<uchar> data(input.begin(), input.end());
        cv::Mat src = cv::imdecode(data, cv::IMREAD_COLOR);
        if (src.empty()) {
            std::cerr << "[Processor] 解码失败，数据为空或格式错误" << std::endl;
            return false;
        }

        cv::Mat dst;
        //根据标签做算法分发
        switch (type) {
            case 1: //灰度化
                makeGray(src, dst);
                break;
            case 2: //Canny边缘检测
                detectEdges(src, dst);
                break;
            case 3: //高斯模糊
                blurImage(src, dst);
                break;
            case 4: //反色处理
                invertImage(src, dst);
                break;
            case 5: //阈值分割 
                thresholdImage(src, dst);
                break;
            case 6: //卡通化
                applyCartoonEffect(src, dst);
                break;
            case 7: //超分辨率重建，保留冗余
                runSuperResolution(src, dst);
                break;
            default:
                std::cout << "[Processor] 未知类型，原样返回" << std::endl;
                src.copyTo(dst);
        }

        //编码准备返回
        std::vector<uchar> buf;
        cv::imencode(".jpg", dst, buf);
        output.assign(buf.begin(), buf.end());
        return true;

    } catch (const cv::Exception& e) {
        std::cerr << "[Processor] OpenCV 异常: " << e.what() << std::endl;
        return false;
    }
}


//灰度化算法
void ImageProcessor::makeGray(const cv::Mat& src, cv::Mat& dst) {
    cv::cvtColor(src, dst, cv::COLOR_BGR2GRAY);
}
//边缘检测
void ImageProcessor::detectEdges(const cv::Mat& src, cv::Mat& dst) {
    cv::Mat gray;
    cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY); //转灰度提高效果
    cv::Canny(gray, dst, 100, 200);
}
//高斯模糊
void ImageProcessor::blurImage(const cv::Mat& src, cv::Mat& dst) {
    //15x15的核
    cv::GaussianBlur(src, dst, cv::Size(15, 15), 0);
}
//反色处理
void ImageProcessor::invertImage(const cv::Mat& src, cv::Mat& dst) {
    cv::bitwise_not(src, dst);
}
//阈值分割 
void ImageProcessor::thresholdImage(const cv::Mat& src, cv::Mat& dst) {
    cv::Mat gray;
    cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
    // 大津法自动寻找阈值进行二值化
    cv::threshold(gray, dst, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
}
//卡通化
void ImageProcessor::applyCartoonEffect(const cv::Mat& src, cv::Mat& dst){
    //边缘检测，中值滤波和自适应阈值提取轮廓
    cv::Mat gray, blur, edges;
    cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
    cv::medianBlur(gray, blur, 7); //中值滤波去噪
    cv::adaptiveThreshold(blur, edges, 255, cv::ADAPTIVE_THRESH_MEAN_C, cv::THRESH_BINARY, 9, 2);
    
    //色彩平滑，双边滤波保留边缘，抹平细节，产生色块效果
    cv::Mat color;
    cv::bilateralFilter(src, color, 9, 300, 300);
    
    //将黑色轮廓叠加到平滑后的彩色图像上
    dst = cv::Mat::zeros(src.size(), src.type());
    color.copyTo(dst, edges);
}
//高分辨率重建
void ImageProcessor::runSuperResolution(const cv::Mat& src, cv::Mat& dst) {
    int org_w = src.cols;
    int org_h = src.rows;

    //预处理Resize到模型要求的224x224
    cv::Mat resized;
    cv::resize(src, resized, cv::Size(224, 224));

    //颜色空间转换，提取Y通道
    cv::Mat ycrcb;
    cv::cvtColor(resized, ycrcb, cv::COLOR_BGR2YCrCb);
    std::vector<cv::Mat> channels;
    cv::split(ycrcb, channels);

    //归一化Y通道
    cv::Mat float_y;
    channels[0].convertTo(float_y, CV_32F, 1.0 / 255.0);

    //加上调试看一下镜像更新好没有
    float debug_pixel = float_y.at<float>(112, 112);
    std::cout << "[SR_DEBUG] V2.3 Logic | Input(112,112): " << debug_pixel 
              << " | Src: " << org_w << "x" << org_h << " -> Target: 224x224" << std::endl;

    //构建张量
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

    //推理执行
    const char* input_names[] = {"input"};
    const char* output_names[] = {"output"};
    auto output_tensors = session->Run(Ort::RunOptions{nullptr}, input_names, &input_tensor, 1, output_names, 1);

    //后处理还原
    float* output_data = output_tensors[0].GetTensorMutableData<float>();
    cv::Mat result_y_raw(224, 224, CV_32F, output_data);
    
    //强制截断与反归一化
    cv::Mat result_y_8u;
    cv::Mat result_y_clamped;
    cv::threshold(result_y_raw, result_y_clamped, 1.0, 1.0, cv::THRESH_TRUNC);
    cv::threshold(result_y_clamped, result_y_clamped, 0.0, 0.0, cv::THRESH_TOZERO);
    result_y_clamped.convertTo(result_y_8u, CV_8U, 255.0);

    // Cr和Cb通道也是224x224，防merge错位
    cv::Mat cr_resized, cb_resized;
    cv::resize(channels[1], cr_resized, cv::Size(224, 224));
    cv::resize(channels[2], cb_resized, cv::Size(224, 224));

    std::vector<cv::Mat> merged_channels = { result_y_8u, cr_resized, cb_resized };
    cv::Mat merged_ycrcb;
    cv::merge(merged_channels, merged_ycrcb);
    
    cv::Mat out_bgr;
    cv::cvtColor(merged_ycrcb, out_bgr, cv::COLOR_YCrCb2BGR);

    //放大输出尺寸
    cv::resize(out_bgr, dst, cv::Size(org_w * 2, org_h * 2));
}