#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace iso_wcx {
void configure_updates(HWND parent, HINSTANCE module);
// Built-in, confirmed, backup-first WCX association repair. No helper EXE.
void repair_tc_registrations(HWND parent,HINSTANCE module,bool russian);
void schedule_update_check();
}
#endif
