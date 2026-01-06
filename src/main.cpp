#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#include <fstream>
#include <sstream>
#include <string>

/* ================= Globals ================= */

static bool g_initialized = false;
static int g_width = 0, g_height = 0;
static EGLContext g_targetcontext = EGL_NO_CONTEXT;
static EGLSurface g_targetsurface = EGL_NO_SURFACE;

static EGLBoolean (*orig_eglswapbuffers)(EGLDisplay, EGLSurface) = nullptr;

/* RGB Accent */
static float redd = 180.f;
static float greenn = 0.f;
static float bluee = 255.f;

/* Crosshair */
static bool crosshair_enabled = false;
static float crosshair_length_x = 35.f;
static float crosshair_length_y = 35.f;
static float crosshair_thickness = 3.f;
static ImVec4 crosshair_color = ImVec4(0,1,0,1);

/* options.txt */
static const char* OPTIONS_PATH =
"/storage/emulated/0/Android/data/org.levimc.launcher/files/games/com.mojang/minecraftpe/options.txt";

static char options_buffer[16384];
static bool options_loaded = false;
static bool options_dirty = false;

/* Input */
static int32_t (*orig_input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

/* ================= Theme ================= */

static void apply_theme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;

    ImVec4 accent(
        redd / 255.f,
        greenn / 255.f,
        bluee / 255.f,
        1.f
    );

    c[ImGuiCol_Text] = ImVec4(1,1,1,1);
    c[ImGuiCol_WindowBg] = ImVec4(0,0,0,0.75f);
    c[ImGuiCol_FrameBg] = ImVec4(0.05f,0.05f,0.05f,0.54f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.19f,0.19f,0.19f,0.54f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.2f,0.22f,0.23f,1);

    c[ImGuiCol_Border] = accent;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Separator] = accent;
    c[ImGuiCol_SeparatorHovered] = ImVec4(accent.x,accent.y,accent.z,0.35f);
    c[ImGuiCol_ResizeGrip] = ImVec4(accent.x,accent.y,accent.z,0.5f);
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_ScrollbarGrab] = ImVec4(accent.x,accent.y,accent.z,0.5f);
    c[ImGuiCol_ScrollbarGrabActive] = accent;

    style.WindowRounding = 7.f;
    style.FrameRounding = 4.f;
    style.ScrollbarRounding = 8.f;
    style.WindowBorderSize = 2.f;
}

/* ================= Input Hook ================= */

static int32_t hook_input2(
    void* thiz, void* a1, bool a2, long a3,
    uint32_t* a4, AInputEvent** event)
{
    int32_t r = orig_input2 ? orig_input2(thiz,a1,a2,a3,a4,event) : 0;
    if (r == 0 && event && *event && g_initialized)
        ImGui_ImplAndroid_HandleInputEvent(*event);
    return r;
}

/* ================= Options ================= */

static void load_options()
{
    std::ifstream f(OPTIONS_PATH);
    if (!f.is_open()) return;
    std::stringstream ss;
    ss << f.rdbuf();
    strncpy(options_buffer, ss.str().c_str(), sizeof(options_buffer)-1);
    options_loaded = true;
}

static void save_options()
{
    std::ofstream f(OPTIONS_PATH, std::ios::trunc);
    if (!f.is_open()) return;
    f << options_buffer;
    options_dirty = false;
}

/* ================= Draw ================= */

static void draw_crosshair()
{
    if (!crosshair_enabled) return;
    auto* d = ImGui::GetBackgroundDrawList();
    ImVec2 c(g_width*0.5f, g_height*0.5f);
    ImU32 col = ImGui::ColorConvertFloat4ToU32(crosshair_color);

    d->AddLine({c.x-crosshair_length_x,c.y},{c.x+crosshair_length_x,c.y},col,crosshair_thickness);
    d->AddLine({c.x,c.y-crosshair_length_y},{c.x,c.y+crosshair_length_y},col,crosshair_thickness);
}

static void draw_menu()
{
    ImVec2 center(g_width*0.5f, g_height*0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {0.5f,0.5f});
    ImGui::SetNextWindowSize({650,650}, ImGuiCond_FirstUseEver);

    ImGui::Begin("Editor");

    ImGui::Checkbox("Crosshair",&crosshair_enabled);
    ImGui::SliderFloat("Length X",&crosshair_length_x,5,150);
    ImGui::SliderFloat("Length Y",&crosshair_length_y,5,150);
    ImGui::SliderFloat("Thickness",&crosshair_thickness,1,10);
    ImGui::ColorEdit4("Color",(float*)&crosshair_color);

    ImGui::Separator();

    if (!options_loaded) load_options();
    ImGui::InputTextMultiline("options.txt",options_buffer,sizeof(options_buffer),{0,300});
    if (ImGui::Button("Save")) save_options();

    ImGui::End();
}

/* ================= Render ================= */

static void setup()
{
    if (g_initialized) return;

    ImGui::CreateContext();
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");

    apply_theme();
    g_initialized = true;
}

static void render()
{
    if (!g_initialized) return;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {(float)g_width,(float)g_height};

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_width,g_height);
    ImGui::NewFrame();

    draw_menu();
    draw_crosshair();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

/* ================= EGL Hook ================= */

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf)
{
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT)
        return orig_eglswapbuffers(dpy,surf);

    eglQuerySurface(dpy,surf,EGL_WIDTH,&g_width);
    eglQuerySurface(dpy,surf,EGL_HEIGHT,&g_height);

    setup();
    render();

    return orig_eglswapbuffers(dpy,surf);
}

/* ================= Init ================= */

static void* mainthread(void*)
{
    sleep(3);
    GlossInit(true);

    GHandle egl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(egl,"eglSwapBuffers",nullptr);
    GlossHook(swap,(void*)hook_eglswapbuffers,(void**)&orig_eglswapbuffers);

    GHandle input = GlossOpen("libinput.so");
    void* sym = (void*)GlossSymbol(
        input,
        "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE",
        nullptr
    );
    GlossHook(sym,(void*)hook_input2,(void**)&orig_input2);

    return nullptr;
}

__attribute__((constructor))
void init()
{
    pthread_t t;
    pthread_create(&t,nullptr,mainthread,nullptr);
}
