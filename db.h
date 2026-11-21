#ifndef DB_H
#define DB_H

#include <stddef.h>
#include <sys/types.h>

#define DB_MAX_PATH 256
#define DB_MAX_RECORD_LEN 1024
#define DB_INITIAL_CAPACITY 16

/* Коды возврата функций базы данных */
typedef enum {
    DB_OK = 0,
    DB_ERR_INVALID_ARG = -1,
    DB_ERR_NOT_OPEN = -2,
    DB_ERR_IO = -3,
    DB_ERR_MEMORY = -4,
    DB_ERR_NOT_FOUND = -5,
    DB_ERR_BUFFER_TOO_SMALL = -6
} DbStatus;

/* Элемент индексной таблицы: отображает логический номер записи на смещение в файле */
typedef struct {
    size_t record_number;   /* Номер записи (начиная с 1) */
    off_t byte_offset;      /* Смещение начала записи в текстовом файле (в байтах) */
    size_t byte_length;     /* Длина записи в байтах (без символа перевода строки '\n') */
    int is_active;          /* 1 — запись существует, 0 — пустая или удалена */
} IndexEntry;

/* Динамическая индексная таблица */
typedef struct {
    IndexEntry *entries;    /* Массив индексных записей */
    size_t count;           /* Текущее количество записей (включая пустые/удаленные) */
    size_t capacity;        /* Выделенная емкость массива */
} IndexTable;

/* Дескриптор открытой базы данных */
typedef struct {
    int data_fd;                    /* Системный файловый дескриптор файла данных */
    int is_open;                    /* Флаг состояния: 1 — открыта, 0 — закрыта */
    char data_path[DB_MAX_PATH];    /* Путь к текстовому файлу базы данных */
    char index_path[DB_MAX_PATH + 8];   /* Путь к текстовому файлу индексной таблицы (.idx) */
    IndexTable index_table;         /* Индексная таблица в оперативной памяти */
} Database;

/* Инициализация структуры базы данных перед использованием */
void db_init(Database *database);

/* 1. Открыть существующую или создать новую базу данных */
DbStatus db_open(Database *database, const char *file_path);

/* Закрыть базу данных и освободить ресурсы */
DbStatus db_close(Database *database);

/* 2. Прочитать запись с указанным номером (нумерация с 1) */
DbStatus db_read(Database *database, size_t record_number, char *out_buffer, size_t buffer_size);

/* 3. Записать запись с указанным номером (добавляет пустые записи при выходе за границы) */
DbStatus db_write(Database *database, size_t record_number, const char *record_text);

/* 4. Добавить запись в конец базы данных; в out_record_number возвращается присвоенный номер */
DbStatus db_append(Database *database, const char *record_text, size_t *out_record_number);

/* 5. Удалить запись с указанным номером */
DbStatus db_delete(Database *database, size_t record_number);

/* 6. Получить количество всех слотов и текстовое описание ошибки */
const char *db_status_to_string(DbStatus status);

/* Вспомогательная проверка валидности текста записи */
int db_is_valid_text(const char *record_text);

#endif /* DB_H */
