// Opaque library handle types. aji.h declares them as
//   typedef class AJI_CHAIN * AJI_CHAIN_ID;
//   typedef class AJI_OPEN  * AJI_OPEN_ID;
// We forward-declare the classes instead of including aji.h, which needs
// build macros (PORT, UNIX, LINUX, WINDOWS, ...) that would leak into users'
// code. Only the wrapper's .cpp files include aji.h.
#pragma once

class AJI_CHAIN;
class AJI_OPEN;
