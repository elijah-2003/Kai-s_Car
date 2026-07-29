#include "command_translation.h"

#include <cstring>

DriveCommand translate_command_string(const char* command)
{
    if (command == nullptr) {
        return DRIVE_STOP;
    }

    if (std::strcmp(command, "forward") == 0) {
        return DRIVE_FORWARD;
    }

    if (std::strcmp(command, "backward") == 0 || std::strcmp(command, "reverse") == 0) {
        return DRIVE_REVERSE;
    }

    if (std::strcmp(command, "left") == 0) {
        return DRIVE_TURN_LEFT;
    }

    if (std::strcmp(command, "right") == 0) {
        return DRIVE_TURN_RIGHT;
    }

    if (std::strcmp(command, "stop") == 0 || std::strcmp(command, "slowdown") == 0) {
        return DRIVE_STOP;
    }

    return DRIVE_STOP;
}
