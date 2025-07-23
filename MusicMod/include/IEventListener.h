#pragma once

#include "ModEvents.h"

class IEventListener {
public:
    virtual void OnEvent(const ModEvent& event) = 0;
    virtual ~IEventListener() = default;
};