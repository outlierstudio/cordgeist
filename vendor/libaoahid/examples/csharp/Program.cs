// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
// Demonstrates explicit managed discovery against a separately deployed native
// library without selecting device or profile policy.
using System.Runtime.InteropServices;
using AoaHid;

if (args.Length != 1 || !uint.TryParse(args[0], out var timeoutMs) || timeoutMs == 0)
{
    Console.Error.WriteLine("usage: Enumerate CONTROL_TIMEOUT_MS");
    return 2;
}

var options = new ContextOptions
{
    StructSize = checked((uint)Marshal.SizeOf<ContextOptions>()),
    Reserved = 0,
    EventMode = EventMode.CallerPoll,
    LogLevel = LogLevel.Disabled,
    LogSink = nint.Zero,
    LogUser = nint.Zero,
};
var status = Native.ContextCreate(in options, out var context);
if (status != Result.Ok)
    return (int)status;
status = Native.Discover(context, timeoutMs, out var discovery);
if (status == Result.Ok)
{
    var count = Native.DiscoveryCount(discovery);
    if (count == 0)
    {
        var detailPointer = Native.LastError();
        if (detailPointer == nint.Zero)
            status = Result.Internal;
        else
        {
            var countStatus = Marshal.PtrToStructure<ErrorDetail>(detailPointer).Code;
            if (countStatus != Result.Ok)
                status = countStatus;
        }
    }
    for (nuint index = 0; status == Result.Ok && index < count; ++index)
    {
        var pointer = Native.DiscoveryGet(discovery, index);
        if (pointer == nint.Zero)
        {
            var detailPointer = Native.LastError();
            status = detailPointer == nint.Zero
                ? Result.Internal
                : Marshal.PtrToStructure<ErrorDetail>(detailPointer).Code;
            if (status == Result.Ok)
                status = Result.Internal;
            break;
        }
        var device = Marshal.PtrToStructure<DeviceInfo>(pointer);
        Console.WriteLine(
            $"{device.VendorId:x4}:{device.ProductId:x4} aoa={device.ProtocolVersion} " +
            $"{Marshal.PtrToStringUTF8(device.Product)} {Marshal.PtrToStringUTF8(device.Serial)}");
    }
    Native.DiscoveryDestroy(discovery);
}
var closeStatus = Native.ContextDestroyBlocking(context, timeoutMs, timeoutMs, timeoutMs);
return status != Result.Ok ? (int)status : (int)closeStatus;
