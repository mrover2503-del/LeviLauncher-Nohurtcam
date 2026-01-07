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
#include <cstring>

#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_android.h"
#include "ImGui/backends/imgui_impl_opengl3.h"

static EGLBoolean (*orig_swap)(EGLDisplay,EGLSurface);
static int32_t (*orig_input)(void*,void*,bool,long,uint32_t*,AInputEvent**);

static int sw,sh;
static bool inited;
static bool show_intro=true;

static float hue;
static int fps,frames;
static double last_fps;

static float touch_x,touch_y;

static bool cfg[10]={1,1,1,1,1,0,0,0,0,0};
static bool hidden[10];

static bool unlocked;
static bool kb_open;
static char pw[32];

struct Item{std::string n;bool e;float a;};
static std::vector<Item> arr={
    {"Battery",1,0},{"FPS",1,0},{"Ping",1,0},{"Resolution",1,0},{"Time",1,0}
};

/* ===================== DARK THEME ===================== */
static void embraceTheDarkness(){
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.20f,0.22f,0.23f,1.00f);
    colors[ImGuiCol_TitleBg]                = ImVec4(0,0,0,1);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0,0,0,0.75f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0,0,0,0.25f);
    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.14f,0.14f,0.14f,1);
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.05f,0.05f,0.05f,0.54f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.70f,0,1,0.50f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.70f,0,1,0.50f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.70f,0,1,1);
    colors[ImGuiCol_CheckMark]              = ImVec4(0.70f,0,1,1);
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.70f,0,1,1);
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.70f,0,1,1);
    colors[ImGuiCol_Button]                 = ImVec4(0.05f,0.05f,0.05f,0.54f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.19f,0.19f,0.19f,0.54f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.20f,0.22f,0.23f,1);
    colors[ImGuiCol_Header]                 = ImVec4(0,0,0,0.52f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0,0,0,0.36f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.20f,0.22f,0.23f,0.33f);
    colors[ImGuiCol_Separator]              = ImVec4(0.70f,0,1,1);

    style.WindowPadding     = ImVec2(8,8);
    style.FramePadding      = ImVec2(5,2);
    style.ItemSpacing       = ImVec2(6,6);
    style.ScrollbarSize     = 10;
    style.WindowBorderSize  = 2;
    style.FrameBorderSize   = 2;
    style.WindowRounding    = 7;
    style.FrameRounding     = 3;
    style.ScrollbarRounding = 9;
    style.TabRounding       = 4;
}

/* ===================== UTILS ===================== */
static ImU32 RGB(){
    hue+=0.6f; if(hue>360) hue=0;
    float h=hue/60,x=1-fabsf(fmodf(h,2)-1);
    float r=0,g=0,b=0;
    if(h<1){r=1;g=x;}else if(h<2){r=x;g=1;}
    else if(h<3){g=1;b=x;}else if(h<4){g=x;b=1;}
    else if(h<5){r=x;b=1;}else{r=1;b=x;}
    return IM_COL32(r*255,g*255,b*255,255);
}

static int Battery(){
    int fd=open("/sys/class/power_supply/battery/capacity",O_RDONLY);
    if(fd<0)return-1;
    char b[8]={0}; read(fd,b,7); close(fd);
    return atoi(b);
}

static int Ping(){
    sockaddr_in a{};
    a.sin_family=AF_INET;
    a.sin_port=htons(53);
    inet_pton(AF_INET,"8.8.8.8",&a.sin_addr);
    int s=socket(AF_INET,SOCK_STREAM,0);
    timeval tv{0,300000};
    setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
    timespec t1,t2;
    clock_gettime(CLOCK_MONOTONIC,&t1);
    int r=connect(s,(sockaddr*)&a,sizeof(a));
    clock_gettime(CLOCK_MONOTONIC,&t2);
    close(s);
    if(r<0)return-1;
    return (t2.tv_sec-t1.tv_sec)*1000+(t2.tv_nsec-t1.tv_nsec)/1000000;
}

/* ===================== UI ===================== */
static void Keyboard(){
    if(!kb_open)return;
    ImGui::SetNextWindowSize({760,460});
    ImGui::Begin("Keyboard",0,ImGuiWindowFlags_NoCollapse);
    const char*k="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for(int i=0;i<36;i++){
        char b[2]={k[i],0};
        if(ImGui::Button(b,{70,70})){
            int l=strlen(pw);
            if(l<31){pw[l]=k[i];pw[l+1]=0;}
        }
        if((i+1)%7)ImGui::SameLine();
    }
    if(ImGui::Button("DEL",{220,60})){int l=strlen(pw);if(l)pw[l-1]=0;}
    ImGui::SameLine();
    if(ImGui::Button("CLEAR",{220,60}))pw[0]=0;
    ImGui::SameLine();
    if(ImGui::Button("CLOSE",{220,60}))kb_open=false;
    ImGui::End();
}

static void OverlayBox(const char*t,float y){
    auto*d=ImGui::GetForegroundDrawList();
    ImVec2 p={20,y};
    d->AddRectFilled(p,{p.x+360,p.y+44},IM_COL32(0,0,0,180),8);
    d->AddRect(p,{p.x+360,p.y+44},RGB(),8,0,3);
    d->AddText({p.x+12,p.y+12},IM_COL32_WHITE,t);
}

static void Overlay(){
    time_t tt=time(0); tm tmv; localtime_r(&tt,&tmv);
    float y=20;
    if(cfg[0])OverlayBox(("Ping: "+std::to_string(Ping())+" ms").c_str(),y),y+=52;
    if(cfg[1])OverlayBox(("FPS: "+std::to_string(fps)).c_str(),y),y+=52;
    if(cfg[2])OverlayBox(("Time: "+std::to_string(tmv.tm_hour)+":"+std::to_string(tmv.tm_min)).c_str(),y),y+=52;
    if(cfg[3])OverlayBox(("Battery: "+std::to_string(Battery())+"%").c_str(),y),y+=52;
    if(cfg[4])OverlayBox(("Res: "+std::to_string(sw)+"x"+std::to_string(sh)).c_str(),y),y+=52;
}

static void ArrayList(){
    auto&io=ImGui::GetIO();
    float y=40;
    for(auto&i:arr){
        if(!i.e)continue;
        i.a+=(1-i.a)*0.15f;
        float w=ImGui::CalcTextSize(i.n.c_str()).x+50;
        float x=io.DisplaySize.x-(w*i.a)-10;
        auto*d=ImGui::GetForegroundDrawList();
        d->AddRectFilled({x,y},{x+w,y+34},IM_COL32(0,0,0,200),6);
        d->AddRectFilled({x+w-4,y},{x+w,y+34},RGB());
        d->AddText({x+12,y+8},IM_COL32_WHITE,i.n.c_str());
        y+=38;
    }
}

static void Intro(){
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
    if(ImGui::Button("I read it",{720,50}))show_intro=false;
    ImGui::End();
}

static void Menu(){
    ImGui::SetNextWindowSize({900,720},ImGuiCond_FirstUseEver);
    ImGui::Begin("Menu");
    ImGui::Text("Config");
    for(int i=0;i<10;i++) ImGui::Checkbox(("Overlay "+std::to_string(i+1)).c_str(),&cfg[i]);
    ImGui::Separator();
    ImGui::Text("Secret");
    if(!unlocked){
        ImGui::InputText("##pw",pw,sizeof(pw));
        ImGui::SameLine();
        if(ImGui::Button("Keyboard"))kb_open=true;
        if(ImGui::Button("Unlock") && !strcmp(pw,"MCMROVER")) unlocked=true;
    }else{
        for(int i=0;i<10;i++) ImGui::Checkbox(("Hidden "+std::to_string(i+1)).c_str(),&hidden[i]);
    }
    ImGui::End();
}

/* ===================== RENDER ===================== */
static void Render(){
    double t=ImGui::GetTime();
    frames++;
    if(t-last_fps>=1){fps=frames;frames=0;last_fps=t;}

    ImGuiIO&io=ImGui::GetIO();
    io.DisplaySize={float(sw),float(sh)};

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(sw,sh);
    ImGui::NewFrame();

    io.FontGlobalScale=1.8f;

    if(show_intro) Intro();
    else { Menu(); Overlay(); ArrayList(); }

    Keyboard();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

/* ===================== HOOKS ===================== */
static EGLBoolean hook_swap(EGLDisplay d,EGLSurface s){
    eglQuerySurface(d,s,EGL_WIDTH,&sw);
    eglQuerySurface(d,s,EGL_HEIGHT,&sh);

    if(!inited){
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        embraceTheDarkness();
        ImGui_ImplAndroid_Init();
        ImGui_ImplOpenGL3_Init("#version 300 es");
        last_fps=ImGui::GetTime();
        inited=true;
    }
    Render();
    return orig_swap(d,s);
}

static int32_t hook_input(void*a,void*b,bool c,long d,uint32_t*e,AInputEvent**ev){
    int r=orig_input(a,b,c,d,e,ev);
    if(ev && *ev && AInputEvent_getType(*ev)==AINPUT_EVENT_TYPE_MOTION){
        ImGui_ImplAndroid_HandleInputEvent(*ev);
    }
    return r;
}

static void* th(void*){
    sleep(3);
    GlossInit(true);
    auto egl=GlossOpen("libEGL.so");
    GlossHook(GlossSymbol(egl,"eglSwapBuffers",0),(void*)hook_swap,(void**)&orig_swap);
    auto inp=GlossOpen("libinput.so");
    GlossHook(
        GlossSymbol(inp,"_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE",0),
        (void*)hook_input,(void**)&orig_input
    );
    return 0;
}

__attribute__((constructor))
void init(){
    pthread_t t;
    pthread_create(&t,0,th,0);
}
