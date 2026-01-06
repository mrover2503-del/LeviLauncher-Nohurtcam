#include <jni.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <ctime>
#include <cmath>
#include <cstring>
#include <string>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_android.h"
#include "ImGui/backends/imgui_impl_opengl3.h"

static EGLBoolean (*orig_swap)(EGLDisplay,EGLSurface)=0;
static int32_t (*orig_input)(void*,void*,bool,long,uint32_t*,AInputEvent**)=0;

static int w=0,h=0;
static bool inited=false;

static bool show_intro=true;
static float intro_t=0;

static int tab=0;

static float hue=0;

static bool cfg[10];
static bool hidden[10];

static bool kb_open=false;
static char pw[32]={0};
static bool unlocked=false;

static int fps=0,frames=0;
static double last_fps=0;

static float touch_x=0,touch_y=0;

static ImVec4 rgb()
{
    hue+=0.15f;
    if(hue>360) hue=0;
    float h6=hue/60,c=1,x=c*(1-fabs(fmod(h6,2)-1));
    float r=0,g=0,b=0;
    if(h6<1){r=c;g=x;}
    else if(h6<2){r=x;g=c;}
    else if(h6<3){g=c;b=x;}
    else if(h6<4){g=x;b=c;}
    else if(h6<5){r=x;b=c;}
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
    c[ImGuiCol_Button]=ImVec4(0.15f,0.15f,0.15f,1);
    c[ImGuiCol_ButtonHovered]=a;
    c[ImGuiCol_ButtonActive]=a;
    c[ImGuiCol_CheckMark]=a;
    c[ImGuiCol_SliderGrab]=a;
    c[ImGuiCol_SliderGrabActive]=a;
    s.WindowBorderSize=2;
    s.WindowRounding=14;
    s.FrameRounding=10;
}

static void keyboard()
{
    if(!kb_open) return;
    ImGui::SetNextWindowSize(ImVec2(720,420));
    ImGui::Begin("Keyboard",0,ImGuiWindowFlags_NoCollapse);
    const char* k="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for(int i=0;i<36;i++)
    {
        char b[2]={k[i],0};
        if(ImGui::Button(b,ImVec2(60,60)))
        {
            int l=strlen(pw);
            if(l<31){ pw[l]=k[i]; pw[l+1]=0; }
        }
        if((i+1)%7!=0) ImGui::SameLine();
    }
    if(ImGui::Button("DEL",ImVec2(200,60)))
    {
        int l=strlen(pw);
        if(l>0) pw[l-1]=0;
    }
    ImGui::SameLine();
    if(ImGui::Button("CLEAR",ImVec2(200,60))) pw[0]=0;
    ImGui::SameLine();
    if(ImGui::Button("CLOSE",ImVec2(200,60))) kb_open=false;
    ImGui::End();
}

static int fake_ping(){ return 45; }
static int fake_battery(){ return 87; }

static void overlay()
{
    ImGui::SetNextWindowPos(ImVec2(w-360,20));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::Begin("Overlay",0,ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoTitleBar);
    if(cfg[0]) ImGui::Text("Ping: %d ms",fake_ping());
    if(cfg[1]) ImGui::Text("FPS: %d",fps);
    if(cfg[2]) ImGui::Text("Time: %02d:%02d:%02d",tm.tm_hour,tm.tm_min,tm.tm_sec);
    if(cfg[3]) ImGui::Text("Battery: %d%%",fake_battery());
    if(cfg[4]) ImGui::Text("Resolution: %dx%d",w,h);
    if(cfg[5]) ImGui::Text("Touch X: %.1f",touch_x);
    if(cfg[6]) ImGui::Text("Touch Y: %.1f",touch_y);
    if(cfg[7]) ImGui::Text("Aspect: %.2f",(float)w/(float)h);
    if(cfg[8]) ImGui::Text("DPI Scale: %.2f",ImGui::GetIO().FontGlobalScale);
    if(cfg[9]) ImGui::Text("RGB Hue: %.1f",hue);
    ImGui::End();

    if(unlocked)
    {
        ImGui::SetNextWindowPos(ImVec2(20,h-320));
        ImGui::Begin("Hidden",0,ImGuiWindowFlags_NoResize);
        for(int i=0;i<10;i++)
        {
            if(hidden[i]) ImGui::Text("Hidden Overlay %d Active",i+1);
        }
        ImGui::End();
    }
}

static void intro()
{
    ImGui::SetNextWindowSize(ImVec2(720,420));
    ImGui::SetNextWindowPos(ImVec2(w/2,h/2),0,ImVec2(0.5f,0.5f));
    ImGui::Begin("Intro",0,ImGuiWindowFlags_NoResize);
    ImGui::SetWindowFontScale(1.6f);
    ImGui::Text("Shortcut Plugin");
    ImGui::Separator();
    ImGui::TextColored(rgb(),"████████████████████████████");
    ImGui::Text("Version: V1.0");
    ImGui::Text("Development: MCPE OVER");
    ImGui::Text("Launcher: Light, Levi Launcher");
    ImGui::End();
    intro_t+=ImGui::GetIO().DeltaTime;
    if(intro_t>3.5f) show_intro=false;
}

static void config_tab()
{
    ImGui::SetWindowFontScale(1.3f);
    ImGui::Text("Overlay Features");
    ImGui::Separator();
    const char* names[10]={
        "Ping","FPS","Clock","Battery",
        "Resolution","Touch X","Touch Y",
        "Aspect Ratio","DPI Scale","RGB Info"
    };
    for(int i=0;i<10;i++) ImGui::Checkbox(names[i],&cfg[i]);
}

static void sorry_tab()
{
    ImGui::SetWindowFontScale(1.3f);
    if(!unlocked)
    {
        ImGui::Text("Protected Area");
        ImGui::InputText("##pw",pw,sizeof(pw));
        ImGui::SameLine();
        if(ImGui::Button("Keyboard",ImVec2(160,50))) kb_open=true;
        if(ImGui::Button("Unlock",ImVec2(260,60)))
            if(strcmp(pw,"MCMROVER")==0) unlocked=true;
    }
    else
    {
        ImGui::Text("Hidden Overlays");
        ImGui::Separator();
        for(int i=0;i<10;i++)
        {
            std::string n="Hidden Overlay "+std::to_string(i+1);
            ImGui::Checkbox(n.c_str(),&hidden[i]);
        }
    }
}

static void ui()
{
    ImGui::SetNextWindowSize(ImVec2(900,700),ImGuiCond_FirstUseEver);
    ImGui::Begin("Main");
    ImGui::BeginChild("left",ImVec2(160,0),true);
    if(ImGui::Button("Config",ImVec2(-1,80))) tab=0;
    if(ImGui::Button("Sorry",ImVec2(-1,80))) tab=1;
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("right",ImVec2(0,0),true);
    if(tab==0) config_tab();
    if(tab==1) sorry_tab();
    ImGui::EndChild();
    ImGui::End();
    keyboard();
    overlay();
}

static int32_t hook_input_func(void*a,void*b,bool c,long d,uint32_t*e,AInputEvent**ev)
{
    int32_t r=orig_input?orig_input(a,b,c,d,e,ev):0;
    if(ev&&*ev)
    {
        if(AInputEvent_getType(*ev)==AINPUT_EVENT_TYPE_MOTION)
        {
            touch_x=AMotionEvent_getX(*ev,0);
            touch_y=AMotionEvent_getY(*ev,0);
        }
        if(inited) ImGui_ImplAndroid_HandleInputEvent(*ev);
    }
    return r;
}

static void setup()
{
    if(inited) return;
    ImGui::CreateContext();
    ImGuiIO&io=ImGui::GetIO();
    io.FontGlobalScale=1.6f;
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    last_fps=ImGui::GetTime();
    inited=true;
}

static void render()
{
    double t=ImGui::GetTime();
    frames++;
    if(t-last_fps>=1.0){ fps=frames; frames=0; last_fps=t; }

    theme();
    ImGuiIO&io=ImGui::GetIO();
    io.DisplaySize=ImVec2(w,h);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(w,h);
    ImGui::NewFrame();
    if(show_intro) intro(); else ui();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

static EGLBoolean hook_swap_func(EGLDisplay d,EGLSurface s)
{
    eglQuerySurface(d,s,EGL_WIDTH,&w);
    eglQuerySurface(d,s,EGL_HEIGHT,&h);
    setup();
    render();
    return orig_swap(d,s);
}

static void* thread(void*)
{
    sleep(3);
    GlossInit(true);
    GHandle egl=GlossOpen("libEGL.so");
    GlossHook((void*)GlossSymbol(egl,"eglSwapBuffers",0),(void*)hook_swap_func,(void**)&orig_swap);
    GHandle inp=GlossOpen("libinput.so");
    GlossHook((void*)GlossSymbol(inp,"_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE",0),(void*)hook_input_func,(void**)&orig_input);
    return 0;
}

__attribute__((constructor))
void init()
{
    pthread_t t;
    pthread_create(&t,0,thread,0);
}
