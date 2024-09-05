#include <cstring>
#include <variant>
#include <unordered_map>

#include "reshade_effect_manager.hpp"
#include "log.hpp"

#include "steamcompmgr.hpp"

#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include "effect_preprocessor.hpp"
#include "gamescope-reshade-protocol.h"

#include "reshade_api_format.hpp"
#include "convar.h"

#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize.h>

#include <mutex>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <iostream>

// This is based on wl_array_for_each from `wayland-util.h` in the Wayland client library.
#define uint8_array_for_each(pos, data, size) \
	for (pos = (decltype(pos))data; (const char *)pos < ((const char *)data + size); (pos)++)

static char* g_reshadeEffectPath = nullptr;
static std::function<void(const char*)> g_effectReadyCallback = nullptr;
static auto g_runtimeUniforms = std::unordered_map<std::string, uint8_t*>();
static std::mutex g_runtimeUniformsMutex;

const char *homedir;

static std::string GetLocalUsrDir()
{
    const char *homedir = nullptr;

    if ((homedir = getenv("HOME")) == nullptr)
        homedir = getpwuid(getuid())->pw_dir;

    return std::string(homedir) + "/.local";
}

static std::string GetUsrDir()
{
    return "/usr";
}

static LogScope reshade_log("gamescope_reshade");

///////////////
// Uniforms
///////////////

class ReshadeUniform
{
public:
    ReshadeUniform(const reshadefx::uniform_info& info);
    virtual ~ReshadeUniform() {};

    virtual void update(void* mappedBuffer) = 0;

protected:

    void copy(void* mappedBuffer, const void* data, size_t size);

    template <typename T>
    void copy(void* mappedBuffer, const T* thing);

    reshadefx::uniform_info m_info;
};

class FrameTimeUniform : public ReshadeUniform
{
public:
    FrameTimeUniform(reshadefx::uniform_info uniformInfo);
    virtual void update(void* mappedBuffer) override;
    virtual ~FrameTimeUniform();

private:
    std::chrono::time_point<std::chrono::high_resolution_clock> lastFrame;
};

class FrameCountUniform : public ReshadeUniform
{
public:
    FrameCountUniform(reshadefx::uniform_info uniformInfo);
    virtual void update(void* mappedBuffer) override;
    virtual ~FrameCountUniform();

private:
    int32_t count = 0;
};

class DateUniform : public ReshadeUniform
{
public:
    DateUniform(reshadefx::uniform_info uniformInfo);
    virtual void update(void* mappedBuffer) override;
    virtual ~DateUniform();
};

class TimerUniform : public ReshadeUniform
{
public:
    TimerUniform(reshadefx::uniform_info uniformInfo);
    virtual void update(void* mappedBuffer) override;
    virtual ~TimerUniform();

private:
    std::chrono::time_point<std::chrono::high_resolution_clock> start;
};

class PingPongUniform : public ReshadeUniform
{
public:
    PingPongUniform(reshadefx::uniform_info uniformInfo);
    virtual void update(void* mappedBuffer) override;
    virtual ~PingPongUniform();

private:
    std::chrono::time_point<std::chrono::high_resolution_clock> lastFrame;

    float min             = 0.0f;
    float max             = 0.0f;
    float stepMin         = 0.0f;
    float stepMax         = 0.0f;
    float smoothing       = 0.0f;
    float currentValue[2] = {0.0f, 1.0f};
};

class RandomUniform : public ReshadeUniform
{
public:
    RandomUniform(reshadefx::uniform_info uniformInfo);
    virtual void update(void* mappedBuffer) override;
    virtual ~RandomUniform();

private:
    int max = 0;
    int min = 0;
};

class KeyUniform : public ReshadeUniform
{
public:
    KeyUniform(reshadefx::uniform_info uniformInfo);
    virtual void update(void* mappedBuffer) override;
    virtual ~KeyUniform();
};

class MouseButtonUniform : public ReshadeUniform
{
public:
    MouseButtonUniform(reshadefx::uniform_info uniformInfo);
    virtual void update(void* mappedBuffer) override;
    virtual ~MouseButtonUniform();
};

class MousePointUniform : public ReshadeUniform
{
public:
    MousePointUniform(reshadefx::uniform_info uniformInfo);
    virtual void update(void* mappedBuffer) override;
    virtual ~MousePointUniform();
};

class MouseDeltaUniform : public ReshadeUniform
{
public:
    MouseDeltaUniform(reshadefx::uniform_info uniformInfo);
    virtual void update(void* mappedBuffer) override;
    virtual ~MouseDeltaUniform();
};

class DepthUniform : public ReshadeUniform
{
public:
    DepthUniform(reshadefx::uniform_info uniformInfo);
    virtual void update(void* mappedBuffer) override;
    virtual ~DepthUniform();
};

class RuntimeUniform : public ReshadeUniform
{
public:
    RuntimeUniform(reshadefx::uniform_info uniformInfo);
    void virtual update(void* mappedBuffer) override;
    virtual ~RuntimeUniform();

private:
    uint32_t offset;
    uint32_t size;
    std::string name;
    reshadefx::type type;
    std::variant<std::monostate, std::vector<float>, std::vector<int32_t>, std::vector<uint32_t>> defaultValue;
};

class DataUniform : public ReshadeUniform
{
public:
    DataUniform(reshadefx::uniform_info uniformInfo);
    virtual void update(void* mappedBuffer) override;
    virtual ~DataUniform();
};

//////////////////////////////////////////////////////////////////////////////////////////////////////////
ReshadeUniform::ReshadeUniform(const reshadefx::uniform_info& info)
    : m_info(info)
{
}

void ReshadeUniform::copy(void* mappedBuffer, const void* data, size_t size)
{
    assert(size <= m_info.size);
    std::memcpy(((uint8_t*)mappedBuffer) + m_info.offset, data, size);
}

template <typename T>
void ReshadeUniform::copy(void* mappedBuffer, const T* thing)
{
    assert(m_info.type.array_length == 0 || m_info.type.array_length == 1);

    uint32_t zero_data[16] = {};
    const auto copy_ = [&](const auto constantDatatype){
        T array_stuff[16] = {};
        const auto thingBuffer = [&](){
            for (uint32_t i = 0; i < m_info.type.components(); i++)
                array_stuff[i] = (m_info.type.base == reshadefx::type::t_bool) ? !!(thing[i]) : thing[i];
            return array_stuff;
        };
        const auto defaultOrZeroBuffer = (m_info.has_initializer_value) ? static_cast<const void*>(std::begin((m_info.initializer_value).*constantDatatype)) : zero_data;
        copy(mappedBuffer, (thing) ? thingBuffer() : defaultOrZeroBuffer, sizeof(T) * m_info.type.components());
    };

    switch (m_info.type.base)
    {
    case reshadefx::type::t_bool:   // VkBool32 = uint32_t;
        copy_(&reshadefx::constant::as_uint);
        break;
    case reshadefx::type::t_int:
        copy_(&reshadefx::constant::as_int);
        break;
    case reshadefx::type::t_uint:
        copy_(&reshadefx::constant::as_uint);
        break;
    case reshadefx::type::t_float:
        copy_(&reshadefx::constant::as_float);
        break;
    default:
        reshade_log.errorf("Unknown uniform type!");
        break;
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
FrameTimeUniform::FrameTimeUniform(reshadefx::uniform_info uniformInfo)
    : ReshadeUniform(uniformInfo)
{
    lastFrame = std::chrono::high_resolution_clock::now();
}
void FrameTimeUniform::update(void* mappedBuffer)
{
    auto                                     currentFrame = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float, std::milli> duration     = currentFrame - lastFrame;
    lastFrame                                             = currentFrame;
    float frametime                                       = duration.count();

    copy(mappedBuffer, &frametime);
}
FrameTimeUniform::~FrameTimeUniform()
{
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
FrameCountUniform::FrameCountUniform(reshadefx::uniform_info uniformInfo)
    : ReshadeUniform(uniformInfo)
{
}
void FrameCountUniform::update(void* mappedBuffer)
{
    copy(mappedBuffer, &count);
    count++;
}
FrameCountUniform::~FrameCountUniform()
{
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
DateUniform::DateUniform(reshadefx::uniform_info uniformInfo)
    : ReshadeUniform(uniformInfo)
{
}
void DateUniform::update(void* mappedBuffer)
{
    auto        now         = std::chrono::system_clock::now();
    std::time_t nowC        = std::chrono::system_clock::to_time_t(now);
    struct tm*  currentTime = std::localtime(&nowC);
    float       year        = 1900.0f + static_cast<float>(currentTime->tm_year);
    float       month       = 1.0f + static_cast<float>(currentTime->tm_mon);
    float       day         = static_cast<float>(currentTime->tm_mday);
    float       seconds     = static_cast<float>((currentTime->tm_hour * 60 + currentTime->tm_min) * 60 + currentTime->tm_sec);
    float       date[]      = {year, month, day, seconds};

    copy(mappedBuffer, date);
}
DateUniform::~DateUniform()
{
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
TimerUniform::TimerUniform(reshadefx::uniform_info uniformInfo)
    : ReshadeUniform(uniformInfo)
{
    start  = std::chrono::high_resolution_clock::now();
}
void TimerUniform::update(void* mappedBuffer)
{
    auto                                     currentFrame = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float, std::milli> duration     = currentFrame - start;
    float                                    timer        = duration.count();

    copy(mappedBuffer, &timer);
}
TimerUniform::~TimerUniform()
{
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
PingPongUniform::PingPongUniform(reshadefx::uniform_info uniformInfo)
    : ReshadeUniform(uniformInfo)
{
    const auto matchesAnnotationName = [&](const auto& name){ return std::ranges::find_if(uniformInfo.annotations, std::bind_front(std::equal_to{}, name), &reshadefx::annotation::name);};
    if (auto minAnnotation = matchesAnnotationName("min");
        minAnnotation != uniformInfo.annotations.end())
    {
        min = minAnnotation->type.is_floating_point() ? minAnnotation->value.as_float[0] : static_cast<float>(minAnnotation->value.as_int[0]);
    }
    if (auto maxAnnotation = matchesAnnotationName("max");
        maxAnnotation != uniformInfo.annotations.end())
    {
        max = maxAnnotation->type.is_floating_point() ? maxAnnotation->value.as_float[0] : static_cast<float>(maxAnnotation->value.as_int[0]);
    }
    if (auto smoothingAnnotation = matchesAnnotationName("smoothing");
        smoothingAnnotation != uniformInfo.annotations.end())
    {
        smoothing = smoothingAnnotation->type.is_floating_point() ? smoothingAnnotation->value.as_float[0]
                                                                    : static_cast<float>(smoothingAnnotation->value.as_int[0]);
    }
    if (auto stepAnnotation = matchesAnnotationName("step");
        stepAnnotation != uniformInfo.annotations.end())
    {
        stepMin =
            stepAnnotation->type.is_floating_point() ? stepAnnotation->value.as_float[0] : static_cast<float>(stepAnnotation->value.as_int[0]);
        stepMax =
            stepAnnotation->type.is_floating_point() ? stepAnnotation->value.as_float[1] : static_cast<float>(stepAnnotation->value.as_int[1]);
    }

    lastFrame = std::chrono::high_resolution_clock::now();
}
void PingPongUniform::update(void* mappedBuffer)
{
    auto currentFrame = std::chrono::high_resolution_clock::now();

    std::chrono::duration<float, std::ratio<1>> frameTime = currentFrame - lastFrame;

    float increment = stepMax == 0 ? stepMin : (stepMin + std::fmod(static_cast<float>(std::rand()), stepMax - stepMin + 1.0f));
    if (currentValue[1] >= 0)
    {
        increment = std::max(increment - std::max(0.0f, smoothing - (max - currentValue[0])), 0.05f);
        increment *= frameTime.count();

        if ((currentValue[0] += increment) >= max)
        {
            currentValue[0] = max, currentValue[1] = -1.0f;
        }
    }
    else
    {
        increment = std::max(increment - std::max(0.0f, smoothing - (currentValue[0] - min)), 0.05f);
        increment *= frameTime.count();

        if ((currentValue[0] -= increment) <= min)
        {
            currentValue[0] = min, currentValue[1] = 1.0f;
        }
    }
    copy(mappedBuffer, currentValue);
}
PingPongUniform::~PingPongUniform()
{
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
RandomUniform::RandomUniform(reshadefx::uniform_info uniformInfo)
    : ReshadeUniform(uniformInfo)
{
    if (auto minAnnotation =
            std::ranges::find_if(uniformInfo.annotations, std::bind_front(std::equal_to{}, "min"), &reshadefx::annotation::name);
        minAnnotation != uniformInfo.annotations.end())
    {
        min = minAnnotation->type.is_integral() ? minAnnotation->value.as_int[0] : static_cast<int>(minAnnotation->value.as_float[0]);
    }
    if (auto maxAnnotation =
            std::ranges::find_if(uniformInfo.annotations, std::bind_front(std::equal_to{}, "max"), &reshadefx::annotation::name);
        maxAnnotation != uniformInfo.annotations.end())
    {
        max = maxAnnotation->type.is_integral() ? maxAnnotation->value.as_int[0] : static_cast<int>(maxAnnotation->value.as_float[0]);
    }
}
void RandomUniform::update(void* mappedBuffer)
{
    int32_t value = min + (std::rand() % (max - min + 1));
    copy(mappedBuffer, &value);
}
RandomUniform::~RandomUniform()
{
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
KeyUniform::KeyUniform(reshadefx::uniform_info uniformInfo)
    : ReshadeUniform(uniformInfo)
{
}
void KeyUniform::update(void* mappedBuffer)
{
    VkBool32 keyDown = VK_FALSE; // TODO
    copy(mappedBuffer, &keyDown);
}
KeyUniform::~KeyUniform()
{
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
MouseButtonUniform::MouseButtonUniform(reshadefx::uniform_info uniformInfo)
    : ReshadeUniform(uniformInfo)
{
}
void MouseButtonUniform::update(void* mappedBuffer)
{
    VkBool32 keyDown = VK_FALSE; // TODO
    copy(mappedBuffer, &keyDown);
}
MouseButtonUniform::~MouseButtonUniform()
{
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
MousePointUniform::MousePointUniform(reshadefx::uniform_info uniformInfo)
    : ReshadeUniform(uniformInfo)
{
}
void MousePointUniform::update(void* mappedBuffer)
{
    MouseCursor *cursor = steamcompmgr_get_current_cursor();
    int32_t point[2] = {0, 0};
    if (cursor)
    {
        point[0] = cursor->x();
        point[1] = cursor->y();
    }
    copy(mappedBuffer, point);
}
MousePointUniform::~MousePointUniform()
{
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
MouseDeltaUniform::MouseDeltaUniform(reshadefx::uniform_info uniformInfo)
    : ReshadeUniform(uniformInfo)
{
}

void MouseDeltaUniform::update(void* mappedBuffer)
{
    float delta[2] = {0.0f, 0.0f}; // TODO
    copy(mappedBuffer, delta);
}
MouseDeltaUniform::~MouseDeltaUniform()
{
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
DepthUniform::DepthUniform(reshadefx::uniform_info uniformInfo)
    : ReshadeUniform(uniformInfo)
{
}
void DepthUniform::update(void* mappedBuffer)
{
    VkBool32 hasDepth = VK_FALSE;
    copy(mappedBuffer, &hasDepth);
}
DepthUniform::~DepthUniform()
{
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
RuntimeUniform::RuntimeUniform(reshadefx::uniform_info uniformInfo)
    : ReshadeUniform(uniformInfo)
{
    offset = uniformInfo.offset;
    size   = uniformInfo.size;
    type   = uniformInfo.type;
    name   = std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "source"; })->value.string_data;

    if (auto defaultValueAnnotation =
            std::find_if(uniformInfo.annotations.begin(), uniformInfo.annotations.end(), [](const auto& a) { return a.name == "defaultValue"; });
        defaultValueAnnotation != uniformInfo.annotations.end())
    {
        reshadefx::constant value = defaultValueAnnotation->value;
        if (type.is_floating_point()) {
            defaultValue = std::vector<float>(value.as_float, value.as_float + type.components());
            reshade_log.debugf("Found float* runtime uniform %s of size %d\n", name.c_str(), type.components());
        } else if (type.is_boolean()) {
            defaultValue = std::vector<uint32_t>(value.as_uint, value.as_uint + type.components());
            reshade_log.debugf("Found bool* runtime uniform %s of size %d\n", name.c_str(), type.components());
        } else if (type.is_numeric()) {
            if (type.is_signed()) {
                defaultValue = std::vector<int32_t>(value.as_int, value.as_int + type.components());
                reshade_log.debugf("Found int32_t* runtime uniform %s of size %d\n", name.c_str(), type.components());
            } else {
                defaultValue = std::vector<uint32_t>(value.as_uint, value.as_uint + type.components());
                reshade_log.debugf("Found uint32_t* runtime uniform %s of size %d\n", name.c_str(), type.components());
            }
        } else {
            reshade_log.errorf("Tried to create a runtime uniform variable of an unsupported type\n");
        }
    }
}
void RuntimeUniform::update(void* mappedBuffer)
{
    std::variant<std::monostate, std::vector<float>, std::vector<int32_t>, std::vector<uint32_t>> value;
    uint8_t* wl_value = nullptr;

    std::lock_guard<std::mutex> lock(g_runtimeUniformsMutex);
    auto it = g_runtimeUniforms.find(name);
    if (it != g_runtimeUniforms.end()) {
        wl_value = it->second;
    }

    if (wl_value) {
        if (type.is_floating_point()) {
            value = std::vector<float>();
            float *float_value = nullptr;
            uint8_array_for_each(float_value, wl_value, type.components() * sizeof(float)) {
                std::get<std::vector<float>>(value).push_back(*float_value);
            }
        } else if (type.is_boolean()) {
            // convert to a uint32_t vector, that's how the reshade uniform code understands booleans
            value = std::vector<uint32_t>();
            uint8_t *bool_value = nullptr;
            uint8_array_for_each(bool_value, wl_value, type.components() * sizeof(uint8_t)) {
                std::get<std::vector<uint32_t>>(value).push_back(*bool_value);
            }
        } else if (type.is_numeric()) {
            if (type.is_signed()) {
                value = std::vector<int32_t>();
                int32_t *int_value = nullptr;
                uint8_array_for_each(int_value, wl_value, type.components() * sizeof(int32_t)) {
                    std::get<std::vector<int32_t>>(value).push_back(*int_value);
                }
            } else {
                value = std::vector<uint32_t>();
                uint32_t *uint_value = nullptr;
                uint8_array_for_each(uint_value, wl_value, type.components() * sizeof(uint32_t)) {
                    std::get<std::vector<uint32_t>>(value).push_back(*uint_value);
                }
            }
        }
    }

    if (std::holds_alternative<std::monostate>(value)) {
        value = defaultValue;
    }

    if (std::holds_alternative<std::vector<float>>(value)) {
        std::vector<float>& vec = std::get<std::vector<float>>(value);
        std::memcpy((uint8_t*) mappedBuffer + offset, vec.data(), vec.size() * sizeof(float));
    } else if (std::holds_alternative<std::vector<int32_t>>(value)) {
        std::vector<int32_t>& vec = std::get<std::vector<int32_t>>(value);
        std::memcpy((uint8_t*) mappedBuffer + offset, vec.data(), vec.size() * sizeof(int32_t));
    } else if (std::holds_alternative<std::vector<uint32_t>>(value)) {
        std::vector<uint32_t>& vec = std::get<std::vector<uint32_t>>(value);
        std::memcpy((uint8_t*) mappedBuffer + offset, vec.data(), vec.size() * sizeof(uint32_t));
    }
}
RuntimeUniform::~RuntimeUniform()
{
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
DataUniform::DataUniform(reshadefx::uniform_info uniformInfo)
    : ReshadeUniform(uniformInfo)
{
}
void DataUniform::update(void* mappedBuffer)
{
    copy<uint32_t>(mappedBuffer, nullptr);
}
DataUniform::~DataUniform()
{
}

static std::vector<std::shared_ptr<ReshadeUniform>> createReshadeUniforms(const reshadefx::module& module)
{
    std::vector<std::shared_ptr<ReshadeUniform>> uniforms;
    for (auto& uniform : module.uniforms)
    {
        auto sourceAnnotation = std::ranges::find_if(uniform.annotations, std::bind_front(std::equal_to{}, "source"), &reshadefx::annotation::name);
        if (sourceAnnotation == uniform.annotations.end())
        {
            uniforms.push_back(std::make_shared<DataUniform>(uniform));
            continue;
        }
        else
        {
            auto& source = sourceAnnotation->value.string_data;
            if (source == "frametime")
            {
                uniforms.push_back(std::make_shared<FrameTimeUniform>(uniform));
            }
            else if (source == "framecount")
            {
                uniforms.push_back(std::make_shared<FrameCountUniform>(uniform));
            }
            else if (source == "date")
            {
                uniforms.push_back(std::make_shared<DateUniform>(uniform));
            }
            else if (source == "timer")
            {
                uniforms.push_back(std::make_shared<TimerUniform>(uniform));
            }
            else if (source == "pingpong")
            {
                uniforms.push_back(std::make_shared<PingPongUniform>(uniform));
            }
            else if (source == "random")
            {
                uniforms.push_back(std::make_shared<RandomUniform>(uniform));
            }
            else if (source == "key")
            {
                uniforms.push_back(std::make_shared<KeyUniform>(uniform));
            }
            else if (source == "mousebutton")
            {
                uniforms.push_back(std::make_shared<MouseButtonUniform>(uniform));
            }
            else if (source == "mousepoint")
            {
                uniforms.push_back(std::make_shared<MousePointUniform>(uniform));
            }
            else if (source == "mousedelta")
            {
                uniforms.push_back(std::make_shared<MouseDeltaUniform>(uniform));
            }
            else if (source == "bufready_depth")
            {
                uniforms.push_back(std::make_shared<DepthUniform>(uniform));
            }
            else if (!source.empty())
            {
                uniforms.push_back(std::make_shared<RuntimeUniform>(uniform));
            }
        }
    }
    return uniforms;
}

//

static reshade::api::color_space ConvertToReshadeColorSpace(GamescopeAppTextureColorspace colorspace)
{
	switch (colorspace)
	{
	default:
		return reshade::api::color_space::unknown;
	case GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR: /* Actually SRGB -> Linear... I should change this cause its confusing */
	case GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB:
		return reshade::api::color_space::srgb_nonlinear;
	case GAMESCOPE_APP_TEXTURE_COLORSPACE_SCRGB:
		return reshade::api::color_space::extended_srgb_linear;
	case GAMESCOPE_APP_TEXTURE_COLORSPACE_HDR10_PQ:
		return reshade::api::color_space::hdr10_st2084;
	}
}

static VkFormat ConvertReshadeFormat(reshadefx::texture_format texFormat)
{
    switch (texFormat)
    {
        case reshadefx::texture_format::r8: return VK_FORMAT_R8_UNORM;
        case reshadefx::texture_format::r16f: return VK_FORMAT_R16_SFLOAT;
        case reshadefx::texture_format::r32f: return VK_FORMAT_R32_SFLOAT;
        case reshadefx::texture_format::rg8: return VK_FORMAT_R8G8_UNORM;
        case reshadefx::texture_format::rg16: return VK_FORMAT_R16G16_UNORM;
        case reshadefx::texture_format::rg16f: return VK_FORMAT_R16G16_SFLOAT;
        case reshadefx::texture_format::rg32f: return VK_FORMAT_R32G32_SFLOAT;
        case reshadefx::texture_format::rgba8: return VK_FORMAT_R8G8B8A8_UNORM;
        case reshadefx::texture_format::rgba16: return VK_FORMAT_R16G16B16A16_UNORM;
        case reshadefx::texture_format::rgba16f: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case reshadefx::texture_format::rgba32f: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case reshadefx::texture_format::rgb10a2: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        default:
            reshade_log.errorf("Couldn't convert texture format: %d\n", static_cast<int>(texFormat));
            return VK_FORMAT_UNDEFINED;
    }
}

#if 0
static VkCompareOp ConvertReshadeCompareOp(reshadefx::pass_stencil_func compareOp)
{
    switch (compareOp)
    {
        case reshadefx::pass_stencil_func::never: return VK_COMPARE_OP_NEVER;
        case reshadefx::pass_stencil_func::less: return VK_COMPARE_OP_LESS;
        case reshadefx::pass_stencil_func::equal: return VK_COMPARE_OP_EQUAL;
        case reshadefx::pass_stencil_func::less_equal: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case reshadefx::pass_stencil_func::greater: return VK_COMPARE_OP_GREATER;
        case reshadefx::pass_stencil_func::not_equal: return VK_COMPARE_OP_NOT_EQUAL;
        case reshadefx::pass_stencil_func::greater_equal: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case reshadefx::pass_stencil_func::always: return VK_COMPARE_OP_ALWAYS;
        default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkStencilOp ConvertReshadeStencilOp(reshadefx::pass_stencil_op stencilOp)
{
    switch (stencilOp)
    {
        case reshadefx::pass_stencil_op::zero: return VK_STENCIL_OP_ZERO;
        case reshadefx::pass_stencil_op::keep: return VK_STENCIL_OP_KEEP;
        case reshadefx::pass_stencil_op::replace: return VK_STENCIL_OP_REPLACE;
        case reshadefx::pass_stencil_op::increment_saturate: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case reshadefx::pass_stencil_op::decrement_saturate: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case reshadefx::pass_stencil_op::invert: return VK_STENCIL_OP_INVERT;
        case reshadefx::pass_stencil_op::increment: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case reshadefx::pass_stencil_op::decrement: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}
#endif

static VkBlendOp ConvertReshadeBlendOp(reshadefx::pass_blend_op blendOp)
{
    switch (blendOp)
    {
        case reshadefx::pass_blend_op::add: return VK_BLEND_OP_ADD;
        case reshadefx::pass_blend_op::subtract: return VK_BLEND_OP_SUBTRACT;
        case reshadefx::pass_blend_op::reverse_subtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case reshadefx::pass_blend_op::min: return VK_BLEND_OP_MIN;
        case reshadefx::pass_blend_op::max: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

static VkBlendFactor ConvertReshadeBlendFactor(reshadefx::pass_blend_factor blendFactor)
{
    switch (blendFactor)
    {
        case reshadefx::pass_blend_factor::zero: return VK_BLEND_FACTOR_ZERO;
        case reshadefx::pass_blend_factor::one: return VK_BLEND_FACTOR_ONE;
        case reshadefx::pass_blend_factor::source_color: return VK_BLEND_FACTOR_SRC_COLOR;
        case reshadefx::pass_blend_factor::source_alpha: return VK_BLEND_FACTOR_SRC_ALPHA;
        case reshadefx::pass_blend_factor::one_minus_source_color: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case reshadefx::pass_blend_factor::one_minus_source_alpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case reshadefx::pass_blend_factor::dest_alpha: return VK_BLEND_FACTOR_DST_ALPHA;
        case reshadefx::pass_blend_factor::one_minus_dest_alpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case reshadefx::pass_blend_factor::dest_color: return VK_BLEND_FACTOR_DST_COLOR;
        case reshadefx::pass_blend_factor::one_minus_dest_color: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        default: return VK_BLEND_FACTOR_ZERO;
    }
}

static VkSamplerAddressMode ConvertReshadeAddressMode(reshadefx::texture_address_mode addressMode)
{
    switch (addressMode)
    {
        case reshadefx::texture_address_mode::wrap: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case reshadefx::texture_address_mode::mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case reshadefx::texture_address_mode::clamp: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case reshadefx::texture_address_mode::border: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

static void ConvertReshadeFilter(const reshadefx::filter_mode& textureFilter, VkFilter& minFilter, VkFilter& magFilter, VkSamplerMipmapMode& mipmapMode)
{
    switch (textureFilter)
    {
        case reshadefx::filter_mode::min_mag_mip_point:
            minFilter  = VK_FILTER_NEAREST;
            magFilter  = VK_FILTER_NEAREST;
            mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            return;
        case reshadefx::filter_mode::min_mag_point_mip_linear:
            minFilter  = VK_FILTER_NEAREST;
            magFilter  = VK_FILTER_NEAREST;
            mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            return;
        case reshadefx::filter_mode::min_point_mag_linear_mip_point:
            minFilter  = VK_FILTER_NEAREST;
            magFilter  = VK_FILTER_LINEAR;
            mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            return;
        case reshadefx::filter_mode::min_point_mag_mip_linear:
            minFilter  = VK_FILTER_NEAREST;
            magFilter  = VK_FILTER_LINEAR;
            mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            return;
        case reshadefx::filter_mode::min_linear_mag_mip_point:
            minFilter  = VK_FILTER_LINEAR;
            magFilter  = VK_FILTER_NEAREST;
            mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            return;
        case reshadefx::filter_mode::min_linear_mag_point_mip_linear:
            minFilter  = VK_FILTER_LINEAR;
            magFilter  = VK_FILTER_NEAREST;
            mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            return;
        case reshadefx::filter_mode::min_mag_linear_mip_point:
            minFilter  = VK_FILTER_LINEAR;
            magFilter  = VK_FILTER_LINEAR;
            mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            return;

        default:
        case reshadefx::filter_mode::min_mag_mip_linear:
            minFilter  = VK_FILTER_LINEAR;
            magFilter  = VK_FILTER_LINEAR;
            mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            return;
    }
}

static uint32_t GetFormatBitDepth(VkFormat format)
{
    switch (format)
    {
    default:
    case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return 8;
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
        return 6;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM:
        return 16;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        return 10;
    }
}

ReshadeEffectPipeline::ReshadeEffectPipeline()
{
}

ReshadeEffectPipeline::~ReshadeEffectPipeline()
{
    m_device->waitIdle();

    for (auto& pipeline : m_pipelines)
        m_device->vk.DestroyPipeline(m_device->device(), pipeline, nullptr);
    m_pipelines.clear();

    for (auto& sampler : m_samplers)
        m_device->vk.DestroySampler(m_device->device(), sampler.sampler, nullptr);
    m_samplers.clear();

    m_uniforms.clear();

    m_textures.clear();
    m_rt = nullptr;

    m_cmdBuffer = std::nullopt;

    m_device->vk.DestroyBuffer(m_device->device(), m_buffer, nullptr);
    m_device->vk.FreeMemory(m_device->device(), m_bufferMemory, nullptr);
    m_mappedPtr = nullptr;

    for (uint32_t i = 0; i < GAMESCOPE_RESHADE_DESCRIPTOR_SET_COUNT; i++)
    {
        m_device->vk.FreeDescriptorSets(m_device->device(), m_descriptorPool, 1, &m_descriptorSets[i]);
        m_device->vk.DestroyDescriptorSetLayout(m_device->device(), m_descriptorSetLayouts[i], nullptr);
    }

    m_device->vk.DestroyDescriptorPool(m_device->device(), m_descriptorPool, nullptr);
    m_device->vk.DestroyPipelineLayout(m_device->device(), m_pipelineLayout, nullptr);
}

bool ReshadeEffectPipeline::init(CVulkanDevice *device, const ReshadeEffectKey &key)
{
    m_key = key;
    m_device = device;

	VkPhysicalDeviceProperties deviceProperties;
	device->vk.GetPhysicalDeviceProperties(device->physDev(), &deviceProperties);

	reshadefx::preprocessor pp;
	pp.add_macro_definition("__RESHADE__", std::to_string(INT_MAX));
	pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "0");
	pp.add_macro_definition("__VENDOR__", std::to_string(deviceProperties.vendorID));
	pp.add_macro_definition("__DEVICE__", std::to_string(deviceProperties.deviceID));
	pp.add_macro_definition("__RENDERER__", std::to_string(0x20000));
	pp.add_macro_definition("__APPLICATION__", std::to_string(0x0));
	pp.add_macro_definition("BUFFER_WIDTH", std::to_string(key.bufferWidth));
	pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(key.bufferHeight));
	pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
	pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
	pp.add_macro_definition("BUFFER_COLOR_SPACE", std::to_string(static_cast<uint32_t>(ConvertToReshadeColorSpace(key.bufferColorSpace))));
	pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", std::to_string(GetFormatBitDepth(key.bufferFormat)));
    pp.add_macro_definition("GAMESCOPE", "1");
    pp.add_macro_definition("GAMESCOPE_SDR_ON_HDR_NITS", std::to_string(g_ColorMgmt.pending.flSDROnHDRBrightness));

    std::string gamescope_reshade_share_path = "/share/gamescope/reshade";

    std::string local_reshade_path = GetLocalUsrDir() + gamescope_reshade_share_path;
    std::string global_reshade_path = GetUsrDir() + gamescope_reshade_share_path;

    pp.add_include_path(local_reshade_path + "/Shaders");
	pp.add_include_path(global_reshade_path + "/Shaders");

    std::string local_shader_file_path = local_reshade_path + "/Shaders/" + key.path;
    std::string global_shader_file_path = global_reshade_path + "/Shaders/" + key.path;

	if (!pp.append_file(local_shader_file_path))
	{
        if (!pp.append_file(global_shader_file_path))
        {
            reshade_log.errorf("Failed to load reshade fx file: %s (%s or %s) - %s", key.path.c_str(), local_shader_file_path.c_str(), global_shader_file_path.c_str(), pp.errors().c_str());
            return false;
        }
	}

	std::string errors = pp.errors();
	if (!errors.empty())
	{
		reshade_log.errorf("Failed to parse reshade fx shader module: %s", errors.c_str());
		return false;
	}

	std::unique_ptr<reshadefx::codegen> codegen(reshadefx::create_codegen_spirv(
		true /* vulkan semantics */, true /* debug info */, false /* uniforms to spec constants */, false /*flip vertex shader*/));

	reshadefx::parser parser;
	parser.parse(pp.output(), codegen.get());

	errors = parser.errors();
	if (!errors.empty())
	{
		reshade_log.errorf("Failed to parse reshade fx shader module: %s", errors.c_str());
		return false;
	}

	m_module = std::make_unique<reshadefx::module>();
	codegen->write_result(*m_module);

#if 0
    FILE *f = fopen("test.spv", "wb");
    fwrite(m_module->code.data(), 1, m_module->code.size(), f);
    fclose(f);
#endif

	if (m_module->techniques.size() <= key.techniqueIdx)
	{
		reshade_log.errorf("Invalid technique index");
		return false;
	}

	auto& technique = m_module->techniques[key.techniqueIdx];
	reshade_log.infof("Using technique: %s\n", technique.name.c_str());

    // Allocate command buffers
    {
		VkCommandBufferAllocateInfo commandBufferAllocateInfo =
        {
			.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool        = device->generalCommandPool(),
			.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1
		};

        VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
		VkResult result = device->vk.AllocateCommandBuffers(device->device(), &commandBufferAllocateInfo, &cmdBuffer);
		if (result != VK_SUCCESS)
		{
			reshade_log.errorf("vkAllocateCommandBuffers failed");
			return false;
		}

        m_cmdBuffer.emplace(device, cmdBuffer, device->generalQueue(), device->generalQueueFamily());
    }

    // Create Uniform Buffer
    {
        VkBufferCreateInfo bufferCreateInfo =
        {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size  = m_module->total_uniform_size,
            .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        };

        VkResult result = device->vk.CreateBuffer(device->device(), &bufferCreateInfo, nullptr, &m_buffer);
        if (result != VK_SUCCESS)
        {
            reshade_log.errorf("vkCreateBuffer failed");
            return false;
        }

        VkMemoryRequirements memRequirements;
        device->vk.GetBufferMemoryRequirements(device->device(), m_buffer, &memRequirements);

        uint32_t memTypeIndex = device->findMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, memRequirements.memoryTypeBits);
        assert(memTypeIndex != ~0u);
        VkMemoryAllocateInfo allocInfo =
        {
            .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize  = memRequirements.size,
            .memoryTypeIndex = memTypeIndex,
        };
        result = device->vk.AllocateMemory(device->device(), &allocInfo, nullptr, &m_bufferMemory);
        if (result != VK_SUCCESS)
        {
            reshade_log.errorf("vkAllocateMemory failed");
            return false;
        }
        device->vk.BindBufferMemory(device->device(), m_buffer, m_bufferMemory, 0);
        if (result != VK_SUCCESS)
        {
            reshade_log.errorf("vkBindBufferMemory failed");
            return false;
        }

        result = device->vk.MapMemory(device->device(), m_bufferMemory, 0, VK_WHOLE_SIZE, 0, &m_mappedPtr);
        if (result != VK_SUCCESS)
        {
            reshade_log.errorf("vkMapMemory failed");
            return false;
        }
    }

    // Create Uniforms
    m_uniforms = createReshadeUniforms(*m_module);

    // Create Textures
    {
        m_rt = new CVulkanTexture();
        CVulkanTexture::createFlags flags;
        flags.bSampled = true;
        flags.bStorage = true;
        flags.bColorAttachment = true;

        bool ret = m_rt->BInit(m_key.bufferWidth, m_key.bufferHeight, 1, VulkanFormatToDRM(m_key.bufferFormat), flags, nullptr);
        assert(ret);
    }

    for (const auto& tex : m_module->textures)
    {
        gamescope::Rc<CVulkanTexture> texture;
        if (tex.semantic.empty())
        {
            texture = new CVulkanTexture();
            CVulkanTexture::createFlags flags;
            flags.bSampled = true;
            // Always need storage.
            flags.bStorage = true;
            if (tex.render_target)
                flags.bColorAttachment = true;

            // Not supported rn.
            assert(tex.levels == 1);
            assert(tex.type == reshadefx::texture_type::texture_2d);

            bool ret = texture->BInit(tex.width, tex.height, tex.depth, VulkanFormatToDRM(ConvertReshadeFormat(tex.format)), flags, nullptr);
            assert(ret);
        }

        if (const auto source = std::ranges::find_if(tex.annotations , std::bind_front(std::equal_to{}, "source"), &reshadefx::annotation::name);
            source != tex.annotations.end())
        {
            std::string filePath = local_reshade_path + "/Textures/" + source->value.string_data;

            int w, h, channels;
            unsigned char *data = stbi_load(filePath.c_str(), &w, &h, &channels, STBI_rgb_alpha);

            if (!data)
            {
                filePath = global_reshade_path + "/Textures/" + source->value.string_data;
                data = stbi_load(filePath.c_str(), &w, &h, &channels, STBI_rgb_alpha);
            }

            if (data)
            {
                uint8_t *pixels = data;

                std::vector<uint8_t> resized_data;
                if (w != (int)texture->width() || h != (int)texture->height())
                {
                    resized_data.resize(texture->width() * texture->height() * 4);
                    stbir_resize_uint8(data, w, h, 0, resized_data.data(), texture->width(), texture->height(), 0, STBI_rgb_alpha);

                    w = texture->width();
                    h = texture->height();
                    pixels = resized_data.data();
                }

                size_t size = w * h * 4;

                VkBufferCreateInfo bufferCreateInfo =
                {
                    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .size  = size,
                    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                };
                VkBuffer scratchBuffer = VK_NULL_HANDLE;
                VkResult result = device->vk.CreateBuffer(device->device(), &bufferCreateInfo, nullptr, &scratchBuffer);
                if (result != VK_SUCCESS)
                {
                    reshade_log.errorf("Failed to create scratch buffer");
                    return false;
                }

                VkMemoryRequirements memRequirements;
                device->vk.GetBufferMemoryRequirements(device->device(), scratchBuffer, &memRequirements);

                uint32_t memTypeIndex = device->findMemoryType(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, memRequirements.memoryTypeBits);
                assert(memTypeIndex != ~0u);
                VkMemoryAllocateInfo allocInfo =
                {
                    .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                    .allocationSize  = memRequirements.size,
                    .memoryTypeIndex = memTypeIndex,
                };
                VkDeviceMemory scratchMemory = VK_NULL_HANDLE;
                result = device->vk.AllocateMemory(device->device(), &allocInfo, nullptr, &scratchMemory);
                if (result != VK_SUCCESS)
                {
                    reshade_log.errorf("vkAllocateMemory failed");
                    return false;
                }
                device->vk.BindBufferMemory(device->device(), scratchBuffer, scratchMemory, 0);
                if (result != VK_SUCCESS)
                {
                    reshade_log.errorf("vkBindBufferMemory failed");
                    return false;
                }

                void *scratchPtr = nullptr;
                result = device->vk.MapMemory(device->device(), scratchMemory, 0, VK_WHOLE_SIZE, 0, &scratchPtr);
                if (result != VK_SUCCESS)
                {
                    reshade_log.errorf("vkMapMemory failed");
                    return false;
                }

                memcpy(scratchPtr, pixels, size);

                m_cmdBuffer->reset();
                m_cmdBuffer->begin();
                m_cmdBuffer->copyBufferToImage(scratchBuffer, 0, 0, texture);
                device->submitInternal(&*m_cmdBuffer);
                device->waitIdle(false);

                free(data);
                device->vk.DestroyBuffer(device->device(), scratchBuffer, nullptr);
                device->vk.FreeMemory(device->device(), scratchMemory, nullptr);
            }
        }
        else if (texture)
        {
            m_cmdBuffer->reset();
            m_cmdBuffer->begin();
            VkClearColorValue clearColor{};
            VkImageSubresourceRange range =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            };
            m_cmdBuffer->prepareDestImage(texture.get());
            m_cmdBuffer->insertBarrier();
            device->vk.CmdClearColorImage(m_cmdBuffer->rawBuffer(), texture->vkImage(), VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1, &range);
            m_cmdBuffer->markDirty(texture.get());
            device->submitInternal(&*m_cmdBuffer);
            device->waitIdle(false);
        }

        m_textures.emplace_back(std::move(texture));
    }

    // Create Samplers
    {
        for (const auto& sampler : m_module->samplers)
        {
            gamescope::Rc<CVulkanTexture> tex;

            tex = findTexture(sampler.texture_name);
            if (!tex)
            {
                reshade_log.errorf("Couldn't find texture with name: %s", sampler.texture_name.c_str());
            }

            VkFilter            minFilter;
            VkFilter            magFilter;
            VkSamplerMipmapMode mipmapMode;
            ConvertReshadeFilter(sampler.filter, minFilter, magFilter, mipmapMode);

            VkSamplerCreateInfo samplerCreateInfo;
            samplerCreateInfo.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            samplerCreateInfo.pNext                   = nullptr;
            samplerCreateInfo.flags                   = 0;
            samplerCreateInfo.magFilter               = magFilter;
            samplerCreateInfo.minFilter               = minFilter;
            samplerCreateInfo.mipmapMode              = mipmapMode;
            samplerCreateInfo.addressModeU            = ConvertReshadeAddressMode(sampler.address_u);
            samplerCreateInfo.addressModeV            = ConvertReshadeAddressMode(sampler.address_v);
            samplerCreateInfo.addressModeW            = ConvertReshadeAddressMode(sampler.address_w);
            samplerCreateInfo.mipLodBias              = sampler.lod_bias;
            samplerCreateInfo.anisotropyEnable        = VK_FALSE;
            samplerCreateInfo.maxAnisotropy           = 0;
            samplerCreateInfo.compareEnable           = VK_FALSE;
            samplerCreateInfo.compareOp               = VK_COMPARE_OP_ALWAYS;
            samplerCreateInfo.minLod                  = sampler.min_lod;
            samplerCreateInfo.maxLod                  = sampler.max_lod;
            samplerCreateInfo.borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            samplerCreateInfo.unnormalizedCoordinates = VK_FALSE;

            VkSampler vkSampler;
            VkResult result = device->vk.CreateSampler(device->device(), &samplerCreateInfo, nullptr, &vkSampler);
            if (result != VK_SUCCESS)
            {
                reshade_log.errorf("vkCreateSampler failed");
                return false;
            }

            m_samplers.emplace_back(vkSampler, std::move(tex));
        }
    }

    // Create Descriptor Set Layouts

    {
        VkDescriptorSetLayoutBinding layoutBinding;
        layoutBinding.binding            = 0;
        layoutBinding.descriptorType     = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        layoutBinding.descriptorCount    = 1;
        layoutBinding.stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        layoutBinding.pImmutableSamplers = nullptr;

        VkDescriptorSetLayoutCreateInfo layoutCreateInfo;
        layoutCreateInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutCreateInfo.pNext        = nullptr;
        layoutCreateInfo.flags        = 0;
        layoutCreateInfo.bindingCount = 1;
        layoutCreateInfo.pBindings    = &layoutBinding;

        VkResult result = device->vk.CreateDescriptorSetLayout(device->device(), &layoutCreateInfo, nullptr, &m_descriptorSetLayouts[GAMESCOPE_RESHADE_DESCRIPTOR_SET_UBO]);
        if (result != VK_SUCCESS)
        {
            reshade_log.errorf("Failed to create descriptor set layout.");
            return false;
        }
    }

    {
        std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
        for (uint32_t i = 0; i < m_module->samplers.size(); i++)
        {
            VkDescriptorSetLayoutBinding layoutBinding;
            layoutBinding.binding            = i;
            layoutBinding.descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            layoutBinding.descriptorCount    = 1;
            layoutBinding.stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
            layoutBinding.pImmutableSamplers = nullptr;

            layoutBindings.push_back(layoutBinding);
        }

        VkDescriptorSetLayoutCreateInfo layoutCreateInfo;
        layoutCreateInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutCreateInfo.pNext        = nullptr;
        layoutCreateInfo.flags        = 0;
        layoutCreateInfo.bindingCount = layoutBindings.size();
        layoutCreateInfo.pBindings    = layoutBindings.data();

        VkResult result = device->vk.CreateDescriptorSetLayout(device->device(), &layoutCreateInfo, nullptr, &m_descriptorSetLayouts[GAMESCOPE_RESHADE_DESCRIPTOR_SET_SAMPLED_IMAGES]);
        if (result != VK_SUCCESS)
        {
            reshade_log.errorf("Failed to create descriptor set layout.");
            return false;
        }
    }

    {
        std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
        for (uint32_t i = 0; i < m_module->samplers.size(); i++)
        {
            VkDescriptorSetLayoutBinding layoutBinding;
            layoutBinding.binding            = i;
            layoutBinding.descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            layoutBinding.descriptorCount    = 1;
            layoutBinding.stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
            layoutBinding.pImmutableSamplers = nullptr;

            layoutBindings.push_back(layoutBinding);
        }

        VkDescriptorSetLayoutCreateInfo layoutCreateInfo;
        layoutCreateInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutCreateInfo.pNext        = nullptr;
        layoutCreateInfo.flags        = 0;
        layoutCreateInfo.bindingCount = layoutBindings.size();
        layoutCreateInfo.pBindings    = layoutBindings.data();

        VkResult result = device->vk.CreateDescriptorSetLayout(device->device(), &layoutCreateInfo, nullptr, &m_descriptorSetLayouts[GAMESCOPE_RESHADE_DESCRIPTOR_SET_STORAGE_IMAGES]);
        if (result != VK_SUCCESS)
        {
            reshade_log.errorf("Failed to create descriptor set layout.");
            return false;
        }
    }

    {
        VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo =
        {
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext                  = nullptr,
            .flags                  = 0,
            .setLayoutCount         = uint32_t(std::size(m_descriptorSetLayouts)),
            .pSetLayouts            = m_descriptorSetLayouts,
            .pushConstantRangeCount = 0,
            .pPushConstantRanges    = nullptr,
        };

        VkResult result = device->vk.CreatePipelineLayout(device->device(), &pipelineLayoutCreateInfo, nullptr, &m_pipelineLayout);
        if (result != VK_SUCCESS)
        {
            reshade_log.errorf("Failed to create pipeline layout.");
            return false;
        }
    }

    {
        VkDescriptorPoolSize descriptorPoolSizes[] =
        {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         uint32_t(GAMESCOPE_RESHADE_DESCRIPTOR_SET_COUNT * 1u) },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(GAMESCOPE_RESHADE_DESCRIPTOR_SET_COUNT * m_module->samplers.size()) },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          uint32_t(GAMESCOPE_RESHADE_DESCRIPTOR_SET_COUNT * m_module->storages.size()) },
        };

        VkDescriptorPoolCreateInfo descriptorPoolCreateInfo;
        descriptorPoolCreateInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        descriptorPoolCreateInfo.pNext         = nullptr;
        descriptorPoolCreateInfo.flags         = 0;
        descriptorPoolCreateInfo.maxSets       = GAMESCOPE_RESHADE_DESCRIPTOR_SET_COUNT;
        descriptorPoolCreateInfo.poolSizeCount = std::size(descriptorPoolSizes);
        descriptorPoolCreateInfo.pPoolSizes    = descriptorPoolSizes;

        VkResult result = device->vk.CreateDescriptorPool(device->device(), &descriptorPoolCreateInfo, nullptr, &m_descriptorPool);
        if (result != VK_SUCCESS)
        {
            reshade_log.errorf("Failed to create descriptor pool.");
            return false;
        }
    }

    for (uint32_t i = 0; i < GAMESCOPE_RESHADE_DESCRIPTOR_SET_COUNT; i++)
    {
        VkDescriptorSetAllocateInfo descriptorSetAllocateInfo;
        descriptorSetAllocateInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        descriptorSetAllocateInfo.pNext              = nullptr;
        descriptorSetAllocateInfo.descriptorPool     = m_descriptorPool;
        descriptorSetAllocateInfo.descriptorSetCount = 1;
        descriptorSetAllocateInfo.pSetLayouts        = &m_descriptorSetLayouts[i];

        VkResult result = device->vk.AllocateDescriptorSets(device->device(), &descriptorSetAllocateInfo, &m_descriptorSets[i]);
        if (result != VK_SUCCESS)
        {
            reshade_log.errorf("Failed to allocate descriptor set.");
            return false;
        }
    }

    // Create Pipelines
	for (const auto& pass : technique.passes)
	{
		reshade_log.infof("Compiling pass: %s", pass.name.c_str());

        VkShaderModuleCreateInfo shaderModuleInfo =
        {
            .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = m_module->code.size(),
            .pCode    = reinterpret_cast<uint32_t*>(m_module->code.data()),
        };

		if (!pass.cs_entry_point.empty())
		{
            VkPipelineShaderStageCreateInfo shaderStageCreateInfoCompute =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = &shaderModuleInfo,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .pName = pass.cs_entry_point.c_str(),
            };

			VkComputePipelineCreateInfo pipelineInfo =
			{
				.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage  = shaderStageCreateInfoCompute,
                .layout = m_pipelineLayout,
			};

			VkPipeline pipeline = VK_NULL_HANDLE;
			VkResult result = device->vk.CreateComputePipelines(device->device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
			if (result != VK_SUCCESS)
            {
				reshade_log.errorf("Failed to CreateComputePipelines");
                return false;
            }

            m_pipelines.push_back(pipeline);
		}
        else
        {
            std::vector<VkPipelineColorBlendAttachmentState> attachmentBlendStates;
            std::vector<VkFormat> colorFormats;
            uint32_t maxRenderWidth = 0;
            uint32_t maxRenderHeight = 0;

            for (int i = 0; i < 8; i++)
            {
                gamescope::Rc<CVulkanTexture> rt;
                if (i == 0 && pass.render_target_names[0].empty())
                    rt = m_rt;
                else if (pass.render_target_names[i].empty())
                    break;
                else
                    rt = findTexture(pass.render_target_names[i]);

                if (rt == nullptr)
                    continue;

                maxRenderWidth = std::max<uint32_t>(maxRenderWidth, rt->width());
                maxRenderHeight = std::max<uint32_t>(maxRenderHeight, rt->height());

                colorFormats.push_back(rt->format());

                VkPipelineColorBlendAttachmentState colorBlendAttachment;
                colorBlendAttachment.blendEnable         = pass.blend_enable[i];
                colorBlendAttachment.srcColorBlendFactor = ConvertReshadeBlendFactor(pass.src_blend[i]);
                colorBlendAttachment.dstColorBlendFactor = ConvertReshadeBlendFactor(pass.dest_blend[i]);
                colorBlendAttachment.colorBlendOp        = ConvertReshadeBlendOp(pass.blend_op[i]);
                colorBlendAttachment.srcAlphaBlendFactor = ConvertReshadeBlendFactor(pass.src_blend_alpha[i]);
                colorBlendAttachment.dstAlphaBlendFactor = ConvertReshadeBlendFactor(pass.dest_blend_alpha[i]);
                colorBlendAttachment.alphaBlendOp        = ConvertReshadeBlendOp(pass.blend_op_alpha[i]);
                colorBlendAttachment.colorWriteMask      = pass.color_write_mask[i];

                attachmentBlendStates.push_back(colorBlendAttachment);
            }

            VkPipelineRenderingCreateInfo renderingCreateInfo;
            renderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
            renderingCreateInfo.pNext = nullptr;
            renderingCreateInfo.viewMask = 0;
            renderingCreateInfo.colorAttachmentCount = colorFormats.size();
            renderingCreateInfo.pColorAttachmentFormats = colorFormats.data();
            renderingCreateInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
            renderingCreateInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

            VkRect2D scissor;
            scissor.offset        = {0, 0};
            scissor.extent.width  = pass.viewport_width ? pass.viewport_width : maxRenderWidth;
            scissor.extent.height = pass.viewport_height ? pass.viewport_height : maxRenderHeight;

            VkViewport viewport;
            viewport.x        = 0.0f;
            viewport.y        = static_cast<float>(scissor.extent.height);
            viewport.width    = static_cast<float>(scissor.extent.width);
            viewport.height   = -static_cast<float>(scissor.extent.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;

            VkPipelineShaderStageCreateInfo shaderStageCreateInfoVert;
            shaderStageCreateInfoVert.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            shaderStageCreateInfoVert.pNext               = &shaderModuleInfo;
            shaderStageCreateInfoVert.flags               = 0;
            shaderStageCreateInfoVert.stage               = VK_SHADER_STAGE_VERTEX_BIT;
            shaderStageCreateInfoVert.module              = VK_NULL_HANDLE;
            shaderStageCreateInfoVert.pName               = pass.vs_entry_point.c_str();
            shaderStageCreateInfoVert.pSpecializationInfo = nullptr;

            VkPipelineShaderStageCreateInfo shaderStageCreateInfoFrag;
            shaderStageCreateInfoFrag.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            shaderStageCreateInfoFrag.pNext               = &shaderModuleInfo;
            shaderStageCreateInfoFrag.flags               = 0;
            shaderStageCreateInfoFrag.stage               = VK_SHADER_STAGE_FRAGMENT_BIT;
            shaderStageCreateInfoFrag.module              = VK_NULL_HANDLE;
            shaderStageCreateInfoFrag.pName               = pass.ps_entry_point.c_str();
            shaderStageCreateInfoFrag.pSpecializationInfo = nullptr;

            VkPipelineShaderStageCreateInfo shaderStages[] = {shaderStageCreateInfoVert, shaderStageCreateInfoFrag};

            VkPipelineVertexInputStateCreateInfo vertexInputCreateInfo;
            vertexInputCreateInfo.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vertexInputCreateInfo.pNext                           = nullptr;
            vertexInputCreateInfo.flags                           = 0;
            vertexInputCreateInfo.vertexBindingDescriptionCount   = 0;
            vertexInputCreateInfo.pVertexBindingDescriptions      = nullptr;
            vertexInputCreateInfo.vertexAttributeDescriptionCount = 0;
            vertexInputCreateInfo.pVertexAttributeDescriptions    = nullptr;


            VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            switch (pass.topology)
            {
                case reshadefx::primitive_topology::point_list:     topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
                case reshadefx::primitive_topology::line_list:      topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
                case reshadefx::primitive_topology::line_strip:     topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
                case reshadefx::primitive_topology::triangle_list:  topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
                case reshadefx::primitive_topology::triangle_strip: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
                default: reshade_log.errorf("Unsupported primitive type: %d", (uint32_t) pass.topology); break;
            }

            VkPipelineInputAssemblyStateCreateInfo inputAssemblyCreateInfo;
            inputAssemblyCreateInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssemblyCreateInfo.pNext                  = nullptr;
            inputAssemblyCreateInfo.flags                  = 0;
            inputAssemblyCreateInfo.topology               = topology;
            inputAssemblyCreateInfo.primitiveRestartEnable = VK_FALSE;

            VkPipelineViewportStateCreateInfo viewportStateCreateInfo;
            viewportStateCreateInfo.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportStateCreateInfo.pNext         = nullptr;
            viewportStateCreateInfo.flags         = 0;
            viewportStateCreateInfo.viewportCount = 1;
            viewportStateCreateInfo.pViewports    = &viewport;
            viewportStateCreateInfo.scissorCount  = 1;
            viewportStateCreateInfo.pScissors     = &scissor;

            VkPipelineRasterizationStateCreateInfo rasterizationCreateInfo;
            rasterizationCreateInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizationCreateInfo.pNext                   = nullptr;
            rasterizationCreateInfo.flags                   = 0;
            rasterizationCreateInfo.depthClampEnable        = VK_FALSE;
            rasterizationCreateInfo.rasterizerDiscardEnable = VK_FALSE;
            rasterizationCreateInfo.polygonMode             = VK_POLYGON_MODE_FILL;
            rasterizationCreateInfo.cullMode                = VK_CULL_MODE_NONE;
            rasterizationCreateInfo.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rasterizationCreateInfo.depthBiasEnable         = VK_FALSE;
            rasterizationCreateInfo.depthBiasConstantFactor = 0.0f;
            rasterizationCreateInfo.depthBiasClamp          = 0.0f;
            rasterizationCreateInfo.depthBiasSlopeFactor    = 0.0f;
            rasterizationCreateInfo.lineWidth               = 1.0f;

            VkPipelineMultisampleStateCreateInfo multisampleCreateInfo;
            multisampleCreateInfo.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampleCreateInfo.pNext                 = nullptr;
            multisampleCreateInfo.flags                 = 0;
            multisampleCreateInfo.rasterizationSamples  = VK_SAMPLE_COUNT_1_BIT;
            multisampleCreateInfo.sampleShadingEnable   = VK_FALSE;
            multisampleCreateInfo.minSampleShading      = 1.0f;
            multisampleCreateInfo.pSampleMask           = nullptr;
            multisampleCreateInfo.alphaToCoverageEnable = VK_FALSE;
            multisampleCreateInfo.alphaToOneEnable      = VK_FALSE;

            VkPipelineColorBlendStateCreateInfo colorBlendCreateInfo;
            colorBlendCreateInfo.sType             = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlendCreateInfo.pNext             = nullptr;
            colorBlendCreateInfo.flags             = 0;
            colorBlendCreateInfo.logicOpEnable     = VK_FALSE;
            colorBlendCreateInfo.logicOp           = VK_LOGIC_OP_NO_OP;
            colorBlendCreateInfo.attachmentCount   = attachmentBlendStates.size();
            colorBlendCreateInfo.pAttachments      = attachmentBlendStates.data();
            colorBlendCreateInfo.blendConstants[0] = 0.0f;
            colorBlendCreateInfo.blendConstants[1] = 0.0f;
            colorBlendCreateInfo.blendConstants[2] = 0.0f;
            colorBlendCreateInfo.blendConstants[3] = 0.0f;

            VkPipelineDynamicStateCreateInfo dynamicStateCreateInfo;
            dynamicStateCreateInfo.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamicStateCreateInfo.pNext             = nullptr;
            dynamicStateCreateInfo.flags             = 0;
            dynamicStateCreateInfo.dynamicStateCount = 0;
            dynamicStateCreateInfo.pDynamicStates    = nullptr;

#if 0
            VkPipelineDepthStencilStateCreateInfo depthStencilStateCreateInfo = {};

            depthStencilStateCreateInfo.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depthStencilStateCreateInfo.pNext                 = nullptr;
            depthStencilStateCreateInfo.depthTestEnable       = VK_FALSE;
            depthStencilStateCreateInfo.depthWriteEnable      = VK_FALSE;
            depthStencilStateCreateInfo.depthCompareOp        = VK_COMPARE_OP_ALWAYS;
            depthStencilStateCreateInfo.depthBoundsTestEnable = VK_FALSE;
            depthStencilStateCreateInfo.stencilTestEnable     = pass.stencil_enable;
            depthStencilStateCreateInfo.front.failOp          = convertReshadeStencilOp(pass.stencil_op_fail);
            depthStencilStateCreateInfo.front.passOp          = convertReshadeStencilOp(pass.stencil_op_pass);
            depthStencilStateCreateInfo.front.depthFailOp     = convertReshadeStencilOp(pass.stencil_op_depth_fail);
            depthStencilStateCreateInfo.front.compareOp       = convertReshadeCompareOp(pass.stencil_comparison_func);
            depthStencilStateCreateInfo.front.compareMask     = pass.stencil_read_mask;
            depthStencilStateCreateInfo.front.writeMask       = pass.stencil_write_mask;
            depthStencilStateCreateInfo.front.reference       = pass.stencil_reference_value;
            depthStencilStateCreateInfo.back                  = depthStencilStateCreateInfo.front;
            depthStencilStateCreateInfo.minDepthBounds        = 0.0f;
            depthStencilStateCreateInfo.maxDepthBounds        = 1.0f;
#endif

            VkGraphicsPipelineCreateInfo pipelineCreateInfo;
            pipelineCreateInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineCreateInfo.pNext               = &renderingCreateInfo;
            pipelineCreateInfo.flags               = 0;
            pipelineCreateInfo.stageCount          = 2;
            pipelineCreateInfo.pStages             = shaderStages;
            pipelineCreateInfo.pVertexInputState   = &vertexInputCreateInfo;
            pipelineCreateInfo.pInputAssemblyState = &inputAssemblyCreateInfo;
            pipelineCreateInfo.pTessellationState  = nullptr;
            pipelineCreateInfo.pViewportState      = &viewportStateCreateInfo;
            pipelineCreateInfo.pRasterizationState = &rasterizationCreateInfo;
            pipelineCreateInfo.pMultisampleState   = &multisampleCreateInfo;
//            pipelineCreateInfo.pDepthStencilState  = &depthStencilStateCreateInfo;
            pipelineCreateInfo.pDepthStencilState  = nullptr;
            pipelineCreateInfo.pColorBlendState    = &colorBlendCreateInfo;
            pipelineCreateInfo.pDynamicState       = &dynamicStateCreateInfo;
            pipelineCreateInfo.layout              = m_pipelineLayout;
            pipelineCreateInfo.renderPass          = VK_NULL_HANDLE;
            pipelineCreateInfo.subpass             = 0;
            pipelineCreateInfo.basePipelineHandle  = VK_NULL_HANDLE;
            pipelineCreateInfo.basePipelineIndex   = -1;

			VkPipeline pipeline = VK_NULL_HANDLE;
			VkResult result = device->vk.CreateGraphicsPipelines(device->device(), VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr, &pipeline);
			if (result != VK_SUCCESS)
            {
				reshade_log.errorf("Failed to vkCreateGraphicsPipelines");
                return false;
            }

            m_pipelines.push_back(pipeline);
        }
	}

    return true;
}

void ReshadeEffectPipeline::update()
{
    if (g_effectReadyCallback && g_reshadeEffectPath) {
        g_effectReadyCallback(g_reshadeEffectPath);
        g_effectReadyCallback = nullptr;
    }

    for (auto& uniform : m_uniforms)
        uniform->update(m_mappedPtr);
}

uint64_t ReshadeEffectPipeline::execute(gamescope::Rc<CVulkanTexture> inImage, gamescope::Rc<CVulkanTexture> *outImage)
{
    CVulkanDevice *device = m_device;
    this->update();

    // Update descriptor sets.
    {
        VkDescriptorBufferInfo bufferInfo =
        {
            .buffer = m_buffer,
            .range  = VK_WHOLE_SIZE,
        };

        VkWriteDescriptorSet writeDescriptorSet =
        {
            .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet           = m_descriptorSets[GAMESCOPE_RESHADE_DESCRIPTOR_SET_UBO],
            .dstBinding       = 0,
            .descriptorCount  = 1,
            .descriptorType   = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo      = &bufferInfo,
        };

        device->vk.UpdateDescriptorSets(device->device(), 1, &writeDescriptorSet, 0, nullptr);
    }

    //device->vk.UpdateDescriptorSets(cmd, )
    for (size_t i = 0; i < m_samplers.size(); i++)
    {
        bool srgb = m_module->samplers[i].srgb;

        VkDescriptorImageInfo imageInfo =
        {
            .sampler     = m_samplers[i].sampler,
            .imageView   = m_samplers[i].texture ? m_samplers[i].texture->view(srgb) : inImage->view(srgb),
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };

        VkWriteDescriptorSet writeDescriptorSet =
        {
            .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet           = m_descriptorSets[GAMESCOPE_RESHADE_DESCRIPTOR_SET_SAMPLED_IMAGES],
            .dstBinding       = uint32_t(i),
            .descriptorCount  = 1,
            .descriptorType   = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo       = &imageInfo,
        };

        device->vk.UpdateDescriptorSets(device->device(), 1, &writeDescriptorSet, 0, nullptr);
    }

    for (size_t i = 0; i < m_module->storages.size(); i++)
    {
        // TODO: Cache
        auto tex = findTexture(m_module->storages[i].texture_name);

        VkDescriptorImageInfo imageInfo =
        {
            .imageView   = tex ? tex->srgbView() : VK_NULL_HANDLE,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };

        VkWriteDescriptorSet writeDescriptorSet =
        {
            .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet           = m_descriptorSets[GAMESCOPE_RESHADE_DESCRIPTOR_SET_STORAGE_IMAGES],
            .dstBinding       = uint32_t(i),
            .descriptorCount  = 1,
            .descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo       = &imageInfo,
        };

        device->vk.UpdateDescriptorSets(device->device(), 1, &writeDescriptorSet, 0, nullptr);
    }

    // Draw and compute time!
    m_cmdBuffer->reset();
    m_cmdBuffer->begin();

    VkCommandBuffer cmd = m_cmdBuffer->rawBuffer();
    device->vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, std::size(m_descriptorSets), m_descriptorSets, 0, nullptr);
    device->vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, std::size(m_descriptorSets), m_descriptorSets, 0, nullptr);

    for (size_t i = 0; i < m_textures.size(); i++)
    {
        auto& tex = m_textures[i];
        auto& texInfo = m_module->textures[i];

        if (tex && (texInfo.storage_access || texInfo.render_target))
            m_cmdBuffer->discardImage(tex.get());
    }

    if (m_rt)
        m_cmdBuffer->discardImage(m_rt.get());

    gamescope::Rc<CVulkanTexture> lastRT;

    auto& technique = m_module->techniques[m_key.techniqueIdx];
    uint32_t passIdx = 0;
    for (auto& pass : technique.passes)
    {
        for (size_t i = 0; i < m_textures.size(); i++)
        {
            auto& tex = m_textures[i];
            auto& texInfo = m_module->textures[i];

            if (tex && texInfo.storage_access)
                m_cmdBuffer->prepareDestImage(tex.get());
            else
                m_cmdBuffer->prepareSrcImage(tex != nullptr ? tex.get() : inImage.get());
        }

        m_cmdBuffer->insertBarrier();

        std::array<gamescope::Rc<CVulkanTexture>, 8> rts{};

        if (!pass.cs_entry_point.empty())
        {
            device->vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelines[passIdx]);

            device->vk.CmdDispatch(cmd,
                pass.viewport_width  ? pass.viewport_width  : m_key.bufferWidth,
                pass.viewport_height ? pass.viewport_height : m_key.bufferHeight,
                pass.viewport_dispatch_z);
        }
        else
        {
            for (int i = 0; i < 8; i++)
            {
                if (i == 0 && pass.render_target_names[0].empty())
                    rts[i] = m_rt;
                else if (pass.render_target_names[i].empty())
                    break;
                else
                    rts[i] = findTexture(pass.render_target_names[i]);
            }

            for (int i = 0; i < 8; i++)
            {
                if (rts[i])
                    m_cmdBuffer->prepareDestImage(rts[i].get());
            }

            device->vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelines[passIdx]);

            std::vector<VkRenderingAttachmentInfo> colorAttachmentInfos;
            uint32_t maxRenderWidth = 0;
            uint32_t maxRenderHeight = 0;
            for (int i = 0; i < 8; i++)
            {
                if (rts[i])
                {
                    maxRenderWidth = std::max<uint32_t>(maxRenderWidth, rts[i]->width());
                    maxRenderHeight = std::max<uint32_t>(maxRenderHeight, rts[i]->height());

                    const VkRenderingAttachmentInfo colorAttachmentInfo
                    {
                        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                        .imageView   = rts[i]->view(pass.srgb_write_enable),
                        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .loadOp      = pass.clear_render_targets ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
                    };
                    colorAttachmentInfos.push_back(colorAttachmentInfo);
                }
            }

            const VkRenderingInfo renderInfo
            {
                .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea           = { { 0, 0, }, { maxRenderWidth, maxRenderHeight }},
                .layerCount           = 1,
                .colorAttachmentCount = uint32_t(colorAttachmentInfos.size()),
                .pColorAttachments    = colorAttachmentInfos.data(),
            };

            device->vk.CmdBeginRendering(cmd, &renderInfo);

            device->vk.CmdDraw(cmd, pass.num_vertices, 1, 0, 0);

            device->vk.CmdEndRendering(cmd);
        }

        for (int i = 0; i < 8; i++)
        {
            if (rts[i])
                m_cmdBuffer->markDirty(rts[i].get());
        }

        // Insert a stupidly huge fat barrier.
        VkMemoryBarrier memBarrier = {};
        memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        device->vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memBarrier, 0, NULL, 0, NULL);

        if (rts[0])
            lastRT = rts[0];

        passIdx++;
    }

    if (lastRT)
        *outImage = lastRT;

    return device->submitInternal(&*m_cmdBuffer);
}

gamescope::Rc<CVulkanTexture> ReshadeEffectPipeline::findTexture(std::string_view name)
{
    for (size_t i = 0; i < m_module->textures.size(); i++)
    {
        if (m_module->textures[i].unique_name == name)
            return m_textures[i];
    }

    return nullptr;
}

////////////////////////////////
// ReshadeEffectManager
////////////////////////////////

ReshadeEffectManager::ReshadeEffectManager()
{
}

void ReshadeEffectManager::init(CVulkanDevice *device)
{
	m_device = device;
}

void ReshadeEffectManager::clear()
{
    m_lastKey = ReshadeEffectKey{};
    m_lastPipeline = nullptr;
}

ReshadeEffectPipeline* ReshadeEffectManager::pipeline(const ReshadeEffectKey &key)
{
    if (m_lastKey == key)
        return m_lastPipeline.get();

    m_lastKey = key;
    m_lastPipeline = nullptr;
    auto pipeline = std::make_unique<ReshadeEffectPipeline>();
    if (!pipeline->init(m_device, key))
        return nullptr;
    m_lastPipeline = std::move(pipeline);

    return m_lastPipeline.get();
}

ReshadeEffectManager g_reshadeManager;

void reshade_effect_manager_set_uniform_variable(const char *key, uint8_t* value) 
{
    std::lock_guard<std::mutex> lock(g_runtimeUniformsMutex);

    auto it = g_runtimeUniforms.find(key);
    if (it != g_runtimeUniforms.end()) {
        delete[] it->second;
    }
    
    g_runtimeUniforms[std::string(key)] = value;
    force_repaint();
}

void reshade_effect_manager_set_effect(const char *path, std::function<void(const char*)> callback)
{
    g_runtimeUniforms.clear();
    if (g_reshadeEffectPath) free(g_reshadeEffectPath);
    g_reshadeEffectPath = strdup(path);
    g_effectReadyCallback = callback;
}

void reshade_effect_manager_enable_effect() 
{
    if (g_reshadeEffectPath) gamescope_set_reshade_effect(g_reshadeEffectPath);
}

void reshade_effect_manager_disable_effect() 
{
    gamescope_set_reshade_effect(nullptr);
}