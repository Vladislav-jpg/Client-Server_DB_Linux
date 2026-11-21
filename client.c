#include "protocol.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 8080

static void strip_newline(char *text) {
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
        text[length - 1] = '\0';
        length--;
    }
}

static void print_menu(void) {
    printf("\n=== Клиент сетевой базы данных ===\n");
    printf("1. Открыть / создать базу данных (OPEN)\n");
    printf("2. Прочитать запись по номеру   (READ)\n");
    printf("3. Записать запись по номеру    (WRITE)\n");
    printf("4. Добавить запись в конец      (APPEND)\n");
    printf("5. Удалить запись по номеру     (DELETE)\n");
    printf("6. Вывести все записи           (LIST)\n");
    printf("0. Выход                        (QUIT)\n");
    printf("Выберите действие: ");
    fflush(stdout);
}

/* Отправка запроса и вывод однострочного ответа сервера */
static int send_and_print_single_response(int server_socket, const char *request_line) {
    if (protocol_send_line(server_socket, request_line) != 0) {
        fprintf(stderr, "Ошибка отправки запроса серверу.\n");
        return -1;
    }

    char response[PROTO_MAX_LINE];
    ssize_t received = protocol_read_line(server_socket, response, sizeof(response));
    if (received <= 0) {
        fprintf(stderr, "Сервер разорвал соединение.\n");
        return -1;
    }

    printf("Ответ сервера: %s", response);
    return 0;
}

/* Отправка LIST и вывод многострочного ответа сервера */
static int send_and_print_list_response(int server_socket) {
    if (protocol_send_line(server_socket, "LIST\n") != 0) {
        fprintf(stderr, "Ошибка отправки запроса LIST.\n");
        return -1;
    }

    char response[PROTO_MAX_LINE];
    ssize_t received = protocol_read_line(server_socket, response, sizeof(response));
    if (received <= 0) {
        fprintf(stderr, "Сервер разорвал соединение.\n");
        return -1;
    }

    if (strncmp(response, "ERROR", 5) == 0) {
        printf("Ответ сервера: %s", response);
        return 0;
    }

    printf("\n--- Список записей (%s", response);
    while (1) {
        received = protocol_read_line(server_socket, response, sizeof(response));
        if (received <= 0) {
            fprintf(stderr, "Сервер разорвал соединение при передаче списка.\n");
            return -1;
        }
        if (strcmp(response, "END\n") == 0 || strcmp(response, "END\r\n") == 0) {
            break;
        }
        printf("  #%s", response);
    }
    printf("--- Конец списка ---\n");
    return 0;
}

int main(int argc, char *argv[]) {
    const char *server_ip = DEFAULT_HOST;
    int server_port = DEFAULT_PORT;

    if (argc >= 2) {
        server_ip = argv[1];
    }
    if (argc >= 3) {
        server_port = atoi(argv[2]);
        if (server_port <= 0 || server_port > 65535) {
            fprintf(stderr, "Использование: %s [IP-адрес] [порт]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons((uint16_t)server_port);

    if (inet_pton(AF_INET, server_ip, &server_address.sin_addr) <= 0) {
        fprintf(stderr, "Некорректный IP-адрес: %s\n", server_ip);
        close(server_socket);
        return EXIT_FAILURE;
    }

    if (connect(server_socket, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
        perror("connect");
        close(server_socket);
        return EXIT_FAILURE;
    }

    printf("Успешное подключение к серверу %s:%d\n", server_ip, server_port);

    char input_buffer[DB_MAX_RECORD_LEN];
    char request_buffer[PROTO_MAX_LINE];

    while (1) {
        print_menu();
        if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL) {
            break;
        }
        strip_newline(input_buffer);

        int choice = atoi(input_buffer);
        if (choice == 0 && strcmp(input_buffer, "0") == 0) {
            send_and_print_single_response(server_socket, "QUIT\n");
            break;
        } else if (choice == 1) {
            printf("Введите имя файла базы данных (например, students.db): ");
            if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL) break;
            strip_newline(input_buffer);
            snprintf(request_buffer, sizeof(request_buffer), "OPEN %s\n", input_buffer);
            if (send_and_print_single_response(server_socket, request_buffer) != 0) break;
        } else if (choice == 2) {
            printf("Введите номер записи: ");
            if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL) break;
            strip_newline(input_buffer);
            snprintf(request_buffer, sizeof(request_buffer), "READ %s\n", input_buffer);
            if (send_and_print_single_response(server_socket, request_buffer) != 0) break;
        } else if (choice == 3) {
            char number_str[64];
            printf("Введите номер записи: ");
            if (fgets(number_str, sizeof(number_str), stdin) == NULL) break;
            strip_newline(number_str);
            printf("Введите текст записи: ");
            if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL) break;
            strip_newline(input_buffer);
            snprintf(request_buffer, sizeof(request_buffer), "WRITE %s %s\n", number_str, input_buffer);
            if (send_and_print_single_response(server_socket, request_buffer) != 0) break;
        } else if (choice == 4) {
            printf("Введите текст новой записи: ");
            if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL) break;
            strip_newline(input_buffer);
            snprintf(request_buffer, sizeof(request_buffer), "APPEND %s\n", input_buffer);
            if (send_and_print_single_response(server_socket, request_buffer) != 0) break;
        } else if (choice == 5) {
            printf("Введите номер записи для удаления: ");
            if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL) break;
            strip_newline(input_buffer);
            snprintf(request_buffer, sizeof(request_buffer), "DELETE %s\n", input_buffer);
            if (send_and_print_single_response(server_socket, request_buffer) != 0) break;
        } else if (choice == 6) {
            if (send_and_print_list_response(server_socket) != 0) break;
        } else {
            printf("Некорректный пункт меню. Повторите ввод.\n");
        }
    }

    close(server_socket);
    return EXIT_SUCCESS;
}
