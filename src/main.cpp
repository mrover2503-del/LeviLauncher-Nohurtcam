#include <jni.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <vector>
#include <fstream>
#include <sstream>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#include <android/log.h>
#include <android/asset_manager.h>

#include <cstring>
#include <string>

#define TAG "INEB"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static bool isEnabled = false;

static uintptr_t g_LibBase = 0;
static uintptr_t g_XPAddress = 0;
static bool g_XPSearchDone = false;

static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;
static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;

/* ===================== Utils ===================== */

uintptr_t GetLibBase(const char* libName) {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;

    char line[512];
    uintptr_t addr = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, libName)) {
            addr = strtoul(line, nullptr, 16);
            break;
        }
    }
    fclose(fp);
    return addr;
}

uintptr_t FindPattern(uintptr_t start, size_t length, const char* pattern) {
    const char* pat = pattern;
    uintptr_t firstMatch = 0;

    for (uintptr_t cur = start; cur < start + length; cur++) {
        if (!*pat) return firstMatch;

        if (*pat == '?' || *(uint8_t*)cur == strtoul(pat, nullptr, 16)) {
            if (!firstMatch) firstMatch = cur;
            pat += (*(pat + 1) == '?' || *pat == '?') ? 2 : 3;
        } else {
            pat = pattern;
            firstMatch = 0;
        }
    }
    return 0;
}

/* ===================== Hooks ===================== */

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;

static void (*orig_Input1)(void*, void*, void*) = nullptr;
static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;
static AAsset* (*orig_AAssetManager_open)(AAssetManager*, const char*, int) = nullptr;

static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (g_Initialized)
        ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)a2);
    if (orig_Input1)
        orig_Input1(thiz, a1, a2);
}

static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event) {
    int32_t res = orig_Input2 ? orig_Input2(thiz, a1, a2, a3, a4, event) : 0;
    if (event && *event && g_Initialized)
        ImGui_ImplAndroid_HandleInputEvent(*event);
    return res;
}

static AAsset* my_AAssetManager_open(AAssetManager* mgr, const char* name, int mode) {
    if (!isEnabled || !name)
        return orig_AAssetManager_open(mgr, name, mode);

    if (strncmp(name, "assets/renderer/materials/", 26) != 0)
        return orig_AAssetManager_open(mgr, name, mode);

    std::string base = strrchr(name, '/') + 1;
    if (base != "RenderChunk.material.bin")
        return orig_AAssetManager_open(mgr, name, mode);

    std::string path = "/storage/emulated/0/games/org.levimc/shaders/" + base;
    return orig_AAssetManager_open(mgr, path.c_str(), mode);
}

/* ===================== ImGui ===================== */

static void DrawMenu() {
    ImGui::Begin("NexCaise Mod Menu");

    ImGui::Checkbox("Entity ESP", &isEnabled);

    if (ImGui::Button("Add 100 XP")) {
        if (!g_LibBase)
            g_LibBase = GetLibBase("libminecraftpe.so");

        if (g_LibBase && !g_XPSearchDone) {
            g_XPAddress = FindPattern(g_LibBase, 0x4000000, "7C 3D ? ? ? C");
            g_XPSearchDone = true;
            LOGI("XP Addr: %p", (void*)g_XPAddress);
        }

        if (g_XPAddress) {
            float* xp = (float*)g_XPAddress;
            *xp += 100.f;
        }
    }

    ImGui::End();
}

/* ===================== Render ===================== */

static void Render() {
    if (!g_Initialized) return;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(g_Width, g_Height);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_Width, g_Height);
    ImGui::NewFrame();

    DrawMenu();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    EGLContext ctx = eglGetCurrentContext();
    if (!orig_eglSwapBuffers || ctx == EGL_NO_CONTEXT)
        return orig_eglSwapBuffers(dpy, surf);

    eglQuerySurface(dpy, surf, EGL_WIDTH, &g_Width);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &g_Height);

    if (!g_Initialized) {
        ImGui::CreateContext();
        ImGui_ImplAndroid_Init();
        ImGui_ImplOpenGL3_Init("#version 300 es");
        g_Initialized = true;
    }

    Render();
    return orig_eglSwapBuffers(dpy, surf);
}

/* ===================== Thread ===================== */

static void* MainThread(void*) {
    sleep(2);

    GlossInit(true);
    auto egl = GlossOpen("libEGL.so");
    auto swap = GlossSymbol(egl, "eglSwapBuffers", nullptr);
    GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);

    return nullptr;
}

__attribute__((constructor))
void Init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}
