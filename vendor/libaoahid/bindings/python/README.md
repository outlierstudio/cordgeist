# aoahid Python binding

This wheel contains only the platform-independent `ctypes` declarations for
the libaoahid C ABI. It deliberately contains no `.so`, `.dll`, `.dylib`, or
libusb runtime. Install the matching libaoahid shared distribution separately,
then pass its exact path to `aoahid.load(path)`. The binding never guesses a
native-library path. The C header's `*_init` functions are inline and not part
of the binary ABI, so a `ctypes` structure starts all zero: set its
`struct_size` and every required field yourself. `docs/API.md` lists the values
the C initializers use; a required field left zero is refused with
`AOAHID_ERR_UNSET_FIELD`.

The Python package version and the native libaoahid version must match. Complete
native archives, dependency notices, and SPDX documents are published with the
corresponding GitHub Release.
