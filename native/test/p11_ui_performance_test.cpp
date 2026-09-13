// Keep the existing Qt fixture as the P11 UI lifecycle gate. P11 changes the
// scheduling path around the same About/sidebar controls, so the established
// P9 assertions remain the meaningful functional regression.
#include "p9_ui_test.cpp"
