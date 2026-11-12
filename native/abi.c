#include <aoahid.h>
#include <stddef.h>
void cg_context_init(aoahid_context_options *p) { aoahid_context_options_init(p); }
size_t cg_context_size(void) { return sizeof(aoahid_context_options); }
size_t cg_context_align(void) { return _Alignof(aoahid_context_options); }
void cg_device_init(aoahid_device_options *p) { aoahid_device_options_init(p); }
size_t cg_device_size(void) { return sizeof(aoahid_device_options); }
size_t cg_device_align(void) { return _Alignof(aoahid_device_options); }
void cg_node_init(aoahid_node_options *p) { aoahid_node_options_init(p); }
size_t cg_node_size(void) { return sizeof(aoahid_node_options); }
size_t cg_node_align(void) { return _Alignof(aoahid_node_options); }
void cg_keyboard_init(aoahid_keyboard_options *p) { aoahid_keyboard_options_init(p); }
size_t cg_keyboard_size(void) { return sizeof(aoahid_keyboard_options); }
size_t cg_keyboard_align(void) { return _Alignof(aoahid_keyboard_options); }
void cg_touchscreen_init(aoahid_touchscreen_options *p) { aoahid_touchscreen_options_init(p); }
size_t cg_touchscreen_size(void) { return sizeof(aoahid_touchscreen_options); }
size_t cg_touchscreen_align(void) { return _Alignof(aoahid_touchscreen_options); }
size_t cg_device_info_size(void) { return sizeof(aoahid_device_info); }
size_t cg_device_info_align(void) { return _Alignof(aoahid_device_info); }

size_t cg_error_detail_size(void) { return sizeof(aoahid_error_detail); }
size_t cg_error_detail_align(void) { return _Alignof(aoahid_error_detail); }
