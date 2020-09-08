#pragma once

#ifdef NOWL
#include <X11/keysym.h>
#define KEY(name) (XK_##name)
#else
#include <xkbcommon/xkbcommon.h>
#define KEY(name) (XKB_KEY_##name)
#endif
