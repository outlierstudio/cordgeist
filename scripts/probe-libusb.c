#include <stdio.h>
#include <libusb.h>
#if LIBUSB_API_VERSION < 0x0100010C
#error "libusb headers older than 1.0.30"
#endif
int main(void) {
    const struct libusb_version *v = libusb_get_version();
    printf("libusb runtime %u.%u.%u, header API 0x%x\n", v->major, v->minor, v->micro, LIBUSB_API_VERSION);
    return !(v->major > 1 || (v->major == 1 && (v->minor > 0 || v->micro >= 30)));
}
