#define WIN32_LEAN_AND_MEAN
#define STRICT
#define _ATL_NO_AUTOMATIC_NAMESPACE
#define _CRTDBG_MAP_ALLOC
#include <atlbase.h>
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <clocale>
#include <cstdio>
#include <memory>
#include <crtdbg.h>
#pragma comment(lib, "user32")

std::unique_ptr<WCHAR[]> GetWindowsError(ULONG error_code = GetLastError())
{
	auto msg = std::make_unique<WCHAR[]>(USHRT_MAX);
	if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error_code, 0, msg.get(), USHRT_MAX, nullptr))
	{
		return msg;
	}
	return nullptr;
}
void PrintWindowsError(ULONG error_code = GetLastError())
{
	if (auto error_msg = GetWindowsError(error_code))
	{
		fprintf(stderr, "%ls\n", error_msg.get());
	}
}
constexpr LONG64 inline ROUNDUP(LONG64 number, ULONG num_digits) noexcept
{
	return (number + num_digits - 1) / num_digits * num_digits;
}
_Success_(return == true)
bool reflink(_In_z_ PCWSTR oldpath, _In_z_ PCWSTR newpath)
{
	_ASSERTE(oldpath != nullptr && newpath != nullptr);
	ATL::CHandle source(CreateFileW(oldpath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
	if (source == INVALID_HANDLE_VALUE)
	{
		source.Detach();
		return false;
	}

	ULONG fs_flags;
	if (!GetVolumeInformationByHandleW(source, nullptr, 0, nullptr, nullptr, &fs_flags, nullptr, 0))
	{
		return false;
	}
	if (!(fs_flags & FILE_SUPPORTS_BLOCK_REFCOUNTING))
	{
		SetLastError(ERROR_NOT_CAPABLE);
		return false;
	}

	LARGE_INTEGER file_size;
	if (!GetFileSizeEx(source, &file_size))
	{
		return false;
	}
	FILE_BASIC_INFO file_basic;
	if (!GetFileInformationByHandleEx(source, FileBasicInfo, &file_basic, sizeof file_basic))
	{
		return false;
	}
	ULONG dummy;
	FSCTL_GET_INTEGRITY_INFORMATION_BUFFER get_integrity;
	if (!DeviceIoControl(source, FSCTL_GET_INTEGRITY_INFORMATION, nullptr, 0, &get_integrity, sizeof get_integrity, &dummy, nullptr))
	{
		return false;
	}

#ifdef _DEBUG
	ATL::CHandle destination(CreateFileW(newpath, GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_ALWAYS, 0, source));
#else
	ATL::CHandle destination(CreateFileW(newpath, GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW, 0, source));
#endif
	if (destination == INVALID_HANDLE_VALUE)
	{
		destination.Detach();
		return false;
	}
	FILE_DISPOSITION_INFO dispose = { TRUE };
	if (!SetFileInformationByHandle(destination, FileDispositionInfo, &dispose, sizeof dispose))
	{
		return false;
	}

	if (file_basic.FileAttributes & FILE_ATTRIBUTE_SPARSE_FILE)
	{
		if (!DeviceIoControl(destination, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &dummy, nullptr))
		{
			return false;
		}
	}
	FSCTL_SET_INTEGRITY_INFORMATION_BUFFER set_integrity = { get_integrity.ChecksumAlgorithm, get_integrity.Reserved, get_integrity.Flags };
	if (!DeviceIoControl(destination, FSCTL_SET_INTEGRITY_INFORMATION, &set_integrity, sizeof set_integrity, nullptr, 0, nullptr, nullptr))
	{
		return false;
	}
	FILE_END_OF_FILE_INFO end_of_file = { file_size };
	if (!SetFileInformationByHandle(destination, FileEndOfFileInfo, &end_of_file, sizeof end_of_file))
	{
		return false;
	}

	const ULONG split_threshold = 0UL - get_integrity.ClusterSizeInBytes;

	DUPLICATE_EXTENTS_DATA dup_extent = { source };
	for (LONG64 offset = 0, remain = ROUNDUP(file_size.QuadPart, get_integrity.ClusterSizeInBytes); remain > 0; offset += split_threshold, remain -= split_threshold)
	{
		dup_extent.SourceFileOffset.QuadPart = dup_extent.TargetFileOffset.QuadPart = offset;
		dup_extent.ByteCount.QuadPart = std::min<LONG64>(split_threshold, remain);
		_ASSERTE(dup_extent.SourceFileOffset.QuadPart % get_integrity.ClusterSizeInBytes == 0);
		_ASSERTE(dup_extent.ByteCount.QuadPart % get_integrity.ClusterSizeInBytes == 0);
		_ASSERTE(dup_extent.ByteCount.HighPart == 0);
		_RPT3(_CRT_WARN, "r=%llx\no=%llx\nb=%llx\n\n", remain, dup_extent.SourceFileOffset.QuadPart, dup_extent.ByteCount.QuadPart);
		if (!DeviceIoControl(destination, FSCTL_DUPLICATE_EXTENTS_TO_FILE, &dup_extent, sizeof dup_extent, nullptr, 0, &dummy, nullptr))
		{
			_CrtDbgBreak();
			return false;
		}
	}

	FILETIME atime = { file_basic.LastAccessTime.LowPart, ULONG(file_basic.LastAccessTime.HighPart) };
	FILETIME mtime = { file_basic.LastWriteTime.LowPart, ULONG(file_basic.LastWriteTime.HighPart) };
	SetFileTime(destination, nullptr, &atime, &mtime);
	dispose = { FALSE };
	return !!SetFileInformationByHandle(destination, FileDispositionInfo, &dispose, sizeof dispose);
}
int __cdecl wmain(int argc, PWSTR argv[])
{
	_CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_LEAK_CHECK_DF);
	_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_FILE);
	setlocale(LC_ALL, "");

	if (argc != 3)
	{
		fputs(
			"Copy file without actual data write.\n"
			"\n"
			"reflink source destination\n"
			"\n"
			"source       Specifies a file to copy.\n"
			"             source must have placed on the ReFS volume.\n"
			"destination  Specifies new file name.\n"
			"             destination must have placed on the same volume as source.\n",
			stderr
		);
		return EXIT_FAILURE;
	}
	if (!reflink(argv[1], argv[2]))
	{
		PrintWindowsError();
		return EXIT_FAILURE;
	}
}