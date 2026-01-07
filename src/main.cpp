#include <jni.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <ctime>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_android.h"
#include "ImGui/backends/imgui_impl_opengl3.h"

static EGLBoolean (*orig_swap)(EGLDisplay,EGLSurface);
static int32_t (*orig_input)(void*,void*,bool,long,uint32_t*,AInputEvent**);

static int sw, sh;
static bool inited;
static bool show_intro = true;

static float hue;
static int fps, frames;
static double last_fps;

static float touch_x, touch_y;

static bool cfg[10];
static const char* cfg_name[10] = {
    "Ping","FPS","Battery","Time","Resolution",
    "Touch X","Touch Y","Aspect Ratio","DPI Scale","RGB Info"
};

struct Feature {
    std::string name;
    bool* state;
    float anim;
};

static std::vector<Feature> features;

static ImU32 RGB() {
    hue += 0.5f;
    if (hue > 360) hue = 0;
    float h = hue / 60.f;
    float x = 1 - fabsf(fmodf(h, 2) - 1);
    float r=0,g=0,b=0;
    if(h<1){r=1;g=x;}
    else if(h<2){r=x;g=1;}
    else if(h<3){g=1;b=x;}
    else if(h<4){g=x;b=1;}
    else if(h<5){r=x;b=1;}
    else{r=1;b=x;}
    return IM_COL32(r*255,g*255,b*255,255);
}

static int Battery() {
    int fd = open("/sys/class/power_supply/battery/capacity", O_RDONLY);
    if(fd < 0) return -1;
    char b[8] = {0};
    read(fd, b, 7);
    close(fd);
    return atoi(b);
}

static int Ping() {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    timeval tv{0,300000};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    timespec t1,t2;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    int r = connect(s,(sockaddr*)&a,sizeof(a));
    clock_gettime(CLOCK_MONOTONIC, &t2);
    close(s);
    if(r < 0) return -1;
    return (t2.tv_sec-t1.tv_sec)*1000 + (t2.tv_nsec-t1.tv_nsec)/1000000;
}

static void OverlayBox(const char* text, float y) {
    ImDrawList* d = ImGui::GetForegroundDrawList();
    ImVec2 p = {20,y};
    float w = 420;
    d->AddRectFilled(p,{p.x+w,p.y+46},IM_COL32(0,0,0,128),8);
    d->AddRect(p,{p.x+w,p.y+46},RGB(),8,0,3);
    d->AddText({p.x+14,p.y+13},IM_COL32_WHITE,text);
}

static void DrawOverlay() {
    float y = 20;
    time_t tt = time(0);
    tm tmv; localtime_r(&tt,&tmv);

    if(cfg[0]) OverlayBox(("Ping: "+std::to_string(Ping())+" ms").c_str(), y), y+=56;
    if(cfg[1]) OverlayBox(("FPS: "+std::to_string(fps)).c_str(), y), y+=56;
    if(cfg[2]) OverlayBox(("Battery: "+std::to_string(Battery())+"%").c_str(), y), y+=56;
    if(cfg[3]) OverlayBox(("Time: "+std::to_string(tmv.tm_hour)+":"+std::to_string(tmv.tm_min)).c_str(), y), y+=56;
    if(cfg[4]) OverlayBox(("Resolution: "+std::to_string(sw)+"x"+std::to_string(sh)).c_str(), y), y+=56;
    if(cfg[5]) OverlayBox(("Touch X: "+std::to_string((int)touch_x)).c_str(), y), y+=56;
    if(cfg[6]) OverlayBox(("Touch Y: "+std::to_string((int)touch_y)).c_str(), y), y+=56;
    if(cfg[7]) OverlayBox(("Aspect Ratio: "+std::to_string((float)sw/sh)).c_str(), y), y+=56;
    if(cfg[8]) OverlayBox(("DPI Scale: "+std::to_string(ImGui::GetIO().FontGlobalScale)).c_str(), y), y+=56;
    if(cfg[9]) OverlayBox(("RGB Hue: "+std::to_string((int)hue)).c_str(), y), y+=56;
}

static void DrawArrayList() {
    auto& io = ImGui::GetIO();
    std::vector<Feature*> active;
    for(auto& f:features) if(*f.state) active.push_back(&f);
    if(active.empty()) return;

    std::sort(active.begin(),active.end(),
        [](Feature* a, Feature* b){return a->name < b->name;});

    float y = 40;
    for(auto* f : active) {
        f->anim += (1 - f->anim) * 0.15f;
        float w = ImGui::CalcTextSize(f->name.c_str()).x + 50;
        float x = io.DisplaySize.x - (w * f->anim) - 10;
        ImDrawList* d = ImGui::GetForegroundDrawList();
        d->AddRectFilled({x,y},{x+w,y+34},IM_COL32(0,0,0,200),6);
        d->AddRectFilled({x+w-4,y},{x+w,y+34},RGB());
        d->AddText({x+12,y+8},IM_COL32_WHITE,f->name.c_str());
        y += 38;
    }
}

static void Intro() {
    ImGui::SetNextWindowSize({760,420});
    ImGui::SetNextWindowPos({sw/2.f,sh/2.f},0,{0.5f,0.5f});
    ImGui::Begin("Intro",0,ImGuiWindowFlags_NoResize);
    ImGui::SetWindowFontScale(1.6f);
    ImGui::Text("Shortcut Plugin");
    ImGui::Separator();
    ImGui::Text("Version: V1.0");
    ImGui::Text("Development: MCPE OVER");
    ImGui::Text("Launcher: Light, Levi Launcher");
    ImGui::SetCursorPosY(340);
    if(ImGui::Button("I read it",{720,50})) show_intro=false;
    ImGui::End();
}

static void Menu() {
    ImGui::SetNextWindowSize({900,720},ImGuiCond_FirstUseEver);
    ImGui::Begin("Menu");
    ImGui::Text("Overlay Features");
    ImGui::Separator();
    for(int i=0;i<10;i++)
        ImGui::Checkbox(cfg_name[i], &cfg[i]);
    ImGui::End();
}

static void Render() {
    double t = ImGui::GetTime();
    frames++;
    if(t-last_fps >= 1){ fps=frames; frames=0; last_fps=t; }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {float(sw),float(sh)};
    io.FontGlobalScale = 1.8f;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(sw,sh);
    ImGui::NewFrame();

    if(show_intro) Intro();
    else {
        Menu();
        DrawOverlay();
        DrawArrayList();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

static EGLBoolean hook_swap(EGLDisplay d,EGLSurface s) {
    eglQuerySurface(d,s,EGL_WIDTH,&sw);
    eglQuerySurface(d,s,EGL_HEIGHT,&sh);
    if(!inited){
        ImGui::CreateContext();
        ImGui_ImplAndroid_Init();
        ImGui_ImplOpenGL3_Init("#version 300 es");
        for(int i=0;i<10;i++) features.push_back({cfg_name[i],&cfg[i],0});
        last_fps = ImGui::GetTime();
        inited = true;
    }
    Render();
    return orig_swap(d,s);
}

static int32_t hook_input(void*a,void*b,bool c,long d,uint32_t*e,AInputEvent**ev) {
    int r = orig_input(a,b,c,d,e,ev);
    if(ev && *ev && AInputEvent_getType(*ev)==AINPUT_EVENT_TYPE_MOTION){
        touch_x = AMotionEvent_getX(*ev,0);
        touch_y = AMotionEvent_getY(*ev,0);
        ImGui_ImplAndroid_HandleInputEvent(*ev);
    }
    return r;
}

static void* th(void*) {
    sleep(3);
    GlossInit(true);
    auto egl = GlossOpen("libEGL.so");
    GlossHook((void*)GlossSymbol(egl,"eglSwapBuffers",0),(void*)hook_swap,(void**)&orig_swap);
    auto inp = GlossOpen("libinput.so");
    GlossHook((void*)GlossSymbol(inp,"_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE",0),(void*)hook_input,(void**)&orig_input);
    return 0;
}

__attribute__((constructor))
void init() {
    pthread_t t;
    pthread_create(&t,0,th,0);
}
