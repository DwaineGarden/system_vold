/*
 * Copyright (C) 2008 The Android Open Source Project
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

#ifndef _G3DEV_H
#define _G3DEV_H

#include <utils/List.h>

#include "Misc.h"
class NetlinkEvent;

class G3Dev : public Misc {
public:
    G3Dev(MiscManager *mm);
    virtual ~G3Dev();
    NetlinkEvent *env;
    int handleUsbEvent(NetlinkEvent *evt);
    int handleScsiEvent(NetlinkEvent *evt);
	int handleUsb();
	int get_tty_id( char* tty_path, int *vid, int* pid);
	int atox( const char * line, int f_base );
    int handleUsbG3dev();
    int get_usb_id(char* usb_path, int *vid, int* pid);
};

#endif

