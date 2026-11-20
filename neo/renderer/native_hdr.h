// Native Windows HDR presentation. Other platforms retain the original renderer.
#ifndef DHEWM3_NATIVE_HDR_H
#define DHEWM3_NATIVE_HDR_H

#if defined(_WIN32) && !defined(ID_DEDICATED)
bool HDR_Requested();
bool HDR_Active();
void HDR_BeginFrame();
void HDR_BeforeView(bool scene, bool subview);
bool HDR_Present();
void HDR_Shutdown();
void HDR_DebugStage(const char* label);
#else
inline bool HDR_Requested() { return false; }
inline bool HDR_Active() { return false; }
inline void HDR_BeginFrame() {}
inline void HDR_BeforeView(bool, bool) {}
inline bool HDR_Present() { return false; }
inline void HDR_Shutdown() {}
inline void HDR_DebugStage(const char*) {}
#endif

#endif
