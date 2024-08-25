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

typedef enum _Action {
    Action__MoveCursorUp,
    Action__MoveCursorDown,
    Action__MoveCursorLeft,
    Action__MoveCursorRight,
    Action__MovePageUp,
    Action__MovePageDown,
} Action;

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

void Property__perform_action(Property * property, Action action) {
    typeof(property->window) * window = &property->window;
    typeof(property->cursor) * cursor = &property->cursor;

    switch (action) {
    case Action__MoveCursorUp:
        if (cursor->pos_y > 0) {
            cursor->pos_y -= 1;
        }
        break;

    case Action__MoveCursorDown:
        if (cursor->pos_y < window->num_columns - 1) {
            cursor->pos_y += 1;
        }
        break;

    case Action__MoveCursorLeft:
        if (cursor->pos_x > 0) {
            cursor->pos_x -= 1;
        }
        break;

    case Action__MoveCursorRight:
        if (cursor->pos_x < window->num_rows - 1) {
            cursor->pos_x += 1;
        }
        break;

    default:
        break;
    }
}

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



typedef enum _State {
    State__Start,

    /* Got '\x1B'. */
    State__GotEsc,

    /* Got '\x1B', and '['. */
    State__GotEscBkt,

    /* Got '\x1B', '[', and digit characters. */
    State__GotEscBktDgt,

    /* Got '\x1B', '[', and alphabet. */
    State__GotEscBktAlp,
} State;

typedef struct _Parser {
    Property * property;
    State state;
    size_t arg_num;
} Parser;

void Parser__init(
    Parser * parser,
    Property * property
) {
    parser->property = property;
    parser->state = State__Start,
    parser->arg_num = 0;
}

typedef enum _Result {
    Result__Continue,
    Result__Again,
} Result;

Result Parser__run_fsm(Parser * parser, uint8_t byte) {
    Property * property = parser->property;

    Result result = Result__Continue;

    switch (parser->state) {
    case State__Start:
        switch (byte) {
        case '\x1B':
            parser->state = State__GotEsc;
            break;

        case 'k':
            Property__perform_action(property, Action__MoveCursorUp);
            break;

        case 'j':
            Property__perform_action(property, Action__MoveCursorDown);
            break;

        case 'h':
            Property__perform_action(property, Action__MoveCursorLeft);
            break;

        case 'l':
            Property__perform_action(property, Action__MoveCursorRight);
            break;

        default:
            break;
        }

        break;

    case State__GotEsc:
        if (byte == '[') {
            parser->state = State__GotEscBkt;
            break;
        }

        parser->state = State__Start;
        result = Result__Again;
        break;

    case State__GotEscBkt:
        if (byte >= '0' &&
            byte <= '9') {
            parser->arg_num = byte - '0';
            parser->state = State__GotEscBktDgt;
            break;
        }

        if (byte >= 'A' &&
            byte <= 'Z') {
            switch (byte) {
            case 'A':
                Property__perform_action(property, Action__MoveCursorUp);
                break;

            case 'B':
                Property__perform_action(property, Action__MoveCursorDown);
                break;

            case 'C':
                Property__perform_action(property, Action__MoveCursorRight);
                break;

            case 'D':
                Property__perform_action(property, Action__MoveCursorLeft);
                break;
            }

            parser->state = State__Start;
            break;
        }

        parser->state = State__Start;
        result = Result__Again;
        break;

    case State__GotEscBktDgt:
        if (byte >= '0' &&
            byte <= '9') {
            parser->arg_num *= 10;
            parser->arg_num += byte - '0';
            break;
        }

        if (byte == '~') {
            switch (parser->arg_num) {
            case 5:
                Property__perform_action(property, Action__MovePageUp);
                break;

            case 6:
                Property__perform_action(property, Action__MovePageDown);
                break;
            }

            parser->state = State__Start;
            break;
        }

        parser->state = State__Start;
        result = Result__Again;
        break;

    case State__GotEscBktAlp:
        break;
    }

    return result;
}

void Parser__feed_byte(Parser * parser, uint8_t byte) {
    Result result;

    do {
        result = Parser__run_fsm(parser, byte);
    } while (result == Result__Again);
}



void Property__update_window_size(Property * property) {
    struct winsize attr;
    ioctl(STREAM_FD, TIOCGWINSZ, &attr);
    property->window.num_rows = attr.ws_row;
    property->window.num_columns = attr.ws_col;
}



int main(int argc, char ** argv) {
    term__backup();

    atexit(about_to_exit);

    term__enable_raw_mode();

    Property__update_window_size(&property);

    Parser parser;
    Parser__init(&parser, &property);

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

        Parser__feed_byte(&parser, byte);

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

