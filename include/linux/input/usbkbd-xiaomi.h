/*
 * Copyright (c) 2024 Sebastiano Barezzi <seba@sebaubuntu.dev>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/types.h>
#include <linux/errno.h>

#ifdef CONFIG_XIAOMI_KEYBOARD
void xiaomi_keyboard_connection_change(bool connected);
int xiaomi_keyboard_connection_query(bool *connected);
void xiaomi_keyboard_request_query(void);
#else
static inline void xiaomi_keyboard_connection_change(bool connected) { }
static inline int xiaomi_keyboard_connection_query(bool *connected) { return -ENODEV; }
static inline void xiaomi_keyboard_request_query(void) { }
#endif
