#pragma once
/* Sounds the host plays itself, mixed into the game's output (audio_mixer.c, Breath of the Wild builds): Tears of the
   Kingdom's own sounds for the abilities (botw_abilities.c), decoded from the user's dump into
   <data>/abilities/sound/<name>.wav by games/botw/scripts/extract_totk_ability_sounds.py, with <name>.loop holding the
   loop's first and end frame where the sound loops. Any thread. */
int host_sound_have(const char *name);   /* 1 when the sound's file is there (loads it once) */
int host_sound_play(const char *name, float volume, float pan, int loop);   /* a voice, or 0; pan -1 left .. 1 right */
void host_sound_set(int voice, float volume, float pan);
void host_sound_stop(int voice, float fade_seconds);   /* a looping voice plays on until stopped */
void host_sound_mix(float *lr, int frames, int rate, float gain);   /* audio_mixer.c: adds the voices to a stereo frame */
