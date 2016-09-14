/* Copyright (C) 2009 Jatin Golani.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 * Except as contained in this notice, the names of the authors or their
 * institutions shall not be used in advertising or otherwise to promote the
 * sale, use or other dealings in this Software without prior written
 * authorization from the authors.
 */


#ifndef _XCB_WINDEFS_H
#define _XCB_WINDEFS_H

#ifndef WINVER
#define WINVER 0x0501 /* required for getaddrinfo/freeaddrinfo defined only for WinXP and above */
#endif

#define INCL_WINSOCK_API_TYPEDEFS 1 /* Needed for LPFN_GETPEERNAME */

#define FD_SETSIZE 1024

#include <X11/Xwinsock.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windef.h>

typedef unsigned char BYTE;

typedef unsigned int in_addr_t;

#define HANDLE void *
typedef int pid_t;

#define STDERR_FILENO 2

#ifdef LIBXCB_DLL
#define XCB_EXTERN __declspec(dllexport) extern
#else
#define XCB_EXTERN __declspec(dllimport) extern
#endif

#endif /* xcb_windefs.h */
