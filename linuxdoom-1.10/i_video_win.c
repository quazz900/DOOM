#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "d_main.h"
#include "doomdef.h"
#include "i_system.h"
#include "i_video.h"
#include "v_video.h"

static const char *window_class_name = "DoomWin32Window";

static HWND doom_window;
static BITMAPINFO doom_bitmap_info;
static uint32_t *doom_framebuffer;
static byte current_palette[256 * 3];
static int window_scale = 2;
static int mouse_initialized;
static int mouse_last_x;
static int mouse_last_y;
static int mouse_buttons;

static void I_BlitFrame(HDC dc);
static void I_PostMouseEvent(int buttons, int delta_x, int delta_y);

static int I_TranslateKey(WPARAM key)
{
    switch (key)
    {
    case VK_LEFT:
        return KEY_LEFTARROW;
    case VK_RIGHT:
        return KEY_RIGHTARROW;
    case VK_UP:
        return KEY_UPARROW;
    case VK_DOWN:
        return KEY_DOWNARROW;
    case VK_ESCAPE:
        return KEY_ESCAPE;
    case VK_RETURN:
        return KEY_ENTER;
    case VK_TAB:
        return KEY_TAB;
    case VK_BACK:
        return KEY_BACKSPACE;
    case VK_PAUSE:
        return KEY_PAUSE;
    case VK_F1:
        return KEY_F1;
    case VK_F2:
        return KEY_F2;
    case VK_F3:
        return KEY_F3;
    case VK_F4:
        return KEY_F4;
    case VK_F5:
        return KEY_F5;
    case VK_F6:
        return KEY_F6;
    case VK_F7:
        return KEY_F7;
    case VK_F8:
        return KEY_F8;
    case VK_F9:
        return KEY_F9;
    case VK_F10:
        return KEY_F10;
    case VK_F11:
        return KEY_F11;
    case VK_F12:
        return KEY_F12;
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
        return KEY_RSHIFT;
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
        return KEY_RCTRL;
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
        return KEY_RALT;
    case VK_SPACE:
        return ' ';
    case VK_OEM_PLUS:
        return KEY_EQUALS;
    case VK_OEM_MINUS:
        return KEY_MINUS;
    default:
        break;
    }

    if (key >= 'A' && key <= 'Z')
        return (int)(key - 'A' + 'a');

    if (key >= '0' && key <= '9')
        return (int)key;

    return 0;
}

static void I_PostKeyEvent(evtype_t type, WPARAM key)
{
    event_t event;
    int translated = I_TranslateKey(key);

    if (!translated)
        return;

    event.type = type;
    event.data1 = translated;
    event.data2 = 0;
    event.data3 = 0;
    D_PostEvent(&event);
}

static int I_MouseButtonsFromWParam(WPARAM wparam)
{
    int buttons = 0;

    if (wparam & MK_LBUTTON)
        buttons |= 1;
    if (wparam & MK_RBUTTON)
        buttons |= 2;
    if (wparam & MK_MBUTTON)
        buttons |= 4;

    return buttons;
}

static void I_PostMouseEvent(int buttons, int delta_x, int delta_y)
{
    event_t event;

    event.type = ev_mouse;
    event.data1 = buttons;
    event.data2 = delta_x;
    event.data3 = delta_y;
    D_PostEvent(&event);
}

static LRESULT CALLBACK I_WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    PAINTSTRUCT paint;
    HDC dc;

    lparam = lparam;

    switch (message)
    {
    case WM_CLOSE:
        I_Quit();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if ((HIWORD(lparam) & KF_REPEAT) == 0)
            I_PostKeyEvent(ev_keydown, wparam);
        return 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        I_PostKeyEvent(ev_keyup, wparam);
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        SetCapture(hwnd);
        mouse_buttons = I_MouseButtonsFromWParam(wparam);
        I_PostMouseEvent(mouse_buttons, 0, 0);
        return 0;

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        mouse_buttons = I_MouseButtonsFromWParam(wparam);
        I_PostMouseEvent(mouse_buttons, 0, 0);
        if (!mouse_buttons)
            ReleaseCapture();
        return 0;

    case WM_MOUSEMOVE:
    {
        int x = GET_X_LPARAM(lparam);
        int y = GET_Y_LPARAM(lparam);

        if (!mouse_initialized)
        {
            mouse_last_x = x;
            mouse_last_y = y;
            mouse_initialized = 1;
        }
        else
        {
            int delta_x = (x - mouse_last_x) << 2;
            int delta_y = (mouse_last_y - y) << 2;

            mouse_last_x = x;
            mouse_last_y = y;
            mouse_buttons = I_MouseButtonsFromWParam(wparam);

            if (delta_x || delta_y)
                I_PostMouseEvent(mouse_buttons, delta_x, delta_y);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        mouse_initialized = 0;
        return 0;

    case WM_PAINT:
        dc = BeginPaint(hwnd, &paint);
        I_BlitFrame(dc);
        EndPaint(hwnd, &paint);
        return 0;

    default:
        return DefWindowProc(hwnd, message, wparam, lparam);
    }
}

static void I_BlitFrame(HDC dc)
{
    RECT client_rect;
    int client_width;
    int client_height;
    int i;

    if (!doom_window || !doom_framebuffer || !screens[0])
        return;

    for (i = 0; i < SCREENWIDTH * SCREENHEIGHT; i++)
    {
        byte color_index = screens[0][i];
        byte *color = &current_palette[color_index * 3];
        doom_framebuffer[i] = ((uint32_t)color[2] << 16) | ((uint32_t)color[1] << 8) | color[0];
    }

    GetClientRect(doom_window, &client_rect);
    client_width = client_rect.right - client_rect.left;
    client_height = client_rect.bottom - client_rect.top;

    StretchDIBits(dc,
                  0, 0, client_width, client_height,
                  0, 0, SCREENWIDTH, SCREENHEIGHT,
                  doom_framebuffer,
                  &doom_bitmap_info,
                  DIB_RGB_COLORS,
                  SRCCOPY);
}

void I_ShutdownGraphics(void)
{
    if (doom_window)
    {
        DestroyWindow(doom_window);
        doom_window = NULL;
    }

    mouse_initialized = 0;
    mouse_buttons = 0;

    if (doom_framebuffer)
    {
        free(doom_framebuffer);
        doom_framebuffer = NULL;
    }
}

void I_StartFrame(void)
{
}

void I_StartTic(void)
{
    MSG message;
    TRACKMOUSEEVENT track;

    while (PeekMessage(&message, NULL, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
            I_Quit();

        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    if (doom_window)
    {
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = doom_window;
        track.dwHoverTime = 0;
        TrackMouseEvent(&track);
    }
}

void I_UpdateNoBlit(void)
{
}

void I_FinishUpdate(void)
{
    HDC dc;

    if (!doom_window)
        return;

    dc = GetDC(doom_window);
    I_BlitFrame(dc);
    ReleaseDC(doom_window, dc);
}

void I_ReadScreen(byte *scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

void I_SetPalette(byte *palette)
{
    memcpy(current_palette, palette, sizeof(current_palette));
}

void I_InitGraphics(void)
{
    WNDCLASS window_class;
    RECT window_rect;

    if (doom_window)
        return;

    memset(&window_class, 0, sizeof(window_class));
    window_class.lpfnWndProc = I_WindowProc;
    window_class.hInstance = GetModuleHandle(NULL);
    window_class.lpszClassName = window_class_name;
    window_class.hCursor = LoadCursor(NULL, IDC_ARROW);

    RegisterClass(&window_class);

    window_rect.left = 0;
    window_rect.top = 0;
    window_rect.right = SCREENWIDTH * window_scale;
    window_rect.bottom = SCREENHEIGHT * window_scale;
    AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);

    doom_window = CreateWindow(window_class_name,
                               "DOOM",
                               WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT,
                               CW_USEDEFAULT,
                               window_rect.right - window_rect.left,
                               window_rect.bottom - window_rect.top,
                               NULL,
                               NULL,
                               GetModuleHandle(NULL),
                               NULL);

    if (!doom_window)
        I_Error("I_InitGraphics: failed to create a window");

    ShowWindow(doom_window, SW_SHOWDEFAULT);
    UpdateWindow(doom_window);

    doom_framebuffer = (uint32_t *)malloc(sizeof(*doom_framebuffer) * SCREENWIDTH * SCREENHEIGHT);
    if (!doom_framebuffer)
        I_Error("I_InitGraphics: failed to allocate framebuffer");

    memset(&doom_bitmap_info, 0, sizeof(doom_bitmap_info));
    doom_bitmap_info.bmiHeader.biSize = sizeof(doom_bitmap_info.bmiHeader);
    doom_bitmap_info.bmiHeader.biWidth = SCREENWIDTH;
    doom_bitmap_info.bmiHeader.biHeight = -SCREENHEIGHT;
    doom_bitmap_info.bmiHeader.biPlanes = 1;
    doom_bitmap_info.bmiHeader.biBitCount = 32;
    doom_bitmap_info.bmiHeader.biCompression = BI_RGB;

    if (!screens[0])
        screens[0] = I_AllocLow(SCREENWIDTH * SCREENHEIGHT);
}
