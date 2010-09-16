
#ifndef EXTRA_H
#define EXTRA_H

int ensure_read(int fd, void* data, int size);
int ensure_write(int fd, const void* data, int size);

int get_screen_xy(int* x, int* y);

long long int get_sdcard_size();

void execute(int show, const char* file, char **args);

char* choose_file_menu(const char* directory, const char* extension, const char* headers[]);

int bmp_info(const char* fn, int *x, int* y);
char* bmp_load(const char* fn);
char* bmp_convert(const char* content);
int bmp_to_565(const char* in, const char* out);

#endif

