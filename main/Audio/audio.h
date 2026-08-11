#ifndef KAI_CAR_AUDIO_H
#define KAI_CAR_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SOUND_NONE = 0,
    SOUND_STARTUP,
    SOUND_DRIVING,
    SOUND_REVERSE,
    SOUND_SCREECH,
    SOUND_HONK,
} SoundEffect;

void initialize_audio(void);
void play_sound(SoundEffect effect);
void set_background_sound(SoundEffect effect);
void clear_background_sound(void);
void stop_sound(void);

#ifdef __cplusplus
}
#endif

#endif
