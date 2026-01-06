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

static int tab=0;
static float hue=0.0f;

static bool cfg[10]={0};
static bool hidden[10]={0};

static bool kb_open=false;
static char pw[32]={0};
static bool unlocked=false;

static int fps=0,frames=0;
static double last_fps=0.0;

static float touch_x=0.0f,touch_y=0.0f;

static ImVec4 rgb()
{
    hue+=0.25f;
    if(hue>=360.0f) hue=0.0f;
    float h6=hue/60.0f;
    float c=1.0f;
    float x=c*(1.0f-fabsf(fmodf(h6,2.0f)-1.0f));
    float r=0,g=0,b=0;
    if(h6<1){r=c;g=x;}
    else if(h6<2){r=x;g=c;}
    else if(h6<3){g=c;b=x;}
    else if(h6<4){g=x;b=c;}
    else if(h6<5){r=x;b=c;}
    else{r=c;b=x;}
    return ImVec4(r,g,b,1.0f);
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
    s.WindowBorderSize=3;
    s.WindowRounding=16;
    s.FrameRounding=12;
}

static void draw_box(const char* title,const char* text,float x,float y)
{
    ImVec4 a=rgb();
    ImGui::SetNextWindowPos(ImVec2(x,y));
    ImGui::SetNextWindowBgAlpha(0.5f);
    ImGui::PushStyleColor(ImGuiCol_Border,a);
    ImGui::Begin(title,0,
        ImGuiWindowFlags_NoTitleBar|
        ImGuiWindowFlags_NoResize|
        ImGuiWindowFlags_NoInputs);
    ImGui::SetWindowFontScale(1.3f);
    ImGui::Text("%s",text);
    ImGui::End();
    ImGui::PopStyleColor();
}

static int fake_ping(){ return 45; }
static int fake_battery(){ return 87; }

static void overlay()
{
    time_t now=time(0);
    struct tm ti;
    localtime_r(&now,&ti);

    float y=20;

    if(cfg[0]){ draw_box("p","Ping: 45 ms",20,y); y+=90; }
    if(cfg[1]){ char b[32]; sprintf(b,"FPS: %d",fps); draw_box("f",b,20,y); y+=90; }
    if(cfg[2]){ char b[32]; sprintf(b,"Time: %02d:%02d:%02d",ti.tm_hour,ti.tm_min,ti.tm_sec); draw_box("t",b,20,y); y+=90; }
    if(cfg[3]){ draw_box("b","Battery: 87%",20,y); y+=90; }
    if(cfg[4]){ char b[32]; sprintf(b,"Res: %dx%d",w,h); draw_box("r",b,20,y); y+=90; }
    if(cfg[5]){ char b[32]; sprintf(b,"Touch X: %.0f",touch_x); draw_box("x",b,20,y); y+=90; }
    if(cfg[6]){ char b[32]; sprintf(b,"Touch Y: %.0f",touch_y); draw_box("y",b,20,y); y+=90; }
    if(cfg[7]){ char b[32]; sprintf(b,"Aspect: %.2f",(float)w/h); draw_box("a",b,20,y); y+=90; }
    if(cfg[8]){ char b[32]; sprintf(b,"Scale: %.2f",ImGui::GetIO().FontGlobalScale); draw_box("s",b,20,y); y+=90; }
    if(cfg[9]){ char b[32]; sprintf(b,"Hue: %.1f",hue); draw_box("h",b,20,y); }

    if(unlocked)
    {
        for(int i=0;i<10;i++)
            if(hidden[i])
                draw_box("hid","Hidden Overlay Active",w-360,60+i*70);
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
    ImGui::Text("Version: V1.0");
    ImGui::Text("Development: MCPE OVER");
    ImGui::Text("Launcher: Light, Levi Launcher");

    ImGui::SetCursorPosY(ImGui::GetWindowSize().y-90);
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x-200)/2);
    if(ImGui::Button("I read it",ImVec2(200,60)))
        show_intro=false;

    ImGui::End();
}

static void config_tab()
{
    const char* n[10]={
        "Ping","FPS","Clock","Battery",
        "Resolution","Touch X","Touch Y",
        "Aspect Ratio","DPI Scale","RGB Hue"
    };
    for(int i=0;i<10;i++) ImGui::Checkbox(n[i],&cfg[i]);
}

static void sorry_tab()
{
    if(!unlocked)
    {
        ImGui::InputText("##pw",pw,sizeof(pw));
        ImGui::SameLine();
        if(ImGui::Button("Keyboard")) kb_open=true;
        if(ImGui::Button("Unlock"))
            if(strcmp(pw,"MCMROVER")==0) unlocked=true;
    }
    else
        for(int i=0;i<10;i++)
            ImGui::Checkbox(("Hidden "+std::to_string(i+1)).c_str(),&hidden[i]);
}

static void ui()
{
    ImGui::SetNextWindowSize(ImVec2(900,700),ImGuiCond_FirstUseEver);
    ImGui::Begin("Main");
    ImGui::BeginChild("left",ImVec2(180,0),true);
    if(ImGui::Button("Config",ImVec2(-1,80))) tab=0;
    if(ImGui::Button("Sorry",ImVec2(-1,80))) tab=1;
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("right",ImVec2(0,0),true);
    if(tab==0) config_tab();
    if(tab==1) sorry_tab();
    ImGui::EndChild();
    ImGui::End();
    overlay();
}

static int32_t hook_input_func(void*a,void*b,bool c,long d,uint32_t*e,AInputEvent**ev)
{
    int32_t r=orig_input?orig_input(a,b,c,d,e,ev):0;
    if(ev&&*ev&&AInputEvent_getType(*ev)==AINPUT_EVENT_TYPE_MOTION)
    {
        touch_x=AMotionEvent_getX(*ev,0);
        touch_y=AMotionEvent_getY(*ev,0);
        if(inited) ImGui_ImplAndroid_HandleInputEvent(*ev);
    }
    return r;
}

static void setup()
{
    if(inited) return;
    ImGui::CreateContext();
    ImGuiIO&io=ImGui::GetIO();
    io.FontGlobalScale=1.5f;
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
    GlossHook((void*)GlossSymbol(inp,
        "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE",0),
        (void*)hook_input_func,(void**)&orig_input);
    return 0;
}

__attribute__((constructor))
void init()
{
    pthread_t t;
    pthread_create(&t,0,thread,0);
}
