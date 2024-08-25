#include <sys/ioctl.h>
#include <termios.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <error.h>
#include <stdio.h>

#define STREAM_FD STDIN_FILENO

typedef struct _Property {
    struct _Property__Window {
        size_t num_rows;
        size_t num_columns;
    } window;
    struct _Property__Cursor {
        size_t pos_x;
        size_t pos_y;
    } cursor;
} Property;

static Property property = {
    .window = {
        .num_rows = 0,
        .num_columns = 0,
    },
    .cursor = {
        .pos_x = 0,
        .pos_y = 0,
    },
};

void update_window_size(void) {
    struct winsize attr;
    ioctl(STREAM_FD, TIOCGWINSZ, &attr);
    property.window.num_rows = attr.ws_row;
    property.window.num_columns = attr.ws_col;
}

static struct termios term__attr_backup;

void term__backup(void) {
    tcgetattr(STREAM_FD, &term__attr_backup);
}

void term__restore(void) {
    tcsetattr(STREAM_FD, TCSAFLUSH, &term__attr_backup);
}

void term__enable_raw_mode(void) {
    struct termios attr;

    tcgetattr(STDIN_FILENO, &attr);

    /* Disable echoing. */
    attr.c_lflag &= ~(ECHO);

    /* Turn off canonical mode. */
    attr.c_lflag &= ~(ICANON);

    /* Turn 'Ctrl-C' and 'Ctrl-Z' signals. */
    attr.c_lflag &= ~(ISIG);

    /* Disable software flow control. */
    attr.c_iflag &= ~(IXON);

    /* Turn off the IEXTEN flag. */
    attr.c_lflag &= ~(IEXTEN);

    /* Prevent from converting '\r' to '\n'. */
    attr.c_iflag &= ~(ICRNL);

    /* Prevent from converting '\n' to '\r\n'. */
    attr.c_oflag &= ~(OPOST);

    /* Miscellaneous flags. */
    attr.c_iflag &= ~(BRKINT | INPCK | ISTRIP);
    attr.c_cflag |= (CS8);

    /* Minimum length of reading. */
    attr.c_cc[VMIN] = 0;

    /* Set timeout to 100ms. */
    attr.c_cc[VTIME] = 1;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &attr);
}

void about_to_exit(void) {
    term__restore();
}

void handle_input(void) {

}

void panic(const char * text) {
    perror(text);
    exit(EXIT_FAILURE);
}

typedef struct _CmdWriter {
    int fd;
    uint8_t * buf;
    size_t cap;
    size_t len;
} CmdWriter;

void CmdWriter__init(CmdWriter * ctx, int fd) {
    ctx->fd = fd;
    ctx->buf = NULL;
    ctx->cap = 0;
    ctx->len = 0;
}

void CmdWriter__push_raw(
    CmdWriter * ctx,
    const void * buf,
    size_t len
) {
    if (len == 0) {
        return;
    }

    size_t required_len = ctx->len + len;
    if (required_len > ctx->cap) {
        const size_t MIN_CAP = 128;
        size_t required_cap = required_len;
        if (required_cap < MIN_CAP) {
            required_cap = MIN_CAP;
        }

        size_t remainder = required_cap % 8;
        if (remainder != 0) {
            required_cap += 8;
        }

        required_cap *= 2;

        uint8_t * required_buf;
        if (ctx->buf != NULL) {
            required_buf = realloc(ctx->buf, required_cap);
        } else {
            required_buf = malloc(required_cap);
        }
        if (required_buf == NULL) {
            panic("CmdWriter__push():allocate-buffer");
        }

        ctx->buf = required_buf;
        ctx->cap = required_cap;
    }

    memcpy(ctx->buf + ctx->len, buf, len);

    ctx->len += len;
}

void CmdWriter__push_str(
    CmdWriter * ctx,
    const char * str
) {
    CmdWriter__push_raw(ctx, str, strlen(str));
}

void CmdWriter__push_fmt(
    CmdWriter * ctx,
    const char * fmt,
    ...
) {
    char fixed_buf[128];
    char * buf = fixed_buf;
    va_list args;
    int len;

    va_start(args, fmt);

    len = vsnprintf(NULL, 0, fmt, args);

    if (len > sizeof(fixed_buf)) {
        buf = (char *)malloc(len + 1);
        if (buf == NULL) {
            va_end(args);
            panic("CmdWriter__push_fmt():allocate-buffer");
        }
    }

    va_end(args);

    va_start(args, fmt);

    vsnprintf(buf, len + 1, fmt, args);

    va_end(args);

    CmdWriter__push_raw(ctx, buf, len);

    if (buf != fixed_buf) {
        free(buf);
    }
}

void CmdWriter__flush(CmdWriter * ctx) {
    if (ctx->len > 0) {
        ssize_t result = write(ctx->fd, ctx->buf, ctx->len);
        if (result != ctx->len) {
            panic("CmdWriter__flush():write-buffer");
        }

        ctx->len = 0;
    }
}

void CmdWriter__free(CmdWriter * ctx) {
    if (ctx->buf != NULL) {
        free(ctx->buf);
    }
}

void cmd__refresh_screen(void) {
    typeof(property.window) * window = &property.window;
    typeof(property.cursor) * cursor = &property.cursor;

    CmdWriter writer;

    CmdWriter__init(&writer, STREAM_FD);

    /* Hide the cursor. */
    CmdWriter__push_str(&writer, "\x1B[?25l");

    /* Clear the whole screen. */
    CmdWriter__push_str(&writer, "\x1B[2J");

    /* Set the cursor positio to (1, 1). */
    CmdWriter__push_str(&writer, "\x1B[1;1H");

    for (size_t i = 0; i < window->num_columns - 1; i++) {
        CmdWriter__push_str(&writer, "~\r\n");
    }
    CmdWriter__push_str(&writer, "~");

    /* Set the cursor positio to the current position. */
    CmdWriter__push_fmt(&writer, "\x1B[%zu;%zuH",
        cursor->pos_y + 1, cursor->pos_x + 1);

    /* Show the cursor. */
    CmdWriter__push_str(&writer, "\x1B[?25h");

    CmdWriter__flush(&writer);
    CmdWriter__free(&writer);
}

int main(int argc, char ** argv) {
    term__backup();

    atexit(about_to_exit);

    term__enable_raw_mode();

    update_window_size();

    while (true) {
        uint8_t byte;
        ssize_t result;

        result = read(STREAM_FD, &byte, 1);
        if (result == -1) {
            if (errno == EAGAIN) {
                continue;
            } else {
                panic("main():read-input");
            }
        } else if (result == 0) {
            continue;
        }

        typeof(property.window) * window = &property.window;
        typeof(property.cursor) * cursor = &property.cursor;

        switch (byte) {
        case 'k':
            if (cursor->pos_y > 0) {
                cursor->pos_y -= 1;
            }
            break;

        case 'j':
            if (cursor->pos_y < window->num_columns - 1) {
                cursor->pos_y += 1;
            }
            break;

        case 'h':
            if (cursor->pos_x > 0) {
                cursor->pos_x -= 1;
            }
            break;

        case 'l':
            if (cursor->pos_x < window->num_rows - 1) {
                cursor->pos_x += 1;
            }
            break;

        default:
            break;
        }

        cmd__refresh_screen();

        if (byte == 'q') {
            exit(EXIT_SUCCESS);
        }

        if (byte == 'Q') {
            panic("main():user-trigger"); 
        }
    }

    return EXIT_SUCCESS;
}

