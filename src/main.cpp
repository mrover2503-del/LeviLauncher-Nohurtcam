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
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, VA_ARGS)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, VA_ARGS)

static bool isEnabled = false;

static uintptr_t g_LibBase = 0;
static uintptr_t g_XPAddress = 0; 
static bool g_XPSearchDone = false;

static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;
static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;

uintptr_t GetLibBase(const char* libName) {
    FILE* fp;
    char line[256];
    uintptr_t addr = 0;
    
    fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;
    
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, libName)) {
            char* end;
            addr = strtoul(line, &end, 16);
            break;
        }
    }
    fclose(fp);
    return addr;
}

uintptr_t FindPattern(uintptr_t start, size_t length, const char* pattern) {
    const char* pat = pattern;
    uintptr_t firstMatch = 0;
    uintptr_t rangeEnd = start + length;

    for (uintptr_t pCur = start; pCur < rangeEnd; pCur++) {
        if (!*pat) return firstMatch;

        if (*(uint8_t*)pat == '\?' || *(uint8_t*)pCur == (uint8_t)strtoul(pat, NULL, 16)) {
            if (!firstMatch) firstMatch = pCur;
            
            if (!pat[2]) return firstMatch;

            if (*(uint16_t*)pat == '\?\?' || *(uint8_t*)pat != '\?') 
                pat += 3;
            else 
                pat += 2;
        } else {
            pat = pattern;
            firstMatch = 0;
        }
    }
    return 0;
}

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static void (orig_Input1)(void, void*, void*) = nullptr;

static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (g_Initialized) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)a2);
    if (orig_Input1) orig_Input1(thiz, a1, a2);
}

static int32_t (orig_Input2)(void, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event) {
    int32_t result = orig_Input2 ? orig_Input2(thiz, a1, a2, a3, a4, event) : 0;
    if (event && *event && g_Initialized) {
        ImGui_ImplAndroid_HandleInputEvent(*event);
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse || io.WantCaptureKeyboard) return result;
    }
    return result;
}

static AAsset* (orig_AAssetManager_open)(AAssetManager, const char*, int) = nullptr;

static AAsset* my_AAssetManager_open(AAssetManager* mgr, const char* name, int mode) {
    if(!isEnabled) return orig_AAssetManager_open(mgr, name, mode);
    if(!(name && strncmp(name, "assets/renderer/materials/", 26) == 0)) return orig_AAssetManager_open(mgr, name, mode);
    std::string base = name;
    size_t pos = base.find_last_of('/');
    if (pos != std::string::npos) base = base.substr(pos + 1);
    if(!(base == "RenderChunk.material.bin")) return orig_AAssetManager_open(mgr, name, mode);
    std::string path = "../../../storage/emulated/0/games/org.levimc/shaders/" + base;
    return orig_AAssetManager_open(mgr, path.c_str(), mode);
}

struct GLState {
    GLint prog, tex, aTex, aBuf, eBuf, vao, fbo, vp[4], sc[4], bSrc, bDst;
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

static void DrawMenu() {
    ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Xp Mod Menu", nullptr, 0);
    
    
    ImGui::Text("Player Stats");
    
    if (ImGui::Button("Add 100 XP")) {
        if (g_LibBase == 0) {
            g_LibBase = GetLibBase("libminecraftpe.so");
        }

        if (g_LibBase != 0 && !g_XPSearchDone) {
            const char* xpPattern = "7C 3D ? ? ? C"; 
            uintptr_t foundAddr = FindPattern(g_LibBase, 0x4000000, xpPattern);
            
            if (foundAddr != 0) {
                g_XPAddress = foundAddr;
                LOGI("XP Address found at: %lX", g_XPAddress);
            } else {
                LOGE("XP Address NOT found with pattern.");
            }
            g_XPSearchDone = true;
        }

        if (g_XPAddress != 0) {
            try {
                float* xpPtr = reinterpret_cast<float*>(g_XPAddress);
                float oldVal = *xpPtr;
                *xpPtr += 100.0f;
                LOGI("Added XP. Old: %f -> New: %f", oldVal, *xpPtr);
            } catch (...) {
                LOGE("Failed to write memory (Crash prevented)");
            }
        } else {
            LOGE("Cannot add XP: Address is null.");
        }
    }

    if (g_XPAddress != 0) {
        ImGui::Text("XP Addr: 0x%lX", g_XPAddress);
    }

    ImGui::End();
}

static void Setup() {
    if (g_Initialized || g_Width <= 0 || g_Height <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    float scale = (float)g_Height / 900.0f;
    if (scale < 1.1f) scale = 1.1f;
    if (scale > 2.5f) scale = 2.5f;
    ImFontConfig cfg;
    cfg.SizePixels = 19.0f * scale;
    io.Fonts->AddFontDefault(&cfg);
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FramePadding = ImVec2(10.0f * scale, 9.0f * scale);
    style.ItemSpacing = ImVec2(9.0f * scale, 9.0f * scale);
    style.GrabMinSize = 19.0f * scale;
    style.ScrollbarSize = 19.0f * scale;
    g_Initialized = true;
}

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

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglSwapBuffers) return EGL_FALSE;
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglSwapBuffers(dpy, surf);
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglSwapBuffers(dpy, surf);
    if (g_TargetContext == EGL_NO_CONTEXT) {
        EGLint buf = 0;
        eglQuerySurface(dpy, surf, EGL_RENDER_BUFFER, &buf);
        if (buf == EGL_BACK_BUFFER) {
            g_TargetContext = ctx;
            g_TargetSurface = surf;
        }
    }
    if (ctx != g_TargetContext || surf != g_TargetSurface) return orig_eglSwapBuffers(dpy, surf);
    g_Width = w;
    g_Height = h;
    Setup();
    Render();
    return orig_eglSwapBuffers(dpy, surf);
}

static void HookInput() {
    void* sym1 = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (sym1) GlossHook(sym1, (void*)hook_Input1, (void**)&orig_Input1);
    void* sym2 = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE", nullptr);
    if (sym2) GlossHook(sym2, (void*)hook_Input2, (void**)&orig_Input2);
}

static void HookAAMO() {
    void* lib = dlopen("libminecraftpe.so", RTLD_NOW);
    if (!lib) return;
    void* sym = dlsym(lib, "AAssetManager_open");
    if (!sym) return;
    pl::hook::pl_hook((pl::hook::FuncPtr)sym, (pl::hook::FuncPtr)my_AAssetManager_open, (pl::hook::FuncPtr*)&orig_AAssetManager_open, pl::hook::PriorityHighest);
}

static void* MainThread(void*) {
    sleep(3);
    GlossInit(true);
    GHandle hEGL = GlossOpen("libEGL.so");
    if (!hEGL) return nullptr;
    void* swap = (void*)GlossSymbol(hEGL, "eglSwapBuffers", nullptr);
    if (!swap) return nullptr;
    GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    HookInput();
    HookAAMO();
    g_LibBase = GetLibBase("libminecraftpe.so");
    return nullptr;
}

__attribute__((constructor))
void DisplayFPS_Init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}
