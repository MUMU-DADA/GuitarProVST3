// P10 editor regression entry point. The production lifecycle test already
// lives in p8_runtime_test.cpp and exercises the same RuntimeEffect through
// a real Qt native HWND. Keeping this thin wrapper gives the P10 suite a
// stable, explicit build target without duplicating the host fixture.
#include "p8_runtime_test.cpp"
