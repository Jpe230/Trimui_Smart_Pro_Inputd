// Copyright 2024 Jose Pablo Ramirez (@Jpe230)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "common.hj"

int main ()
{   
    int readResult = readSerialJoypad(joystickFd, &joystickSerialData);
    //                                                                LEFT JOY       RIGHT JOY
    int joystickFd = openSerialJoystick(joystickToCalibrate == 0 ? "/dev/ttyS4" : "/dev/ttyS3");
    closeSerialJoystick(joystickFd);
    exit(0);
}

