#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <syslog.h>
#include <libconfig.h>
#include <dirent.h>
#include <stdbool.h>

#define CFG_PATH "/etc/daemon.conf"
#define LOG_PATH "/var/log/daemon.log"

// Глобальные переменные для конфигурации
const char *dir_path = NULL;
int check_period = 10;
bool is_first_run = true; // Флаг первого запуска

// Структура для хранения состояния
struct file_state {
    char path[1024];   // Путь к файлу
    time_t mtime;      // Время последнего изменения
    time_t atime;      // Время последнего доступа
    struct file_state *next; // Указатель на следующий элемент
};

struct file_state *directory_state = NULL;

// Функции для добавления и удаления состояний
void add_or_update_file(const char *path, time_t mtime, time_t atime) {
    struct file_state *current = directory_state;

    // Проверяем, существует ли файл в списке
    while (current) {
        if (strcmp(current->path, path) == 0) {
            current->mtime = mtime; // Обновляем время изменения
            current->atime = atime; // Обновляем время последнего доступа
            return;
        }
        current = current->next;
    }

    // Если файл не найден, добавляем новый элемент
    struct file_state *new_state = malloc(sizeof(struct file_state));
    strncpy(new_state->path, path, sizeof(new_state->path));
    new_state->mtime = mtime;
    new_state->atime = atime;
    new_state->next = directory_state;
    directory_state = new_state;
}


void remove_file(const char *path) {
    struct file_state **current = &directory_state;

    while (*current) {
        if (strcmp((*current)->path, path) == 0) {
            struct file_state *to_delete = *current;
            *current = to_delete->next;
            free(to_delete);
            return;
        }
        current = &(*current)->next;
    }
}

// Освобождение памяти при заверщении работы
void free_directory_state() {
    struct file_state *current = directory_state;
    while (current) {
        struct file_state *to_delete = current;
        current = current->next;
        free(to_delete);
    }
    directory_state = NULL;
}



bool read_cfg(const char *cfg_path) {
    config_t cfg;
    config_init(&cfg);

    if (!config_read_file(&cfg, cfg_path)) {
        syslog(LOG_ERR, "Ошибка чтения конфигурационного файла: %s", cfg_path);
        config_destroy(&cfg);
        return false;
    }

    const char *temp_dir_path;
    if (!config_lookup_string(&cfg, "dir", &temp_dir_path)) {
        syslog(LOG_ERR, "Ошибка чтения параметра dir из конфигурационного файла");
        config_destroy(&cfg);
        return false;
    }

    // Копируем строку в отдельный буфер
    static char dir_buffer[1024];
    snprintf(dir_buffer, sizeof(dir_buffer), "%s", temp_dir_path);
    dir_path = dir_buffer; // Присваиваем глобальной переменной

    if (!config_lookup_int(&cfg, "period", &check_period)) {
        syslog(LOG_ERR, "Ошибка чтения параметра period из конфигурационного файла");
        config_destroy(&cfg);
        return false;
    }

    syslog(LOG_INFO, "Демон будет следить за директорией: %s", dir_path);

    config_destroy(&cfg);
    return true;
}


// Функция для проверки модификации файлов в директории
void check_directory(const char *path, bool is_root) {
    if (is_root) {
        syslog(LOG_INFO, "Начата проверка директории: %s", path);
    }

    DIR *dir = opendir(path);
    if (!dir) {
        syslog(LOG_ERR, "Не удалось открыть директорию: %s", path);
        return;
    }

    struct dirent *entry;
    struct stat file_stat;
    int has_entries = 0; // Флаг наличия записей
    struct file_state *current_state = NULL;

    // Проход по всем объектам в директории
    while ((entry = readdir(dir)) != NULL) {
        has_entries = 1; // Если найдена запись, устанавливаем флаг

        // Пропускаем записи "." и ".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        if (stat(full_path, &file_stat) < 0) {
            syslog(LOG_ERR, "Ошибка доступа к файлу: %s", full_path);
            continue;
        }

        if (S_ISDIR(file_stat.st_mode)) { // Если объект - директория
            if (!is_first_run) {
                struct file_state *current = directory_state;
                bool exists = false;

                // Проверяем, существует ли директория в текущем состоянии
                while (current) {
                    if (strcmp(current->path, full_path) == 0) {
                        exists = true;
                        break;
                    }
                    current = current->next;
                }

                if (!exists) {
                    syslog(LOG_INFO, "Создана новая директория: %s", full_path);
                }
            }
            add_or_update_file(full_path, file_stat.st_mtime, file_stat.st_atime);

            // Рекурсивный вызов для проверки содержимого поддиректории
            check_directory(full_path, false);

        } else if (S_ISREG(file_stat.st_mode)) { // Если объект - файл
         if (!is_first_run) {
          struct file_state *current = directory_state;
          bool exists = false;

          // Проверяем, существует ли файл в текущем состоянии
          while (current) {
            if (strcmp(current->path, full_path) == 0) {
                exists = true;

                // Проверяем, изменился ли файл
                if (file_stat.st_mtime != current->mtime) {
                    syslog(LOG_INFO, "Файл изменён: %s", full_path);
                    current->mtime = file_stat.st_mtime; // Обновляем время изменения
                }

                // Проверяем, был ли доступ к файлу
                if (file_stat.st_atime != current->atime) {
                    syslog(LOG_INFO, "Файл открыт (доступ): %s", full_path);
                    current->atime = file_stat.st_atime; // Обновляем время последнего доступа
                }
                break;
            }
            current = current->next;
        }

        if (!exists) {
            syslog(LOG_INFO, "Создан новый файл: %s", full_path);
        }
    }
    add_or_update_file(full_path, file_stat.st_mtime, file_stat.st_atime);
}

    }

    // Если файлов не найдено, логируем пустую директорию
    if (!has_entries && is_root) {
        syslog(LOG_INFO, "Директория %s пуста", path);
    }

    closedir(dir);

    // Проверка на удаление объектов
    if (!is_first_run) {
        struct file_state **current = &directory_state;
        while (*current) {
            if (strncmp((*current)->path, path, strlen(path)) == 0) {
                struct stat temp_stat;
                if (stat((*current)->path, &temp_stat) < 0) {
                    syslog(LOG_INFO, "Удалён файл или директория: %s", (*current)->path);
                    struct file_state *to_delete = *current;
                    *current = to_delete->next;
                    free(to_delete);
                    continue;
                }
            }
            current = &(*current)->next;
        }
    }

    // Убираем флаг первого запуска после первой проверки
    if (is_root && is_first_run) {
        is_first_run = false;
    }
}

// Обработчик сигналов
void signal_handler(int sig) {
    switch (sig) {
        case SIGHUP:
            if (read_cfg(CFG_PATH)) {
                syslog(LOG_INFO, "Конфигурационный файл успешно перечитан. Новый каталог: %s, интервал: %d секунд", dir_path, check_period);

            } else {
                syslog(LOG_ERR, "Ошибка перечитывания конфигурационного файла");
            }
            break;

        case SIGTERM:
            syslog(LOG_INFO, "Демон завершает свою работу. Освобождение ресурсов...");
            free_directory_state(); // Освобождаем память
            syslog(LOG_INFO, "Все ресурсы освобождены. Завершение работы демона.");
            closelog();
            exit(0);
            break;
    }
}

// Функция для демонизации процесса
void daemonize() {
    pid_t pid = fork();

    if (pid < 0) {
        exit(EXIT_FAILURE);
    }

    if (pid > 0) {
        exit(EXIT_SUCCESS); // Родитель завершает работу
    }

    umask(0);

    if (setsid() < 0) {
        exit(EXIT_FAILURE);
    }

    if (chdir("/") < 0) {
        exit(EXIT_FAILURE);
    }

    for (int fd = sysconf(_SC_OPEN_MAX); fd >= 0; fd--) {
        close(fd);
    }

    openlog("daemon", LOG_PID, LOG_DAEMON);
    syslog(LOG_INFO, "Демон успешно инициализирован и стал фоновым процессом.");
}

// Главная функция
int main() {
    if (!read_cfg(CFG_PATH)) {
        fprintf(stderr, "Ошибка чтения конфигурационного файла\n");
        return EXIT_FAILURE;
    }

    daemonize();

    signal(SIGHUP, signal_handler);
    signal(SIGTERM, signal_handler);

    syslog(LOG_INFO, "Демон запущен, наблюдение за каталогом: %s", dir_path);

    while (1) {
        check_directory(dir_path, true);
        sleep(check_period);
    }

    return EXIT_SUCCESS;
}
