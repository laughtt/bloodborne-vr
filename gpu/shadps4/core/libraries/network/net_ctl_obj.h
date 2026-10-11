// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include "common/types.h"

namespace Libraries::NetCtl {

using OrbisNetCtlCallback = PS4_SYSV_ABI void (*)(int eventType, void* arg);
using OrbisNetCtlCallbackForNpToolkit = PS4_SYSV_ABI void (*)(int eventType, void* arg);

struct NetCtlCallback {
    OrbisNetCtlCallback func;
    void* arg;
};

struct NetCtlCallbackForNpToolkit {
    OrbisNetCtlCallbackForNpToolkit func;
    void* arg;
};

class NetCtlInternal {
public:
    explicit NetCtlInternal();
    ~NetCtlInternal();

    s32 RegisterCallback(OrbisNetCtlCallback func, void* arg);
    s32 RegisterNpToolkitCallback(OrbisNetCtlCallbackForNpToolkit func, void* arg);
    void CheckCallback();
    // BB_NETCTL_DELIVER=1: like the real libSceNetCtl, deliver an event to each registered callback only
    // when the state differs from the last one it was given (the first check after registration
    // delivers the current state).
    void DeliverChanges();
    void CheckNpToolkitCallback();

public:
    std::array<NetCtlCallbackForNpToolkit, 8> nptool_callbacks{};
    std::array<NetCtlCallback, 8> callbacks{};
    std::array<int, 8> last_event{-1, -1, -1, -1, -1, -1, -1, -1};
    std::mutex m_mutex;
};
} // namespace Libraries::NetCtl
