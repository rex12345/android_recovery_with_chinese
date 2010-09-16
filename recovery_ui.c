/*
 * Copyright (C) 2009 The Android Open Source Project
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

#include <linux/input.h>

#include "recovery_ui.h"
#include "common.h"

char* MENU_HEADERS[] = { "Android系统恢复工具",
                         "",
                         "中文支持: www.goapk.com",
                         "",
                         NULL };

char* MENU_ITEMS[] = { "立即重启系统",
                       "U盘模式切换",
                       "快速安装update.zip",
                       "快速更新splash.bmp",
                       "从SD卡安装升级文件",
                       "从SD卡更新开机屏幕",
                       "清空数据",
                       "清空缓存",
                       "SD卡分区(vfat/ext3/swap)",
                       NULL };

int device_toggle_display(volatile char* key_pressed, int key_code) {
    return key_code == KEY_HOME;
}

int device_reboot_now(volatile char* key_pressed, int key_code) {
    return 0;
}

int device_handle_key(int key_code, int visible) {
    if (visible) {
        switch (key_code) {
            case KEY_DOWN:
                return HIGHLIGHT_DOWN;

            case KEY_UP:
                return HIGHLIGHT_UP;

            case KEY_ENTER:
			case KEY_CENTER:
			case BTN_MOUSE:
            case KEY_F21:
                return SELECT_ITEM;

			case KEY_BACK:
			case KEY_POWER:
				return SELECT_BACK;

			case KEY_LEFT:
			case KEY_SEND:
			case KEY_VOLUMEDOWN:
				return SELECT_LEFT;

			case KEY_RIGHT:
			case KEY_END:
			case KEY_VOLUMEUP:
				return SELECT_RIGHT;
        }
    }

    return NO_ACTION;
}

int device_perform_action(int which) {
    return which;
}

int device_wipe_data() {
    return 0;
}

int get_menu_selection(char** headers, char** items, int menu_only) {
    // throw away keys pressed previously, so user doesn't
    // accidentally trigger menu items.
    ui_clear_key_queue();

    ui_start_menu(headers, items);
    int selected = 0;
    int chosen_item = -1;

    while (chosen_item < 0) {
        int key = ui_wait_key();
        int visible = ui_text_visible();

        int action = device_handle_key(key, visible);

        if (action < 0) {
            switch (action) {
                case HIGHLIGHT_UP:
                    --selected;
                    selected = ui_menu_select(selected);
                    break;
                case HIGHLIGHT_DOWN:
                    ++selected;
                    selected = ui_menu_select(selected);
                    break;
                case SELECT_ITEM:
                    chosen_item = selected;
                    break;
                case NO_ACTION:
                    break;
            }
			if(action == SELECT_BACK)
			{
				chosen_item = action;
				break;
			}
        } else if (!menu_only) {
            chosen_item = action;
        }
    }

    ui_end_menu();
    return chosen_item;
}

