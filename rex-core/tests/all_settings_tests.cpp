// This file includes all per-phase test files to ensure they are in a single
// translation unit, preventing the MSVC linker from stripping static test
// registration initializers across separate TUs.
#include "test_settings_phase0.cpp"
#include "test_settings_phase1.cpp"
#include "test_settings_phase2.cpp"
#include "test_settings_phase3.cpp"
