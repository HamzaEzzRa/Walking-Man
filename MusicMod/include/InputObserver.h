#pragma once  

#include "InputCode.h"  

class InputObserver  
{  
public:  
    virtual void OnKeyPress(const InputCode&) = 0;
    virtual void OnKeyDown(const InputCode&) = 0;
    virtual void OnKeyUp(const InputCode&) = 0;
    virtual ~InputObserver() = default;
};