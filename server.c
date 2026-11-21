#include "db.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_PORT 8080
#define LISTEN_BACKLOG 16

/* Общее состояние сервера, защищенное мьютексом */
typedef struct {
    Database database;
    pthread_mutex_t db_mutex;
} ServerContext;

/* Аргументы потока обработки клиента */
typedef struct {
    int client_socket;
    struct sockaddr_in client_address;
    ServerContext *server_context;
} ClientThreadArgs;

static volatile sig_atomic_t server_running = 1;
static int global_listen_socket = -1;

static void handle_sigint(int signal_number) {
    (void)signal_number;
    server_running = 0;
    if (global_listen_socket >= 0) {
        close(global_listen_socket);
        global_listen_socket = -1;
    }
}

/* Преобразование DbStatus в текстовый код ошибки протокола */
static const char *db_status_to_error_code(DbStatus status) {
    switch (status) {
        case DB_ERR_INVALID_ARG:      return "INVALID_ARGS";
        case DB_ERR_NOT_OPEN:         return "DB_NOT_OPEN";
        case DB_ERR_IO:               return "IO_ERROR";
        case DB_ERR_MEMORY:           return "OUT_OF_MEMORY";
        case DB_ERR_NOT_FOUND:        return "RECORD_NOT_FOUND";
        case DB_ERR_BUFFER_TOO_SMALL: return "BUFFER_OVERFLOW";
        default:                      return "INTERNAL_ERROR";
    }
}

/* Обработка одного разобранного клиентского запроса */
static int process_client_request(int client_socket, ServerContext *context, const ProtocolRequest *request) {
    char response[PROTO_MAX_LINE];
    char record_buffer[DB_MAX_RECORD_LEN];

    pthread_mutex_lock(&context->db_mutex);

    switch (request->type) {
        case CMD_OPEN: {
            DbStatus status = db_open(&context->database, request->argument_text);
            if (status == DB_OK) {
                snprintf(
                    response,
                    sizeof(response),
                    "OK База данных '%s' открыта (слотов: %zu)\n",
                    context->database.data_path,
                    context->database.index_table.count
                );
                protocol_send_line(client_socket, response);
            } else {
                protocol_send_error(client_socket, db_status_to_error_code(status), db_status_to_string(status));
            }
            break;
        }

        case CMD_READ: {
            DbStatus status = db_read(&context->database, request->record_number, record_buffer, sizeof(record_buffer));
            if (status == DB_OK) {
                snprintf(response, sizeof(response), "OK DATA %zu %s\n", request->record_number, record_buffer);
                protocol_send_line(client_socket, response);
            } else {
                protocol_send_error(client_socket, db_status_to_error_code(status), db_status_to_string(status));
            }
            break;
        }

        case CMD_WRITE: {
            DbStatus status = db_write(&context->database, request->record_number, request->argument_text);
            if (status == DB_OK) {
                snprintf(response, sizeof(response), "OK Запись #%zu успешно сохранена\n", request->record_number);
                protocol_send_line(client_socket, response);
            } else {
                protocol_send_error(client_socket, db_status_to_error_code(status), db_status_to_string(status));
            }
            break;
        }

        case CMD_APPEND: {
            size_t new_record_number = 0;
            DbStatus status = db_append(&context->database, request->argument_text, &new_record_number);
            if (status == DB_OK) {
                snprintf(response, sizeof(response), "OK Добавлена запись #%zu\n", new_record_number);
                protocol_send_line(client_socket, response);
            } else {
                protocol_send_error(client_socket, db_status_to_error_code(status), db_status_to_string(status));
            }
            break;
        }

        case CMD_DELETE: {
            DbStatus status = db_delete(&context->database, request->record_number);
            if (status == DB_OK) {
                snprintf(response, sizeof(response), "OK Запись #%zu удалена\n", request->record_number);
                protocol_send_line(client_socket, response);
            } else {
                protocol_send_error(client_socket, db_status_to_error_code(status), db_status_to_string(status));
            }
            break;
        }

        case CMD_LIST: {
            if (!context->database.is_open) {
                protocol_send_error(client_socket, "DB_NOT_OPEN", db_status_to_string(DB_ERR_NOT_OPEN));
                break;
            }

            /* Подсчитываем количество активных записей */
            size_t active_count = 0;
            for (size_t i = 0; i < context->database.index_table.count; i++) {
                if (context->database.index_table.entries[i].is_active) {
                    active_count++;
                }
            }

            snprintf(response, sizeof(response), "OK LIST %zu\n", active_count);
            protocol_send_line(client_socket, response);

            for (size_t i = 0; i < context->database.index_table.count; i++) {
                if (!context->database.index_table.entries[i].is_active) {
                    continue;
                }
                size_t record_num = i + 1;
                if (db_read(&context->database, record_num, record_buffer, sizeof(record_buffer)) == DB_OK) {
                    snprintf(response, sizeof(response), "%zu %s\n", record_num, record_buffer);
                    protocol_send_line(client_socket, response);
                }
            }
            protocol_send_line(client_socket, "END\n");
            break;
        }

        case CMD_QUIT: {
            protocol_send_line(client_socket, "OK Соединение закрыто\n");
            pthread_mutex_unlock(&context->db_mutex);
            return 1; /* Завершить сессию клиента */
        }

        default:
            protocol_send_error(client_socket, "UNKNOWN_COMMAND", "Неизвестная команда");
            break;
    }

    pthread_mutex_unlock(&context->db_mutex);
    return 0;
}

/* Поток обслуживания отдельного клиента */
static void *client_thread_routine(void *arg) {
    ClientThreadArgs *thread_args = (ClientThreadArgs *)arg;
    int client_socket = thread_args->client_socket;
    ServerContext *context = thread_args->server_context;

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &thread_args->client_address.sin_addr, client_ip, sizeof(client_ip));
    int client_port = ntohs(thread_args->client_address.sin_port);

    printf("[SERVER] Подключен клиент %s:%d\n", client_ip, client_port);
    free(thread_args);

    char raw_line[PROTO_MAX_LINE];
    while (1) {
        ssize_t bytes_received = protocol_read_line(client_socket, raw_line, sizeof(raw_line));
        if (bytes_received <= 0) {
            break;
        }

        ProtocolRequest request;
        if (protocol_parse_request(raw_line, &request) != 0) {
            protocol_send_error(client_socket, "BAD_REQUEST", "Некорректный формат команды или аргументов");
            continue;
        }

        if (process_client_request(client_socket, context, &request) != 0) {
            break;
        }
    }

    printf("[SERVER] Клиент %s:%d отключен\n", client_ip, client_port);
    close(client_socket);
    return NULL;
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    if (argc >= 2) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Использование: %s [порт (1..65535)]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    signal(SIGINT, handle_sigint);
    signal(SIGPIPE, SIG_IGN);

    ServerContext context;
    db_init(&context.database);
    if (pthread_mutex_init(&context.db_mutex, NULL) != 0) {
        perror("pthread_mutex_init");
        return EXIT_FAILURE;
    }

    int listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket < 0) {
        perror("socket");
        pthread_mutex_destroy(&context.db_mutex);
        return EXIT_FAILURE;
    }

    int reuse_opt = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_opt, sizeof(reuse_opt));

    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons((uint16_t)port);

    if (bind(listen_socket, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
        perror("bind");
        close(listen_socket);
        pthread_mutex_destroy(&context.db_mutex);
        return EXIT_FAILURE;
    }

    if (listen(listen_socket, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(listen_socket);
        pthread_mutex_destroy(&context.db_mutex);
        return EXIT_FAILURE;
    }

    global_listen_socket = listen_socket;
    printf("[SERVER] Сервер базы данных запущен на порту %d. Нажмите Ctrl+C для остановки.\n", port);
    fflush(stdout);

    while (server_running) {
        struct sockaddr_in client_address;
        socklen_t client_len = sizeof(client_address);
        int client_socket = accept(listen_socket, (struct sockaddr *)&client_address, &client_len);
        if (client_socket < 0) {
            if (!server_running) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }

        ClientThreadArgs *args = (ClientThreadArgs *)malloc(sizeof(ClientThreadArgs));
        if (args == NULL) {
            close(client_socket);
            continue;
        }
        args->client_socket = client_socket;
        args->client_address = client_address;
        args->server_context = &context;

        pthread_t client_thread;
        if (pthread_create(&client_thread, NULL, client_thread_routine, args) != 0) {
            perror("pthread_create");
            free(args);
            close(client_socket);
            continue;
        }
        pthread_detach(client_thread);
    }

    printf("\n[SERVER] Остановка сервера...\n");
    pthread_mutex_lock(&context.db_mutex);
    db_close(&context.database);
    pthread_mutex_unlock(&context.db_mutex);
    pthread_mutex_destroy(&context.db_mutex);

    if (global_listen_socket >= 0) {
        close(global_listen_socket);
    }

    return EXIT_SUCCESS;
}
