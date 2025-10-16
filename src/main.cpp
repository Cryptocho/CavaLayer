#include <cstring>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define namespace ns
#include "layer-shell-client-protocol.h"
#undef namespace
#include "cava-input.hpp"
#include "shaders.hpp"

// RAII包装
struct WlDeleter {
    void operator()(wl_display* d) const { if (d) wl_display_disconnect(d); }
    void operator()(wl_registry* r) const { if (r) wl_registry_destroy(r); }
    void operator()(wl_compositor* c) const { if (c) wl_compositor_destroy(c); }
    void operator()(wl_surface* s) const { if (s) wl_surface_destroy(s); }
    void operator()(wl_shm* s) const { if (s) wl_shm_destroy(s); }
    void operator()(zwlr_layer_shell_v1* ls) const { if (ls) zwlr_layer_shell_v1_destroy(ls); }
    void operator()(zwlr_layer_surface_v1* ls) const { if (ls) zwlr_layer_surface_v1_destroy(ls); }
    void operator()(wl_egl_window* w) const { if (w) wl_egl_window_destroy(w); }
    void operator()(wl_seat* s) const { if (s) wl_seat_destroy(s); }
};

struct ClientState {
    // Wayland 资源
    std::unique_ptr<wl_display, WlDeleter> display;
    std::unique_ptr<wl_registry, WlDeleter> registry;
    std::unique_ptr<wl_compositor, WlDeleter> compositor;
    std::unique_ptr<wl_shm, WlDeleter> shm;
    std::unique_ptr<wl_surface, WlDeleter> surface;
    std::unique_ptr<zwlr_layer_shell_v1, WlDeleter> layer_shell;
    std::unique_ptr<zwlr_layer_surface_v1, WlDeleter> layer_surface;
    std::unique_ptr<wl_egl_window, WlDeleter> egl_window;
    std::unique_ptr<wl_seat, WlDeleter> seat;
    wl_keyboard *keyboard = nullptr;
    // EGL 资源
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    GLuint program = 0;
    GLuint vbo = 0;
    GLuint position_attr = -1;
    GLuint colorTop_uniform = -1;
    GLuint colorBottom_uniform = -1;
    // Cava 资源
    std::vector<float> cava_frame;
    size_t cava_bars = 64;
    const char *bit_format = "16bit";
    size_t ring_capacity = 16; // 环形缓冲区容量
    // 状态管理
    uint32_t configure_serial = 0;
    bool configured = false;
    bool egl_initialized = false;
    int width = 0;
    int height = 0;
    bool running = true;
};

static void keyboard_keymap(void *data, wl_keyboard *keyboard, uint32_t format, int fd, uint32_t size) {
    // 忽略
}

static void keyboard_enter(void *data, wl_keyboard *keyboard, uint32_t serial, wl_surface *surface, wl_array *keys) {
    // 光标进入
}

static void keyboard_leave(void *data, wl_keyboard *keyboard, uint32_t serial, wl_surface *surface) {
    // 光标离开
}

static void keyboard_modifiers(void *data, wl_keyboard *keyboard, uint32_t serial, uint32_t mods_depressed,
                               uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    // 忽略
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    ClientState *client = static_cast<ClientState *>(data);
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        std::cout << "[Key] keycode " << key << " pressed" << std::endl;
    }
    if (key == 1) {
        std::cout << "[Key] Esc pressed, exiting..." << std::endl;
        client->running = false;
    }
}

static const wl_keyboard_listener keyboard_listener = {
    keyboard_keymap,
    keyboard_enter,
    keyboard_leave,
    keyboard_key,
    keyboard_modifiers
};

static void seat_capabilities(void *data, wl_seat *seat, uint32_t caps) {
    ClientState* client = static_cast<ClientState*>(data);
    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        client->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(client->keyboard, &keyboard_listener, client);
    }
}

static const wl_seat_listener seat_listener = {
    seat_capabilities
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
    ClientState *state = (ClientState *)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        uint32_t bind_version = std::min<uint32_t>(version, 4);
        state->compositor.reset(static_cast<wl_compositor*>(
            wl_registry_bind(registry, id, &wl_compositor_interface, bind_version)
        ));
        std::cout << "[Wayland] Bound wl_compositor" << std::endl;
    }
    else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm.reset(static_cast<wl_shm*>(
            wl_registry_bind(registry, id, &wl_shm_interface, 1)
        ));
        std::cout << "[Wayland] Bound wl_shm" << std::endl;
    }
    else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->layer_shell.reset(static_cast<zwlr_layer_shell_v1*>(
            wl_registry_bind(registry, id, &zwlr_layer_shell_v1_interface, 1)
        ));
        std::cout << "[Wayland] Bound zwlr_layer_shell_v1" << std::endl;
    }
    else if (strcmp(interface, wl_seat_interface.name) == 0) {
        state->seat.reset(static_cast<wl_seat*>(
            wl_registry_bind(registry, id, &wl_seat_interface, 1)
        ));
        wl_seat_add_listener(state->seat.get(), &seat_listener, state);
        std::cout << "[Wayland] Bound wl_seat" << std::endl;
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t id) {
    // no-op
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *layer_surface, uint32_t serial, uint32_t width, uint32_t height) {
    ClientState *state = static_cast<ClientState *>(data);
    std::cout << "[Layer-Shell] 收到 configure 事件： 尺寸 " << width << "x" << height << ", 序列号 " << serial << std::endl;
    state->configure_serial = serial;
    state->configured = true;
    state->width = width;
    state->height = height;

    if (!state->egl_initialized) {
        state->egl_window.reset(wl_egl_window_create(state->surface.get(), width, height));
        if (!state->egl_window) {
            std::cerr << "Failed to create EGL window" << std::endl;
            exit(1);
        }
        std::cout << "[EGL] Created EGL window" << std::endl;
    } else {
        wl_egl_window_resize(state->egl_window.get(), width, height, 0, 0);
        std::cout << "[EGL] Resized EGL window to " << width << "x" << height << std::endl;
    }
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *layer_surface) {
    ClientState *state = static_cast<ClientState *>(data);
    std::cout << "[Layer-Shell] 收到 closed 事件, 销毁 layer_surface" << std::endl;
    state->layer_surface.reset();
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint log_len;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
        std::vector<GLchar> log(log_len);
        glGetShaderInfoLog(shader, log_len, nullptr, log.data());
        std::cerr << "Failed to compile shader: " << log.data() << std::endl;
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

bool create_shader_program(ClientState *state) {
    GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
    GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
    
    if (!vertex_shader || !fragment_shader) {
        return false;
    }
    
    state->program = glCreateProgram();
    glAttachShader(state->program, vertex_shader);
    glAttachShader(state->program, fragment_shader);
    glLinkProgram(state->program);
    
    GLint success;
    glGetProgramiv(state->program, GL_LINK_STATUS, &success);
    if (!success) {
        GLchar info_log[512];
        glGetProgramInfoLog(state->program, 512, nullptr, info_log);
        std::cerr << "Shader program linking error: " << info_log << std::endl;
        return false;
    }
    
    state->position_attr = glGetAttribLocation(state->program, "position");
    state->colorTop_uniform = glGetUniformLocation(state->program, "colorTop");
    state->colorBottom_uniform = glGetUniformLocation(state->program, "colorBottom");
    
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    
    return true;
}

bool init_egl(ClientState *state) {
    state->egl_display = eglGetDisplay(state->display.get());
    if (state->egl_display == EGL_NO_DISPLAY) {
        std::cerr << "Failed to get EGL display" << std::endl;
        return false;
    }
    
    EGLint major, minor;
    if (!eglInitialize(state->egl_display, &major, &minor)) {
        std::cerr << "Failed to initialize EGL" << std::endl;
        return false;
    }
    std::cout << "[EGL] Initialized EGL " << major << "." << minor << std::endl;
    
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };

    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(state->egl_display, config_attribs, &config, 1, &num_configs)) {
        std::cerr << "Failed to choose EGL config" << std::endl;
        return false;
    }
    
    state->egl_surface = eglCreateWindowSurface(
        state->egl_display, 
        config, 
        (EGLNativeWindowType)state->egl_window.get(), 
        nullptr
    );
    if (state->egl_surface == EGL_NO_SURFACE) {
        std::cerr << "Failed to create EGL surface: " << eglGetError() << std::endl;
        return false;
    }
    std::cout << "[EGL] Created EGL surface" << std::endl;

    EGLint context_attribs[] = { 
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 2,
        EGL_NONE
    };
    state->egl_context = eglCreateContext(state->egl_display, config, EGL_NO_CONTEXT, context_attribs);
    if (state->egl_context == EGL_NO_CONTEXT) {
        std::cerr << "Failed to create EGL context: " << eglGetError() << std::endl;
        return false;
    }
    std::cout << "[EGL] Created EGL context" << std::endl;

    if (!eglMakeCurrent(
        state->egl_display, 
        state->egl_surface, 
        state->egl_surface, 
        state->egl_context
    )) {
        std::cerr << "Failed to make EGL context current" << std::endl;
        return false;
    }
    std::cout << "[EGL] Made EGL context current successfully" << std::endl;

    if (!create_shader_program(state)) {
        std::cerr << "Failed to create shader program" << std::endl;
        return false;
    }
    glGenBuffers(1, &state->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, state->vbo);
    glEnableVertexAttribArray(state->position_attr);
    glVertexAttribPointer(state->position_attr, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    EGLint swap_interval = 1;
    if (!eglSwapInterval(state->egl_display, swap_interval)) {
        std::cerr << "Failed to set swap interval: " << eglGetError() << std::endl;
        return false;
    }
    std::cout << "[EGL] Set swap interval to " << swap_interval << std::endl;

    const GLubyte* version = glGetString(GL_VERSION);
    std::cout << "[EGL] Running on GLES " << version << std::endl;

    state->egl_initialized = true;
    std::cout << "[EGL] Initialization complete" << std::endl;

    return true;
}

void draw_frame(ClientState *state) {
    if (!state->egl_initialized) {
        std::cerr << "EGL not initialized" << std::endl;
        return;
    }
    if (state->width == 0 || state->height == 0) {
        std::cerr << "Invalid window size: " << state->width << "x" << state->height << std::endl;
        return;
    }

    glViewport(0, 0, state->width, state->height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    int ret = cava_reader_try_pop(state->cava_frame.data(), state->cava_frame.size());
    if (ret >= 0) {
        size_t n = state->cava_frame.size();
        if (n < 2) return;

        // Cardinal spline
        const float tension = 0.5f;
        const size_t points_per_segment = 128;
        std::vector<GLfloat> vertices;
        std::vector<float> control_points(n);

        std::vector<float> x_coords(n);
        for (size_t i = 0; i < n; i++) {
            x_coords[i] = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(n - 1);
            control_points[i] = state->cava_frame[i] * 2.0f - 1.0f;
        }

        std::vector<float> tangents(n);
        for (size_t i = 0; i < n - 1; i++) {
            if (i == 0) {
                tangents[i] = (1.0f - tension) / 2.0f * (control_points[1] - control_points[0]);
            } else if (i == n - 1) {
                tangents[i] = (1.0f - tension) / 2.0f * (control_points[n - 1] - control_points[n - 2]);
            } else {
                tangents[i] = (1.0f - tension) / 2.0f * (control_points[i + 1] - control_points[i - 1]);
            }
        }

        vertices.reserve((n + (n - 1) * points_per_segment) * 4);
        for (size_t i = 0; i < n - 1; i++) {
            float x0 = x_coords[i];
            float x1 = x_coords[i + 1];
            float y0 = control_points[i];
            float y1 = control_points[i + 1];
            float m0 = tangents[i];
            float m1 = tangents[i + 1];

            vertices.push_back(x0);
            vertices.push_back(-1.0f);
            vertices.push_back(x0);
            vertices.push_back(y0);

            for (size_t j = 1; j <= points_per_segment; j++) {
                float u = static_cast<float>(j) / static_cast<float>(points_per_segment + 1);
                float h0 = 2.0f * u * u * u - 3.0f * u * u + 1.0f;
                float h1 = -2.0f * u * u * u + 3.0f * u * u;
                float h2 = u * u * u - 2.0f * u * u + u;
                float h3 = u * u * u - u * u;

                float x = x0 + u * (x1 - x0);
                float y = h0 * y0 + h1 * y1 + h2 * m0 + h3 * m1;

                vertices.push_back(x);
                vertices.push_back(-1.0f);
                vertices.push_back(x);
                vertices.push_back(y);
            }
        }

        vertices.push_back(x_coords[n - 1]);
        vertices.push_back(-1.0f);
        vertices.push_back(x_coords[n - 1]);
        vertices.push_back(control_points[n - 1]);

        // TODO: 设置更复杂的颜色渐变
        glUseProgram(state->program);
        GLint screenHeight_uniform = glGetUniformLocation(state->program, "screenHeight");
        glUniform4f(state->colorTop_uniform, 0.0f, 0.4f, 1.0f, 0.4f);
        glUniform4f(state->colorBottom_uniform, 0.0f, 1.0f, 0.4f, 0.4f);
        glUniform1f(screenHeight_uniform, static_cast<float>(state->height));
        glBindBuffer(GL_ARRAY_BUFFER, state->vbo);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(GLfloat), vertices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(state->position_attr);
        glVertexAttribPointer(state->position_attr, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, vertices.size() / 2);
        glDisableVertexAttribArray(state->position_attr);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glUseProgram(0);
    }

    glFlush();

    // 交换缓冲区
    eglSwapBuffers(state->egl_display, state->egl_surface);

    wl_surface_commit(state->surface.get());
}

void cleanup_egl(ClientState *state) {
    if (state->vbo) {
        glDeleteBuffers(1, &state->vbo);
        state->vbo = 0;
    }
    if (state->program) {
        glDeleteProgram(state->program);
        state->program = 0;
    }
    if (state->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(state->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        
        if (state->egl_context != EGL_NO_CONTEXT) {
            eglDestroyContext(state->egl_display, state->egl_context);
        }
        
        if (state->egl_surface != EGL_NO_SURFACE) {
            eglDestroySurface(state->egl_display, state->egl_surface);
        }
        
        eglTerminate(state->egl_display);
    }
    
    state->egl_display = EGL_NO_DISPLAY;
    state->egl_context = EGL_NO_CONTEXT;
    state->egl_surface = EGL_NO_SURFACE;
    state->egl_initialized = false;
    std::cout << "[EGL] Cleaned up EGL resources" << std::endl;
}

int main() {
    ClientState state;
    state.display.reset(wl_display_connect(nullptr));
    if (!state.display) {
        std::cerr << "Failed to connect to Wayland display" << std::endl;
        return 1;
    }
    std::cout << "[Wayland] Connected to display" << std::endl;

    state.registry.reset(static_cast<wl_registry*>(
        wl_display_get_registry(state.display.get())
    ));
    wl_registry_add_listener(state.registry.get(), &registry_listener, &state);
    wl_display_roundtrip(state.display.get()); // 等待 registry 回调完成

    if (!state.compositor) {
        std::cerr << "Compositor not available" << std::endl;
        return 1;
    }
    if (!state.layer_shell) {
        std::cerr << "Layer Shell not available" << std::endl;
        return 1;
    }

    state.surface.reset(wl_compositor_create_surface(state.compositor.get()));
    if (!state.surface) {
        std::cerr << "Failed to create surface" << std::endl;
        return 1;
    }
    std::cout << "[Wayland] Created surface" << std::endl;

    state.layer_surface.reset(zwlr_layer_shell_v1_get_layer_surface(
        state.layer_shell.get(), state.surface.get(),
        nullptr, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        "cavalayer"
    ));
    if (!state.layer_surface) {
        std::cerr << "Failed to create layer surface" << std::endl;
        return 1;
    }
    std::cout << "[Layer-Shell] Created layer surface" << std::endl;
    
    zwlr_layer_surface_v1_set_anchor(state.layer_surface.get(), ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
    zwlr_layer_surface_v1_set_size(state.layer_surface.get(), 480, 220);
    zwlr_layer_surface_v1_set_margin(state.layer_surface.get(), 0, 0, 15, 15);
    zwlr_layer_surface_v1_set_keyboard_interactivity(state.layer_surface.get(), ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
    zwlr_layer_surface_v1_add_listener(state.layer_surface.get(), &layer_surface_listener, &state);

    // 首次提交：触发 compositor 发送 configure 事件
    wl_surface_commit(state.surface.get());
    std::cout << "[Wayland] 首次提交 surface" << std::endl;
    // 等待 compositor 配置
    while (!state.configured) {
        wl_display_dispatch(state.display.get());
    }
    std::cout << "[Layer-Shell] compositor 配置完成" << std::endl;

    // 初始化 EGL
    if (!init_egl(&state)) {
        std::cerr << "Failed to initialize EGL" << std::endl;
        return 1;
    }

    state.cava_frame.resize(state.cava_bars);
    if (cava_reader_start(state.bit_format, state.cava_bars, state.ring_capacity) != CAVA_OK) {
        std::cerr << "无法启动 cava_reader" << std::endl;
        return 1;
    }
    std::cout << "[CAVA] Reader started with " << state.cava_bars << " bars" << std::endl;

    // 主渲染循环
    std::cout << "[Layer-Shell] 客户端运行中" << std::endl;
    while (state.running) {
        wl_display_dispatch_pending(state.display.get());
        wl_display_flush(state.display.get());
        draw_frame(&state);
    }

    // 清理资源
    cava_reader_stop();
    cleanup_egl(&state);
    std::cout << "[CAVA] Reader stopped" << std::endl;

    return 0;
}