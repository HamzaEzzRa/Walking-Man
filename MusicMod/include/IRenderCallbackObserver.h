#pragma once

#include <string>

class IRenderCallbackObserver
{
public:
	virtual void OnSetup() {};
	virtual void OnRender() {};
};