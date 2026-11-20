// Keep Windows' hidden-cursor SDR island away from the HDR crosshair.
// Uses SDL's own clip ownership so focus loss, menus and shutdown release it.
#ifndef DHEWM3_HDR_CURSOR_H
#define DHEWM3_HDR_CURSOR_H

#if defined(_WIN32) && SDL_VERSION_ATLEAST(2, 24, 0) && !SDL_VERSION_ATLEAST(3, 0, 0)
#include <windows.h>
#include <string>
#include <vector>

class NativeHDRCursor {
    bool parked = false;
    bool hadCenterHint = false;
    std::string centerHint;

public:
    // Returns true while this window owns a corner-parking request.
    bool Update(SDL_Window* window, bool requested) {
        const bool relative = SDL_GetRelativeMouseMode() == SDL_TRUE;
        const bool focused = window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS);
        const bool warp = SDL_GetHintBoolean(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, SDL_FALSE) == SDL_TRUE;
        if (!requested || !relative || !focused || warp) {
            if (parked) {
                SDL_SetWindowMouseRect(window, NULL);
                if (hadCenterHint) {
                    SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_MODE_CENTER, centerHint.c_str(), SDL_HINT_OVERRIDE);
                } else {
                    SDL_ResetHint(SDL_HINT_MOUSE_RELATIVE_MODE_CENTER);
                }
                parked = false;
            }
            return false;
        }

        // SDL can fall back to cursor warping if raw input registration fails.
        // Constraining that fallback to one pixel would break mouse-look.
        if (!parked) {
            UINT count = 0;
            if (GetRegisteredRawInputDevices(NULL, &count, sizeof(RAWINPUTDEVICE)) == UINT(-1) || !count) return false;
            std::vector<RAWINPUTDEVICE> devices(count);
            if (GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE)) == UINT(-1)) return false;
            bool rawMouse = false;
            for (UINT i = 0; i < count; ++i) {
                if (devices[i].usUsagePage == 1 && devices[i].usUsage == 2) rawMouse = true;
            }
            if (!rawMouse) return false;
        }

        int width = 0, height = 0;
        SDL_GetWindowSize(window, &width, &height);
        if (width <= 0 || height <= 0) return false;
        const bool entering = !parked;
        if (entering) {
            const char* hint = SDL_GetHint(SDL_HINT_MOUSE_RELATIVE_MODE_CENTER);
            hadCenterHint = hint != NULL;
            centerHint = hint ? hint : "";
            // Otherwise SDL's special relative-mode center clip takes priority
            // over the window mouse rectangle in WIN_UpdateClipCursor().
            if (!SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_MODE_CENTER, "0", SDL_HINT_OVERRIDE)) return false;
        }
        const SDL_Rect corner = {width - 1, height - 1, 1, 1};
        // Reapply even if unchanged: SDL can defer clipping during activation
        // clicks, and its grab setters otherwise early-return on later frames.
        if (SDL_SetWindowMouseRect(window, &corner) != 0) {
            parked = true;
            Update(window, false);
            return false;
        }
        parked = true;
        return true;
    }
};
#else
class NativeHDRCursor {
public:
    bool Update(void*, bool) { return false; }
};
#endif

#endif
