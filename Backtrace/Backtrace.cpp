#include "Backtrace.h"
#include <memory>

#define USED_CONTEXT_FLAGS    CONTEXT_ALL
#define BACKTRACE_MAX_NAMELEN 254
#define BACKTRACE_MAIN        "main"
#define SUCCESS_CODE          1L
#define BACKTRACE_CUTOFF      2 //0 to rewind member functions

namespace
{
   typedef struct CallstackEntry
   {
      DWORD64  offset;
      CHAR     function[BACKTRACE_MAX_NAMELEN];
      DWORD64  offsetFromSmybol;
      DWORD    offsetFromLine;
      DWORD    line;
      CHAR     file[BACKTRACE_MAX_NAMELEN];
   };

   // TODO: Try to replace with template<> struct
#ifdef _M_IX86
#define GET_CURRENT_CONTEXT(context, contextFlags) \
  do { \
    memset(&context, 0, sizeof(CONTEXT)); \
    context.ContextFlags = contextFlags; \
    __asm    call x \
    __asm x: pop eax \
    __asm    mov context.Eip, eax \
    __asm    mov context.Ebp, ebp \
    __asm    mov context.Esp, esp \
  } while(0);
#else
#define GET_CURRENT_CONTEXT(context, contextFlags) \
  do { \
    memset(&context, 0, sizeof(CONTEXT)); \
    context.ContextFlags = contextFlags; \
    RtlCaptureContext(&context); \
} while(0);
#endif

   // SymCleanup()
   typedef BOOL(__stdcall *tSC)(
      IN HANDLE                        hProcess);
   tSC pSC;

   // SymFunctionTableAccess64()
   typedef PVOID(__stdcall *tSFTA)(
      HANDLE                           hProcess,
      DWORD64                          AddrBase);
   tSFTA pSFTA;

   // SymGetLineFromAddr64()
   typedef BOOL(__stdcall *tSGLFA)(
      IN HANDLE                        hProcess,
      IN DWORD64                       dwAddr,
      OUT PDWORD                       pdwDisplacement,
      OUT PIMAGEHLP_LINE64             Line);
   tSGLFA pSGLFA;

   // SymGetModuleBase64()
   typedef DWORD64(__stdcall *tSGMB)(
      IN HANDLE                        hProcess,
      IN DWORD64                       dwAddr);
   tSGMB pSGMB;

   // SymGetSymFromAddr64()
   typedef BOOL(__stdcall *tSGSFA)(
      IN HANDLE                        hProcess,
      IN DWORD64                       dwAddr,
      OUT PDWORD64                     pdwDisplacement,
      OUT PIMAGEHLP_SYMBOL64           Symbol);
   tSGSFA pSGSFA;

   // SymInitialize()
   typedef BOOL(__stdcall *tSI)(
      IN HANDLE                        hProcess,
      IN PSTR                          UserSearchPath,
      IN BOOL                          fInvadeProcess);
   tSI pSI;

   // SymLoadModule64()
   typedef DWORD64(__stdcall *tSLM)(
      IN HANDLE                        hProcess,
      IN HANDLE                        hFile,
      IN PSTR                          ImageName,
      IN PSTR                          ModuleName,
      IN DWORD64                       BaseOfDll,
      IN DWORD                         SizeOfDll);
   tSLM pSLM;

   // StackWalk64()
   typedef BOOL(__stdcall *tSW)(
      DWORD                            MachineType,
      HANDLE                           hProcess,
      HANDLE                           hThread,
      LPSTACKFRAME64                   StackFrame,
      PVOID                            ContextRecord,
      PREAD_PROCESS_MEMORY_ROUTINE64   ReadMemoryRoutine,
      PFUNCTION_TABLE_ACCESS_ROUTINE64 FunctionTableAccessRoutine,
      PGET_MODULE_BASE_ROUTINE64       GetModuleBaseRoutine,
      PTRANSLATE_ADDRESS_ROUTINE64     TranslateAddress);
   tSW pSW;

   BOOL __stdcall ReadProcMem(HANDLE hProcess, DWORD64 qwBaseAddress, PVOID lpBuffer, DWORD nSize, LPDWORD lpNumberOfBytesRead)
   {
      SIZE_T st;
      BOOL bRet = ReadProcessMemory(hProcess, (LPVOID)qwBaseAddress, lpBuffer, nSize, &st);
      *lpNumberOfBytesRead = (DWORD)st;
      return bRet;
   }

   DWORD LoadModule(HANDLE hProcess, const char modulePath[], DWORD64 baseAddr, DWORD size)
   {
      // TODO: ugly things are going on here..
      DWORD result = ERROR_SUCCESS;
      if (pSLM(hProcess, NULL, (PSTR)modulePath, NULL, baseAddr, size) == ERROR_SUCCESS)
         result = GetLastError();
      return result;
   }
   void LoadDbgHelp(HMODULE& hDbgHelp, const HANDLE& hProc)
   {
      if (hDbgHelp == NULL)
         hDbgHelp = LoadLibrary(_T("dbghelp.dll"));

      pSI = (tSI)GetProcAddress(hDbgHelp, "SymInitialize");
      pSC = (tSC)GetProcAddress(hDbgHelp, "SymCleanup");
      pSW = (tSW)GetProcAddress(hDbgHelp, "StackWalk64");
      pSFTA = (tSFTA)GetProcAddress(hDbgHelp, "SymFunctionTableAccess64");
      pSGLFA = (tSGLFA)GetProcAddress(hDbgHelp, "SymGetLineFromAddr64");
      pSGMB = (tSGMB)GetProcAddress(hDbgHelp, "SymGetModuleBase64");
      pSGSFA = (tSGSFA)GetProcAddress(hDbgHelp, "SymGetSymFromAddr64");
      pSLM = (tSLM)GetProcAddress(hDbgHelp, "SymLoadModule64");

      // https://msdn.microsoft.com/en-us/library/windows/desktop/ms681351(v=vs.85).aspx
      if (pSI(hProc, NULL, FALSE) == FALSE)
      {
         FreeLibrary(hDbgHelp);
         hDbgHelp = NULL;
         pSC = NULL;
         throw std::domain_error("Could not load dbghelp.dll");
      }
   }
   void LoadPsapi(HANDLE hProcess)
   {
      typedef struct _MODULEINFO {
         LPVOID   lpBaseOfDll;
         DWORD    SizeOfImage;
         LPVOID   EntryPoint;
      } *LPMODULEINFO;
      _MODULEINFO moduleInfo;

      typedef BOOL(__stdcall *tEPM)(HANDLE hProcess, HMODULE *lphModule, DWORD cb, LPDWORD lpcbNeeded); tEPM pEPM;
      typedef DWORD(__stdcall *tGMFNE)(HANDLE hProcess, HMODULE hModule, LPSTR lpFilename, DWORD nSize); tGMFNE pGMFNE;
      typedef BOOL(__stdcall *tGMI)(HANDLE hProcess, HMODULE hModule, LPMODULEINFO pmi, DWORD nSize); tGMI pGMI;

      HINSTANCE hPsapi;

      HMODULE *hMods = 0;
      const SIZE_T BUFFER_LEN = 8096;

      hPsapi = LoadLibrary(_T("psapi.dll"));
      if (NULL == hPsapi)
         throw std::domain_error("Could not load psapi.dll");

      pEPM = (tEPM)GetProcAddress(hPsapi, "EnumProcessModules");
      pGMFNE = (tGMFNE)GetProcAddress(hPsapi, "GetModuleFileNameExA");
      pGMI = (tGMI)GetProcAddress(hPsapi, "GetModuleInformation");

      if ((NULL == pEPM) || (NULL == pGMFNE) || (NULL == pGMI))
      {
         FreeLibrary(hPsapi);
         throw std::domain_error("Invalid interface response dbghelp.dll");
      }

      hMods = (HMODULE*)malloc(sizeof(HMODULE) * (BUFFER_LEN / sizeof HMODULE));
      if (hMods == NULL)
      {
         if (hPsapi != NULL) FreeLibrary(hPsapi);
         if (hMods != NULL) free(hMods);
         throw std::length_error("Not enough memory");
      }

      DWORD cbNeeded; // just to cover interface
      if (!pEPM(hProcess, hMods, BUFFER_LEN, &cbNeeded))
      {
         if (hPsapi != NULL) FreeLibrary(hPsapi);
         if (hMods != NULL) free(hMods);
         throw std::domain_error("Invalid interface response dbghelp.dll");
      }

      pGMI(hProcess, hMods[0], &moduleInfo, sizeof moduleInfo); // base address, size

      char modulePath[sizeof(char) * BUFFER_LEN];
      modulePath[0] = 0;
      pGMFNE(hProcess, hMods[0], (LPSTR)modulePath, BACKTRACE_MAX_NAMELEN); // image file function

      // should provide address
      // TODO: add other error codes from MSDN
      if (ERROR_SUCCESS != LoadModule(hProcess, modulePath, (DWORD64)moduleInfo.lpBaseOfDll, moduleInfo.SizeOfImage))
         throw std::domain_error("Invalid interface response dbghelp.dll");

      if (hPsapi != NULL) FreeLibrary(hPsapi);
      if (hMods != NULL) free(hMods);
   }
   void InitStackFrame(CONTEXT& context, DWORD& imageType, STACKFRAME64& stackFrame)
   {
      memset(&stackFrame, 0, sizeof(stackFrame));

      // TODO: if possible try compile-time struct
      GET_CURRENT_CONTEXT(context, USED_CONTEXT_FLAGS);

#ifdef _M_IX86
      imageType = IMAGE_FILE_MACHINE_I386;
      stackFrame.AddrPC.Offset = context.Eip;
      stackFrame.AddrPC.Mode = AddrModeFlat;
      stackFrame.AddrFrame.Offset = context.Ebp;
      stackFrame.AddrFrame.Mode = AddrModeFlat;
      stackFrame.AddrStack.Offset = context.Esp;
      stackFrame.AddrStack.Mode = AddrModeFlat;
#elif _M_X64
      imageType = IMAGE_FILE_MACHINE_AMD64;
      stackFrame.AddrPC.Offset = context.Rip;
      stackFrame.AddrPC.Mode = AddrModeFlat;
      stackFrame.AddrFrame.Offset = context.Rsp;
      stackFrame.AddrFrame.Mode = AddrModeFlat;
      stackFrame.AddrStack.Offset = context.Rsp;
      stackFrame.AddrStack.Mode = AddrModeFlat;
#elif _M_IA64
      imageType = IMAGE_FILE_MACHINE_IA64;
      stackFrame.AddrPC.Offset = context.StIIP;
      stackFrame.AddrPC.Mode = AddrModeFlat;
      stackFrame.AddrFrame.Offset = context.IntSp;
      stackFrame.AddrFrame.Mode = AddrModeFlat;
      stackFrame.AddrBStore.Offset = context.RsBSP;
      stackFrame.AddrBStore.Mode = AddrModeFlat;
      stackFrame.AddrStack.Offset = context.IntSp;
      stackFrame.AddrStack.Mode = AddrModeFlat;
#else
      throw std::domain_error("Platform not supported");
#endif
   }
   void GetNextFrame(const DWORD& iType, const HANDLE& hProc, STACKFRAME64& sFrame, CONTEXT& c)
   {
      if (pSW == NULL)
         throw std::domain_error("Invalid interface response dbghelp.dll");

      auto res = pSW(iType, hProc, GetCurrentThread(), &sFrame, &c, ReadProcMem, pSFTA, pSGMB, NULL);
      if (res != SUCCESS_CODE)
         throw std::invalid_argument("Not accessible stack frame");

      if (sFrame.AddrPC.Offset == sFrame.AddrReturn.Offset ||
         sFrame.AddrPC.Offset == 0)
         throw std::invalid_argument("Not valid PC address");
   }
   void GetFunctionName(const HANDLE& hProc, const STACKFRAME64& sFrame, CallstackEntry& csEntry, PIMAGEHLP_SYMBOL64& pSym)
   {
      pSym = (IMAGEHLP_SYMBOL64*)malloc(sizeof(IMAGEHLP_SYMBOL64) + BACKTRACE_MAX_NAMELEN);
      if (NULL == pSym)
         throw std::length_error("Not enough memory");

      memset(pSym, 0, sizeof(IMAGEHLP_SYMBOL64) + BACKTRACE_MAX_NAMELEN);
      pSym->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
      pSym->MaxNameLength = BACKTRACE_MAX_NAMELEN;

      if (NULL == pSGSFA)
         throw std::domain_error("Invalid interface response dbghelp.dll");

      if (FALSE != pSGSFA(hProc, sFrame.AddrPC.Offset, &(csEntry.offsetFromSmybol), pSym))
         strcpy_s(csEntry.function, pSym->Name);
      else
         strcpy_s(csEntry.function, "NOT_AVAILABLE");
   }
   void GetLineAndFile(const HANDLE& hProcess, const STACKFRAME64& stackFrame, CallstackEntry& csEntry)
   {
      if (pSGLFA == NULL)
         throw std::domain_error("Invalid interface response dbghelp.dll");

      IMAGEHLP_LINE64 Line;
      memset(&Line, 0, sizeof(Line));
      Line.SizeOfStruct = sizeof(Line);
      if (pSGLFA(hProcess, stackFrame.AddrPC.Offset, &(csEntry.offsetFromLine), &Line) != FALSE)
      {
         csEntry.line = Line.LineNumber;
         strcpy_s(csEntry.file, Line.FileName);
      }
      else
      {
         csEntry.line = -1;
         strcpy_s(csEntry.file, "NOT_AVAILABLE");
      }
   }
}

Backtrace::Backtrace(const int& maxDepth) :
   maxDepth(maxDepth + BACKTRACE_CUTOFF),
   dwProcessId(GetCurrentProcessId()),
   hDbgHelp(NULL),
   hProcess(GetCurrentProcess())
{}

Backtrace::~Backtrace()
{
   if (pSC != NULL)
      pSC(hProcess);
   if (hDbgHelp != NULL)
      FreeLibrary(hDbgHelp);
   hDbgHelp = NULL;
}

std::string Backtrace::GetBacktrace()
{
   try
   {
      Callstack();
   }
   catch (std::exception e)
   {
      std::stringstream error;
      error << "Callstack exception occurred:\n" << e.what() << std::endl;
      return error.str();
   }

   std::string sRet;
   std::ostringstream lineOss;
   std::ostringstream it;

   for (size_t i = 0; i < callStack.size(); i++)
   {
      if (i > BACKTRACE_CUTOFF)
      {
         auto frame = callStack[i];

         lineOss << frame.line;
         it << i - BACKTRACE_CUTOFF;

         sRet += "[bt]: (" + it.str() + ") "
            + frame.file + ":"
            + lineOss.str() + " "
            + frame.function + ":\n";

         lineOss.str("");
         it.str("");
      }
   }

   return sRet;
}

void Backtrace::Callstack()
{
   CONTEXT context;
   DWORD imageType;
   STACKFRAME64 stackFrame;
   IMAGEHLP_SYMBOL64 *pSym = NULL;

   try
   {
      LoadDbgHelp(hDbgHelp, hProcess);
      LoadPsapi(hProcess);
      InitStackFrame(context, imageType, stackFrame);
   }
   catch (std::domain_error &e)
   {
      throw std::exception(e.what());
   }
   catch (std::length_error &e)
   {
      throw std::exception(e.what());
   }

   CallstackEntry csEntry{ stackFrame.AddrPC.Offset, 0, 0, -1 };
   Entry entry{ "", 0, "", 0 };

   while (entry.function != BACKTRACE_MAIN && entry.count <= maxDepth)
   {
      try
      {
         GetNextFrame(imageType, hProcess, stackFrame, context);
         GetFunctionName(hProcess, stackFrame, csEntry, pSym);
         GetLineAndFile(hProcess, stackFrame, csEntry);
      }
      catch (std::invalid_argument &e)
      {
         //Log::Info << "Callstack handled exception: " << e.what() << std::endl;
         std::cout << "Callstack handled exception: " << e.what() << std::endl; // TO DELETE!
         break;
      }

      if (csEntry.offset != 0)
      {
         entry.function = csEntry.function;
         entry.line = csEntry.line;
         entry.file = csEntry.file;
         entry.count++;

         callStack.push_back(entry);
      }
   }

   if (NULL != pSym)
      free(pSym);
}