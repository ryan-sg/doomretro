/*
========================================================================

                               DOOM RETRO
         The classic, refined DOOM source port. For Windows PC.

========================================================================

  Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
  Copyright (C) 2013-2015 Brad Harding.

  DOOM RETRO is a fork of CHOCOLATE DOOM by Simon Howard.
  For a complete list of credits, see the accompanying AUTHORS file.

  This file is part of DOOM RETRO.

  DOOM RETRO is free software: you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation, either version 3 of the License, or (at your
  option) any later version.

  DOOM RETRO is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with DOOM RETRO. If not, see <http://www.gnu.org/licenses/>.

  DOOM is a registered trademark of id Software LLC, a ZeniMax Media
  company, in the US and/or other countries and is used without
  permission. All other trademarks are the property of their respective
  holders. DOOM RETRO is in no way affiliated with nor endorsed by
  id Software LLC.

========================================================================
*/

#include "c_console.h"
#include "d_main.h"
#include "doomstat.h"
#include "hu_stuff.h"
#include "i_gamepad.h"
#include "i_system.h"
#include "i_tinttab.h"
#include "i_video.h"
#include "m_config.h"
#include "m_menu.h"
#include "m_misc.h"
#include "SDL.h"
#include "s_sound.h"
#include "v_video.h"
#include "version.h"
#include "w_wad.h"
#include "z_zone.h"

#ifdef WIN32
#include "SDL_syswm.h"
#endif

#ifndef _MSC_VER
#define __forceinline inline __attribute__((always_inline))
#endif

// Window position:
char                    *windowposition = WINDOWPOSITION_DEFAULT;

SDL_Surface             *screen = NULL;
SDL_Surface             *screenbuffer = NULL;
SDL_Window              *window = NULL;
SDL_Renderer            *renderer;
static SDL_Surface      *rgbabuffer = NULL;
static SDL_Texture      *texture = NULL; 

boolean                 vsync = VSYNC_DEFAULT;

// palette
SDL_Color               palette[256];
static boolean          palette_to_set;

// Bit mask of mouse button state
static unsigned int     mouse_button_state = 0;

boolean                 novert = NOVERT_DEFAULT;

static int              buttons[MAX_MOUSE_BUTTONS + 1] = { 0, 1, 4, 2, 8, 16, 32, 64, 128 };

// Fullscreen width and height
int                     screenwidth = SCREENWIDTH_DEFAULT;
int                     screenheight = SCREENHEIGHT_DEFAULT;

// Window width and height
int                     windowwidth = WINDOWWIDTH_DEFAULT;
int                     windowheight = WINDOWHEIGHT_DEFAULT;

// Run in full screen mode?
boolean                 fullscreen = FULLSCREEN_DEFAULT;

boolean                 widescreen = WIDESCREEN_DEFAULT;
boolean                 returntowidescreen = false;
boolean                 hud = HUD_DEFAULT;

char                    *scalequality = SCALEQUALITY_DEFAULT;

// Flag indicating whether the screen is currently visible:
// when the screen isn't visible, don't render the screen
boolean                 screenvisible;

boolean                 window_focused;

// Empty mouse cursor
static SDL_Cursor       *cursors[2];

// Window resize state.
static boolean          need_resize = false;
static unsigned int     resize_w;
static unsigned int     resize_h;

int                     desktopwidth;
int                     desktopheight;

char                    *videodriver = VIDEODRIVER_DEFAULT;
char                    envstring[255];

static int              width;
static int              height;
static int              stepx;
static int              stepy;
static int              startx;
static int              starty;
static int              pitch;

byte                    *pixels;

byte                    *rows[SCREENHEIGHT];

boolean                 keys[UCHAR_MAX];

byte                    gammatable[GAMMALEVELS][256];

float                   gammalevels[GAMMALEVELS] =
{
    // Darker
    0.50f, 0.55f, 0.60f, 0.65f, 0.70f, 0.75f, 0.80f, 0.85f, 0.90f, 0.95f,

    // No gamma correction
    1.0f,

    // Lighter
    1.05f, 1.10f, 1.15f, 1.20f, 1.25f, 1.30f, 1.35f, 1.40f, 1.45f, 1.50f,
    1.55f, 1.60f, 1.65f, 1.70f, 1.75f, 1.80f, 1.85f, 1.90f, 1.95f, 2.0f
};

// Gamma correction level to use
int                     gammaindex;
float                   gammalevel = GAMMALEVEL_DEFAULT;

SDL_Rect                src_rect = { 0, 0, 0, 0 };

boolean                 showfps = false;
int                     fps = 0;
int                     frames = 0;
int                     starttime;
int                     currenttime;

// Mouse acceleration
//
// This emulates some of the behavior of DOS mouse drivers by increasing
// the speed when the mouse is moved fast.
//
// The mouse input values are input directly to the game, but when
// the values exceed the value of mouse_threshold, they are multiplied
// by mouse_acceleration to increase the speed.
float                   mouse_acceleration = MOUSEACCELERATION_DEFAULT;
int                     mouse_threshold = MOUSETHRESHOLD_DEFAULT;

int                     capslock;
boolean                 alwaysrun = ALWAYSRUN_DEFAULT;

static void ApplyWindowResize(int resize_h);
static void SetWindowPositionVars(void);

boolean MouseShouldBeGrabbed(void)
{
    // if the window doesn't have focus, never grab it
    if (!window_focused)
        return false;

    // always grab the mouse when full screen (dont want to
    // see the mouse pointer)
    if (fullscreen)
        return true;

    // when menu is active or game is paused, release the mouse
    if (menuactive || paused)
        return false;

    // only grab mouse when playing levels
    return (gamestate == GS_LEVEL);
}

// Update the value of window_focused when we get a focus event
//
// We try to make ourselves be well-behaved: the grab on the mouse
// is removed if we lose focus (such as a popup window appearing),
// and we dont move the mouse around if we aren't focused either.
static void UpdateFocus(void)
{
    Uint32              state = SDL_GetWindowFlags(window);

    // Should the screen be grabbed?
    screenvisible = (state & SDL_WINDOW_SHOWN);

    // We should have input (keyboard) focus and be visible
    // (not minimized)
    window_focused = ((state & SDL_WINDOW_INPUT_FOCUS) && screenvisible);

    if (!window_focused && !menuactive && gamestate == GS_LEVEL && !paused && !consoleactive)
    {
        sendpause = true;
        blurred = false;
    }
}

// Show or hide the mouse cursor. We have to use different techniques
// depending on the OS.
static void SetShowCursor(boolean show)
{
    // On Windows, using SDL_ShowCursor() adds lag to the mouse input,
    // so work around this by setting an invisible cursor instead. On
    // other systems, it isn't possible to change the cursor, so this
    // hack has to be Windows-only. (Thanks to entryway for this)
#ifdef WIN32
    SDL_SetCursor(cursors[show]);
#else
    SDL_ShowCursor(show);
#endif

    // When the cursor is hidden, grab the input.
    SDL_SetRelativeMouseMode(!show);
}

int translatekey[] =
{
    0, 0, 0, 0, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '1',
    '2', '3', '4', '5', '6', '7', '8', '9', '0', KEY_ENTER, KEY_ESCAPE,
    KEY_BACKSPACE, KEY_TAB, ' ', KEY_MINUS, KEY_EQUALS, '[', ']', '\\', '\\',
    ';', '\'', '`', ',', '.', '/', KEY_CAPSLOCK, KEY_F1, KEY_F2, KEY_F3,
    KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    0, KEY_SCRLCK, KEY_PAUSE, KEY_INS, KEY_HOME, KEY_PGUP, KEY_DEL, KEY_END,
    KEY_PGDN, KEY_RIGHTARROW, KEY_LEFTARROW, KEY_DOWNARROW, KEY_UPARROW,
    KEY_NUMLOCK, KEYP_DIVIDE, KEYP_MULTIPLY, KEYP_MINUS, KEYP_PLUS, KEYP_ENTER,
    KEYP_1, KEYP_2, KEYP_3, KEYP_4, KEYP_5, KEYP_6, KEYP_7, KEYP_8, KEYP_9,
    KEYP_0, KEYP_PERIOD, 0, 0, 0, KEYP_EQUALS, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, KEY_RCTRL, KEY_RSHIFT, KEY_RALT, 0, KEY_RCTRL,
    KEY_RSHIFT, KEY_RALT, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

int TranslateKey2(int key)
{
    switch (key)
    {
        case KEY_LEFTARROW:    return SDL_SCANCODE_LEFT;
        case KEY_RIGHTARROW:   return SDL_SCANCODE_RIGHT;
        case KEY_DOWNARROW:    return SDL_SCANCODE_DOWN;
        case KEY_UPARROW:      return SDL_SCANCODE_UP;
        case KEY_ESCAPE:       return SDL_SCANCODE_ESCAPE;
        case KEY_ENTER:        return SDL_SCANCODE_RETURN;
        case KEY_TAB:          return SDL_SCANCODE_TAB;
        case KEY_F1:           return SDL_SCANCODE_F1;
        case KEY_F2:           return SDL_SCANCODE_F2;
        case KEY_F3:           return SDL_SCANCODE_F3;
        case KEY_F4:           return SDL_SCANCODE_F4;
        case KEY_F5:           return SDL_SCANCODE_F5;
        case KEY_F6:           return SDL_SCANCODE_F6;
        case KEY_F7:           return SDL_SCANCODE_F7;
        case KEY_F8:           return SDL_SCANCODE_F8;
        case KEY_F9:           return SDL_SCANCODE_F9;
        case KEY_F10:          return SDL_SCANCODE_F10;
        case KEY_F11:          return SDL_SCANCODE_F11;
        case KEY_F12:          return SDL_SCANCODE_F12;
        case KEY_BACKSPACE:    return SDL_SCANCODE_BACKSPACE;
        case KEY_DEL:          return SDL_SCANCODE_DELETE;
        case KEY_PAUSE:        return SDL_SCANCODE_PAUSE;
        case KEY_EQUALS:       return SDL_SCANCODE_EQUALS;
        case KEY_MINUS:        return SDL_SCANCODE_MINUS;
        case KEY_RSHIFT:       return SDL_SCANCODE_RSHIFT;
        case KEY_RCTRL:        return SDL_SCANCODE_RCTRL;
        case KEY_RALT:         return SDL_SCANCODE_RALT;
        case KEY_CAPSLOCK:     return SDL_SCANCODE_CAPSLOCK;
        case KEY_SCRLCK:       return SDL_SCANCODE_SCROLLLOCK;
        case KEYP_0:           return SDL_SCANCODE_KP_0;
        case KEYP_1:           return SDL_SCANCODE_KP_1;
        case KEYP_3:           return SDL_SCANCODE_KP_3;
        case KEYP_5:           return SDL_SCANCODE_KP_5;
        case KEYP_7:           return SDL_SCANCODE_KP_7;
        case KEYP_9:           return SDL_SCANCODE_KP_9;
        case KEYP_PERIOD:      return SDL_SCANCODE_KP_PERIOD;
        case KEYP_MULTIPLY:    return SDL_SCANCODE_KP_MULTIPLY;
        case KEYP_DIVIDE:      return SDL_SCANCODE_KP_DIVIDE;
        case KEY_INS:          return SDL_SCANCODE_INSERT;
        case KEY_NUMLOCK:      return SDL_SCANCODE_NUMLOCKCLEAR;
        default:               return key;
    }
}

boolean keystate(int key)
{
    const Uint8 *keystate = SDL_GetKeyboardState(NULL);

    return keystate[TranslateKey2(key)];
}

void I_SaveWindowPosition(void)
{
#ifdef WIN32
    SDL_SysWMinfo       info;

    SDL_VERSION(&info.version);

    if (SDL_GetWindowWMInfo(window, &info))
    {
        HWND    hwnd;
        RECT    r;

        hwnd = info.info.win.window;
        GetWindowRect(hwnd, &r);
        if (widescreen)
            r.left += (screen->w - screenbuffer->w) / 2;
        sprintf(windowposition, "%i,%i", r.left + 8, r.top + 30);
        M_SaveDefaults();
    }
#endif
}

void RepositionWindow(int amount)
{
#ifdef WIN32
    SDL_SysWMinfo       info;

    SDL_VERSION(&info.version);

    if (SDL_GetWindowWMInfo(window, &info))
    {
        HWND    hwnd;
        RECT    r;

        hwnd = info.info.win.window;
        GetWindowRect(hwnd, &r);
        SetWindowPos(hwnd, NULL, r.left + amount, r.top, 0, 0, SWP_NOSIZE);
    }
#endif
}

void I_ShutdownGraphics(void)
{
    SetShowCursor(true);

    SDL_FreeSurface(screenbuffer);
    SDL_FreeSurface(rgbabuffer);

    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void I_ShutdownKeyboard(void)
{
#ifdef WIN32
    if ((GetKeyState(VK_CAPITAL) & 0x0001) && !capslock)
    {
        keybd_event(VK_CAPITAL, 0x45, KEYEVENTF_EXTENDEDKEY, (uintptr_t)0);
        keybd_event(VK_CAPITAL, 0x45, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, (uintptr_t)0);
    }
#endif
}

static int AccelerateMouse(int val)
{
    if (val < 0)
        return -AccelerateMouse(-val);

    if (val > mouse_threshold)
        return (int)((val - mouse_threshold) * mouse_acceleration + mouse_threshold);
    else
        return val;
}

// Warp the mouse back to the middle of the screen
static void CenterMouse(void)
{
    // Warp to the screen center
    SDL_WarpMouseInWindow(window, screen->w / 2, screen->h / 2);

    // Clear any relative movement caused by warping
    SDL_PumpEvents();
    SDL_GetRelativeMouseState(NULL, NULL);
}

boolean altdown = false;
boolean waspaused = false;

boolean noinput = true;

void I_GetEvent(void)
{
    SDL_Event           sdlevent;
    event_t             ev;

    while (SDL_PollEvent(&sdlevent))
    {
        switch (sdlevent.type)
        {
            case SDL_KEYDOWN:
                if (noinput)
                    return;

                ev.type = ev_keydown;

                ev.data1 = translatekey[sdlevent.key.keysym.scancode];
                ev.data2 = sdlevent.key.keysym.sym;
                if (ev.data2 < SDLK_SPACE || ev.data2 > SDLK_z)
                    ev.data2 = 0;

                altdown = (sdlevent.key.keysym.mod & KMOD_ALT);

                if (altdown && ev.data1 == KEY_TAB)
                    ev.data1 = ev.data2 = 0;

                if (!isdigit(ev.data2))
                    idclev = idmus = false;

                if (idbehold && keys[ev.data2])
                {
                    HU_clearMessages();
                    idbehold = false;
                }

                if (ev.data1)
                    D_PostEvent(&ev);
                break;

            case SDL_KEYUP:
                ev.type = ev_keyup;

                ev.data1 = translatekey[sdlevent.key.keysym.scancode];
                ev.data2 = 0;

                altdown = (sdlevent.key.keysym.mod & KMOD_ALT);
                keydown = 0;

                if (ev.data1)
                    D_PostEvent(&ev);
                break;

            case SDL_MOUSEBUTTONDOWN:
                if (mousesensitivity || menuactive)
                {
                    idclev = false;
                    idmus = false;
                    if (idbehold)
                    {
                        HU_clearMessages();
                        idbehold = false;
                    }
                    ev.type = ev_mouse;
                    mouse_button_state |= buttons[sdlevent.button.button];
                    ev.data1 = mouse_button_state;
                    ev.data2 = 0;
                    ev.data3 = 0;
                    D_PostEvent(&ev);
                }
                break;

            case SDL_MOUSEBUTTONUP:
                if (mousesensitivity || menuactive)
                {
                    keydown = 0;
                    ev.type = ev_mouse;
                    mouse_button_state &= ~buttons[sdlevent.button.button];
                    ev.data1 = mouse_button_state;
                    ev.data2 = 0;
                    ev.data3 = 0;
                    D_PostEvent(&ev);
                }
                break;

            case SDL_MOUSEWHEEL:
                if (mousesensitivity || menuactive)
                {
                    keydown = 0;
                    ev.type = ev_mousewheel;
                    ev.data1 = sdlevent.wheel.y;
                    ev.data2 = 0;
                    ev.data3 = 0;
                    D_PostEvent(&ev);
                }
                break;

            case SDL_JOYBUTTONUP:
                keydown = 0;
                break;

            case SDL_QUIT:
                if (!quitting && !splashscreen)
                {
                    keydown = 0;
                    if (paused)
                    {
                        paused = false;
                        waspaused = true;
                    }
                    S_StartSound(NULL, sfx_swtchn);
                    M_QuitDOOM(0);
                }
                break;

#ifdef WIN32
            case SDL_SYSWMEVENT:
                if (!fullscreen)
                {
                    if (sdlevent.syswm.msg->msg.win.msg == WM_MOVE)
                    {
                        I_SaveWindowPosition();
                        SetWindowPositionVars();
                    }
                }
                break;
#endif

            case SDL_WINDOWEVENT:
                switch (sdlevent.window.event)
                {
                    case SDL_WINDOWEVENT_FOCUS_GAINED:
                    case SDL_WINDOWEVENT_FOCUS_LOST:
                        // need to update our focus state
                        UpdateFocus();
                        break;

                    case SDL_WINDOWEVENT_EXPOSED:
                        palette_to_set = true;
                        break;

                    case SDL_WINDOWEVENT_RESIZED:
                        if (!fullscreen)
                        {
                            need_resize = true;
                            resize_h = sdlevent.window.data2;
                        }
                        break;
                }
                break;

            default:
                break;
        }
    }
}

static void I_ReadMouse(void)
{
    int         x, y;
    event_t     ev;

    ev.type = ev_mouse;
    ev.data1 = SDL_GetRelativeMouseState(&x, &y);
    ev.data2 = AccelerateMouse(x);
    ev.data3 = (novert ? 0 : -AccelerateMouse(y));

    D_PostEvent(&ev);

    if (MouseShouldBeGrabbed())
        CenterMouse();
}

//
// I_StartTic
//
void I_StartTic(void)
{
    I_GetEvent();
    if (mousesensitivity)
        I_ReadMouse();
    gamepadfunc();
}

boolean currently_grabbed = false;

static void UpdateGrab(void)
{
    boolean     grab = MouseShouldBeGrabbed();

    if (grab && !currently_grabbed)
    {
        SetShowCursor(false);
        CenterMouse();
    }
    else if (!grab && currently_grabbed)
    {
        SetShowCursor(true);
        SDL_WarpMouseInWindow(window, screen->w - 16, screen->h - 16);
        SDL_GetRelativeMouseState(NULL, NULL);
    }

    currently_grabbed = grab;
}

//
// I_FinishUpdate
//
void I_FinishUpdate(void)
{
    if (need_resize)
    {
        ApplyWindowResize(resize_h);
        need_resize = false;
        palette_to_set = true;
    }

    UpdateGrab();

    // Don't update the screen if the window isn't visible.
    // Not doing this breaks under Windows when we alt-tab away
    // while fullscreen.
    if (!screenvisible)
        return;

    if (palette_to_set)
    {
        SDL_SetPaletteColors(screenbuffer->format->palette, palette, 0, 256);
        palette_to_set = false;
    }

    SDL_BlitSurface(screenbuffer, NULL, rgbabuffer, NULL);
    SDL_UpdateTexture(texture, NULL, rgbabuffer->pixels, SCREENWIDTH * sizeof(Uint32));
    SDL_RenderCopy(renderer, texture, &src_rect, NULL);
    SDL_RenderPresent(renderer);

    if (showfps)
    {
        ++frames;
        currenttime = SDL_GetTicks();
        if (currenttime - starttime >= 1000)
        {
            fps = frames;
            frames = 0;
            starttime = currenttime;
        }
    }
}

//
// I_ReadScreen
//
void I_ReadScreen(byte *scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

//
// I_SetPalette
//
void I_SetPalette(byte *doompalette)
{
    int i;

    for (i = 0; i < 256; ++i)
    {
        palette[i].r = gammatable[gammaindex][*doompalette++];
        palette[i].g = gammatable[gammaindex][*doompalette++];
        palette[i].b = gammatable[gammaindex][*doompalette++];
        palette[i].a = 255;
    }

    palette_to_set = true;
}

static void CreateCursors(void)
{
    static Uint8 empty_cursor_data = 0;

    // Save the default cursor so it can be recalled later
    cursors[1] = SDL_GetCursor();

    // Create an empty cursor
    cursors[0] = SDL_CreateCursor(&empty_cursor_data, &empty_cursor_data, 1, 1, 0, 0);
}

static void SetWindowPositionVars(void)
{
    char        buf[64];
    int         x, y;

    if (sscanf(windowposition, "%i,%i", &x, &y) == 2)
    {
        if (x < 0)
            x = 0;
        else if (x > desktopwidth)
            x = desktopwidth - 16;
        if (y < 0)
            y = 0;
        else if (y > desktopheight)
            y = desktopheight - 16;
        sprintf(buf, "SDL_VIDEO_WINDOW_POS=%i,%i", x, y);
        putenv(buf);
    }
    else
        putenv("SDL_VIDEO_CENTERED=1");
}

static void GetDesktopDimensions(void)
{
    SDL_Rect            displaybounds;

    SDL_GetDisplayBounds(0, &displaybounds);
    desktopwidth = displaybounds.w;
    desktopheight = displaybounds.h;
}

static void SetupScreenRects(void)
{
    src_rect.w = SCREENWIDTH;
    src_rect.h = SCREENHEIGHT - SBARHEIGHT * widescreen;
}

static void SetVideoMode(void)
{
    int flags = (SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);

    if (vsync)
        flags |= SDL_RENDERER_PRESENTVSYNC;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scalequality);

    if (fullscreen)
    {
        width = screenwidth;
        height = screenheight;
        if (!width || !height)
        {
            width = desktopwidth;
            height = desktopheight;
            screenwidth = 0;
            screenheight = 0;
            M_SaveDefaults();
        }

        window = SDL_CreateWindow(PACKAGE_NAME, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            width, height, SDL_WINDOW_FULLSCREEN_DESKTOP);
        renderer = SDL_CreateRenderer(window, -1, flags);
        screen = SDL_GetWindowSurface(window);

        if (!screen)
            I_Error("SetVideoMode, line %i: %s\n", __LINE__ - 5, SDL_GetError());

        SDL_RenderSetLogicalSize(renderer, SCREENWIDTH, SCREENWIDTH * 3 / 4);

        height = screen->h;
        width = height * 4 / 3;
        width += (width & 1);

        if (width > screen->w)
        {
            width = screen->w;
            height = width * 3 / 4;
            height += (height & 1);
        }
    }
    else
    {
        if (windowheight > desktopheight)
        {
            windowheight = desktopheight;
            windowwidth = windowheight * 4 / 3;
            M_SaveDefaults();
        }
        height = MAX(ORIGINALWIDTH * 3 / 4, windowheight);
        width = height * 4 / 3;

        if (width > windowwidth)
        {
            width = windowwidth;
            height = width * 3 / 4;
        }

        SetWindowPositionVars();

        SDL_CreateWindowAndRenderer(windowwidth, windowheight, SDL_WINDOW_RESIZABLE, &window,
            &renderer);
        screen = SDL_GetWindowSurface(window);

        if (!screen)
            I_Error("SetVideoMode, line %i: %s\n", __LINE__ - 5, SDL_GetError());

        widescreen = false;
    }

    screenbuffer = SDL_CreateRGBSurface(0, SCREENWIDTH, SCREENHEIGHT, 8, 0, 0, 0, 0);
    rgbabuffer = SDL_CreateRGBSurface(0, SCREENWIDTH, SCREENHEIGHT, 32, 0, 0, 0, 0);
    SDL_FillRect(rgbabuffer, NULL, 0);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        SCREENWIDTH, SCREENHEIGHT);

    if (!screenbuffer)
        I_Error("SetVideoMode, line %i: %s\n", __LINE__ - 3, SDL_GetError());

    SetupScreenRects();

    pitch = screenbuffer->pitch;
    pixels = (byte *)screenbuffer->pixels;

    stepx = (SCREENWIDTH << FRACBITS) / width;
    stepy = (SCREENHEIGHT << FRACBITS) / height;

    startx = stepx - 1;
    starty = stepy - 1;
}

void ToggleWidescreen(boolean toggle)
{
    if (fullscreen && (double)screenwidth / screenheight < (double)16 / 10)
    {
        widescreen = returntowidescreen = false;
        return;
    }

    SDL_FillRect(screenbuffer, NULL, 0);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    if (toggle)
    {
        widescreen = true;

        if (returntowidescreen && screensize == 8)
        {
            screensize = 7;
            R_SetViewSize(screensize);
        }

        SDL_RenderSetLogicalSize(renderer, SCREENWIDTH, SCREENHEIGHT);

        src_rect.h = SCREENHEIGHT - SBARHEIGHT;
    }
    else
    {
        widescreen = false;

        SDL_RenderSetLogicalSize(renderer, SCREENWIDTH, SCREENWIDTH * 3 / 4);

        src_rect.h = SCREENHEIGHT;
    }

    returntowidescreen = false;

    palette_to_set = true;
}

#ifdef WIN32
void I_InitWindows32(void);
#endif

void ToggleFullscreen(void)
{
    fullscreen = !fullscreen;
    M_SaveDefaults();
    if (fullscreen)
    {
        width = screenwidth;
        height = screenheight;
        if (!width || !height)
        {
            width = desktopwidth;
            height = desktopheight;
            screenwidth = 0;
            screenheight = 0;
            M_SaveDefaults();
        }

        SDL_CreateWindowAndRenderer(width, height, SDL_WINDOW_FULLSCREEN_DESKTOP, &window,
            &renderer);
        screen = SDL_GetWindowSurface(window);

        if (!screen)
        {
            width = desktopwidth;
            height = desktopheight;
            screenwidth = 0;
            screenheight = 0;
            M_SaveDefaults();

            SDL_CreateWindowAndRenderer(width, height, SDL_WINDOW_FULLSCREEN_DESKTOP, &window,
                &renderer);
            screen = SDL_GetWindowSurface(window);

            if (!screen)
                I_Error("ToggleFullscreen, line %i: %s\n", __LINE__ - 5, SDL_GetError());
        }

        if (widescreen)
        {
            if (gamestate != GS_LEVEL)
                returntowidescreen = true;
            else
            {
                ToggleWidescreen(true);
                if (widescreen)
                    screensize = 7;
                R_SetViewSize(screensize);
                M_SaveDefaults();
                if (widescreen)
                    return;
            }
        }

        height = screen->h;
        width = height * 4 / 3;
        width += (width & 1);

        if (width > screen->w)
        {
            width = screen->w;
            height = width * 3 / 4;
            height += (height & 1);
        }
    }
    else
    {
        if (windowheight > desktopheight)
        {
            windowheight = desktopheight;
            windowwidth = windowheight * 4 / 3;
            M_SaveDefaults();
        }

        height = MAX(ORIGINALWIDTH * 3 / 4, windowheight);
        width = height * 4 / 3;

        if (width > windowwidth)
        {
            width = windowwidth;
            height = width * 3 / 4;
        }

        SetWindowPositionVars();

        SDL_DestroyWindow(window);
        window = SDL_CreateWindow(gamedescription, SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED, width, height, SDL_WINDOW_RESIZABLE);
        screen = SDL_GetWindowSurface(window);

        if (!screen)
            I_Error("ToggleFullscreen, line %i: %s\n", __LINE__ - 5, SDL_GetError());

        if (widescreen)
        {
            if (gamestate != GS_LEVEL)
                returntowidescreen = true;
            else
            {
                ToggleWidescreen(true);
                if (widescreen)
                    screensize = 7;
                R_SetViewSize(screensize);
                M_SaveDefaults();
                return;
            }
        }
    }

    SDL_FreeSurface(screenbuffer);
    screenbuffer = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 8, 0, 0, 0, 0);

    if (!screenbuffer)
        I_Error("ToggleFullscreen, line %i: %s\n", __LINE__ - 3, SDL_GetError());

    SetupScreenRects();

    pitch = screenbuffer->pitch;
    pixels = (byte *)screenbuffer->pixels;

    stepx = (SCREENWIDTH << FRACBITS) / width;
    stepy = (SCREENHEIGHT << FRACBITS) / height;

    startx = stepx - 1;
    starty = stepy - 1;
}

static void ApplyWindowResize(int resize_h)
{
    windowheight = height = MAX(SCREENWIDTH * 3 / 4, MIN(resize_h, desktopheight));
    windowwidth = windowheight * 4 / 3;

    if (widescreen)
        height += (int)((double)height * SBARHEIGHT / (SCREENHEIGHT - SBARHEIGHT) + 1.5);

    SDL_SetWindowSize(window, windowwidth, windowheight);
    screen = SDL_GetWindowSurface(window);

    if (!screen)
        I_Error("ApplyWindowResize, line %i: %s\n", __LINE__ - 5, SDL_GetError());

    SDL_FreeSurface(screenbuffer);
    screenbuffer = SDL_CreateRGBSurface(SDL_SWSURFACE, windowwidth, height, 8, 0, 0, 0, 0);

    if (!screenbuffer)
        I_Error("ApplyWindowResize, line %i: %s\n", __LINE__ - 3, SDL_GetError());

    SetupScreenRects();

    pitch = screenbuffer->pitch;
    pixels = (byte *)screenbuffer->pixels;

    stepx = (SCREENWIDTH << FRACBITS) / windowwidth;
    stepy = (SCREENHEIGHT << FRACBITS) / height;

    startx = stepx - 1;
    starty = stepy - 1;

    M_SaveDefaults();
}

void I_InitGammaTables(void)
{
    int i;
    int j;

    for (i = 0; i < GAMMALEVELS; i++)
        if (gammalevels[i] == 1.0)
            for (j = 0; j < 256; j++)
                gammatable[i][j] = j;
        else
            for (j = 0; j < 256; j++)
                gammatable[i][j] = (byte)(pow((j + 1) / 256.0, 1.0 / gammalevels[i]) * 255.0);
}

boolean I_ValidScreenMode(int width, int height)
{
    SDL_DisplayMode     mode;
    const int           modecount = SDL_GetNumDisplayModes(0);
    int                 i;

    for (i = 0; i < modecount; i++)
    {
        SDL_GetDisplayMode(0, i, &mode);
        if (!mode.w || !mode.h || (width >= mode.w && height >= mode.h))
            return true;
    }
    return false;
}

void I_InitKeyboard(void)
{
#ifdef WIN32
    capslock = (GetKeyState(VK_CAPITAL) & 0x0001);

    if ((alwaysrun && !capslock) || (!alwaysrun && capslock))
    {
        keybd_event(VK_CAPITAL, 0x45, KEYEVENTF_EXTENDEDKEY, (uintptr_t)0);
        keybd_event(VK_CAPITAL, 0x45, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, (uintptr_t)0);
    }
#endif
}

void I_InitGraphics(void)
{
    int         i = 0;
    SDL_Event   dummy;
    byte        *doompal = W_CacheLumpName("PLAYPAL", PU_CACHE);

    putenv("SDL_DISABLE_LOCK_KEYS=1");

    while (i < UCHAR_MAX)
        keys[i++] = true;
    keys['v'] = keys['V'] = false;
    keys['s'] = keys['S'] = false;
    keys['i'] = keys['I'] = false;
    keys['r'] = keys['R'] = false;
    keys['a'] = keys['A'] = false;
    keys['l'] = keys['L'] = false;

    I_InitTintTables(doompal);

    I_InitGammaTables();

    if (videodriver != NULL && strlen(videodriver) > 0)
    {
        M_snprintf(envstring, sizeof(envstring), "SDL_VIDEODRIVER=%s", videodriver);
        putenv(envstring);
    }

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
    {
#ifdef WIN32
        if (strcasecmp(videodriver, "windows"))
            M_StringCopy(videodriver, "windows", 8);
        M_snprintf(envstring, sizeof(envstring), "SDL_VIDEODRIVER=%s", videodriver);
        putenv(envstring);
        M_SaveDefaults();

        if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
#endif
            I_Error("I_InitGraphics, line %i: %s\n", __LINE__ - 2, SDL_GetError());
    }

    CreateCursors();
    SDL_SetCursor(cursors[0]);

    if (fullscreen && (screenwidth || screenheight))
        if (!I_ValidScreenMode(screenwidth, screenheight))
        {
            screenwidth = 0;
            screenheight = 0;
        }

    GetDesktopDimensions();

    SetVideoMode();

#ifdef WIN32
    I_InitWindows32();
#endif

    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);

    SDL_SetWindowTitle(window, PACKAGE_NAME);

    I_SetPalette(doompal);

    SDL_SetPaletteColors(screenbuffer->format->palette, palette, 0, 256);

    if (!fullscreen)
        currently_grabbed = true;
    UpdateFocus();
    UpdateGrab();

    screens[0] = screenbuffer->pixels;

    for (i = 0; i < SCREENHEIGHT; i++)
        rows[i] = *screens + i * SCREENWIDTH;

    if (showfps)
        starttime = SDL_GetTicks();

    I_FinishUpdate();

    while (SDL_PollEvent(&dummy));

    if (fullscreen)
        CenterMouse();
}
