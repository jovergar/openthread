// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once

#include <windows.h>
#include <winnt.h>
#include <new>
#include <vector>
#include <tuple>

// Define to export necessary functions
#define OTDLL
#define OTAPI EXTERN_C __declspec(dllexport)

#include <openthread.h>
#include <platform/logging-windows.h>

#include <winioctl.h>
#include <otLwfIoctl.h>
#include <rtlrefcount.h>

EXTERN_C
{
#include <ws2def.h>
#include <ws2ipdef.h>
#include <mstcpip.h>
}
