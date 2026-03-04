#pragma once

enum class screenshot_format : int {
    AVIF = 0,
    JXL = 1
};

// Global variable declaration
extern screenshot_format format;