#pragma once
#include <opencv2/opencv.hpp>
#include <vector>
#include <cstdint>

constexpr int RVAC_FRAME_WIDTH = 1920;
constexpr int RVAC_FRAME_HEIGHT = 1080;
constexpr size_t RVAC_FRAME_CAPACITY =
    static_cast<size_t>(RVAC_FRAME_WIDTH) * RVAC_FRAME_HEIGHT;

cv::Mat blob_to_frame(const std::vector<uint8_t>& blob);
std::vector<uint8_t> frame_to_blob(const cv::Mat& frame);