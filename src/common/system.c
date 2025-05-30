#include "system.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

void sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

void sleep_us(uint64_t us) {
    struct timespec ts;
    ts.tv_sec = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000;
    nanosleep(&ts, NULL);
}

uint32_t get_uptime_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // monotonic is uptime on linux
    return ts.tv_sec;
}

uint32_t get_uptime_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // monotonic is uptime on linux
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

uint64_t get_uptime_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // monotonic is uptime on linux
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

uint32_t get_unix_time_s(void) {
    time_t rawtime;
    time(&rawtime);
    return (uint32_t)rawtime;
}

int run_bash_command(char *in, char *out, int out_max_len) {
    int out_len = 0;
    FILE *pipe = popen(in, "r");
    if (pipe == NULL) {
        return -errno;
    }
    int c;
    for (out_len = 0; (c = fgetc(pipe)) != EOF; out_len++) {
        if (out_len > (out_max_len - 1)) {
            break; // max len reached
        }
        out[out_len] = (char)c;
    }
    pclose(pipe);
    out_len++;
    out[out_len - 1] = '\0';
    return out_len;
}

bool is_file(char *path) {
    bool r = false;
    FILE *fptr = fopen(path, "r");
    if (fptr != NULL) {
        fclose(fptr);
        r = true;
    }
    return r;
}

bool is_file_in_dir(char *dir_path, char *file_name) {
    bool found = false;
    DIR *d = opendir(dir_path);
    if (d == NULL) {
        return found;
    }

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) { // directory found
        if (strncmp(dir->d_name, ".", strlen(dir->d_name)) == 0 ||
            strncmp(dir->d_name, "..", strlen(dir->d_name)) == 0) {
            continue; // skip . and ..
        }
        if (strncmp(dir->d_name, file_name, strlen(dir->d_name)) == 0) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

int copy_file(char *src, char *dest) {
    int r = 0;

    int src_fd = open(src, O_RDONLY);
    if (src_fd == -1) {
        return -errno;
    }

    char *tmp = dest;
    if (is_dir(dest)) {
        char dest_tmp[PATH_MAX];
        path_join(dest, basename(src), dest_tmp, PATH_MAX);
        tmp = dest_tmp;
    }

    int dest_fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dest_fd == -1) {
        close(src_fd);
        return -errno;
    }

    struct stat file_stat = {0};
    int result = fstat(src_fd, &file_stat);
    off_t offset = 0;
    ssize_t written;
    while ((result == 0) && (offset < file_stat.st_size)) {
        written = sendfile(dest_fd, src_fd, &offset, file_stat.st_size);
        if (written == -1) {
            r = -errno;
            break;
        } else {
            offset += written;
        }
    }

    close(src_fd);
    close(dest_fd);
    return r;
}

int move_file(char *src, char *dest) {
    int r = copy_file(src, dest);
    if (r >= 0) {
        r = remove(src);
        if (r == -1) {
            r = -errno;
        }
    }
    return r;
}

int get_file_crc32(char *file_path, uint32_t *crc) {
    if (!file_path || !crc) {
        return -EINVAL;
    }

    FILE *fp = fopen(file_path, "r");
    if (!fp) {
        return -errno;
    }

    int r = 0;
    void *file_data = NULL;

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    if (file_size <= 0) {
        r = -errno;
        goto crc_end;
    }
    fseek(fp, 0, SEEK_SET);

    file_data = malloc(file_size);
    if (!file_data) {
        r = -ENOMEM;
        goto crc_end;
    }

    if (fread(file_data, 1, file_size, fp) != (size_t)file_size) {
        r = -1;
        goto crc_end;
    }

    if (r == 0) {
        uint32_t tmp = crc32(0L, Z_NULL, 0);
        tmp = crc32(tmp, file_data, file_size);
        *crc = tmp;
    }

crc_end:
    if (file_data) {
        free(file_data);
    }
    if (fp) {
        fclose(fp);
    }
    return r;
}

bool check_file_crc32_match(char *file_path_1, char *file_path_2) {
    uint32_t crc1;
    uint32_t crc2;
    if (get_file_crc32(file_path_1, &crc1) || get_file_crc32(file_path_2, &crc2)) {
        return false;
    }
    return crc1 == crc2;
}

bool is_dir(char *path) {
    bool r = false;
    DIR *dir = opendir(path);
    if (dir != NULL) {
        closedir(dir);
        r = true;
    }
    return r;
}

int mkdir_path(char *path, mode_t mode) {
    char temp_path[PATH_MAX];
    int r = 0;

    if (path == NULL) {
        return -EINVAL;
    }

    // start on 1 to skip 1st '/' in absolut path
    for (size_t i = 1; i <= strlen(path) && i < PATH_MAX - 1; ++i) {
        if (path[i] == '/' || i == strlen(path)) {
            strncpy(temp_path, path, i);
            temp_path[i] = '\0';

            if (is_dir(temp_path)) {
                continue; // dir already exist
            }

            if ((r = mkdir(temp_path, mode)) != 0) {
                break;
            }
        }
    }

    return r;
}

int clear_dir(char *path) {
    DIR *d = opendir(path);
    if (d == NULL) { // add all existing file to list
        return -ENOENT;
    }

    int r = 0;
    char filepath[PATH_MAX];
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) { // directory found
        if (strncmp(dir->d_name, ".", strlen(dir->d_name)) == 0 ||
            strncmp(dir->d_name, "..", strlen(dir->d_name)) == 0) {
            continue; // skip . and ..
        }

        if (path[strlen(path) - 1] == '/') { // path ends with a '/'
            sprintf(filepath, "%s%s", path, dir->d_name);
        } else { // need a '/' at end of path
            sprintf(filepath, "%s/%s", path, dir->d_name);
        }

        if ((r = remove(filepath)) != 0) {
            break; // remove failed
        }
    }
    closedir(d);
    return r;
}

int files_in_dir(char *path) {
    DIR *d = opendir(path);
    if (d == NULL) {
        return -ENOENT;
    }

    int count = 0;
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) { // directory found
        if (strncmp(dir->d_name, ".", strlen(dir->d_name)) == 0 ||
            strncmp(dir->d_name, "..", strlen(dir->d_name)) == 0) {
            continue; // skip . and ..
        }
        count++;
    }
    closedir(d);
    return count;
}

int path_join(char *head, char *tail, char *out, uint32_t out_len) {
    size_t head_len = strlen(head);
    size_t tail_len = strlen(tail);
    bool head_startswith_slash = head[0] == '/';
    bool tail_startswith_slash = tail[0] == '/';
    bool head_is_root = (head_len == 1) && head_startswith_slash;
    bool tail_is_root = (tail_len == 1) && tail_startswith_slash;

    if (tail_is_root || (head_is_root && out_len < 2)) {
        return -EINVAL;
    }

    int r = 0;
    if (head_is_root && tail_is_root) {
        out[0] = '/';
        out[1] = '\0';
    } else if (head_is_root) {
        if (tail_startswith_slash) {
            sprintf(out, "%s", tail);
        } else {
            sprintf(out, "/%s", tail);
        }
    } else {
        strncpy(out, head, head_len);
        if (head[head_len - 1] == '/') {
            out[head_len] = '\0';
        } else {
            out[head_len] = '/';
            out[head_len + 1] = '\0';
        }
        strncat(out, tail, tail_len);
        if (out[strlen(out)] == '/') {
            out[strlen(out)] = '\0';
        }
    }

    return r;
}
