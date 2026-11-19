#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "dobby.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "gillamod", __VA_ARGS__)
#define VIS extern "C" __attribute__((visibility("default")))
#define DIR "/sdcard/ModData/com.playgilla.gilla/gilla_mod"

static const char* PKG = "com.playgilla.gilla";
static JavaVM* g_vm = nullptr;
static bool g_ready = false;
static std::string g_ver = "?", g_banner;

// ---------- il2cpp ----------
namespace il2 {
void *h = nullptr;
void*  (*domain_get)();
void** (*domain_asms)(void*, size_t*);
void*  (*asm_image)(void*);
void*  (*class_from_name)(void*, const char*, const char*);
void*  (*method_from_name)(void*, const char*, int);
void*  (*field_from_name)(void*, const char*);
size_t (*field_offset)(void*);
void*  (*invoke)(void*, void*, void**, void**);
size_t (*class_count)();
void*  (*class_at)(size_t);
const char* (*class_name)(void*);
const char* (*class_ns)(void*);
void*  (*object_new)(void*);
void*  (*thread_attach)(void*);

bool init() {
  h = dlopen("libil2cpp.so", RTLD_NOW);
  if (!h) return false;
  #define R(n) *(void**)&n = dlsym(h, #n)
  R(domain_get); R(domain_asms); R(asm_image); R(class_from_name);
  R(method_from_name); R(field_from_name); R(field_offset); R(invoke);
  R(class_count); R(class_at); R(class_name); R(class_ns); R(thread_attach);
  #undef R
  return domain_get && class_from_name && invoke;
}

void* klass(const char* ns, const char* name) {
  size_t n = 0; void** a = domain_asms ? domain_asms(domain_get(), &n) : nullptr;
  for (size_t i = 0; a && i < n; ++i)
    if (void* k = class_from_name(asm_image(a[i]), ns, name)) return k;
  return nullptr;
}

// field of a class by name, returns nullptr if missing
void* field(void* k, const char* n) { return k && field_from_name ? field_from_name(k, n) : nullptr; }
size_t off(void* f) { return f ? field_offset(f) : 0; }

bool call(void* m, void* self, void** args) {
  if (!m) return false;
  void* e = nullptr; invoke(m, self, args, &e); return e == nullptr;
}

// UnityEngine helpers
void* cam_main() {
  void* k = klass("UnityEngine", "Camera"); if (!k) return nullptr;
  void** a = nullptr; void* r = nullptr;
  void* m = method_from_name(k, "get_main", 0);
  if (m) { void* e = nullptr; r = invoke(m, nullptr, a, &e); }
  return r;
}
bool set_prop(const char* cls, const char* get, const char* set, float v) {
  void* k = klass("UnityEngine", cls); if (!k) return false;
  void* o = cam_main(); if (!o) return false;
  void* m = method_from_name(k, set, 1); if (!m) return false;
  void* a[1] = { &v }; return call(m, o, a);
}

// ---------- config ----------
struct Cfg { bool on=false, fov=false, fog=false; float fovv=90, ts=1.0f; } cfg;

void save() {
  mkdir("/sdcard/ModData", 0777); mkdir("/sdcard/ModData/com.playgilla.gilla", 0777);
  mkdir(DIR, 0777);
  FILE* f = fopen(DIR "/config.ini", "w"); if (!f) return;
  fprintf(f, "on=%d\nfov=%d\nfovv=%.1f\ntimescale=%.2f\n", cfg.on, cfg.fov, cfg.fovv, cfg.ts);
  fclose(f);
}
void load() {
  FILE* f = fopen(DIR "/config.ini", "r"); if (!f) return;
  char k[32], v[32];
  while (fscanf(f, "%31[^=]=%31s\n", k, v) == 2) {
    if (!strcmp(k, "on")) cfg.on = atoi(v);
    else if (!strcmp(k, "fov")) cfg.fov = atoi(v);
    else if (!strcmp(k, "fovv")) cfg.fovv = atof(v);
    else if (!strcmp(k, "timescale")) cfg.ts = atof(v);
  }
  fclose(f);
}

// ---------- compat ----------
void probe() {
  JNIEnv* env = nullptr; if (!g_vm) return;
  g_vm->AttachCurrentThread(&env, nullptr);
  jclass pm = env->FindClass("android/content/pm/PackageManager");
  jobject ctx = nullptr;
  // version string via PackageManager
  jclass at = env->FindClass("android/app/ActivityThread");
  jmethodID cur = env->GetStaticMethodID(at, "currentApplication", "()Landroid/app/Application;");
  if (cur) ctx = env->CallStaticObjectMethod(at, cur);
  if (ctx) {
    jmethodID gp = env->GetMethodID(env->GetObjectClass(ctx), "getPackageManager",
                                    "()Landroid/content/pm/PackageManager;");
    jobject pmgr = env->CallObjectMethod(ctx, gp);
    jmethodID gi = env->GetMethodID(pm, "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
    jstring js = env->NewStringUTF(PKG);
    jobject pi = env->CallObjectMethod(pmgr, gi, js, 0);
    if (pi) {
      jfieldID vf = env->GetFieldID(env->GetObjectClass(pi), "versionName", "Ljava/lang/String;");
      jstring vn = (jstring)env->GetObjectField(pi, vf);
      const char* c = env->GetStringUTFChars(vn, nullptr);
      g_ver = c; env->ReleaseStringUTFChars(vn, c);
    }
  }
  if (g_ver != "0.1.0") g_banner = "Untested build " + g_ver;
  g_vm->DetachCurrentThread();
}

// ---------- dump ----------
void dump() {
  mkdir("/sdcard/ModData", 0777); mkdir(DIR, 0777);
  FILE* f = fopen(DIR "/dump.txt", "w"); if (!f) return;
  size_t n = class_count ? class_count() : 0;
  for (size_t i = 0; i < n; ++i) {
    void* k = class_at(i); if (!k) continue;
    const char* ns = class_ns(k); const char* nm = class_name(k);
    if (nm) fprintf(f, "%s::%s\n", ns ? ns : "", nm);
  }
  fclose(f);
  g_banner = "dumped " + std::to_string(n) + " classes";
}

// ---------- ui ----------
static const char* CATS[] = { "Visuals", "Movement", "Player", "Monster", "Level", "Config" };
static int cat = 0, sel = 0;
static float anim = 0.f, rows = 0.f;
static bool open = false;

static void theme() {
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 14; s.FrameRounding = 8; s.GrabRounding = 8;
  s.WindowPadding = ImVec2(18, 16); s.ItemSpacing = ImVec2(10, 10);
  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.05f, 0.06f, 0.96f);
  c[ImGuiCol_Border] = ImVec4(0.78f, 0.10f, 0.14f, 0.55f);
  c[ImGuiCol_Text] = ImVec4(0.95f, 0.95f, 0.96f, 1);
  c[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.45f, 0.48f, 1);
  c[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.12f, 0.14f, 1);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.12f, 0.13f, 1);
  c[ImGuiCol_CheckMark] = ImVec4(0.92f, 0.15f, 0.19f, 1);
  c[ImGuiCol_SliderGrab] = ImVec4(0.92f, 0.15f, 0.19f, 1);
  c[ImGuiCol_Header] = ImVec4(0.55f, 0.07f, 0.10f, 0.85f);
  c[ImGuiCol_Separator] = ImVec4(0.78f, 0.10f, 0.14f, 0.35f);
}

static void toggle(const char* label, bool* v, const char* hint = nullptr) {
  ImGui::PushID(label);
  bool hov = ImGui::IsItemHovered() || rows == sel;
  ImGui::TextColored(hov ? ImVec4(1, .3f, .33f, 1) : ImVec4(.9f, .9f, .9f, 1), "%s", label);
  ImGui::SameLine(300);
  ImGui::Checkbox("##v", v);
  if (hint) { ImGui::SameLine(); ImGui::TextDisabled("%s", hint); }
  ImGui::PopID();
  rows++;
}

static void draw() {
  ImGui::SetNextWindowSize(ImVec2(560, 380), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(120, 90), ImGuiCond_Always);
  ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
               | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
  ImGui::TextColored(ImVec4(.92f, .15f, .19f, 1), "GILLA MOD");
  ImGui::SameLine();
  ImGui::TextDisabled("v0.1  build %s", g_ver.c_str());
  if (!g_banner.empty()) ImGui::TextColored(ImVec4(1, .7f, .2f, 1), "%s", g_banner.c_str());
  ImGui::Separator();

  ImGui::BeginChild("##side", ImVec2(130, 0), true);
  for (int i = 0; i < IM_ARRAYSIZE(CATS); ++i)
    if (ImGui::Selectable(CATS[i], cat == i)) { cat = i; sel = 0; }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("##body", ImVec2(0, 0), false);

  rows = 0;
  if (cat == 0) {
    toggle("Field of view", &cfg.fov);
    ImGui::SliderFloat("##fov", &cfg.fovv, 60, 130, "%.0f");
    if (ImGui::Button("Reset FOV")) cfg.fovv = 90;
  } else if (cat == 1) {
    ImGui::SliderFloat("Time scale", &cfg.ts, 0.2f, 4.f, "%.2f");
  } else if (cat == 2 || cat == 3 || cat == 4) {
    ImGui::TextDisabled("name-resolution targets resolve at runtime");
    if (cat == 4 && ImGui::Button("Dump classes")) dump();
  } else {
    if (ImGui::Button("Save config")) save();
    ImGui::SameLine();
    if (ImGui::Button("Reload")) load();
  }
  ImGui::EndChild();
  ImGui::End();
}

// ---------- input ----------
static void* ovr_get_down(int btn) {
  void* k = klass("", "OVRInput"); if (!k) return nullptr;
  void* m = method_from_name(k, "GetDown", 1); return m;
}
static bool pressed(int btn) {
  void* m = ovr_get_down(btn); if (!m) return false;
  int b = btn; void* a[1] = { &b }; void* e = nullptr;
  void* r = il2::invoke(m, nullptr, a, &e); return r && *(bool*)((char*)r + 16);
}
enum { BTN_UP = 0x00000400, BTN_A = 0x00000001, BTN_B = 0x00000002 };

static void nav() {
  static auto last = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  if (std::chrono::duration<float>(now - last).count() < 0.18f) return;
  if (pressed(BTN_UP)) { sel = (sel + 1) % 6; last = now; }
  if (pressed(BTN_A)) { if (sel == 0) cfg.fov = !cfg.fov; last = now; }
  if (pressed(BTN_B)) { open = !open; last = now; }
}

// ---------- render ----------
static void frame() {
  if (!g_ready) return;
  GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
  anim += ((open ? 1.f : 0.f) - anim) * 0.15f;
  if (anim < 0.01f) return;

  nav();
  if (cfg.fov) set_prop("Camera", "get_fieldOfView", "set_fieldOfView", cfg.fovv);

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2((float)vp[2], (float)vp[3]);
  ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, anim);
  draw();
  ImGui::PopStyleVar();
  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

static EGLBoolean (*orig_swap)(EGLDisplay, EGLSurface);
static EGLBoolean swap_hook(EGLDisplay d, EGLSurface s) {
  frame();
  return orig_swap(d, s);
}

// ---------- entry ----------
static void boot() {
  load(); probe();
  if (!il2::init()) { g_banner = "libil2cpp.so missing"; return; }
  il2::thread_attach(il2::domain_get());
  theme();
  IMGUI_CHECKVERSION(); ImGui::CreateContext();
  ImGui_ImplOpenGL3_Init("#version 300 es");
  void* p = dlsym(dlopen("libEGL.so", RTLD_NOW), "eglSwapBuffers");
  DobbyHook(p, (void*)swap_hook, (void**)&orig_swap);
  g_ready = true;
}

VIS void modloader_load(void*) { boot(); }
VIS void modloader_unload(void*) { if (g_ready) save(); }
VIS jint JNI_OnLoad(JavaVM* vm, void*) { g_vm = vm; boot(); return JNI_VERSION_1_6; }
