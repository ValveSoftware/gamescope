// Code adapted from:
// https://github.com/libsdl-org/SDL/blob/main/src/video/wayland/SDL_waylandmessagebox.c
// which is licensed under Z-Lib
// as follows:
//
// Simple DirectMedia Layer
// Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.

#pragma once

#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdio>

#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

namespace messagey {

    static constexpr int MaxButtons = 8;

    namespace MessageBoxFlag
    {
        static constexpr uint32_t Error              = (1u << 0);
        static constexpr uint32_t Warning            = (1u << 1);

        static constexpr uint32_t Information        = (1u << 2);
        static constexpr uint32_t ButtonsLeftToRight = (1u << 3);
        static constexpr uint32_t ButtonsRightToLeft = (1u << 4);

        static constexpr uint32_t Simple_OK          = (1u << 5);
        static constexpr uint32_t Simple_Cancel      = (1u << 6);
    }
    using MessageBoxFlags = uint32_t;

    namespace MessageBoxButtonFlag {
        static constexpr uint32_t ReturnKeyDefault = (1u << 0);
        static constexpr uint32_t EscapeKeyDefault = (1u << 1);
    }
    using MessageBoxButtonFlags = uint32_t;

    struct MessageBoxButtonData
    {
        uint32_t flags;
        int buttonid;
        const char* text;
    };

    struct MessageBoxData
    {
        uint32_t    flags;
        const char* title;
        const char* message;

        int numbuttons;
        const MessageBoxButtonData *buttons;
    };

    struct ErrorData
    {
        bool valid = false;
        char str[256];
    };

    inline ErrorData* GetErrBuf()
    {
        static thread_local ErrorData err;
        return &err;
    }

    inline ErrorData* GetError()
    {
        ErrorData *err = GetErrBuf();
        if (!err->valid)
            return nullptr;
        return err;
    }

    inline int SetError(const char *fmt, ...)
    {
        if (fmt != nullptr) {
            va_list ap;
            ErrorData *error = GetErrBuf();

            error->valid = true;

            va_start(ap, fmt);
            vsnprintf(error->str, sizeof(ErrorData::str), fmt, ap);
            va_end(ap);
        }

        return -1;
    }

    inline int Show(const MessageBoxData *messageboxdata, int *buttonid)
    {
        int fd_pipe[2]; /* fd_pipe[0]: read end of pipe, fd_pipe[1]: write end of pipe */
        pid_t pid1;

        const char *zenityDisableEnv = getenv("GAMESCOPE_ZENITY_DISABLE");
        if (zenityDisableEnv && *zenityDisableEnv && atoi(zenityDisableEnv) != 0)
            return 0;

        if (messageboxdata->numbuttons > MaxButtons) {
            return SetError("Too many buttons (%d max allowed)", MaxButtons);
        }

        if (pipe(fd_pipe) != 0) { /* create a pipe */
            return SetError("pipe() failed: %s", strerror(errno));
        }

        pid1 = fork();
        if (pid1 == 0) {  /* child process */
            int argc = 5, i;
            const char* argv[5 + 2/* icon name */ + 2/* title */ + 2/* message */ + 2*MaxButtons + 1/* nullptr */] = {
                "zenity", "--question", "--switch", "--no-wrap", "--no-markup"
            };

            close(fd_pipe[0]); /* no reading from pipe */
            /* write stdout in pipe */
            if (dup2(fd_pipe[1], STDOUT_FILENO) == -1) {
                _exit(128);
            }

            argv[argc++] = "--icon-name";
            if (messageboxdata->flags & MessageBoxFlag::Error)
                argv[argc++] = "dialog-error";
            else if (messageboxdata->flags & MessageBoxFlag::Warning)
                argv[argc++] = "dialog-warning";
            else if (messageboxdata->flags & MessageBoxFlag::Information)
                argv[argc++] = "dialog-information";

            if (messageboxdata->title && messageboxdata->title[0]) {
                argv[argc++] = "--title";
                argv[argc++] = messageboxdata->title;
            } else {
                argv[argc++] = "--title=\"\"";
            }

            if (messageboxdata->message && messageboxdata->message[0]) {
                argv[argc++] = "--text";
                argv[argc++] = messageboxdata->message;
            } else {
                argv[argc++] = "--text=\"\"";
            }

            for (i = 0; i < messageboxdata->numbuttons; ++i) {
                if (messageboxdata->buttons[i].text && messageboxdata->buttons[i].text[0]) {
                    argv[argc++] = "--extra-button";
                    argv[argc++] = messageboxdata->buttons[i].text;
                } else {
                    argv[argc++] = "--extra-button=\"\"";
                }
            }
            argv[argc] = nullptr;

            /* const casting argv is fine:
            * https://pubs.opengroup.org/onlinepubs/9699919799/functions/fexecve.html -> rational
            */
            execvp("zenity", (char **)argv);
            _exit(129);
        } else if (pid1 < 0) {
            close(fd_pipe[0]);
            close(fd_pipe[1]);
            return SetError("fork() failed: %s", strerror(errno));
        } else {
            int status;
            if (waitpid(pid1, &status, 0) == pid1) {
                if (WIFEXITED(status)) {
                    if (WEXITSTATUS(status) < 128) {
                        int i;
                        size_t output_len = 1;
                        char* output = nullptr;
                        char* tmp = nullptr;
                        FILE* stdout = nullptr;

                        close(fd_pipe[1]); /* no writing to pipe */
                        /* At this point, if no button ID is needed, we can just bail as soon as the
                        * process has completed.
                        */
                        if (buttonid == NULL) {
                            close(fd_pipe[0]);
                            return 0;
                        }
                        *buttonid = -1;

                        /* find button with longest text */
                        for (i = 0; i < messageboxdata->numbuttons; ++i) {
                            if (messageboxdata->buttons[i].text != NULL) {
                                const size_t button_len = strlen(messageboxdata->buttons[i].text);
                                if (button_len > output_len) {
                                    output_len = button_len;
                                }
                            }
                        }
                        output = (char *)malloc(output_len + 1);
                        if (!output) {
                            close(fd_pipe[0]);
                            return SetError("Out of memory");
                        }
                        output[0] = '\0';

                        stdout = fdopen(fd_pipe[0], "r");
                        if (!stdout) {
                            free(output);
                            close(fd_pipe[0]);
                            return SetError("Couldn't open pipe for reading: %s", strerror(errno));
                        }
                        tmp = fgets(output, output_len + 1, stdout);
                        fclose(stdout);

                        if ((tmp == NULL) || (*tmp == '\0') || (*tmp == '\n')) {
                            free(output);
                            return 0; /* User simply closed the dialog */
                        }

                        /* It likes to add a newline... */
                        tmp = strrchr(output, '\n');
                        if (tmp != NULL) {
                            *tmp = '\0';
                        }

                        /* Check which button got pressed */
                        for (i = 0; i < messageboxdata->numbuttons; i += 1) {
                            if (messageboxdata->buttons[i].text != NULL) {
                                if (strcmp(output, messageboxdata->buttons[i].text) == 0) {
                                    *buttonid = messageboxdata->buttons[i].buttonid;
                                    break;
                                }
                            }
                        }

                        free(output);
                        return 0;  /* success! */
                    } else {
                        return SetError("zenity reported error or failed to launch: %d", WEXITSTATUS(status));
                    }
                } else {
                    return SetError("zenity failed for some reason");
                }
            } else {
                return SetError("Waiting on zenity failed: %s", strerror(errno));
            }
        }
    }

    inline int ShowSimple(const char *message, const char *caption, MessageBoxFlags flags, int *buttonid)
    {
        int buttonCount = 0;
        MessageBoxButtonData buttons[2];

        bool hasCancel = !!(flags & MessageBoxFlag::Simple_Cancel);
        bool hasOK = !!(flags & MessageBoxFlag::Simple_OK);

        if (hasCancel) {
            buttons[buttonCount++] =
            {
                .flags    = MessageBoxButtonFlag::EscapeKeyDefault | (hasOK ? 0 : MessageBoxButtonFlag::ReturnKeyDefault),
                .buttonid = 0,
                .text     = "Cancel",
            };
        }

        if (hasOK) {
            buttons[buttonCount++] =
            {
                .flags    = MessageBoxButtonFlag::ReturnKeyDefault,
                .buttonid = 1,
                .text     = "OK",
            };
        }

        MessageBoxData data =
        {
            .flags   = flags,
            .title   = caption,
            .message = message,
            .numbuttons = buttonCount,
            .buttons    = buttons,
        };

        return Show(&data, buttonid);
    }
}

