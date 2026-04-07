#include "wb_h/wb_1_capture.h"
#include "wb_h/wb_2_stitching.h"
#include "wb_h/wb_7_preview.h"
#include "wb_h/wb_egl_manager.h"
#include "wb_h/wb_9_imu.h"
#include "wb_h/wb_photo_saver.h"
#include "wb_h/wb_thermal_throttle.h"
#include "wb_h/wb_video_mode.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <opencv2/opencv.hpp>
#include <opencv2/core/ocl.hpp>
#include <camera/NdkCaptureRequest.h>

#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>



#define ASENSOR_DELAY_FASTEST 0          // 最快（约0ms延迟）
#define ASENSOR_DELAY_CAMERA 5000       //5ms, 200hz
#define ASENSOR_DELAY_GAME 20000         // 游戏级（约20ms延迟）50hz
#define ASENSOR_DELAY_TEST 200000         // 游戏级（约200ms延迟）5hz
#define ASENSOR_DELAY_UI 66667           // UI级（约66ms延迟）
#define ASENSOR_DELAY_NORMAL 200000      // 普通级（约200ms延迟）
// 调试开关宏定义 - 开启/关闭纹理调试代码
//#define DEBUG_TEXTURE_TO_MAT  // 取消注释以启用调试代码

// ============ 是否启用独立 YUV 处理线程的开关 ============
// 设置为 1 启用独立线程处理 YUV 转 RGB，设置为 0 使用原始回调线程处理
// 模式 2：零拷贝模式，直接传递 AImage 指针（推荐）
#define USE_SEPARATE_YUV_THREAD 2

// 声明全局EGL上下文获取函数
extern EGLContext wb_get_global_egl_context();
extern EGLDisplay wb_get_global_egl_display();
#define CAM_SIZE 3000
#define CAM_SIZE_W 1920
#define CAM_SIZE_H 960

// 全局优化变量
static GLuint g_reusable_framebuffer = 0;
static std::vector<uint8_t> g_pixel_buffer_pool;
static std::mutex g_optimization_mutex;

// PBO双缓冲异步读取优化变量
static GLuint g_pbo_pool[2] = {0, 0};           // PBO双缓冲池
static int g_pbo_index = 0;                     // 当前PBO索引
static bool g_pbo_initialized = false;         // PBO是否已初始化
static int g_pbo_width = 0;                    // PBO缓冲区宽度
static int g_pbo_height = 0;                   // PBO缓冲区高度
static bool g_pbo_first_frame = true;          // 是否为第一帧（用于异步读取）
static std::mutex g_pbo_mutex;                 // PBO操作互斥锁
static std::atomic<bool> g_full_model_mode{false};
// IMU加速度计初始值持久化（XML）相关全局状态
static bool g_accel_init_loaded = false;                 // 是否已从XML加载
static bool g_accel_init_need_write_on_first_event = false; // 如果未加载，则在首个加速度事件时写入
static float g_accel_init_bias[3] = {0.0f, 0.0f, 0.0f};  // 作为零偏使用的初始值
static int sight_lock_mode = 0;  // 0=固定, 1=跟随镜头
static const char* kIMUInitDir = "/data/data/com.webuild.camera.app/files";     // 应用私有目录，无需外部存储权限
static const char* kIMUInitPath = "/data/data/com.webuild.camera.app/imu_accel_init.xml"; // XML路径


static bool file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

static bool ensure_dir_exists(const char* dir) {
    struct stat st{};
    if (stat(dir, &st) == 0) {
        return (st.st_mode & S_IFDIR) != 0;
    }
    // 尝试创建目录
    if (mkdir(dir, 0777) == 0) return true;
    LOGD_IMU("mkdir %s failed: %d", dir, errno);
    return false;
}

static bool write_accel_init_xml(float ax, float ay, float az) {
    if (!ensure_dir_exists(kIMUInitDir)) {
        LOGD_IMU("ensure_dir_exists failed");
        return false;
    }
    FILE* f = fopen(kIMUInitPath, "wb");
    if (!f) {
        LOGD_IMU("open %s for write failed: %d", kIMUInitPath, errno);
        return false;
    }
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "<imu>\n  <accel_init x=\"%f\" y=\"%f\" z=\"%f\"/>\n</imu>\n",
                     ax, ay, az);
    fwrite(buf, 1, (size_t)n, f);
    fclose(f);
    LOGD_IMU( "wrote accel init XML: %s", kIMUInitPath);
    return true;
}

static bool parse_attr(const char* src, const char* key, float* out) {
    // 简单解析 x="..." y="..." z="..."，不做严格XML处理
    const char* p = strstr(src, key);
    if (!p) return false;
    p = strchr(p, '\"');
    if (!p) return false;
    ++p;
    char* endptr = nullptr;
    float val = strtof(p, &endptr);
    if (p == endptr) return false;
    *out = val;
    return true;
}

static bool read_accel_init_xml(float* out3) {
    if (!file_exists(kIMUInitPath)) return false;
    FILE* f = fopen(kIMUInitPath, "rb");
    if (!f) return false;
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    float x=0, y=0, z=0;
    if (!parse_attr(buf, "x", &x)) return false;
    if (!parse_attr(buf, "y", &y)) return false;
    if (!parse_attr(buf, "z", &z)) return false;
    out3[0] = x; out3[1] = y; out3[2] = z;
    return true;
}

static void load_or_prepare_accel_init() {
    float bias[3];
    if (read_accel_init_xml(bias)) {
        g_accel_init_bias[0] = bias[0];
        g_accel_init_bias[1] = bias[1];
        g_accel_init_bias[2] = bias[2];
        g_accel_init_loaded = true;
        g_accel_init_need_write_on_first_event = false;
        LOGD_IMU("Loaded accel init: %f,%f,%f", bias[0], bias[1], bias[2]);
    } else {
        // 未找到时，在首个加速度事件到来时采样并写入
        g_accel_init_loaded = false;
        g_accel_init_need_write_on_first_event = true;
        LOGD_IMU( "No accel init XML; will write on first event");
    }
}

/**
 * 初始化PBO双缓冲异步读取
 * @param width 图像宽度
 * @param height 图像高度
 * @return 初始化是否成功
 */
bool init_async_pbo(int width, int height) {
    std::lock_guard<std::mutex> lock(g_pbo_mutex);

    // 如果已初始化且尺寸相同，直接返回
    if (g_pbo_initialized && g_pbo_width == width && g_pbo_height == height) {
        return true;
    }

    // 清理旧的PBO资源
    if (g_pbo_initialized) {
        glDeleteBuffers(2, g_pbo_pool);
        g_pbo_pool[0] = g_pbo_pool[1] = 0;
        LOGD( "清理旧的PBO资源");
    }

    // 创建新的PBO
    glGenBuffers(2, g_pbo_pool);

    // 检查PBO创建是否成功
    GLenum error = glGetError();
    if (error != GL_NO_ERROR || g_pbo_pool[0] == 0 || g_pbo_pool[1] == 0) {
        LOGD( "创建PBO失败，错误: 0x%x", error);
        return false;
    }

    size_t buffer_size = width * height * 3; // RGB格式

    // 初始化两个PBO
    for (int i = 0; i < 2; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo_pool[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, buffer_size, nullptr, GL_STREAM_READ);

        error = glGetError();
        if (error != GL_NO_ERROR) {
            LOGD( "初始化PBO[%d]失败，错误: 0x%x", i, error);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            return false;
        }
    }

    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    // 更新状态
    g_pbo_width = width;
    g_pbo_height = height;
    g_pbo_index = 0;
    g_pbo_first_frame = true;
    g_pbo_initialized = true;

    LOGD("PBO双缓冲初始化成功，尺寸: %dx%d, 缓冲区大小: %zu bytes",
         width, height, buffer_size);
    LOGD("PBO ID: [%d, %d]", g_pbo_pool[0], g_pbo_pool[1]);

    return true;
}

/**
 * 清理PBO资源
 */
void cleanup_async_pbo() {
    std::lock_guard<std::mutex> lock(g_pbo_mutex);

    if (g_pbo_initialized) {
        glDeleteBuffers(2, g_pbo_pool);
        g_pbo_pool[0] = g_pbo_pool[1] = 0;
        g_pbo_initialized = false;
        g_pbo_first_frame = true;
        LOGD("PBO资源清理完成");
    }
}

/**
 * 清理全局优化资源
 * 应在程序退出时调用
 */
void wb_capture_cleanup_optimization_resources() {
    std::lock_guard<std::mutex> lock(g_optimization_mutex);

    // 清理PBO资源
    cleanup_async_pbo();

    // 清理可重用的帧缓冲区
    if (g_reusable_framebuffer != 0) {
        glDeleteFramebuffers(1, &g_reusable_framebuffer);
        g_reusable_framebuffer = 0;
        LOGD_STITCHING( "已清理可重用帧缓冲区");
    }

    // 清理内存池
    if (!g_pixel_buffer_pool.empty()) {
        g_pixel_buffer_pool.clear();
        g_pixel_buffer_pool.shrink_to_fit(); // 释放内存
        LOGD_STITCHING( "已清理像素缓冲区内存池");
    }

    LOGD_STITCHING( "全局优化资源清理完成");
}
//#define DEBUG_DEV_BOARD 1   //如果是开发板，放开这个注释
bool isRunning = false;
int64_t g_clock_offset_ = 0;
ASensorManager* sensorManager = nullptr;
const ASensor* accelerometer = nullptr;
const ASensor* gyroscope = nullptr;
const ASensor* rotationVector = nullptr;
std::vector<SensorInfo> accelerometers;  // 存储所有加速度计
//std::vector<SensorInfo> gyroscopes;      // 存储所有陀螺仪
pthread_t sensorThread;
struct AImage_Plane {
    uint8_t* data;      // 平面数据指针
    int dataSize;       // 数据大小
    int rowStride;      // 行步长
    int pixelStride;    // 像素步长
};

// 期望的目标帧率（通过启动/参数设置传入）
static int g_target_fps = 30;
static int g_max_request_fps = 24;
static int g_min_request_fps = 20;

// 统一日志/FPS打印时间窗口（毫秒），所有模块共享，默认1000ms（1秒）。
// 可通过 wb_set_log_interval_ms() 在运行时动态修改。
std::atomic<int> g_log_interval_ms{5000};
/* 全局变量 - 前后摄像头实例 */
CameraDevice backCamera;
CameraDevice frontCamera;
static PFNEGLCREATEIMAGEKHRPROC g_eglCreateImageKHR  = nullptr;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC g_glEGLImageTargetTexture2DOES  = nullptr;
static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC g_eglGetNativeClientBufferANDROID = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC g_eglDestroyImageKHR = nullptr;
bool initEglExtensions(EGLDisplay eglDisplay) {
    if (eglDisplay == EGL_NO_DISPLAY) {
        LOGE("[EGL] EGLDisplay未初始化");
        return false;
    }
    g_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    g_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    g_eglGetNativeClientBufferANDROID = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    g_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    if (!g_eglCreateImageKHR || !g_glEGLImageTargetTexture2DOES) {
        LOGE("[EGL] 扩展函数加载失败");
        return false;
    }
    if (!g_eglGetNativeClientBufferANDROID) {
        LOGW("[EGL] eglGetNativeClientBufferANDROID 加载失败，可能无法使用AHardwareBuffer零拷贝");
    }
    if (!g_eglDestroyImageKHR) {
        LOGW("[EGL] eglDestroyImageKHR 加载失败，EGLImage销毁将使用平台符号");
    }
    LOGI("[EGL] 扩展初始化成功");
    return true;
}
#define CHECK_GL_ERROR(msg) \
    do { \
        GLenum error = glGetError(); \
        if (error != GL_NO_ERROR) { \
            LOGE("[GL] %s: 错误码 0x%x", msg, error); \
            return false; \
        } \
    } while(0)
#define CHECK_GL_ERROR_VOID(msg) \
    do { \
        GLenum error = glGetError(); \
        if (error != GL_NO_ERROR) { \
            LOGE("[GL] %s: 错误码 0x%x", msg, error); \
            return; \
        } \
    } while(0)
class ShaderUtils {
public:
    static GLuint compileShader(GLenum type, const char* source) {
        GLuint shader = glCreateShader(type);
        if (shader == 0) {
            LOGE("[GL] 创建着色器失败");
            return 0;
        }
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint compiled;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            GLint infoLen = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
            if (infoLen > 1) {
                char* infoLog = new char[infoLen];
                glGetShaderInfoLog(shader, infoLen, nullptr, infoLog);
                LOGE("[GL] 着色器编译失败: %s", infoLog);
                delete[] infoLog;
            }
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }
    static GLuint linkProgram(GLuint vertexShader, GLuint fragmentShader) {
        GLuint program = glCreateProgram();
        if (program == 0) {
            LOGE("[GL] 创建程序失败");
            return 0;
        }
        glAttachShader(program, vertexShader);
        glAttachShader(program, fragmentShader);
        glLinkProgram(program);
        GLint linked;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            GLint infoLen = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &infoLen);
            if (infoLen > 1) {
                char* infoLog = new char[infoLen];
                glGetProgramInfoLog(program, infoLen, nullptr, infoLog);
                LOGE("[GL] 程序链接失败: %s", infoLog);
                delete[] infoLog;
            }
            glDeleteProgram(program);
            return 0;
        }
        return program;
    }
};
class YUVToRGBConverter {
private:
    GLuint program_ = 0;
    GLuint program_oes_ = 0;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint fbo_ = 0;
    GLuint outputTexture_ = 0;
    GLint ySamplerLoc_ = -1;
    GLint uSamplerLoc_ = -1;
    GLint vSamplerLoc_ = -1;
    GLint useInterleavedUVLoc_ = -1;
    GLint uvSwapLoc_ = -1;
    GLint applySRGBLoc_ = -1;
    GLint brightnessLoc_ = -1;
    GLint rotationLoc_ = -1;  // 旋转角度uniform位置
    GLint useLimitedRangeLoc_ = -1;
    GLint useBT709Loc_ = -1;
    GLint brightnessLocOES_ = -1;
    GLint rotationLocOES_ = -1;
    GLint oesSamplerLoc_ = -1;
    GLint positionLoc_ = -1;
    GLint texCoordLoc_ = -1;
    GLint cropRectLoc_ = -1;  // 裁切矩形区域 (minX, minY, maxX, maxY)
    GLint enableSquareCropLoc_ = -1;  // 是否启用正方形裁切
    int width_ = 0;
    int height_ = 0;
    int output_width_ = 0;   // 输出纹理宽度（可能是正方形）
    int output_height_ = 0;  // 输出纹理高度（可能是正方形）
    bool initialized_ = false;
    bool use_interleaved_uv_ = false;
    bool uv_swap_ = false;
    // 缓存动态uniform与FBO绑定，减少每帧GL调用
    float brightness_value_ = 1.0f;
    float rotation_value_ = 0.0f;
    bool brightness_dirty_ = true;
    bool rotation_dirty_ = true;
    GLuint last_output_texture_ = 0;

    // 交错UV与UV交换uniform缓存，避免每帧重复设置
    int last_use_interleaved_uv_ = -1; // -1表示未初始化，强制首次更新
    int last_uv_swap_ = -1;            // -1表示未初始化，强制首次更新
    int last_use_limited_range_ = -1;
    int limited_range_value_ = 1;
    bool limited_range_dirty_ = true;
    int last_use_bt709_ = -1;
    int use_bt709_value_ = 0;
    bool use_bt709_dirty_ = true;
    int last_apply_srgb_ = -1;
    int apply_srgb_value_ = 1;
    bool apply_srgb_dirty_ = true;
    const char* vertexShaderSource_ = R"(
        #version 300 es
        layout(location = 0) in vec4 aPosition;
        layout(location = 1) in vec2 aTexCoord;
        out vec2 vTexCoord;
		// 旋转角度（度），顺时针为正，支持任意角度
        uniform float rotation;
        void main() {
            gl_Position = aPosition;

            // 根据旋转角度变换纹理坐标
            /*vec2 texCoord = aTexCoord;
            if (rotation > 0.5 && rotation < 1.5) {
                // 90度顺时针旋转: (x,y) -> (y, 1-x)
                texCoord = vec2(aTexCoord.y, 1.0 - aTexCoord.x);
            } else if (rotation > 1.5 && rotation < 2.5) {
                // 180度旋转: (x,y) -> (1-x, 1-y)
                texCoord = vec2(1.0 - aTexCoord.x, 1.0 - aTexCoord.y);
            } else if (rotation > 2.5 && rotation < 3.5) {
                // 270度顺时针旋转: (x,y) -> (1-y, x)
                texCoord = vec2(1.0 - aTexCoord.y, aTexCoord.x);
            }*/
			 // 连续旋转：rotation为顺时针角度（度）
            /*float rad = radians(rotation);
            rad = -rad; // 顺时针为正
            vec2 center = vec2(0.5, 0.5);
            vec2 coord = aTexCoord - center;
            mat2 R = mat2(cos(rad), -sin(rad),
                          sin(rad),  cos(rad));
            vec2 rotated = R * coord;
            float len = length(coord);
            if (len > 0.5) { // 半径大于0.5的区域不旋转
                rotated = coord;
            }
            vTexCoord = rotated + center;*/
            // 连续旋转：rotation为顺时针角度（度）
            float rad = radians(rotation);
            rad = -rad; // 顺时针为正
            vec2 center = vec2(0.5, 0.5);
            vec2 coord = aTexCoord - center;
            mat2 R = mat2(cos(rad), -sin(rad),
                       sin(rad),  cos(rad));
            vec2 texCoord = (R * coord) + center;

            vTexCoord = texCoord;
        }
    )";
    const char* fragmentShaderSource_ = R"(
        #version 300 es
        precision highp float;
        in vec2 vTexCoord;
        out vec4 fragColor;
        uniform sampler2D ySampler;
        uniform sampler2D uSampler;
        uniform sampler2D vSampler;
        uniform int useInterleavedUV; // 1=使用RG交错UV, 0=分离U/V
        uniform int uvSwap;           // 1=NV21(VU), 0=NV12(UV)
        uniform float brightness;
        uniform int useLimitedRange;
        uniform int useBT709;
        uniform int applySRGB;
        uniform vec4 cropRect;        // 裁切矩形区域 (minX, minY, maxX, maxY) 归一化坐标[0,1]
        uniform int enableSquareCrop; // 是否启用正方形裁切
        const mat3 yuv2rgb = mat3(
            1.0,      1.0,      1.0,
            0.0,     -0.34414,  1.772,
            1.402,   -0.71414,  0.0
        );
        const mat3 yuv2rgb709 = mat3(
            1.0,      1.0,       1.0,
            0.0,     -0.18732,   1.8556,
            1.5748,  -0.46812,   0.0
        );
        void main() {
            // 正方形裁切逻辑
            vec2 finalTexCoord = vTexCoord;
            if (enableSquareCrop == 1) {
                // vTexCoord 是输出纹理的归一化坐标 [0,1]
                // 我们需要将其映射到输入纹理的裁切区域
                // cropRect = (minX, minY, maxX, maxY) 是输入纹理的归一化坐标

                float cropWidth = cropRect.z - cropRect.x;
                float cropHeight = cropRect.w - cropRect.y;

                // 将输出坐标 [0,1] 映射到输入纹理的裁切区域
                finalTexCoord.x = cropRect.x + vTexCoord.x * cropWidth;
                finalTexCoord.y = cropRect.y + vTexCoord.y * cropHeight;
            }

            float y = texture(ySampler, finalTexCoord).r;
            float u;
            float v;
            if (useInterleavedUV == 1) {
                vec2 uv = texture(uSampler, finalTexCoord).rg; // RG交错采样
                if (uvSwap == 1) {
                    u = uv.g; // NV21: VU -> 交换
                    v = uv.r;
                } else {
                    u = uv.r; // NV12: UV
                    v = uv.g;
                }
            } else {
                u = texture(uSampler, finalTexCoord).r;
                v = texture(vSampler, finalTexCoord).r;
            }

            if (useLimitedRange == 1) {
                y = (y * 255.0 - 16.0) / 219.0;
                u = (u * 255.0 - 16.0) / 224.0 - 0.5;
                v = (v * 255.0 - 16.0) / 224.0 - 0.5;
                y = clamp(y, 0.0, 1.0);
                u = clamp(u, -0.5, 0.5);
                v = clamp(v, -0.5, 0.5);
            } else {
                u = u - 0.5;
                v = v - 0.5;
            }

            // YUV到RGB转换（BT.601标准）
            float r;
            float g;
            float b;
            if (useBT709 == 1) {
                r = y + 1.5748 * v;
                g = y - 0.18732 * u - 0.46812 * v;
                b = y + 1.8556 * u;
            } else {
                r = y + 1.402 * v;
                g = y - 0.34414 * u - 0.71414 * v;
                b = y + 1.772 * u;
            }

            // 应用亮度调整
            r = r * brightness;
            g = g * brightness;
            b = b * brightness;

            vec3 rgb = vec3(clamp(r, 0.0, 1.0), clamp(g, 0.0, 1.0), clamp(b, 0.0, 1.0));
            if (applySRGB == 1) {
                vec3 lo = 12.92 * rgb;
                vec3 hi = 1.055 * pow(rgb, vec3(1.0/2.4)) - vec3(0.055);
                bvec3 cond = lessThanEqual(rgb, vec3(0.0031308));
                rgb = vec3(cond.x ? lo.x : hi.x, cond.y ? lo.y : hi.y, cond.z ? lo.z : hi.z);
            }
            fragColor = vec4(rgb, 1.0);
         }
     )";
    const char* fragmentOESShaderSource_ = R"(
         #version 300 es
         #extension GL_OES_EGL_image_external_essl3 : require
         precision highp float;
         in vec2 vTexCoord;
         out vec4 fragColor;
         uniform samplerExternalOES oesSampler;
         uniform float brightness;
         void main() {
             vec4 color = texture(oesSampler, vTexCoord);
            color.rgb *= brightness;
            fragColor = vec4(color.r, color.g, color.b, color.a);
         }
     )";
public:
    bool initialize(int width, int height) {
        if (initialized_ && width_ == width && height_ == height) {
            return true; // 已初始化且尺寸匹配
        }
        cleanup(); // 清理旧资源
        width_ = width;
        height_ = height;
        GLuint vertexShader = ShaderUtils::compileShader(GL_VERTEX_SHADER, vertexShaderSource_);
        if (vertexShader == 0) return false;
        GLuint fragmentShader = ShaderUtils::compileShader(GL_FRAGMENT_SHADER, fragmentShaderSource_);
        if (fragmentShader == 0) {
            glDeleteShader(vertexShader);
            return false;
        }
        program_ = ShaderUtils::linkProgram(vertexShader, fragmentShader);
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        if (program_ == 0) return false;
        ySamplerLoc_ = glGetUniformLocation(program_, "ySampler");
        uSamplerLoc_ = glGetUniformLocation(program_, "uSampler");
        vSamplerLoc_ = glGetUniformLocation(program_, "vSampler");
        useInterleavedUVLoc_ = glGetUniformLocation(program_, "useInterleavedUV");
        uvSwapLoc_ = glGetUniformLocation(program_, "uvSwap");
        brightnessLoc_ = glGetUniformLocation(program_, "brightness");
        rotationLoc_ = glGetUniformLocation(program_, "rotation");  // 获取旋转uniform位置
        positionLoc_ = glGetAttribLocation(program_, "aPosition");
        texCoordLoc_ = glGetAttribLocation(program_, "aTexCoord");

        // 禁用抖动以降低片段着色阶段成本
        glDisable(GL_DITHER);
        glGenVertexArrays(1, &vao_);
        glBindVertexArray(vao_);
        static const float vertices[] = {
                -1.0f, -1.0f, 0.0f, 0.0f, 1.0f,
                1.0f, -1.0f, 0.0f, 1.0f, 1.0f,
                -1.0f,  1.0f, 0.0f, 0.0f, 0.0f,
                1.0f,  1.0f, 0.0f, 1.0f, 0.0f
        };
        glGenBuffers(1, &vbo_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glEnableVertexAttribArray(positionLoc_);
        glVertexAttribPointer(positionLoc_, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(texCoordLoc_);
        glVertexAttribPointer(texCoordLoc_, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        glGenFramebuffers(1, &fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glGenTextures(1, &outputTexture_);
        glBindTexture(GL_TEXTURE_2D, outputTexture_);
        // 使用不可变纹理存储以减少驱动开销
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB8, width_, height_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture_, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("[GL] FBO不完整");
            cleanup();
            return false;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindVertexArray(0);
        // 编译并链接 External OES 程序
        GLuint vertexShaderOES = ShaderUtils::compileShader(GL_VERTEX_SHADER, vertexShaderSource_);
        if (vertexShaderOES == 0) return false;
        GLuint fragmentShaderOES = ShaderUtils::compileShader(GL_FRAGMENT_SHADER, fragmentOESShaderSource_);
        if (fragmentShaderOES == 0) {
            glDeleteShader(vertexShaderOES);
            cleanup();
            return false;
        }
        program_oes_ = ShaderUtils::linkProgram(vertexShaderOES, fragmentShaderOES);
        glDeleteShader(vertexShaderOES);
        glDeleteShader(fragmentShaderOES);
        if (program_oes_ == 0) {
            cleanup();
            return false;
        }
        // 获取OES程序的uniform位置
        oesSamplerLoc_ = glGetUniformLocation(program_oes_, "oesSampler");
        brightnessLocOES_ = glGetUniformLocation(program_oes_, "brightness");
        rotationLocOES_ = glGetUniformLocation(program_oes_, "rotation");
        // 预先设置采样器与缓存的亮度/旋转，避免每帧重复设置
        glUseProgram(program_);
        if (ySamplerLoc_ >= 0) glUniform1i(ySamplerLoc_, 0);
        if (uSamplerLoc_ >= 0) glUniform1i(uSamplerLoc_, 1);
        if (vSamplerLoc_ >= 0) glUniform1i(vSamplerLoc_, 2);
        if (brightnessLoc_ >= 0) glUniform1f(brightnessLoc_, brightness_value_);
        if (rotationLoc_ >= 0) glUniform1f(rotationLoc_, rotation_value_);
        if (useLimitedRangeLoc_ >= 0) glUniform1i(useLimitedRangeLoc_, limited_range_value_);
        useBT709Loc_ = glGetUniformLocation(program_, "useBT709");
        if (useBT709Loc_ >= 0) glUniform1i(useBT709Loc_, use_bt709_value_);
        applySRGBLoc_ = glGetUniformLocation(program_, "applySRGB");
        if (applySRGBLoc_ >= 0) glUniform1i(applySRGBLoc_, apply_srgb_value_);
        cropRectLoc_ = glGetUniformLocation(program_, "cropRect");
        enableSquareCropLoc_ = glGetUniformLocation(program_, "enableSquareCrop");

        // 初始化正方形裁切状态和输出尺寸
        if (enableSquareCropLoc_ >= 0 && cropRectLoc_ >= 0) {
            // 启用正方形裁切
            int enableCrop = (width_ != height_) ? 1 : 0;
            glUniform1i(enableSquareCropLoc_, enableCrop);

            if (enableCrop) {
                // 计算裁切矩形（以短边为边，中心裁切）
                int shortSide = (width_ < height_) ? width_ : height_;

                // 设置输出尺寸为正方形
                output_width_ = shortSide;
                output_height_ = shortSide;

                float centerX = 0.5f;
                float centerY = 0.5f;

                // 计算归一化的裁切区域
                float halfSize = (float)shortSide / (2.0f * (float)((width_ > height_) ? width_ : height_));

                float minX, minY, maxX, maxY;
                if (width_ > height_) {
                    // 宽度大于高度，裁切左右两边
                    minX = centerX - halfSize;
                    maxX = centerX + halfSize;
                    minY = 0.0f;
                    maxY = 1.0f;
                } else {
                    // 高度大于宽度，裁切上下两边
                    minX = 0.0f;
                    maxX = 1.0f;
                    minY = centerY - halfSize;
                    maxY = centerY + halfSize;
                }

                glUniform4f(cropRectLoc_, minX, minY, maxX, maxY);
                LOGI("[GL] 检测到图片尺寸不一致(宽=%d, 高=%d)，启用正方形裁切，输出尺寸=%dx%d，裁切区域=(%.3f,%.3f,%.3f,%.3f)",
                     width_, height_, output_width_, output_height_, minX, minY, maxX, maxY);
            } else {
                // 宽高相等，不裁切，使用全图
                output_width_ = width_;
                output_height_ = height_;
                glUniform4f(cropRectLoc_, 0.0f, 0.0f, 1.0f, 1.0f);
            }
        } else {
            // 如果没有裁切功能，输出尺寸与输入相同
            output_width_ = width_;
            output_height_ = height_;
        }
        glUseProgram(program_oes_);
        if (oesSamplerLoc_ >= 0) glUniform1i(oesSamplerLoc_, 0);
        if (brightnessLocOES_ >= 0) glUniform1f(brightnessLocOES_, brightness_value_);
        if (rotationLocOES_ >= 0) glUniform1f(rotationLocOES_, rotation_value_);
        glUseProgram(0);
        // 初始化后标记为不需要更新，直到业务层改变
        brightness_dirty_ = false;
        rotation_dirty_ = false;
        initialized_ = true;
        CHECK_GL_ERROR("YUVToRGBConverter初始化");
        return true;
    }
    bool convert(GLuint yTexture, GLuint uTexture, GLuint vTexture, cv::UMat& output) {
        if (!initialized_) {
            LOGE("[GL] YUVToRGBConverter未初始化");
            return false;
        }
        if (output.empty() || output.size() != cv::Size(width_, height_) || output.type() != CV_8UC3) {
            output.create(height_, width_, CV_8UC3);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glViewport(0, 0, width_, height_);
        glUseProgram(program_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, yTexture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, uTexture);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, vTexture);
        glUniform1i(ySamplerLoc_, 0);
        glUniform1i(uSamplerLoc_, 1);
        glUniform1i(vSamplerLoc_, 2);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        cv::Mat outputMat = output.getMat(cv::ACCESS_WRITE);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width_, height_, GL_RGB, GL_UNSIGNED_BYTE, outputMat.data);
        glPixelStorei(GL_PACK_ALIGNMENT, 4); // 恢复默认值
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindVertexArray(0);
        CHECK_GL_ERROR("YUV转换");
        return true;
    }

    // 设置亮度值
    void setBrightness(float brightness) {
        // 缓存值并延迟到绘制时更新，减少glUseProgram切换
        brightness_value_ = brightness;
        brightness_dirty_ = true;
    }

    // 设置旋转角度（度），顺时针为正，支持任意角度
    void setRotation(float rotation) {
        rotation_value_ = rotation;
        //
        rotation_dirty_ = true;
    }
    void setInterleavedUV(bool use, bool swap) {
        use_interleaved_uv_ = use;
        uv_swap_ = swap;
    }

    void setLimitedRange(bool use) {
        int v = use ? 1 : 0;
        if (v != last_use_limited_range_) {
            limited_range_value_ = v;
            limited_range_dirty_ = true;
            last_use_limited_range_ = v;
        }
    }

    void setBT709(bool use) {
        int v = use ? 1 : 0;
        if (v != last_use_bt709_) {
            use_bt709_value_ = v;
            use_bt709_dirty_ = true;
            last_use_bt709_ = v;
        }
    }

    void setSRGBEncode(bool use) {
        int v = use ? 1 : 0;
        if (v != last_apply_srgb_) {
            apply_srgb_value_ = v;
            apply_srgb_dirty_ = true;
            last_apply_srgb_ = v;
        }
    }

    // 获取输出纹理尺寸（可能是裁切后的正方形）
    int get_output_width() const { return output_width_; }
    int get_output_height() const { return output_height_; }

    // 直接转换为RGB纹理，不经过UMat
    bool convertToTexture(GLuint yTexture, GLuint uTexture, GLuint vTexture, GLuint& outputTexture,bool isFront) {
        if (!initialized_) {
            LOGE("[GL] YUVToRGBConverter未初始化");
            return false;
        }

        // 如果输出纹理未创建，创建一个新的RGB纹理
        if (outputTexture == 0) {
            glGenTextures(1, &outputTexture);
            glBindTexture(GL_TEXTURE_2D, outputTexture);
            // 使用不可变存储以减少驱动开销（使用输出尺寸，可能是正方形）
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB8, output_width_, output_height_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            LOGI("[GL] 创建输出纹理: %dx%d (输入尺寸: %dx%d)", output_width_, output_height_, width_, height_);
        }

        // 将输出纹理绑定到帧缓冲区（仅在变更时更新并检查）
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        if (outputTexture != last_output_texture_) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0);
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                LOGE("[GL] 帧缓冲区不完整: 0x%x", status);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                return false;
            }
            last_output_texture_ = outputTexture;
        }

        // 使用输出尺寸设置viewport（可能是正方形）
        glViewport(0, 0, output_width_, output_height_);
        glUseProgram(program_);

        // 绑定YUV纹理
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, yTexture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, uTexture);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, vTexture);

        // 设置仅变化的uniform（采样器在初始化阶段已设置）
        if (brightness_dirty_ && brightnessLoc_ >= 0) {
            glUniform1f(brightnessLoc_, brightness_value_);
            brightness_dirty_ = false;
        }
        if (rotation_dirty_ && rotationLoc_ >= 0) {
            glUniform1f(rotationLoc_, rotation_value_);
            rotation_dirty_ = false;
        }
        if (useLimitedRangeLoc_ >= 0 && limited_range_dirty_) {
            glUniform1i(useLimitedRangeLoc_, limited_range_value_);
            limited_range_dirty_ = false;
        }
        if (useBT709Loc_ >= 0 && use_bt709_dirty_) {
            glUniform1i(useBT709Loc_, use_bt709_value_);
            use_bt709_dirty_ = false;
        }
        if (applySRGBLoc_ >= 0 && apply_srgb_dirty_) {
            glUniform1i(applySRGBLoc_, apply_srgb_value_);
            apply_srgb_dirty_ = false;
        }
        int use_flag = use_interleaved_uv_ ? 1 : 0;
        if (useInterleavedUVLoc_ >= 0 && use_flag != last_use_interleaved_uv_) {
            glUniform1i(useInterleavedUVLoc_, use_flag);
            last_use_interleaved_uv_ = use_flag;
        }
        int swap_flag = uv_swap_ ? 1 : 0;
        if (uvSwapLoc_ >= 0 && swap_flag != last_uv_swap_) {
            glUniform1i(uvSwapLoc_, 0);
            last_uv_swap_ = swap_flag;
        }

        // 渲染到纹理
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // 恢复状态
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindVertexArray(0);
        glFlush();
        CHECK_GL_ERROR("YUV转RGB纹理");
        return true;
    }

    // 从 External OES 纹理转换到 RGB 目标纹理
    bool convertExternalToTexture(GLuint externalTexture, GLuint& outputTexture) {
        if (!initialized_) {
            LOGE("[GL] YUVToRGBConverter未初始化");
            return false;
        }
        if (outputTexture == 0) {
            glGenTextures(1, &outputTexture);
            glBindTexture(GL_TEXTURE_2D, outputTexture);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB8, width_, height_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        if (outputTexture != last_output_texture_) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0);
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                LOGE("[GL] 帧缓冲区不完整: 0x%x", status);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                return false;
            }
            last_output_texture_ = outputTexture;
        }
        glViewport(0, 0, width_, height_);
        glUseProgram(program_oes_);
        if (brightness_dirty_ && brightnessLocOES_ >= 0) {
            glUniform1f(brightnessLocOES_, brightness_value_);
            brightness_dirty_ = false;
        }
        if (rotation_dirty_ && rotationLocOES_ >= 0) {
            glUniform1f(rotationLocOES_, rotation_value_);
            rotation_dirty_ = false;
        }
        // 绑定 External OES 纹理
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, externalTexture);
        // 采样器已在初始化阶段设置为0
        // 渲染到目标
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindVertexArray(0);
        CHECK_GL_ERROR("External OES 转 RGB 纹理");
        return true;
    }

    void cleanup() {
        // 检查是否有有效的 OpenGL 上下文
        EGLContext ctx = eglGetCurrentContext();
        if (ctx == EGL_NO_CONTEXT) {
            LOGW("[GL] YUVToRGBConverter::cleanup() 无有效 OpenGL 上下文，跳过资源释放");
            // 标记资源 ID 为 0，防止后续误用，但实际资源可能泄漏
            program_ = program_oes_ = vao_ = vbo_ = fbo_ = outputTexture_ = 0;
            initialized_ = false;
            return;
        }
        
        if (program_ != 0) {
            glDeleteProgram(program_);
            program_ = 0;
        }
        if (program_oes_ != 0) {
            glDeleteProgram(program_oes_);
            program_oes_ = 0;
        }
        if (vao_ != 0) {
            glDeleteVertexArrays(1, &vao_);
            vao_ = 0;
        }
        if (vbo_ != 0) {
            glDeleteBuffers(1, &vbo_);
            vbo_ = 0;
        }
        if (fbo_ != 0) {
            glDeleteFramebuffers(1, &fbo_);
            fbo_ = 0;
        }

        if (outputTexture_ != 0) {
            glDeleteTextures(1, &outputTexture_);
            outputTexture_ = 0;
        }
        initialized_ = false;
        LOGI("[GL] YUVToRGBConverter 资源已清理");
    }
    ~YUVToRGBConverter() {
        cleanup();
    }
};

// ============ 全局内存泄漏统计实例 ============
static MemoryLeakStats g_memoryStats;

// ============ 零拷贝 AImage 任务结构（模式2） ============
struct AImageTask {
    AImage* image = nullptr;
    CameraDevice* camera = nullptr;
    int64_t timestamp = 0;
    
    AImageTask() = default;
    AImageTask(AImage* img, CameraDevice* cam) : image(img), camera(cam) {
        if (img) AImage_getTimestamp(img, &timestamp);
    }
    
    // 移动构造
    AImageTask(AImageTask&& other) noexcept 
        : image(other.image), camera(other.camera), timestamp(other.timestamp) {
        other.image = nullptr;
        other.camera = nullptr;
    }
    
    // 移动赋值
    AImageTask& operator=(AImageTask&& other) noexcept {
        if (this != &other) {
            // 释放当前持有的 image
            if (image) AImage_delete(image);
            image = other.image;
            camera = other.camera;
            timestamp = other.timestamp;
            other.image = nullptr;
            other.camera = nullptr;
        }
        return *this;
    }
    
    // 禁用拷贝
    AImageTask(const AImageTask&) = delete;
    AImageTask& operator=(const AImageTask&) = delete;
    
    ~AImageTask() {
        if (image) {
            AImage_delete(image);
            image = nullptr;
        }
    }
    
    bool isValid() const { return image != nullptr && camera != nullptr; }
};

// ============ 线程安全的 YUV 帧队列 ============
template<typename T>
class ThreadSafeQueue {
private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    std::atomic<bool> stopped_{false};
    size_t maxSize_;
    std::atomic<int64_t> dropCount_{0};  // 丢帧计数
    
public:
    explicit ThreadSafeQueue(size_t maxSize = 3) : maxSize_(maxSize) {}
    
    ~ThreadSafeQueue() {
        stop();
    }
    
    // 非阻塞入队，队满时丢弃最旧帧
    bool push(T&& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopped_) return false;
        
        // 如果队列满了，丢弃最旧的帧
        while (queue_.size() >= maxSize_) {
            queue_.pop();
            dropCount_.fetch_add(1, std::memory_order_relaxed);
        }
        
        queue_.push(std::move(item));
        lock.unlock();
        cond_.notify_one();
        return true;
    }
    
    // 阻塞出队，超时返回 false
    bool pop(T& item, int timeoutMs = 100) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cond_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                          [this] { return !queue_.empty() || stopped_; })) {
            if (stopped_ && queue_.empty()) return false;
            item = std::move(queue_.front());
            queue_.pop();
            return true;
        }
        return false;
    }
    
    // 尝试非阻塞出队
    bool tryPop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty() || stopped_) return false;
        item = std::move(queue_.front());
        queue_.pop();
        return true;
    }
    
    void stop() {
        stopped_ = true;
        cond_.notify_all();
    }
    
    void clear() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!queue_.empty()) queue_.pop();
    }
    
    bool isStopped() const { return stopped_; }
    
    size_t size() const {
        std::unique_lock<std::mutex> lock(mutex_);
        return queue_.size();
    }
    
    int64_t getDropCount() const { return dropCount_.load(std::memory_order_relaxed); }
    
    void resetDropCount() { dropCount_ = 0; }
    
    void restart() {
        stopped_ = false;
        dropCount_ = 0;
    }
};

// ============ YUV 处理线程类（支持零拷贝模式） ============
class YUVProcessingThread {
private:
    std::unique_ptr<std::thread> thread_;
    ThreadSafeQueue<AImageTask> imageTaskQueue_;  // 零拷贝：直接传递 AImage
    std::atomic<bool> running_{false};
    bool isFront_ = false;
    std::string name_;
    
    // 线程内 OpenGL 资源
    std::unique_ptr<YUVToRGBConverter> yuvConverter_ = nullptr;
    GLuint cachedYTexture_ = 0;
    GLuint cachedUTexture_ = 0;
    GLuint cachedVTexture_ = 0;
    GLuint cachedUVTextureRG_ = 0;
    int cachedWidth_ = 0;
    int cachedHeight_ = 0;
    
    // 统计
    std::atomic<int64_t> processedFrames_{0};
    std::atomic<int64_t> totalProcessTimeUs_{0};
    
    // 缓冲区（减少频繁分配）
    std::vector<uint8_t> uvExtractBuffer_;
    
    // 初始化线程的 EGL 上下文和 OpenGL 资源
    bool initGLResources() {
        auto egl_manager = EGLContextManager::getInstance();
        
        // 为此线程创建/激活共享 EGL 上下文
        auto contextType = isFront_ ? 
            EGLContextManager::ContextType::CAPTURE_CONTEXT :
            EGLContextManager::ContextType::RECORDING_CONTEXT;
            
        if (!egl_manager->createSharedContext(contextType)) {
            LOGE("[YUVThread][%s] 创建共享 EGL 上下文失败", name_.c_str());
            return false;
        }
        
        if (!egl_manager->activateCurrentThreadContext()) {
            LOGE("[YUVThread][%s] 激活 EGL 上下文失败", name_.c_str());
            return false;
        }
        
        if (!initEglExtensions(egl_manager->getMainDisplay())) {
            LOGE("[YUVThread][%s] EGL 扩展初始化失败", name_.c_str());
            return false;
        }
        
        yuvConverter_ = std::make_unique<YUVToRGBConverter>();
        
        LOGI("[YUVThread][%s] OpenGL 资源初始化成功", name_.c_str());
        return true;
    }
    
    // 清理 OpenGL 资源
    void cleanupGLResources() {
        // 检查是否有有效的 OpenGL 上下文
        EGLContext ctx = eglGetCurrentContext();
        if (ctx == EGL_NO_CONTEXT) {
            LOGW("[YUVThread][%s] 无有效 OpenGL 上下文，跳过资源释放", name_.c_str());
            cachedYTexture_ = cachedUTexture_ = cachedVTexture_ = cachedUVTextureRG_ = 0;
            yuvConverter_.reset();
        } else {
            if (cachedYTexture_ != 0) {
                glDeleteTextures(1, &cachedYTexture_);
                LOGD("[YUVThread][%s] 删除 cachedYTexture_=%d", name_.c_str(), cachedYTexture_);
                cachedYTexture_ = 0;
            }
            if (cachedUTexture_ != 0) {
                glDeleteTextures(1, &cachedUTexture_);
                LOGD("[YUVThread][%s] 删除 cachedUTexture_=%d", name_.c_str(), cachedUTexture_);
                cachedUTexture_ = 0;
            }
            if (cachedVTexture_ != 0) {
                glDeleteTextures(1, &cachedVTexture_);
                LOGD("[YUVThread][%s] 删除 cachedVTexture_=%d", name_.c_str(), cachedVTexture_);
                cachedVTexture_ = 0;
            }
            if (cachedUVTextureRG_ != 0) {
                glDeleteTextures(1, &cachedUVTextureRG_);
                LOGD("[YUVThread][%s] 删除 cachedUVTextureRG_=%d", name_.c_str(), cachedUVTextureRG_);
                cachedUVTextureRG_ = 0;
            }
            
            // 释放 YUV 转换器（会调用其 cleanup()）
            yuvConverter_.reset();
        }
        
        // 销毁当前线程的 EGL 上下文（释放 GPU 资源）
        auto egl_manager = EGLContextManager::getInstance();
        if (egl_manager) {
            egl_manager->destroyCurrentThreadContext();
            LOGI("[YUVThread][%s] EGL 上下文已销毁", name_.c_str());
        }
        
        LOGI("[YUVThread][%s] OpenGL 资源已清理", name_.c_str());
    }
    
    // 创建或复用纹理（直接从 AImage 数据上传）
    void uploadTexture(GLuint& texture, int w, int h, const uint8_t* data, 
                       int stride, int pixelStride) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (texture == 0) {
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, w, h);
        } else {
            glBindTexture(GL_TEXTURE_2D, texture);
        }
        
        if (pixelStride == 2) {
            // 交错 UV 提取到临时缓冲区
            size_t bufSize = static_cast<size_t>(w * h);
            if (uvExtractBuffer_.size() < bufSize) uvExtractBuffer_.resize(bufSize);
            for (int y = 0; y < h; y++) {
                const uint8_t* srcRow = data + y * stride;
                uint8_t* dstRow = uvExtractBuffer_.data() + y * w;
                for (int x = 0; x < w; x++) {
                    dstRow[x] = srcRow[x * 2];
                }
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, uvExtractBuffer_.data());
        } else {
            if (stride == w) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, data);
            } else {
                glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, data);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            }
        }
    }

    // 上传交错 UV 为 RG 纹理
    void uploadUVTextureRG(GLuint& texture, int w, int h, const uint8_t* data, int stride, int dataLen) {
        //LOGI("[Debug] U Plane: len=%d, stride=%d, w=%d, h=%d", dataLen, stride, w, h);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (texture == 0) {
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG8, w, h);
        } else {
            glBindTexture(GL_TEXTURE_2D, texture);
        }
        
        // 简单检查数据长度是否足够（防止访问越界导致Crash）
        int requiredBytes = stride * h;
        if (stride == w * 2) {
             requiredBytes = w * h * 2;
        }

        if (stride == w * 2) {
            // 显式重置 ROW_LENGTH 为 0，确保使用紧凑模式
           // glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 2);
           // glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            // [Safety Fix] 如果数据长度不足，哪怕只少1字节，glTexSubImage2D 读取时也会越界 Crash
            // 因此必须回退到内存拷贝模式：拷贝有效数据到临时缓冲区，并补齐尾部
            if (dataLen < requiredBytes) {
                // Resize 临时缓冲区
                size_t neededSize = (size_t)requiredBytes;
                if (uvExtractBuffer_.size() < neededSize) {
                    uvExtractBuffer_.resize(neededSize);
                }
                
                // 拷贝有效数据
//                memcpy(uvExtractBuffer_.data(), data, dataLen);
//                // 补齐剩余不足的字节（通常只有1-2字节）
//                memset(uvExtractBuffer_.data() + dataLen, 0x80, neededSize - dataLen);
//                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RG, GL_UNSIGNED_BYTE, uvExtractBuffer_.data());
                int lastRow = h - 1;
                int lastCol = w - 1;

                // 上传前面完整的行
                if (lastRow > 0) {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, lastRow,
                                    GL_RG, GL_UNSIGNED_BYTE, data);
                }

                // 上传最后一行，但少一个像素
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, lastRow, lastCol, 1,
                                GL_RG, GL_UNSIGNED_BYTE,
                                data + lastRow * w * 2);
                // 使用临时缓冲区上传
               // glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h-1, GL_RG, GL_UNSIGNED_BYTE, data);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0); // 恢复默认
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RG, GL_UNSIGNED_BYTE, data); 
            }
        } else {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 2);
            
            // 同上，非紧凑模式下的安全检查
//             if (dataLen < requiredBytes) {
//                 size_t neededSize = (size_t)requiredBytes;
//                 if (uvExtractBuffer_.size() < neededSize) {
//                     uvExtractBuffer_.resize(neededSize);
//                 }
//                 memcpy(uvExtractBuffer_.data(), data, dataLen);
//                 memset(uvExtractBuffer_.data() + dataLen, 0, neededSize - dataLen);
//
//                 glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RG, GL_UNSIGNED_BYTE, uvExtractBuffer_.data());
//             } else {
//
//             }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RG, GL_UNSIGNED_BYTE, data);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
    }
    
    // 零拷贝：直接处理 AImage
    bool processAImageTask(AImageTask& task);
    
    // 线程主循环（零拷贝模式）
    // 线程主循环（零拷贝模式）
    void threadLoop();
    void logPerformanceStats(); // 提取的FPS统计函数
    
public:
    explicit YUVProcessingThread(bool isFront) 
        : imageTaskQueue_(2)  // 最多缓存 3 帧（平衡延迟和吞吐）
        , isFront_(isFront)
        , name_(isFront ? "Front" : "Back") {
    }
    
    ~YUVProcessingThread() {
        stop();
    }
    
    bool start() {
        if (running_) return true;
        
        running_ = true;
        imageTaskQueue_.restart();
        thread_ = std::make_unique<std::thread>(&YUVProcessingThread::threadLoop, this);
        
        LOGI("[YUVThread][%s] 启动成功", name_.c_str());
        return true;
    }
    
    void stop() {
        if (!running_) return;
        
        running_ = false;
        imageTaskQueue_.stop();
        
        if (thread_ && thread_->joinable()) {
            thread_->join();
        }
        thread_.reset();
        
        LOGI("[YUVThread][%s] 已停止", name_.c_str());
    }
    
    // 零拷贝：提交 AImage 任务
    bool submitAImage(AImage* image, CameraDevice* camera) {
        if (!running_ || !image || !camera) return false;
        AImageTask task(image, camera);
        return imageTaskQueue_.push(std::move(task));
    }
    
    int64_t getProcessedFrames() const { return processedFrames_.load(); }
    int64_t getDroppedFrames() const { return imageTaskQueue_.getDropCount(); }
    size_t getQueueSize() const { return imageTaskQueue_.size(); }
    
    bool isRunning() const { return running_; }
    bool isFront() const { return isFront_; }
};

// 前向声明 CaptureManager（processFrame 需要访问）
class CaptureManager;
// g_capture_manager 定义在 CaptureManager 类后面

// EGLThreadContext结构体已删除，现在使用EGLContextManager统一管理
/**
 * 双目取流+IMU矫正模块
 * 负责实时获取双目图像数据，进行IMU矫正，并调用拼接模块完成全景拼接
 * 拼接结果供其他模块（AI、录像、推流、回显）使用
 */
class CaptureManager {
private:
    // 允许帧回调访问私有方法
    friend void onYUVFrameAvailable(void *context, AImageReader *reader);
    friend class YUVProcessingThread;
    std::atomic<bool> is_running_{false};        // 运行状态
    std::unique_ptr<std::thread> capture_thread_;// 取流线程
    std::mutex frame_mutex_;                     // 帧数据互斥锁

    std::atomic<bool> is_imu_running_{false};  // IMU线程运行状态
    pthread_t imu_thread_;                     // IMU线程
    std::mutex imu_mutex_;
    CameraParams cam_params_ = {
            .fx = 797.1372f,    // 焦距x（像素）
            .fy = 795.2877f,    // 焦距y（像素）
            .sensorWidth = 0.0064f,  // 传感器宽度（m，示例值）
            .sensorHeight = 0.0048f // 传感器高度（m，示例值）
    }; // 相机内参（需标定后赋值）
    //    std::vector<GyroData> gyro_history_; // 陀螺仪历史数据（用于时间对齐）
    std::vector<AccelData> accel_history_; // 加速度计历史数据
    std::mutex imu_history_mutex_; // 保护IMU历史数据的互斥锁
    int64_t last_camera_timestamp_; // 最近一帧相机图像的时间戳（ns）
    const size_t MAX_IMU_HISTORY = 100; // 最大IMU历史数据量（防止内存溢出）
    // 图像数据缓冲区（内存指针方式，提高性能）
//    uint8_t *left_frame_buffer_{nullptr};    // 左目图像缓冲区
//    uint8_t *right_frame_buffer_{nullptr};   // 右目图像缓冲区
//    uint8_t *panorama_frame_buffer_{nullptr};// 拼接结果缓冲区
//    uint8_t *real_camera_buffer_{nullptr};   // 真实摄像头数据缓冲区
//    cv::UMat panorama_frame_buffer_umat_ = cv::UMat::ones(cv::Size(CAM_SIZE_W, CAM_SIZE_H), CV_8UC3);
//    cv::UMat left_frame_buffer_umat = cv::UMat::ones(cv::Size(CAM_SIZE_W, CAM_SIZE_H), CV_8UC3);
//    cv::UMat right_frame_buffer_umat = cv::UMat::ones(cv::Size(CAM_SIZE_W, CAM_SIZE_H), CV_8UC3);
//    cv::UMat real_camera_umat = cv::UMat::ones(cv::Size(CAM_SIZE_W, CAM_SIZE_H), CV_8UC3);

    // OpenGL纹理管理（新的FBO方案）
    GLuint left_texture_id_{0};               // 左摄像头纹理ID
    GLuint right_texture_id_{0};              // 右摄像头纹理ID
    GLuint panorama_texture_id_{0};           // 全景拼接结果纹理ID
    GLuint left_rgb_texture_id_{0};           // 左摄像头RGB纹理ID（直接从YUV转换）
    GLuint right_rgb_texture_id_{0};          // 右摄像头RGB纹理ID（直接从YUV转换）
    // RGB双缓冲池（减少延迟并保持简单轮转）
    GLuint left_rgb_pool_[2] {0, 0};
    GLuint right_rgb_pool_[2] {0, 0};
    int rgb_pool_index_left_{0};
    int rgb_pool_index_right_{0};
    // 全景双缓冲池与FBO（用于快速blit复制与发布）
    GLuint panorama_pool_[2] {0, 0};
    int panorama_pool_index_write_{0};
    int panorama_pool_index_read_{0};
    GLuint panorama_fbo_read_{0};
    GLuint panorama_fbo_draw_{0};
    bool gl_textures_initialized_{false};     // OpenGL纹理是否已初始化
    bool created_egl_context_{false};         // 是否由本类初始化时创建了UI线程EGL上下文
    std::mutex texture_mutex_;                // 纹理访问互斥锁

    // 用于单摄像头模拟双摄像头的标志
    bool use_real_camera_{true};              // 是否使用真实摄像头
    bool has_new_frame_{false};               // 是否有新的摄像头帧数据
    std::mutex camera_data_mutex_;            // 摄像头数据互斥锁
    bool gpu_buffers_initialized_{false};     // GPU缓冲区是否已初始化
    float brightness_{1.0f};                   // 图像亮度值，1.0为正常亮度
                                               // 图像旋转角度（度），顺时针为正，支持任意角度
    float rotation_{0.0f};                     // 图像旋转角度：0=无旋转, 1=90度顺时针, 2=180度, 3=270度
public: float startup_rotation_offset_deg_{+90.0f}; // 启动偏移角（度），顺时针90度
    bool enable_stitching_fps_control_{true}; // 是否启用wb_stitching_process的18帧/秒帧率控制
    // 图像参数（原始采集参数）
    int frame_width_{CAM_SIZE_W};    // 单个相机采集宽度
    int frame_height_{CAM_SIZE_H};   // 单个相机采集高度
    int panorama_width_{5760}; // 拼接后全景宽度
    int panorama_height_{2880};// 拼接后全景高度
    int video_mode_{0};         // 当前视频模式（与 VideoMode 枚举对应）

    // IMU数据
    float imu_data_[6];// IMU数据数组 [加速度x,y,z, 陀螺仪x,y,z, 磁力计x,y,z]
    float accumulated_rotation_z_{0.0f}; // 累积的Z轴旋转角度（度）
    std::chrono::steady_clock::time_point rotation_start_time_; // 旋转开始时间
    bool rotation_tracking_started_{false}; // 是否开始跟踪旋转

    // 水平校正相关变量
    std::chrono::steady_clock::time_point last_horizontal_correction_time_; // 上次水平校正时间
    float current_horizontal_correction_{0.0f}; // 当前水平校正角度（度）
    bool horizontal_correction_enabled_{false}; // 是否启用水平校正
    const float HORIZONTAL_CORRECTION_INTERVAL_MS = 2000.0f; // 2秒校正间隔

    // 基于IMU的旋转基线和更新（每200ms）
    float initial_yaw_deg_{0.0f};            // 初始yaw（xy投影，度）
    bool initial_yaw_set_{false};            // 是否已设置初始yaw
    std::chrono::steady_clock::time_point last_yaw_update_time_; // 上次yaw更新
    const float YAW_UPDATE_INTERVAL_MS = 200.0f; // 200ms更新间隔

    // 合成FPS统计（以全景输出为准）
    std::atomic<float> combined_fps_{0.0f};
    int combined_fps_frame_count_{0};
    std::chrono::steady_clock::time_point combined_fps_window_start_;
    // 每摄像头固定窗口FPS统计（由capture_loop统一打印）
    std::atomic<int> front_cam_frame_count_{0};
    std::atomic<int> back_cam_frame_count_{0};
    std::chrono::steady_clock::time_point per_cam_fps_window_start_;

    // YUV取帧统计（AImageReader成功获取到的原始YUV帧）
    std::atomic<int> yuv_front_frame_count_{0};
    std::atomic<int> yuv_back_frame_count_{0};
    std::chrono::steady_clock::time_point yuv_fps_window_start_;

    // RGB整体转换统计（固定窗口每秒打印）
    std::atomic<int> rgb_conv_frame_count_{0};
    std::atomic<long long> rgb_conv_duration_sum_us_{0};
    std::chrono::steady_clock::time_point rgb_conv_window_start_;

#ifdef DEBUG_DEV_BOARD
    Mat front_img_;// 模拟前置摄像头
    Mat back_img_; // 模拟后置摄像头
#endif
    ASensorManager* sensor_manager_{nullptr};
    const ASensor* accelerometer_{nullptr};
    ASensorEventQueue* sensor_queue_{nullptr};
    ALooper* looper_{nullptr};
    bool init_imu_sensors();
    void stop_imu_sensors();
    static void* imu_loop(void* arg);
    static int handle_imu_events(int fd, int events, void* data);
    void calculateClockOffset();  // 移除static关键字

public:
    explicit CaptureManager(int video_mode) {
        const PanoramaConfig& cfg = get_video_mode_config(video_mode);
        video_mode_      = video_mode;
        frame_width_     = cfg.real_capture_width;
        frame_height_    = cfg.real_capture_height;
        panorama_width_  = cfg.panorama_width;
        panorama_height_ = cfg.panorama_height;
        // 初始化图像缓冲区
        /* 计算YUV420_888所需总大小 */
        //size_t y_size = frame_width_ * frame_height_;
        //size_t uv_size = (frame_width_ / 2) * (frame_height_ / 2);
        // size_t total_buffer_size_ = y_size + uv_size * 2;  // Y + U + V
        size_t frame_size = frame_width_ * frame_height_ * 3;// RGB格式
        size_t panorama_size = panorama_width_ * panorama_height_ * 3;
//        left_frame_buffer_ = new uint8_t[frame_size];
//        right_frame_buffer_ = new uint8_t[frame_size];
//        real_camera_buffer_ = new uint8_t[frame_size];
//        panorama_frame_buffer_ = new uint8_t[panorama_size];

//        real_camera_umat.create(frame_height_, frame_width_, CV_8UC3);
//        left_frame_buffer_umat.create(frame_height_, frame_width_, CV_8UC3);
//        right_frame_buffer_umat.create(frame_height_, frame_width_, CV_8UC3);
//        panorama_frame_buffer_umat_.create(panorama_height_, panorama_width_, CV_8UC3);
        // 启用OpenCL以确保UMat使用GPU加速
//        if (cv::ocl::haveOpenCL()) {
//            cv::ocl::setUseOpenCL(true);
//            LOGI("[Capture] OpenCL已启用，UMat将使用GPU加速");
//        } else {
//            LOGW("[Capture] OpenCL不可用，UMat将回退到CPU处理");
//        }

        // 初始化合成FPS统计窗口
        combined_fps_window_start_ = std::chrono::steady_clock::now();
        combined_fps_frame_count_ = 0;
        combined_fps_.store(0.0f);
        // 初始化每摄像头FPS统计窗口
        per_cam_fps_window_start_ = std::chrono::steady_clock::now();
        // 初始化YUV取帧FPS统计窗口
        yuv_fps_window_start_ = std::chrono::steady_clock::now();
        // 初始化RGB整体转换统计窗口
        rgb_conv_window_start_ = std::chrono::steady_clock::now();
        rgb_conv_frame_count_.store(0, std::memory_order_relaxed);
        rgb_conv_duration_sum_us_.store(0, std::memory_order_relaxed);

        // 初始化OpenGL纹理
        init_gl_textures();

#ifdef DEBUG_DEV_BOARD
        LOGI("[Capture] 图片加载ing");
        front_img_ = imread("/sdcard/aaaa/3200_2400_back.jpg");
        back_img_ = imread("/sdcard/aaaa/3200_2400_front.jpg");
        resize(front_img_, front_img_, Size(frame_width_, frame_height_));
        resize(back_img_, back_img_, Size(frame_width_, frame_height_));
        LOGI("[Capture] 图片加载完成，尺寸: %dx%d", frame_width_, frame_height_);
        if (front_img_.empty() || back_img_.empty()) {
            LOGE("[Capture] 图片加载失败，请检查路径是否正确");
        }
#endif
        // 初始化IMU数据
        memset(imu_data_, 0, sizeof(imu_data_));

        // 初始化水平校正时间
        last_horizontal_correction_time_ = std::chrono::steady_clock::now();

        LOGI("[Capture] 取流管理器初始化完成");
    }

    ~CaptureManager() {
        stop();

        // 释放缓冲区
//        delete[] left_frame_buffer_;
//        delete[] right_frame_buffer_;
//        delete[] real_camera_buffer_;
//        delete[] panorama_frame_buffer_;
//        left_frame_buffer_umat.release();
//        right_frame_buffer_umat.release();
//        real_camera_umat.release();
//        panorama_frame_buffer_umat_.release();
        
        // 清理拼接模块资源
        LOGI("[Capture] 清理拼接模块资源...");
        wb_stitching_cleanup();
        
        // 清理OpenGL纹理
        cleanup_gl_textures();
        
        // 打印 EGL 上下文状态（调试用）
        auto egl_manager = EGLContextManager::getInstance();
        if (egl_manager) {
            egl_manager->printActiveContexts("析构后");
        }

        LOGI("[Capture] 取流管理器已销毁");
    }

//    uint8_t *getLeftFrameBuffer() {
//        return left_frame_buffer_;// 返回私有缓冲区指针
//    }
//    uint8_t *getRightFrameBuffer() {
//        return right_frame_buffer_;// 返回私有缓冲区指针
//    }
//    cv::UMat &getLeftFrameBufferUMat() {
//        return left_frame_buffer_umat;
//    }
//    cv::UMat &getRightFrameBufferUMat() {
//        return right_frame_buffer_umat;
//    }
    
//    uint8_t *getRealCameraBuffer() {
//        return real_camera_buffer_;// 返回真实摄像头缓冲区指针
//    }

    // 获取RGB纹理ID的访问器方法
    GLuint& getLeftRGBTextureId() {
        return left_rgb_texture_id_;
    }
    GLuint& getRightRGBTextureId() {
        return right_rgb_texture_id_;
    }

#ifdef DEBUG_TEXTURE_TO_MAT
    // 调试用的纹理转Mat方法的公共接口
    cv::Mat debugTextureToMat(GLuint texture_id, int width, int height, const std::string& debug_name) {
        return texture_to_mat_debug(texture_id, width, height, debug_name);
    }
#endif

    // 设置新的摄像头帧数据
    void setNewCameraFrame() {
        std::lock_guard<std::mutex> lock(camera_data_mutex_);
        has_new_frame_ = true;
    }

    std::vector<GyroData> alignIMUToCameraFrame(int64_t cameraFrameTimestamp) {
        // 已移除对陀螺仪的操作：不再基于陀螺仪对齐
        /* std::vector<GyroData> aligned;
        std::lock_guard<std::mutex> lock(imu_history_mutex_);
        int64_t exposureStart = cameraFrameTimestamp - 33000000;
        for (const auto& gyro : gyro_history_) {
            if (gyro.timestamp >= exposureStart && gyro.timestamp <= cameraFrameTimestamp) {
                aligned.push_back(gyro);
            }
        }*/
        return {};
    }
    // 检查是否有新的摄像头帧数据
    bool hasNewFrame() {
        std::lock_guard<std::mutex> lock(camera_data_mutex_);
        return has_new_frame_;
    }

    // 重置新帧标志
    void resetNewFrameFlag() {
        std::lock_guard<std::mutex> lock(camera_data_mutex_);
        has_new_frame_ = false;
    }
    std::mutex &getFrameMutex() {
        return frame_mutex_;
    }
    CameraMotion estimateMotion(GyroData gyro, AccelData accel, float dt) {
        CameraMotion motion;
        motion.rotX = gyro.x * dt * (180.0f / M_PI); // 绕X轴（俯仰）
        motion.rotY = gyro.y * dt * (180.0f / M_PI); // 绕Y轴（横滚）
        motion.rotZ = gyro.z * dt * (180.0f / M_PI); // 绕Z轴（偏航）
        float dx = 0.5f * accel.x * dt * dt;
        float dy = 0.5f * accel.y * dt * dt;
        motion.transX = dx * (cam_params_.fx / cam_params_.sensorWidth);
        motion.transY = dy * (cam_params_.fy / cam_params_.sensorHeight);
        return motion;
    }
    /**
     * 启动取流+IMU矫正+拼接工作线程
     */
    bool start_capture_pipeline(bool init_stitching) {
        if (is_running_.load()) {
            LOGI("[Capture] 取流服务已在运行中");
            return false;
        }
        // 从已存储的 video_mode_ 查表获取实际采集尺寸和帧率
        const PanoramaConfig& cfg = get_video_mode_config(video_mode_);
        g_target_fps = cfg.capture_fps;
        // 初始化温控调速模块（默认关闭，上层可通过 wb_thermal_throttle_enable(true) 启用）
        wb_thermal_throttle_init(g_target_fps, video_mode_, 3);
        LOGI("[Capture] 启动取流+IMU矫正+拼接服务... 模式=%d, 取流=%dx%d, 帧率=%dfps",
             video_mode_, cfg.real_capture_width, cfg.real_capture_height, g_target_fps);
        wb_capture_set_rotation(0.0f);
        OrientationConfig config = {
                .complementary_alpha = 0.98f,      // 互补滤波系数
                .use_magnetometer = true,
                .sensor_delay = ORIENTATION_SENSOR_DELAY_CAMERA,
                .enable_low_pass_filter = true,
                .low_pass_alpha = 0.1f,
                .portrait_mode = true,             // 竖屏模式：立着时Roll=0
                .auto_calibrate = true             // 自动校准：启动时设为零点
        };
        wb_orientation_init(&config);
        wb_orientation_start();
        //sleep(3000);
        /* 初始化单个摄像头用于模拟双摄像头 */
#ifndef DEBUG_DEV_BOARD
        // 只初始化后置摄像头，用于模拟双摄像头数据
        //initDualCameras(width, height);
        openCamera(cfg.real_capture_width, cfg.real_capture_height);
        use_real_camera_ = true;
        //LOGI("[Capture] 启用真实摄像头模式，使用单摄像头模拟双摄像头");
#else
        use_real_camera_ = false;
        LOGI("[Capture] 使用测试图像模式");
#endif
//        if (enable_imu) {
//            int ret = pthread_create(&imu_thread_, nullptr, CaptureManager::imu_loop, this);
//            if (ret != 0) {
//                LOGE("[IMU] 创建IMU线程失败");
//                stop_imu_sensors();
//                return false;
//            }
//        }
        // 初始化姿态估计模块

        /* 初始化OpenGL纹理 */
        //init_gl_textures();

        is_running_.store(true);
        // 启动取流线程
        if(init_stitching){
            capture_thread_ = std::make_unique<std::thread>(&CaptureManager::capture_loop, this);
        }
        LOGI("[Capture] 取流+拼接服务启动成功");
        return true;
    }

    /**
     * 停止取流服务
     */
    void stop() {
        if (!is_running_.load()) {
            return;
        }

        LOGI("[Capture] 停止取流服务...");

        // 销毁温控调速模块
        wb_thermal_throttle_destroy();

        wb_photo_saver_shutdown();
        is_running_.store(false);

        // 等待线程结束
        if (capture_thread_ && capture_thread_->joinable()) {
            capture_thread_->join();
        }
        //        if (is_imu_running_.load()) {
        //            is_imu_running_.store(false);
        //            pthread_join(imu_thread_, nullptr);  // 等待IMU线程结束
        //            stop_imu_sensors();
        //        }

        wb_orientation_stop();
        wb_orientation_release();
        LOGI("[Capture] 取流服务已停止");
    }

    /**
     * 获取最新的全景拼接结果（供其他模块使用）
     * @param buffer 输出缓冲区
     * @param width 图像宽度
     * @param height 图像高度
     * @return 是否成功获取
     */
    bool get_latest_panorama_frame(uint8_t *&buffer, int &width, int &height) {
        std::lock_guard<std::mutex> lock(frame_mutex_);

        if (!is_running_.load() /*|| !panorama_frame_buffer_*/) {
            return false;
        }

        //buffer = panorama_frame_buffer_;
        width = panorama_width_;
        height = panorama_height_;

        return true;
    }

    bool get_latest_panorama_frame_opengl(GLuint panorama_texture_id_, int& width, int& height) {
        std::lock_guard<std::mutex> lock(frame_mutex_);


        width = panorama_width_;
        height = panorama_height_;

        return true;
    }

    bool get_latest_panorama_frame_umat(cv::UMat &umat, int &width, int &height) {
        std::lock_guard<std::mutex> lock(frame_mutex_);

        if (!is_running_.load() /*|| panorama_frame_buffer_umat_.empty()*/) {
            return false;
        }

        //umat = panorama_frame_buffer_umat_;
        width = panorama_width_;
        height = panorama_height_;

        return true;
    }

    /**
     * 获取运行状态
     */
    bool is_running() const {
        return is_running_.load();
    }

    // 获取合成FPS（以全景输出为准）
    bool get_combined_fps(float &out_fps) {
        if (!is_running_.load()) {
            return false;
        }
        out_fps = combined_fps_.load();
        return true;
    }

    /**
     * 设置拍摄参数
     * @param width 拼接后全景宽度
     * @param height 拼接后全景高度
     * @param fps 帧率
     * @return 设置是否成功
     */
    bool set_mode(int video_mode) {
        if (is_running_.load()) {
            LOGI("[Capture] 无法在运行时修改参数，请先停止服务");
            return false;
        }
		
		  // 释放旧的缓冲区
//        if (left_frame_buffer_) {
//            delete[] left_frame_buffer_;
//            left_frame_buffer_ = nullptr;
//        }
//        if (right_frame_buffer_) {
//            delete[] right_frame_buffer_;
//            right_frame_buffer_ = nullptr;
//        }
//        if (real_camera_buffer_) {
//            delete[] real_camera_buffer_;
//            real_camera_buffer_ = nullptr;
//        }
//        if (panorama_frame_buffer_) {
//            delete[] panorama_frame_buffer_;
//            panorama_frame_buffer_ = nullptr;
//        }

        const PanoramaConfig& cfg = get_video_mode_config(video_mode);
        video_mode_       = video_mode;
        frame_width_      = cfg.real_capture_width;
        frame_height_     = cfg.real_capture_height;
        panorama_width_   = cfg.panorama_width;
        panorama_height_  = cfg.panorama_height;
        g_target_fps      = cfg.capture_fps;
        g_min_request_fps = cfg.min_request_fps;
        g_max_request_fps = cfg.max_request_fps;

        LOGI("[Capture] 模式设置完成 - %s(%d): 取流 %dx%d, 拼接 %dx%d, fps=%d [%d~%d]",
             cfg.name, video_mode_,
             frame_width_, frame_height_,
             panorama_width_, panorama_height_,
             g_target_fps, g_min_request_fps, g_max_request_fps);
        // 重新分配缓冲区
//        size_t frame_size = frame_width_ * frame_height_ * 3;// RGB格式
//        size_t panorama_size = panorama_width_ * panorama_height_ * 3;

//        left_frame_buffer_ = new uint8_t[frame_size];
//        right_frame_buffer_ = new uint8_t[frame_size];
//        real_camera_buffer_ = new uint8_t[frame_size];
//        panorama_frame_buffer_ = new uint8_t[panorama_size];
//        panorama_frame_buffer_umat_.create(panorama_height_, panorama_width_, CV_8UC3);
//        real_camera_umat.create(frame_height_, frame_width_, CV_8UC3);
        // 同步更新温控帧率配置（视频模式切换时使限速档位立即生效）
        wb_thermal_throttle_update_config(g_target_fps, video_mode_);
        return true;
    }

    /**
     * 获取左右摄像头的OpenGL纹理ID
     */
    bool get_stereo_textures(GLuint &left_texture_id, GLuint &right_texture_id) {
        std::lock_guard<std::mutex> lock(texture_mutex_);

        if (!gl_textures_initialized_) {
            return false;
        }

        // 返回当前双缓冲池的RGB输出纹理ID，供下游直接使用
        left_texture_id = left_rgb_texture_id_;
        right_texture_id = right_rgb_texture_id_;

        return true;
    }

    /**
     * 获取全景拼接结果的OpenGL纹理ID
     */
    bool get_panorama_texture(GLuint &panorama_texture_id) {
        std::lock_guard<std::mutex> lock(texture_mutex_);

        if (!gl_textures_initialized_) {
            return false;
        }
        if(panorama_texture_id_ != 0){
            panorama_texture_id = panorama_texture_id_;
        }else{
            panorama_texture_id = left_rgb_texture_id_;
        }

        return true;
    }
    /**
     * 获取全局帧率
     */
    int get_global_fps(){
        return g_target_fps;
    }

    /**
     * 获取全景拼接尺寸
     */
    void get_panorama_size(int &width, int &height) {
        width = panorama_width_;
        height = panorama_height_;
    }

    /**
     * 设置图像亮度
     * @param brightness 亮度值，1.0为正常亮度，大于1.0增加亮度，小于1.0降低亮度
     */
    void set_brightness(float brightness) {
        // 设置GPU版本的亮度（YUVToRGBConverter）
        // 这里需要在实际使用YUVToRGBConverter的地方调用setBrightness
        // 暂时存储亮度值，在转换时使用
        brightness_ = brightness;
        LOGI("[Capture] 亮度设置为: %.2f", brightness);
    }

    float get_brightness() const {
        return brightness_;
    }

    void set_rotation(float rotation) {
        // 设置GPU版本的旋转角度（YUVToRGBConverter）
        // 旋转角度：0=无旋转, 1=90度顺时针, 2=180度, 3=270度
        // 旋转角度（度），顺时针为正，支持任意角度
        rotation_ = rotation;
        LOGI("[Capture] 旋转角度设置为: %.1f° (连续角度，顺时针为正)", rotation);
    }

    float get_rotation() const {
        return rotation_;
    }

    /**
     * 设置wb_stitching_process函数的帧率控制开关
     * @param enable true=启用18帧/秒帧率控制，false=禁用帧率控制
     */
    void set_stitching_fps_control(bool enable) {
        enable_stitching_fps_control_ = enable;
        LOGI("[Capture] wb_stitching_process帧率控制设置为: %s", enable ? "启用" : "禁用");
    }

    /**
     * 获取wb_stitching_process函数的帧率控制开关状态
     * @return true=已启用帧率控制，false=已禁用帧率控制
     */
    bool get_stitching_fps_control() const {
        return enable_stitching_fps_control_;
    }

    // 记录摄像头帧（前/后）用于固定窗口统计
    void record_camera_frame(bool is_front) {
        if (is_front) {
            front_cam_frame_count_.fetch_add(1, std::memory_order_relaxed);
        } else {
            back_cam_frame_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // 记录YUV取帧（在AImageReader成功返回图像后调用）
    void record_yuv_frame(bool is_front) {
        if (is_front) {
            yuv_front_frame_count_.fetch_add(1, std::memory_order_relaxed);
        } else {
            yuv_back_frame_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // 记录RGB整体转换耗时（微秒），用于固定窗口统计
    void record_rgb_conversion(long long duration_us) {
        rgb_conv_duration_sum_us_.fetch_add(duration_us, std::memory_order_relaxed);
        rgb_conv_frame_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // 获取最新加速度计数据（预处理后）
    bool get_latest_accel(AccelData &out_accel) {
        std::lock_guard<std::mutex> lock(imu_history_mutex_);
        if (accel_history_.empty()) {
            return false;
        }
        out_accel = preprocessAccel(accel_history_.back());
        return true;
    }

private:
    /**
     * 初始化OpenGL纹理
     */
    void init_gl_textures() {
        std::lock_guard<std::mutex> lock(texture_mutex_);

        if (gl_textures_initialized_) {
            return;
        }

        // 使用EGL管理器确保当前线程有正确的EGL上下文
        EGLContextManager* egl_manager = EGLContextManager::getInstance();
        if (!egl_manager) {
            LOGE("[Capture] EGLContextManager实例获取失败");
            return;
        }

        // 检查当前线程是否已有激活的EGL上下文
        EGLContext current_context = eglGetCurrentContext();
        if (current_context == EGL_NO_CONTEXT) {
            LOGD_GL("[Capture] 创建并激活共享EGL上下文...");

            // 先创建共享上下文
            if (egl_manager->createSharedContext(EGLContextManager::ContextType::CAPTURE_CONTEXT)) {
                // 然后激活当前线程的EGL上下文
                if (egl_manager->activateCurrentThreadContext()) {
                    LOGD_GL("[Capture] EGL共享上下文创建激活成功");
                    created_egl_context_ = true;
                } else {
                    LOGE("[Capture] EGL共享上下文激活失败");
                    return;
                }
            } else {
                LOGE("[Capture] EGL共享上下文创建失败");
                return;
            }
        } else {
            LOGD_GL("[Capture] EGL上下文已激活");
        }

        // 创建RGB双缓冲池纹理（左/右），用于YUV/OES转换输出
        for (int i = 0; i < 2; ++i) {
            glGenTextures(1, &left_rgb_pool_[i]);
            glBindTexture(GL_TEXTURE_2D, left_rgb_pool_[i]);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB8, frame_width_, frame_height_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            glGenTextures(1, &right_rgb_pool_[i]);
            glBindTexture(GL_TEXTURE_2D, right_rgb_pool_[i]);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB8, frame_width_, frame_height_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        // 将当前可写纹理ID指向池的起始位置
        left_rgb_texture_id_ = left_rgb_pool_[rgb_pool_index_left_];
        right_rgb_texture_id_ = right_rgb_pool_[rgb_pool_index_right_];

        // 创建全景双缓冲池纹理（用于发布），拼接输出将blit到池中当前写入槽
        for (int i = 0; i < 2; ++i) {
            glGenTextures(1, &panorama_pool_[i]);
            glBindTexture(GL_TEXTURE_2D, panorama_pool_[i]);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGB8, panorama_width_, panorama_height_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, 0);

        // 创建FBO用于快速blit复制
        glGenFramebuffers(1, &panorama_fbo_read_);
        glGenFramebuffers(1, &panorama_fbo_draw_);

        // 检查OpenGL错误
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            LOGE("[Capture] OpenGL纹理初始化失败，错误码: %d", error);
            cleanup_gl_textures();
            return;
        }

        gl_textures_initialized_ = true;
        LOGI("[Capture] OpenGL纹理初始化成功 | 左右纹理:[%d,%d] RGB池:L[%d,%d]R[%d,%d] 全景池:P[%d,%d]FBO[%d,%d]",
             left_texture_id_, right_texture_id_,
             left_rgb_pool_[0], left_rgb_pool_[1], right_rgb_pool_[0], right_rgb_pool_[1],
             panorama_pool_[0], panorama_pool_[1], panorama_fbo_read_, panorama_fbo_draw_);
    }

    /**
     * 清理OpenGL纹理
     */
    void cleanup_gl_textures() {
        std::lock_guard<std::mutex> lock(texture_mutex_);

        if (!gl_textures_initialized_) {
            return;
        }
        
        // 检查是否有有效的 OpenGL 上下文
        EGLContext ctx = eglGetCurrentContext();
        if (ctx == EGL_NO_CONTEXT) {
            LOGW("[Capture] cleanup_gl_textures() 无有效 OpenGL 上下文，尝试激活...");
            // 尝试激活上下文
            auto egl_manager = EGLContextManager::getInstance();
            if (egl_manager && !egl_manager->activateCurrentThreadContext()) {
                LOGE("[Capture] 无法激活 OpenGL 上下文，纹理资源可能泄漏！");
                // 仅标记为未初始化，但资源可能泄漏
                gl_textures_initialized_ = false;
                return;
            }
        }

        // 删除RGB双缓冲池纹理
        for (int i = 0; i < 2; ++i) {
            if (left_rgb_pool_[i] != 0) { 
                glDeleteTextures(1, &left_rgb_pool_[i]); 
                LOGD("[Capture] 删除 left_rgb_pool_[%d]=%d", i, left_rgb_pool_[i]);
                left_rgb_pool_[i] = 0; 
            }
            if (right_rgb_pool_[i] != 0) { 
                glDeleteTextures(1, &right_rgb_pool_[i]); 
                LOGD("[Capture] 删除 right_rgb_pool_[%d]=%d", i, right_rgb_pool_[i]);
                right_rgb_pool_[i] = 0; 
            }
        }
        left_rgb_texture_id_ = 0;
        right_rgb_texture_id_ = 0;
        rgb_pool_index_left_ = 0;
        rgb_pool_index_right_ = 0;

        // 删除全景双缓冲池纹理与FBO
        for (int i = 0; i < 2; ++i) {
            if (panorama_pool_[i] != 0) { 
                glDeleteTextures(1, &panorama_pool_[i]); 
                LOGD("[Capture] 删除 panorama_pool_[%d]=%d", i, panorama_pool_[i]);
                panorama_pool_[i] = 0; 
            }
        }
        if (panorama_fbo_read_ != 0) { 
            glDeleteFramebuffers(1, &panorama_fbo_read_); 
            LOGD("[Capture] 删除 panorama_fbo_read_=%d", panorama_fbo_read_);
            panorama_fbo_read_ = 0; 
        }
        if (panorama_fbo_draw_ != 0) { 
            glDeleteFramebuffers(1, &panorama_fbo_draw_); 
            LOGD("[Capture] 删除 panorama_fbo_draw_=%d", panorama_fbo_draw_);
            panorama_fbo_draw_ = 0; 
        }
        panorama_pool_index_write_ = 0;
        panorama_pool_index_read_ = 0;
        panorama_texture_id_ = 0;  // 发布引用重置

        gl_textures_initialized_ = false;
        
        // 销毁在此处创建的UI线程EGL上下文
        if (created_egl_context_) {
            auto egl_manager = EGLContextManager::getInstance();
            if (egl_manager) {
                egl_manager->destroyCurrentThreadContext();
                LOGI("[Capture] 释放创建时的UI线程 EGL 上下文成功");
            }
            created_egl_context_ = false;
        }

        LOGI("[Capture] OpenGL纹理已清理（共 8 纹理 + 2 FBO）");
    }

    /**
     * 将RGB数据上传到OpenGL纹理
     */
    void upload_frame_to_texture(GLuint texture_id, uint8_t* rgb_data, int width, int height) {
        if (texture_id == 0 || rgb_data == nullptr) {
            return;
        }

        glBindTexture(GL_TEXTURE_2D, texture_id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgb_data);
        glBindTexture(GL_TEXTURE_2D, 0);

        // 检查OpenGL错误
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            LOGE("[Capture] 上传纹理数据失败，纹理ID: %d, 错误码: %d", texture_id, error);
        }
    }

#ifdef DEBUG_TEXTURE_TO_MAT
    /**
     * 将OpenGL纹理转换为Mat数据用于调试 (OpenGL ES兼容版本)
     */
    cv::Mat texture_to_mat_debug(GLuint texture_id, int width, int height, const std::string& debug_name) {
        LOGD("[Debug] 纹理转换: %s ID:%d %dx%d | 全局ctx:%p display:%p 当前ctx:%p",
             debug_name.c_str(), texture_id, width, height,
             wb_get_global_egl_context(), wb_get_global_egl_display(), eglGetCurrentContext());

        if (texture_id == 0) {
            LOGE("[Debug] 无效的纹理ID: %d", texture_id);
            return cv::Mat();
        }

        // 检查并激活EGL上下文
        EGLContext global_context = wb_get_global_egl_context();
        EGLDisplay global_display = wb_get_global_egl_display();
        EGLContext current_context = eglGetCurrentContext();

        EGLSurface temp_surface = EGL_NO_SURFACE;
        bool context_activated_here = false;

        if (current_context == EGL_NO_CONTEXT) {
            if (global_context == EGL_NO_CONTEXT || global_display == EGL_NO_DISPLAY) {
                LOGE("[Debug] %s - 全局EGL上下文或显示无效", debug_name.c_str());
                return cv::Mat();
            }

            LOGI("[Debug] %s - 当前线程无EGL上下文，尝试激活全局上下文", debug_name.c_str());

            // 首先尝试释放主线程中的EGL上下文绑定（如果存在）
            eglMakeCurrent(global_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

            // 获取EGL配置 - 使用更宽松的配置要求
            EGLConfig config;
            EGLint num_configs;
            const EGLint config_attribs[] = {
                    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,  // 降级到ES2以提高兼容性
                    EGL_RED_SIZE, 8,
                    EGL_GREEN_SIZE, 8,
                    EGL_BLUE_SIZE, 8,
                    EGL_ALPHA_SIZE, 8,
                    EGL_NONE
            };

            if (!eglChooseConfig(global_display, config_attribs, &config, 1, &num_configs) || num_configs == 0) {
                LOGE("[Debug] %s - 选择EGL配置失败，错误码: 0x%x", debug_name.c_str(), eglGetError());
                return cv::Mat();
            }

            LOGD("[Debug] %s - 找到 %d 个EGL配置", debug_name.c_str(), num_configs);

            // 创建临时PBuffer表面
            const EGLint pbuffer_attribs[] = {
                    EGL_WIDTH, 1,
                    EGL_HEIGHT, 1,
                    EGL_NONE
            };

            temp_surface = eglCreatePbufferSurface(global_display, config, pbuffer_attribs);
            if (temp_surface == EGL_NO_SURFACE) {
                EGLint error = eglGetError();
                LOGE("[Debug] %s - 创建临时EGL表面失败，错误码: 0x%x", debug_name.c_str(), error);
                return cv::Mat();
            }

            LOGD("[Debug] %s - 临时EGL表面创建成功: %p", debug_name.c_str(), temp_surface);

            // 激活EGL上下文
            if (!eglMakeCurrent(global_display, temp_surface, temp_surface, global_context)) {
                EGLint error = eglGetError();
                LOGE("[Debug] %s - 激活EGL上下文失败，错误码: 0x%x", debug_name.c_str(), error);
                eglDestroySurface(global_display, temp_surface);
                return cv::Mat();
            }

            context_activated_here = true;
            LOGI("[Debug] %s - EGL上下文激活成功", debug_name.c_str());

            // 验证上下文激活
            EGLContext verify_context = eglGetCurrentContext();
            if (verify_context == EGL_NO_CONTEXT) {
                LOGE("[Debug] %s - EGL上下文激活验证失败", debug_name.c_str());
                eglDestroySurface(global_display, temp_surface);
                return cv::Mat();
            }
            LOGD("[Debug] %s - EGL上下文激活验证成功: %p", debug_name.c_str(), verify_context);

        } else {
            LOGD("[Debug] %s - OpenGL上下文已激活: %p", debug_name.c_str(), current_context);
        }

        // 检查纹理是否存在
        GLboolean is_texture = glIsTexture(texture_id);
        if (!is_texture) {
            LOGE("[Debug] 纹理ID %d 不是有效的纹理对象", texture_id);
            // 清理临时EGL表面
            if (context_activated_here && temp_surface != EGL_NO_SURFACE) {
                EGLDisplay global_display = wb_get_global_egl_display();
                eglMakeCurrent(global_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroySurface(global_display, temp_surface);
            }
            return cv::Mat();
        }

        // 创建临时缓冲区 - 使用RGBA格式以提高兼容性
        std::vector<uint8_t> pixel_data(width * height * 4);

        // 保存当前的framebuffer绑定
        GLint current_fbo;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_fbo);
        LOGD("[Debug] 当前FBO: %d", current_fbo);

        // 创建临时framebuffer对象
        GLuint temp_fbo;
        glGenFramebuffers(1, &temp_fbo);
        if (temp_fbo == 0) {
            LOGE("[Debug] 创建临时FBO失败");
            // 清理临时EGL表面
            if (context_activated_here && temp_surface != EGL_NO_SURFACE) {
                EGLDisplay global_display = wb_get_global_egl_display();
                eglMakeCurrent(global_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroySurface(global_display, temp_surface);
            }
            return cv::Mat();
        }

        glBindFramebuffer(GL_FRAMEBUFFER, temp_fbo);
        LOGD("[Debug] 创建临时FBO: %d", temp_fbo);

        // 将纹理附加到framebuffer
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_id, 0);

        // 检查framebuffer状态
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            const char* status_str = "未知状态";
            switch(status) {
                case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: status_str = "不完整附件"; break;
                case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: status_str = "缺少附件"; break;
                case GL_FRAMEBUFFER_UNSUPPORTED: status_str = "不支持的格式"; break;
            }
            LOGE("[Debug] Framebuffer不完整，状态: %d (%s)", status, status_str);
            glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
            glDeleteFramebuffers(1, &temp_fbo);
            // 清理临时EGL表面
            if (context_activated_here && temp_surface != EGL_NO_SURFACE) {
                EGLDisplay global_display = wb_get_global_egl_display();
                eglMakeCurrent(global_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroySurface(global_display, temp_surface);
            }
            return cv::Mat();
        }

        LOGD("[Debug] Framebuffer状态正常，开始读取像素数据");

        // 先尝试RGBA格式读取
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixel_data.data());

        // 检查OpenGL错误
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            LOGE("[Debug] 读取纹理数据失败，纹理ID: %d, 错误码: %d", texture_id, error);

            // 尝试RGB格式
            LOGD("[Debug] 尝试使用RGB格式读取");
            pixel_data.resize(width * height * 3);
            glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixel_data.data());
            error = glGetError();
            if (error != GL_NO_ERROR) {
                LOGE("[Debug] RGB格式读取也失败，错误码: %d", error);
                glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
                glDeleteFramebuffers(1, &temp_fbo);
                // 清理临时EGL表面
                if (context_activated_here && temp_surface != EGL_NO_SURFACE) {
                    EGLDisplay global_display = wb_get_global_egl_display();
                    eglMakeCurrent(global_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                    eglDestroySurface(global_display, temp_surface);
                }
                return cv::Mat();
            }
        }

        // 恢复原来的framebuffer绑定
        glBindFramebuffer(GL_FRAMEBUFFER, current_fbo);
        glDeleteFramebuffers(1, &temp_fbo);

        // 检查数据是否有效（不全为0）
        bool has_data = false;
        int channels = (pixel_data.size() == width * height * 4) ? 4 : 3;
        for (int i = 0; i < std::min(100, (int)pixel_data.size()); i++) {
            if (pixel_data[i] != 0) {
                has_data = true;
                break;
            }
        }

        if (!has_data) {
            LOGW("[Debug] 警告：纹理数据似乎全为0，可能纹理未正确上传数据");
        }

        // 创建Mat对象
        cv::Mat mat;
        if (channels == 4) {
            mat = cv::Mat(height, width, CV_8UC4, pixel_data.data());
            cv::Mat bgr_mat;
            // OpenGL的RGBA格式转换为OpenCV的BGR格式
            cv::cvtColor(mat, bgr_mat, cv::COLOR_RGBA2BGR);
            mat = bgr_mat;
        } else {
            mat = cv::Mat(height, width, CV_8UC3, pixel_data.data());
            cv::Mat bgr_mat;
            // OpenGL的RGB格式转换为OpenCV的BGR格式
            cv::cvtColor(mat, bgr_mat, cv::COLOR_RGB2BGR);
            mat = bgr_mat;
        }

        // OpenGL纹理是上下翻转的，需要翻转
        cv::Mat flipped_mat;
        cv::flip(mat, flipped_mat, 0); // 垂直翻转

        // 复制数据以避免悬空指针
        cv::Mat result = flipped_mat.clone();

        // 输出一些像素值用于验证
        if (result.rows > 0 && result.cols > 0) {
            cv::Vec3b center_pixel = result.at<cv::Vec3b>(result.rows/2, result.cols/2);
            LOGD("[Debug] %s 纹理转Mat成功，尺寸: %dx%d, 中心像素值: (%d,%d,%d)",
                 debug_name.c_str(), width, height,
                 center_pixel[0], center_pixel[1], center_pixel[2]);
        }

        // 清理临时EGL表面（如果我们创建了的话）
        if (context_activated_here && temp_surface != EGL_NO_SURFACE) {
            EGLDisplay global_display = wb_get_global_egl_display();
            // 释放当前线程的EGL上下文绑定
            eglMakeCurrent(global_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            // 销毁临时表面
            eglDestroySurface(global_display, temp_surface);
            LOGD("[Debug] %s - 临时EGL表面已清理", debug_name.c_str());
        }

        return result;
    }
#endif

    // 在转换前轮转RGB池并设置当前输出纹理ID
public: void rotate_rgb_pool_for_camera(bool is_front) {
        if (!gl_textures_initialized_) return;
        if (is_front) {
            rgb_pool_index_left_ = (rgb_pool_index_left_ + 1) % 2;
            left_rgb_texture_id_ = left_rgb_pool_[rgb_pool_index_left_];
        } else {
            rgb_pool_index_right_ = (rgb_pool_index_right_ + 1) % 2;
            right_rgb_texture_id_ = right_rgb_pool_[rgb_pool_index_right_];
        }
    }

    // 将拼接结果快速blit到全景池当前写入槽，并发布
    bool blit_panorama_to_pool_and_publish(GLuint src_texture) {
        if (!gl_textures_initialized_ || src_texture == 0) return false;
        GLuint dst_texture = panorama_pool_[panorama_pool_index_write_];
        if (dst_texture == 0) return false;
        // 附加源到读FBO，目标到写FBO
        glBindFramebuffer(GL_READ_FRAMEBUFFER, panorama_fbo_read_);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src_texture, 0);
        GLenum read_status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
        if (read_status != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("[Capture] 读FBO不完整: 0x%x", read_status);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            return false;
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, panorama_fbo_draw_);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_texture, 0);
        GLenum draw_status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        if (draw_status != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("[Capture] 写FBO不完整: 0x%x", draw_status);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            return false;
        }
        // 执行blit复制
        glBlitFramebuffer(0, 0, panorama_width_, panorama_height_,
                          0, 0, panorama_width_, panorama_height_,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        // 解绑FBO
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        
        // 【关键】刷新 GPU 命令队列，确保纹理数据对其他共享上下文可见
        // 这是跨上下文纹理同步的必要步骤
        glFlush();
        
        CHECK_GL_ERROR("全景blit复制");
        // 发布：切换可读槽，并更新对外发布纹理ID
        panorama_pool_index_read_ = panorama_pool_index_write_;
        panorama_pool_index_write_ = (panorama_pool_index_write_ + 1) % 2;
        panorama_texture_id_ = panorama_pool_[panorama_pool_index_read_];
        return true;
    }

    /**
     * 取流循环
     */
    void capture_loop() {
        LOGI("[Capture] 取流线程启动");
        pthread_setname_np(pthread_self(), "capture_loop");
        // 使用EGL管理器为当前线程创建共享上下文
        EGLContextManager* egl_manager = EGLContextManager::getInstance();
        bool egl_context_activated = false;

        // 创建共享上下文
        if (egl_manager->createSharedContext(EGLContextManager::ContextType::CAPTURE_CONTEXT)) {
            // 激活当前线程的上下文
            if (egl_manager->activateCurrentThreadContext()) {
                egl_context_activated = true;
                LOGD_GL("[Capture] 取流线程EGL共享上下文创建激活成功");
            } else {
                LOGE("[Capture] EGL共享上下文激活失败");
            }
        } else {
            LOGE("[Capture] EGL共享上下文创建失败");
        }

        int frame_count = 0;
        auto last_time = std::chrono::steady_clock::now();

        while (is_running_.load()) {
            try {
                // 1. 获取双目图像数据
                if (!capture_stereo_frames()) {
                    LOGI("[Capture] 获取双目图像失败");
                    int target_fps = g_target_fps;
                    if (target_fps > 0) {
                        auto frame_interval_ms = std::chrono::milliseconds(static_cast<int64_t>(1000.0 / target_fps));
                        std::this_thread::sleep_for(frame_interval_ms);
                    }
                    continue;
                }

                // 2. 获取IMU数据
                //                if (!capture_imu_data()) {
                //                    LOGI("[Capture] 获取IMU数据失败");
                //                }

                // 3. IMU图像矫正
                //                if (!apply_imu_correction()) {
                //                    LOGD("[Capture] IMU图像矫正失败");
                //                }
                int throttle_fps = wb_thermal_throttle_get_fps();
                g_target_fps = throttle_fps;
                //LOGW("[Capture] throttle_fps = %d, g_target_fps = %d", throttle_fps, g_target_fps);
                // 4. 将RGB数据直接上传到OpenGL纹理
                {
                    auto uploadStartTime = std::chrono::high_resolution_clock::now();

                    // 检查OpenGL纹理是否已初始化
                    if (!gl_textures_initialized_) {
                        LOGW("[Capture] OpenGL纹理未初始化，跳过此帧");
                        int target_fps = g_target_fps;
                        if (target_fps > 0) {
                            auto frame_interval_ms = std::chrono::milliseconds(static_cast<int64_t>(1000.0 / target_fps));
                            std::this_thread::sleep_for(frame_interval_ms);
                        }
                        continue;
                    }

                    //                    cv::Mat left = cv::imread("sdcard/Android/data/com.webuild.camera.app/back.jpg");
                    //                    cv::Mat right = cv::imread("sdcard/Android/data/com.webuild.camera.app/front.jpg");
                    //
                    //                    cv::flip(left, left, 0);
                    //                    cv::flip(right, right, 0);
                    //                    // 模拟双摄像头数据（保留用户要求的逻辑）
                    //                    // 将real_camera_buffer_数据分别作为左右摄像头数据
                    //                    uint8_t* left_rgb_data = left.data;
                    //                    uint8_t* right_rgb_data = right.data; // 模拟用同一个摄像头数据
                    //
                    //                    // 上传左摄像头数据到OpenGL纹理
                    //                    upload_frame_to_texture(left_texture_id_, left_rgb_data, 2976, 2976);
                    //
                    //                    // 上传右摄像头数据到OpenGL纹理
                    //                    upload_frame_to_texture(right_texture_id_, right_rgb_data, 2976, 2976);
                    //
                    //                    auto uploadEndTime = std::chrono::high_resolution_clock::now();
                    //                    auto uploadDuration = std::chrono::duration<double, std::milli>(uploadEndTime - uploadStartTime);
                    //                    LOGD("[Capture] 纹理上传耗时: %.2f ms", uploadDuration.count());

#ifdef DEBUG_TEXTURE_TO_MAT
                    // 调试代码：将上传的纹理转换为Mat数据以便在IDE中查看
                    static int debug_frame_count = 0;
                    if (debug_frame_count % 30 == 0) { // 每30帧调试一次，避免过于频繁
                        // 首先验证源数据是否有效
                        bool source_data_valid = false;
                        if (real_camera_buffer_ != nullptr) {
                            // 检查前100个字节是否不全为0
                            for (int i = 0; i < std::min(100, frame_width_ * frame_height_ * 3); i++) {
                                if (real_camera_buffer_[i] != 0) {
                                    source_data_valid = true;
                                    break;
                                }
                            }

                            // 输出源数据的一些像素值
                            if (source_data_valid) {
                                int center_offset = (frame_height_ / 2 * frame_width_ + frame_width_ / 2) * 3;
                                LOGD("[Debug] 源数据有效，中心像素RGB: (%d,%d,%d)",
                                     real_camera_buffer_[center_offset],
                                     real_camera_buffer_[center_offset + 1],
                                     real_camera_buffer_[center_offset + 2]);
                            } else {
                                LOGW("[Debug] 警告：real_camera_buffer_数据似乎全为0");
                            }
                        } else {
                            LOGE("[Debug] 错误：real_camera_buffer_为空指针");
                        }

                        cv::Mat left_debug_mat = texture_to_mat_debug(left_texture_id_, frame_width_, frame_height_, "左摄像头纹理");
                        cv::Mat right_debug_mat = texture_to_mat_debug(right_texture_id_, frame_width_, frame_height_, "右摄像头纹理");

                        // 在此处设置断点，可以在IDE中查看 left_debug_mat 和 right_debug_mat
                        // 这些Mat包含了上传到OpenGL纹理的图像数据
                        LOGD("[Debug] 第%d帧纹理调试数据已准备，源数据有效: %s，可在IDE中查看Mat变量",
                             debug_frame_count, source_data_valid ? "是" : "否");
                    }
                    debug_frame_count++;
#endif

                    // 5. 调用OpenGL拼接处理
                    auto stitchingStartTime = std::chrono::high_resolution_clock::now();

                    // 获取姿态角
                    float roll, pitch, yaw;
                    EulerAngles angles;
                    if (wb_orientation_get_euler(&angles)) {
                        roll = angles.roll;   // 横滚角
                        pitch = angles.pitch; // 俯仰角

                        if(sight_lock_mode == 0){  // 1=跟随镜头
                            yaw = angles.yaw;     // 偏航角
                        }
                    }
                    // 调用新的基于OpenGL纹理的拼接处理
                    GLuint stitching_output_texture = 0;
                    if (wb_stitching_process_with_rotation(left_rgb_texture_id_, right_rgb_texture_id_, frame_height_, frame_height_, yaw, pitch, roll, g_full_model_mode.load() ? 6 : 0, stitching_output_texture)) {
                        //if (wb_stitching_process(left_rgb_texture_id_, right_rgb_texture_id_, frame_height_, frame_height_, stitching_output_texture)) {
                        // 使用FBO进行快速复制到全景三缓冲池并发布
                        if (!blit_panorama_to_pool_and_publish(stitching_output_texture)) {
                            // 如果blit失败，回退直接发布拼接输出纹理
                            panorama_texture_id_ = stitching_output_texture;
                        }
                    } else{
                        LOGW("[Capture] OpenGL全景拼接处理失败");
                    }

                    auto stitchingEndTime = std::chrono::high_resolution_clock::now();
                    auto stitchingDuration = std::chrono::duration<double, std::milli>(stitchingEndTime - stitchingStartTime);
                    //LOGD_GL("[Capture] 拼接耗时: %.2fms", stitchingDuration.count());

                    // 帧率控制：限制wb_stitching_process处理速度为18帧/秒（可通过开关控制）
                    if (enable_stitching_fps_control_) {
                        static auto last_stitching_time = std::chrono::high_resolution_clock::now();
                        const double target_stitching_fps = g_target_fps;
                        const auto target_frame_interval = std::chrono::duration<double, std::milli>(1000.0 / target_stitching_fps);

                        auto current_time = std::chrono::high_resolution_clock::now();
                        auto elapsed_time = current_time - last_stitching_time;

                        if (elapsed_time < target_frame_interval) {
                            auto sleep_duration = target_frame_interval - elapsed_time;
                            std::this_thread::sleep_for(sleep_duration);
                            //                            LOGD_FPS("[FPS_] 帧率控制: 睡眠%.1fms维持%dfps",
                            //                                     std::chrono::duration<double, std::milli>(sleep_duration).count(),
                            //                                     (int)target_stitching_fps);
                        }

                        last_stitching_time = std::chrono::high_resolution_clock::now();
                    }

#ifdef DEBUG_TEXTURE_TO_MAT
                    // 调试代码：将拼接结果纹理转换为Mat数据以便在IDE中查看
                    static int panorama_debug_count = 0;
                    if (panorama_debug_count % 30 == 0) { // 每30帧调试一次，避免过于频繁
                        cv::Mat panorama_debug_mat = texture_to_mat_debug(panorama_texture_id_, panorama_width_, panorama_height_, "全景拼接结果");

                        // 在此处设置断点，可以在IDE中查看 panorama_debug_mat
                        // 这个Mat包含了拼接后的全景图像数据
                        LOGD("[Debug] 第%d帧全景拼接调试数据已准备，可在IDE中查看Mat变量", panorama_debug_count);
                    }
                    panorama_debug_count++;
#endif
                }
                //LOGI("[FPS_] %s frame ",true ? "前置" : "后置");
                // 6. 帧率统计
                frame_count++;
                auto current_time = std::chrono::steady_clock::now();
                auto elapsed_ms0 = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - last_time).count();
                if (elapsed_ms0 >= wb_get_log_interval_ms()) {
                    double fps = frame_count * 1000.0 / elapsed_ms0;
                    LOGD_FPS("[FPS_] 当前帧率: fps = %.2f fps g_target_fps = %d fps", fps, g_target_fps);
                    frame_count = 0;
                    last_time = current_time;
                }

                // 合成FPS统计：以全景输出处理为准，每 g_log_interval_ms 更新一次
                {
                    combined_fps_frame_count_++;
                    auto fps_now = current_time;
                    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(fps_now - combined_fps_window_start_).count();
                    if (elapsed_ms >= wb_get_log_interval_ms()) {
                        float fps_val = (float)combined_fps_frame_count_ * 1000.0f / (float)elapsed_ms;
                        combined_fps_.store(fps_val);
                        combined_fps_frame_count_ = 0;
                        combined_fps_window_start_ = fps_now;
                    }
                }

                // 每摄像头固定窗口FPS打印：时间窗口由 g_log_interval_ms 统一控制
                {
                    auto fps_now = current_time;
                    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(fps_now - per_cam_fps_window_start_).count();
                    if (elapsed_ms >= wb_get_log_interval_ms()) {
                        int front_count = front_cam_frame_count_.exchange(0, std::memory_order_relaxed);
                        int back_count = back_cam_frame_count_.exchange(0, std::memory_order_relaxed);
                        double front_fps = front_count * 1000.0 / (double)elapsed_ms;
                        double back_fps = back_count * 1000.0 / (double)elapsed_ms;
                        LOGD("[FPS_] 相机处理 - 前置:%.1f 后置:%.1f (窗口:%lldms)",
                                front_fps, back_fps, (long long)elapsed_ms);
                        per_cam_fps_window_start_ = fps_now;
                    }
                }

                // 输出节流：根据 g_target_fps 统一控制处理节奏，稳定到指定帧率
                {
                    static int last_target_fps = -1;
                    int target_fps = g_target_fps;
                    if (target_fps > 0) {
                        auto frame_interval_us = std::chrono::microseconds(static_cast<int64_t>(1000000.0 / target_fps));
                        static std::chrono::steady_clock::time_point next_tick = std::chrono::steady_clock::now() + frame_interval_us;
                        auto now = std::chrono::steady_clock::now();
                        // 若目标帧率变化，重置节奏起点
                        if (last_target_fps != target_fps) {
                            next_tick = now + frame_interval_us;
                            last_target_fps = target_fps;
                        }
                        if (now < next_tick) {
                            std::this_thread::sleep_until(next_tick);
                        }
                        // 滚动下一时刻，避免累计漂移
                        next_tick += frame_interval_us;
                        // 若处理过慢（落后多个周期），重置下一时刻为当前时间 + 一个周期
                        if (std::chrono::steady_clock::now() > next_tick + frame_interval_us) {
                            next_tick = std::chrono::steady_clock::now() + frame_interval_us;
                        }
                    }
                }

            } catch (const std::exception &e) {
                LOGE("[Capture] 取流循环异常: %s", e.what());
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        // 清理EGL上下文
        if (egl_context_activated) {
            egl_manager->destroyCurrentThreadContext();
            LOGD_GL("[Capture] 取流线程EGL上下文清理成功");
        }

        LOGI("[Capture] 取流线程结束");
    }

    /**
     * 获取双目图像数据
     * 这里是模拟实现，实际需要调用相机SDK
     */
    bool capture_stereo_frames() {
        // TODO: 实际实现中需要调用双目相机SDK获取图像数据
        // 这里用模拟数据填充缓冲区

        //        if (!left_frame_buffer_ || !right_frame_buffer_) {
        //            return false;
        //        }

#ifdef DEBUG_DEV_BOARD
        if (front_img_.empty() || back_img_.empty()) {
            LOGE("[Capture] 图片数据为空，无法填充缓冲区");
            return false;
        }
#endif
        // 模拟填充图像数据（实际应该从相机获取）
        size_t frame_size = frame_width_ * frame_height_ * 3;
        //memset(left_frame_buffer_, 128, frame_size);   // 灰色填充
        //memset(right_frame_buffer_, 128, frame_size);  // 灰色填充
        last_camera_timestamp_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::system_clock::now().time_since_epoch()
                                                 ).count();
        //LOGD("last_camera_timestamp_=%f", last_camera_timestamp_);

        return true;
    }

    /**
     * 获取IMU数据
     */
    bool capture_imu_data() {
        // TODO: 实际实现中需要调用IMU传感器接口获取数据
        // 这里用模拟数据

        // 模拟IMU数据（加速度、陀螺仪、磁力计）
        imu_data_[0] = 0.0f;// 加速度 x
        imu_data_[1] = 0.0f;// 加速度 y
        imu_data_[2] = 9.8f;// 加速度 z
        imu_data_[3] = 0.0f;// 陀螺仪 x
        imu_data_[4] = 0.0f;// 陀螺仪 y
        imu_data_[5] = 0.0f;// 陀螺仪 z

        return true;
    }

    /**
     * 应用IMU矫正
     */
    bool apply_imu_correction() {
        auto now = std::chrono::system_clock::now();
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now.time_since_epoch()
                                      ).count(); // 转换为自纪元以来的毫秒数
        /*  auto aligned_gyros = alignIMUToCameraFrame(last_camera_timestamp_);
        if (aligned_gyros.empty()) {
            LOGW("[IMU] 无对齐的陀螺仪数据");
            return false;
        }
        // TODO: 实际实现中需要根据IMU数据对图像进行矫正
        GyroData processed_gyro = preprocessGyro(aligned_gyros.back());
        // 这里是模拟实现
        float dt = 0.033f; // 约33ms（或根据实际时间戳计算）
        if (aligned_gyros.size() >= 2) {
            int64_t time_diff = aligned_gyros.back().timestamp - aligned_gyros[0].timestamp;
            dt = static_cast<float>(time_diff) / 1e9f; // 转换为秒
        }
        LOGI("[IMU] 采样时间间隔dt: %.1f ms", dt*1000);

        float horizontal_rot = processed_gyro.z * dt * (180.0f / M_PI);
        if (!rotation_tracking_started_) {
            rotation_start_time_ = std::chrono::steady_clock::now();
            rotation_tracking_started_ = true;
            accumulated_rotation_z_ = 0.0f;
            LOGI("[IMU] 开始跟踪累积旋转角度");
        }
        accumulated_rotation_z_ += horizontal_rot;
        auto current_time = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            current_time - rotation_start_time_).count();
        // 根据IMU数据计算矫正参数
        LOGI("[IMU] horizontal_rot=%f, 累积旋转角度=%.2f°, 经过时间=%lld ms",
             horizontal_rot, accumulated_rotation_z_, elapsed_ms);*/
        auto current_time = std::chrono::steady_clock::now();
        // 获取最新的加速度计数据
        AccelData latest_accel;
        {
            std::lock_guard<std::mutex> lock(imu_history_mutex_);
            if (accel_history_.empty()) {
                LOGD("[IMU] 无加速度计数据，无法计算倾斜角度");
                return false;
            }
            // 获取最新的加速度计数据并预处理
            latest_accel = preprocessAccel(accel_history_.back());
        }

        // 计算俯仰角（绕X轴）和横滚角（绕Y轴）
        const float ax = latest_accel.x;
        const float ay = latest_accel.y;
        const float az = latest_accel.z;

        // 避免az接近0导致计算异常（相机完全倾斜）
        const float az_threshold = 0.1f;
        float pitch_angle = 0.0f; // 俯仰角（度）
        float roll_angle = 0.0f;  // 横滚角（度）

        if (fabs(az) > az_threshold) {
            // 弧度转角度：arctan2返回弧度，乘以180/M_PI
            pitch_angle = atan2(ay, az) * (180.0f / M_PI);
            roll_angle = atan2(ay, ax) * (180.0f / M_PI);
        } else {
            LOGW("[IMU] 加速度计Z轴分量过小，无法准确计算角度");
        }

        // 水平校正功能：每2秒检查一次是否需要校正
        if (horizontal_correction_enabled_) {
            auto correction_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 current_time - last_horizontal_correction_time_).count();

            if (correction_elapsed_ms >= HORIZONTAL_CORRECTION_INTERVAL_MS) {
                // 计算水平校正角度（基于横滚角）
                float target_correction = -roll_angle; // 负号表示反向校正

                // 限制校正角度范围（避免过度校正）
                const float MAX_CORRECTION_ANGLE = 15.0f; // 最大校正15度
                target_correction = std::max(-MAX_CORRECTION_ANGLE,
                                             std::min(MAX_CORRECTION_ANGLE, target_correction));

                // 平滑过渡到新的校正角度（避免突变）
                const float CORRECTION_SMOOTHING = 0.3f; // 平滑系数
                current_horizontal_correction_ = current_horizontal_correction_ * (1.0f - CORRECTION_SMOOTHING) +
                                                 target_correction * CORRECTION_SMOOTHING;

                // 更新校正时间
                last_horizontal_correction_time_ = current_time;

                LOGD_IMU("[IMU] 水平校正 - 横滚:%.1f° 目标校正:%.1f° 当前校正:%.1f°",
                         roll_angle, target_correction, current_horizontal_correction_);

                // 应用水平校正到图像旋转参数
                // 这里将校正角度转换为旋转参数（0-3的范围）
                float corrected_rotation = rotation_;
                if (fabs(current_horizontal_correction_) > 1.0f) { // 只有当校正角度大于1度时才应用
                    // 将校正角度映射到旋转参数
                    // 这里简化处理：每90度对应1个旋转单位
                    float rotation_adjustment = current_horizontal_correction_ / 90.0f;
                    corrected_rotation = fmod(rotation_ + rotation_adjustment + 4.0f, 4.0f);

                    LOGD_IMU("[IMU] 旋转调整: %.2f -> %.2f", rotation_, corrected_rotation);

                    // 这里可以将corrected_rotation应用到YUVToRGBConverter的setRotation方法
                    // 或者直接修改rotation_成员变量
                    rotation_ = corrected_rotation; // 如果需要永久应用校正
                }
            }
        }

        // 打印角度结果（调试用）
        LOGD_IMU("[IMU] 倾斜角度 - 俯仰:%.1f° 横滚:%.1f° 校正:%.1f°",
                 pitch_angle, roll_angle, current_horizontal_correction_);

        //对left_frame_buffer_和right_frame_buffer_进行矫正处理
        //std::lock_guard<std::mutex> lock(frame_mutex_);
        //cv::Mat left_mat(frame_height_, frame_width_, CV_8UC3, left_frame_buffer_);
        //cv::Mat left_rotated = compensateHorizontalRotation(left_mat, horizontal_rot);
        //memcpy(left_frame_buffer_, left_rotated.data, frame_width_ * frame_height_ * 3);

        //cv::Mat right_mat(frame_height_, frame_width_, CV_8UC3, right_frame_buffer_);
        //cv::Mat right_rotated = compensateHorizontalRotation(right_mat, horizontal_rot);
        //memcpy(right_frame_buffer_, right_rotated.data, frame_width_ * frame_height_ * 3);

        return true;
    }
};

// 全局取流管理器实例
static std::unique_ptr<CaptureManager> g_capture_manager = nullptr;

// ============ YUVProcessingThread::threadLoop 实现 ============
void YUVProcessingThread::threadLoop() {
    LOGI("[YUVThread][%s] 线程启动（零拷贝模式）", name_.c_str());
    
    if (!initGLResources()) {
        LOGE("[YUVThread][%s] 初始化失败，线程退出", name_.c_str());
        return;
    }
    
    while (running_) {
        // 提取统计逻辑到单独函数，保持主循环简洁
        logPerformanceStats();

        AImageTask task;
        // 使用较短的超时避免线程空转
        if (imageTaskQueue_.pop(task, 33)) {  // ~30fps
            if (!task.isValid()) continue;
            
            auto start = std::chrono::high_resolution_clock::now();
            
            if (processAImageTask(task)) {
                processedFrames_.fetch_add(1);
            }
            // task 析构时会自动释放 AImage
            
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            totalProcessTimeUs_.fetch_add(duration.count());
        }
    }
    
    // 清空队列中剩余的任务（会自动释放 AImage）
    imageTaskQueue_.clear();
    cleanupGLResources();
    
    LOGI("[YUVThread][%s] 线程结束，处理帧数: %lld, 平均耗时: %.2f ms",
         name_.c_str(),
         (long long)processedFrames_.load(),
         processedFrames_ > 0 ? (totalProcessTimeUs_.load() / 1000.0 / processedFrames_) : 0);
}

extern "C" void wb_capture_notify_performance_stats(double frontFps, double backFps, double convFps, long long avgConvTimeUs);

void YUVProcessingThread::logPerformanceStats() {
    // 仅由前置摄像头线程负责打印，且必须确保 capture_manager 有效
    if (!isFront_ || !g_capture_manager) return;

    // 统一获取一次时间，减少系统调用
    auto now = std::chrono::steady_clock::now();

    // 1. 每摄像头YUV取帧FPS
    auto elapsed_yuv = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_capture_manager->yuv_fps_window_start_).count();
    static double s_last_front_fps = 0;
    static double s_last_back_fps = 0;
    if (elapsed_yuv >= wb_get_log_interval_ms()) {
        int front_yuv = g_capture_manager->yuv_front_frame_count_.exchange(0, std::memory_order_relaxed);
        int back_yuv = g_capture_manager->yuv_back_frame_count_.exchange(0, std::memory_order_relaxed);
        double front_fps = front_yuv * 1000.0 / (double)elapsed_yuv;
        double back_fps = back_yuv * 1000.0 / (double)elapsed_yuv;
        s_last_front_fps = front_fps;
        s_last_back_fps = back_fps;
        LOGD("[FPS_] YUV取帧 - 前置:%.1f 后置:%.1f (窗口:%lldms)",
                 front_fps, back_fps, (long long)elapsed_yuv);
        g_capture_manager->yuv_fps_window_start_ = now;
    }

    // 2. RGB整体转换FPS与耗时
    auto elapsed_rgb = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_capture_manager->rgb_conv_window_start_).count();
    if (elapsed_rgb >= wb_get_log_interval_ms()) {
        int conv_count = g_capture_manager->rgb_conv_frame_count_.exchange(0, std::memory_order_relaxed);
        long long conv_sum_us = g_capture_manager->rgb_conv_duration_sum_us_.exchange(0, std::memory_order_relaxed);
        double conv_fps = conv_count * 1000.0 / (double)elapsed_rgb;
        long long avg_us = conv_count > 0 ? (conv_sum_us / conv_count) : 0;
        LOGD("[FPS_] RGB转换: %.1fFPS 平均耗时:%lldμs (窗口:%lldms)",
             conv_fps, avg_us, (long long)elapsed_rgb);
        g_capture_manager->rgb_conv_window_start_ = now;

        // 回调到 Java 层展示
        // 我们需要保留 front_fps 和 back_fps 的状态，或者在每次 1s 窗口结束时上报
        // 这里为了简单，我们直接在 RGB 转换统计结束时上报所有数据
        // 注意：front_fps 和 back_fps 是在第一个 if 块里计算的，由于 logPerformanceStats 被频繁调用，
        // 我们可以把统计值存起来或者在这里重算
        
        // 为了实时性，我们在 RGB 窗口结束时上报最近的 YUV FPS
        
        wb_capture_notify_performance_stats(s_last_front_fps, s_last_back_fps, conv_fps, avg_us);
    }
}


// ============ YUVProcessingThread::processAImageTask 实现（零拷贝） ============
bool YUVProcessingThread::processAImageTask(AImageTask& task) {
    if (!task.isValid() || !yuvConverter_ || !g_capture_manager) {
        return false;
    }
    auto overall_start_time = std::chrono::high_resolution_clock::now();
    AImage* image = task.image;
    CameraDevice* camera = task.camera;


    // 保存第 200 帧 YUV 原始数据 (NV12 格式: YYYY... UVUVUV...)
    if (300 != 300) {
        char filename[128];
        int32_t w, h;
        AImage_getWidth(image, &w);
        AImage_getHeight(image, &h);
        snprintf(filename, sizeof(filename), "/sdcard/Android/data/com.webuild.camera.app/files/%s_%dx%d_frame200_nv12.yuv",
                 camera->isFront ? "front" : "back", w, h);

        // 确保目录存在
        std::string path_str = filename;
        size_t last_slash = path_str.find_last_of('/');
        if (last_slash != std::string::npos) {
            std::string dir = path_str.substr(0, last_slash);
            // 递归创建目录（对于私有目录，通常 Android 已经创建了顶级目录）
            mkdir("/sdcard/Android/data/com.webuild.camera.app", 0775);
            mkdir("/sdcard/Android/data/com.webuild.camera.app/files", 0775);
        }

        int fd = open(filename, O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd >= 0) {
            // 1. 写入 Y 平面
            uint8_t* yData;
            int ySize; // AImage_getPlaneData expects int* for dataSize
            int32_t yStride, yPixelStride;
            AImage_getPlaneData(image, 0, &yData, &ySize);
            AImage_getPlaneRowStride(image, 0, &yStride);
            AImage_getPlanePixelStride(image, 0, &yPixelStride);
            LOGI("[Debug] Y Plane: len=%d, stride=%d, pixelStride=%d", ySize, yStride, yPixelStride);
            //uint8_t* lastVPixel = yData + 3686400;
            //*lastVPixel = 0x80;
            for (int y = 0; y < h; y++) {
                write(fd, yData + y * yStride, w);
            }

            // 2. 写入 UV 平面 (NV12: U-V 交叉)
            uint8_t *uData, *vData;
            int uSize, vSize;
            int32_t uStride, vStride, uPixelStride, vPixelStride;
            AImage_getPlaneData(image, 1, &uData, &uSize);
            AImage_getPlaneData(image, 2, &vData, &vSize);
            AImage_getPlaneRowStride(image, 1, &uStride);
            AImage_getPlaneRowStride(image, 2, &vStride);
            AImage_getPlanePixelStride(image, 1, &uPixelStride);
            AImage_getPlanePixelStride(image, 2, &vPixelStride);
            LOGI("[Debug] U Plane: len=%d, stride=%d, pixelStride=%d", uSize, uStride, uPixelStride);
            LOGI("[Debug] V Plane: len=%d, stride=%d, pixelStride=%d", vSize, vStride, vPixelStride);

            //            std::vector<uint8_t> uv_row(w);
            //            for (int y = 0; y < h / 2; y++) {
            //                uint8_t* uRowPtr = uData + y * uStride;
            //                uint8_t* vRowPtr = vData + y * vStride;
            //                for (int x = 0; x < w / 2; x++) {
            //                    uv_row[x * 2] = uRowPtr[x * uPixelStride];     // U
            //                    uv_row[x * 2 + 1] = vRowPtr[x * vPixelStride]; // V
            //                }
            //                write(fd, uv_row.data(), w);
            //            }
            for (int y = 0; y < h / 2; ++y)
            {
                uint8_t* uRowPtr = uData + y * uStride;
                uint8_t* vRowPtr = vData + y * vStride;

                for (int x = 0; x < w / 2; ++x)
                {
                    uint8_t uv[2];

                    uv[0] = uRowPtr[x * uPixelStride]; // U
                    uv[1] = vRowPtr[x * vPixelStride]; // V

                    write(fd, uv, 2);  // 每像素写 2 byte
                }
            }
            uint8_t* lastVPixel = vData + 1843199;
            uint8_t value = *lastVPixel;
            LOGI("[Debug] 已成功保存 NV12 原始数据到: value=%d", value);
            //*lastVPixel = 0x80;

            close(fd);
            LOGI("[Debug] 已成功保存 NV12 原始数据到: %s", filename);
        } else {
            LOGE("[Debug] 无法创建 YUV 文件: %s (errno=%d, 原因: %s)",
                 filename, errno, strerror(errno));
        }
    }
    // 获取图像信息
    int32_t width, height, format;
    AImage_getWidth(image, &width);
    AImage_getHeight(image, &height);
    AImage_getFormat(image, &format);
    
    if (format != AIMAGE_FORMAT_YUV_420_888) {
        LOGW("[YUVThread][%s] 非 YUV_420_888 格式=%d", name_.c_str(), format);
        return false;
    }
    
    // 获取 YUV 平面数据（直接使用 AImage 内部缓冲区，零拷贝）
    uint8_t *yData = nullptr, *uData = nullptr, *vData = nullptr;
    int yDataLen = 0, uDataLen = 0, vDataLen = 0;
    int32_t yStride = 0, uStride = 0, vStride = 0;
    int32_t yPixelStride = 0, uPixelStride = 0, vPixelStride = 0;
    
    if (AImage_getPlaneData(image, 0, &yData, &yDataLen) != AMEDIA_OK ||
        AImage_getPlaneData(image, 1, &uData, &uDataLen) != AMEDIA_OK) {
        LOGE("[YUVThread][%s] 获取 Y/U 平面数据失败", name_.c_str());
        return false;
    }
    
    AImage_getPlaneRowStride(image, 0, &yStride);
    AImage_getPlaneRowStride(image, 1, &uStride);
    AImage_getPlanePixelStride(image, 0, &yPixelStride);
    AImage_getPlanePixelStride(image, 1, &uPixelStride);
    
    // 判断是否需要 V 平面数据
    bool interleavedUV = camera->interleavedUVKnown ? camera->interleavedUV 
                                                    : (uPixelStride == 2);
    bool uvSwap = camera->uvSwap;
    
    if (!interleavedUV || !camera->interleavedUVKnown) {
        if (AImage_getPlaneData(image, 2, &vData, &vDataLen) != AMEDIA_OK) {
            LOGE("[YUVThread][%s] 获取 V 平面数据失败", name_.c_str());
            return false;
        }
        AImage_getPlaneRowStride(image, 2, &vStride);
        AImage_getPlanePixelStride(image, 2, &vPixelStride);
        
        // 首次检测 UV 格式
        if (!camera->interleavedUVKnown) {
            interleavedUV = (uPixelStride == 2 && vPixelStride == 2);
            if (interleavedUV) {
                int uvWidth = width / 2;
                int sample = std::min(16, uvWidth);
                int eqNV12 = 0, eqNV21 = 0;
                for (int x = 0; x < sample; ++x) {
                    uint8_t u1 = uData[x * 2 + 1];
                    uint8_t v0 = vData[x * 2];
                    uint8_t u0 = uData[x * 2];
                    uint8_t v1 = vData[x * 2 + 1];
                    if (u1 == v0) eqNV12++;
                    if (u0 == v1) eqNV21++;
                }
                uvSwap = (eqNV21 > eqNV12);
                camera->uvSwap = uvSwap;
                LOGI("[YUVThread][%s] 检测到 %s 格式", name_.c_str(), 
                     uvSwap ? "NV21(VU)" : "NV12(UV)");
            }
            camera->interleavedUV = interleavedUV;
            camera->interleavedUVKnown = true;
        }
    }
    
    int uvWidth = width / 2;
    int uvHeight = height / 2;
    
    // 检查分辨率变化
    if (cachedWidth_ != width || cachedHeight_ != height) {
        if (cachedYTexture_ != 0) {
            glDeleteTextures(1, &cachedYTexture_);
            glDeleteTextures(1, &cachedUTexture_);
            glDeleteTextures(1, &cachedVTexture_);
            if (cachedUVTextureRG_ != 0) glDeleteTextures(1, &cachedUVTextureRG_);
            cachedYTexture_ = cachedUTexture_ = cachedVTexture_ = cachedUVTextureRG_ = 0;
        }
        cachedWidth_ = width;
        cachedHeight_ = height;
        
        if (!yuvConverter_->initialize(width, height)) {
            LOGE("[YUVThread][%s] YUV 转换器初始化失败", name_.c_str());
            return false;
        }
    }
    
    // 直接从 AImage 缓冲区上传纹理（零拷贝核心）
    uploadTexture(cachedYTexture_, width, height, yData, yStride, yPixelStride);
    
    if (interleavedUV) {
        // [Fix] 对于交错 UV (NV12/NV21)，必须使用起始地址较小的那个指针作为 Base
        // 防止出现 NV21 时传入了 offset+1 的 uData，导致数据错位且长度少1字节
        const uint8_t* baseData = uData;
        int baseLen = uDataLen;
        
        if (vData != nullptr && vData < uData) {
            baseData = vData;
            // 如果 vData 在前，说明是 NV21 结构
            baseLen = vDataLen;
             // 二次确认：如果 uData 在 vData 后面，检查 baseLen 是否足够覆盖 uData 的末尾
             if (uData > vData && (uData + uDataLen) > (vData + baseLen)) {
                 // 扩展长度以包含 uData 的末尾
                 baseLen = (uData + uDataLen) - vData;
             }
        }

        uploadUVTextureRG(cachedUVTextureRG_, uvWidth, uvHeight, baseData, uStride, baseLen);
    } else {
        uploadTexture(cachedUTexture_, uvWidth, uvHeight, uData, uStride, uPixelStride);
        uploadTexture(cachedVTexture_, uvWidth, uvHeight, vData, vStride, vPixelStride);
    }
    
    // 轮转 RGB 纹理池
    g_capture_manager->rotate_rgb_pool_for_camera(isFront_);
    
    GLuint& targetRGBTexture = isFront_ ? 
        g_capture_manager->getLeftRGBTextureId() : 
        g_capture_manager->getRightRGBTextureId();
    
    // 设置转换参数
    yuvConverter_->setBrightness(1.0f);
    
    float base = g_capture_manager->startup_rotation_offset_deg_;
    float dyn = g_capture_manager->get_rotation();
    float dynSigned = isFront_ ? dyn : -dyn;
    wb_preview_set_view_rotation(dynSigned);
    float rot = base + dynSigned - 180.0f;
    yuvConverter_->setRotation(rot);
    
    yuvConverter_->setInterleavedUV(interleavedUV, uvSwap);
    yuvConverter_->setLimitedRange(true);
    bool use709 = (width >= 1280 || height >= 720);
    yuvConverter_->setBT709(use709);
    yuvConverter_->setSRGBEncode(false);
    
    GLuint uTexForConvert = interleavedUV ? cachedUVTextureRG_ : cachedUTexture_;
    GLuint vTexForConvert = interleavedUV ? 0 : cachedVTexture_;
    
    if (!yuvConverter_->convertToTexture(cachedYTexture_, uTexForConvert, vTexForConvert, 
                                         targetRGBTexture, isFront_)) {
        LOGE("[YUVThread][%s] YUV 转 RGB 失败", name_.c_str());
        return false;
    }
    
    g_capture_manager->setNewCameraFrame();
    auto overall_end_time = std::chrono::high_resolution_clock::now();
    auto overall_duration = std::chrono::duration_cast<std::chrono::microseconds>(overall_end_time - overall_start_time);
    // 记录帧统计
    if (g_capture_manager) {
        g_capture_manager->record_camera_frame(isFront_);
        g_capture_manager->record_rgb_conversion(static_cast<long long>(overall_duration.count()));
    }
    
    return true;
}

void CaptureManager::calculateClockOffset() {
    struct timespec mono_ts;
    clock_gettime(CLOCK_MONOTONIC, &mono_ts);
    int64_t mono_ns = mono_ts.tv_sec * 1e9 + mono_ts.tv_nsec;
    auto realtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::system_clock::now().time_since_epoch()
                                       ).count();
    g_clock_offset_ = realtime_ns - mono_ns;
}
bool CaptureManager::init_imu_sensors() {
    if (!sensor_manager_) {
        sensor_manager_ = ASensorManager_getInstance();
        calculateClockOffset();
        if (!sensor_manager_) {
            LOGE("[IMU] 无法获取传感器管理器");
            return false;
        }
    }
    ASensorList sensorList;
    int count = ASensorManager_getSensorList(sensor_manager_, &sensorList);
    for (int i = 0; i < count; ++i) {
        const ASensor* sensor = sensorList[i];
        if (ASensor_getType(sensor) == ASENSOR_TYPE_ACCELEROMETER) {
            accelerometers.push_back({
                    sensor,
                    static_cast<int>(ASensor_getHandle(sensor)),  // 确保为int类型
                    std::string(ASensor_getName(sensor)),  // const char* -> std::string
                    std::string(ASensor_getVendor(sensor))   // 同上   // 厂商（如"InvenSense"）
            });
            LOGD_IMU("[IMU] 发现加速度计: %s (handle:%d)", 
                     accelerometers.back().name.c_str(), 
                     accelerometers.back().handle);

        }
    }
    accelerometer_ = ASensorManager_getDefaultSensor(sensor_manager_, ASENSOR_TYPE_ACCELEROMETER);
    if (!accelerometer_) {
        LOGE("[IMU] 缺少必要的传感器accelerometer_");
        return false;
    }

    LOGI("[IMU] ASensorManager_createEventQueue");
    sensor_queue_ = ASensorManager_createEventQueue(
            sensor_manager_,
            looper_,
            1,
            handle_imu_events,
            this  // 将当前CaptureManager实例作为上下文
    );
    if (!sensor_queue_) {
        LOGE("[IMU] 无法创建传感器事件队列");
        return false;
    }
    ASensorEventQueue_enableSensor(sensor_queue_, accelerometer_);
    ASensorEventQueue_setEventRate(sensor_queue_, accelerometer_, ASENSOR_DELAY_CAMERA);

    // 加载或准备加速度计初始值（用于零偏校准）
    load_or_prepare_accel_init();
    return true;
}
void CaptureManager::stop_imu_sensors() {
    if (sensor_queue_) {
        if (accelerometer_) ASensorEventQueue_disableSensor(sensor_queue_, accelerometer_);
        // 已移除对陀螺仪的操作：不再禁用gyroscope_
        // if (gyroscope_) ASensorEventQueue_disableSensor(sensor_queue_, gyroscope_);
        ASensorManager_destroyEventQueue(sensor_manager_, sensor_queue_);
        sensor_queue_ = nullptr;
    }
    if (looper_) {
        ALooper_release(looper_);
        looper_ = nullptr;
    }
    sensor_manager_ = nullptr;
    accelerometer_ = nullptr;
}

inline float NormalizeAngle180(float angle) {
    // 将任意角度归一化到 [-180,180]
    angle = fmodf(angle + 180.0f, 360.0f);
    if (angle < 0) angle += 360.0f;
    return angle - 180.0f;
}

inline float SmoothRotation(float current, float target, float alpha) {
    float delta = NormalizeAngle180(target - current);
    return current + alpha * delta;
}

int CaptureManager::handle_imu_events(int fd, int events, void* data) {
    if (!data) return 1;
    CaptureManager* manager = static_cast<CaptureManager*>(data);
    ASensorEvent event;
    static int64_t last_acceler_ts = 0;
    while (ASensorEventQueue_getEvents(manager->sensor_queue_, &event, 1) > 0) {
        std::lock_guard<std::mutex> lock(manager->imu_mutex_);  // 线程安全
        switch (event.type) {
            case ASENSOR_TYPE_ACCELEROMETER:
                last_acceler_ts = event.timestamp + g_clock_offset_;
                manager->imu_data_[0] = event.acceleration.x;
                manager->imu_data_[1] = event.acceleration.y;
                manager->imu_data_[2] = event.acceleration.z;
                manager->accel_history_.push_back({
                        event.acceleration.x,
                        event.acceleration.y,
                        event.acceleration.z,
                        last_acceler_ts // 时间戳（ns）
                });
                // 首个加速度事件：若未加载初始值，则写入XML并缓存零偏
                if (g_accel_init_need_write_on_first_event) {
                    const float ax0 = event.acceleration.x;
                    const float ay0 = event.acceleration.y;
                    const float az0 = event.acceleration.z;
                    if (write_accel_init_xml(ax0, ay0, az0)) {
                        g_accel_init_bias[0] = ax0;
                        g_accel_init_bias[1] = ay0;
                        g_accel_init_bias[2] = az0;
                        g_accel_init_loaded = true;
                        g_accel_init_need_write_on_first_event = false;
                        LOGD_IMU( "Stored initial accel: %f,%f,%f", ax0, ay0, az0);
                    } else {
                        LOGD_IMU( "Failed to write accel init XML");
                    }
                }

                // 计算当前yaw（基于加速度xy投影）并每200ms更新图像旋转
                {
                    // ---- IMU 回调处理 ----
                    const float ax = event.acceleration.x;
                    const float ay = event.acceleration.y;

                    // 计算当前水平角度（地面水平投影）
                    float yaw_deg = atan2f(ay, ax) * 57.29578f; // rad -> deg
                    if (yaw_deg < 0.0f) yaw_deg += 360.0f;

                    auto now = std::chrono::steady_clock::now();

                    // 初始化初始 yaw（优先使用 XML）
                    if (!manager->initial_yaw_set_) {
                        float yaw_init_deg = yaw_deg;
                        if (g_accel_init_loaded) {
                            const float ax_init = g_accel_init_bias[0];
                            const float ay_init = g_accel_init_bias[1];
                            yaw_init_deg = atan2f(ay_init, ax_init) * 57.29578f;
                            if (yaw_init_deg < 0.0f) yaw_init_deg += 360.0f;
                        }
                        manager->initial_yaw_deg_ = yaw_init_deg;
                        manager->initial_yaw_set_ = true;
                        manager->last_yaw_update_time_ = now;
                        manager->rotation_ = 0.0f;
                        LOGI("[IMU] 初始化 yaw 基线: %.2f° (来源=%s)", manager->initial_yaw_deg_, g_accel_init_loaded ? "XML" : "首帧");
                    } else {
                        // 每 200ms 更新一次图像旋转
                        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - manager->last_yaw_update_time_).count() >=
                            (int)manager->YAW_UPDATE_INTERVAL_MS) {

                            // 目标旋转 = 逆向旋转，使内容与重力垂直
                            float target_rotation = -NormalizeAngle180(yaw_deg - manager->initial_yaw_deg_);

                            // 平滑更新 rotation_
                            const float ROTATION_SMOOTH_ALPHA = 0.25f;
                            manager->rotation_ = SmoothRotation(manager->rotation_, target_rotation, ROTATION_SMOOTH_ALPHA);

                            manager->last_yaw_update_time_ = now;

                            LOGD_IMU("[IMU] 200ms更新: 初始=%.1f° 当前=%.1f° 目标=%.1f° 输出=%.1f°",
                                     manager->initial_yaw_deg_, yaw_deg, target_rotation, manager->rotation_);
                        }
                    }

                    // 额外节流日志：每 g_log_interval_ms 打印当前yaw
                    static auto last_print = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_print).count() >= wb_get_log_interval_ms()) {
                        LOGD_IMU("[IMU] yaw:%.1f° (ax=%.2f ay=%.2f)", yaw_deg, ax, ay);
                        last_print = now;
                    }
                }
                break;
        }
        // 限制历史数据量，避免内存溢出
        if (manager->accel_history_.size() > manager->MAX_IMU_HISTORY) {
            manager->accel_history_.erase(manager->accel_history_.begin());
        }

        // for(int i=0;i<6;i++) {
        //     LOGI("[IMU] manager->imu_data_[%d]=%f", i, manager->imu_data_[i]);
        // }
    }
    return 1;  // 继续监听事件
}

// IMU线程函数
void* CaptureManager::imu_loop(void* arg) {
    if (!arg) return nullptr;
    pthread_setname_np(pthread_self(), "imu_loop");
    CaptureManager* manager = static_cast<CaptureManager*>(arg);
    LOGI("[IMU] 线程启动");

    manager->looper_ = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    if (!manager->looper_) {
        LOGE("[IMU] 无法创建Looper");
        //return false;
    }
    // 第二步：初始化传感器和事件队列（使用当前线程的Looper）
    if (!manager->init_imu_sensors()) {
        LOGE("[IMU] 传感器初始化失败");
        ALooper_release(manager->looper_);
        manager->looper_ = nullptr;
        return nullptr;
    }

    // 第三步：启动事件循环（此时Looper已在当前线程初始化）
    manager->is_imu_running_.store(true);
    while (manager->is_imu_running_.load()) {
        // 轮询事件（设置超时以避免永久阻塞），并主动调用事件处理以防回调异常
        int ident = ALooper_pollAll(100, nullptr, nullptr, nullptr);
        if (ident == ALOOPER_POLL_ERROR) {
            LOGE("[IMU] Looper轮询错误");
            break;
        }
        // 主动提取并处理队列事件，确保加速度数据进入历史
        handle_imu_events(0, 0, manager);
    }

    // 清理资源
    manager->stop_imu_sensors();
    if (manager->looper_) {
        ALooper_release(manager->looper_);
        manager->looper_ = nullptr;
    }

    LOGI("[IMU] 线程退出");
    return nullptr;
}

// 加速度计数据预处理：去零偏 + 低通滤波（分离重力）
AccelData preprocessAccel(AccelData raw) {
    // 1. 零偏校准（使用持久化初始值作为零偏）
    raw.x -= g_accel_init_bias[0];
    raw.y -= g_accel_init_bias[1];
    raw.z -= g_accel_init_bias[2];

    // 2. 低通滤波（保留低频重力分量，抑制高频运动加速度）
    static AccelData filtered = {0, 0, 0};
    const float alpha = 0.8f; // 滤波系数（0.8历史+0.2当前，平滑效果）
    filtered.x = alpha * filtered.x + (1 - alpha) * raw.x;
    filtered.y = alpha * filtered.y + (1 - alpha) * raw.y;
    filtered.z = alpha * filtered.z + (1 - alpha) * raw.z;

    filtered.timestamp = raw.timestamp;
    return filtered;
}


// 预处理：去除零偏和噪声（简单低通滤波）
//陀螺仪已禁用，移除此函数定义。
/*GyroData preprocessGyro(GyroData raw) {
    // 零偏校准（静态时测量的基准值，需提前校准）
    static const float gyroBias[3] = {6e-4f, -6e-4f, 6e-4f}; 
    raw.x -= gyroBias[0];
    raw.y -= gyroBias[1];
    raw.z -= gyroBias[2];
    
    // 低通滤波（抑制高频噪声）
    static GyroData filtered = {0, 0, 0};
    filtered.x = 0.8f * filtered.x + 0.2f * raw.x;
    filtered.y = 0.8f * filtered.y + 0.2f * raw.y;
    filtered.z = 0.8f * filtered.z + 0.2f * raw.z;
    filtered.timestamp = raw.timestamp; // 保留时间戳
    return filtered;
}*/

// 修改compensateMotion函数，专注水平旋转
cv::Mat compensateHorizontalRotation(cv::Mat frame, float rotationZ) {
    int rows = frame.rows;
    int cols = frame.cols;
    
    // 构建水平旋转矩阵（绕图像中心旋转）
    cv::Mat rotMat = cv::getRotationMatrix2D(
            cv::Point2f(cols / 2, rows / 2),  // 旋转中心
            -rotationZ,                       // 反向旋转补偿（IMU检测到的旋转需要反向修正）
            1.0                               // 不缩放
    );
    
    // 应用仿射变换
    cv::Mat compensated;
    cv::warpAffine(
            frame,
            compensated,
            rotMat,
            cv::Size(cols, rows),
            cv::INTER_LINEAR,                 // 双线性插值
            cv::BORDER_CONSTANT,              // 边缘填充方式
            cv::Scalar(0, 0, 0)               // 边缘填充颜色（黑色）
    );
    
    return compensated;
}

// 对图像应用运动补偿（反向旋转和平移）
cv::Mat compensateMotion(cv::Mat frame, CameraMotion motion) {
    int rows = frame.rows;
    int cols = frame.cols;

    // 1. 构建旋转矩阵（绕Z轴为主，抵消抖动）
    cv::Mat rotMat = cv::getRotationMatrix2D(
            cv::Point2f(cols/2, rows/2), // 旋转中心
            -motion.rotZ, // 反向旋转（补偿相机旋转）
            1.0 // 不缩放
    );
    
    // 2. 加入平移补偿（反向平移）
    rotMat.at<double>(0, 2) -= motion.transX;
    rotMat.at<double>(1, 2) -= motion.transY;
    
    // 3. 应用仿射变换（双线性插值，边缘填充黑色）
    cv::Mat compensated;
    cv::warpAffine(
            frame, compensated, rotMat, cv::Size(cols, rows),
            cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0)
    );
    
    // 4. 裁剪边缘（去除黑边，保留中心区域）
    int crop = 30; // 裁剪30像素边缘（根据需求调整）
    if (crop > 0 && cols > 2*crop && rows > 2*crop) {
        return compensated(cv::Rect(crop, crop, cols-2*crop, rows-2*crop));
    }
    return compensated;
}
/**
 * 外部接口实现
 */
bool wb_capture_start(int video_mode, bool init_stitching) {
    try {
        if (!g_capture_manager) {
            return false;
        }
        return g_capture_manager->start_capture_pipeline(init_stitching);
    } catch (const std::exception &e) {
        LOGI("[Capture] 启动异常: ");
        return false;
    }
}
/* 释放单个相机资源 */
void releaseCamera(CameraDevice &camera) {
    LOGI("[Camera] 开始释放%s摄像头资源...", camera.isFront ? "前置" : "后置");
    
    // 0. 重置会话关闭标志（为等待 onClosed 回调做准备）
    camera.sessionClosed.store(false);
    
    // 1. 首先标记停止捕获，阻止新的回调处理
    // 使用 release 语义确保后续的资源释放对回调线程可见
    camera.isCapturing.store(false, std::memory_order_release);
    
    // 2. 等待所有活跃回调完成（最多等待 500ms）
    int waitCount = 0;
    const int maxWait = 50;  // 50 * 10ms = 500ms
    while (camera.activeCallbacks.load(std::memory_order_acquire) > 0 && waitCount < maxWait) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waitCount++;
    }
    if (camera.activeCallbacks.load(std::memory_order_acquire) > 0) {
        LOGW("[Camera] %s摄像头仍有 %d 个活跃回调，强制继续释放", 
             camera.isFront ? "前置" : "后置", 
             camera.activeCallbacks.load(std::memory_order_acquire));
    }
    
    // 3. 请求清理原始模式的 thread_local 资源（模式 0）
#if USE_SEPARATE_YUV_THREAD == 0
    wb_capture_request_sync_mode_cleanup();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
#endif
    
    // 4. 停止 YUV 处理线程（如果启用独立线程模式）
#if USE_SEPARATE_YUV_THREAD > 0
    if (camera.yuvProcessor) {
        LOGI("[YUVThread] 停止%s摄像头处理线程...", camera.isFront ? "前置" : "后置");
        camera.yuvProcessor->stop();
        LOGI("[YUVThread][%s] 停止统计 - 已处理: %lld, 丢帧: %lld",
             camera.isFront ? "Front" : "Back",
             (long long)camera.yuvProcessor->getProcessedFrames(),
             (long long)camera.yuvProcessor->getQueueSize());
        camera.yuvProcessor.reset();
    }
#endif

    // 5. 停止捕获会话（按正确顺序）
    if (camera.session != nullptr) {
        // 先停止重复捕获
        camera_status_t status = ACameraCaptureSession_stopRepeating(camera.session);
        if (status != ACAMERA_OK) {
            LOGW("[Camera] %s stopRepeating 返回 %d（可能已停止）", 
                 camera.isFront ? "前置" : "后置", status);
        }
        
        // 中止所有捕获
        status = ACameraCaptureSession_abortCaptures(camera.session);
        if (status != ACAMERA_OK) {
            LOGW("[Camera] %s abortCaptures 返回 %d", 
                 camera.isFront ? "前置" : "后置", status);
        }
        
        // 等待相机完全停止
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // 关闭会话
        ACameraCaptureSession_close(camera.session);
        camera.session = nullptr;
        
        // 等待 onClosed 回调完成（最多等待 2 秒）
        // 确保回调完毕后再释放相关资源，避免与回调线程竞争
        {
            std::unique_lock<std::mutex> lk(camera.sessionMutex);
            if (!camera.sessionClosed.load()) {
                LOGI("[Camera] 等待%s摄像头 onClosed 回调...", camera.isFront ? "前置" : "后置");
                bool closed = camera.sessionCv.wait_for(lk, std::chrono::seconds(2),
                    [&] { return camera.sessionClosed.load(); });
                if (!closed) {
                    LOGW("[Camera] %s摄像头 onClosed 回调等待超时，继续释放", camera.isFront ? "前置" : "后置");
                }
            }
        }
        LOGI("[Camera] 已释放%s摄像头会话", camera.isFront ? "前置" : "后置");
    }
    
    // 6. 释放 CaptureRequest（如果有）
    if (camera.request != nullptr) {
        ACaptureRequest_free(camera.request);
        camera.request = nullptr;
        LOGI("[Camera] 已释放%s摄像头捕获请求", camera.isFront ? "前置" : "后置");
    }
    
    // 释放 OutputTarget
    if (camera.outputTarget != nullptr) {
        ACameraOutputTarget_free(camera.outputTarget);
        camera.outputTarget = nullptr;
    }
    
    // 释放 SessionOutput 和 Container
    if (camera.output != nullptr) {
        ACaptureSessionOutput_free(camera.output);
        camera.output = nullptr;
    }
    if (camera.container != nullptr) {
        ACaptureSessionOutputContainer_free(camera.container);
        camera.container = nullptr;
    }

    // 7. 关闭 CameraDevice（建议在 ImageReader 之前关闭，断开硬件流）
    if (camera.device != nullptr) {
        ACameraDevice_close(camera.device);
        camera.device = nullptr;
        LOGI("[Camera] 已关闭%s摄像头设备", camera.isFront ? "前置" : "后置");
    }
    
    // 8. 释放 ImageReader
    if (camera.imageReader != nullptr) {
        // 先移除监听，防止回调继续触发
        AImageReader_setImageListener(camera.imageReader, nullptr);
        
        // 关键修复：先删除 Reader，因为它依赖于下面要释放的 Window
        AImageReader_delete(camera.imageReader);
        camera.imageReader = nullptr;
        LOGI("[Camera] 已释放%s摄像头 ImageReader", camera.isFront ? "前置" : "后置");
    }

    // 9. 释放 Window（必须在 ImageReader 销毁之后）
    if (camera.window != nullptr) {
        ANativeWindow_release(camera.window);
        camera.window = nullptr;
        LOGI("[Camera] 已释放%s摄像头 Window", camera.isFront ? "前置" : "后置");
    }

    // 10. 释放 CameraManager（最后释放）
    if (camera.manager != nullptr) {
        ACameraManager_delete(camera.manager);
        camera.manager = nullptr;
        LOGI("[Camera] 已释放%s摄像头管理器", camera.isFront ? "前置" : "后置");
    }
    
    // 重置 UV 格式检测标志
    camera.interleavedUVKnown = false;
    camera.interleavedUV = false;
    camera.uvSwap = false;
    
    // 打印 EGL 上下文状态（调试用）
    auto egl_manager = EGLContextManager::getInstance();
    if (egl_manager) {
        LOGI("[EGL] 释放%s相机后，活跃 EGL 上下文数量: %zu",
             camera.isFront ? "前置" : "后置",
             egl_manager->getActiveContextCount());
    }
    
    LOGI("[Camera] %s摄像头资源释放完成", camera.isFront ? "前置" : "后置");
}

/* 前后摄像头图像回调 - 零拷贝独立线程版本（模式2） */
#if USE_SEPARATE_YUV_THREAD == 2
void onYUVFrameAvailable(void *context, AImageReader *reader) {
    if (!context) return;
    CameraDevice *camera = static_cast<CameraDevice *>(context);
    camera->activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
    
    if (!camera->isCapturing.load(std::memory_order_acquire)) {
        camera->activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    
    static std::atomic<int> framecounter{0};
    static std::atomic<int> performance_counter{0};
    // 获取图像（零拷贝：直接传递指针）
    AImage *image = nullptr;
    media_status_t status = AImageReader_acquireLatestImage(reader, &image);
    
    if (status != AMEDIA_OK || !image) {
        camera->activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        LOGE("%s摄像头获取图像失败，错误码: %d", camera->isFront ? "前置" : "后置", status);
        if (image) AImage_delete(image);
        return;
    }

    // 首帧到达时发出信号，通知 openCamera 中等待的线程启动第二个摄像头
    if (!camera->firstFrameArrived.exchange(true)) {
        LOGI("%s摄像头首帧到达，通知启动第二摄像头", camera->isFront ? "前置" : "后置");
        camera->firstFrameCv.notify_all();
    }

    auto yuv_start_time = std::chrono::high_resolution_clock::now();
    // 跳过前几帧确保稳定性
    int currentFrame = framecounter.fetch_add(1);
    if (currentFrame < 0) {
        AImage_delete(image);
        camera->activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    // 记录 YUV 取帧
    if (g_capture_manager) {
        g_capture_manager->record_yuv_frame(camera->isFront);
    }
    
    // 检查处理线程是否可用
    if (!camera->yuvProcessor || !camera->yuvProcessor->isRunning()) {
        AImage_delete(image);
        camera->activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    
    // 零拷贝：直接提交 AImage 指针到处理线程
    // 注意：image 的所有权转移给处理线程，由处理线程负责释放
    if (!camera->yuvProcessor->submitAImage(image, camera)) {
        // 提交失败，手动释放
        AImage_delete(image);
    }
    // 提交成功后，image 由处理线程管理，回调立即返回
    auto yuv_end_time = std::chrono::high_resolution_clock::now();
    auto yuv_duration = std::chrono::duration_cast<std::chrono::microseconds>(yuv_end_time - yuv_start_time);
    camera->activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    int perfCount = performance_counter.fetch_add(1);
    if (perfCount % 120 == 0) {
        LOGD_GL("[GL] YUV转RGB 帧#%d YUV:%lldμs", currentFrame, yuv_duration.count());
    }
}

// 模式 2 的清理请求函数（空实现，因为模式 2 使用独立线程管理资源）
void wb_capture_request_sync_mode_cleanup() {
    // 模式 2 使用独立线程，资源由 YUVProcessingThread 管理
    // 这里不需要做任何事情
}

#elif USE_SEPARATE_YUV_THREAD == 1
/* 前后摄像头图像回调 - 数据拷贝独立线程版本（模式1，已弃用） */
void onYUVFrameAvailable(void *context, AImageReader *reader) {
    // 模式1 已弃用，直接使用模式0
    if (!context) return;
    CameraDevice *camera = static_cast<CameraDevice *>(context);
    AImage *image = nullptr;
    AImageReader_acquireLatestImage(reader, &image);
    if (image) AImage_delete(image);
}

// 模式 1 的清理请求函数（空实现）
void wb_capture_request_sync_mode_cleanup() {
    // 模式 1 已弃用
}

#else
// 原始模式的清理请求标志
static std::atomic<bool> g_sync_mode_cleanup_requested{false};

// 原始模式清理函数（在回调线程内调用，确保有 OpenGL 上下文）
static void cleanupSyncModeResources(std::unique_ptr<YUVToRGBConverter>& yuvConverter,
                                      GLuint& cachedYTexture, GLuint& cachedUTexture,
                                      GLuint& cachedVTexture, GLuint& cachedUVTextureRG,
                                      bool& isInitialized) {
    LOGI("[GL] 开始清理原始模式资源...");
    
    if (cachedYTexture != 0) {
        glDeleteTextures(1, &cachedYTexture);
        LOGD("[GL] 删除 cachedYTexture=%d", cachedYTexture);
        cachedYTexture = 0;
    }
    if (cachedUTexture != 0) {
        glDeleteTextures(1, &cachedUTexture);
        LOGD("[GL] 删除 cachedUTexture=%d", cachedUTexture);
        cachedUTexture = 0;
    }
    if (cachedVTexture != 0) {
        glDeleteTextures(1, &cachedVTexture);
        LOGD("[GL] 删除 cachedVTexture=%d", cachedVTexture);
        cachedVTexture = 0;
    }
    if (cachedUVTextureRG != 0) {
        glDeleteTextures(1, &cachedUVTextureRG);
        LOGD("[GL] 删除 cachedUVTextureRG=%d", cachedUVTextureRG);
        cachedUVTextureRG = 0;
    }
    yuvConverter.reset();  // 会调用 YUVToRGBConverter::cleanup()
    isInitialized = false;
    
    // 销毁当前线程的 EGL 上下文（释放 GPU 资源，避免 "Gfx dev" 泄漏）
    auto egl_manager = EGLContextManager::getInstance();
    if (egl_manager) {
        egl_manager->destroyCurrentThreadContext();
        LOGI("[GL] EGL 上下文已销毁");
    }
    
    LOGI("[GL] 原始模式资源已清理");
}

// 请求清理原始模式资源
void wb_capture_request_sync_mode_cleanup() {
    g_sync_mode_cleanup_requested = true;
}

/* 前后摄像头图像回调（通过参数区分） - 原始版本（同步处理） */
/* 前后摄像头图像回调（通过参数区分） - 原始版本（同步处理） */
void onYUVFrameAvailable(void *context, AImageReader *reader) {
    if (!context) return;
    CameraDevice *camera = static_cast<CameraDevice *>(context);
    camera->activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
    
    // 线程局部变量：优化的EGL上下文和YUV转换器
    static thread_local std::unique_ptr<YUVToRGBConverter> yuvConverter = nullptr;
    static thread_local bool isInitialized = false;

    // 线程局部纹理缓存（避免频繁创建销毁）
    static thread_local GLuint cachedYTexture = 0;
    static thread_local GLuint cachedUTexture = 0;
    static thread_local GLuint cachedVTexture = 0;
    static thread_local GLuint cachedUVTextureRG = 0; // 交错UV的RG纹理缓存
    static thread_local int cachedWidth = 0;
    static thread_local int cachedHeight = 0;
    
    // 检查是否需要清理资源
    if (g_sync_mode_cleanup_requested.load()) {
        if (isInitialized) {
            cleanupSyncModeResources(yuvConverter, cachedYTexture, cachedUTexture,
                                     cachedVTexture, cachedUVTextureRG, isInitialized);
        }
        g_sync_mode_cleanup_requested = false;
        camera->activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    
    if (!camera->isCapturing.load(std::memory_order_acquire)) {
        camera->activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    // 记录整体帧处理开始时间
    auto overall_start_time = std::chrono::high_resolution_clock::now();

    static std::atomic<int> framecounter{0};
    static std::atomic<int> performance_counter{0};
    // 移除线程局部的FPS统计，改为统一在capture_loop中打印固定窗口

    // 初始化线程局部资源（仅首次调用）
    if (!isInitialized) {
        // 使用EGLContextManager确保当前线程有活跃的EGL上下文
        auto egl_manager = EGLContextManager::getInstance();

        // 首先为当前线程创建共享EGL上下文（如果不存在）
        if (!egl_manager->createSharedContext(EGLContextManager::ContextType::CAPTURE_CONTEXT)) {
            LOGE("[EGL] capture 线程创建共享EGL上下文失败");
            return;
        }

        // 然后激活上下文
        if (!egl_manager->activateCurrentThreadContext()) {
            LOGE("[EGL] capture 线程EGL上下文激活失败");
            return;
        }

        if (!initEglExtensions(egl_manager->getMainDisplay())) {
            LOGE("[EGL] 扩展初始化失败");
            return;
        }

        // 初始化YUV转换器（使用默认分辨率，后续会动态调整）
        yuvConverter = std::make_unique<YUVToRGBConverter>();

        isInitialized = true;
        LOGI("[GL] 线程GL资源初始化成功");
    }

    // 已在函数开头获取 camera 指针
    AImage *image = nullptr;
    media_status_t status = AImageReader_acquireLatestImage(reader, &image);

    if (status != AMEDIA_OK || !image) {
        LOGE("%s摄像头获取图像失败，错误码: %d", camera->isFront ? "前置" : "后置", status);
        camera->activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        AImage_delete(image);
        return;
    }

    // 跳过前几帧确保稳定性
    int currentFrame = framecounter.fetch_add(1);
    if (currentFrame < 10) {
        AImage_delete(image);
        camera->activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    // 记录一次成功的YUV取帧（跳过初始不稳定的前10帧）
    if (g_capture_manager) {
        g_capture_manager->record_yuv_frame(camera->isFront);
    }

    // YUV转换开始时间
    auto yuv_start_time = std::chrono::high_resolution_clock::now();

    try {
        // 获取图像基本信息
        int32_t width, height, format;
        AImage_getWidth(image, &width);
        AImage_getHeight(image, &height);
        AImage_getFormat(image, &format);
        // 非 YUV_420_888 格式直接丢弃（已禁用零拷贝路径）
        if (format != AIMAGE_FORMAT_YUV_420_888) {
            LOGW("[Camera] 非YUV_420_888 格式=%d，丢弃该帧（禁用零拷贝）", format);
            AImage_delete(image);
            camera->activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
            return;
        }

        // 获取YUV平面数据（按需获取V平面，减少不必要的访问）
        AImage_Plane yPlane, uPlane, vPlane;
        if (AImage_getPlaneData(image, 0, &yPlane.data, &yPlane.dataSize) != AMEDIA_OK ||
            AImage_getPlaneData(image, 1, &uPlane.data, &uPlane.dataSize) != AMEDIA_OK) {
            LOGE("[Camera] 获取Y/U平面数据失败");
            AImage_delete(image);
            camera->activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
            return;
        }

        AImage_getPlaneRowStride(image, 0, &yPlane.rowStride);
        AImage_getPlaneRowStride(image, 1, &uPlane.rowStride);

        AImage_getPlanePixelStride(image, 0, &yPlane.pixelStride);
        AImage_getPlanePixelStride(image, 1, &uPlane.pixelStride);

        // 若交错UV已知为 true，则通常无需读取 V 平面数据
        bool needVPlaneData = true;
        if (camera->interleavedUVKnown && camera->interleavedUV) {
            needVPlaneData = false;
        }
        // 若还未判定交错UV或是分离UV路径，需要获取 V 平面数据
        if (needVPlaneData) {
            if (AImage_getPlaneData(image, 2, &vPlane.data, &vPlane.dataSize) != AMEDIA_OK) {
                LOGE("[Camera] 获取V平面数据失败");
                AImage_delete(image);
                camera->activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
                return;
            }
            AImage_getPlaneRowStride(image, 2, &vPlane.rowStride);
            AImage_getPlanePixelStride(image, 2, &vPlane.pixelStride);
        } else {
            // 仍需像素步长信息以便快速判断（不访问数据指针）
            AImage_getPlanePixelStride(image, 2, &vPlane.pixelStride);
            AImage_getPlaneRowStride(image, 2, &vPlane.rowStride);
        }

        // 仅在分辨率变化时输出YUV平面信息，避免每帧日志开销
        static thread_local int lastLoggedWidth = 0;
        static thread_local int lastLoggedHeight = 0;
        if (lastLoggedWidth != width || lastLoggedHeight != height) {
            LOGD_YUV("[YUV] Y=%dx%d(stride=%d,pixel=%d) U/V=%dx%d(stride=%d,pixel=%d/%d)",
                     width, height, yPlane.rowStride, yPlane.pixelStride,
                     width/2, height/2, uPlane.rowStride, uPlane.pixelStride, vPlane.pixelStride);
            lastLoggedWidth = width;
            lastLoggedHeight = height;
        }

        // 创建或重用缓存的纹理
        // 复用UV提取缓冲区，减少每帧分配
        static thread_local std::vector<uint8_t> uvExtractBuffer;
        auto createOrReuseTexture = [](GLuint& texture, int w, int h, const uint8_t* data, int stride, int pixelStride, std::vector<uint8_t>& uvBuf) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            if (texture == 0) {
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                // 使用不可变存储，后续用 SubImage 更新
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, w, h);
            } else {
                glBindTexture(GL_TEXTURE_2D, texture);
            }

            if (pixelStride == 2) {
                // 交错UV提取
                if (uvBuf.size() != static_cast<size_t>(w * h)) uvBuf.resize(w * h);
                for (int y = 0; y < h; y++) {
                    const uint8_t* srcRow = data + y * stride;
                    uint8_t* dstRow = uvBuf.data() + y * w;
                    for (int x = 0; x < w; x++) {
                        dstRow[x] = srcRow[x * pixelStride];
                    }
                }
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, uvBuf.data());
            } else {
                if (stride == w) {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, data);
                } else {
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED, GL_UNSIGNED_BYTE, data);
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                }
            }
        };
        // 专用于交错UV上传为GL_RG的创建/复用函数（移除CPU重排，直接使用行步长）
        auto createOrReuseUVTextureRG = [](GLuint& texture, int w, int h, const uint8_t* data, int stride, bool /*uvSwap*/, std::vector<uint8_t>& /*uvBuf*/) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            if (texture == 0) {
                glGenTextures(1, &texture);
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                // 使用不可变存储，后续使用SubImage按行步长直接更新
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG8, w, h);
            } else {
                glBindTexture(GL_TEXTURE_2D, texture);
            }
            // 使用行步长（以像素为单位）避免CPU重拷贝；stride为字节数，每像素2字节
            if (stride == w * 2) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RG, GL_UNSIGNED_BYTE, data);
            } else {
                glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 2);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RG, GL_UNSIGNED_BYTE, data);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            }
        };

        // 检查是否需要重新创建纹理（分辨率变化）
        if (cachedWidth != width || cachedHeight != height) {
            if (cachedYTexture != 0) {
                glDeleteTextures(1, &cachedYTexture);
                glDeleteTextures(1, &cachedUTexture);
                glDeleteTextures(1, &cachedVTexture);
                if (cachedUVTextureRG != 0) glDeleteTextures(1, &cachedUVTextureRG);
                cachedYTexture = cachedUTexture = cachedVTexture = 0;
                cachedUVTextureRG = 0;
            }
            cachedWidth = width;
            cachedHeight = height;

            // 在尺寸变化时初始化转换器（首次或分辨率变更）
            if (!yuvConverter->initialize(width, height)) {
                LOGE("[GL] YUV转换器初始化失败（尺寸变更）");
                AImage_delete(image);
                camera->activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
                return;
            }
        }

        // 计算UV分量的正确尺寸（YUV420格式：UV为Y的1/4）
        const int uvWidth = width / 2;
        const int uvHeight = height / 2;

        static thread_local int limitedRangeFlag = -1;
        if (limitedRangeFlag == -1) {
            int minY = 255;
            int maxY = 0;
            int stepY = std::max(1, height / 8);
            int stepX = std::max(1, width / 8);
            for (int yy = 0; yy < height; yy += stepY) {
                const uint8_t* row = yPlane.data + yy * yPlane.rowStride;
                for (int xx = 0; xx < width; xx += stepX) {
                    int idx = xx * yPlane.pixelStride;
                    if (idx < yPlane.rowStride) {
                        uint8_t v = row[idx];
                        if (v < minY) minY = v;
                        if (v > maxY) maxY = v;
                    }
                }
            }
            limitedRangeFlag = (minY > 10 && maxY < 246) ? 1 : 0;
        }

        // 检测是否为交错UV（NV12/NV21）并上传（缓存判定减少每帧开销）
        bool interleavedUV = camera->interleavedUVKnown
                                     ? camera->interleavedUV
                                     : (uPlane.pixelStride == 2 && vPlane.pixelStride == 2);
        bool uvSwap = camera->uvSwap; // 默认沿用缓存
        if (!camera->interleavedUVKnown && interleavedUV) {
            // 简单探测一次：比较U行第二字节与V行第一字节的匹配度
            int sample = std::min(16, uvWidth);
            int eqNV12 = 0, eqNV21 = 0;
            const uint8_t* uRow = uPlane.data;
            const uint8_t* vRow = vPlane.data;
            for (int x = 0; x < sample; ++x) {
                uint8_t u0 = uRow[x * 2];
                uint8_t u1 = uRow[x * 2 + 1];
                uint8_t v0 = vRow[x * 2];
                uint8_t v1 = vRow[x * 2 + 1];
                if (u1 == v0) eqNV12++;
                if (u0 == v1) eqNV21++;
            }
            uvSwap = (eqNV21 > eqNV12);
            camera->interleavedUV = true;
            camera->interleavedUVKnown = true;
            camera->uvSwap = uvSwap;
            LOGI("[YUV] %s摄像头使用交错UV路径: %s", camera->isFront ? "前置" : "后置", uvSwap ? "NV21(VU)" : "NV12(UV)");
        }

        // 上传Y分量
        createOrReuseTexture(cachedYTexture, width, height, yPlane.data, yPlane.rowStride, yPlane.pixelStride, uvExtractBuffer);
        if (interleavedUV) {
            // 使用U平面作为源，将交错UV打包为RG
            createOrReuseUVTextureRG(cachedUVTextureRG, uvWidth, uvHeight, uPlane.data, uPlane.rowStride, uvSwap, uvExtractBuffer);
        } else {
            // 分离U/V上传（RED纹理）
            createOrReuseTexture(cachedUTexture, uvWidth, uvHeight, uPlane.data, uPlane.rowStride, uPlane.pixelStride, uvExtractBuffer);
            createOrReuseTexture(cachedVTexture, uvWidth, uvHeight, vPlane.data, vPlane.rowStride, vPlane.pixelStride, uvExtractBuffer);
        }

        // 在转换前轮转RGB池，避免覆盖尚在使用的纹理
        g_capture_manager->rotate_rgb_pool_for_camera(camera->isFront);
        // 获取目标RGB纹理ID
        GLuint& target_rgb_texture = camera->isFront ?
                                                     g_capture_manager->getLeftRGBTextureId() :
                                                     g_capture_manager->getRightRGBTextureId();

        // 设置GPU亮度
        yuvConverter->setBrightness(1.0f);

        // 设置GPU旋转角度：前置与后置动态跟随方向相反
        float base = g_capture_manager->startup_rotation_offset_deg_;
        float dyn = g_capture_manager->get_rotation();

        float dynSigned = camera->isFront ? dyn : -dyn; // 后置反向跟随
        wb_preview_set_view_rotation(dynSigned);
        float rot = base + dynSigned;
        // 统一减去180°以纠正起始倒置（两路生效）
        rot -= 180.0f;
        yuvConverter->setRotation(rot);
//        LOGD("[Rotation] apply: base=%.2f°, dyn=%.2f°(%s), final=%.2f°, front=%d",
//             base, dyn, camera->isFront ? "+" : "-", rot, camera->isFront ? 1 : 0);

        // 执行YUV到RGB纹理转换
        if (interleavedUV) {
            yuvConverter->setInterleavedUV(true, uvSwap);
        } else {
            yuvConverter->setInterleavedUV(false, false);
        }
        yuvConverter->setLimitedRange(true);
        bool use709 = (width >= 1280 || height >= 720);
        yuvConverter->setBT709(use709);
        yuvConverter->setSRGBEncode(false);
        GLuint uTexForConvert = interleavedUV ? cachedUVTextureRG : cachedUTexture;
        GLuint vTexForConvert = interleavedUV ? 0 : cachedVTexture;
        if (!yuvConverter->convertToTexture(cachedYTexture, uTexForConvert, vTexForConvert, target_rgb_texture,camera->isFront)) {
            LOGE("[GL] YUV到RGB纹理转换失败，使用UMat回退");

            // UMat回退方案
            //            cv::UMat &target_umat = camera->isFront ?
            //                g_capture_manager->getLeftFrameBufferUMat() :
            //                g_capture_manager->getRightFrameBufferUMat();
            //
            //            if (!yuvConverter->convert(cachedYTexture, cachedUTexture, cachedVTexture, target_umat)) {
            //                LOGE("[GL] YUV到RGB UMat转换也失败，使用CPU回退");
            //
            //                // CPU回退方案 - 使用YUV420转换而不是NV21
            //                uint8_t* target_rgb_buffer = camera->isFront ?
            //                    g_capture_manager->getLeftFrameBuffer() :
            //                    g_capture_manager->getRightFrameBuffer();
            //
            //                yuv420ToRgb(yPlane.data, yPlane.rowStride,
            //                           uPlane.data, uPlane.rowStride,
            //                           vPlane.data, vPlane.rowStride,
            //                           width, height, target_rgb_buffer, g_capture_manager->get_brightness());
            //            }
        } else {
            g_capture_manager->setNewCameraFrame();
            // 转换成功，标记新帧
        }
#ifdef DEBUG_TEXTURE_TO_MAT
        cv::Mat taget_debug_mat = g_capture_manager->debugTextureToMat(target_rgb_texture, CAM_SIZE_W, CAM_SIZE_H, "摄像头纹理");
#endif
        //        cv::Mat target_mat;
        //        target_umat.copyTo(target_mat);

        // 注意：不再删除纹理，因为它们被缓存重用

        // YUV转换结束时间和统计
        auto yuv_end_time = std::chrono::high_resolution_clock::now();
        auto yuv_duration = std::chrono::duration_cast<std::chrono::microseconds>(yuv_end_time - yuv_start_time);
        //g_yuvConversionStats.recordFrame(yuv_duration.count());

        // 整体帧处理结束时间和统计
        auto overall_end_time = std::chrono::high_resolution_clock::now();
        auto overall_duration = std::chrono::duration_cast<std::chrono::microseconds>(overall_end_time - overall_start_time);
        //g_overallFrameStats.recordFrame(overall_duration.count());

        // 增加每摄像头帧计数用于固定窗口统计
        if (g_capture_manager) {
            g_capture_manager->record_camera_frame(camera->isFront);
            // 记录RGB整体转换耗时（使用overall_duration）
            g_capture_manager->record_rgb_conversion(static_cast<long long>(overall_duration.count()));
        }

        // 性能统计（保留原有的定期日志）
        int perfCount = performance_counter.fetch_add(1);
        if (perfCount % 120 == 0) {
            LOGD_GL("[GL] YUV转RGB 帧#%d YUV:%lldμs 总:%lldμs",
                    currentFrame, yuv_duration.count(), overall_duration.count());
        }

    } catch (const std::exception& e) {
        LOGE("[GL] YUV处理异常: %s", e.what());
    }
    // 释放图像，避免队列积压
    if (image) {
        AImage_delete(image);
        image = nullptr;
    }
    camera->activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
}
#endif

void nv21ToBgr(uint8_t *yData, int yStride,
               uint8_t *uvData, int uvStride,
               int width, int height,
               uint8_t *bgrBuffer, float brightness) {
    /* 检查所有输入指针是否为空 */
    if (!yData) {
        LOGE("nv21ToBgr: 输入指针yData不能为空！");
        return;
    }
    if (!uvData) {
        LOGE("nv21ToBgr: 输入指针uvData不能为空！");
        return;
    }
    if (!bgrBuffer) {
        LOGE("nv21ToBgr: 输入指针bgrBuffer不能为空！");
        return;
    }
    /* 检查宽高是否合法 */
    if (width <= 0 || height <= 0) {
        LOGE("nv21ToBgr: 宽高必须为正数(width=%d, height=%d)", width, height);
        return;
    }

    brightness = 1.0f;
    // 计算UV平面的尺寸（YUV420中UV分辨率是Y的1/2）
    const int uvWidth = width / 2;
    const int uvHeight = height / 2;
    // 转换系数（基于标准YUV到RGB公式的整数优化）
    const int32_t COEF_V_R = 1436;// 1.402 * 1024
    const int32_t COEF_U_B = 1814;// 1.772 * 1024
    const int32_t COEF_U_G = 352; // 0.34414 * 1024
    const int32_t COEF_V_G = 731; // 0.71414 * 1024

    // 遍历每个像素
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            // 获取Y分量
            const int32_t Y = yData[y * yStride + x];

            // 计算UV索引（NV21是V在前、U在后的交织存储）
            const int uvX = x / 2;
            const int uvY = y / 2;
            const int uvIndex = uvY * uvStride + uvX * 2;// 每个UV对占2字节

            // 提取V和U分量（NV21格式：V在前，U在后）
            const int32_t V = uvData[uvIndex] - 128;
            const int32_t U = uvData[uvIndex + 1] - 128;

            // 计算RGB值（整数运算避免浮点损耗）
            int32_t R = Y + ((COEF_V_R * V) >> 10);
            int32_t G = Y - ((COEF_U_G * U + COEF_V_G * V) >> 10);
            int32_t B = Y + ((COEF_U_B * U) >> 10);

            // 应用亮度调整
            R = (int32_t)(R * brightness);
            G = (int32_t)(G * brightness);
            B = (int32_t)(B * brightness);

            // 应用亮度调整
            R = static_cast<int32_t>(R * brightness);
            G = static_cast<int32_t>(G * brightness);
            B = static_cast<int32_t>(B * brightness);

            // 限制RGB值在0~255范围内
            R = std::max(0, std::min(255, R));
            G = std::max(0, std::min(255, G));
            B = std::max(0, std::min(255, B));

            // 存储到BGR缓冲区（BGR格式，每个像素3字节）
            const int bgrIndex = (y * width + x) * 3;
            bgrBuffer[bgrIndex] = static_cast<uint8_t>(B);     // B
            bgrBuffer[bgrIndex + 1] = static_cast<uint8_t>(G); // G
            bgrBuffer[bgrIndex + 2] = static_cast<uint8_t>(R); // R
        }
    }
}

void yuv420ToBgr(uint8_t *yData, int yStride, uint8_t *uData, int uStride, uint8_t *vData, int vStride,
                 int width, int height, uint8_t *bgrBuffer, float brightness) {
    /* 检查所有输入指针是否为空 */
    if (!yData || !uData || !vData || !bgrBuffer) {
        LOGE("yuv420ToBgr: 输入指针不能为空！");
        return;
    }
    /* 检查宽高是否合法 */
    if (width <= 0 || height <= 0) {
        LOGE("yuv420ToBgr: 宽高必须为正数（width=%d, height=%d）", width, height);
        return;
    }
    brightness = 1.0f;
    // YUV420格式特点：U和V平面分辨率为Y的1/2（宽和高均为1/2）
    const int uvWidth = width / 2;
    const int uvHeight = height / 2;

    // YUV转RGB系数（整数优化）- 考虑TV范围到PC范围的转换
    const int32_t COEF_V_R = 1436;// 1.402 * 1024
    const int32_t COEF_U_B = 1814;// 1.772 * 1024
    const int32_t COEF_U_G = 352; // 0.34414 * 1024
    const int32_t COEF_V_G = 731; // 0.71414 * 1024

    // 遍历每个像素
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            // 获取Y分量（使用yStride处理内存对齐）
            int32_t Y = yData[y * yStride + x];

            // 计算UV索引（2x2像素共享一组UV）
            const int uvX = x / 2;
            const int uvY = y / 2;

            // 检查UV索引是否越界
            if (uvX >= uvWidth || uvY >= uvHeight) {
                LOGE("UV索引越界(uvX=%d, uvY=%d, uvWidth=%d, uvHeight=%d)",
                     uvX, uvY, uvWidth, uvHeight);
                continue;
            }

            int32_t U, V;

            // 检查是否为交错存储（pixelStride=2）
            // 如果uData和vData指向同一内存区域或相邻区域，说明是交错存储
            if (abs(uData - vData) <= 1) {
                // 交错存储格式（NV21/NV12）：UV数据在同一平面中交错存储
                // pixelStride=2意味着每隔一个字节读取一个分量
                const int uvBaseIndex = uvY * uStride + uvX * 2;

                // 根据U和V数据的相对位置确定读取顺序
                if (uData < vData) {
                    // NV12格式：UVUV...
                    U = uData[uvBaseIndex];
                    V = uData[uvBaseIndex + 1];
                } else {
                    // NV21格式：VUVU...
                    V = uData[uvBaseIndex];
                    U = uData[uvBaseIndex + 1];
                }
            } else {
                // 分离存储格式：U和V在不同的平面中
                const int uIndex = uvY * uStride + uvX;
                const int vIndex = uvY * vStride + uvX;
                U = uData[uIndex];
                V = vData[vIndex];
            }

            // TV范围到PC范围的转换
            Y = std::max(0, std::min(255, (Y - 16) * 255 / 219));
            U = std::max(0, std::min(255, (U - 16) * 255 / 224)) - 128;
            V = std::max(0, std::min(255, (V - 16) * 255 / 224)) - 128;

            // 计算RGB值（整数运算避免浮点损耗）
            int32_t R = Y + ((COEF_V_R * V) >> 10);
            int32_t G = Y - ((COEF_U_G * U + COEF_V_G * V) >> 10);
            int32_t B = Y + ((COEF_U_B * U) >> 10);

            // 应用亮度调整
            R = (int32_t)(R * brightness);
            G = (int32_t)(G * brightness);
            B = (int32_t)(B * brightness);

            // 限制RGB值在[0, 255]范围
            R = std::max(0, std::min(255, R));
            G = std::max(0, std::min(255, G));
            B = std::max(0, std::min(255, B));

            // 写入BGR缓冲区（BGR格式，每个像素3字节）
            const int bgrIndex = (y * width + x) * 3;
            bgrBuffer[bgrIndex] = static_cast<uint8_t>(B);     // B
            bgrBuffer[bgrIndex + 1] = static_cast<uint8_t>(G); // G
            bgrBuffer[bgrIndex + 2] = static_cast<uint8_t>(R); // R
        }
    }
}

//void yuv420ToRgb(uint8_t* yData, uint8_t* uData, uint8_t* vData, int width, int height, uint8_t* rgbBuffer) {
/*
void yuv420ToRgb(uint8_t* yData, int yStride, uint8_t* uData, int uStride, uint8_t* vData, int vStride,
                 int width, int height, uint8_t* rgbBuffer) {
    if (!yData || !uData || !vData || !rgbBuffer) {
        LOGE("yuv420ToRgb: 输入指针不能为空！");
        return;
    }
    if (width <= 0 || height <= 0) {
        LOGE("yuv420ToRgb: 宽高必须为正数（width=%d, height=%d）", width, height);
        return;
    }

    // 计算UV平面的尺寸（YUV420中UV分辨率是Y的1/2）
    const int uvWidth = width / 2;
    const int uvHeight = height / 2;
    // 预计算YUV到RGB的转换系数（使用整数运算避免浮点性能损耗）
    // 系数基于标准公式：
    // R = Y + 1.402*(V-128)
    // G = Y - 0.34414*(U-128) - 0.71414*(V-128)
    // B = Y + 1.772*(U-128)
    const int32_t COEF_V_R = 1436;  // 1.402 * 1024
    const int32_t COEF_U_B = 1814;  // 1.772 * 1024
    const int32_t COEF_U_G = 352;   // 0.34414 * 1024
    const int32_t COEF_V_G = 731;   // 0.71414 * 1024
    // 遍历每个像素
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            // 获取Y分量（每个像素一个Y值）
            // const int32_t Y = yData[y * width + x];
            // // 计算UV索引（2x2像素共享一组UV值）
            // const int uvX = x / 2;
            // const int uvY = y / 2;
            // const int32_t U = uData[uvY * uvWidth + uvX] - 128;
            // const int32_t V = vData[uvY * uvWidth + uvX] - 128;
            const int32_t Y = yData[y * yStride + x];
            // UV索引计算
            const int uvX = x / 2;
            const int uvY = y / 2;
            // 按U/V步长访问（而非uvWidth）
            const int32_t U = uData[uvY * uStride + uvX] - 128;
            const int32_t V = vData[uvY * vStride + uvX] - 128;
            // 计算RGB值（使用整数运算并右移10位模拟除法）
            int32_t R = Y + ((COEF_V_R * V) >> 10);
            int32_t G = Y - ((COEF_U_G * U + COEF_V_G * V) >> 10);
            int32_t B = Y + ((COEF_U_B * U) >> 10);
            //  clamp值到0-255范围
            R = std::max(0, std::min(255, R));
            G = std::max(0, std::min(255, G));
            B = std::max(0, std::min(255, B));
            // 存储到RGB缓冲区（RGB格式，每个像素3字节）
            const int rgbIndex = (y * width + x) * 3;
            rgbBuffer[rgbIndex] = static_cast<uint8_t>(R);
            rgbBuffer[rgbIndex + 1] = static_cast<uint8_t>(G);
            rgbBuffer[rgbIndex + 2] = static_cast<uint8_t>(B);
        }
    }
}

*/

static std::mutex g_camera_op_mutex; // 保护相机开启和关闭操作的互斥锁

void openCamera(int32_t width, int32_t height){
    std::lock_guard<std::mutex> lock(g_camera_op_mutex);
    LOGI("[Camera] 正在开启摄像头系统...");
    backCamera.isFront = false;
    frontCamera.isFront = true;

    // 重置首帧信号
    backCamera.firstFrameArrived.store(false);
    frontCamera.firstFrameArrived.store(false);

    // 1. 直接在当前（主）线程初始化后置摄像头
    // initSingleCamera 内部是非阻塞的，很快就会返回
    initSingleCamera(backCamera, width, height);

    // 2. 阻塞等待，直到后台回调函数 onYUVFrameContext 收到真正的第一帧YUV数据
    {
        auto wait_start = std::chrono::high_resolution_clock::now();
        
        std::unique_lock<std::mutex> lk(backCamera.firstFrameMutex);
        // 把超时时间加到 1500ms 比较保险，部分设备第一帧比较慢
        bool arrived = backCamera.firstFrameCv.wait_for(lk, std::chrono::milliseconds(1000),
            [] { return backCamera.firstFrameArrived.load(); });
            
        auto wait_end = std::chrono::high_resolution_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wait_end - wait_start).count();
            
        if (arrived) {
            LOGI("[Camera] 后置摄像头首帧已稳定到达，等待耗时: %lld ms，现在开启前置摄像头", (long long)elapsed_ms);
        } else {
            LOGW("[Camera] 等待后置首帧超时(1000ms)，实际阻塞时间: %lld ms，强制开启前置摄像头", (long long)elapsed_ms);
        }
    }

    // 3. 后置已稳定，直接在当前线程开启前置
    initSingleCamera(frontCamera, width, height);
    LOGI("[Camera] 摄像头系统已全部开启");
}

void closeCamera() {
    std::lock_guard<std::mutex> lock(g_camera_op_mutex);
    LOGI("[Camera] 正在关闭摄像头系统...");
    // 恢复顺序释放：避免多个线程并发调用驱动导致 HAL 层崩溃
    releaseCamera(backCamera);
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 给硬件一点点喘息余地
    releaseCamera(frontCamera);
    LOGI("[Camera] 摄像头系统已完全关闭");
}
/* 相机错误回调 */
void onCameraError(void *context, ACameraDevice *device, int error) {
    if (!context) return;
    CameraDevice *camera = static_cast<CameraDevice *>(context);
    if(error != 4){
        releaseCamera(*camera);
    }
    LOGE("CameraActivity_ %s摄像头错误: %d", camera->isFront ? "前置" : "后置", error);
    //closeCamera();
}

void setColorTemperature(ACaptureRequest* request, float warmth) {
    // 限制范围 [-1, 1]
    if (warmth < -1.0f) warmth = -1.0f;
    if (warmth >  1.0f) warmth =  1.0f;

    ACameraMetadata_rational gains[4];

    if (warmth > 0) {
        // 变暖：提高 R，降低 B
        float r = 1.0f + 0.5f * warmth;  // 1.0 ~ 1.5
        float b = 1.0f - 0.5f * warmth;  // 1.0 ~ 0.5

        gains[0] = {(int)(r * 1000), 1000};
        gains[1] = {1, 1};
        gains[2] = {1, 1};
        gains[3] = {(int)(b * 1000), 1000};
    } else {
        // 变冷：降低 R，提高 B
        float r = 1.0f + 0.5f * warmth;  // 1.0 ~ 0.5
        float b = 1.0f - 0.5f * warmth;  // 1.0 ~ 1.5

        gains[0] = {(int)(r * 1000), 1000};
        gains[1] = {1, 1};
        gains[2] = {1, 1};
        gains[3] = {(int)(b * 1000), 1000};
    }

    uint8_t mode = ACAMERA_COLOR_CORRECTION_MODE_TRANSFORM_MATRIX;

    ACaptureRequest_setEntry_u8(
            request,
            ACAMERA_COLOR_CORRECTION_MODE,
            1,
            &mode
    );

    ACaptureRequest_setEntry_rational(
            request,
            ACAMERA_COLOR_CORRECTION_GAINS,
            4,
            gains
    );
}
/* 创建捕获会话 */
void createCaptureSession(CameraDevice &camera) {
    if (camera.device == nullptr || camera.imageReader == nullptr) {
        LOGE("%s摄像头设备未就绪", camera.isFront ? "前置" : "后置");
        return;
    }


    if (AImageReader_getWindow(camera.imageReader, &camera.window) != AMEDIA_OK || camera.window == nullptr) {
        LOGE("%s摄像头获取图像窗口失败", camera.isFront ? "前置" : "后置");
        return;
    }
    // 增加计数以确保在 releaseCamera 中释放时合法
    ANativeWindow_acquire(camera.window);

    if (ACameraOutputTarget_create(camera.window, &camera.outputTarget) != ACAMERA_OK || camera.outputTarget == nullptr) {
        LOGE("%s摄像头创建输出目标失败", camera.isFront ? "前置" : "后置");
        ANativeWindow_release(camera.window);
        return;
    }

    if (ACameraDevice_createCaptureRequest(camera.device, TEMPLATE_RECORD, &camera.request) != ACAMERA_OK) {
        LOGE("%s摄像头创建预览请求失败", camera.isFront ? "前置" : "后置");
        ACameraOutputTarget_free(camera.outputTarget);
        ANativeWindow_release(camera.window);
        return;
    }

    // 设置预览/录像意图与 3A 模式，目标帧率范围等，提升帧率与稳定性
    {
        uint8_t capture_intent = ACAMERA_CONTROL_CAPTURE_INTENT_PREVIEW;
        if (ACaptureRequest_setEntry_u8(camera.request, ACAMERA_CONTROL_CAPTURE_INTENT, 1, &capture_intent) != ACAMERA_OK) {
            LOGW("%s摄像头设置预览意图失败", camera.isFront ? "前置" : "后置");
        }

        uint8_t control_mode = ACAMERA_CONTROL_MODE_AUTO;
        ACaptureRequest_setEntry_u8(camera.request, ACAMERA_CONTROL_MODE, 1, &control_mode);

        // 3A 基本设置
        uint8_t ae_mode = ACAMERA_CONTROL_AE_MODE_ON;
        if (ACaptureRequest_setEntry_u8(camera.request, ACAMERA_CONTROL_AE_MODE, 1, &ae_mode) != ACAMERA_OK) {
            LOGW("%s摄像头设置AE模式失败", camera.isFront ? "前置" : "后置");
        }

        uint8_t af_mode = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
        if (ACaptureRequest_setEntry_u8(camera.request, ACAMERA_CONTROL_AF_MODE, 1, &af_mode) != ACAMERA_OK) {
            LOGW("%s摄像头设置AF模式失败", camera.isFront ? "前置" : "后置");
        }

        uint8_t awb_mode = ACAMERA_CONTROL_AWB_MODE_AUTO;
        if (ACaptureRequest_setEntry_u8(camera.request, ACAMERA_CONTROL_AWB_MODE, 1, &awb_mode) != ACAMERA_OK) {
            LOGW("%s摄像头设置AWB模式失败", camera.isFront ? "前置" : "后置");
        }

        // 关闭视频防抖以减少处理延迟（设备可能硬件加速，按需调整）
        uint8_t vs_mode = ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
        ACaptureRequest_setEntry_u8(camera.request, ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE, 1, &vs_mode);

        // 关闭降噪与锐化，降低ISP开销
        uint8_t nr_mode = ACAMERA_NOISE_REDUCTION_MODE_FAST;
        if (ACaptureRequest_setEntry_u8(camera.request, ACAMERA_NOISE_REDUCTION_MODE, 1, &nr_mode) != ACAMERA_OK) {
            LOGW("%s摄像头关闭降噪失败，可能不支持", camera.isFront ? "前置" : "后置");
        }
        uint8_t edge_mode = ACAMERA_EDGE_MODE_FAST;
        if (ACaptureRequest_setEntry_u8(camera.request, ACAMERA_EDGE_MODE, 1, &edge_mode) != ACAMERA_OK) {
            LOGW("%s摄像头关闭锐化失败，可能不支持", camera.isFront ? "前置" : "后置");
        }

//        int64_t exposure = 50000000;
//        ACaptureRequest_setEntry_i64(camera.request, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &exposure);
//        int32_t iso = 200; // 适当增加 ISO 值
//        ACaptureRequest_setEntry_i32(camera.request, ACAMERA_SENSOR_SENSITIVITY, 1, &iso);



        uint8_t tonemapMode = ACAMERA_TONEMAP_MODE_FAST;
        ACaptureRequest_setEntry_u8(camera.request, ACAMERA_TONEMAP_MODE, 1, &tonemapMode);

        uint8_t aeLock = ACAMERA_CONTROL_AE_LOCK_OFF;
        ACaptureRequest_setEntry_u8(camera.request, ACAMERA_CONTROL_AE_LOCK, 1, &aeLock);

        // 开启防频闪为 AUTO，便于在 50/60Hz 环境下稳定到 30fps
        uint8_t antibanding_mode = ACAMERA_CONTROL_AE_ANTIBANDING_MODE_AUTO;
        if (ACaptureRequest_setEntry_u8(camera.request, ACAMERA_CONTROL_AE_ANTIBANDING_MODE, 1, &antibanding_mode) != ACAMERA_OK) {
            LOGW("%s摄像头设置防频闪为AUTO失败，可能不支持", camera.isFront ? "前置" : "后置");
        } else {
            LOGI("%s摄像头开启AE防频闪为AUTO", camera.isFront ? "前置" : "后置");
        }

        // 如果设备支持手动传感器，并且目标帧率>=60，尝试使用手动帧时长/曝光以减少AE限制
        if (g_target_fps >= 60 && camera.manager && !camera.cameraId.empty()) {
            ACameraMetadata *chars_caps = nullptr;
            if (ACameraManager_getCameraCharacteristics(camera.manager, camera.cameraId.c_str(), &chars_caps) == ACAMERA_OK && chars_caps) {
                ACameraMetadata_const_entry caps_entry;
                bool has_manual_sensor = false;
                if (ACameraMetadata_getConstEntry(chars_caps, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, &caps_entry) == ACAMERA_OK && caps_entry.type == ACAMERA_TYPE_INT32) {
                    for (uint32_t i = 0; i < caps_entry.count; ++i) {
                        if (caps_entry.data.i32[i] == ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR) {
                            has_manual_sensor = true;
                            break;
                        }
                    }
                }
                if (has_manual_sensor) {
                    // 关闭自动曝光，设置手动帧时长/曝光/增益
                    uint8_t ae_off = ACAMERA_CONTROL_AE_MODE_OFF;
                    if (ACaptureRequest_setEntry_u8(camera.request, ACAMERA_CONTROL_AE_MODE, 1, &ae_off) != ACAMERA_OK) {
                        LOGW("%s摄像头关闭AE失败（手动传感器模式）", camera.isFront ? "前置" : "后置");
                    }
                    // 期望帧时长 = 1 / g_target_fps（ns）
                    int64_t frame_duration_ns = (int64_t)(1000000000LL / std::max(1, g_target_fps));
                    // 曝光时间设置为帧时长的 60%-70%，避免过度曝光并留给读出时间
                    int64_t exposure_ns = (int64_t)(frame_duration_ns * 0.65);
                    // 基础ISO（敏感度），根据设备动态范围可调整
                    int32_t iso = 400;
                    if (ACaptureRequest_setEntry_i64(camera.request, ACAMERA_SENSOR_FRAME_DURATION, 1, &frame_duration_ns) != ACAMERA_OK) {
                        LOGW("%s摄像头设置帧时长失败", camera.isFront ? "前置" : "后置");
                    }
                    if (ACaptureRequest_setEntry_i64(camera.request, ACAMERA_SENSOR_EXPOSURE_TIME, 1, &exposure_ns) != ACAMERA_OK) {
                        LOGW("%s摄像头设置曝光时间失败", camera.isFront ? "前置" : "后置");
                    }
                    if (ACaptureRequest_setEntry_i32(camera.request, ACAMERA_SENSOR_SENSITIVITY, 1, &iso) != ACAMERA_OK) {
                        LOGW("%s摄像头设置ISO失败", camera.isFront ? "前置" : "后置");
                    }
                    LOGI("%s摄像头启用手动传感器: frame=%lldns, exposure=%lldns, ISO=%d", camera.isFront ? "前置" : "后置", (long long)frame_duration_ns, (long long)exposure_ns, iso);
                } else {
                    LOGI("%s摄像头不支持手动传感器能力，维持AE模式", camera.isFront ? "前置" : "后置");
                }
                ACameraMetadata_free(chars_caps);
            }
        }

        // 若设备支持高速视频场景，尝试启用以提高帧率（通常需约束高速会话）
#ifdef ACAMERA_CONTROL_SCENE_MODE_HIGH_SPEED_VIDEO
        if (g_target_fps >= 60 && camera.manager && !camera.cameraId.empty()) {
            ACameraMetadata *chars_scene = nullptr;
            if (ACameraManager_getCameraCharacteristics(camera.manager, camera.cameraId.c_str(), &chars_scene) == ACAMERA_OK && chars_scene) {
                ACameraMetadata_const_entry scene_entry;
                bool has_hsv = false;
                if (ACameraMetadata_getConstEntry(chars_scene, ACAMERA_CONTROL_AVAILABLE_SCENE_MODES, &scene_entry) == ACAMERA_OK && scene_entry.type == ACAMERA_TYPE_BYTE) {
                    for (uint32_t i = 0; i < scene_entry.count; ++i) {
                        if (scene_entry.data.u8[i] == ACAMERA_CONTROL_SCENE_MODE_HIGH_SPEED_VIDEO) {
                            has_hsv = true;
                            break;
                        }
                    }
                }
                if (has_hsv) {
                    uint8_t control_mode = ACAMERA_CONTROL_MODE_USE_SCENE_MODE;
                    uint8_t scene_mode = ACAMERA_CONTROL_SCENE_MODE_HIGH_SPEED_VIDEO;
                    if (ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_MODE, &control_mode, 1) != ACAMERA_OK) {
                        LOGW("%s摄像头启用场景模式失败", camera.isFront ? "前置" : "后置");
                    }
                    if (ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_SCENE_MODE, &scene_mode, 1) != ACAMERA_OK) {
                        LOGW("%s摄像头设置高速视频场景失败", camera.isFront ? "前置" : "后置");
                    } else {
                        LOGI("%s摄像头启用高速视频场景模式", camera.isFront ? "前置" : "后置");
                    }
                } else {
                    LOGW("%s摄像头不支持高速视频场景模式", camera.isFront ? "前置" : "后置");
                }
                ACameraMetadata_free(chars_scene);
            }
        }
#else
        LOGW("%s摄像头: NDK头文件未定义高速视频场景常量，跳过启用", camera.isFront ? "前置" : "后置");
#endif

        // 查询设备支持的 AE 目标帧率范围，并选择最接近 g_target_fps 的范围
        int32_t chosen_range[2] = { g_target_fps, g_target_fps };
        bool range_set = false;
        if (camera.manager && !camera.cameraId.empty()) {
            ACameraMetadata *chars = nullptr;
            if (ACameraManager_getCameraCharacteristics(camera.manager, camera.cameraId.c_str(), &chars) == ACAMERA_OK && chars) {
                ACameraMetadata_const_entry entry;
                if (ACameraMetadata_getConstEntry(chars, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &entry) == ACAMERA_OK && entry.type == ACAMERA_TYPE_INT32 && entry.count >= 2) {
                    // 若目标为>=60fps，优先选择设备支持的 [30, 60]
                    if (g_target_fps >= 60) {
                        for (uint32_t i = 0; i + 1 < entry.count; i += 2) {
                            int32_t min_fps = entry.data.i32[i];
                            int32_t max_fps = entry.data.i32[i + 1];
                            if (min_fps == 30 && max_fps == 60) {
                                chosen_range[0] = 30;
                                chosen_range[1] = 60;
                                range_set = true;
                                LOGI("%s摄像头优先使用高帧率范围: [30, 60]", camera.isFront ? "前置" : "后置");
                                break;
                            }
                        }
                    }

                    // 若未找到[30,60]，按原有评分策略选择最接近目标的范围
                    if (!range_set) {
                        int best_idx = -1;
                        int best_score = INT_MAX; // 越小越好
                        for (uint32_t i = 0; i + 1 < entry.count; i += 2) {
                            int32_t min_fps = entry.data.i32[i];
                            int32_t max_fps = entry.data.i32[i + 1];
                            // 评分规则：优先包含目标帧率的范围；否则选择max最接近目标的范围
                            int score = 0;
                            if (min_fps <= g_target_fps && g_target_fps <= max_fps) {
                                score = (max_fps - g_target_fps) + (g_target_fps - min_fps); // 越紧凑越好
                            } else {
                                score = std::abs(max_fps - g_target_fps) + 1000; // 不包含目标则加大惩罚
                            }
                            if (score < best_score) {
                                best_score = score;
                                best_idx = (int)i;
                            }
                        }
                        if (best_idx >= 0) {
                            chosen_range[0] = entry.data.i32[best_idx];
                            chosen_range[1] = entry.data.i32[best_idx + 1];
                            range_set = true;
                        }
                    }
                }
                ACameraMetadata_free(chars);
            }
        }
        chosen_range[0] = g_min_request_fps;
        chosen_range[1] = g_max_request_fps;
        camera_status_t fps_status = ACaptureRequest_setEntry_i32(camera.request, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, chosen_range);
        if (fps_status == ACAMERA_OK) {
            LOGI("%s摄像头设置目标FPS范围: [%d, %d]%s", camera.isFront ? "前置" : "后置", chosen_range[0], chosen_range[1], range_set ? " (来自设备支持列表)" : " (直接目标)");
        } else {
            LOGW("%s摄像头设置目标FPS范围失败，可能不受支持", camera.isFront ? "前置" : "后置");
        }
    }

    if (ACaptureRequest_addTarget(camera.request, camera.outputTarget) == ACAMERA_OK) {
        if (ACaptureSessionOutput_create(camera.window, &camera.output) == ACAMERA_OK &&
            ACaptureSessionOutputContainer_create(&camera.container) == ACAMERA_OK &&
            ACaptureSessionOutputContainer_add(camera.container, camera.output) == ACAMERA_OK) {
            ACameraCaptureSession_stateCallbacks callbacks = {
                    .context = &camera,
                    .onClosed = [](void *context, ACameraCaptureSession *) {
                        if (context) {
                            CameraDevice *camera = static_cast<CameraDevice *>(context);
                            // 仅设置关闭标志并通知等待的线程
                            // 所有资源释放由 releaseCamera 统一处理，避免 double-free
                            camera->sessionClosed.store(true);
                            {
                                std::lock_guard<std::mutex> lk(camera->sessionMutex);
                            }
                            camera->sessionCv.notify_all();
                            LOGI("%s摄像头会话onClosed", camera->isFront ? "前置" : "后置");
                        }
                    },
                    .onReady = [](void *context, ACameraCaptureSession *) {
                        CameraDevice *cam = static_cast<CameraDevice *>(context);
                        LOGI("%s摄像头会话onReady", cam->isFront ? "前置" : "后置");
                    },
                    .onActive = [](void *context, ACameraCaptureSession *) {
                        CameraDevice *cam = static_cast<CameraDevice *>(context);
                        LOGI("%s摄像头会话onActive", cam->isFront ? "前置" : "后置");
                    },
            };

            if (ACameraDevice_createCaptureSession(camera.device, camera.container, &callbacks, &camera.session) == ACAMERA_OK) {
                LOGI("%s摄像头会话创建成功，发送捕获请求", camera.isFront ? "前置" : "后置");
                //usleep(1000000);
                //ACameraCaptureSession_capture(camera.session, nullptr, 1, &request, nullptr);
                if (ACameraCaptureSession_setRepeatingRequest(camera.session, nullptr, 1, &camera.request, nullptr) == ACAMERA_OK) {
                    LOGI("%s摄像头预览流启动成功", camera.isFront ? "前置" : "后置");
                } else {
                    LOGE("%s摄像头启动预览流失败", camera.isFront ? "前置" : "后置");
                }
            } else {
                LOGE("%s摄像头创建会话失败", camera.isFront ? "前置" : "后置");
            }
        }
    }
}
/* 初始化单个摄像头 */
void initSingleCamera(CameraDevice &camera, int32_t width, int32_t height) {
    releaseCamera(camera);
    LOGI("初始化%s摄像头 w=%d, h=%d", camera.isFront ? "前置" : "后置", width, height);

    //enumerateCameras();
    camera.manager = ACameraManager_create();
    if (!camera.manager) {
        LOGE("%s摄像头创建管理器失败", camera.isFront ? "前置" : "后置");
        return;
    }

    ACameraIdList *cameraIds = nullptr;
    if (ACameraManager_getCameraIdList(camera.manager, &cameraIds) != ACAMERA_OK ||
        !cameraIds || cameraIds->numCameras == 0) {
        LOGE("%s摄像头获取列表失败", camera.isFront ? "前置" : "后置");
        releaseCamera(camera);
        return;
    }

    /* 选择对应的摄像头*/
    for (int i = 0; i < cameraIds->numCameras; i++) {
        ACameraMetadata *metadata = nullptr;
        if (ACameraManager_getCameraCharacteristics(camera.manager, cameraIds->cameraIds[i], &metadata) == ACAMERA_OK) {
            ACameraMetadata_const_entry entry;
            if (ACameraMetadata_getConstEntry(metadata, ACAMERA_LENS_FACING, &entry) == ACAMERA_OK) {
                // ACAMERA_LENS_FACING_BACK = 0, ACAMERA_LENS_FACING_FRONT = 1
                if ((!camera.isFront && entry.data.i32[0] == 0) ||
                    (camera.isFront && entry.data.i32[0] == 1)) {
                    camera.cameraId = cameraIds->cameraIds[i];
                    LOGD("%s摄像头id = %s", camera.isFront ? "前置" : "后置", camera.cameraId.c_str());
                }
            }
            ACameraMetadata_free(metadata);
        }
        if (!camera.cameraId.empty()) break;
    }
    if (camera.cameraId.empty()) {
        LOGE("未找到%s摄像头", camera.isFront ? "前置" : "后置");
        ACameraManager_deleteCameraIdList(cameraIds);
        releaseCamera(camera);
        return;
    }
    // 强制使用 YUV_420_888 格式，禁用 PRIVATE/OES 零拷贝路径
    uint64_t usage_yuv = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN 
                         | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE
                         | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    // 零拷贝模式下 AImage 生命周期更长（在处理队列中等待 + 处理中），需要更多缓冲区
    // 至少需要：1(相机产出) + 1(回调获取) + 2(队列缓存) + 1(处理中) = 5
#if USE_SEPARATE_YUV_THREAD == 2
    int max_images_yuv = 6;  // 零拷贝模式需要更多缓冲区
#else
    int max_images_yuv = 4;  // 同步/拷贝模式用较少缓冲区
#endif

    //int status = AImageReader_new(width, height, AIMAGE_FORMAT_YUV_420_888, max_images_yuv, &camera.imageReader);

    int status = AImageReader_newWithUsage(width,height,AIMAGE_FORMAT_YUV_420_888,AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,max_images_yuv,  &camera.imageReader);
    //int status = AImageReader_newWithUsage(width, height, AIMAGE_FORMAT_YUV_420_888, usage_yuv, max_images_yuv, &camera.imageReader);
    if(status != AMEDIA_OK || !camera.imageReader) {
        LOGE("[image]%s摄像头创建 YUV_420_888 图像阅读器失败", camera.isFront ? "前置" : "后置");
        ACameraManager_deleteCameraIdList(cameraIds);
        releaseCamera(camera);
        return;
    }
    /* 启动 YUV 处理线程（如果启用独立线程模式） */
#if USE_SEPARATE_YUV_THREAD > 0
    if (!camera.yuvProcessor) {
        camera.yuvProcessor = std::make_shared<YUVProcessingThread>(camera.isFront);
    }
    if (!camera.yuvProcessor->isRunning()) {
        if (!camera.yuvProcessor->start()) {
            LOGE("[YUVThread] %s摄像头处理线程启动失败", camera.isFront ? "前置" : "后置");
        } else {
            LOGI("[YUVThread] %s摄像头处理线程启动成功", camera.isFront ? "前置" : "后置");
        }
    }
#endif
    
    /* 设置图像监听器 */
    AImageReader_ImageListener imageListener;
    imageListener.context = &camera;
    imageListener.onImageAvailable = onYUVFrameAvailable;
    AImageReader_setImageListener(camera.imageReader, &imageListener);
    /* 打开相机 */
    ACameraDevice_StateCallbacks deviceCallbacks = {
            .context = &camera,
            .onDisconnected = [](void *context, ACameraDevice *) {
                if (context) {
                    CameraDevice * cam = static_cast<CameraDevice *>(context);
                    LOGI("%s摄像头已断开连接", cam->isFront ? "前置" : "后置");
                }
            },
            .onError = onCameraError};

    if (ACameraManager_openCamera(camera.manager, camera.cameraId.c_str(), &deviceCallbacks, &camera.device) != ACAMERA_OK || !camera.device) {
        LOGE("%s摄像头打开失败", camera.isFront ? "前置" : "后置");
        ACameraManager_deleteCameraIdList(cameraIds);
        releaseCamera(camera);
        return;
    }
    LOGI("%s摄像头初始化完成", camera.isFront ? "前置" : "后置");
    ACameraManager_deleteCameraIdList(cameraIds);
    camera.sessionClosed.store(false);
    camera.isCapturing.store(true, std::memory_order_release);
    createCaptureSession(camera);
}

void initDualCameras(int32_t width, int32_t height) {
    backCamera.isFront = false;
    frontCamera.isFront = true;
    /*
    // 启动后置摄像头
    std::thread([&]{
        width = 1920;
        height = 1080;
        initSingleCamera(backCamera, width, height);
    }).detach();
    // 延迟启动前置摄像头，避免资源竞争
    std::thread([&]{
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        width = 1920;
        height = 1080;
        initSingleCamera(frontCamera, width, height);
    }).detach();
*/
    // 在initDualCameras中
    std::thread back_thread([&] { initSingleCamera(backCamera, width, height); });
    std::thread front_thread([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        initSingleCamera(frontCamera, width, height);
    });
    back_thread.join(); // 等待后置摄像头初始化完成
    front_thread.join();// 等待前置摄像头初始化完成
}

void wb_capture_stop() {
    try {
        if (g_capture_manager) {
            g_capture_manager->stop();
        }
    } catch (const std::exception &e) {
        LOGI("[Capture] 停止异常: ");
    }
}

bool wb_capture_is_running() {
    if (g_capture_manager) {
        return g_capture_manager->is_running();
    }
    return false;
}

/**
 * 基于PBO的异步纹理读取函数（优化版本）
 * @param texture_id 纹理ID
 * @param width 图像宽度
 * @param height 图像高度
 * @param output_mat 输出Mat
 * @return 是否成功
 */
bool convert_panorama_to_mat_async_pbo(GLuint texture_id, int width, int height, cv::Mat& output_mat) {
    auto start_time = std::chrono::high_resolution_clock::now();

    if (texture_id == 0) {
        LOGD_PBO( "无效的纹理ID");
        return false;
    }

    // 检查OpenGL上下文
    EGLContext current_context = eglGetCurrentContext();
    if (current_context == EGL_NO_CONTEXT) {
        LOGD_PBO( "没有当前的OpenGL上下文");
        return false;
    }

    // 清除之前的OpenGL错误
    while (glGetError() != GL_NO_ERROR) {
        // 清除错误队列
    }

    // 初始化PBO（如果需要）
    if (!init_async_pbo(width, height)) {
        LOGD_PBO("PBO初始化失败");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_pbo_mutex);

    // 保存当前帧缓冲区绑定
    GLint current_framebuffer;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_framebuffer);

    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        LOGD_PBO( "获取当前帧缓冲区绑定失败，错误: 0x%x", error);
        return false;
    }

    // 重用帧缓冲区对象
    if (g_reusable_framebuffer == 0) {
        glGenFramebuffers(1, &g_reusable_framebuffer);
        error = glGetError();
        if (error != GL_NO_ERROR || g_reusable_framebuffer == 0) {
            LOGD_PBO( "创建帧缓冲区失败，错误: 0x%x", error);
            return false;
        }
        LOGD_PBO( "创建可重用帧缓冲区，ID: %u", g_reusable_framebuffer);
    }

    // 绑定帧缓冲区并附加纹理
    glBindFramebuffer(GL_FRAMEBUFFER, g_reusable_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_id, 0);

    // 检查帧缓冲区完整性
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGD_PBO( "帧缓冲区不完整，状态: 0x%x", status);
        glBindFramebuffer(GL_FRAMEBUFFER, current_framebuffer);
        return false;
    }

    glViewport(0, 0, width, height);

    // PBO双缓冲异步读取逻辑
    int current_pbo = g_pbo_index;
    int next_pbo = 1 - g_pbo_index;

    LOGD_PBO("使用PBO[%d]读取，PBO[%d]处理上一帧", current_pbo, next_pbo);

    // 绑定当前PBO并启动异步读取
    glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo_pool[current_pbo]);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, nullptr); // 异步读取到PBO

    error = glGetError();
    if (error != GL_NO_ERROR) {
        LOGD_PBO("异步读取像素数据失败，错误: 0x%x", error);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, current_framebuffer);
        return false;
    }

    // 如果不是第一帧，从另一个PBO读取上一帧的数据
    if (!g_pbo_first_frame) {
        // 绑定另一个PBO并映射内存
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo_pool[next_pbo]);

        // 映射PBO内存进行读取
        void* pbo_data = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, width * height * 3, GL_MAP_READ_BIT);

        error = glGetError();
        if (error != GL_NO_ERROR || pbo_data == nullptr) {
            LOGD_PBO( "映射PBO内存失败，错误: 0x%x", error);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, current_framebuffer);
            return false;
        }

        // 直接从映射的内存创建Mat，避免额外拷贝
        cv::Mat temp_mat(height, width, CV_8UC3, pbo_data);
        output_mat = temp_mat;
//        cv::flip(temp_mat, output_mat, 0); // 翻转图像

        // 解除内存映射
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);

        error = glGetError();
        if (error != GL_NO_ERROR) {
            LOGD_PBO( "解除PBO内存映射失败，错误: 0x%x", error);
        }

        LOGD_PBO( "成功从PBO[%d]读取上一帧数据", next_pbo);
    } else {
        // 第一帧：使用同步方式读取当前PBO
        LOGD_PBO("第一帧使用同步读取");

        void* pbo_data = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, width * height * 3, GL_MAP_READ_BIT);

        error = glGetError();
        if (error != GL_NO_ERROR || pbo_data == nullptr) {
            LOGD_PBO("第一帧映射PBO内存失败，错误: 0x%x", error);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, current_framebuffer);
            return false;
        }

        cv::Mat temp_mat(height, width, CV_8UC3, pbo_data);
        cv::flip(temp_mat, output_mat, 0);

        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        g_pbo_first_frame = false;
    }

    // 切换PBO索引
    g_pbo_index = next_pbo;

    // 恢复OpenGL状态
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, current_framebuffer);

    // 性能统计
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double, std::milli>(end_time - start_time);
    LOGD_GL("[Performance] PBO异步转换: %.1fms (%dx%d)", duration.count(), width, height);

    return true;
}

/**
 * 原始的纹理转Mat函数（保留作为备用）
 */
bool convert_panorama_to_mat(GLuint texture_id, int width, int height, cv::Mat& output_mat) {
    // 性能测试：记录开始时间
    auto start_time = std::chrono::high_resolution_clock::now();

    if (texture_id == 0) {
        LOGD_STITCHING( "无效的纹理ID");
        return false;
    }

    // 检查OpenGL上下文
    EGLContext current_context = eglGetCurrentContext();
    if (current_context == EGL_NO_CONTEXT) {
        LOGD_STITCHING("没有当前的OpenGL上下文");
        return false;
    }

    // 清除之前的OpenGL错误
    while (glGetError() != GL_NO_ERROR) {
        // 清除错误队列
    }

    // 性能优化：使用线程安全的锁保护共享资源
    std::lock_guard<std::mutex> lock(g_optimization_mutex);

    // 性能优化：重用内存池，避免频繁分配
    size_t required_size = width * height * 3; // RGB格式
    if (g_pixel_buffer_pool.size() != required_size) {
        g_pixel_buffer_pool.resize(required_size);
        LOGD_STITCHING("调整内存池大小到: %zu bytes", required_size);
    }

    // 保存当前帧缓冲区绑定
    GLint current_framebuffer;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_framebuffer);

    // 检查glGetIntegerv是否成功
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        LOGD_STITCHING("获取当前帧缓冲区绑定失败，错误: 0x%x", error);
        return false;
    }

    // 性能优化：重用帧缓冲区对象，避免重复创建/删除
    if (g_reusable_framebuffer == 0) {
        glGenFramebuffers(1, &g_reusable_framebuffer);

        // 检查glGenFramebuffers是否成功
        error = glGetError();
        if (error != GL_NO_ERROR) {
            LOGD_STITCHING("glGenFramebuffers调用失败，错误: 0x%x", error);
            return false;
        }

        if (g_reusable_framebuffer == 0) {
            LOGD_STITCHING("创建可重用帧缓冲区失败，返回的ID为0");
            return false;
        }

        LOGD_STITCHING("成功创建可重用帧缓冲区，ID: %u", g_reusable_framebuffer);
    }

    // 绑定可重用的帧缓冲区
    glBindFramebuffer(GL_FRAMEBUFFER, g_reusable_framebuffer);

    // 将纹理附加到帧缓冲区
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_id, 0);

    // 检查帧缓冲区完整性
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGD_STITCHING( "帧缓冲区不完整，状态: 0x%x", status);
        glBindFramebuffer(GL_FRAMEBUFFER, current_framebuffer);
        return false;
    }

    // 设置视口
    glViewport(0, 0, width, height);

    // 性能优化：移除glFinish()，减少同步等待
    // glFinish(); // 注释掉强制同步等待

    // 读取像素数据到内存池
    LOGD_STITCHING("开始读取纹理像素数据，纹理ID: %d, 尺寸: %dx%d", texture_id, width, height);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, g_pixel_buffer_pool.data());

    // 检查OpenGL错误
    error = glGetError();
    if (error != GL_NO_ERROR) {
        LOGD_STITCHING( "读取像素数据失败，错误: 0x%x", error);
        glBindFramebuffer(GL_FRAMEBUFFER, current_framebuffer);
        return false;
    }

    // 检查读取的像素数据（简化版本，减少循环开销）
    bool all_black = true;
    bool all_white = true;
    int sample_count = std::min(50, (int)g_pixel_buffer_pool.size() / 3); // 减少采样数量
    for (int i = 0; i < sample_count; i++) {
        uint8_t r = g_pixel_buffer_pool[i * 3];
        uint8_t g = g_pixel_buffer_pool[i * 3 + 1];
        uint8_t b = g_pixel_buffer_pool[i * 3 + 2];

        if (r != 0 || g != 0 || b != 0) all_black = false;
        if (r != 255 || g != 255 || b != 255) all_white = false;
    }

    LOGD_STITCHING( "像素数据采样结果 - 全黑: %s, 全白: %s",
                   all_black ? "是" : "否", all_white ? "是" : "否");

    // 恢复原来的帧缓冲区绑定
    glBindFramebuffer(GL_FRAMEBUFFER, current_framebuffer);

    // 性能优化：直接创建翻转的Mat，避免额外的拷贝
    cv::Mat temp_mat(height, width, CV_8UC3, g_pixel_buffer_pool.data());
    cv::flip(temp_mat, output_mat, 0); // 直接翻转到输出Mat

    // 性能测试：计算并输出耗时
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double, std::milli>(end_time - start_time);
    LOGD_GL("[Performance] 纹理转Mat: %.1fms (%dx%d)", duration.count(), width, height);
    return true;
}

bool wb_capture_get_panorama_frame(uint8_t *&buffer, int &width, int &height) {
    if (g_capture_manager) {
        return g_capture_manager->get_latest_panorama_frame(buffer, width, height);
    }
    return false;
}

bool wb_capture_get_panorama_frame_opengl(GLuint panorama_texture_id_, int width, int height, uint8_t *&panprama_buffer)
{
    auto start_time = std::chrono::high_resolution_clock::now();
    if (panorama_texture_id_ == 0) {
        LOGE("[Capture] 无效的纹理ID");
        return false;
    }
    EGLContext current_context = eglGetCurrentContext();
    if (current_context == EGL_NO_CONTEXT) {
        LOGE("[Capture] 当前线程无EGL上下文");
        return false;
    }

    if (!init_async_pbo(width, height)) {
        LOGE("[Capture] 初始化PBO失败");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_pbo_mutex);

    GLint current_framebuffer;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_framebuffer);

    if (g_reusable_framebuffer == 0) {
        glGenFramebuffers(1, &g_reusable_framebuffer);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_reusable_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, panorama_texture_id_, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("[Capture] FBO不完整");
        glBindFramebuffer(GL_FRAMEBUFFER, current_framebuffer);
        return false;
    }

    glViewport(0, 0, width, height);

    int current_pbo = g_pbo_index;
    int next_pbo = 1 - g_pbo_index;

    glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo_pool[current_pbo]);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, nullptr);

    static std::vector<uint8_t> s_readback;
    s_readback.resize(static_cast<size_t>(width) * height * 3);

    if (!g_pbo_first_frame) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo_pool[next_pbo]);
        void* pbo_data = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(s_readback.size()), GL_MAP_READ_BIT);
        if (!pbo_data) {
            LOGE("[Capture] 映射PBO失败");
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, current_framebuffer);
            return false;
        }
        memcpy(s_readback.data(), pbo_data, s_readback.size());
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    } else {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo_pool[current_pbo]);
        void* pbo_data = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(s_readback.size()), GL_MAP_READ_BIT);
        if (!pbo_data) {
            LOGE("[Capture] 第一帧映射PBO失败");
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, current_framebuffer);
            return false;
        }
        memcpy(s_readback.data(), pbo_data, s_readback.size());
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        g_pbo_first_frame = false;
    }

    g_pbo_index = next_pbo;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, current_framebuffer);

    panprama_buffer = s_readback.data();

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration<double, std::milli>(end_time - start_time);
    LOGD_GL("[Performance] 取OpenGL帧(零拷贝给上层指针): %.1fms (%dx%d)", duration.count(), width, height);
    return true;
}

bool wb_capture_get_panorama_frame_mat(cv::UMat &buffer, int &width, int &height) {
    if (g_capture_manager) {
        return g_capture_manager->get_latest_panorama_frame_umat(buffer, width, height);
    }
    return false;
}

void wb_release_opengl_res(){

    if(g_capture_manager){
        g_capture_manager.reset();
        g_capture_manager = nullptr;
    }
}

// 全局设置参数
static int g_image_format = 0;      // 0=JPEG, 1=HEIF
static int g_image_quality = 100;     // 100=高画质, 95=标准, 90=低画质
static int g_video_codec = 0;       // 0=H.264, 1=H.265
static int g_frame_rate = 30;       // 帧率: 24/30/60
static int g_bitrate = 0;           // 码率(Mbps): 0=自动, 8/16/32/64
static int g_noise_reduction = 2;   // 0=关闭, 1=标准, 2=高品质
static bool g_stabilization = false;
static bool g_mic_enabled = true;

bool wb_set_general_param(const char* key, const char* value) {
    int intValue = atoi(value);
    
    if (strcmp(key, "sight_lock") == 0) {
        sight_lock_mode = intValue;  // 0=固定, 1=跟随镜头
    } else if (strcmp(key, "image_format") == 0) {
        g_image_format = intValue;   // 0=JPEG, 1=HEIF
    } else if (strcmp(key, "image_quality") == 0) {
        g_image_quality = intValue;  // 0=高画质, 1=标准, 2=低画质
    } else if (strcmp(key, "video_codec") == 0) {
        g_video_codec = intValue;    // 0=H.264, 1=H.265
    } else if (strcmp(key, "frame_rate") == 0) {
        g_frame_rate = intValue;     // 24/30/60
    } else if (strcmp(key, "bitrate") == 0) {
        g_bitrate = intValue;        // 0=自动, 8/16/32/64 Mbps
    } else if (strcmp(key, "noise_reduction") == 0) {
        g_noise_reduction = intValue; // 0=关闭, 1=标准, 2=高品质
    } else if (strcmp(key, "stabilization") == 0) {
        g_stabilization = (intValue == 1);
    } else if (strcmp(key, "mic_enabled") == 0) {
        g_mic_enabled = (intValue == 1);
    }
    
    LOGI("[Settings] %s = %s (int: %d)", key, value, intValue);
    return true;
}

// 获取设置参数的辅助函数
int wb_get_image_format() { return g_image_format; }
int wb_get_image_quality() { return g_image_quality; }
int wb_get_video_codec() { return g_video_codec; }
int wb_get_frame_rate() { return g_frame_rate; }
int wb_get_bitrate() { return g_bitrate; }
int wb_get_noise_reduction() { return g_noise_reduction; }
bool wb_get_stabilization() { return g_stabilization; }
bool wb_get_mic_enabled() { return g_mic_enabled; }
int wb_get_sight_lock_mode() { return sight_lock_mode; }

bool wb_capture_set_mode(int video_mode) {
    try {
        if (!g_capture_manager) {
            LOGW("[Capture] CaptureManager 未初始化，创建 mode=%d", video_mode);
            g_capture_manager = std::make_unique<CaptureManager>(video_mode);
        }
        return g_capture_manager->set_mode(video_mode);
    } catch (const std::exception &e) {
        LOGI("[Capture] set_mode 异常: ");
        return false;
    }
}

bool wb_capture_get_combined_fps(float &combined_fps) {
    if (g_capture_manager) {
        return g_capture_manager->get_combined_fps(combined_fps);
    }
    return false;
}

bool wb_capture_get_stereo_textures(GLuint &left_texture_id, GLuint &right_texture_id) {
    if (!g_capture_manager) {
        LOGE("[Capture] 取流管理器未初始化");
        return false;
    }

    return g_capture_manager->get_stereo_textures(left_texture_id, right_texture_id);
}

bool wb_capture_get_panorama_texture(GLuint &panorama_texture_id) {
    if (!g_capture_manager) {
        LOGE("[Capture] 取流管理器未初始化");
        return false;
    }

    return g_capture_manager->get_panorama_texture(panorama_texture_id);
}

bool wb_capture_has_new_frame() {
    if (!g_capture_manager) {
        return false;
    }
    return g_capture_manager->hasNewFrame();
}

bool wb_capture_is_producing_frames() {
    if (!g_capture_manager) {
        return false;
    }
    // 检查是否在运行且有新帧数据
    return g_capture_manager->is_running() && g_capture_manager->hasNewFrame();
}

int wb_get_global_fps() {
    if (!g_capture_manager) {
        LOGE("[Capture] 取流管理器未初始化");
        return false;
    }
    return g_capture_manager->get_global_fps();
}

void wb_capture_set_rotation(float rotation) {
    if (!g_capture_manager) {
        LOGE("[Capture] 取流管理器未初始化");
        return;
    }

    g_capture_manager->set_rotation(rotation);
}

float wb_capture_get_rotation() {
    if (!g_capture_manager) {
        LOGE("[Capture] 取流管理器未初始化");
        return 0.0f;
    }

    return g_capture_manager->get_rotation();
}

void wb_capture_set_full_model(bool enable) {
    g_full_model_mode.store(enable);
}

void wb_capture_set_stitching_fps_control(bool enable) {
    if (!g_capture_manager) {
        LOGE("[Capture] 取流管理器未初始化");
        return;
    }

    g_capture_manager->set_stitching_fps_control(enable);
}

bool wb_capture_get_stitching_fps_control() {
    if (!g_capture_manager) {
        LOGE("[Capture] 取流管理器未初始化");
        return false;
    }

    return g_capture_manager->get_stitching_fps_control();
}


bool wb_capture_take_picture(){
    if (!g_capture_manager) {
        LOGE("[Capture] 取流管理器未初始化");
        return false;
    }
    return take_picture();
}

float wb_capture_get_accel_z_rotation() {
    if (!g_capture_manager) {
        LOGE("[IMU] 取流管理器未初始化");
        return 0.0f;
    }
    if (!g_capture_manager->is_running()) {
        LOGW("[IMU] 服务未运行，返回0");
        return 0.0f;
    }
    AccelData accel;
    if (!g_capture_manager->get_latest_accel(accel)) {
        LOGW("[IMU] 无加速度计数据，返回0");
        return 0.0f;
    }
    const float ax = accel.x;
    const float ay = accel.y;
    float yaw_deg = atan2f(ay, ax) * 57.29578f; // 180/pi
    if (yaw_deg < 0.0f) yaw_deg += 360.0f;      // 归一化到 [0, 360)
    return yaw_deg;
}

// ============ 内存泄漏检测接口 ============
void wb_capture_print_memory_stats(const char* tag) {
    g_memoryStats.printStats(tag ? tag : "Unknown");
    
    // 打印 EGL 上下文统计
    auto egl_manager = EGLContextManager::getInstance();
    if (egl_manager) {
        egl_manager->printActiveContexts(tag ? tag : "MemStats");
    }
    
    // 如果有处理线程，打印线程统计信息
#if USE_SEPARATE_YUV_THREAD > 0
    if (backCamera.yuvProcessor) {
        LOGI("[MemStats][Back] 已处理帧: %lld, 丢帧: %lld, 队列: %zu",
             (long long)backCamera.yuvProcessor->getProcessedFrames(),
             (long long)backCamera.yuvProcessor->getDroppedFrames(),
             backCamera.yuvProcessor->getQueueSize());
    }
    if (frontCamera.yuvProcessor) {
        LOGI("[MemStats][Front] 已处理帧: %lld, 丢帧: %lld, 队列: %zu",
             (long long)frontCamera.yuvProcessor->getProcessedFrames(),
             (long long)frontCamera.yuvProcessor->getDroppedFrames(),
             frontCamera.yuvProcessor->getQueueSize());
    }
#endif
}

void wb_capture_reset_memory_stats() {
    g_memoryStats.reset();
    LOGI("[MemStats] 内存统计已重置");
}

// ============ YUV 处理线程启动/停止接口 ============
#if USE_SEPARATE_YUV_THREAD > 0
bool wb_capture_start_yuv_processing_threads() {
    bool success = true;
    
    // 创建并启动后置摄像头 YUV 处理线程
    if (!backCamera.yuvProcessor) {
        backCamera.yuvProcessor = std::make_shared<YUVProcessingThread>(false);
    }
    if (!backCamera.yuvProcessor->isRunning()) {
        if (!backCamera.yuvProcessor->start()) {
            LOGE("[YUVThread] 后置摄像头处理线程启动失败");
            success = false;
        }
    }
    
    // 创建并启动前置摄像头 YUV 处理线程
    if (!frontCamera.yuvProcessor) {
        frontCamera.yuvProcessor = std::make_shared<YUVProcessingThread>(true);
    }
    if (!frontCamera.yuvProcessor->isRunning()) {
        if (!frontCamera.yuvProcessor->start()) {
            LOGE("[YUVThread] 前置摄像头处理线程启动失败");
            success = false;
        }
    }
    
    if (success) {
        LOGI("[YUVThread] 前后摄像头 YUV 处理线程启动成功");
    }
    
    return success;
}

void wb_capture_stop_yuv_processing_threads() {
    // 停止后置摄像头处理线程
    if (backCamera.yuvProcessor) {
        backCamera.yuvProcessor->stop();
        // 打印统计信息
        LOGI("[YUVThread][Back] 停止时统计 - 已处理: %lld, 丢帧: %lld",
             (long long)backCamera.yuvProcessor->getProcessedFrames(),
             (long long)backCamera.yuvProcessor->getDroppedFrames());
    }
    
    // 停止前置摄像头处理线程
    if (frontCamera.yuvProcessor) {
        frontCamera.yuvProcessor->stop();
        LOGI("[YUVThread][Front] 停止时统计 - 已处理: %lld, 丢帧: %lld",
             (long long)frontCamera.yuvProcessor->getProcessedFrames(),
             (long long)frontCamera.yuvProcessor->getDroppedFrames());
    }
    
    // 打印内存泄漏统计
    wb_capture_print_memory_stats("ThreadStop");
    
    LOGI("[YUVThread] 前后摄像头 YUV 处理线程已停止");
}
#else
bool wb_capture_start_yuv_processing_threads() {
    LOGI("[YUVThread] 独立处理线程模式未启用");
    return true;
}

void wb_capture_stop_yuv_processing_threads() {
    LOGI("[YUVThread] 独立处理线程模式未启用");
}
#endif

