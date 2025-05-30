/**
 * @file vlc-demux-test.c
 */
/*****************************************************************************
 * Copyright © 2016 Rémi Denis-Courmont
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Rémi Denis-Courmont reserves the right to redistribute this file under
 * the terms of the GNU Lesser General Public License as published by the
 * the Free Software Foundation; either version 2.1 or the License, or
 * (at his option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "src/input/demux-run.h"

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static struct vlc_run_args args;
static libvlc_instance_t *vlc;

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void) argc;
    vlc_run_args_init(&args);

    if (args.name == NULL)
    {
        char *name = *argv[0];
        static const char suffix[] = "-libfuzzer";
        static const size_t suffix_len = sizeof(suffix) - 1;
        char *target_start = strstr(name, suffix);
        if (target_start != NULL && target_start[suffix_len] == '-')
            args.name = &target_start[suffix_len + 1];
    }
    vlc = libvlc_create(&args);

    return vlc ? 0 : -1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    libvlc_demux_process_memory(vlc, &args, data, size);
    return 0;
}
