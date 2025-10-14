#define _POSIX_C_SOURCE 200112L
#include <wayland-client.h>
#include <cstring>
#include <iostream>
#include <memory>
#include <unistd.h>
#define namespace ns
#include "layer-shell-client-protocol.h"
#undef namespace
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

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
    // EGL 资源
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    // 状态管理
    uint32_t configure_serial = 0;
    bool configured = false;
    bool egl_initialized = false;
    int width = 0;
    int height = 0;
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
    
    eglBindAPI(EGL_OPENGL_API);

    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
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
        EGL_CONTEXT_CLIENT_VERSION, 2,
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

    eglMakeCurrent(state->egl_display, state->egl_surface, state->egl_surface, state->egl_context);
    glViewport(0, 0, state->width, state->height);

    // 清空屏幕
    glClearColor(1.0f, 0.0f, 0.0f, 0.2f);
    glClear(GL_COLOR_BUFFER_BIT);

    // 交换缓冲区
    eglSwapBuffers(state->egl_display, state->egl_surface);

    wl_surface_commit(state->surface.get());
}

void cleanup_egl(ClientState *state) {
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
    zwlr_layer_surface_v1_set_size(state.layer_surface.get(), 500, 250);
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

    // 主渲染循环
    std::cout << "[Layer-Shell] 客户端运行中 (按 Ctrl+C 退出)" << std::endl;
    while (true) {
        wl_display_dispatch_pending(state.display.get());
        wl_display_flush(state.display.get());
        draw_frame(&state);
        usleep(16000); // 60 FPS
    }

    // 清理 EGL 资源
    cleanup_egl(&state);
    return 0;
}