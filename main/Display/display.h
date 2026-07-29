#ifndef KAI_CAR_DISPLAY_H
#define KAI_CAR_DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FACE_HAPPY = 0,
    FACE_LOVE,
    FACE_CONNECTED,
    FACE_DRIVING,
    FACE_SLEEP,
    FACE_SAD,
} FaceType;

void initialize_display(void);
void show_face(FaceType face);
void show_battery_percentage(unsigned int percentage);
void show_greeting(const char* name);

#ifdef __cplusplus
}
#endif

#endif
