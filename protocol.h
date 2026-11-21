#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <sys/types.h>
#include "db.h"

#define PROTO_MAX_LINE 2048

/* Типы команд текстового протокола */
typedef enum {
    CMD_UNKNOWN = 0,
    CMD_OPEN,
    CMD_READ,
    CMD_WRITE,
    CMD_APPEND,
    CMD_DELETE,
    CMD_LIST,
    CMD_QUIT
} CommandType;

/* Разобранный запрос клиента */
typedef struct {
    CommandType type;
    size_t record_number;
    char argument_text[DB_MAX_RECORD_LEN];
} ProtocolRequest;

/* Разбор текстовой строки запроса в структуру ProtocolRequest */
int protocol_parse_request(const char *raw_line, ProtocolRequest *out_request);

/* Чтение одной строки (до '\n') из сетевого сокета */
ssize_t protocol_read_line(int socket_fd, char *buffer, size_t max_length);

/* Отправка текстовой строки в сетевой сокет */
int protocol_send_line(int socket_fd, const char *text_line);

/* Отправка форматированного ответа об ошибке */
int protocol_send_error(int socket_fd, const char *error_code, const char *description);

#endif /* PROTOCOL_H */
