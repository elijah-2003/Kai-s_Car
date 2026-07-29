#ifndef KAI_CAR_AUDIO_H
#define KAI_CAR_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SOUND_NONE = 0,
    SOUND_STARTUP,
    SOUND_HONK,
    SOUND_REVERSE_BEEP,
    SOUND_DRIVE_HUM,
    SOUND_TURN_TICK,
    SOUND_LOW_BATTERY,
    SOUND_CONNECTED,
} SoundEffect;

void initialize_audio(void);

/* Play a sound effect. Non-blocking — runs on the audio task.
   Calling again replaces the current sound. */
void play_sound(SoundEffect effect);

/* Set a looping background sound (drive hum, reverse beep, etc).
   Auto-resumes after one-shot sounds (like honk) finish. */
void set_background_sound(SoundEffect effect);

/* Clear the background sound. */
void clear_background_sound(void);

/* Stop whatever is currently playing and clear background. */
void stop_sound(void);

#ifdef __cplusplus
}
#endif

#endif
