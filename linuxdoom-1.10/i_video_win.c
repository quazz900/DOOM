#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "d_main.h"
#include "doomdef.h"
#include "doomstat.h"
#include "i_system.h"
#include "i_video.h"
#include "v_video.h"

static const char *window_class_name = "DoomWin32Window";
static const int doom_output_width = 320;
static const int doom_output_height = 240;

static HWND doom_window;
static BITMAPINFO doom_bitmap_info;
static uint32_t *doom_framebuffer;
static byte current_palette[256 * 3];
static int window_scale = 2;
static int mouse_initialized;
static int mouse_buttons;
static int mouse_captured;
static int mouse_ignore_move;
static int mouse_center_x;
static int mouse_center_y;
static int window_focused;

static void I_BlitFrame(HDC dc);
static void I_PostMouseEvent(int buttons, int delta_x, int delta_y);
static void I_UpdateMouseCenter(void);
static void I_SetMouseCapture(boolean capture);
static void I_RecenterMouse(void);
static void I_SyncMouseCapture(void);

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

static void I_UpdateMouseCenter(void)
{
    RECT rect;

    if (!doom_window)
        return;

    GetClientRect(doom_window, &rect);
    mouse_center_x = (rect.right - rect.left) / 2;
    mouse_center_y = (rect.bottom - rect.top) / 2;
}

static void I_RecenterMouse(void)
{
    POINT point;

    if (!doom_window || !mouse_captured)
        return;

    I_UpdateMouseCenter();
    point.x = mouse_center_x;
    point.y = mouse_center_y;
    ClientToScreen(doom_window, &point);
    mouse_ignore_move = 1;
    SetCursorPos(point.x, point.y);
}

static void I_SetMouseCapture(boolean capture)
{
    RECT rect;
    POINT top_left;
    POINT bottom_right;

    if (!doom_window || mouse_captured == capture)
        return;

    mouse_captured = capture;
    mouse_initialized = 0;

    if (capture)
    {
        GetClientRect(doom_window, &rect);
        top_left.x = rect.left;
        top_left.y = rect.top;
        bottom_right.x = rect.right;
        bottom_right.y = rect.bottom;
        ClientToScreen(doom_window, &top_left);
        ClientToScreen(doom_window, &bottom_right);
        rect.left = top_left.x;
        rect.top = top_left.y;
        rect.right = bottom_right.x;
        rect.bottom = bottom_right.y;

        ClipCursor(&rect);
        SetCapture(doom_window);
        while (ShowCursor(FALSE) >= 0)
            ;
        I_RecenterMouse();
    }
    else
    {
        ClipCursor(NULL);
        ReleaseCapture();
        while (ShowCursor(TRUE) < 0)
            ;
    }
}

static void I_SyncMouseCapture(void)
{
    I_SetMouseCapture(window_focused && !menuactive && !paused);
}

static LRESULT CALLBACK I_WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    PAINTSTRUCT paint;
    HDC dc;

    lparam = lparam;

    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_CLOSE:
        I_Quit();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_ACTIVATE:
        window_focused = (LOWORD(wparam) != WA_INACTIVE);
        I_SyncMouseCapture();
        return 0;

    case WM_SETFOCUS:
        window_focused = 1;
        I_SyncMouseCapture();
        return 0;

    case WM_KILLFOCUS:
        window_focused = 0;
        I_SyncMouseCapture();
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
        mouse_buttons = I_MouseButtonsFromWParam(wparam);
        I_PostMouseEvent(mouse_buttons, 0, 0);
        return 0;

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        mouse_buttons = I_MouseButtonsFromWParam(wparam);
        I_PostMouseEvent(mouse_buttons, 0, 0);
        return 0;

    case WM_MOUSEMOVE:
    {
        int x = GET_X_LPARAM(lparam);
        int y = GET_Y_LPARAM(lparam);
        int delta_x;
        int delta_y;

        if (mouse_ignore_move)
        {
            mouse_ignore_move = 0;
            return 0;
        }

        if (!mouse_initialized)
        {
            mouse_initialized = 1;
            if (mouse_captured)
                I_RecenterMouse();
        }

        if (mouse_captured)
        {
            delta_x = (x - mouse_center_x) << 2;
            delta_y = (mouse_center_y - y) << 2;
        }
        else
        {
            static int last_x;
            static int last_y;

            delta_x = (x - last_x) << 2;
            delta_y = (last_y - y) << 2;
            last_x = x;
            last_y = y;
        }

        mouse_buttons = I_MouseButtonsFromWParam(wparam);

        if (delta_x || delta_y)
            I_PostMouseEvent(mouse_buttons, delta_x, delta_y);

        if (mouse_captured)
            I_RecenterMouse();

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
    RECT fill_rect;
    int client_width;
    int client_height;
    int scale_x;
    int scale_y;
    int scale;
    int dest_width;
    int dest_height;
    int dest_x;
    int dest_y;
    int i;

    if (!doom_window || !doom_framebuffer || !screens[0])
        return;

    for (i = 0; i < SCREENWIDTH * SCREENHEIGHT; i++)
    {
        byte color_index = screens[0][i];
        byte *color = &current_palette[color_index * 3];
        doom_framebuffer[i] = ((uint32_t)color[0] << 16) | ((uint32_t)color[1] << 8) | color[2];
    }

    GetClientRect(doom_window, &client_rect);
    client_width = client_rect.right - client_rect.left;
    client_height = client_rect.bottom - client_rect.top;

    scale_x = client_width / SCREENWIDTH;
    scale_y = client_height / doom_output_height;
    scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale < 1)
        scale = 1;

    dest_width = doom_output_width * scale;
    dest_height = doom_output_height * scale;
    dest_x = (client_width - dest_width) / 2;
    dest_y = (client_height - dest_height) / 2;

    SetStretchBltMode(dc, COLORONCOLOR);

    if (dest_y > 0)
    {
        fill_rect.left = 0;
        fill_rect.top = 0;
        fill_rect.right = client_width;
        fill_rect.bottom = dest_y;
        FillRect(dc, &fill_rect, (HBRUSH)GetStockObject(BLACK_BRUSH));

        fill_rect.top = dest_y + dest_height;
        fill_rect.bottom = client_height;
        FillRect(dc, &fill_rect, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }

    if (dest_x > 0)
    {
        fill_rect.left = 0;
        fill_rect.top = dest_y;
        fill_rect.right = dest_x;
        fill_rect.bottom = dest_y + dest_height;
        FillRect(dc, &fill_rect, (HBRUSH)GetStockObject(BLACK_BRUSH));

        fill_rect.left = dest_x + dest_width;
        fill_rect.right = client_width;
        FillRect(dc, &fill_rect, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }

    StretchDIBits(dc,
                  dest_x, dest_y, dest_width, dest_height,
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
        I_SetMouseCapture(false);
        DestroyWindow(doom_window);
        doom_window = NULL;
    }

    mouse_initialized = 0;
    mouse_buttons = 0;
    mouse_ignore_move = 0;
    window_focused = 0;

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

    I_SyncMouseCapture();

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
    if (!dc)
        return;

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
    window_class.style = CS_OWNDC;
    window_class.lpfnWndProc = I_WindowProc;
    window_class.hInstance = GetModuleHandle(NULL);
    window_class.lpszClassName = window_class_name;
    window_class.hCursor = LoadCursor(NULL, IDC_ARROW);
    window_class.hbrBackground = NULL;

    RegisterClass(&window_class);

    window_rect.left = 0;
    window_rect.top = 0;
    window_rect.right = doom_output_width * window_scale;
    window_rect.bottom = doom_output_height * window_scale;
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
    SetForegroundWindow(doom_window);
    I_UpdateMouseCenter();
    window_focused = 1;
    I_SyncMouseCapture();

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
