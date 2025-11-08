// Copyright 2025 Jose Pablo Ramirez (@Jpe230)
// SPDX-License-Identifier: GPL-2.0-or-later

// Entry point responsible for parsing CLI args and delegating to the controller runtime.

#include <stdio.h>
#include <stdlib.h>

#include "controller/controller.h"

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [config_dir]\n", prog);
}

int main(int argc, char **argv)
{
    const char *override_dir = NULL;

    if (argc > 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        override_dir = argv[1];
    }

    return run_controller(override_dir);
}
