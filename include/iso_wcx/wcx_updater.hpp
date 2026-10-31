#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace iso_wcx {
void configure_updates(HWND parent, HINSTANCE module);
// Built-in, confirmed, backup-first WCX association repair. No helper EXE.
void repair_tc_registrations(HWND parent,HINSTANCE module,bool russian,
                             bool quiet_when_clean=false);
// On first WIM open, detect stale own registrations and offer a confirmed
// backup-first repair; never write to another plugin's association.
void offer_tc_association_repair_once();
void schedule_update_check();
}
#endif
