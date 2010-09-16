/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "bootloader.h"
#include "commands.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"
#include "extra.h"

static const struct option OPTIONS[] = {
  { "send_intent", required_argument, NULL, 's' },
  { "update_package", required_argument, NULL, 'u' },
  { "wipe_data", no_argument, NULL, 'w' },
  { "wipe_cache", no_argument, NULL, 'c' },
  { NULL, 0, NULL, 0 },
};

static const char *COMMAND_FILE = "CACHE:recovery/command";
static const char *INTENT_FILE = "CACHE:recovery/intent";
static const char *LOG_FILE = "CACHE:recovery/log";
static const char *SDCARD_PACKAGE_FILE = "SDCARD:update.zip";
static const char *TEMPORARY_LOG_FILE = "/tmp/recovery.log";

/*
 * The recovery tool communicates with the main system through /cache files.
 *   /cache/recovery/command - INPUT - command line for tool, one arg per line
 *   /cache/recovery/log - OUTPUT - combined log file from recovery run(s)
 *   /cache/recovery/intent - OUTPUT - intent that was passed in
 *
 * The arguments which may be supplied in the recovery.command file:
 *   --send_intent=anystring - write the text out to recovery.intent
 *   --update_package=root:path - verify install an OTA package file
 *   --wipe_data - erase user data (and cache), then reboot
 *   --wipe_cache - wipe cache (but not user data), then reboot
 *
 * After completing, we remove /cache/recovery/command and reboot.
 * Arguments may also be supplied in the bootloader control block (BCB).
 * These important scenarios must be safely restartable at any point:
 *
 * FACTORY RESET
 * 1. user selects "factory reset"
 * 2. main system writes "--wipe_data" to /cache/recovery/command
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--wipe_data"
 *    -- after this, rebooting will restart the erase --
 * 5. erase_root() reformats /data
 * 6. erase_root() reformats /cache
 * 7. finish_recovery() erases BCB
 *    -- after this, rebooting will restart the main system --
 * 8. main() calls reboot() to boot main system
 *
 * OTA INSTALL
 * 1. main system downloads OTA package to /cache/some-filename.zip
 * 2. main system writes "--update_package=CACHE:some-filename.zip"
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--update_package=..."
 *    -- after this, rebooting will attempt to reinstall the update --
 * 5. install_package() attempts to install the update
 *    NOTE: the package install must itself be restartable from any point
 * 6. finish_recovery() erases BCB
 *    -- after this, rebooting will (try to) restart the main system --
 * 7. ** if install failed **
 *    7a. prompt_and_wait() shows an error icon and waits for the user
 *    7b; the user reboots (pulling the battery, etc) into the main system
 * 8. main() calls maybe_install_firmware_update()
 *    ** if the update contained radio/hboot firmware **:
 *    8a. m_i_f_u() writes BCB with "boot-recovery" and "--wipe_cache"
 *        -- after this, rebooting will reformat cache & restart main system --
 *    8b. m_i_f_u() writes firmware image into raw cache partition
 *    8c. m_i_f_u() writes BCB with "update-radio/hboot" and "--wipe_cache"
 *        -- after this, rebooting will attempt to reinstall firmware --
 *    8d. bootloader tries to flash firmware
 *    8e. bootloader writes BCB with "boot-recovery" (keeping "--wipe_cache")
 *        -- after this, rebooting will reformat cache & restart main system --
 *    8f. erase_root() reformats /cache
 *    8g. finish_recovery() erases BCB
 *        -- after this, rebooting will (try to) restart the main system --
 * 9. main() calls reboot() to boot main system
 */

static const int MAX_ARG_LENGTH = 4096;
static const int MAX_ARGS = 100;

// open a file given in root:path format, mounting partitions as necessary
static FILE*
fopen_root_path(const char *root_path, const char *mode) {
    if (ensure_root_path_mounted(root_path) != 0) {
        LOGE("无法挂载：%s\n", root_path);
        return NULL;
    }

    char path[PATH_MAX] = "";
    if (translate_root_path(root_path, path, sizeof(path)) == NULL) {
        LOGE("路径错误：%s\n", root_path);
        return NULL;
    }

    // When writing, try to create the containing directory, if necessary.
    // Use generous permissions, the system (init.rc) will reset them.
    if (strchr("wa", mode[0])) dirCreateHierarchy(path, 0777, NULL, 1);

    FILE *fp = fopen(path, mode);
    return fp;
}

// close a file, log an error if the error indicator is set
static void
check_and_fclose(FILE *fp, const char *name) {
    fflush(fp);
    if (ferror(fp)) LOGE("%s出错\n(%s)\n", name, strerror(errno));
    fclose(fp);
}

// command line args come from, in decreasing precedence:
//   - the actual command line
//   - the bootloader control block (one per line, after "recovery")
//   - the contents of COMMAND_FILE (one per line)
static void
get_args(int *argc, char ***argv) {
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    get_bootloader_message(&boot);  // this may fail, leaving a zeroed structure

    if (boot.command[0] != 0 && boot.command[0] != 255) {
        LOGI("Boot command: %.*s\n", sizeof(boot.command), boot.command);
    }

    if (boot.status[0] != 0 && boot.status[0] != 255) {
        LOGI("Boot status: %.*s\n", sizeof(boot.status), boot.status);
    }

    // --- if arguments weren't supplied, look in the bootloader control block
    if (*argc <= 1) {
        boot.recovery[sizeof(boot.recovery) - 1] = '\0';  // Ensure termination
        const char *arg = strtok(boot.recovery, "\n");
        if (arg != NULL && !strcmp(arg, "recovery")) {
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = strdup(arg);
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if ((arg = strtok(NULL, "\n")) == NULL) break;
                (*argv)[*argc] = strdup(arg);
            }
            LOGI("Got arguments from boot message\n");
        } else if (boot.recovery[0] != 0 && boot.recovery[0] != 255) {
            LOGE("无效启动信息\n\"%.20s\"\n", boot.recovery);
        }
    }

    // --- if that doesn't work, try the command file
    if (*argc <= 1) {
        FILE *fp = fopen_root_path(COMMAND_FILE, "r");
        if (fp != NULL) {
            char *argv0 = (*argv)[0];
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = argv0;  // use the same program name

            char buf[MAX_ARG_LENGTH];
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if (!fgets(buf, sizeof(buf), fp)) break;
                (*argv)[*argc] = strdup(strtok(buf, "\r\n"));  // Strip newline.
            }

            check_and_fclose(fp, COMMAND_FILE);
            LOGI("Got arguments from %s\n", COMMAND_FILE);
        }
    }

    // --> write the arguments we have back into the bootloader control block
    // always boot into recovery after this (until finish_recovery() is called)
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    int i;
    for (i = 1; i < *argc; ++i) {
        strlcat(boot.recovery, (*argv)[i], sizeof(boot.recovery));
        strlcat(boot.recovery, "\n", sizeof(boot.recovery));
    }
    set_bootloader_message(&boot);
}

static void
set_sdcard_update_bootloader_message()
{
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    set_bootloader_message(&boot);
}

// clear the recovery command and prepare to boot a (hopefully working) system,
// copy our log file to cache as well (for the system to read), and
// record any intent we were asked to communicate back to the system.
// this function is idempotent: call it as many times as you like.
static void
finish_recovery(const char *send_intent)
{
    // By this point, we're ready to return to the main system...
    if (send_intent != NULL) {
        FILE *fp = fopen_root_path(INTENT_FILE, "w");
        if (fp == NULL) {
            LOGE("无法打开%s\n", INTENT_FILE);
        } else {
            fputs(send_intent, fp);
            check_and_fclose(fp, INTENT_FILE);
        }
    }

    // Copy logs to cache so the system can find out what happened.
    FILE *log = fopen_root_path(LOG_FILE, "a");
    if (log == NULL) {
        LOGE("无法打开%s\n", LOG_FILE);
    } else {
        FILE *tmplog = fopen(TEMPORARY_LOG_FILE, "r");
        if (tmplog == NULL) {
            LOGE("无法打开%s\n", TEMPORARY_LOG_FILE);
        } else {
            static long tmplog_offset = 0;
            fseek(tmplog, tmplog_offset, SEEK_SET);  // Since last write
            char buf[4096];
            while (fgets(buf, sizeof(buf), tmplog)) fputs(buf, log);
            tmplog_offset = ftell(tmplog);
            check_and_fclose(tmplog, TEMPORARY_LOG_FILE);
        }
        check_and_fclose(log, LOG_FILE);
    }

    // Reset the bootloader message to revert to a normal main system boot.
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    set_bootloader_message(&boot);

    // Remove the command file, so recovery won't repeat indefinitely.
    char path[PATH_MAX] = "";
    if (ensure_root_path_mounted(COMMAND_FILE) != 0 ||
        translate_root_path(COMMAND_FILE, path, sizeof(path)) == NULL ||
        (unlink(path) && errno != ENOENT)) {
        LOGW("Can't unlink %s\n", COMMAND_FILE);
    }

    sync();  // For good measure.
}

static int
erase_root(const char *root)
{
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();
    ui_print("正在格式化%s...\n", root);
    return format_root_device(root);
}

static char**
prepend_title(char** headers) {
    char* title[] = { "Android系统恢复<"
                          EXPAND(RECOVERY_API_VERSION) "e>",
                      "",
                      NULL };

    // count the number of lines in our title, plus the
    // caller-provided headers.
    int count = 0;
    char** p;
    for (p = title; *p; ++p, ++count);
    for (p = headers; *p; ++p, ++count);

    char** new_headers = malloc((count+1) * sizeof(char*));
    char** h = new_headers;
    for (p = title; *p; ++p, ++h) *h = *p;
    for (p = headers; *p; ++p, ++h) *h = *p;
    *h = NULL;

    return new_headers;
}

static void
wipe_data(int confirm) {
    if (confirm) {
        static char** title_headers = NULL;

        if (title_headers == NULL) {
            char* headers[] = { "确认清除所有数据？",
                                "  操作不可逆！",
                                "",
                                NULL };
            title_headers = prepend_title(headers);
        }

        char* items[] = { 
                          " 不要啊",
                          " 好吧，删了吧",   // [7]
                          NULL };

        int chosen_item = get_menu_selection(title_headers, items, 1);
        if (chosen_item != 1) {
            return;
        }
    }

    ui_print("\n-- 清空数据...\n");
    device_wipe_data();
    erase_root("DATA:");
    erase_root("CACHE:");
    ui_print("清空数据完成.\n");
}

static void install_zip_file(const char* file)
{
    set_sdcard_update_bootloader_message();
	ui_print("\n安装开始.\n");
    int status = install_package(file);
    if (status != INSTALL_SUCCESS)
	{
    	ui_set_background(BACKGROUND_ICON_ERROR);
    	ui_print("安装中断.\n");
    } 
	else if (!ui_text_visible())
	{
   		return;  // reboot if logs aren't visible
    }
	else
	{
    	if (firmware_update_pending())
		{
    		ui_print("\n重启以完成安装.\n");
    	}
		else
		{
    		ui_print("\n安装完成.\n");
    	}
    }
}

#define SDCARD_SPLASH_FILE "/sdcard/splash.bmp"
#define RGB565_SPLASH_FILE "/tmp/splash.565"

static void install_bmp_file(const char* file)
{
	int ret;
	int sx, sy, bx, by;
	char* argv[4];

	ret = ensure_root_path_mounted("SDCARD:");
	if(ret != 0)
		return;
	ret = get_screen_xy(&sx, &sy);
	if(ret < 0)
	{
		ui_print("无法获得屏幕大小!\n");
		return;
	}
	ret = bmp_info(file, &bx, &by);
	if(ret < 0)
	{
		ui_print("无法打开%s!\n", file);
		return;
	}
	if(sx != bx || sy != by)
	{
		ui_print("警告!\n");
		ui_print("屏幕大小是%dx%d.\n", sx, sy);
		ui_print("但位图大小是%dx%d.\n", bx, by);
	}
	ret = bmp_to_565(file, RGB565_SPLASH_FILE);
	if(ret < 0)
	{
		ui_print("无法转换%s!\n", file);
		return;
	}
	argv[0] = "/sbin/flash.sh";
	argv[1] = "splash";
	argv[2] = RGB565_SPLASH_FILE;
	argv[3] = NULL;

	execute(1, "/bin/sh", argv);
}

static void process_ums_toggle()
{
	char *argv[2];

	argv[0] = "/sbin/umstgl.sh";
	argv[1] = NULL;

	execute(1, "/bin/sh", (char**)argv);
}

static void process_browse_update()
{
	int ret;
	char* file;
	char* path;
	const char* headers[] = {
		"选择一个ZIP文件",
		"",
		NULL,
	};

	ret = ensure_root_path_mounted("SDCARD:");
	if(ret != 0)
		return;
	file = choose_file_menu("/sdcard/", ".zip", headers);
	if(file)
	{
		path = (char*)malloc(strlen(file)+1);
		strcpy(path, "SDCARD:");
		strcat(path, file+8);
		install_zip_file(path);
		free(path);
	}
}

static void process_browse_splash()
{
	int ret;
	char* file;
	const char* headers[] = {
		"选择一个BMP文件",
		"",
		NULL,
	};

	ret = ensure_root_path_mounted("SDCARD:");
	if(ret != 0)
		return;
	file = choose_file_menu("/sdcard/", ".bmp", headers);
	if(file)
		install_bmp_file(file);
}

static void process_partition()
{
	char headers[3][64];
	char* theHeaders[6];
	char items[3][32];
	char* theItems[4];
    int selected, key, visible, action;
	long long int size;
	int mbsd, mbvf, mbex, mbsw;
	char args[6][16];
	char* argv[8];

	theHeaders[0] = (char*)headers[0];
	theHeaders[1] = (char*)headers[1];
	theHeaders[2] = "";
	theHeaders[3] = (char*)headers[2];
	theHeaders[4] = "";
	theHeaders[5] = NULL;
	theItems[0] = (char*)items[0];
	theItems[1] = (char*)items[1];
	theItems[2] = (char*)items[2];
	theItems[3] = NULL;

	size = get_sdcard_size();
	if(size == 0)
	{
		ui_print("SD卡未就绪!\n");
		return;
	}
	mbsd = ((size>>20)&0xffffffff);
	mbvf = (mbsd*70/100);
	mbex = mbsd-mbvf;
	mbsw = 0;
	sprintf(headers[0], "SD卡大小%dMB", mbsd);
	sprintf(headers[1], "(方向/音量键调节分区)");
	sprintf(headers[2], "未分配%dMB", mbsd-mbvf-mbex-mbsw);
	sprintf(items[0], "  vfat = %d MB", mbvf);
	sprintf(items[1], "  ext3 = %d MB", mbex);
	sprintf(items[2], "  swap = %d MB", mbsw);

	selected = 0;
    ui_clear_key_queue();
    ui_start_menu(theHeaders, theItems);
    while (1)
	{
        key = ui_wait_key();
        visible = ui_text_visible();
        action = device_handle_key(key, visible);
        if (action < 0)
		{
            switch (action) {
                case HIGHLIGHT_UP:
                    --selected;
                    selected = ui_menu_select(selected);
                    break;
                case HIGHLIGHT_DOWN:
                    ++selected;
                    selected = ui_menu_select(selected);
                    break;
                case NO_ACTION:
                    break;
            }
			if(action == SELECT_BACK)
				break;
			else if(action == SELECT_ITEM)
			{
				if(mbvf == 0)
				{
					ui_print("必须存在vfat分区!\n");
					continue;
				}
				else
				{
					break;
				}
			}
			else if(action == SELECT_LEFT || action == SELECT_RIGHT)
			{
				if(selected == 0)
				{
					mbvf += (action == SELECT_LEFT) ? (-128) : 128;
					if(mbvf < 0) mbvf = 0;
					if(mbvf > (mbsd-mbex-mbsw)) mbvf = (mbsd-mbex-mbsw);
				}
				else if(selected == 1)
				{
					mbex += (action == SELECT_LEFT) ? (-128) : 128;
					if(mbex < 0) mbex = 0;
					if(mbex > (mbsd-mbvf-mbsw)) mbex = (mbsd-mbvf-mbsw);
				}
				else if(selected == 2)
				{
					mbsw += (action == SELECT_LEFT) ? (-16) : 16;
					if(mbsw < 0) mbsw = 0;
					if(mbsw > (mbsd-mbex-mbvf)) mbsw = (mbsd-mbex-mbvf);
				}
				sprintf(headers[2], "未分配%dMB", mbsd-mbvf-mbex-mbsw);
				sprintf(items[0], "  vfat = %d MB", mbvf);
				sprintf(items[1], "  ext3 = %d MB", mbex);
				sprintf(items[2], "  swap = %d MB", mbsw);
				ui_modify_menu(theHeaders, theItems);
			}
        }
    }
	if(action == SELECT_ITEM)
	{
		ui_print("再次按下以确认!\n");
		key = ui_wait_key();
		if(key == KEY_CENTER || key == KEY_ENTER || key == BTN_MOUSE || key == KEY_F21)
		{
			strcpy(args[0], "-v");
			sprintf(args[1], "%d", mbvf);
			strcpy(args[2], "-e");
			sprintf(args[3], "%d", mbex);
			strcpy(args[4], "-s");
			sprintf(args[5], "%d", mbsw);
			argv[0] = "/sbin/partsdc.sh";
			argv[1] = (char*)args[0];
			argv[2] = (char*)args[1];
			argv[3] = (char*)args[2];
			argv[4] = (char*)args[3];
			argv[5] = (char*)args[4];
			argv[6] = (char*)args[5];
			argv[7] = NULL;
			execute(1, "/bin/sh", argv);
		}
	}
    ui_end_menu();
}

static void
prompt_and_wait()
{
    char** headers = prepend_title(MENU_HEADERS);

    for (;;) {
        finish_recovery(NULL);
        ui_reset_progress();

        int chosen_item = get_menu_selection(headers, MENU_ITEMS, 0);

        // device-specific code may take some action here.  It may
        // return one of the core actions handled in the switch
        // statement below.
        chosen_item = device_perform_action(chosen_item);

        switch (chosen_item) {
            case ITEM_REBOOT:
                return;

            case ITEM_WIPE_DATA:
                wipe_data(ui_text_visible());
                if (!ui_text_visible()) return;
                break;

            case ITEM_WIPE_CACHE:
                ui_print("\n-- 清空缓存...\n");
                erase_root("CACHE:");
                ui_print("缓存已清空.\n");
                if (!ui_text_visible()) return;
                break;

            case ITEM_APPLY_SDCARD:
                install_zip_file(SDCARD_PACKAGE_FILE);
                break;

			case ITEM_UMS_TOGGLE:
				process_ums_toggle();
				break;

			case ITEM_APPLY_SPLASH:
				install_bmp_file(SDCARD_SPLASH_FILE);
				break;

			case ITEM_BROWSE_UPDATE:
				process_browse_update();
				break;

			case ITEM_BROWSE_SPLASH:
				process_browse_splash();
				break;

			case ITEM_PARTITION:
				process_partition();
				break;
        }
    }
}

static void
print_property(const char *key, const char *name, void *cookie)
{
    fprintf(stderr, "%s=%s\n", key, name);
}

int
main(int argc, char **argv)
{
    time_t start = time(NULL);

    // If these fail, there's not really anywhere to complain...
    freopen(TEMPORARY_LOG_FILE, "a", stdout); setbuf(stdout, NULL);
    freopen(TEMPORARY_LOG_FILE, "a", stderr); setbuf(stderr, NULL);
    fprintf(stderr, "Starting recovery on %s", ctime(&start));

    ui_init();
    get_args(&argc, &argv);

    int previous_runs = 0;
    const char *send_intent = NULL;
    const char *update_package = NULL;
    int wipe_data = 0, wipe_cache = 0;

    int arg;
    while ((arg = getopt_long(argc, argv, "", OPTIONS, NULL)) != -1) {
        switch (arg) {
        case 'p': previous_runs = atoi(optarg); break;
        case 's': send_intent = optarg; break;
        case 'u': update_package = optarg; break;
        case 'w': wipe_data = wipe_cache = 1; break;
        case 'c': wipe_cache = 1; break;
        case '?':
            LOGE("无效命令参数\n");
            continue;
        }
    }

    fprintf(stderr, "Command:");
    for (arg = 0; arg < argc; arg++) {
        fprintf(stderr, " \"%s\"", argv[arg]);
    }
    fprintf(stderr, "\n\n");

    property_list(print_property, NULL);
    fprintf(stderr, "\n");

#if TEST_AMEND
    test_amend();
#endif

    RecoveryCommandContext ctx = { NULL };
    if (register_update_commands(&ctx)) {
        LOGE("初始化脚本运行环境失败\n");
    }

    int status = INSTALL_SUCCESS;

    if (update_package != NULL) {
        status = install_package(update_package);
        if (status != INSTALL_SUCCESS) ui_print("放弃安装.\n");
    } else if (wipe_data) {
        if (device_wipe_data()) status = INSTALL_ERROR;
        if (erase_root("DATA:")) status = INSTALL_ERROR;
        if (wipe_cache && erase_root("CACHE:")) status = INSTALL_ERROR;
        if (status != INSTALL_SUCCESS) ui_print("清空数据失败.\n");
    } else if (wipe_cache) {
        if (wipe_cache && erase_root("CACHE:")) status = INSTALL_ERROR;
        if (status != INSTALL_SUCCESS) ui_print("清空缓存失败.\n");
    } else {
        status = INSTALL_ERROR;  // No command specified
    }

    if (status != INSTALL_SUCCESS) ui_set_background(BACKGROUND_ICON_ERROR);
    if (status != INSTALL_SUCCESS || ui_text_visible()) prompt_and_wait();

    // If there is a radio image pending, reboot now to install it.
    maybe_install_firmware_update(send_intent);

    // Otherwise, get ready to boot the main system...
    finish_recovery(send_intent);
    ui_print("重启中...\n");
    sync();
    reboot(RB_AUTOBOOT);
    return EXIT_SUCCESS;
}
