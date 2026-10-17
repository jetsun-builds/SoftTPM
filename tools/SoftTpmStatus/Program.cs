using System;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;

namespace SoftTpmStatus
{
    internal static class TbsNative
    {
        [StructLayout(LayoutKind.Sequential)]
        public struct TBS_CONTEXT_PARAMS2
        {
            public UInt32 version;
            public UInt32 flags;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct TPM_DEVICE_INFO
        {
            public UInt32 structVersion;
            public UInt32 tpmVersion;
            public UInt32 tpmInterfaceType;
            public UInt32 tpmImpRevision;
        }

        public const UInt32 TBS_COMMAND_LOCALITY_ZERO = 0;
        public const UInt32 TBS_COMMAND_PRIORITY_NORMAL = 200;

        [DllImport("tbs.dll")]
        public static extern UInt32 Tbsi_Context_Create(
            ref TBS_CONTEXT_PARAMS2 contextParams,
            out IntPtr context);

        [DllImport("tbs.dll")]
        public static extern UInt32 Tbsi_GetDeviceInfo(
            IntPtr context,
            out TPM_DEVICE_INFO deviceInfo);

        [DllImport("tbs.dll")]
        public static extern UInt32 Tbsip_Context_Close(IntPtr context);

        [DllImport("tbs.dll")]
        public static extern UInt32 Tbsip_Submit_Command(
            IntPtr context,
            UInt32 locality,
            UInt32 priority,
            byte[] command,
            UInt32 commandSize,
            byte[] response,
            ref UInt32 responseSize);
    }

    internal static class TpmCommand
    {
        public const UInt32 TPM_CC_GET_CAPABILITY = 0x0000017A;
        public const UInt32 TPM_CC_READ_PUBLIC = 0x00000173;

        public const UInt32 TPM_CAP_TPM_PROPERTIES = 0x00000006;

        public const UInt32 TPM_PT_FAMILY_INDICATOR = 0x00000100;
        public const UInt32 TPM_PT_LEVEL = 0x00000101;
        public const UInt32 TPM_PT_REVISION = 0x00000102;
        public const UInt32 TPM_PT_MANUFACTURER = 0x00000105;
        public const UInt32 TPM_PT_VENDOR_STRING_1 = 0x00000106;
        public const UInt32 TPM_PT_VENDOR_STRING_2 = 0x00000107;
        public const UInt32 TPM_PT_VENDOR_STRING_3 = 0x00000108;
        public const UInt32 TPM_PT_VENDOR_STRING_4 = 0x00000109;
        public const UInt32 TPM_PT_FIRMWARE_VERSION_1 = 0x0000010E;
        public const UInt32 TPM_PT_FIRMWARE_VERSION_2 = 0x0000010F;

        public const UInt32 EK_PERSISTENT_HANDLE = 0x81010001;
        public const UInt32 SRK_PERSISTENT_HANDLE = 0x81000001;

        public static void WriteU16BE(byte[] buffer, int offset, UInt16 value)
        {
            buffer[offset] = (byte)(value >> 8);
            buffer[offset + 1] = (byte)value;
        }

        public static void WriteU32BE(byte[] buffer, int offset, UInt32 value)
        {
            buffer[offset] = (byte)(value >> 24);
            buffer[offset + 1] = (byte)(value >> 16);
            buffer[offset + 2] = (byte)(value >> 8);
            buffer[offset + 3] = (byte)value;
        }

        public static UInt16 ReadU16BE(byte[] buffer, int offset)
        {
            return (UInt16)((buffer[offset] << 8) | buffer[offset + 1]);
        }

        public static UInt32 ReadU32BE(byte[] buffer, int offset)
        {
            return ((UInt32)buffer[offset] << 24)
                | ((UInt32)buffer[offset + 1] << 16)
                | ((UInt32)buffer[offset + 2] << 8)
                | buffer[offset + 3];
        }

        public static byte[] GetCapability(UInt32 property)
        {
            byte[] command = new byte[22];

            WriteU16BE(command, 0, 0x8001);
            WriteU32BE(command, 2, (UInt32)command.Length);
            WriteU32BE(command, 6, TPM_CC_GET_CAPABILITY);
            WriteU32BE(command, 10, TPM_CAP_TPM_PROPERTIES);
            WriteU32BE(command, 14, property);
            WriteU32BE(command, 18, 1);

            return command;
        }

        public static byte[] ReadPublic(UInt32 handle)
        {
            byte[] command = new byte[14];

            WriteU16BE(command, 0, 0x8001);
            WriteU32BE(command, 2, (UInt32)command.Length);
            WriteU32BE(command, 6, TPM_CC_READ_PUBLIC);
            WriteU32BE(command, 10, handle);

            return command;
        }
    }

    internal static class Program
    {
        private const int ExitSuccess = 0;
        private const int ExitFailure = 1;

        private static bool s_failure;

        private static void Fail(string message)
        {
            Console.WriteLine("FAIL  " + message);
            s_failure = true;
        }

        private static byte[] Submit(IntPtr context, byte[] command)
        {
            byte[] response = new byte[4096];
            UInt32 responseSize = (UInt32)response.Length;

            UInt32 result = TbsNative.Tbsip_Submit_Command(
                context,
                TbsNative.TBS_COMMAND_LOCALITY_ZERO,
                TbsNative.TBS_COMMAND_PRIORITY_NORMAL,
                command,
                (UInt32)command.Length,
                response,
                ref responseSize);

            if (result != 0)
            {
                throw new InvalidOperationException(
                    string.Format(CultureInfo.InvariantCulture, "TBS error 0x{0:X8}", result));
            }

            Array.Resize(ref response, (int)responseSize);
            return response;
        }

        private static UInt32 ResponseCode(byte[] response)
        {
            if (response.Length < 10)
            {
                return 0xFFFFFFFF;
            }

            return TpmCommand.ReadU32BE(response, 6);
        }

        private static UInt32 GetCapabilityProperty(IntPtr context, UInt32 property)
        {
            byte[] response = Submit(context, TpmCommand.GetCapability(property));
            UInt32 responseCode = ResponseCode(response);

            if (responseCode != 0)
            {
                throw new InvalidOperationException(
                    string.Format(CultureInfo.InvariantCulture,
                        "GetCapability(0x{0:X3}) returned TPM_RC 0x{1:X8}",
                        property, responseCode));
            }

            if (response.Length < 27)
            {
                throw new InvalidOperationException("GetCapability response is truncated");
            }

            return TpmCommand.ReadU32BE(response, 23);
        }

        private static string PropertyToChars(UInt32 value)
        {
            StringBuilder builder = new StringBuilder(4);

            builder.Append((char)((value >> 24) & 0xFF));
            builder.Append((char)((value >> 16) & 0xFF));
            builder.Append((char)((value >> 8) & 0xFF));
            builder.Append((char)(value & 0xFF));

            return builder.ToString();
        }

        private static void PrintProperty(IntPtr context, string name, UInt32 property, bool asChars)
        {
            try
            {
                UInt32 value = GetCapabilityProperty(context, property);

                if (asChars)
                {
                    Console.WriteLine("OK    {0,-20} 0x{1:X8} \"{2}\"",
                        name, value, PropertyToChars(value));
                }
                else
                {
                    Console.WriteLine("OK    {0,-20} 0x{1:X8}", name, value);
                }
            }
            catch (InvalidOperationException exception)
            {
                Fail(name + ": " + exception.Message);
            }
        }

        private static void PrintPublic(IntPtr context, string name, UInt32 handle)
        {
            try
            {
                byte[] response = Submit(context, TpmCommand.ReadPublic(handle));
                UInt32 responseCode = ResponseCode(response);

                if (responseCode != 0)
                {
                    Fail(string.Format(CultureInfo.InvariantCulture,
                        "{0} (0x{1:X8}) ReadPublic returned TPM_RC 0x{2:X8}",
                        name, handle, responseCode));
                    return;
                }

                if (response.Length < 16)
                {
                    Fail(name + ": ReadPublic response is truncated");
                    return;
                }

                int publicSize = TpmCommand.ReadU16BE(response, 10);
                int areaOffset = 12;

                if (areaOffset + publicSize > response.Length || publicSize < 10)
                {
                    Fail(name + ": ReadPublic public area is malformed");
                    return;
                }

                UInt32 type = TpmCommand.ReadU16BE(response, areaOffset);
                UInt32 nameAlg = TpmCommand.ReadU16BE(response, areaOffset + 2);
                UInt32 attributes = TpmCommand.ReadU32BE(response, areaOffset + 4);

                Console.WriteLine(
                    "OK    {0,-20} handle 0x{1:X8} type 0x{2:X4} nameAlg 0x{3:X4} attributes 0x{4:X8} public {5} bytes",
                    name, handle, type, nameAlg, attributes, publicSize);
            }
            catch (InvalidOperationException exception)
            {
                Fail(name + ": " + exception.Message);
            }
        }

        private static int Main(string[] args)
        {
            Console.WriteLine("SoftTPM status tool");
            Console.WriteLine();

            TbsNative.TBS_CONTEXT_PARAMS2 contextParams = new TbsNative.TBS_CONTEXT_PARAMS2();
            contextParams.version = 2;
            contextParams.flags = 0x4;

            IntPtr context;

            UInt32 result = TbsNative.Tbsi_Context_Create(ref contextParams, out context);

            if (result != 0)
            {
                Fail(string.Format(CultureInfo.InvariantCulture,
                    "Tbsi_Context_Create failed with 0x{0:X8}", result));
                return ExitFailure;
            }

            try
            {
                TbsNative.TPM_DEVICE_INFO deviceInfo;

                result = TbsNative.Tbsi_GetDeviceInfo(context, out deviceInfo);

                if (result != 0)
                {
                    Fail(string.Format(CultureInfo.InvariantCulture,
                        "Tbsi_GetDeviceInfo failed with 0x{0:X8}", result));
                }
                else
                {
                    Console.WriteLine(
                        "OK    TBS device info      structVersion {0}, tpmVersion {1}, tpmInterfaceType {2}, tpmImpRevision {3}",
                        deviceInfo.structVersion,
                        deviceInfo.tpmVersion,
                        deviceInfo.tpmInterfaceType,
                        deviceInfo.tpmImpRevision);

                    if (deviceInfo.tpmVersion != 2)
                    {
                        Fail("TBS does not report TPM 2.0");
                    }
                }

                PrintProperty(context, "familyIndicator", TpmCommand.TPM_PT_FAMILY_INDICATOR, true);
                PrintProperty(context, "level", TpmCommand.TPM_PT_LEVEL, false);
                PrintProperty(context, "revision", TpmCommand.TPM_PT_REVISION, false);
                PrintProperty(context, "manufacturer", TpmCommand.TPM_PT_MANUFACTURER, true);
                PrintProperty(context, "vendorString1", TpmCommand.TPM_PT_VENDOR_STRING_1, true);
                PrintProperty(context, "vendorString2", TpmCommand.TPM_PT_VENDOR_STRING_2, true);
                PrintProperty(context, "vendorString3", TpmCommand.TPM_PT_VENDOR_STRING_3, true);
                PrintProperty(context, "vendorString4", TpmCommand.TPM_PT_VENDOR_STRING_4, true);
                PrintProperty(context, "firmwareVersion1", TpmCommand.TPM_PT_FIRMWARE_VERSION_1, false);
                PrintProperty(context, "firmwareVersion2", TpmCommand.TPM_PT_FIRMWARE_VERSION_2, false);

                PrintPublic(context, "EK", TpmCommand.EK_PERSISTENT_HANDLE);
                PrintPublic(context, "SRK", TpmCommand.SRK_PERSISTENT_HANDLE);
            }
            finally
            {
                TbsNative.Tbsip_Context_Close(context);
            }

            Console.WriteLine();

            if (s_failure)
            {
                Console.WriteLine("SOFTTPM SMOKE TEST FAILED");
                return ExitFailure;
            }

            Console.WriteLine("SOFTTPM SMOKE TEST PASSED");
            return ExitSuccess;
        }
    }
}
