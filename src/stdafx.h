#pragma once

#ifdef __cplusplus
// winsock2.h must be included BEFORE anything that pulls in winsock.h
#include <winsock2.h>
// Prevent the old winsock.h from being included (avoids redefinition conflicts)
#define _WINSOCKAPI_
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <SDK/foobar2000.h>
#include <helpers/input_helpers.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <memory>
#include <string>
#endif
