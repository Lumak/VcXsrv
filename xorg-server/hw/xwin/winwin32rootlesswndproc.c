/*
 *Copyright (C) 1994-2000 The XFree86 Project, Inc. All Rights Reserved.
 *
 *Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 *"Software"), to deal in the Software without restriction, including
 *without limitation the rights to use, copy, modify, merge, publish,
 *distribute, sublicense, and/or sell copies of the Software, and to
 *permit persons to whom the Software is furnished to do so, subject to
 *the following conditions:
 *
 *The above copyright notice and this permission notice shall be
 *included in all copies or substantial portions of the Software.
 *
 *THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *NONINFRINGEMENT. IN NO EVENT SHALL THE XFREE86 PROJECT BE LIABLE FOR
 *ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 *CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 *WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *Except as contained in this notice, the name of the XFree86 Project
 *shall not be used in advertising or otherwise to promote the sale, use
 *or other dealings in this Software without prior written authorization
 *from the XFree86 Project.
 *
 * Authors:	Kensuke Matsuzaki
 *		Earle F. Philhower, III
 *		Harold L Hunt II
 */
#ifdef HAVE_XWIN_CONFIG_H
#include <xwin-config.h>
#endif
#include "win.h"
#include <winuser.h>
#define _WINDOWSWM_SERVER_
#include <X11/extensions/windowswmstr.h>
#include "dixevents.h"
#include "propertyst.h"
#include <X11/Xatom.h>
#include "winmultiwindowclass.h"
#include "winmsg.h"
#include "inputstr.h"

/*
 * Constant defines
 */

#define MOUSE_ACTIVATE_DEFAULT		TRUE
#define RAISE_ON_CLICK_DEFAULT		FALSE

/*
 * Local globals
 */

static UINT_PTR g_uipMousePollingTimerID = 0;

/*
 * Local function
 */

DEFINE_ATOM_HELPER(AtmWindowsWMMouseActivate, WINDOWSWM_MOUSE_ACTIVATE)
/* DEFINE_ATOM_HELPER(AtmWindowsWMClientWindow, WINDOWSWM_CLIENT_WINDOW) */

/*
 * ConstrainSize - Taken from TWM sources - Respects hints for sizing
 */
#define makemult(a,b) ((b==1) ? (a) : (((int)((a)/(b))) * (b)) )
static void
ConstrainSize(WinXSizeHints hints, int *widthp, int *heightp)
{
    int minWidth, minHeight, maxWidth, maxHeight, xinc, yinc, delta;
    int baseWidth, baseHeight;
    int dwidth = *widthp, dheight = *heightp;

    if (hints.flags & PMinSize) {
        minWidth = hints.min_width;
        minHeight = hints.min_height;
    }
    else if (hints.flags & PBaseSize) {
        minWidth = hints.base_width;
        minHeight = hints.base_height;
    }
    else
        minWidth = minHeight = 1;

    if (hints.flags & PBaseSize) {
        baseWidth = hints.base_width;
        baseHeight = hints.base_height;
    }
    else if (hints.flags & PMinSize) {
        baseWidth = hints.min_width;
        baseHeight = hints.min_height;
    }
    else
        baseWidth = baseHeight = 0;

    if (hints.flags & PMaxSize) {
        maxWidth = hints.max_width;
        maxHeight = hints.max_height;
    }
    else {
        maxWidth = MAXINT;
        maxHeight = MAXINT;
    }

    if (hints.flags & PResizeInc) {
        xinc = hints.width_inc;
        yinc = hints.height_inc;
    }
    else
        xinc = yinc = 1;

    /*
     * First, clamp to min and max values
     */
    if (dwidth < minWidth)
        dwidth = minWidth;
    if (dheight < minHeight)
        dheight = minHeight;

    if (dwidth > maxWidth)
        dwidth = maxWidth;
    if (dheight > maxHeight)
        dheight = maxHeight;

    /*
     * Second, fit to base + N * inc
     */
    dwidth = ((dwidth - baseWidth) / xinc * xinc) + baseWidth;
    dheight = ((dheight - baseHeight) / yinc * yinc) + baseHeight;

    /*
     * Third, adjust for aspect ratio
     */

    /*
     * The math looks like this:
     *
     * minAspectX    dwidth     maxAspectX
     * ---------- <= ------- <= ----------
     * minAspectY    dheight    maxAspectY
     *
     * If that is multiplied out, then the width and height are
     * invalid in the following situations:
     *
     * minAspectX * dheight > minAspectY * dwidth
     * maxAspectX * dheight < maxAspectY * dwidth
     *
     */

    if (hints.flags & PAspect) {
        if (hints.min_aspect.x * dheight > hints.min_aspect.y * dwidth) {
            delta =
                makemult(hints.min_aspect.x * dheight / hints.min_aspect.y -
                         dwidth, xinc);
            if (dwidth + delta <= maxWidth)
                dwidth += delta;
            else {
                delta =
                    makemult(dheight -
                             dwidth * hints.min_aspect.y / hints.min_aspect.x,
                             yinc);
                if (dheight - delta >= minHeight)
                    dheight -= delta;
            }
        }

        if (hints.max_aspect.x * dheight < hints.max_aspect.y * dwidth) {
            delta =
                makemult(dwidth * hints.max_aspect.y / hints.max_aspect.x -
                         dheight, yinc);
            if (dheight + delta <= maxHeight)
                dheight += delta;
            else {
                delta =
                    makemult(dwidth -
                             hints.max_aspect.x * dheight / hints.max_aspect.y,
                             xinc);
                if (dwidth - delta >= minWidth)
                    dwidth -= delta;
            }
        }
    }

    /* Return computed values */
    *widthp = dwidth;
    *heightp = dheight;
}

#undef makemult

/*
 * ValidateSizing - Ensures size request respects hints
 */
static int
ValidateSizing(HWND hwnd, WindowPtr pWin, WPARAM wParam, LPARAM lParam)
{
    WinXSizeHints sizeHints;
    RECT *rect;
    int iWidth, iHeight, iTopBorder;
    POINT pt;

    /* Invalid input checking */
    if (pWin == NULL || lParam == 0) {
        ErrorF("Invalid input checking\n");
        return FALSE;
    }

    /* No size hints, no checking */
    if (!winMultiWindowGetWMNormalHints(pWin, &sizeHints)) {
        ErrorF("No size hints, no checking\n");
        return FALSE;
    }

    /* Avoid divide-by-zero */
    if (sizeHints.flags & PResizeInc) {
        if (sizeHints.width_inc == 0)
            sizeHints.width_inc = 1;
        if (sizeHints.height_inc == 0)
            sizeHints.height_inc = 1;
    }

    rect = (RECT *) lParam;

    iWidth = rect->right - rect->left;
    iHeight = rect->bottom - rect->top;

    /* Get title bar height, there must be an easier way?! */
    pt.x = pt.y = 0;
    ClientToScreen(hwnd, &pt);
    iTopBorder = pt.y - rect->top;

    /* Now remove size of any borders */
    iWidth -= 2 * GetSystemMetrics(SM_CXSIZEFRAME);
    iHeight -= GetSystemMetrics(SM_CYSIZEFRAME) + iTopBorder;

    /* Constrain the size to legal values */
    ConstrainSize(sizeHints, &iWidth, &iHeight);

    /* Add back the borders */
    iWidth += 2 * GetSystemMetrics(SM_CXSIZEFRAME);
    iHeight += GetSystemMetrics(SM_CYSIZEFRAME) + iTopBorder;

    /* Adjust size according to where we're dragging from */
    switch (wParam) {
    case WMSZ_TOP:
    case WMSZ_TOPRIGHT:
    case WMSZ_BOTTOM:
    case WMSZ_BOTTOMRIGHT:
    case WMSZ_RIGHT:
        rect->right = rect->left + iWidth;
        break;
    default:
        rect->left = rect->right - iWidth;
        break;
    }
    switch (wParam) {
    case WMSZ_BOTTOM:
    case WMSZ_BOTTOMRIGHT:
    case WMSZ_BOTTOMLEFT:
    case WMSZ_RIGHT:
    case WMSZ_LEFT:
        rect->bottom = rect->top + iHeight;
        break;
    default:
        rect->top = rect->bottom - iHeight;
        break;
    }
    return TRUE;
}

/*
 * IsMouseActive
 */

static Bool
IsMouseActive(WindowPtr pWin)
{

    struct _Window *pwin;
    struct _Property *prop;

    WindowPtr pRoot = GetCurrentRootWindow(g_pwinPointer);

    if (!pWin) {
        ErrorF("IsMouseActive - pWin was NULL use default value:%d\n",
               MOUSE_ACTIVATE_DEFAULT);
        return MOUSE_ACTIVATE_DEFAULT;
    }

    pwin = (struct _Window *) pWin;

    if (pwin->optional)
        prop = (struct _Property *) pwin->optional->userProps;
    else
        prop = NULL;

    while (prop) {
        if (prop->propertyName == AtmWindowsWMMouseActivate()
            && prop->type == XA_INTEGER && prop->format == 32) {
            return *(int *) prop->data;
        }
        else
            prop = prop->next;
    }

    if (pWin != pRoot) {
        return IsMouseActive(pRoot);
    }
    else {
#if CYGMULTIWINDOW_DEBUG
        winDebug("IsMouseActive - no prop use default value:%d\n",
                 MOUSE_ACTIVATE_DEFAULT);
#endif
        return MOUSE_ACTIVATE_DEFAULT;
    }
}

/*
 * winMWExtWMWindowProc - Window procedure
 */

LRESULT CALLBACK
winMWExtWMWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    WindowPtr pWin = NULL;
    win32RootlessWindowPtr pRLWinPriv = NULL;
    ScreenPtr pScreen = NULL;
    winPrivScreenPtr pScreenPriv = NULL;
    winScreenInfo *pScreenInfo = NULL;
    HWND hwndScreen = NULL;
    POINT ptMouse;
    static Bool s_fTracking = FALSE;
    HDC hdcUpdate;
    PAINTSTRUCT ps;
    LPWINDOWPOS pWinPos = NULL;
    RECT rcClient;

    /* Check if the Windows window property for our X window pointer is valid */
    if ((pRLWinPriv =
         (win32RootlessWindowPtr) GetProp(hwnd, WIN_WINDOW_PROP)) != NULL) {
        pWin = pRLWinPriv->pFrame->win;
        pScreen = pWin->drawable.pScreen;
        if (pScreen)
            pScreenPriv = winGetScreenPriv(pScreen);
        if (pScreenPriv)
            pScreenInfo = pScreenPriv->pScreenInfo;
        if (pScreenPriv)
            hwndScreen = pScreenPriv->hwndScreen;

        winDebugWin32Message("winMWExtWMWindowProc", hwnd, message, wParam,
                             lParam);

        winDebug("\thWnd %p\n", hwnd);
        winDebug("\tpScreenPriv %p\n", pScreenPriv);
        winDebug("\tpScreenInfo %p\n", pScreenInfo);
        winDebug("\thwndScreen %p\n", hwndScreen);
        winDebug("winMWExtWMWindowProc (%p) %08x %08x %08x\n",
                 pRLWinPriv, message, (int)wParam, (int)lParam);
    }
    /* Branch on message type */
    switch (message) {
    case WM_CREATE:
#if CYGMULTIWINDOW_DEBUG
        winDebug("winMWExtWMWindowProc - WM_CREATE\n");
#endif
        /* */
        SetProp(hwnd,
                WIN_WINDOW_PROP,
                (HANDLE) ((LPCREATESTRUCT) lParam)->lpCreateParams);
        return 0;

    case WM_CLOSE:
        winDebug("winMWExtWMWindowProc - WM_CLOSE %d\n", pRLWinPriv->fClose);

        /* Tell window-manager to close window */
        if (pRLWinPriv->fClose) {
            DestroyWindow(hwnd);
        }
        else {
#ifdef XWIN_MULTIWINDOWINTWM
#endif
            winWindowsWMSendEvent(WindowsWMControllerNotify,
                                  WindowsWMControllerNotifyMask,
                                  1,
                                  WindowsWMCloseWindow,
                                  pWin->drawable.id, 0, 0, 0, 0);
        }
        return 0;

    case WM_DESTROY:
#if CYGMULTIWINDOW_DEBUG
        winDebug("winMWExtWMWindowProc - WM_DESTROY\n");
#endif
        /* Free the shaodw DC; which allows the bitmap to be freed */
        DeleteDC(pRLWinPriv->hdcShadow);
        pRLWinPriv->hdcShadow = NULL;

        /* Free the shadow bitmap */
        DeleteObject(pRLWinPriv->hbmpShadow);
        pRLWinPriv->hbmpShadow = NULL;

        /* Free the screen DC */
        ReleaseDC(pRLWinPriv->hWnd, pRLWinPriv->hdcScreen);
        pRLWinPriv->hdcScreen = NULL;

        /* Free shadow buffer info header */
        free(pRLWinPriv->pbmihShadow);
        pRLWinPriv->pbmihShadow = NULL;

        pRLWinPriv->fResized = FALSE;
        pRLWinPriv->pfb = NULL;
        free(pRLWinPriv);
        RemoveProp(hwnd, WIN_WINDOW_PROP);
        break;

    case WM_MOUSEMOVE:
        if (wParam & (MK_LBUTTON|MK_RBUTTON|MK_MBUTTON))
        {
            if (lParam==GetWindowLongPtr(hwnd, WND_IDX_BUTTONDOWNLPARAM))
            {
                return 0;  /* Ignore the mouse since the mouse was not moved wrt the button down click */
            }
            SetWindowLongPtr(hwnd, WND_IDX_BUTTONDOWNLPARAM,-1);
        }
#if CYGMULTIWINDOW_DEBUG && 0
        winDebug("winMWExtWMWindowProc - WM_MOUSEMOVE\n");
#endif
        /* Unpack the client area mouse coordinates */
        ptMouse.x = GET_X_LPARAM(lParam);
        ptMouse.y = GET_Y_LPARAM(lParam);

        /* Translate the client area mouse coordinates to screen coordinates */
        ClientToScreen(hwnd, &ptMouse);

        /* Screen Coords from (-X, -Y) -> Root Window (0, 0) */
        ptMouse.x -= GetSystemMetrics(SM_XVIRTUALSCREEN);
        ptMouse.y -= GetSystemMetrics(SM_YVIRTUALSCREEN);

        /* We can't do anything without privates */
        if (pScreenPriv == NULL || pScreenInfo->fIgnoreInput)
            break;

        /* Has the mouse pointer crossed screens? */
        if (pScreen != miPointerGetScreen(g_pwinPointer))
             miPointerSetScreen(g_pwinPointer, pScreenInfo->dwScreen,
                                ptMouse.x - pScreenInfo->dwXOffset,
                                ptMouse.y - pScreenInfo->dwYOffset);

        /* Are we tracking yet? */
        if (!s_fTracking) {
            TRACKMOUSEEVENT tme;

            /* Setup data structure */
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;

            /* Call the tracking function */
            if (!TrackMouseEvent(&tme))
                ErrorF("winMWExtWMWindowProc - TrackMouseEvent failed\n");

            /* Flag that we are tracking now */
            s_fTracking = TRUE;
        }

        /* Kill the timer used to poll mouse events */
        if (g_uipMousePollingTimerID != 0) {
            KillTimer(pScreenPriv->hwndScreen, WIN_POLLING_MOUSE_TIMER_ID);
            g_uipMousePollingTimerID = 0;
        }

        /* Deliver absolute cursor position to X Server */
        winEnqueueMotion(ptMouse.x - pScreenInfo->dwXOffset,
                         ptMouse.y - pScreenInfo->dwYOffset);

        return 0;

    case WM_NCMOUSEMOVE:
#if CYGMULTIWINDOW_DEBUG && 0
        winDebug("winMWExtWMWindowProc - WM_NCMOUSEMOVE\n");
#endif
        /*
         * We break instead of returning 0 since we need to call
         * DefWindowProc to get the mouse cursor changes
         * and min/max/close button highlighting in Windows XP.
         * The Platform SDK says that you should return 0 if you
         * process this message, but it fails to mention that you
         * will give up any default functionality if you do return 0.
         */

        /* We can't do anything without privates */
        if (pScreenPriv == NULL || pScreenInfo->fIgnoreInput)
            break;

        /*
         * Timer to poll mouse events.  This is needed to make
         * programs like xeyes follow the mouse properly.
         */
        if (g_uipMousePollingTimerID == 0)
            g_uipMousePollingTimerID = SetTimer(pScreenPriv->hwndScreen,
                                                WIN_POLLING_MOUSE_TIMER_ID,
                                                MOUSE_POLLING_INTERVAL, NULL);
        break;

    case WM_MOUSELEAVE:
#if CYGMULTIWINDOW_DEBUG
        winDebug("winMWExtWMWindowProc - WM_MOUSELEAVE\n");
#endif
        /* Mouse has left our client area */

        /* Flag that we are no longer tracking */
        s_fTracking = FALSE;

        /*
         * Timer to poll mouse events.  This is needed to make
         * programs like xeyes follow the mouse properly.
         */
        if (g_uipMousePollingTimerID == 0)
            g_uipMousePollingTimerID = SetTimer(pScreenPriv->hwndScreen,
                                                WIN_POLLING_MOUSE_TIMER_ID,
                                                MOUSE_POLLING_INTERVAL, NULL);
        return 0;

    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONDOWN:
        SetWindowLongPtr(hwnd, WND_IDX_BUTTONDOWNLPARAM,lParam);
#if CYGMULTIWINDOW_DEBUG
        winDebug("winMWExtWMWindowProc - WM_LBUTTONDBLCLK\n");
#endif
        if (pScreenPriv == NULL || pScreenInfo->fIgnoreInput)
            break;
        SetCapture(hwnd);
        return winMouseButtonsHandle(pScreen, ButtonPress, Button1, wParam);

    case WM_LBUTTONUP:
#if CYGMULTIWINDOW_DEBUG
        winDebug("winMWExtWMWindowProc - WM_LBUTTONUP\n");
#endif
        if (pScreenPriv == NULL || pScreenInfo->fIgnoreInput)
            break;
        ReleaseCapture();
        return winMouseButtonsHandle(pScreen, ButtonRelease, Button1, wParam);

    case WM_MBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
        SetWindowLongPtr(hwnd, WND_IDX_BUTTONDOWNLPARAM,lParam);
#if CYGMULTIWINDOW_DEBUG
        winDebug("winMWExtWMWindowProc - WM_MBUTTONDBLCLK\n");
#endif
        if (pScreenPriv == NULL || pScreenInfo->fIgnoreInput)
            break;
        SetCapture(hwnd);
        return winMouseButtonsHandle(pScreen, ButtonPress, Button2, wParam);

    case WM_MBUTTONUP:
#if CYGMULTIWINDOW_DEBUG
        winDebug("winMWExtWMWindowProc - WM_MBUTTONUP\n");
#endif
        if (pScreenPriv == NULL || pScreenInfo->fIgnoreInput)
            break;
        ReleaseCapture();
        return winMouseButtonsHandle(pScreen, ButtonRelease, Button2, wParam);

    case WM_RBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
        SetWindowLongPtr(hwnd, WND_IDX_BUTTONDOWNLPARAM,lParam);
#if CYGMULTIWINDOW_DEBUG
        winDebug("winMWExtWMWindowProc - WM_RBUTTONDBLCLK\n");
#endif
        if (pScreenPriv == NULL || pScreenInfo->fIgnoreInput)
            break;
        SetCapture(hwnd);
        return winMouseButtonsHandle(pScreen, ButtonPress, Button3, wParam);

    case WM_RBUTTONUP:
#if CYGMULTIWINDOW_DEBUG
        winDebug("winMWExtWMWindowProc - WM_RBUTTONUP\n");
#endif
        if (pScreenPriv == NULL || pScreenInfo->fIgnoreInput)
            break;
        ReleaseCapture();
        return winMouseButtonsHandle(pScreen, ButtonRelease, Button3, wParam);

    case WM_XBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
        SetWindowLongPtr(hwnd, WND_IDX_BUTTONDOWNLPARAM,lParam);
        if (pScreenPriv == NULL || pScreenInfo->fIgnoreInput)
            break;
        SetCapture(hwnd);
        return winMouseButtonsHandle(pScreen, ButtonPress, HIWORD(wParam) + 7,
                                     wParam);
    case WM_XBUTTONUP:
        if (pScreenPriv == NULL || pScreenInfo->fIgnoreInput)
            break;
        ReleaseCapture();
        return winMouseButtonsHandle(pScreen, ButtonRelease, HIWORD(wParam) + 7,
                                     wParam);

    case WM_MOUSEWHEEL:
#if CYGMULTIWINDOW_DEBUG
        winDebug("winMWExtWMWindowProc - WM_MOUSEWHEEL\n");
#endif

        /* Pass the message to the root window */
        SendMessage(hwndScreen, message, wParam, lParam);
        return 0;

    case WM_MOUSEHWHEEL:
#if CYGMULTIWINDOW_DEBUG
        winDebug("winMWExtWMWindowProc - WM_MOUSEHWHEEL\n");
#endif

        /* Pass the message to the root window */
        SendMessage(hwndScreen, message, wParam, lParam);
        return 0;

    case WM_MOUSEACTIVATE:
        winDebug("winMWExtWMWindowProc - WM_MOUSEACTIVATE\n");

        if (!IsMouseActive(pWin))
            return MA_NOACTIVATE;

        break;

    case WM_KILLFOCUS:
        /* Pop any pressed keys since we are losing keyboard focus */
        winKeybdReleaseKeys();
        return 0;

    case WM_SYSDEADCHAR:
    case WM_DEADCHAR:
        /*
         * NOTE: We do nothing with WM_*CHAR messages,
         * nor does the root window, so we can just toss these messages.
         */
        return 0;

    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
#if CYGMULTIWINDOW_DEBUG
        winDebug("winMWExtWMWindowProc - WM_*KEYDOWN\n");
#endif

        /*
         * Don't pass Alt-F4 key combo to root window,
         * let Windows translate to WM_CLOSE and close this top-level window.
         *
         * NOTE: We purposely don't check the fUseWinKillKey setting because
         * it should only apply to the key handling for the root window,
         * not for top-level window-manager windows.
         *
         * ALSO NOTE: We do pass Ctrl-Alt-Backspace to the root window
         * because that is a key combo that no X app should be expecting to
         * receive, since it has historically been used to shutdown the X server.
         * Passing Ctrl-Alt-Backspace to the root window preserves that
         * behavior, assuming that -unixkill has been passed as a parameter.
         */
        if (wParam == VK_F4 && (GetKeyState(VK_MENU) & 0x8000))
            break;

        /* Pass the message to the root window */
        SendMessage(hwndScreen, message, wParam, lParam);
        return 0;

    case WM_SYSKEYUP:
    case WM_KEYUP:

#if CYGMULTIWINDOW_DEBUG
        winDebug("winMWExtWMWindowProc - WM_*KEYUP\n");
#endif

        /* Pass the message to the root window */
        SendMessage(hwndScreen, message, wParam, lParam);
        return 0;

    case WM_HOTKEY:
#if CYGMULTIWINDOW_DEBUG
        winDebug("winMWExtWMWindowProc - WM_HOTKEY\n");
#endif

        /* Pass the message to the root window */
        SendMessage(hwndScreen, message, wParam, lParam);
        return 0;

    case WM_ERASEBKGND:
        winDebug("winMWExtWMWindowProc - WM_ERASEBKGND\n");
        /*
         * Pretend that we did erase the background but we don't care,
         * since we repaint the entire region anyhow
         * This avoids some flickering when resizing.
         */
        return TRUE;

    case WM_PAINT:

        /* BeginPaint gives us an hdc that clips to the invalidated region */
        hdcUpdate = BeginPaint(hwnd, &ps);

        /* Try to copy from the shadow buffer */
        if (!BitBlt(hdcUpdate,
                    ps.rcPaint.left, ps.rcPaint.top,
                    ps.rcPaint.right - ps.rcPaint.left,
                    ps.rcPaint.bottom - ps.rcPaint.top,
                    pRLWinPriv->hdcShadow,
                    ps.rcPaint.left, ps.rcPaint.top, SRCCOPY)) {
            LPVOID lpMsgBuf;
            DWORD errcode = GetLastError();

            /* Display a fancy error message */
            FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                          FORMAT_MESSAGE_FROM_SYSTEM |
                          FORMAT_MESSAGE_IGNORE_INSERTS,
                          NULL,
                          errcode,
                          MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                          (LPTSTR) &lpMsgBuf, 0, NULL);

            if (errcode) ErrorF("winMWExtWMWindowProc - BitBlt failed: %s\n",
                   (LPSTR) lpMsgBuf);
            LocalFree(lpMsgBuf);
        }

        /* EndPaint frees the DC */
        EndPaint(hwnd, &ps);
        break;

    case WM_ACTIVATE:
        winDebug("winMWExtWMWindowProc - WM_ACTIVATE\n");
        if (LOWORD(wParam) != WA_INACTIVE) {
            winWindowsWMSendEvent(WindowsWMControllerNotify,
                                  WindowsWMControllerNotifyMask,
                                  1,
                                  WindowsWMActivateWindow,
                                  pWin->drawable.id, 0, 0, 0, 0);
        }
        return 0;

#if 1
    case WM_WINDOWPOSCHANGING:
        pWinPos = (LPWINDOWPOS) lParam;
        if (!(pWinPos->flags & SWP_NOZORDER)) {
            if (pRLWinPriv->fRestackingNow || pScreenPriv->fRestacking) {
                winDebug("Win %p is now restacking.\n",
                         pRLWinPriv);
                break;
            }

            winDebug("Win %p forbid to change z order (%p).\n",
                     pRLWinPriv,
                     pWinPos->hwndInsertAfter);
            pWinPos->flags |= SWP_NOZORDER;
        }
        break;
#endif

    case WM_MOVE:
#if CYGMULTIWINDOW_DEBUG
        winDebug("winMWExtWMWindowProc - WM_MOVE\n");
#endif
        if (g_fNoConfigureWindow)
            break;
#if 0
        /* Bail if Windows window is not actually moving */
        if (pRLWinPriv->dwX == (short) LOWORD(lParam)
            && pRLWinPriv->dwY == (short) HIWORD(lParam))
            break;

        /* Also bail if we're maximizing, we'll do the whole thing in WM_SIZE */
        {
            WINDOWPLACEMENT windPlace;

            windPlace.length = sizeof(WINDOWPLACEMENT);

            /* Get current window placement */
            GetWindowPlacement(hwnd, &windPlace);

            /* Bail if maximizing */
            if (windPlace.showCmd == SW_MAXIMIZE
                || windPlace.showCmd == SW_SHOWMAXIMIZED)
                break;
        }
#endif

        winDebug("\t(%d, %d)\n", (short) LOWORD(lParam),
                 (short) HIWORD(lParam));
        if (!pRLWinPriv->fMovingOrSizing) {
            winMWExtWMMoveXWindow(pWin, (LOWORD(lParam) - wBorderWidth(pWin)
                                         - GetSystemMetrics(SM_XVIRTUALSCREEN)),
                                  (HIWORD(lParam) - wBorderWidth(pWin)
                                   - GetSystemMetrics(SM_YVIRTUALSCREEN)));
        }
        return 0;

    case WM_SHOWWINDOW:
        winDebug("winMWExtWMWindowProc - WM_SHOWWINDOW\n");

        /* Bail out if the window is being hidden */
        if (!wParam)
            return 0;

        winMWExtWMUpdateWindowDecoration(pRLWinPriv, pScreenInfo);

        break;

    case WM_SIZING:
        /* Need to legalize the size according to WM_NORMAL_HINTS */
        /* for applications like xterm */
        return ValidateSizing(hwnd, pWin, wParam, lParam);

    case WM_WINDOWPOSCHANGED:
    {
        pWinPos = (LPWINDOWPOS) lParam;
#ifdef _DEBUG
        winDebug("winMWExtWMWindowProc - WM_WINDOWPOSCHANGED\n");
        winDebug("\tflags: %s%s%s%s%s%s%s%s%s%s%s%s\n",
                 (pWinPos->flags & SWP_DRAWFRAME) ? "SWP_DRAWFRAME " : "",
                 (pWinPos->flags & SWP_FRAMECHANGED) ? "SWP_FRAMECHANGED " : "",
                 (pWinPos->flags & SWP_HIDEWINDOW) ? "SWP_HIDEWINDOW " : "",
                 (pWinPos->flags & SWP_NOACTIVATE) ? "SWP_NOACTIVATE " : "",
                 (pWinPos->flags & SWP_NOCOPYBITS) ? "SWP_NOCOPYBITS " : "",
                 (pWinPos->flags & SWP_NOMOVE) ? "SWP_NOMOVE " : "",
                 (pWinPos->
                  flags & SWP_NOOWNERZORDER) ? "SWP_NOOWNERZORDER " : "",
                 (pWinPos->flags & SWP_NOSIZE) ? "SWP_NOSIZE " : "",
                 (pWinPos->flags & SWP_NOREDRAW) ? "SWP_NOREDRAW " : "",
                 (pWinPos->
                  flags & SWP_NOSENDCHANGING) ? "SWP_NOSENDCHANGING " : "",
                 (pWinPos->flags & SWP_NOZORDER) ? "SWP_NOZORDER " : "",
                 (pWinPos->flags & SWP_SHOWWINDOW) ? "SWP_SHOWWINDOW " : "");
        winDebug("\tno_configure: %s\n", (g_fNoConfigureWindow ? "Yes" : "No"));
        winDebug("\textend: (%d, %d, %d, %d)\n",
                 pWinPos->x, pWinPos->y, pWinPos->cx, pWinPos->cy);

#endif
        if (pWinPos->flags & SWP_HIDEWINDOW)
            break;

        if (!(pWinPos->flags & SWP_NOSIZE)) {
            if (IsIconic(hwnd)) {
                winDebug("\tIconic -> MINIMIZED\n");

                winWindowsWMSendEvent(WindowsWMControllerNotify,
                                      WindowsWMControllerNotifyMask,
                                      1,
                                      WindowsWMMinimizeWindow,
                                      pWin->drawable.id, 0, 0, 0, 0);
            }
            else if (IsZoomed(hwnd)) {
                winDebug("\tZoomed -> MAXIMIZED\n");

                winWindowsWMSendEvent(WindowsWMControllerNotify,
                                      WindowsWMControllerNotifyMask,
                                      1,
                                      WindowsWMMaximizeWindow,
                                      pWin->drawable.id, 0, 0, 0, 0);
            }
            else {
                winDebug("\tnone -> RESTORED\n");

                winWindowsWMSendEvent(WindowsWMControllerNotify,
                                      WindowsWMControllerNotifyMask,
                                      1,
                                      WindowsWMRestoreWindow,
                                      pWin->drawable.id, 0, 0, 0, 0);
            }
        }
        if (!g_fNoConfigureWindow) {

            if (!pRLWinPriv->fMovingOrSizing
                /*&& (pWinPos->flags & SWP_SHOWWINDOW) */ ) {
                GetClientRect(hwnd, &rcClient);
                MapWindowPoints(hwnd, HWND_DESKTOP, (LPPOINT) &rcClient, 2);

                if (!(pWinPos->flags & SWP_NOMOVE)
                    && !(pWinPos->flags & SWP_NOSIZE)) {
                    winDebug("\tmove & resize\n");

                    winMWExtWMMoveResizeXWindow(pWin,
                                                rcClient.left -
                                                wBorderWidth(pWin)
                                                -
                                                GetSystemMetrics
                                                (SM_XVIRTUALSCREEN),
                                                rcClient.top -
                                                wBorderWidth(pWin)
                                                -
                                                GetSystemMetrics
                                                (SM_YVIRTUALSCREEN),
                                                rcClient.right - rcClient.left -
                                                wBorderWidth(pWin) * 2,
                                                rcClient.bottom - rcClient.top -
                                                wBorderWidth(pWin) * 2);
                }
                else if (!(pWinPos->flags & SWP_NOMOVE)) {

                    winMWExtWMMoveXWindow(pWin,
                                          rcClient.left - wBorderWidth(pWin)
                                          - GetSystemMetrics(SM_XVIRTUALSCREEN),
                                          rcClient.top - wBorderWidth(pWin)
                                          -
                                          GetSystemMetrics(SM_YVIRTUALSCREEN));
                }
                else if (!(pWinPos->flags & SWP_NOSIZE)) {
                    winDebug("\tresize\n");

                    winMWExtWMResizeXWindow(pWin,
                                            rcClient.right - rcClient.left
                                            - wBorderWidth(pWin) * 2,
                                            rcClient.bottom - rcClient.top
                                            - wBorderWidth(pWin) * 2);
                }
            }
        }
    }
        winDebug("winMWExtWMWindowProc - WM_WINDOWPOSCHANGED - done.\n");

        return 0;

    case WM_SIZE:
        /* see dix/window.c */
        /* FIXME: Maximize/Restore? */
        winDebug("winMWExtWMWindowProc - WM_SIZE\n");

        winDebug("\t(%d, %d) %d\n", (short) LOWORD(lParam),
                 (short) HIWORD(lParam), g_fNoConfigureWindow);
        if (g_fNoConfigureWindow)
            break;

        /* Branch on type of resizing occurring */
        switch (wParam) {
        case SIZE_MINIMIZED:
            winDebug("\tSIZE_MINIMIZED\n");


            winWindowsWMSendEvent(WindowsWMControllerNotify,
                                  WindowsWMControllerNotifyMask,
                                  1,
                                  WindowsWMMinimizeWindow,
                                  pWin->drawable.id,
                                  0, 0, LOWORD(lParam), HIWORD(lParam));
            break;

        case SIZE_RESTORED:
            winDebug("\tSIZE_RESTORED\n");

            winWindowsWMSendEvent(WindowsWMControllerNotify,
                                  WindowsWMControllerNotifyMask,
                                  1,
                                  WindowsWMRestoreWindow,
                                  pWin->drawable.id,
                                  0, 0, LOWORD(lParam), HIWORD(lParam));
            break;

        case SIZE_MAXIMIZED:
#if CYGMULTIWINDOW_DEBUG
            winDebug("\tSIZE_MAXIMIZED\n");
#endif
            winWindowsWMSendEvent(WindowsWMControllerNotify,
                                  WindowsWMControllerNotifyMask,
                                  1,
                                  WindowsWMMaximizeWindow,
                                  pWin->drawable.id,
                                  0, 0, LOWORD(lParam), HIWORD(lParam));
            break;
        }

        /* Perform the resize and notify the X client */
        if (!pRLWinPriv->fMovingOrSizing) {
            winMWExtWMResizeXWindow(pWin, (short) LOWORD(lParam)
                                    - wBorderWidth(pWin) * 2,
                                    (short) HIWORD(lParam)
                                    - wBorderWidth(pWin) * 2);
        }
        break;

    case WM_ACTIVATEAPP:
#if CYGMULTIWINDOW_DEBUG
        winDebug("winMWExtWMWindowProc - WM_ACTIVATEAPP\n");
#endif
        if (wParam) {
            winWindowsWMSendEvent(WindowsWMActivationNotify,
                                  WindowsWMActivationNotifyMask,
                                  1,
                                  WindowsWMIsActive,
                                  pWin->drawable.id, 0, 0, 0, 0);
        }
        else {
            winWindowsWMSendEvent(WindowsWMActivationNotify,
                                  WindowsWMActivationNotifyMask,
                                  1,
                                  WindowsWMIsInactive,
                                  pWin->drawable.id, 0, 0, 0, 0);
        }
        break;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            if (!g_fSoftwareCursor)
                SetCursor(pScreenPriv->cursor.handle);
            return TRUE;
        }
        break;

    case WM_ENTERSIZEMOVE:
        winDebug("winMWExtWMWindowProc - WM_ENTERSIZEMOVE\n");

        pRLWinPriv->fMovingOrSizing = TRUE;
        break;

    case WM_EXITSIZEMOVE:
        winDebug("winMWExtWMWindowProc - WM_EXITSIZEMOVE\n");

        pRLWinPriv->fMovingOrSizing = FALSE;

        GetClientRect(hwnd, &rcClient);

        MapWindowPoints(hwnd, HWND_DESKTOP, (LPPOINT) &rcClient, 2);

        winMWExtWMMoveResizeXWindow(pWin, rcClient.left - wBorderWidth(pWin)
                                    - GetSystemMetrics(SM_XVIRTUALSCREEN),
                                    rcClient.top - wBorderWidth(pWin)
                                    - GetSystemMetrics(SM_YVIRTUALSCREEN),
                                    rcClient.right - rcClient.left
                                    - wBorderWidth(pWin) * 2,
                                    rcClient.bottom - rcClient.top
                                    - wBorderWidth(pWin) * 2);
        break;

    default:
        break;
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}
