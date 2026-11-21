#include "protocol.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Удаление завершающих символов '\r' и '\n' */
static void strip_trailing_newlines(char *text) {
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
        text[length - 1] = '\0';
        length--;
    }
}

/* Безопасный разбор положительного целого числа */
static int parse_positive_size_t(const char *token, size_t *out_value) {
    if (token == NULL || token[0] == '\0' || token[0] == '-') {
        return 0;
    }
    char *end_ptr = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(token, &end_ptr, 10);
    if (errno != 0 || end_ptr == token || *end_ptr != '\0' || parsed == 0) {
        return 0;
    }
    *out_value = (size_t)parsed;
    return 1;
}

int protocol_parse_request(const char *raw_line, ProtocolRequest *out_request) {
    if (raw_line == NULL || out_request == NULL) {
        return -1;
    }

    memset(out_request, 0, sizeof(ProtocolRequest));
    out_request->type = CMD_UNKNOWN;

    char working_copy[PROTO_MAX_LINE];
    strncpy(working_copy, raw_line, sizeof(working_copy) - 1);
    working_copy[sizeof(working_copy) - 1] = '\0';
    strip_trailing_newlines(working_copy);

    /* Пропускаем ведущие пробелы */
    char *cursor = working_copy;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    if (*cursor == '\0') {
        return -1;
    }

    /* Извлекаем имя команды */
    char command_name[32];
    size_t cmd_len = 0;
    while (*cursor != '\0' && !isspace((unsigned char)*cursor) && cmd_len < sizeof(command_name) - 1) {
        command_name[cmd_len++] = (char)toupper((unsigned char)*cursor);
        cursor++;
    }
    command_name[cmd_len] = '\0';

    /* Пропускаем пробелы после имени команды */
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }

    if (strcmp(command_name, "OPEN") == 0) {
        if (*cursor == '\0' || strlen(cursor) >= DB_MAX_PATH) {
            return -1;
        }
        out_request->type = CMD_OPEN;
        strncpy(out_request->argument_text, cursor, sizeof(out_request->argument_text) - 1);
        return 0;
    }

    if (strcmp(command_name, "READ") == 0) {
        if (!parse_positive_size_t(cursor, &out_request->record_number)) {
            return -1;
        }
        out_request->type = CMD_READ;
        return 0;
    }

    if (strcmp(command_name, "DELETE") == 0) {
        if (!parse_positive_size_t(cursor, &out_request->record_number)) {
            return -1;
        }
        out_request->type = CMD_DELETE;
        return 0;
    }

    if (strcmp(command_name, "APPEND") == 0) {
        if (*cursor == '\0' || strlen(cursor) >= DB_MAX_RECORD_LEN) {
            return -1;
        }
        out_request->type = CMD_APPEND;
        strncpy(out_request->argument_text, cursor, sizeof(out_request->argument_text) - 1);
        return 0;
    }

    if (strcmp(command_name, "WRITE") == 0) {
        /* Формат: WRITE <номер> <текст записи> */
        char number_token[64];
        size_t num_len = 0;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor) && num_len < sizeof(number_token) - 1) {
            number_token[num_len++] = *cursor++;
        }
        number_token[num_len] = '\0';

        if (!parse_positive_size_t(number_token, &out_request->record_number)) {
            return -1;
        }
        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (*cursor == '\0' || strlen(cursor) >= DB_MAX_RECORD_LEN) {
            return -1;
        }
        out_request->type = CMD_WRITE;
        strncpy(out_request->argument_text, cursor, sizeof(out_request->argument_text) - 1);
        return 0;
    }

    if (strcmp(command_name, "LIST") == 0) {
        if (*cursor != '\0') {
            return -1;
        }
        out_request->type = CMD_LIST;
        return 0;
    }

    if (strcmp(command_name, "QUIT") == 0) {
        out_request->type = CMD_QUIT;
        return 0;
    }

    return -1;
}

ssize_t protocol_read_line(int socket_fd, char *buffer, size_t max_length) {
    if (buffer == NULL || max_length == 0) {
        return -1;
    }

    size_t total_read = 0;
    while (total_read < max_length - 1) {
        char ch = '\0';
        ssize_t result = recv(socket_fd, &ch, 1, 0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            break; /* Соединение закрыто удаленной стороной */
        }
        buffer[total_read++] = ch;
        if (ch == '\n') {
            break;
        }
    }
    buffer[total_read] = '\0';
    return (ssize_t)total_read;
}

int protocol_send_line(int socket_fd, const char *text_line) {
    if (text_line == NULL) {
        return -1;
    }
    size_t length = strlen(text_line);
    size_t sent_total = 0;

    while (sent_total < length) {
        ssize_t sent = send(socket_fd, text_line + sent_total, length - sent_total, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        sent_total += (size_t)sent;
    }
    return 0;
}

int protocol_send_error(int socket_fd, const char *error_code, const char *description) {
    char response[PROTO_MAX_LINE];
    snprintf(response, sizeof(response), "ERROR %s %s\n", error_code, description);
    return protocol_send_line(socket_fd, response);
}
