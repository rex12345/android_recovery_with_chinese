
#include <malloc.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <errno.h>
#include <dirent.h>

#include "bootloader.h"
#include "commands.h"
#include "common.h"
#include "firmware.h"
#include "install.h"
#include "recovery_ui.h"
#include "roots.h"
#include "verifier.h"
#include "minui/minui.h"
#include "mtdutils/mtdutils.h"

#include "extra.h"

int ensure_read(int fd, void* data, int size)
{
	int n, tmp;

	n = 0;
	while(n < size)
	{
		tmp = read(fd, (char*)data+n, size-n);
		if(tmp <= 0)
			break;
		n += tmp;
	}

	return (n == size) ? 0 : -1;
}

int ensure_write(int fd, const void* data, int size)
{
	int n, tmp;

	n = 0;
	while(n < size)
	{
		tmp = write(fd, (char*)data+n, size-n);
		if(tmp <= 0)
			break;
		n += tmp;
	}

	return (n == size) ? 0 : -1;
}

int get_screen_xy(int* x, int* y)
{
	int fd, ret;
	struct fb_var_screeninfo info;

	if(!x || !y)
		return -1;
	fd = open("/dev/graphics/fb0", O_RDONLY);
	if(fd < 0)
		return -1;
	ret = ioctl(fd, FBIOGET_VSCREENINFO, &info);
	close(fd);
	if(ret < 0)
		return -1;
	*x = info.xres;
	*y = info.yres;

	return 0;
}

long long int get_sdcard_size()
{
	int fd, ret;
	long long int size;

	fd = open("/dev/block/mmcblk0", O_RDONLY);
	if(fd < 0)
		return 0;
	ret = ioctl(fd, BLKGETSIZE64, &size);
	close(fd);

	return ret == 0 ? size : 0;
}

void execute(int show, const char* file, char **args)
{
	int fds[2];
	int rdsz;
	char buffer[65];
	int i;
	char** argv;

	i = 0;
	if(args)
	{
		while(args[i])
			i++;
	}
	argv = (char**)(malloc(sizeof(char*)*(i+2)));
	if(!argv)
		return;
	argv[0] = (char*)file;
	argv[i+1] = NULL;
	while(i)
	{
		argv[i] = args[i-1];
		i--;
	}
	if(pipe(fds) == -1)
		return;
	if(fork() == 0)
	{
		close(fds[0]);
		if(-1 == dup2(fds[1], STDOUT_FILENO))
			exit(1);
		if(-1 == execvp(argv[0], argv))
			exit(1);
		free(argv);
	}
	else
	{
		close(fds[1]);
		free(argv);
		for(;;)
		{
			memset(buffer, 0, sizeof(buffer));
			rdsz = read(fds[0], buffer, sizeof(buffer)-1);
			if(rdsz <= 0)
				break;
			if(show)
				ui_print("%s", buffer);
		}
		close(fds[0]);
	}
}

void free_string_array(char** array)
{
    if (array == NULL)
        return;
    char* cursor = array[0];
    int i = 0;
    while (cursor != NULL)
    {
        free(cursor);
        cursor = array[++i];
    }
    free(array);
}

char** enumerate_files_in_directory(const char* directory, const char* extension, int* numFiles)
{
    DIR *dir;
    struct dirent *de;
    int total = 0;
    int i;
    char** files = NULL;
    int pass;
    *numFiles = 0;
    int dirLen = strlen(directory);

    dir = opendir(directory);
    if (dir == NULL) {
        ui_print("无法打开文件夹.\n");
        return NULL;
    }
  
    int extLen = 0;
    if (extension != NULL)
        extLen = strlen(extension);
  
    int isCounting = 1;
    i = 0;
    for (pass = 0; pass < 2; pass++) {
        while ((de=readdir(dir)) != NULL) {
            // skip hidden files
            if (de->d_name[0] == '.')
                continue;
            
            // NULL means that we are gathering directories, so skip this
            if (extension != NULL)
            {
                // make sure that we can have the desired extension (prevent seg fault)
                if (strlen(de->d_name) < (unsigned)extLen)
                    continue;
                // compare the extension
                if (strcmp(de->d_name + strlen(de->d_name) - extLen, extension) != 0)
                    continue;
            }
            else
            {
                struct stat info;
                char fullFileName[PATH_MAX];
                strcpy(fullFileName, directory);
                strcat(fullFileName, de->d_name);
                stat(fullFileName, &info);
                // make sure it is a directory
                if (!(S_ISDIR(info.st_mode)))
                    continue;
            }
            
            if (pass == 0)
            {
                total++;
                continue;
            }
            
            files[i] = (char*) malloc(dirLen + strlen(de->d_name) + 2);
            strcpy(files[i], directory);
            strcat(files[i], de->d_name);
            if (extension == NULL)
                strcat(files[i], "/");
            i++;
        }
        if (pass == 1)
            break;
        if (total == 0)
            break;
        rewinddir(dir);
        *numFiles = total;
        files = (char**) malloc((total+1)*sizeof(char*));
        files[total]=NULL;
    }

    if(closedir(dir) < 0) {
        LOGE("无法关闭文件夹.");
    }

    if (total==0) {
        return NULL;
    }

    return files;
}

// pass in NULL for extension and you will get a directory chooser
char* choose_file_menu(const char* directory, const char* extension, const char* headers[])
{
    DIR *dir;
    struct dirent *de;
    int numFiles = 0;
    int numDirs = 0;
    int i;
    char* return_value = NULL;
    int dir_len = strlen(directory);

    char** files = enumerate_files_in_directory(directory, extension, &numFiles);
    char** dirs = NULL;
    if (extension != NULL)
        dirs = enumerate_files_in_directory(directory, NULL, &numDirs);
    int total = numDirs + numFiles;
    if (total == 0)
    {
        ui_print("无匹配项目.\n");
    }
    else
    {
        char** list = (char**) malloc((total + 1) * sizeof(char*));
        list[total] = NULL;


        for (i = 0 ; i < numDirs; i++)
        {
            list[i] = strdup(dirs[i] + dir_len);
        }

        for (i = 0 ; i < numFiles; i++)
        {
            list[numDirs + i] = strdup(files[i] + dir_len);
        }

        for (;;)
        {
            int chosen_item = get_menu_selection((char**)headers, list, 0);
            if (chosen_item == SELECT_BACK)
                break;
            static char ret[PATH_MAX];
            if (chosen_item < numDirs)
            {
                char* subret = choose_file_menu(dirs[chosen_item], extension, headers);
                if (subret != NULL)
                {
                    strcpy(ret, subret);
                    return_value = ret;
                    break;
                }
                continue;
            } 
            strcpy(ret, files[chosen_item - numDirs]);
            return_value = ret;
            break;
        }
        free_string_array(list);
    }

    free_string_array(files);
    free_string_array(dirs);
    return return_value;
}

// bmp
// --------------------------------------------------------------------------------

#pragma pack(push)
#pragma pack(1)

struct bmp_file_header
{
	unsigned short type;
	unsigned long file_size;
	unsigned short r0;
	unsigned short r1;
	unsigned long data_offset;
};

struct bmp_data_header
{
	unsigned long header_size;
	unsigned long x;
	unsigned long y;
	unsigned short r0;
	unsigned short depth;
	unsigned long compress;
	unsigned long image_size;
	unsigned long r1;
	unsigned long r2;
	unsigned long color;
	unsigned long r3;
};

struct rgbquad
{
	unsigned char red;
	unsigned char green;
	unsigned char blue;
	unsigned char unused;
};

#pragma pack(pop)

// rgb888 to rgb565
static unsigned short convert(unsigned char r, unsigned char g, unsigned char b)
{
	unsigned short rr, gg, bb;

	rr = r>>3;
	gg = g>>2;
	bb = b>>3;
	return ((rr) | (gg<<5) | (bb<<11));
}

// get c bits from addr+n as a little edian number
static int bits(const char* addr, int n, int c)
{
	char t;
	int ret, v, i;

	ret = 0;
	v = 1;
	for(i = 0; i < c; i++)
	{
		t = *((addr)+(n+i)/8) & (1<<((n+i)%8));
		ret |= t ? v : 0;
		v <<= 1;
	}
	return ret;
}

int bmp_info(const char* fn, int* w, int* h)
{
	int fd, ret;
	char buffer[sizeof(struct bmp_file_header)+sizeof(struct bmp_data_header)];
	struct bmp_data_header* hdr;

	if(!w || !h)
		return -1;
	fd = open(fn, O_RDONLY);
	if(fd < 0)
		return -1;
	ret = ensure_read(fd, buffer, sizeof(buffer));
	close(fd);
	if(ret < 0)
		return -1;
	hdr = (struct bmp_data_header*)(buffer+sizeof(struct bmp_file_header));
	*w = hdr->x;
	*h = hdr->y;

	return 0;
}

char* bmp_load(const char* fn)
{
	int fd, ret;
	char *content, *temp;
	struct bmp_file_header* file;
	struct bmp_data_header* data;

	fd = open(fn, O_RDONLY);
	if(fd < 0)
		return NULL;
	content = (char*)(malloc(sizeof(struct bmp_file_header)));
	ret = ensure_read(fd, content, sizeof(struct bmp_file_header));
	if(ret < 0)
	{
		close(fd);
		free(content);
		return NULL;
	}
	file = (struct bmp_file_header*)(content);
	if((file->type != 0x4d42) || file->r0 || file->r1 || (file->data_offset > file->file_size))
	{
		close(fd);
		free(content);
		return NULL;
	}
	temp = (char*)(realloc(content, file->file_size));
	if(!temp)
	{
		close(fd);
		free(content);
		return NULL;
	}
	content = temp;
	file = (struct bmp_file_header*)(content);
	ret = ensure_read(fd, content+sizeof(struct bmp_file_header), file->file_size-sizeof(struct bmp_file_header));
	if(ret < 0)
	{
		close(fd);
		free(content);
		return NULL;
	}
	close(fd);
	data = (struct bmp_data_header*)(content+sizeof(struct bmp_file_header));
	if(data->compress)
	{
		free(content);
		return NULL;
	}

	return content;
}

// convert bmp to rgb565
char* bmp_convert(const char* content)
{
	unsigned short *logo;
	int i, c, x, y;
	struct bmp_file_header* file;
	struct bmp_data_header* data;
	struct rgbquad* rgb;

	data = (struct bmp_data_header*)(content+sizeof(struct bmp_file_header));
	x = data->x;
	y = data->y;
	logo = (unsigned short*)(malloc(sizeof(unsigned short)*x*y));
	if(!logo)
		return 0;
	file = (struct bmp_file_header*)(content);
	// there is a platte
	if(file->data_offset > (sizeof(struct bmp_file_header)+sizeof(struct bmp_data_header)))
	{
        for(i = 0; i < x*y; i++)
        {
            c = bits((content+file->data_offset), data->depth*i, data->depth);
            rgb = (struct rgbquad*)(content+sizeof(struct bmp_file_header)+sizeof(struct bmp_data_header))+c;
            logo[(y-1-i/x)*x+i%x] = convert(rgb->red, rgb->green, rgb->blue);
        }
	}
	else
	{
        for(i = 0; i < x*y; i++)
        {
            c = bits((content+file->data_offset), data->depth*i, data->depth);
            logo[(y-1-i/x)*x+i%x] = convert((c & 0x000000ff), (c & 0x0000ff00)>>8, (c & 0x00ff0000)>>16);
        }
	}
	return (char*)(logo);
}

int bmp_to_565(const char* in, const char* out)
{
	int x, y, fd, ret;
	char* data;
	char* logo;

	if(bmp_info(in, &x, &y) < 0)
		return -1;
	data = bmp_load(in);
	if(!data)
		return -1;
	logo = bmp_convert(data);
	if(!logo)
	{
		free(data);
		return -1;
	}
	fd = open(out, O_CREAT|O_WRONLY);
	if(fd < 0)
	{
		free(logo);
		free(data);
		return -1;
	}
	ret = ensure_write(fd, logo, sizeof(unsigned short)*x*y);
	close(fd);
	free(logo);
	free(data);

	return ret;
}

// jpg
// --------------------------------------------------------------------------------

// png
// --------------------------------------------------------------------------------

