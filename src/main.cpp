#include <jni.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <cmath>
#include <ctime>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

static bool initialized=false;
static int width=0,height=0;
static EGLBoolean (*orig_swap)(EGLDisplay,EGLSurface)=0;
static int32_t (*orig_input)(void*,void*,bool,long,uint32_t*,AInputEvent**)=0;

static bool show_intro=true;
static float intro_time=0;

static int tab=0;
static bool show_keyboard=false;
static std::string password_input="";
static bool password_ok=false;

static float hue=0;

static bool features[30];
static bool hidden_features[10];

static int fps=0;
static int frames=0;
static double last_time=0;

static int get_ping(){ return 42; }

static ImVec4 rgb()
{
    hue+=0.15f;
    if(hue>360)hue=0;
    float h=hue/60,c=1,x=c*(1-fabs(fmod(h,2)-1));
    float r=0,g=0,b=0;
    if(h<1){r=c;g=x;}
    else if(h<2){r=x;g=c;}
    else if(h<3){g=c;b=x;}
    else if(h<4){g=x;b=c;}
    else if(h<5){r=x;b=c;}
    else{r=c;b=x;}
    return ImVec4(r,g,b,1);
}

static void theme()
{
    ImGuiStyle&s=ImGui::GetStyle();
    ImVec4* c=s.Colors;
    ImVec4 a=rgb();
    c[ImGuiCol_WindowBg]=ImVec4(0,0,0,0.85f);
    c[ImGuiCol_Border]=a;
    c[ImGuiCol_Button]=ImVec4(0.18f,0.18f,0.18f,0.9f);
    c[ImGuiCol_ButtonHovered]=a;
    c[ImGuiCol_ButtonActive]=a;
    s.WindowBorderSize=2;
    s.WindowRounding=14;
    s.FrameRounding=10;
}

static void keyboard()
{
    if(!show_keyboard)return;
    ImGui::SetNextWindowSize(ImVec2(720,420));
    ImGui::Begin("Virtual Keyboard",0,ImGuiWindowFlags_NoCollapse);
    const char* k="ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for(int i=0;i<26;i++)
    {
        char b[2]={k[i],0};
        if(ImGui::Button(b,ImVec2(60,60))) password_input.push_back(k[i]);
        if((i+1)%7!=0) ImGui::SameLine();
    }
    if(ImGui::Button("DEL",ImVec2(200,60)) && !password_input.empty()) password_input.pop_back();
    ImGui::SameLine();
    if(ImGui::Button("CLEAR",ImVec2(200,60))) password_input.clear();
    ImGui::SameLine();
    if(ImGui::Button("CLOSE",ImVec2(200,60))) show_keyboard=false;
    ImGui::End();
}

static void overlays()
{
    ImGui::SetNextWindowPos(ImVec2(width-300,20));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("Overlay",0,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize);
    ImGui::Text("Ping: %d ms",get_ping());
    ImGui::Text("FPS: %d",fps);
    time_t t=time(0);
    tm* lt=localtime(&t);
    ImGui::Text("Time: %02d:%02d:%02d",lt->tm_hour,lt->tm_min,lt->tm_sec);
    ImGui::End();
}

static void intro()
{
    ImGui::SetNextWindowSize(ImVec2(700,420));
    ImGui::SetNextWindowPos(ImVec2(width/2,height/2),0,ImVec2(0.5f,0.5f));
    ImGui::Begin("Intro",0,ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoResize);
    ImGui::SetWindowFontScale(1.6f);
    ImGui::Text("Shortcut Plugin");
    ImGui::Separator();
    ImGui::TextColored(rgb(),"████████████████████████████");
    ImGui::Text("Version: V1.0");
    ImGui::Text("Development: MCPE OVER");
    ImGui::Text("Launcher: Light, Levi Launcher");
    ImGui::End();
    intro_time+=ImGui::GetIO().DeltaTime;
    if(intro_time>3.5f) show_intro=false;
}

static void config_tab()
{
    ImGui::SetWindowFontScale(1.3f);
    ImGui::Text("Config Features");
    ImGui::Separator();
    for(int i=0;i<30;i++)
    {
        std::string name="Feature "+std::to_string(i+1);
        ImGui::Checkbox(name.c_str(),&features[i]);
        if(i%2==0) ImGui::SameLine();
    }
}

static void sorry_tab()
{
    ImGui::SetWindowFontScale(1.3f);
    if(!password_ok)
    {
        ImGui::Text("Password Required");
        ImGui::InputText("##pw",&password_input);
        ImGui::SameLine();
        if(ImGui::Button("Keyboard",ImVec2(160,50))) show_keyboard=true;
        if(ImGui::Button("Unlock",ImVec2(240,60)))
            if(password_input=="MCMROVER") password_ok=true;
    }
    else
    {
        ImGui::Text("Hidden Features");
        ImGui::Separator();
        for(int i=0;i<10;i++)
        {
            std::string n="Hidden Feature "+std::to_string(i+1);
            ImGui::Checkbox(n.c_str(),&hidden_features[i]);
        }
    }
}

static void ui()
{
    ImGui::SetNextWindowSize(ImVec2(820,660),ImGuiCond_FirstUseEver);
    ImGui::Begin("Main Panel");
    ImGui::BeginChild("tabs",ImVec2(160,0),true);
    if(ImGui::Button("Config",ImVec2(-1,80))) tab=0;
    if(ImGui::Button("Sorry",ImVec2(-1,80))) tab=1;
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("content",ImVec2(0,0),true);
    if(tab==0) config_tab();
    if(tab==1) sorry_tab();
    ImGui::EndChild();
    ImGui::End();
    keyboard();
    overlays();
}

static int32_t hook_input(void*a,void*b,bool c,long d,uint32_t*e,AInputEvent**ev)
{
    int32_t r=orig_input?orig_input(a,b,c,d,e,ev):0;
    if(r==0 && ev && *ev && initialized)
        ImGui_ImplAndroid_HandleInputEvent(*ev);
    return r;
}

static void setup()
{
    if(initialized)return;
    ImGui::CreateContext();
    ImGuiIO&io=ImGui::GetIO();
    io.FontGlobalScale=1.6f;
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    last_time=ImGui::GetTime();
    initialized=true;
}

static void render()
{
    double now=ImGui::GetTime();
    frames++;
    if(now-last_time>=1.0)
    {
        fps=frames;
        frames=0;
        last_time=now;
    }
    theme();
    ImGuiIO&io=ImGui::GetIO();
    io.DisplaySize=ImVec2(width,height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(width,height);
    ImGui::NewFrame();
    if(show_intro) intro(); else ui();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

static EGLBoolean hook_swap(EGLDisplay d,EGLSurface s)
{
    eglQuerySurface(d,s,EGL_WIDTH,&width);
    eglQuerySurface(d,s,EGL_HEIGHT,&height);
    setup();
    render();
    return orig_swap(d,s);
}

static void* thread(void*)
{
    sleep(3);
    GlossInit(true);
    GHandle egl=GlossOpen("libEGL.so");
    void* sw=(void*)GlossSymbol(egl,"eglSwapBuffers",0);
    GlossHook(sw,(void*)hook_swap,(void**)&orig_swap);
    GHandle inp=GlossOpen("libinput.so");
    void* sym=(void*)GlossSymbol(inp,"_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE",0);
    GlossHook(sym,(void*)hook_input,(void**)&orig_input);
    return 0;
}

__attribute__((constructor))
void init()
{
    pthread_t t;
    pthread_create(&t,0,thread,0);
}
