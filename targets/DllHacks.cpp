#include "DllHacks.h"
#include "AsmHacks.h"
#include "D3DHook.h"
#include "Exceptions.h"
#include "ProcessManager.h"
#include "Algorithms.h"
#include "Enum.h"

#include <windows.h>
#include <d3dx9.h>
#include <MinHook.h>

using namespace std;
using namespace AsmHacks;


#define FONT_HEIGHT                 ( 18 )

#define TEXT_BORDER                 ( 10 )

#define OVERLAY_CHANGE_DELTA        ( 4 + abs ( overlayHeight - newHeight ) / 4 )


namespace DllHacks
{

ENUM ( Overlay, Disabled, Disabling, Enabled, Enabling );

static Overlay overlayState = Overlay::Disabled;

static int overlayHeight = 0, oldHeight = 0, newHeight = 0;

static array<string, 3> overlayText;

static int getTextHeight ( const array<string, 3>& newText )
{
    int height = 0;

    for ( const string& text : newText )
        height = max ( height, FONT_HEIGHT * ( 1 + count ( text.begin(), text.end(), '\n' ) ) );

    return height;
}


void enableOverlay()
{
    if ( overlayState != Overlay::Enabled )
        overlayState = Overlay::Enabling;
}

void disableOverlay()
{
    if ( overlayState != Overlay::Disabled )
        overlayState = Overlay::Disabling;
}

void toggleOverlay()
{
    if ( isOverlayEnabled() )
        disableOverlay();
    else
        enableOverlay();
}

void updateOverlay ( const array<string, 3>& newText )
{
    switch ( overlayState.value )
    {
        default:
        case Overlay::Disabled:
            overlayHeight = oldHeight = newHeight = 0;
            return;

        case Overlay::Disabling:
            newHeight = 0;

            if ( overlayHeight != newHeight )
                break;

            overlayState = Overlay::Disabled;
            oldHeight = 0;
            break;

        case Overlay::Enabled:
            newHeight = getTextHeight ( newText );

            if ( newHeight > overlayHeight )
                break;

            if ( newHeight == overlayHeight )
                oldHeight = overlayHeight;

            overlayText = newText;
            break;

        case Overlay::Enabling:
            newHeight = getTextHeight ( newText );

            if ( overlayHeight != newHeight )
                break;

            overlayState = Overlay::Enabled;
            oldHeight = overlayHeight;
            break;
    }

    if ( overlayHeight == newHeight )
        return;

    if ( newHeight > overlayHeight )
        overlayHeight = clamped ( overlayHeight + OVERLAY_CHANGE_DELTA, overlayHeight, newHeight );
    else
        overlayHeight = clamped ( overlayHeight - OVERLAY_CHANGE_DELTA, newHeight, overlayHeight );
}

bool isOverlayEnabled()
{
    return ( overlayState != Overlay::Disabled );
}

} // namespace DllHacks


using namespace DllHacks;


static bool initalizedDirectx = false;

static ID3DXFont *font = 0;

static IDirect3DVertexBuffer9 *background = 0;


struct Vertex
{
    FLOAT x, y, z;
    DWORD color;

    static const DWORD format = ( D3DFVF_XYZ | D3DFVF_DIFFUSE );
};


static int DrawText ( const string& text, RECT& rect, int flags, int r, int g, int b )
{
    if ( !font )
        return 0;

    return font->DrawText ( 0,                                  // text as a ID3DXSprite object
                            &text[0],                           // text buffer
                            text.size(),                        // number of characters, -1 if null-terminated
                            &rect,                              // text bounding RECT
                            flags | DT_WORDBREAK | DT_NOCLIP,   // text formatting
                            D3DCOLOR_XRGB ( r, g, b ) );        // text colour
}

// Note: this is on the SAME thread as the main thread where callback happens
void PresentFrameBegin ( IDirect3DDevice9 *device )
{
    if ( !initalizedDirectx )
    {
        initalizedDirectx = true;

        D3DXCreateFont ( device,                                // device pointer
                         FONT_HEIGHT,                           // height
                         0,                                     // width
                         FW_REGULAR,                            // weight
                         1,                                     // # of mipmap levels
                         false,                                 // italic
                         DEFAULT_CHARSET,                       // charset
                         OUT_DEFAULT_PRECIS,                    // output precision
                         ANTIALIASED_QUALITY,                   // quality
                         DEFAULT_PITCH | FF_DONTCARE,           // pitch and family
                         "Lucida Console",                      // typeface name
                         &font );                               // pointer to ID3DXFont

        static const Vertex verts[4] =
        {
            { -1.0, -1.0, 0.0f, D3DCOLOR_ARGB ( 210, 0, 0, 0 ) },
            {  1.0, -1.0, 0.0f, D3DCOLOR_ARGB ( 210, 0, 0, 0 ) },
            { -1.0,  1.0, 0.0f, D3DCOLOR_ARGB ( 210, 0, 0, 0 ) },
            {  1.0,  1.0, 0.0f, D3DCOLOR_ARGB ( 210, 0, 0, 0 ) }
        };

        device->CreateVertexBuffer ( 4 * sizeof ( Vertex ),     // buffer size in bytes
                                     0,                         // memory usage flags
                                     Vertex::format,            // vertex format
                                     D3DPOOL_MANAGED,           // memory storage flags
                                     &background,               // pointer to IDirect3DVertexBuffer9
                                     0 );                       // unused

        void *ptr = 0;

        background->Lock ( 0, 0, ( void ** ) &ptr, 0 );
        memcpy ( ptr, verts, 4 * sizeof ( verts[0] ) );
        background->Unlock();
    }

    if ( overlayState == Overlay::Disabled )
        return;

    D3DVIEWPORT9 viewport;
    device->GetViewport ( &viewport );

    // Only draw in the main viewport; there should only be one with this width
    if ( viewport.Width != * CC_SCREEN_WIDTH_ADDR )
        return;

    // Scaling factor for the overlay background
    const float scaleY = float ( overlayHeight + 2 * TEXT_BORDER ) / viewport.Height;

    D3DXMATRIX translate, scale;
    D3DXMatrixScaling ( &scale, 1.0f, scaleY, 1.0f );
    D3DXMatrixTranslation ( &translate, 0.0f, 1.0f - scaleY, 0.0f );
    device->SetTransform ( D3DTS_VIEW, & ( scale = scale * translate ) );

    device->SetStreamSource ( 0, background, 0, sizeof ( Vertex ) );
    device->SetFVF ( Vertex::format );
    device->DrawPrimitive ( D3DPT_TRIANGLESTRIP, 0, 2 );

    // Only draw text if fully enabled
    if ( overlayState != Overlay::Enabled )
        return;

    if ( ! ( overlayText[0].empty() && overlayText[1].empty() && overlayText[2].empty() ) )
    {
        const int centerX = viewport.Width / 2;

        RECT rect;
        rect.left   = centerX - int ( ( viewport.Width / 2 ) * 1.0 ) + TEXT_BORDER;
        rect.right  = centerX + int ( ( viewport.Width / 2 ) * 1.0 ) - TEXT_BORDER;
        rect.top    = TEXT_BORDER;
        rect.bottom = rect.top + overlayHeight + TEXT_BORDER;

        if ( !overlayText[0].empty() )
            DrawText ( overlayText[0], rect, DT_LEFT, 255, 0, 0 );

        if ( !overlayText[1].empty() )
            DrawText ( overlayText[1], rect, DT_CENTER, 255, 0, 0 );

        if ( !overlayText[2].empty() )
            DrawText ( overlayText[2], rect, DT_RIGHT, 255, 0, 0 );
    }
}

void PresentFrameEnd ( IDirect3DDevice9 *device )
{
}

void InvalidateDeviceObjects()
{
    if ( initalizedDirectx )
    {
        initalizedDirectx = false;

        font->OnLostDevice();
        font = 0;

        background->Release();
        background = 0;
    }
}

#define WRITE_ASM_HACK(ASM_HACK)                                                                                    \
    do {                                                                                                            \
        int error = ASM_HACK.write();                                                                               \
        if ( error != 0 ) {                                                                                         \
            LOG ( "%s; %s failed; addr=%08x", WinException::getAsString ( error ), #ASM_HACK, ASM_HACK.addr );      \
            exit ( -1 );                                                                                            \
        }                                                                                                           \
    } while ( 0 )

extern void startStall();

extern void stopStall();


namespace DllHacks
{

void initializePreLoad()
{
    for ( const Asm& hack : hookMainLoop )
        WRITE_ASM_HACK ( hack );

    for ( const Asm& hack : enableDisabledStages )
        WRITE_ASM_HACK ( hack );

    for ( const Asm& hack : hijackControls )
        WRITE_ASM_HACK ( hack );

    for ( const Asm& hack : hijackMenu )
        WRITE_ASM_HACK ( hack );

    for ( const Asm& hack : detectRoundStart )
        WRITE_ASM_HACK ( hack );

    WRITE_ASM_HACK ( detectAutoReplaySave );

    // TODO find an alternative because this doesn't work on Wine
    // WRITE_ASM_HACK ( disableFpsLimit );
}


// Note: this is on the SAME thread as the main thread where callback happens
MH_WINAPI_HOOK ( LRESULT, CALLBACK, WindowProc, HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
    switch ( uMsg )
    {
        case WM_ENTERSIZEMOVE:  // This happens when the window is starting to be moved / resized
        case WM_STYLECHANGING:  // This happens when the window is starting to change fullscreen state
            startStall();
            break;

        case WM_EXITSIZEMOVE:   // This happens when the window is finished moving / resizing
        case WM_STYLECHANGED:   // This happens when the window is finished changing fullscreen state
            stopStall();
            break;
    }

    return oWindowProc ( hwnd, uMsg, wParam, lParam );
}


static pWindowProc WindowProc = 0;

static HHOOK keybdHook = 0;

static LRESULT CALLBACK keyboardCallback ( int, WPARAM, LPARAM );


void *mainWindowHandle = 0;


void initializePostLoad()
{
    *CC_DAMAGE_LEVEL_ADDR = 2;
    *CC_TIMER_SPEED_ADDR = 2;

    // *CC_DAMAGE_LEVEL_ADDR = 4;

    LOG ( "threadId=%08x", GetCurrentThreadId() );

    // Hook and ignore keyboard messages to prevent lag from unhandled messages
    if ( ! ( keybdHook = SetWindowsHookEx ( WH_KEYBOARD, keyboardCallback, 0, GetCurrentThreadId() ) ) )
        LOG ( "SetWindowsHookEx failed: %s", WinException::getLastError() );

    // Get the handle to the main window
    if ( ! ( mainWindowHandle = ProcessManager::findWindow ( CC_TITLE ) ) )
        LOG ( "Couldn't find window '%s'", CC_TITLE );

    // We can't save replays on Wine because MBAA crashes even without us.
    // We don't need to hook WindowProc on Wine because the game DOESN'T stop running if moving/resizing.
    // We can't hook DirectX calls on Wine (yet?).
    if ( ProcessManager::isWine() )
    {
        *CC_AUTO_REPLAY_SAVE_ADDR = 0;
        return;
    }

    // // Disable resizing (this has weird behaviour with the viewport size)
    // const DWORD dwStyle = GetWindowLong ( ( HWND ) mainWindowHandle, GWL_STYLE );
    // SetWindowLong ( ( HWND ) mainWindowHandle, GWL_STYLE, ( dwStyle | WS_BORDER ) & ~ WS_THICKFRAME );

    // Hook the game's WindowProc
    WindowProc = ( pWindowProc ) GetWindowLong ( ( HWND ) mainWindowHandle, GWL_WNDPROC );

    LOG ( "WindowProc=%08x", WindowProc );

    MH_STATUS status = MH_Initialize();
    if ( status != MH_OK )
        LOG ( "Initialize failed: %s", MH_StatusString ( status ) );

    status = MH_CREATE_HOOK ( WindowProc );
    if ( status != MH_OK )
        LOG ( "Create hook failed: %s", MH_StatusString ( status ) );

    status = MH_EnableHook ( ( void * ) WindowProc );
    if ( status != MH_OK )
        LOG ( "Enable hook failed: %s", MH_StatusString ( status ) );

    // Hook the game's DirectX calls
    string err;
    if ( ! ( err = InitDirectX ( mainWindowHandle ) ).empty() )
        LOG ( "InitDirectX failed: %s", err );
    else if ( ! ( err = HookDirectX() ).empty() )
        LOG ( "HookDirectX failed: %s", err );
}

void deinitialize()
{
    UnhookDirectX();

    if ( WindowProc )
    {
        MH_DisableHook ( ( void * ) WindowProc );
        MH_REMOVE_HOOK ( WindowProc );
        MH_Uninitialize();
        WindowProc = 0;
    }

    if ( keybdHook )
        UnhookWindowsHookEx ( keybdHook );

    for ( int i = hookMainLoop.size() - 1; i >= 0; --i )
        hookMainLoop[i].revert();
}

LRESULT CALLBACK keyboardCallback ( int code, WPARAM wParam, LPARAM lParam )
{
    // Pass through the Alt and Enter keys
    if ( code == HC_ACTION && ( wParam == VK_MENU || wParam == VK_RETURN ) )
        return CallNextHookEx ( 0, code, wParam, lParam );

    return 1;
}

} // namespace DllHacks
