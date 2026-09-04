#pragma once

#include <SKSEMenuFramework.h>

namespace MPL::MenuControlCorners
{
    class Scope
    {
    public:
        Scope();
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

    bool SliderFloat(const char* a_label, float* a_value, float a_minimum, float a_maximum,
        const char* a_format = "%.3f");
    bool SliderValueInput(const char* a_label, float* a_value, const char* a_format);
    void CompressionGauge(float a_width, float a_darkLimit, float a_brightLimit);
}
