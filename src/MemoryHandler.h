#pragma once

#include "Type.h"


void* allocAlignedStd(size_t, size_t);
void  freeAlignedStd(void*);

void *allocAlignedLargePages(size_t);
void  freeAlignedLargePages(void*);

/// Win Processors Group
/// Under Windows it is not possible for a process to run on more than one logical processor group.
/// This usually means to be limited to use max 64 cores.
/// To overcome this, some special platform specific API should be called to set group affinity for each thread.
/// Original code from Texel by Peter Osterlund.
namespace WinProcGroup {

    extern void bind(u16);
}
