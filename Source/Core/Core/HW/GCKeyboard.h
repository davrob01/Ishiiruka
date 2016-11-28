// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "Common/CommonTypes.h"

class InputConfig;
struct KeyboardStatus;

namespace Keyboard
{
void Shutdown();
void Initialize();
void LoadConfig();

InputConfig* GetConfig();

KeyboardStatus GetStatus(int port);
}
