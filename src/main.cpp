#include <jni.h>
#include <android/input.h>
#include <android/log.h>
#include <android/asset_manager.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>

#include <vector>
#include <fstream>
#include <sstream>
#include <cstring>
#include <string>
#include <cstdio>
#include <cstdint>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define TAG "INEB"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* ===================== Globals ===================== */

static bool isEnabled = false;

static uintptr_t g_LibBase = 0;
static uintptr_t g_XPAddress = 0;
static bool g_XPSearchDone = false;

static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;

static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;

/* ===================== Function Pointers ===================== */

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;

static void (*orig_Input1)(void*, void*, void*) = nullptr;
static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

static AAsset* (*orig_AAssetManager_open)(AAssetManager*, const char*, int) = nullptr;

/* ===================== Utils ===================== */

uintptr_t GetLibBase(const char* lib) {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;

    char line[512];
    uintptr_t base = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, lib)) {
            base = strtoul(line, nullptr, 16);
            break;
        }
    }
    fclose(fp);
    return base;
}

uintptr_t FindPattern(uintptr_t start, size_t size, const char* pattern) {
    const char* pat = pattern;
    uintptr_t first = 0;

    for (uintptr_t cur = start; cur < start + size; cur++) {
        if (!*pat) return first;

        if (*pat == '?' || *(uint8_t*)cur == strtoul(pat, nullptr, 16)) {
            if (!first) first = cur;
            pat += (*(pat + 1) == '?' || *pat == '?') ? 2 : 3;
        } else {
            pat = pattern;
            first = 0;
        }
    }
    return 0;
}

/* ===================== GL State ===================== */

struct GLState {
    GLint prog, tex, aTex, aBuf, eBuf, vao, fbo;
    GLint vp[4], sc[4];
    GLint bSrc, bDst;
    GLboolean blend, cull, depth, scissor;
};

static void SaveGL(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.aTex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.aBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.eBuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.vp);
    glGetIntegerv(GL_SCISSOR_BOX, s.sc);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.bSrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.bDst);

    s.blend = glIsEnabled(GL_BLEND);
    s.cull = glIsEnabled(GL_CULL_FACE);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
}

static void RestoreGL(const GLState& s) {
    glUseProgram(s.prog);
    glActiveTexture(s.aTex);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.aBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.eBuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    glScissor(s.sc[0], s.sc[1], s.sc[2], s.sc[3]);
    glBlendFunc(s.bSrc, s.bDst);

    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    s.cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    s.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
}

/* ===================== Input Hooks ===================== */

static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (g_Initialized)
        ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)a2);
    if (orig_Input1)
        orig_Input1(thiz, a1, a2);
}

static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3,
                           uint32_t* a4, AInputEvent** event) {
    int32_t ret = orig_Input2 ? orig_Input2(thiz, a1, a2, a3, a4, event) : 0;
    if (event && *event && g_Initialized)
        ImGui_ImplAndroid_HandleInputEvent(*event);
    return ret;
}

/* ===================== AAsset Hook ===================== */

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

/* ===================== Menu ===================== */

static void DrawMenu() {
    ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("NexCaise Mod Menu");

    ImGui::Checkbox("Entity ESP", &isEnabled);

    if (ImGui::Button("Add 100 XP")) {
        if (!g_LibBase)
            g_LibBase = GetLibBase("libminecraftpe.so");

        if (g_LibBase && !g_XPSearchDone) {
            g_XPAddress = FindPattern(g_LibBase, 0x4000000, "7C 3D ? ? ? C");
            g_XPSearchDone = true;
            LOGI("XP Addr = %p", (void*)g_XPAddress);
        }

        if (g_XPAddress) {
            float* xp = (float*)g_XPAddress;
            *xp += 100.f;
        }
    }

    if (g_XPAddress)
        ImGui::Text("XP Addr: %p", (void*)g_XPAddress);

    ImGui::End();
}

/* ===================== Render ===================== */

static void Render() {
    if (!g_Initialized) return;

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

/* ===================== EGL Hook ===================== */

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglSwapBuffers)
        return EGL_FALSE;

    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT)
        return orig_eglSwapBuffers(dpy, surf);

    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);

    if (w < 500 || h < 500)
        return orig_eglSwapBuffers(dpy, surf);

    if (g_TargetContext == EGL_NO_CONTEXT) {
        g_TargetContext = ctx;
        g_TargetSurface = surf;
    }

    if (ctx != g_TargetContext || surf != g_TargetSurface)
        return orig_eglSwapBuffers(dpy, surf);

    g_Width = w;
    g_Height = h;

    if (!g_Initialized) {
        ImGui::CreateContext();
        ImGui_ImplAndroid_Init();
        ImGui_ImplOpenGL3_Init("#version 300 es");
        g_Initialized = true;
    }

    Render();
    return orig_eglSwapBuffers(dpy, surf);
}

/* ===================== Hooks Setup ===================== */

static void HookInput() {
    GHandle h = GlossOpen("libinput.so");

    void* s1 = (void*)GlossSymbol(h,
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE",
        nullptr);
    if (s1)
        GlossHook(s1, (void*)hook_Input1, (void**)&orig_Input1);

    void* s2 = (void*)GlossSymbol(h,
        "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE",
        nullptr);
    if (s2)
        GlossHook(s2, (void*)hook_Input2, (void**)&orig_Input2);
}

static void HookAAMO() {
    void* lib = dlopen("libminecraftpe.so", RTLD_NOW);
    if (!lib) return;

    void* sym = dlsym(lib, "AAssetManager_open");
    if (!sym) return;

    pl::hook::pl_hook(
        (pl::hook::FuncPtr)sym,
        (pl::hook::FuncPtr)my_AAssetManager_open,
        (pl::hook::FuncPtr*)&orig_AAssetManager_open,
        pl::hook::PriorityHighest
    );
}

/* ===================== Thread ===================== */

static void* MainThread(void*) {
    sleep(3);

    GlossInit(true);

    GHandle hEGL = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(hEGL, "eglSwapBuffers", nullptr);

    GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);

    HookInput();
    HookAAMO();

    g_LibBase = GetLibBase("libminecraftpe.so");
    return nullptr;
}

/* ===================== Entry ===================== */

__attribute__((constructor))
void Init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}
