// The panel's speaker: I2S straight into the onboard amplifier, no codec.
#pragma once

void soundBegin();       // amplifier off. Call once in setup()
// Plays the race-start clip (start_sound.h, see tools/make_sound.py) and
// returns at once; does nothing while one is playing, with no clip built in,
// or with SOUND_VOLUME 0. Safe to call from any task.
void soundRaceStart();
