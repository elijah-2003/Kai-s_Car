#ifndef KAI_CAR_COMMAND_TRANSLATION_H
#define KAI_CAR_COMMAND_TRANSLATION_H

#include "drive.h"

#ifdef __cplusplus
extern "C" {
#endif

DriveCommand translate_command_string(const char* command);

#ifdef __cplusplus
}
#endif

#endif
