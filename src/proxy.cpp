// SuperBLT native plugin for PAYDAY 2 on headsets with canted displays.
//
// Loaded by the mod's Lua (blt.load_native) after VR init. It takes the game's IVRSystem /
// IVRCompositor from the stock openvr_api.dll and patches a few vtable slots in place:
//   - log what the engine queries (eye-to-head, projection, render size, hidden area mesh, IPD)
//   - on Submit, attach a render pose that undoes each eye's cant rotation, so the compositor
//     reprojects a parallel-rendered eye onto the canted display.
//
// Config: openvr_proxy.ini beside PAYDAY2.exe. Log: openvr_proxy.log.
// Hotkeys (polled each frame): Ctrl+F9 toggle fix, Ctrl+F10 / Ctrl+F11 cant scale -/+ 0.1,
// Ctrl+F8 write a snapshot of the current state to the log.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <mutex>
#include "openvr.h"

using namespace vr;

// ---------------------------------------------------------------------------------------------
// logging and config

static FILE *g_log = nullptr;
static std::mutex g_logMutex;
static char g_dir[MAX_PATH];

static void Log(const char *fmt, ...)
{
	std::lock_guard<std::mutex> lock(g_logMutex);
	if (!g_log)
		return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(g_log, fmt, ap);
	va_end(ap);
	fputc('\n', g_log);
	fflush(g_log);
}

struct Config
{
	bool fix = true;          // attach cant-compensating pose on Submit
	float cantScale = 1.0f;   // multiplier on the compensated cant angle (1 = full, -1 = inverted)
	// Per-eye image shift via the submitted texture bounds, in percent of the eye's image. Each eye
	// moves half of it in opposite directions; positive moves the right eye's sample window up/right.
	float verticalPct = -0.75f;   // tuned in-headset on a Steam Frame (reported as Quest 2, 0 deg cant)
	float horizontalPct = -0.75f;
	bool hotkeys = true;
	int logCalls = 8;         // how many calls of each hooked function to log
};
static Config g_cfg;

static void IniPath(char *out)
{
	snprintf(out, MAX_PATH, "%s\\openvr_proxy.ini", g_dir);
}

static void LoadConfig()
{
	char path[MAX_PATH];
	IniPath(path);
	if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
	{
		WritePrivateProfileStringA("fix", "enabled", "1", path);
		WritePrivateProfileStringA("fix", "cant_scale", "1.0", path);
		WritePrivateProfileStringA("fix", "vertical_pct", "-0.75", path);
		WritePrivateProfileStringA("fix", "horizontal_pct", "-0.75", path);
		WritePrivateProfileStringA("fix", "hotkeys", "1", path);
		WritePrivateProfileStringA("log", "calls_per_function", "8", path);
	}
	char buf[64];
	g_cfg.fix = GetPrivateProfileIntA("fix", "enabled", 1, path) != 0;
	GetPrivateProfileStringA("fix", "cant_scale", "1.0", buf, sizeof(buf), path);
	g_cfg.cantScale = (float)atof(buf);
	GetPrivateProfileStringA("fix", "vertical_pct", "-0.75", buf, sizeof(buf), path);
	g_cfg.verticalPct = (float)atof(buf);
	GetPrivateProfileStringA("fix", "horizontal_pct", "-0.75", buf, sizeof(buf), path);
	g_cfg.horizontalPct = (float)atof(buf);
	g_cfg.hotkeys = GetPrivateProfileIntA("fix", "hotkeys", 1, path) != 0;
	g_cfg.logCalls = GetPrivateProfileIntA("log", "calls_per_function", 8, path);
}

static void SaveConfig()
{
	char path[MAX_PATH], buf[64];
	IniPath(path);
	WritePrivateProfileStringA("fix", "enabled", g_cfg.fix ? "1" : "0", path);
	snprintf(buf, sizeof(buf), "%.2f", g_cfg.cantScale);
	WritePrivateProfileStringA("fix", "cant_scale", buf, path);
	snprintf(buf, sizeof(buf), "%.2f", g_cfg.verticalPct);
	WritePrivateProfileStringA("fix", "vertical_pct", buf, path);
	snprintf(buf, sizeof(buf), "%.2f", g_cfg.horizontalPct);
	WritePrivateProfileStringA("fix", "horizontal_pct", buf, path);
}

// ---------------------------------------------------------------------------------------------
// math

struct M3
{
	float m[3][3];
};

static M3 RotOf(const HmdMatrix34_t &a)
{
	M3 r;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			r.m[i][j] = a.m[i][j];
	return r;
}

// Scale the angle of a rotation matrix (axis-angle), keeping the axis.
static M3 ScaleRotation(const M3 &r, float s)
{
	float tr = r.m[0][0] + r.m[1][1] + r.m[2][2];
	float c = (tr - 1.0f) * 0.5f;
	if (c > 1.0f) c = 1.0f;
	if (c < -1.0f) c = -1.0f;
	float angle = acosf(c);
	M3 out = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
	if (angle < 1e-6f)
		return out;
	float x = r.m[2][1] - r.m[1][2], y = r.m[0][2] - r.m[2][0], z = r.m[1][0] - r.m[0][1];
	float len = sqrtf(x * x + y * y + z * z);
	if (len < 1e-9f)
		return out;
	x /= len; y /= len; z /= len;
	float a = angle * s, ca = cosf(a), sa = sinf(a), t = 1.0f - ca;
	out.m[0][0] = t * x * x + ca;     out.m[0][1] = t * x * y - sa * z; out.m[0][2] = t * x * z + sa * y;
	out.m[1][0] = t * x * y + sa * z; out.m[1][1] = t * y * y + ca;     out.m[1][2] = t * y * z - sa * x;
	out.m[2][0] = t * x * z - sa * y; out.m[2][1] = t * y * z + sa * x; out.m[2][2] = t * z * z + ca;
	return out;
}

// a * [b^T | 0]: post-multiply a pose by the inverse of rotation b.
static HmdMatrix34_t MulInvRot(const HmdMatrix34_t &a, const M3 &b)
{
	HmdMatrix34_t o;
	for (int i = 0; i < 3; i++)
	{
		for (int j = 0; j < 3; j++)
			o.m[i][j] = a.m[i][0] * b.m[j][0] + a.m[i][1] * b.m[j][1] + a.m[i][2] * b.m[j][2];
		o.m[i][3] = a.m[i][3];
	}
	return o;
}

static float AngleDeg(const M3 &r)
{
	float c = (r.m[0][0] + r.m[1][1] + r.m[2][2] - 1.0f) * 0.5f;
	if (c > 1.0f) c = 1.0f;
	if (c < -1.0f) c = -1.0f;
	return acosf(c) * 57.2957795f;
}

static void LogM34(const char *tag, const HmdMatrix34_t &m)
{
	Log("%s [%.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f]", tag,
		m.m[0][0], m.m[0][1], m.m[0][2], m.m[0][3], m.m[1][0], m.m[1][1], m.m[1][2], m.m[1][3],
		m.m[2][0], m.m[2][1], m.m[2][2], m.m[2][3]);
}

static void LogM44(const char *tag, const HmdMatrix44_t &m)
{
	Log("%s [%.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f | %.5f %.5f %.5f %.5f]",
		tag, m.m[0][0], m.m[0][1], m.m[0][2], m.m[0][3], m.m[1][0], m.m[1][1], m.m[1][2], m.m[1][3],
		m.m[2][0], m.m[2][1], m.m[2][2], m.m[2][3], m.m[3][0], m.m[3][1], m.m[3][2], m.m[3][3]);
}

// ---------------------------------------------------------------------------------------------
// state

static IVRSystem *g_sys = nullptr;
static HmdMatrix34_t g_eyeToHead[2];
static bool g_haveEyes = false;
static HmdMatrix34_t g_renderPose;    // HMD pose from the last WaitGetPoses
static bool g_haveRenderPose = false;
static int g_frame = 0;

static bool ShouldLog(int &counter)
{
	return counter++ < g_cfg.logCalls;
}

// ---------------------------------------------------------------------------------------------
// vtable hooks. Member functions keep the MSVC member ABI (this in rcx, hidden return pointer
// after it), so the hook signatures match the real interface methods exactly.

template <typename T> static T FromPtr(void *p)
{
	T t;
	memcpy(&t, &p, sizeof(p));
	return t;
}
template <typename T> static void *ToPtr(T t)
{
	void *p;
	memcpy(&p, &t, sizeof(p));
	return p;
}

static void *PatchSlot(void *obj, int index, void *fn)
{
	void **vtbl = *(void ***)obj;
	void *old = vtbl[index];
	if (old == fn)
		return nullptr;
	DWORD prot;
	VirtualProtect(&vtbl[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &prot);
	vtbl[index] = fn;
	VirtualProtect(&vtbl[index], sizeof(void *), prot, &prot);
	return old;
}

// IVRSystem_026 slots
enum { kSys_GetRecommendedRenderTargetSize = 0, kSys_GetProjectionMatrix = 1, kSys_GetProjectionRaw = 2,
	kSys_GetEyeToHeadTransform = 5, kSys_GetFloatTrackedDeviceProperty = 23, kSys_GetHiddenAreaMesh = 34 };
// IVRCompositor_029 slots
enum { kComp_WaitGetPoses = 2, kComp_Submit = 6, kComp_SubmitWithArrayIndex = 7 };

struct Hooks
{
	void GetRecommendedRenderTargetSize(uint32_t *w, uint32_t *h);
	HmdMatrix44_t GetProjectionMatrix(EVREye eye, float zn, float zf);
	void GetProjectionRaw(EVREye eye, float *l, float *r, float *t, float *b);
	HmdMatrix34_t GetEyeToHeadTransform(EVREye eye);
	float GetFloatTrackedDeviceProperty(TrackedDeviceIndex_t dev, ETrackedDeviceProperty prop, ETrackedPropertyError *err);
	HiddenAreaMesh_t GetHiddenAreaMesh(EVREye eye, EHiddenAreaMeshType type);
	EVRCompositorError WaitGetPoses(TrackedDevicePose_t *render, uint32_t nRender, TrackedDevicePose_t *game, uint32_t nGame);
	EVRCompositorError Submit(EVREye eye, const Texture_t *tex, const VRTextureBounds_t *bounds, EVRSubmitFlags flags);
	EVRCompositorError SubmitWithArrayIndex(EVREye eye, const Texture_t *tex, uint32_t idx, const VRTextureBounds_t *bounds, EVRSubmitFlags flags);
};

typedef void (Hooks::*Fn_RenderSize)(uint32_t *, uint32_t *);
typedef HmdMatrix44_t (Hooks::*Fn_ProjMatrix)(EVREye, float, float);
typedef void (Hooks::*Fn_ProjRaw)(EVREye, float *, float *, float *, float *);
typedef HmdMatrix34_t (Hooks::*Fn_EyeToHead)(EVREye);
typedef float (Hooks::*Fn_FloatProp)(TrackedDeviceIndex_t, ETrackedDeviceProperty, ETrackedPropertyError *);
typedef HiddenAreaMesh_t (Hooks::*Fn_HiddenArea)(EVREye, EHiddenAreaMeshType);
typedef EVRCompositorError (Hooks::*Fn_WaitGetPoses)(TrackedDevicePose_t *, uint32_t, TrackedDevicePose_t *, uint32_t);
typedef EVRCompositorError (Hooks::*Fn_Submit)(EVREye, const Texture_t *, const VRTextureBounds_t *, EVRSubmitFlags);
typedef EVRCompositorError (Hooks::*Fn_SubmitArr)(EVREye, const Texture_t *, uint32_t, const VRTextureBounds_t *, EVRSubmitFlags);

static Fn_RenderSize o_RenderSize;
static Fn_ProjMatrix o_ProjMatrix;
static Fn_ProjRaw o_ProjRaw;
static Fn_EyeToHead o_EyeToHead;
static Fn_FloatProp o_FloatProp;
static Fn_HiddenArea o_HiddenArea;
static Fn_WaitGetPoses o_WaitGetPoses;
static Fn_Submit o_Submit;
static Fn_SubmitArr o_SubmitArr;

static int c_RenderSize, c_ProjMatrix, c_ProjRaw, c_EyeToHead, c_FloatProp, c_HiddenArea, c_Wait, c_Submit, c_SubmitArr;

static const char *EyeName(EVREye e)
{
	return e == Eye_Left ? "L" : "R";
}

void Hooks::GetRecommendedRenderTargetSize(uint32_t *w, uint32_t *h)
{
	(this->*o_RenderSize)(w, h);
	if (ShouldLog(c_RenderSize))
		Log("GetRecommendedRenderTargetSize -> %u x %u", w ? *w : 0, h ? *h : 0);
}

HmdMatrix44_t Hooks::GetProjectionMatrix(EVREye eye, float zn, float zf)
{
	HmdMatrix44_t m = (this->*o_ProjMatrix)(eye, zn, zf);
	if (ShouldLog(c_ProjMatrix))
	{
		char tag[96];
		snprintf(tag, sizeof(tag), "GetProjectionMatrix(%s, near=%.4f, far=%.1f) ->", EyeName(eye), zn, zf);
		LogM44(tag, m);
	}
	return m;
}

void Hooks::GetProjectionRaw(EVREye eye, float *l, float *r, float *t, float *b)
{
	(this->*o_ProjRaw)(eye, l, r, t, b);
	if (ShouldLog(c_ProjRaw))
		Log("GetProjectionRaw(%s) -> left=%.5f right=%.5f top=%.5f bottom=%.5f", EyeName(eye), *l, *r, *t, *b);
}

HmdMatrix34_t Hooks::GetEyeToHeadTransform(EVREye eye)
{
	HmdMatrix34_t m = (this->*o_EyeToHead)(eye);
	if (ShouldLog(c_EyeToHead))
	{
		char tag[96];
		snprintf(tag, sizeof(tag), "GetEyeToHeadTransform(%s) [cant %.3f deg] ->", EyeName(eye), AngleDeg(RotOf(m)));
		LogM34(tag, m);
	}
	return m;
}

float Hooks::GetFloatTrackedDeviceProperty(TrackedDeviceIndex_t dev, ETrackedDeviceProperty prop, ETrackedPropertyError *err)
{
	float v = (this->*o_FloatProp)(dev, prop, err);
	if (ShouldLog(c_FloatProp))
		Log("GetFloatTrackedDeviceProperty(dev=%u, prop=%d) -> %.5f", dev, (int)prop, v);
	return v;
}

HiddenAreaMesh_t Hooks::GetHiddenAreaMesh(EVREye eye, EHiddenAreaMeshType type)
{
	HiddenAreaMesh_t m = (this->*o_HiddenArea)(eye, type);
	if (ShouldLog(c_HiddenArea))
		Log("GetHiddenAreaMesh(%s, type=%d) -> %u triangles", EyeName(eye), (int)type, m.unTriangleCount);
	return m;
}

static void PollHotkeys()
{
	if (!g_cfg.hotkeys)
		return;
	// Ctrl+F9 toggle; Ctrl+F10/F11 vertical -/+; Ctrl+Shift+F10/F11 horizontal -/+;
	// Ctrl+Shift+F9 reset both offsets; Ctrl+F8 log a marker. Steps are 0.25 percent.
	static bool prev[4];
	bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
	bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
	const int keys[4] = {VK_F8, VK_F9, VK_F10, VK_F11};
	for (int i = 0; i < 4; i++)
	{
		bool down = ctrl && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
		if (down && !prev[i])
		{
			float &axis = shift ? g_cfg.horizontalPct : g_cfg.verticalPct;
			if (i == 1 && shift)
				g_cfg.verticalPct = g_cfg.horizontalPct = 0.0f;
			else if (i == 1)
				g_cfg.fix = !g_cfg.fix;
			else if (i == 2)
				axis -= 0.25f;
			else if (i == 3)
				axis += 0.25f;
			if (i != 0)
				SaveConfig();
			Log("hotkey: fix=%d vertical=%.2f%% horizontal=%.2f%% cant_scale=%.2f (frame %d)", g_cfg.fix ? 1 : 0,
				g_cfg.verticalPct, g_cfg.horizontalPct, g_cfg.cantScale, g_frame);
			MessageBeep(g_cfg.fix ? MB_OK : MB_ICONHAND);
		}
		prev[i] = down;
	}
}

EVRCompositorError Hooks::WaitGetPoses(TrackedDevicePose_t *render, uint32_t nRender, TrackedDevicePose_t *game, uint32_t nGame)
{
	EVRCompositorError e = (this->*o_WaitGetPoses)(render, nRender, game, nGame);
	g_frame++;
	if (render && nRender > k_unTrackedDeviceIndex_Hmd && render[k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
	{
		g_renderPose = render[k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
		g_haveRenderPose = true;
	}
	if (ShouldLog(c_Wait))
		Log("WaitGetPoses(nRender=%u, nGame=%u) -> %d", nRender, nGame, (int)e);
	PollHotkeys();
	return e;
}

// Build the posed texture we hand to the compositor instead of the game's. False = submit as-is.
static bool BuildPosedTexture(EVREye eye, const Texture_t *tex, EVRSubmitFlags flags, VRTextureWithPoseAndDepth_t &out)
{
	if (!g_cfg.fix || !g_haveEyes || !tex)
		return false;
	if ((flags & 0x200) == 0x200) // Submit_TextureWithMotion: larger struct we don't copy
		return false;
	memset(&out, 0, sizeof(out));
	HmdMatrix34_t pose;
	if (flags & Submit_TextureWithPose)
	{
		const VRTextureWithPose_t *p = (const VRTextureWithPose_t *)tex;
		pose = p->mDeviceToAbsoluteTracking;
		if (flags & Submit_TextureWithDepth)
			out.depth = ((const VRTextureWithPoseAndDepth_t *)tex)->depth;
	}
	else
	{
		if (!g_haveRenderPose)
			return false;
		pose = g_renderPose;
		if (flags & Submit_TextureWithDepth)
			out.depth = ((const VRTextureWithDepth_t *)tex)->depth;
	}
	*(Texture_t *)&out = *tex;
	M3 cant = ScaleRotation(RotOf(g_eyeToHead[eye]), g_cfg.cantScale);
	out.mDeviceToAbsoluteTracking = MulInvRot(pose, cant);
	return true;
}

EVRCompositorError Hooks::Submit(EVREye eye, const Texture_t *tex, const VRTextureBounds_t *bounds, EVRSubmitFlags flags)
{
	bool log = ShouldLog(c_Submit);
	if (log)
	{
		if (bounds)
			Log("Submit(%s, type=%d, color=%d, flags=0x%x, bounds u=[%.4f %.4f] v=[%.4f %.4f]) frame %d", EyeName(eye),
				tex ? (int)tex->eType : -1, tex ? (int)tex->eColorSpace : -1, (unsigned)flags,
				bounds->uMin, bounds->uMax, bounds->vMin, bounds->vMax, g_frame);
		else
			Log("Submit(%s, type=%d, color=%d, flags=0x%x, no bounds) frame %d", EyeName(eye),
				tex ? (int)tex->eType : -1, tex ? (int)tex->eColorSpace : -1, (unsigned)flags, g_frame);
	}
	VRTextureWithPoseAndDepth_t posed;
	bool applied = BuildPosedTexture(eye, tex, flags, posed);
	// Shift this eye's sample window; the eyes move half each, in opposite directions.
	VRTextureBounds_t shifted = bounds ? *bounds : VRTextureBounds_t{0.0f, 0.0f, 1.0f, 1.0f};
	if (g_cfg.fix && (g_cfg.verticalPct != 0.0f || g_cfg.horizontalPct != 0.0f))
	{
		float side = eye == Eye_Right ? 0.5f : -0.5f;
		float du = side * g_cfg.horizontalPct * 0.01f * (shifted.uMax - shifted.uMin);
		float dv = side * g_cfg.verticalPct * 0.01f * (shifted.vMax - shifted.vMin);
		shifted.uMin -= du;
		shifted.uMax -= du;
		shifted.vMin += dv;
		shifted.vMax += dv;
		bounds = &shifted;
	}
	EVRCompositorError e = applied
		? (this->*o_Submit)(eye, (const Texture_t *)&posed, bounds, (EVRSubmitFlags)(flags | Submit_TextureWithPose))
		: (this->*o_Submit)(eye, tex, bounds, flags);
	static int errCount;
	if (log || (e != VRCompositorError_None && errCount++ < 20))
		Log("  Submit(%s) fix=%d -> %d", EyeName(eye), applied ? 1 : 0, (int)e);
	return e;
}

EVRCompositorError Hooks::SubmitWithArrayIndex(EVREye eye, const Texture_t *tex, uint32_t idx, const VRTextureBounds_t *bounds, EVRSubmitFlags flags)
{
	if (ShouldLog(c_SubmitArr))
		Log("SubmitWithArrayIndex(%s, idx=%u, flags=0x%x) - not compensated", EyeName(eye), idx, (unsigned)flags);
	return (this->*o_SubmitArr)(eye, tex, idx, bounds, flags);
}

template <typename T> static void Hook(void *obj, int slot, T hook, T &orig)
{
	void *old = PatchSlot(obj, slot, ToPtr(hook));
	if (old)
		orig = FromPtr<T>(old);
}

static void LogDeviceInfo(IVRSystem *sys)
{
	char model[256] = {0}, maker[256] = {0}, driver[256] = {0};
	sys->GetStringTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, Prop_ModelNumber_String, model, sizeof(model));
	sys->GetStringTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, Prop_ManufacturerName_String, maker, sizeof(maker));
	sys->GetStringTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, Prop_TrackingSystemName_String, driver, sizeof(driver));
	Log("HMD: %s / %s / driver %s, runtime %s", maker, model, driver, sys->GetRuntimeVersion());
	Log("HMD: IPD %.4f m, %.2f Hz",
		sys->GetFloatTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, Prop_UserIpdMeters_Float),
		sys->GetFloatTrackedDeviceProperty(k_unTrackedDeviceIndex_Hmd, Prop_DisplayFrequency_Float));
	for (int e = 0; e < 2; e++)
	{
		g_eyeToHead[e] = (((Hooks *)sys)->*o_EyeToHead)((EVREye)e);
		char tag[64];
		snprintf(tag, sizeof(tag), "real eye-to-head %s [cant %.3f deg]", e ? "R" : "L", AngleDeg(RotOf(g_eyeToHead[e])));
		LogM34(tag, g_eyeToHead[e]);
		float l, r, t, b;
		(((Hooks *)sys)->*o_ProjRaw)((EVREye)e, &l, &r, &t, &b);
		Log("real projection raw %s: left=%.5f right=%.5f top=%.5f bottom=%.5f", e ? "R" : "L", l, r, t, b);
	}
	uint32_t w = 0, h = 0;
	(((Hooks *)sys)->*o_RenderSize)(&w, &h);
	Log("real recommended render size %u x %u", w, h);
	g_haveEyes = true;
}

static void HookSystem(IVRSystem *sys)
{
	Hook(sys, kSys_GetRecommendedRenderTargetSize, &Hooks::GetRecommendedRenderTargetSize, o_RenderSize);
	Hook(sys, kSys_GetProjectionMatrix, &Hooks::GetProjectionMatrix, o_ProjMatrix);
	Hook(sys, kSys_GetProjectionRaw, &Hooks::GetProjectionRaw, o_ProjRaw);
	Hook(sys, kSys_GetEyeToHeadTransform, &Hooks::GetEyeToHeadTransform, o_EyeToHead);
	Hook(sys, kSys_GetFloatTrackedDeviceProperty, &Hooks::GetFloatTrackedDeviceProperty, o_FloatProp);
	Hook(sys, kSys_GetHiddenAreaMesh, &Hooks::GetHiddenAreaMesh, o_HiddenArea);
	if (!g_sys && o_EyeToHead)
	{
		g_sys = sys;
		LogDeviceInfo(sys);
	}
}

static void HookCompositor(IVRCompositor *comp)
{
	Hook(comp, kComp_WaitGetPoses, &Hooks::WaitGetPoses, o_WaitGetPoses);
	Hook(comp, kComp_Submit, &Hooks::Submit, o_Submit);
	Hook(comp, kComp_SubmitWithArrayIndex, &Hooks::SubmitWithArrayIndex, o_SubmitArr);
}

// ---------------------------------------------------------------------------------------------
// SuperBLT native plugin. PAYDAY2.exe hash-checks the DLLs it ships, so openvr_api.dll stays stock;
// a small BLT mod calls blt.load_native on this DLL, which by then runs after VR init. We fetch the
// game's existing IVRSystem / IVRCompositor from the loaded openvr_api.dll and patch their vtables.

typedef void *(VR_CALLTYPE *Fn_GetGenericInterface)(const char *, EVRInitError *);

static void Init()
{
	static bool done;
	if (done)
		return;
	done = true;
	char exe[MAX_PATH];
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
	strcpy(g_dir, exe);
	char *slash = strrchr(g_dir, '\\');
	if (slash)
		*slash = 0;
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s\\openvr_proxy.log", g_dir);
	g_log = fopen(path, "w");
	LoadConfig();
	Log("pd2-canted-fix plugin loaded into %s", exe);
	Log("config: fix=%d vertical=%.2f%% horizontal=%.2f%% cant_scale=%.2f hotkeys=%d", g_cfg.fix ? 1 : 0,
		g_cfg.verticalPct, g_cfg.horizontalPct, g_cfg.cantScale, g_cfg.hotkeys ? 1 : 0);
	HMODULE openvr = GetModuleHandleA("openvr_api.dll");
	Fn_GetGenericInterface get = openvr ? (Fn_GetGenericInterface)GetProcAddress(openvr, "VR_GetGenericInterface") : nullptr;
	if (!get)
	{
		Log("openvr_api.dll not loaded; nothing hooked");
		return;
	}
	EVRInitError e1 = VRInitError_None, e2 = VRInitError_None;
	void *sys = get(IVRSystem_Version, &e1);
	void *comp = get(IVRCompositor_Version, &e2);
	Log("%s -> %p err=%d, %s -> %p err=%d", IVRSystem_Version, sys, (int)e1, IVRCompositor_Version, comp, (int)e2);
	if (!sys || !comp)
	{
		Log("VR not initialised (desktop mode?); nothing hooked");
		return;
	}
	HookSystem((IVRSystem *)sys);
	HookCompositor((IVRCompositor *)comp);
	Log("hooks installed");
}

typedef void *(*lua_access_func_t)(const char *);

extern "C" __declspec(dllexport) const uint64_t SBLT_API_REVISION = 1;
extern "C" __declspec(dllexport) const char *const MODULE_LICENCE_DECLARATION =
	"This module is licenced under the GNU GPL version 2 or later, or another compatible licence";
extern "C" __declspec(dllexport) const char *const MODULE_SOURCE_CODE_LOCATION = "https://github.com/Lordmau5/pd2-canted-vr-fix";
extern "C" __declspec(dllexport) const char *const MODULE_SOURCE_CODE_REVISION = "0.2";

extern "C" __declspec(dllexport) void SuperBLT_Plugin_Setup(lua_access_func_t)
{
	Init();
}

extern "C" __declspec(dllexport) void SuperBLT_Plugin_Init_State(void *)
{
}

extern "C" __declspec(dllexport) int SuperBLT_Plugin_PushLua(void *)
{
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_DETACH && g_log)
	{
		Log("unloading");
		fclose(g_log);
		g_log = nullptr;
	}
	return TRUE;
}
