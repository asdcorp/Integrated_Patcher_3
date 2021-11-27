#define _WIN32_WINNT _WIN32_WINNT_WIN10
#include <Windows.h>
#include <stdio.h>
#include <time.h>
#include <ImageHlp.h>

#define TARGETSKU_FILE L"TargetSKU.txt"

#define GATHEROSSTATE_ORIGINAL L"gatherosstate.exe"
#define GATHEROSSTATE_MODIFIED L"gatherosstatemodified.exe"

#define GATHEROSSTATE_ORIGINAL_CHECKSUM (DWORD) 0x60A8D

#define SL_E_VALUE_NOT_FOUND 0xC004F012

typedef DWORD SLDATATYPE;
typedef GUID SLID;
typedef DWORD SLIDTYPE;
typedef void * HSLC;

typedef struct _SL_LICENSING_STATUS {
	SLID SkuId;
	DWORD eStatus;
	DWORD dwGraceTime;
	DWORD dwTotalGraceDays;
	HRESULT hrReason;
	UINT64 qwValidityExpiration;
} SL_LICENSING_STATUS;

// Non-exported internal functions

BOOL CheckIfFileExists(
	wchar_t *lpwszFileName
) {

	DWORD dwFileAttributes = GetFileAttributes(
		lpwszFileName
	);

	if(dwFileAttributes == INVALID_FILE_ATTRIBUTES)
		return FALSE;

	return TRUE;
}

BOOL ReadFileToString(
	HANDLE hFile,
	char *lpszOutput,
	DWORD cbBufferSize
) {
	// Using fgetws wouldn't be a crime, but we're not really using any stdlib functions elsewhere.  
	DWORD cbBytesWritten = 0;
	return ReadFile(
		hFile,
		lpszOutput,
		cbBufferSize,
		&cbBytesWritten,
		NULL
	);
}

BOOL WriteToFileAtOffset(
	HANDLE hFile,
	DWORD dwOffset,
	void *lpBuffer,
	DWORD cbToWrite
) {
	BOOL status;
	DWORD cbBytesWritten = 0;

	//Using the OVERLAPPED structure is unnecessary and would require heap allocation.
	SetFilePointer(
		hFile,
		dwOffset,
		NULL,
		FILE_BEGIN
	);

	status = WriteFile(
		hFile,
		lpBuffer,
		cbToWrite,
		&cbBytesWritten,
		NULL
	);

	if(cbBytesWritten != cbToWrite)
		status = FALSE;

	return status;
}

wchar_t *GetRegistryPfn() {
	HRESULT status;
	DWORD cbBufferSize = 256 * sizeof(wchar_t);

	wchar_t *lpwszOSProductPfn = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cbBufferSize);

	reRead:
	status = RegGetValueW(
		HKEY_LOCAL_MACHINE,
		L"SYSTEM\\CurrentControlSet\\Control\\ProductOptions",
		L"OSProductPfn",
		RRF_RT_REG_SZ,
		NULL,
		lpwszOSProductPfn,
		&cbBufferSize
	);

	if(status == ERROR_MORE_DATA) {

		cbBufferSize += 8 * sizeof(wchar_t);

		lpwszOSProductPfn = HeapReAlloc(
			GetProcessHeap(),
			HEAP_ZERO_MEMORY,
			lpwszOSProductPfn,
			cbBufferSize
		);

		goto reRead;
	}

	if(status != ERROR_SUCCESS)
		return NULL;

	return lpwszOSProductPfn;
}

DWORD SkuIdFromPfn(
	wchar_t *lpwszPfn
) {
	DWORD dwSkuId;
	if(wcsncmp(lpwszPfn, L"Microsoft.Windows.", 18) == 0) {
		wchar_t *lpwszSkuId = lpwszPfn + 18; //Skip the Microsoft.Windows. part
		DWORD cbSkuIdLength = wcschr(lpwszSkuId, L'.') - lpwszSkuId;

		lpwszSkuId[cbSkuIdLength] = 0; //Cut off string after .
		dwSkuId = wcstoul(lpwszSkuId, NULL, 10);
	} else {
		// Resort to system detection.
		GetProductInfo(10, 0, 0, 0, (LPDWORD) &dwSkuId);
	}

	return dwSkuId;
}

wchar_t *GetSystemChannel() {
	wchar_t *lpwszSystemChannel;
	LSTATUS lDetectionStatus = ERROR_ACCESS_DENIED;

	// Used only here. Does not need a typedef. Could even be entirely based on an offset.
	struct DigitalProductId4 {
		unsigned int uiSize;
		unsigned short MajorVersion;
		unsigned short MinorVersion;
		wchar_t szAdvancedPid[64];
		wchar_t szActivationId[64];
		wchar_t szOemID[8];
		wchar_t szEditionType[260];
		BYTE bIsUpgrade;
		BYTE bReserved[7];
		BYTE bCDKey[16];
		BYTE bCDKey256Hash[32];
		BYTE b256Hash[32];
		wchar_t szEditionId[64];
		wchar_t szKeyType[64];
		wchar_t szEULA[64];
	};

	struct DigitalProductId4 *lpDigitalProductId4 = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(struct DigitalProductId4));
	DWORD cbDigitalProductId4 = sizeof(struct DigitalProductId4);

	HKEY hKey;
	if(!RegOpenKeyEx(
		HKEY_LOCAL_MACHINE,
		L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
		0,
		KEY_READ | KEY_WOW64_64KEY,
		&hKey
	)) {
		lDetectionStatus = RegGetValue(
			hKey,
			L"",
			L"DigitalProductId4",
			RRF_RT_REG_BINARY,
			NULL,
			lpDigitalProductId4,
			&cbDigitalProductId4
		);
	}
	if(lDetectionStatus != ERROR_SUCCESS)
		return 0;

	lpwszSystemChannel = HeapAlloc(GetProcessHeap(), 0, sizeof(lpDigitalProductId4->szKeyType));
	wcscpy(lpwszSystemChannel, lpDigitalProductId4->szKeyType);

	HeapFree(GetProcessHeap(), 0, lpDigitalProductId4);
	
	return lpwszSystemChannel;
}

// Exported functions

BOOL APIENTRY WINAPI dll_main(
	HINSTANCE hinstDll,
	DWORD fdwReason,
	LPVOID lpvReserved
) {
	return TRUE;
}

DWORD SLGetSkuInfo(
	wchar_t *lpwszChannel
) {
	DWORD dwSkuId;
	long lChannel = -1;

	// Read the configuration file.
	if(CheckIfFileExists(TARGETSKU_FILE)) {
		HANDLE hConfigFile;
		char *lpszConfigContents;
		DWORD dwFileSize;

		hConfigFile = CreateFile(
			TARGETSKU_FILE,
			GENERIC_READ,
			FILE_SHARE_READ,
			NULL,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			NULL
		);
		// This assures us that further used calls have no chance of failing.
		if(hConfigFile == INVALID_HANDLE_VALUE) {
			goto noConfig;
		}

		dwFileSize = GetFileSize(
			hConfigFile,
			NULL
		);

		lpszConfigContents = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwFileSize);

		ReadFileToString(hConfigFile, lpszConfigContents, dwFileSize);

		long tempSkuId;
		if(sscanf(lpszConfigContents, "%ld %ld", &tempSkuId, &lChannel) < 2)
			goto noConfig;
		
		if(tempSkuId == -1)
			GetProductInfo(10, 0, 0, 0, &dwSkuId);
		else 
			dwSkuId = (DWORD) tempSkuId;
	} else {
		noConfig:
		; // Automatic SkuId detection.

		wchar_t *lpwszOSProductPfn = GetRegistryPfn();
		dwSkuId = SkuIdFromPfn(lpwszOSProductPfn);

		HeapFree(
			GetProcessHeap(),
			0,
			lpwszOSProductPfn
		);
	}

	if(!lpwszChannel)
		return dwSkuId;

	switch(lChannel) {
		case -1: // Automatic Channel detection
			wcscpy(lpwszChannel, GetSystemChannel());
			break;
		case 0:
			wcscpy(lpwszChannel, L"Retail");
			break;
		case 1:
			wcscpy(lpwszChannel, L"Volume:GVLK");
			break;
	}

	return dwSkuId;
}

HRESULT WINAPI SLGetPKeyInformation(
	HSLC hSLC,
	SLID *pPKeyId,
	wchar_t *lpwszValueName,
	SLDATATYPE *lpDataType,
	unsigned int *lpcbValue,
	BYTE **lplpbValue
) {

	if(wcscmp(lpwszValueName, L"Channel") != 0)
		return SL_E_VALUE_NOT_FOUND;

	DWORD cbSize = 128;
	*lplpbValue = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cbSize);
	SLGetSkuInfo((short unsigned int *)*lplpbValue);

	*lpcbValue = cbSize;
	*lpDataType = REG_SZ;

	return S_OK;
}

HRESULT WINAPI SLGetLicensingStatusInformation(
	HSLC hSLC,
	SLID *lpAppID,
	SLID *lpProductSkuId,
	wchar_t *lpwszRightName,
	UINT *lpnStatusCount,
	SL_LICENSING_STATUS **lplpLicensingStatus
) {

	SL_LICENSING_STATUS *lpEntry = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(SL_LICENSING_STATUS));

	lpEntry->eStatus = 1;
	lpEntry->dwGraceTime = ((INT_MAX-60) - time(NULL)) / 60;

	*lpnStatusCount = 1;
	*lplpLicensingStatus = lpEntry;

	return S_OK;
}

HRESULT WINAPI SLGetWindowsInformationDWORD(
	wchar_t *lpwszValueName,
	DWORD *lpdwValue
) {

	DWORD dwValue = 1;

	if(wcscmp(lpwszValueName, L"Kernel-ProductInfo") == 0) {
		//dwValue = SLGetSkuInfo(NULL);
		dwValue = 100;
	}

	*lpdwValue = dwValue;
	return S_OK;
}

HRESULT WINAPI SLGetSLIDList(
	HSLC hSLC,
	SLIDTYPE eQueryIdType,
	SLID *pQueryId,
	SLIDTYPE eReturnIdType,
	unsigned int *lpnReturnIds,
	SLID **lplpReturnIds
) {

	*lpnReturnIds = 1;
	*lplpReturnIds = HeapAlloc(GetProcessHeap(), 0, sizeof(SLID));

	return S_OK;
}

HRESULT WINAPI SLGetGenuineInformation(
	const SLID *lpAppId,
	const wchar_t *lpwcszValueName,
	SLDATATYPE *lpeDataType,
	unsigned int *lpcbValue,
	BYTE **lplpbValue
) {
	return SL_E_VALUE_NOT_FOUND;
}

HRESULT WINAPI SLGetServiceInformation(
	HSLC hSLC,
	const wchar_t *lpwcszValueName,
	SLDATATYPE *lpeDataType,
	unsigned int *lpcbValue,
	BYTE **lplpbValue
) {
	return SL_E_VALUE_NOT_FOUND;
}

HRESULT WINAPI SLOpen(
	HSLC *phSLC
){

	*phSLC = (void *) 0xDABBED;
	return S_OK;
}

HRESULT WINAPI SLClose(
	HSLC hSLC
){
	return S_OK;
}

/*
 * This function patches the 14393 x86 ADK gatherosstate residing in the current working directory.
 * Keep in mind that it won't touch any other version of gatherosstate.
 */
void PatchGatherosstate() {
	BOOL status;
	BOOL bDiscontinueCopy = FALSE;
	HANDLE hModifiedFile;
	wchar_t *lpwszRegistryPfn = GetRegistryPfn();

	DWORD dwOriginalCheckSum = 0, dwModifiedCheckSum = 0;

	if(lpwszRegistryPfn == NULL)
		goto failed;;

	// Weed out improbable Pfns.
	if(wcslen(lpwszRegistryPfn) > 512)
		goto failed;;

	if(!CheckIfFileExists(GATHEROSSTATE_ORIGINAL))
		goto failed;;

	// Check the PE signature to verify that we're working with the x86 ADK 14393 gatherosstate.exe
	MapFileAndCheckSum(
		GATHEROSSTATE_ORIGINAL,
		&dwOriginalCheckSum,
		&dwOriginalCheckSum
	);
	if(dwOriginalCheckSum != GATHEROSSTATE_ORIGINAL_CHECKSUM) {
		MessageBox(NULL, L"You're using an incorrect version of gatherosstate. Please use the 14393 x86 ADK gatherosstate.exe", L"SLC Integrated Patcher", MB_OK);
		goto failed;
	}

	if(!CopyFileEx(
		GATHEROSSTATE_ORIGINAL,
		GATHEROSSTATE_MODIFIED,
		NULL,
		NULL,
		&bDiscontinueCopy,
		COPY_FILE_OPEN_SOURCE_FOR_WRITE
	)){
		goto failed;
	}


	hModifiedFile = CreateFile(
		GATHEROSSTATE_MODIFIED,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL
	);
	if(hModifiedFile == INVALID_HANDLE_VALUE) {
		goto failed;
	}

	status = WriteToFileAtOffset(
		hModifiedFile,
		0x68C0,
		lpwszRegistryPfn,
		(wcslen(lpwszRegistryPfn) + 1) * sizeof(wchar_t)
	);
	if(!status) {
		goto failed;
	}

	MapFileAndCheckSum(
		GATHEROSSTATE_MODIFIED,
		&dwOriginalCheckSum,
		&dwModifiedCheckSum
	);
	if(dwOriginalCheckSum == dwModifiedCheckSum) { // Pretty self-explanatory
		goto failed;
	}

	status = WriteToFileAtOffset(
		hModifiedFile,
		0x140, // Pretty much constant. This is the offset of the Optional Header's Checksum field.
		&dwModifiedCheckSum,
		sizeof(DWORD)
	);
	if(!status) {
		goto failed;
	}

	CloseHandle(hModifiedFile);

	HeapFree(GetProcessHeap(), 0, lpwszRegistryPfn);

	return;

	// We cannot return an error code using rundll32.
	// In case of any plans for adding error checking - create a file and write to it.
	// Not much else can be done.
	failed:

	HeapFree(GetProcessHeap(), 0, lpwszRegistryPfn);

	if(CheckIfFileExists(GATHEROSSTATE_MODIFIED)) {
		DeleteFile(
			GATHEROSSTATE_MODIFIED
		);
	}

	exit(10);
}