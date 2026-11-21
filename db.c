#include "db.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Надежная запись всех байтов через системный вызов write() */
static ssize_t write_all_bytes(int file_descriptor, const void *buffer, size_t byte_count) {
    const char *current_ptr = (const char *)buffer;
    size_t bytes_remaining = byte_count;

    while (bytes_remaining > 0) {
        ssize_t bytes_written = write(file_descriptor, current_ptr, bytes_remaining);
        if (bytes_written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        current_ptr += bytes_written;
        bytes_remaining -= (size_t)bytes_written;
    }
    return (ssize_t)byte_count;
}

/* Надежное чтение точного количества байтов через системный вызов read() */
static ssize_t read_exact_bytes(int file_descriptor, void *buffer, size_t byte_count) {
    char *current_ptr = (char *)buffer;
    size_t bytes_remaining = byte_count;

    while (bytes_remaining > 0) {
        ssize_t bytes_read = read(file_descriptor, current_ptr, bytes_remaining);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (bytes_read == 0) {
            break; /* Конец файла */
        }
        current_ptr += bytes_read;
        bytes_remaining -= (size_t)bytes_read;
    }
    return (ssize_t)(byte_count - bytes_remaining);
}

/* Освобождение памяти индексной таблицы */
static void index_table_free(IndexTable *table) {
    if (table->entries != NULL) {
        free(table->entries);
        table->entries = NULL;
    }
    table->count = 0;
    table->capacity = 0;
}

/* Добавление элемента в индексную таблицу с динамическим расширением */
static DbStatus index_table_push(IndexTable *table, off_t byte_offset, size_t byte_length, int is_active) {
    if (table->count >= table->capacity) {
        size_t new_capacity = (table->capacity == 0) ? DB_INITIAL_CAPACITY : (table->capacity * 2);
        IndexEntry *resized_entries = (IndexEntry *)realloc(table->entries, new_capacity * sizeof(IndexEntry));
        if (resized_entries == NULL) {
            return DB_ERR_MEMORY;
        }
        table->entries = resized_entries;
        table->capacity = new_capacity;
    }

    IndexEntry *entry = &table->entries[table->count];
    entry->record_number = table->count + 1;
    entry->byte_offset = byte_offset;
    entry->byte_length = byte_length;
    entry->is_active = is_active;
    table->count++;

    return DB_OK;
}

/* Сохранение индексной таблицы в текстовый файл .idx с помощью системных вызовов */
static DbStatus db_save_index_file(Database *database) {
    int index_fd = open(database->index_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (index_fd < 0) {
        return DB_ERR_IO;
    }

    char line_buffer[128];
    for (size_t i = 0; i < database->index_table.count; i++) {
        const IndexEntry *entry = &database->index_table.entries[i];
        int formatted_len = snprintf(
            line_buffer,
            sizeof(line_buffer),
            "%zu %lld %zu %d\n",
            entry->record_number,
            (long long)entry->byte_offset,
            entry->byte_length,
            entry->is_active
        );
        if (formatted_len < 0 || write_all_bytes(index_fd, line_buffer, (size_t)formatted_len) < 0) {
            close(index_fd);
            return DB_ERR_IO;
        }
    }

    if (close(index_fd) < 0) {
        return DB_ERR_IO;
    }
    return DB_OK;
}

/* Построение индексной таблицы путем сканирования текстового файла данных через read() */
static DbStatus db_rebuild_index(Database *database) {
    index_table_free(&database->index_table);

    if (lseek(database->data_fd, 0, SEEK_SET) < 0) {
        return DB_ERR_IO;
    }

    char read_chunk[4096];
    off_t current_file_offset = 0;
    off_t line_start_offset = 0;
    size_t current_line_length = 0;

    while (1) {
        ssize_t bytes_read = read(database->data_fd, read_chunk, sizeof(read_chunk));
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return DB_ERR_IO;
        }
        if (bytes_read == 0) {
            break;
        }

        for (ssize_t i = 0; i < bytes_read; i++) {
            if (read_chunk[i] == '\n') {
                int is_active = (current_line_length > 0) ? 1 : 0;
                DbStatus push_status = index_table_push(
                    &database->index_table,
                    line_start_offset,
                    current_line_length,
                    is_active
                );
                if (push_status != DB_OK) {
                    return push_status;
                }
                current_file_offset++;
                line_start_offset = current_file_offset;
                current_line_length = 0;
            } else {
                current_line_length++;
                current_file_offset++;
            }
        }
    }

    /* Если последняя строка в файле не заканчивалась символом '\n' */
    if (current_line_length > 0) {
        if (write_all_bytes(database->data_fd, "\n", 1) < 0) {
            return DB_ERR_IO;
        }
        DbStatus push_status = index_table_push(
            &database->index_table,
            line_start_offset,
            current_line_length,
            1
        );
        if (push_status != DB_OK) {
            return push_status;
        }
    }

    return db_save_index_file(database);
}

/* Замена одной записи переменной длины с безопасной перезаписью через временный файл */
static DbStatus db_replace_record_variable_length(
    Database *database,
    size_t record_number,
    const char *new_text,
    size_t new_length
) {
    char temp_path[DB_MAX_PATH + 16];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", database->data_path);

    int temp_fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (temp_fd < 0) {
        return DB_ERR_IO;
    }

    IndexEntry *target_entry = &database->index_table.entries[record_number - 1];
    off_t prefix_size = target_entry->byte_offset;
    off_t suffix_start = target_entry->byte_offset + (off_t)target_entry->byte_length + 1; /* +1 за '\n' */

    /* 1. Копируем все байты до изменяемой записи */
    if (lseek(database->data_fd, 0, SEEK_SET) < 0) {
        close(temp_fd);
        unlink(temp_path);
        return DB_ERR_IO;
    }

    char copy_buffer[4096];
    off_t remaining_prefix = prefix_size;
    while (remaining_prefix > 0) {
        size_t chunk_size = (remaining_prefix > (off_t)sizeof(copy_buffer))
                            ? sizeof(copy_buffer)
                            : (size_t)remaining_prefix;
        ssize_t bytes_read = read_exact_bytes(database->data_fd, copy_buffer, chunk_size);
        if (bytes_read <= 0 || write_all_bytes(temp_fd, copy_buffer, (size_t)bytes_read) < 0) {
            close(temp_fd);
            unlink(temp_path);
            return DB_ERR_IO;
        }
        remaining_prefix -= bytes_read;
    }

    /* 2. Записываем новое содержимое записи и перевод строки */
    if (new_length > 0) {
        if (write_all_bytes(temp_fd, new_text, new_length) < 0) {
            close(temp_fd);
            unlink(temp_path);
            return DB_ERR_IO;
        }
    }
    if (write_all_bytes(temp_fd, "\n", 1) < 0) {
        close(temp_fd);
        unlink(temp_path);
        return DB_ERR_IO;
    }

    /* 3. Копируем все байты после старой записи */
    if (lseek(database->data_fd, suffix_start, SEEK_SET) < 0) {
        close(temp_fd);
        unlink(temp_path);
        return DB_ERR_IO;
    }

    while (1) {
        ssize_t bytes_read = read(database->data_fd, copy_buffer, sizeof(copy_buffer));
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(temp_fd);
            unlink(temp_path);
            return DB_ERR_IO;
        }
        if (bytes_read == 0) {
            break;
        }
        if (write_all_bytes(temp_fd, copy_buffer, (size_t)bytes_read) < 0) {
            close(temp_fd);
            unlink(temp_path);
            return DB_ERR_IO;
        }
    }

    if (close(temp_fd) < 0) {
        unlink(temp_path);
        return DB_ERR_IO;
    }

    /* Закрываем старый дескриптор, атомарно заменяем файл и открываем заново */
    close(database->data_fd);
    database->data_fd = -1;

    if (rename(temp_path, database->data_path) < 0) {
        unlink(temp_path);
        database->is_open = 0;
        return DB_ERR_IO;
    }

    database->data_fd = open(database->data_path, O_RDWR, 0644);
    if (database->data_fd < 0) {
        database->is_open = 0;
        return DB_ERR_IO;
    }

    /* Пересчитываем смещения в индексной таблице без повторного сканирования файла */
    ssize_t length_delta = (ssize_t)new_length - (ssize_t)target_entry->byte_length;
    target_entry->byte_length = new_length;
    target_entry->is_active = (new_length > 0) ? 1 : 0;

    for (size_t i = record_number; i < database->index_table.count; i++) {
        database->index_table.entries[i].byte_offset += length_delta;
    }

    return db_save_index_file(database);
}

void db_init(Database *database) {
    if (database == NULL) {
        return;
    }
    database->data_fd = -1;
    database->is_open = 0;
    database->data_path[0] = '\0';
    database->index_path[0] = '\0';
    database->index_table.entries = NULL;
    database->index_table.count = 0;
    database->index_table.capacity = 0;
}

int db_is_valid_text(const char *record_text) {
    if (record_text == NULL) {
        return 0;
    }
    size_t length = strlen(record_text);
    if (length == 0 || length >= DB_MAX_RECORD_LEN) {
        return 0;
    }
    if (strchr(record_text, '\n') != NULL || strchr(record_text, '\r') != NULL) {
        return 0;
    }
    return 1;
}

DbStatus db_open(Database *database, const char *file_path) {
    if (database == NULL || file_path == NULL || file_path[0] == '\0') {
        return DB_ERR_INVALID_ARG;
    }
    if (strlen(file_path) >= DB_MAX_PATH - 5) {
        return DB_ERR_INVALID_ARG;
    }

    if (database->is_open) {
        db_close(database);
    }

    int fd = open(file_path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return DB_ERR_IO;
    }

    database->data_fd = fd;
    database->is_open = 1;
    strncpy(database->data_path, file_path, DB_MAX_PATH - 1);
    database->data_path[DB_MAX_PATH - 1] = '\0';
    snprintf(database->index_path, sizeof(database->index_path), "%s.idx", database->data_path);

    DbStatus status = db_rebuild_index(database);
    if (status != DB_OK) {
        db_close(database);
        return status;
    }

    return DB_OK;
}

DbStatus db_close(Database *database) {
    if (database == NULL) {
        return DB_ERR_INVALID_ARG;
    }
    if (database->data_fd >= 0) {
        close(database->data_fd);
        database->data_fd = -1;
    }
    index_table_free(&database->index_table);
    database->is_open = 0;
    database->data_path[0] = '\0';
    database->index_path[0] = '\0';
    return DB_OK;
}

DbStatus db_read(Database *database, size_t record_number, char *out_buffer, size_t buffer_size) {
    if (database == NULL || out_buffer == NULL || buffer_size == 0 || record_number == 0) {
        return DB_ERR_INVALID_ARG;
    }
    if (!database->is_open) {
        return DB_ERR_NOT_OPEN;
    }
    if (record_number > database->index_table.count) {
        return DB_ERR_NOT_FOUND;
    }

    const IndexEntry *entry = &database->index_table.entries[record_number - 1];
    if (!entry->is_active) {
        return DB_ERR_NOT_FOUND;
    }
    if (entry->byte_length + 1 > buffer_size) {
        return DB_ERR_BUFFER_TOO_SMALL;
    }

    /* Прямой доступ за O(1) по смещению из индексной таблицы */
    if (lseek(database->data_fd, entry->byte_offset, SEEK_SET) < 0) {
        return DB_ERR_IO;
    }

    ssize_t bytes_read = read_exact_bytes(database->data_fd, out_buffer, entry->byte_length);
    if (bytes_read != (ssize_t)entry->byte_length) {
        return DB_ERR_IO;
    }
    out_buffer[entry->byte_length] = '\0';

    return DB_OK;
}

DbStatus db_append(Database *database, const char *record_text, size_t *out_record_number) {
    if (database == NULL || !db_is_valid_text(record_text)) {
        return DB_ERR_INVALID_ARG;
    }
    if (!database->is_open) {
        return DB_ERR_NOT_OPEN;
    }

    off_t end_offset = lseek(database->data_fd, 0, SEEK_END);
    if (end_offset < 0) {
        return DB_ERR_IO;
    }

    size_t text_length = strlen(record_text);
    if (write_all_bytes(database->data_fd, record_text, text_length) < 0 ||
        write_all_bytes(database->data_fd, "\n", 1) < 0) {
        return DB_ERR_IO;
    }

    DbStatus status = index_table_push(&database->index_table, end_offset, text_length, 1);
    if (status != DB_OK) {
        return status;
    }

    if (out_record_number != NULL) {
        *out_record_number = database->index_table.count;
    }

    return db_save_index_file(database);
}

DbStatus db_write(Database *database, size_t record_number, const char *record_text) {
    if (database == NULL || record_number == 0 || !db_is_valid_text(record_text)) {
        return DB_ERR_INVALID_ARG;
    }
    if (!database->is_open) {
        return DB_ERR_NOT_OPEN;
    }

    size_t new_length = strlen(record_text);

    /* Если номер за границами БД — дописываем пустые записи, затем целевую */
    if (record_number > database->index_table.count) {
        off_t end_offset = lseek(database->data_fd, 0, SEEK_END);
        if (end_offset < 0) {
            return DB_ERR_IO;
        }

        while (database->index_table.count < record_number - 1) {
            if (write_all_bytes(database->data_fd, "\n", 1) < 0) {
                return DB_ERR_IO;
            }
            DbStatus push_status = index_table_push(&database->index_table, end_offset, 0, 0);
            if (push_status != DB_OK) {
                return push_status;
            }
            end_offset += 1;
        }

        return db_append(database, record_text, NULL);
    }

    /* Если запись уже существует и длина совпадает — перезаписываем на месте */
    IndexEntry *entry = &database->index_table.entries[record_number - 1];
    if (entry->byte_length == new_length) {
        if (lseek(database->data_fd, entry->byte_offset, SEEK_SET) < 0) {
            return DB_ERR_IO;
        }
        if (write_all_bytes(database->data_fd, record_text, new_length) < 0) {
            return DB_ERR_IO;
        }
        entry->is_active = 1;
        return db_save_index_file(database);
    }

    /* Длина изменилась — выполняем безопасную замену записи переменной длины */
    return db_replace_record_variable_length(database, record_number, record_text, new_length);
}

DbStatus db_delete(Database *database, size_t record_number) {
    if (database == NULL || record_number == 0) {
        return DB_ERR_INVALID_ARG;
    }
    if (!database->is_open) {
        return DB_ERR_NOT_OPEN;
    }
    if (record_number > database->index_table.count) {
        return DB_ERR_NOT_FOUND;
    }

    IndexEntry *entry = &database->index_table.entries[record_number - 1];
    if (!entry->is_active) {
        return DB_ERR_NOT_FOUND;
    }

    /* Заменяем содержимое записи на пустую строку, сохраняя нумерацию остальных записей */
    return db_replace_record_variable_length(database, record_number, "", 0);
}

const char *db_status_to_string(DbStatus status) {
    switch (status) {
        case DB_OK:                   return "Операция выполнена успешно";
        case DB_ERR_INVALID_ARG:      return "Некорректные аргументы запроса";
        case DB_ERR_NOT_OPEN:         return "База данных не открыта";
        case DB_ERR_IO:               return "Ошибка ввода-вывода файловой системы";
        case DB_ERR_MEMORY:           return "Недостаточно оперативной памяти";
        case DB_ERR_NOT_FOUND:        return "Запись с указанным номером не найдена";
        case DB_ERR_BUFFER_TOO_SMALL: return "Превышен размер буфера чтения";
        default:                      return "Неизвестная ошибка базы данных";
    }
}
