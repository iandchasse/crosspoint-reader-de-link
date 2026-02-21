#pragma once

#ifdef FRONTLIGHT_PRESENT

#include <FrontlightManager.h>

// Global instance of the frontlight manager so it can be accessed
// from any activity without needing to pass it through constructors.
extern FrontlightManager frontlightManager;

#endif  // FRONTLIGHT_PRESENT
