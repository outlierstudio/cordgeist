# AoaHid .NET binding

This NuGet package contains only the managed P/Invoke declarations for the
libaoahid C ABI. It does not contain `aoahid.dll`, `libaoahid.so`, libusb, or
other native runtime assets. Install the matching libaoahid shared distribution
separately and make it available to the .NET native-library loader under the
name `aoahid`.

The NuGet package version and native libaoahid version must match. Complete
native archives, dependency notices, and SPDX documents are published with the
corresponding GitHub Release. The C header's `*_init` functions are inline and
not part of the binary ABI, so a structure starts all zero: set its
`StructSize` and every required field yourself. `docs/API.md` lists the values
the C initializers use; a required field left zero is refused with
`Result.UnsetField`.

## Callback lifetime

`ContextOptions.LogSink` holds a native function pointer, typically obtained
with `Marshal.GetFunctionPointerForDelegate(sink)`. The garbage collector does
not know that native code still uses it: keep the `LogSink` delegate instance
referenced, for example in a field, until `ContextDestroy` has returned, or a
later log call may run freed code and crash the process.
