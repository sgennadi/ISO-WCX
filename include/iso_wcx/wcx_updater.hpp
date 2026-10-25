#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace iso_wcx {
void configure_updates(HWND parent, HINSTANCE module);
}
#endif
