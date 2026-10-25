#include "usbdriver.h"

#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>

#include <cwctype>
#include <vector>

#pragma comment(lib, "setupapi")
#pragma comment(lib, "newdev")
#pragma comment(lib, "cfgmgr32")

namespace usbdriver {

namespace {

// libusb finds WinUSB devices through this interface GUID (unique to this project)
const wchar_t* kInterfaceGuid = L"{25CFC11A-7AA7-49DC-B6C3-40375C134616}";
// "Universal Serial Bus devices" class, the class of the inbox winusb.inf
const wchar_t* kUsbDeviceClass = L"{88BAE032-5A81-49F0-BC3D-A4FF138216D6}";

struct Node
{
	std::wstring instanceId;
	std::wstring service;
	bool mi00 = false;     // video interface of the composite device
	bool otherMi = false;  // audio interfaces: never touched
};

std::wstring Upper(std::wstring s)
{
	for (auto& c : s)
		c = (wchar_t)towupper(c);
	return s;
}

std::wstring LastErrorText(const wchar_t* what)
{
	DWORD code = GetLastError();
	wchar_t buf[400] = L"";
	FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, 400, nullptr);
	std::wstring s = std::wstring(what) + L": " + buf;
	while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' '))
		s.pop_back();
	wchar_t hex[24];
	swprintf_s(hex, L" (0x%08lX)", code);
	return s + hex;
}

std::vector<Node> FindNodes()
{
	std::vector<Node> nodes;
	HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
	if (set == INVALID_HANDLE_VALUE)
		return nodes;

	SP_DEVINFO_DATA d{ sizeof(d) };
	for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); i++)
	{
		wchar_t ids[4096] = {};
		if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, nullptr, (BYTE*)ids, sizeof(ids) - 4 * sizeof(wchar_t), nullptr))
			continue;
		std::wstring first = Upper(ids);
		if (first.find(L"VID_1415&PID_2000") == std::wstring::npos)
			continue;

		Node n;
		wchar_t id[MAX_DEVICE_ID_LEN] = L"";
		if (!SetupDiGetDeviceInstanceIdW(set, &d, id, MAX_DEVICE_ID_LEN, nullptr))
			continue;
		n.instanceId = id;
		wchar_t svc[256] = L"";
		if (SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_SERVICE, nullptr, (BYTE*)svc, sizeof(svc) - sizeof(wchar_t), nullptr))
			n.service = svc;
		n.mi00 = first.find(L"&MI_00") != std::wstring::npos;
		n.otherMi = !n.mi00 && first.find(L"&MI_") != std::wstring::npos;
		nodes.push_back(n);
	}
	SetupDiDestroyDeviceInfoList(set);
	return nodes;
}

// The node libusb talks to: the video interface (MI_00) of the composite device, or the whole
// device when a driver is already bound to it directly (e.g. installed by Zadig on the parent).
bool FindTarget(Node& out)
{
	auto nodes = FindNodes();
	for (auto& n : nodes)
	{
		if (n.mi00)
		{
			out = n;
			return true;
		}
	}
	for (auto& n : nodes)
	{
		if (!n.otherMi && _wcsicmp(n.service.c_str(), L"usbccgp") != 0)
		{
			out = n;
			return true;
		}
	}
	return false;
}

bool IsUserModeUsbDriver(const std::wstring& svc)
{
	return !_wcsicmp(svc.c_str(), L"WinUSB") || !_wcsicmp(svc.c_str(), L"libusbK") || !_wcsicmp(svc.c_str(), L"libusb0");
}

// Single-device info set
struct DevSet
{
	HDEVINFO set = INVALID_HANDLE_VALUE;
	SP_DEVINFO_DATA d{ sizeof(SP_DEVINFO_DATA) };

	bool Open(const std::wstring& id)
	{
		Close();
		set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
		if (set == INVALID_HANDLE_VALUE)
			return false;
		d = SP_DEVINFO_DATA{ sizeof(SP_DEVINFO_DATA) };
		return SetupDiOpenDeviceInfoW(set, id.c_str(), nullptr, 0, &d) != FALSE;
	}
	void Close()
	{
		if (set != INVALID_HANDLE_VALUE)
			SetupDiDestroyDeviceInfoList(set);
		set = INVALID_HANDLE_VALUE;
	}
	~DevSet() { Close(); }
};

bool HasInterfaceGuid(DevSet& ds)
{
	HKEY k = SetupDiOpenDevRegKey(ds.set, &ds.d, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
	if (k == INVALID_HANDLE_VALUE)
		return false;
	DWORD type = 0, size = 0;
	bool has = RegQueryValueExW(k, L"DeviceInterfaceGUIDs", nullptr, &type, nullptr, &size) == ERROR_SUCCESS ||
		RegQueryValueExW(k, L"DeviceInterfaceGUID", nullptr, &type, nullptr, &size) == ERROR_SUCCESS;
	RegCloseKey(k);
	return has;
}

bool SetInterfaceGuid(DevSet& ds)
{
	HKEY k = SetupDiCreateDevRegKeyW(ds.set, &ds.d, DICS_FLAG_GLOBAL, 0, DIREG_DEV, nullptr, nullptr);
	if (k == INVALID_HANDLE_VALUE)
		return false;
	std::wstring multi = kInterfaceGuid;
	multi.push_back(L'\0'); // REG_MULTI_SZ: "guid\0\0" (c_str adds the last one)
	LSTATUS st = RegSetValueExW(k, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ, (const BYTE*)multi.c_str(), (DWORD)((multi.size() + 1) * sizeof(wchar_t)));
	RegCloseKey(k);
	return st == ERROR_SUCCESS;
}

bool Restart(DevSet& ds)
{
	SP_PROPCHANGE_PARAMS pc{};
	pc.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
	pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
	pc.StateChange = DICS_PROPCHANGE;
	pc.Scope = DICS_FLAG_GLOBAL;
	return SetupDiSetClassInstallParamsW(ds.set, &ds.d, &pc.ClassInstallHeader, sizeof(pc)) &&
		SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, ds.set, &ds.d);
}

void Rescan()
{
	DEVINST root = 0;
	if (CM_Locate_DevNodeW(&root, nullptr, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
		CM_Reenumerate_DevNode(root, CM_REENUMERATE_SYNCHRONOUS);
}

} // namespace

std::wstring CurrentService()
{
	Node n;
	return FindTarget(n) ? n.service : L"";
}

Result Install(std::wstring& error)
{
	Node target;
	if (!FindTarget(target))
		return FindNodes().empty() ? Result::NotPlugged : (error = L"Could not find the camera's video interface.", Result::Failed);

	DevSet ds;
	if (!ds.Open(target.instanceId))
	{
		error = LastErrorText(L"Opening the camera device failed");
		return Result::Failed;
	}

	if (IsUserModeUsbDriver(target.service))
	{
		// already fine (installed earlier, or by Zadig). Make sure libusb can find a WinUSB device.
		if (!_wcsicmp(target.service.c_str(), L"WinUSB") && !HasInterfaceGuid(ds))
		{
			SetInterfaceGuid(ds);
			Restart(ds);
		}
		return Result::AlreadyInstalled;
	}

	// 1. interface GUID that WinUSB will publish when it starts
	if (!SetInterfaceGuid(ds))
	{
		error = LastErrorText(L"Writing the device settings failed");
		return Result::Failed;
	}

	// 2. put the device in the USB devices class, where winusb.inf lives
	std::wstring cls = kUsbDeviceClass;
	SetupDiSetDeviceRegistryPropertyW(ds.set, &ds.d, SPDRP_CLASSGUID, (const BYTE*)cls.c_str(), (DWORD)((cls.size() + 1) * sizeof(wchar_t)));
	if (!ds.Open(target.instanceId)) // reopen so the new class is picked up
	{
		error = LastErrorText(L"Reopening the camera device failed");
		return Result::Failed;
	}

	// 3. list the drivers in Windows' own winusb.inf, regardless of hardware ID
	SP_DEVINSTALL_PARAMS_W p{};
	p.cbSize = sizeof(p);
	if (!SetupDiGetDeviceInstallParamsW(ds.set, &ds.d, &p))
	{
		error = LastErrorText(L"Reading install parameters failed");
		return Result::Failed;
	}
	wchar_t windir[MAX_PATH] = L"C:\\Windows";
	GetWindowsDirectoryW(windir, MAX_PATH);
	swprintf_s(p.DriverPath, L"%s\\INF\\winusb.inf", windir);
	p.Flags |= DI_ENUMSINGLEINF;
	p.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
	if (!SetupDiSetDeviceInstallParamsW(ds.set, &ds.d, &p) || !SetupDiBuildDriverInfoList(ds.set, &ds.d, SPDIT_CLASSDRIVER))
	{
		error = LastErrorText(L"Loading the built-in WinUSB driver failed");
		return Result::Failed;
	}

	SP_DRVINFO_DATA_W drv{};
	drv.cbSize = sizeof(drv);
	bool found = false;
	for (DWORD i = 0; SetupDiEnumDriverInfoW(ds.set, &ds.d, SPDIT_CLASSDRIVER, i, &drv); i++)
	{
		found = true;
		if (wcsstr(Upper(drv.Description).c_str(), L"WINUSB"))
			break; // prefer the "WinUsb Device" entry
	}
	if (!found)
	{
		error = L"Windows' built-in WinUSB driver (winusb.inf) was not found.";
		return Result::Failed;
	}
	SetupDiSetSelectedDriverW(ds.set, &ds.d, &drv);

	// 4. install it on the camera
	BOOL reboot = FALSE;
	if (!DiInstallDevice(nullptr, ds.set, &ds.d, &drv, 0, &reboot))
	{
		error = LastErrorText(L"Installing WinUSB on the camera failed");
		return Result::Failed;
	}

	// 5. make sure the GUID survived and WinUSB picked it up
	if (!HasInterfaceGuid(ds))
		SetInterfaceGuid(ds);
	Restart(ds);
	return Result::Installed;
}

bool Remove(std::wstring& error)
{
	Node target;
	if (!FindTarget(target) || !IsUserModeUsbDriver(target.service))
		return true; // nothing of ours bound

	DevSet ds;
	if (!ds.Open(target.instanceId))
	{
		error = LastErrorText(L"Opening the camera device failed");
		return false;
	}

	// remember a Zadig/libwdi package (oemNN.inf) so it doesn't come straight back after the rescan
	wchar_t inf[MAX_PATH] = L"";
	HKEY drvKey = SetupDiOpenDevRegKey(ds.set, &ds.d, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
	if (drvKey != INVALID_HANDLE_VALUE)
	{
		DWORD size = sizeof(inf) - sizeof(wchar_t), type = 0;
		RegQueryValueExW(drvKey, L"InfPath", nullptr, &type, (BYTE*)inf, &size);
		RegCloseKey(drvKey);
	}

	BOOL reboot = FALSE;
	if (!DiUninstallDevice(nullptr, ds.set, &ds.d, 0, &reboot))
	{
		error = LastErrorText(L"Removing the USB driver failed");
		return false;
	}
	ds.Close();

	if (!_wcsnicmp(inf, L"oem", 3))
		SetupUninstallOEMInfW(inf, SUOI_FORCEDELETE, nullptr);

	Rescan();
	return true;
}

} // namespace usbdriver
