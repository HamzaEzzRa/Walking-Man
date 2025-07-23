#pragma once

class FunctionHook {
protected:
    typedef void* (*GenericFunction_t)(void*, void*, void*, void*);
    typedef void (*GenericInstruction_t)();
};