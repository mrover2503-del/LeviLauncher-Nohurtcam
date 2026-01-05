#include <jni.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <android/log.h>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "imgui.h"
#include "backends/imgui_impl_android.h"
#include "backends/imgui_impl_opengl3.h"

#define TAG "INEB"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;
static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static void (*orig_Input1)(void*, void*, void*) = nullptr;
static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

uintptr_t GetLibBase(const char* lib) {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;
    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, lib)) {
            sscanf(line, "%lx-", &base);
            break;
        }
    }
    fclose(fp);
    return base;
}

struct Pattern {
    std::vector<uint8_t> bytes;
    std::string mask;
};

Pattern MakePattern() {
    uint8_t data[] = {
        0x51,0x00,0xAE,0xC1,
        0x46,0x00,0x00,0x41,0x00,0x00,0x00,0x00,
        0xFE,0xFF,0x46,0xFE,0xFF,
        0x46,0xD0,0xA6,0xFE,
        0x6B,0xBA,0x72,0x00,
        0xC0,0xC1,0x39,0x6C,
        0xBA,0x72,0x00
    };
    std::string mask =
        "xxxx"
        "xxxxxxxxxxxx"
        "xxxxx"
        "xxxx"
        "xxxx"
        "xx?x"
        "xxxx"
        "xxx";
    return { std::vector<uint8_t>(data, data + sizeof(data)), mask };
}

bool Match(uint8_t* addr, const Pattern& p) {
    for (size_t i = 0; i < p.bytes.size(); i++) {
        if (p.mask[i] == 'x' && addr[i] != p.bytes[i])
            return false;
    }
    return true;
}

std::vector<uintptr_t> ScanAll(uintptr_t start, size_t size, const Pattern& p) {
    std::vector<uintptr_t> out;
    for (uintptr_t i = start; i < start + size - p.bytes.size(); i++) {
        if (Match((uint8_t*)i, p)) {
            out.push_back(i);
        }
    }
    return out;
}

static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (g_Initialized)
        ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)a2);
    if (orig_Input1)
        orig_Input1(thiz, a1, a2);
}

static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event) {
    int32_t ret = orig_Input2 ? orig_Input2(thiz, a1, a2, a3, a4, event) : 0;
    if (event && *event && g_Initialized)
        ImGui_ImplAndroid_HandleInputEvent(*event);
    return ret;
}

struct GLState {
    GLint prog, vao, fbo, vp[4];
};

static void SaveGL(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.vp);
}

static void RestoreGL(const GLState& s) {
    glUseProgram(s.prog);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
}

static void DrawMenu() {
    ImGui::Begin("XP Hack");
    if (ImGui::Button("Set All To 100")) {
        uintptr_t base = GetLibBase("libminecraftpe.so");
        if (base) {
            Pattern p = MakePattern();
            auto results = ScanAll(base, 0x8000000, p);
            for (auto addr : results) {
                float* f = (float*)addr;
                *f = 100.0f;
            }
        }
    }
    ImGui::End();
}

static void Setup() {
    if (g_Initialized) return;
    ImGui::CreateContext();
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    g_Initialized = true;
}

static void Render() {
    GLState s;
    SaveGL(s);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_Width, g_Height);
    ImGui::NewFrame();
    DrawMenu();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    RestoreGL(s);
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT)
        return orig_eglSwapBuffers(dpy, surf);
    eglQuerySurface(dpy, surf, EGL_WIDTH, &g_Width);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &g_Height);
    if (!g_Initialized)
        Setup();
    Render();
    return orig_eglSwapBuffers(dpy, surf);
}

static void HookInput() {
    void* lib = GlossOpen("libinput.so");
    void* s1 = GlossSymbol(lib, "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    void* s2 = GlossSymbol(lib, "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE", nullptr);
    if (s1) GlossHook(s1, (void*)hook_Input1, (void**)&orig_Input1);
    if (s2) GlossHook(s2, (void*)hook_Input2, (void**)&orig_Input2);
}

static void* MainThread(void*) {
    sleep(3);
    GlossInit(true);
    void* egl = GlossOpen("libEGL.so");
    void* swap = GlossSymbol(egl, "eglSwapBuffers", nullptr);
    GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    HookInput();
    return nullptr;
}

__attribute__((constructor))
void Init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}
