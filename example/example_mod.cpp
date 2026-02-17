#include "example_mod.h"
#include "nise/stub.h"

// Simple mod API function
extern "C" void StopAllMusic()
{
    // Stop all currently playing FMOD sounds
    FMODHook::getInstance().stopAll();
}
