using System;
using System.Runtime.InteropServices;

namespace SuperMenu.Media;

// Windows' Focus / Do Not Disturb is exposed through the QuietHoursSettings
// local COM service. This is the same state the taskbar's bell / Calendar
// panel uses; the interface definition follows the public QuietHours IDL.
internal static class QuietHours
{
    private const string OffProfile = "Microsoft.QuietHoursProfile.Unrestricted";
    private const string PriorityProfile = "Microsoft.QuietHoursProfile.PriorityOnly";

    [ComImport, Guid("6bff4732-81ec-4ffb-ae67-b6c1bc29631f"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IQuietHoursSettings
    {
        [PreserveSig] int get_UserSelectedProfile(out IntPtr value);
        [PreserveSig] int set_UserSelectedProfile(IntPtr value);
        [PreserveSig] int GetProfile(IntPtr profileId, out IntPtr profile);
        [PreserveSig] int GetAllProfileData(out uint count, IntPtr data);
        [PreserveSig] int GetDisplayNameForProfile(IntPtr profileId, out IntPtr displayName);
        [PreserveSig] int get_QuietMomentsManager(out IntPtr value);
        [PreserveSig] int get_OffProfileId(out IntPtr value);
        [PreserveSig] int get_ActiveQuietMomentProfile(out IntPtr value);
        [PreserveSig] int set_ActiveQuietMomentProfile(IntPtr value);
        [PreserveSig] int get_ActiveProfile(out IntPtr value);
        [PreserveSig] int get_QuietHoursPinnedContactManager(out IntPtr value);
        [PreserveSig] int set_QuietMomentsShowSummaryEnabled(IntPtr isEnabled);
        [PreserveSig] int GetAlwaysAllowedApps(out uint count, out IntPtr allowed);
        [PreserveSig] int StartProcessing();
        [PreserveSig] int StopProcessing();
    }

    [DllImport("ole32.dll")]
    private static extern int CoCreateInstance(ref Guid clsid, IntPtr outer, uint clsctx, ref Guid iid,
        out IntPtr instance);

    private static IQuietHoursSettings? Create()
    {
        var clsid = new Guid("f53321fa-34f8-4b7f-b9a3-361877cb94cf");
        var iid = new Guid("6bff4732-81ec-4ffb-ae67-b6c1bc29631f");
        if (CoCreateInstance(ref clsid, IntPtr.Zero, 4, ref iid, out var ptr) != 0 || ptr == IntPtr.Zero) return null;
        try
        {
            return (IQuietHoursSettings)Marshal.GetTypedObjectForIUnknown(ptr, typeof(IQuietHoursSettings));
        }
        finally
        {
            Marshal.Release(ptr);
        }
    }

    private static string ReadProfile(IQuietHoursSettings settings)
    {
        var hr = settings.get_ActiveProfile(out var value);
        if (hr != 0 || value == IntPtr.Zero) return "";
        try
        {
            return Marshal.PtrToStringUni(value) ?? "";
        }
        finally
        {
            Marshal.FreeCoTaskMem(value);
        }
    }

    public static bool TryGetEnabled(out bool enabled)
    {
        enabled = false;
        try
        {
            var settings = Create();
            if (settings is null) return false;
            var profile = ReadProfile(settings);
            if (profile.Length == 0) return false;
            enabled = !profile.Equals(OffProfile, StringComparison.OrdinalIgnoreCase);
            return true;
        }
        catch
        {
            return false;
        }
    }

    public static bool SetEnabled(bool enabled)
    {
        try
        {
            var settings = Create();
            if (settings is null) return false;
            var profile = Marshal.StringToCoTaskMemUni(enabled ? PriorityProfile : OffProfile);
            try
            {
                return settings.set_UserSelectedProfile(profile) == 0;
            }
            finally
            {
                Marshal.FreeCoTaskMem(profile);
            }
        }
        catch
        {
            return false;
        }
    }
}
