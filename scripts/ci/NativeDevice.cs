using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace SoftTpmPackage {
    public static class NativeDevice {
        static Guid SecurityClass = new Guid("d94ee5d8-d189-4994-83d2-f68d7d41b0e6");
        const string HardwareId = @"Root\SoftTpm";
        [StructLayout(LayoutKind.Sequential)]
        struct DeviceInfo { public uint cbSize; public Guid ClassGuid; public uint DevInst; public IntPtr Reserved; }
        [StructLayout(LayoutKind.Sequential)]
        struct CodeIntegrityInfo { public uint Length; public uint Options; }
        [DllImport("ntdll.dll")] static extern int NtQuerySystemInformation(int infoClass, ref CodeIntegrityInfo info, int length, out int returned);
        [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr SetupDiGetClassDevsW(ref Guid cls, string enumerator, IntPtr parent, uint flags);
        [DllImport("setupapi.dll", SetLastError=true)] static extern bool SetupDiEnumDeviceInfo(IntPtr set, uint index, ref DeviceInfo info);
        [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool SetupDiGetDeviceRegistryPropertyW(IntPtr set, ref DeviceInfo info, uint prop, out uint type, byte[] buffer, uint size, out uint needed);
        [DllImport("setupapi.dll", SetLastError=true)] static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid cls, IntPtr parent);
        [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool SetupDiCreateDeviceInfoW(IntPtr set, string name, ref Guid cls, string description, IntPtr parent, uint flags, ref DeviceInfo info);
        [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set, ref DeviceInfo info, uint prop, byte[] buffer, uint size);
        [DllImport("setupapi.dll", SetLastError=true)] static extern bool SetupDiCallClassInstaller(uint function, IntPtr set, ref DeviceInfo info);
        [DllImport("setupapi.dll")] static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
        [DllImport("newdev.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr parent, string hardwareId, string inf, uint flags, out bool reboot);

        static DeviceInfo NewInfo() { return new DeviceInfo { cbSize = (uint)Marshal.SizeOf(typeof(DeviceInfo)) }; }
        static void Check(bool ok) { if (!ok) throw new Win32Exception(Marshal.GetLastWin32Error()); }
        static bool IsOurs(IntPtr set, ref DeviceInfo info) {
            uint type, needed;
            byte[] buffer = new byte[8192];
            if (!SetupDiGetDeviceRegistryPropertyW(set, ref info, 1, out type, buffer, (uint)buffer.Length, out needed)) return false;
            foreach (string id in Encoding.Unicode.GetString(buffer, 0, (int)Math.Min(needed, (uint)buffer.Length)).Split('\0'))
                if (String.Equals(id, HardwareId, StringComparison.OrdinalIgnoreCase)) return true;
            return false;
        }
        static IntPtr OpenDevices() {
            IntPtr set = SetupDiGetClassDevsW(ref SecurityClass, null, IntPtr.Zero, 0);
            if (set == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
            return set;
        }
        public static bool TestSigningEnabled() {
            CodeIntegrityInfo info = new CodeIntegrityInfo { Length = 8 };
            int returned;
            int status = NtQuerySystemInformation(103, ref info, 8, out returned);
            if (status != 0) throw new InvalidOperationException("Code integrity query failed: 0x" + status.ToString("X8"));
            return (info.Options & 2) != 0;
        }
        public static bool Exists() {
            IntPtr set = OpenDevices();
            try {
                for (uint i = 0; ; i++) {
                    DeviceInfo info = NewInfo();
                    if (!SetupDiEnumDeviceInfo(set, i, ref info)) {
                        int error = Marshal.GetLastWin32Error();
                        if (error == 259) return false;
                        throw new Win32Exception(error);
                    }
                    if (IsOurs(set, ref info)) return true;
                }
            } finally { SetupDiDestroyDeviceInfoList(set); }
        }
        public static bool Install(string inf) {
            IntPtr createdSet = IntPtr.Zero;
            DeviceInfo createdInfo = NewInfo();
            bool created = false;
            try {
                if (!Exists()) {
                    createdSet = SetupDiCreateDeviceInfoList(ref SecurityClass, IntPtr.Zero);
                    if (createdSet == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
                    Check(SetupDiCreateDeviceInfoW(createdSet, "SoftTpm", ref SecurityClass, "SoftTPM Software TPM 2.0", IntPtr.Zero, 1, ref createdInfo));
                    byte[] ids = Encoding.Unicode.GetBytes(HardwareId + "\0\0");
                    Check(SetupDiSetDeviceRegistryPropertyW(createdSet, ref createdInfo, 1, ids, (uint)ids.Length));
                    Check(SetupDiCallClassInstaller(0x19, createdSet, ref createdInfo));
                    created = true;
                }
                bool reboot;
                Check(UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, HardwareId, inf, 1, out reboot));
                return reboot;
            } catch {
                if (created) SetupDiCallClassInstaller(5, createdSet, ref createdInfo);
                throw;
            } finally {
                if (createdSet != IntPtr.Zero && createdSet != new IntPtr(-1)) SetupDiDestroyDeviceInfoList(createdSet);
            }
        }
        public static int RemoveAll() {
            IntPtr set = OpenDevices();
            int removed = 0;
            try {
                for (uint i = 0; ; i++) {
                    DeviceInfo info = NewInfo();
                    if (!SetupDiEnumDeviceInfo(set, i, ref info)) {
                        int error = Marshal.GetLastWin32Error();
                        if (error == 259) break;
                        throw new Win32Exception(error);
                    }
                    if (IsOurs(set, ref info)) { Check(SetupDiCallClassInstaller(5, set, ref info)); removed++; }
                }
                return removed;
            } finally { SetupDiDestroyDeviceInfoList(set); }
        }
    }
}
