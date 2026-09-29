#ifndef TLOU_PLUGIN_SDK_H
#define TLOU_PLUGIN_SDK_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
    Only functions and one opaque handle cross the boundary between the loader
    and a plugin.

    A plugin exports:

        int  Init(LoaderHandle loader);          required
        int  SupportsGame(LoaderHandle loader);  optional, absent means yes
        const char* PluginName(void);            optional
        const char* PluginVersion(void);         optional
        const char* PluginAuthor(void);          optional

    Only Init is declared here. Define the optional ones in the plugin as

        extern "C" TLOU_PLUGIN_EXPORT const char* PluginName(void) { ... }

    Define TLOU_PLUGIN before including this header to get the export macro.

    Call loaderBind from Init or SupportsGame before using any loader wrapper
    below, once in each translation unit that uses a wrapper.

    Strings returned by the loader stay valid for the life of the process.
*/

typedef struct LoaderHandle_ *LoaderHandle;

/*
    A hook callback is handed an opaque pointer to the registers as they were
    at the hook site, and reads one with loaderHookRegister. A number below is
    never reused for another register.
*/
typedef void (*LoaderHookCallback)(void *registers, void *user);

#define LOADER_REG_RAX  0
#define LOADER_REG_RCX  1
#define LOADER_REG_RDX  2
#define LOADER_REG_RBX  3
#define LOADER_REG_RBP  4
#define LOADER_REG_RSI  5
#define LOADER_REG_RDI  6
#define LOADER_REG_R8   7
#define LOADER_REG_R9   8
#define LOADER_REG_R10  9
#define LOADER_REG_R11  10
#define LOADER_REG_R12  11
#define LOADER_REG_R13  12
#define LOADER_REG_R14  13
#define LOADER_REG_R15  14
#define LOADER_REG_COUNT 15

/* Windows only. __declspec(dllexport) is understood by MSVC, gcc and clang. */
#ifdef TLOU_PLUGIN
#define TLOU_PLUGIN_EXPORT __declspec(dllexport)
#else
#define TLOU_PLUGIN_EXPORT
#endif

TLOU_PLUGIN_EXPORT int Init(LoaderHandle loader);

/* ------------------------------------------------------------------------ */
/* Plugin side only. The loader must include this header without
   TLOU_PLUGIN. */
#ifdef TLOU_PLUGIN

static LoaderHandle global_loaderHandle = 0;

static void     (*global_loaderLogFn)(LoaderHandle, const char*, const char*, ...) = 0;
static void*    (*global_loaderFindPatternFn)(LoaderHandle, const char*) = 0;
static int      (*global_loaderPatchFn)(LoaderHandle, void*, const void*, size_t, const void*) = 0;
static void*    (*global_loaderAllocNearGameFn)(LoaderHandle, size_t) = 0;
static uint64_t (*global_loaderGameModuleBaseFn)(LoaderHandle) = 0;
static uint64_t (*global_loaderGameModuleSizeFn)(LoaderHandle) = 0;
static uint64_t (*global_loaderGameTextBaseFn)(LoaderHandle) = 0;
static uint64_t (*global_loaderGameTextSizeFn)(LoaderHandle) = 0;
static uint32_t (*global_loaderGameTimeDateStampFn)(LoaderHandle) = 0;
static uint32_t (*global_loaderGameCheckSumFn)(LoaderHandle) = 0;
static const char* (*global_loaderGameExeNameFn)(LoaderHandle) = 0;
static uint32_t (*global_loaderGameFileVersionFn)(LoaderHandle, int) = 0;
static const char* (*global_loaderGameFolderFn)(LoaderHandle) = 0;
static const char* (*global_loaderPluginsFolderFn)(LoaderHandle) = 0;
static const char* (*global_loaderLogsFolderFn)(LoaderHandle) = 0;
static const char* (*global_loaderConfigFolderFn)(LoaderHandle) = 0;
static const char* (*global_loaderModFolderFn)(LoaderHandle) = 0;
static const char* (*global_loaderCacheFolderFn)(LoaderHandle) = 0;
static int      (*global_loaderHookFunctionFn)(LoaderHandle, const char*, const char*, LoaderHookCallback, void*) = 0;
static int      (*global_loaderHookAddressFn)(LoaderHandle, const char*, uint64_t, LoaderHookCallback, void*) = 0;
static uint64_t (*global_loaderHookRegisterFn)(LoaderHandle, void*, int) = 0;
static int      (*global_loaderHookSetRegisterFn)(LoaderHandle, void*, int, uint64_t) = 0;
static uint64_t (*global_loaderHookTrampolineFn)(LoaderHandle, const char*) = 0;
static uint64_t (*global_loaderFindFunctionFn)(LoaderHandle, const char*, const char*) = 0;
static int      (*global_loaderDescribeFaultFn)(LoaderHandle, void*, const char*, const char*) = 0;
static void     (*global_loaderReportBadArgumentFn)(LoaderHandle, const char*, const char*, const char*, const char*, unsigned int) = 0;

/* The handle is the loader's own module. Plugin code must treat it as
   opaque. */
#define TLOU_PLUGIN_BIND(slot, type, exportName)                     \
    slot = (type)(void*)GetProcAddress(m, exportName);               \
    if (slot) ++resolved;

static inline int loaderBind(LoaderHandle loader)
{
    HMODULE m = (HMODULE)loader;
    int resolved = 0;
    if (!m) return 0;
    global_loaderHandle = loader;

    TLOU_PLUGIN_BIND(global_loaderLogFn,
        void (*)(LoaderHandle, const char*, const char*, ...), "LoaderLog")
    TLOU_PLUGIN_BIND(global_loaderFindPatternFn,
        void* (*)(LoaderHandle, const char*), "LoaderFindPattern")
    TLOU_PLUGIN_BIND(global_loaderPatchFn,
        int (*)(LoaderHandle, void*, const void*, size_t, const void*), "LoaderPatch")
    TLOU_PLUGIN_BIND(global_loaderAllocNearGameFn,
        void* (*)(LoaderHandle, size_t), "LoaderAllocNearGame")
    TLOU_PLUGIN_BIND(global_loaderGameModuleBaseFn,
        uint64_t (*)(LoaderHandle), "LoaderGameModuleBase")
    TLOU_PLUGIN_BIND(global_loaderGameModuleSizeFn,
        uint64_t (*)(LoaderHandle), "LoaderGameModuleSize")
    TLOU_PLUGIN_BIND(global_loaderGameTextBaseFn,
        uint64_t (*)(LoaderHandle), "LoaderGameTextBase")
    TLOU_PLUGIN_BIND(global_loaderGameTextSizeFn,
        uint64_t (*)(LoaderHandle), "LoaderGameTextSize")
    TLOU_PLUGIN_BIND(global_loaderGameTimeDateStampFn,
        uint32_t (*)(LoaderHandle), "LoaderGameTimeDateStamp")
    TLOU_PLUGIN_BIND(global_loaderGameCheckSumFn,
        uint32_t (*)(LoaderHandle), "LoaderGameCheckSum")
    TLOU_PLUGIN_BIND(global_loaderGameExeNameFn,
        const char* (*)(LoaderHandle), "LoaderGameExeName")
    TLOU_PLUGIN_BIND(global_loaderGameFileVersionFn,
        uint32_t (*)(LoaderHandle, int), "LoaderGameFileVersion")
    TLOU_PLUGIN_BIND(global_loaderGameFolderFn,
        const char* (*)(LoaderHandle), "LoaderGameFolder")
    TLOU_PLUGIN_BIND(global_loaderPluginsFolderFn,
        const char* (*)(LoaderHandle), "LoaderPluginsFolder")
    TLOU_PLUGIN_BIND(global_loaderLogsFolderFn,
        const char* (*)(LoaderHandle), "LoaderLogsFolder")
    TLOU_PLUGIN_BIND(global_loaderConfigFolderFn,
        const char* (*)(LoaderHandle), "LoaderConfigFolder")
    TLOU_PLUGIN_BIND(global_loaderModFolderFn,
        const char* (*)(LoaderHandle), "LoaderModFolder")
    TLOU_PLUGIN_BIND(global_loaderCacheFolderFn,
        const char* (*)(LoaderHandle), "LoaderCacheFolder")
    TLOU_PLUGIN_BIND(global_loaderHookFunctionFn,
        int (*)(LoaderHandle, const char*, const char*, LoaderHookCallback, void*),
        "LoaderHookFunction")
    TLOU_PLUGIN_BIND(global_loaderHookRegisterFn,
        uint64_t (*)(LoaderHandle, void*, int), "LoaderHookRegister")
    TLOU_PLUGIN_BIND(global_loaderHookSetRegisterFn,
        int (*)(LoaderHandle, void*, int, uint64_t), "LoaderHookSetRegister")
    TLOU_PLUGIN_BIND(global_loaderHookAddressFn,
        int (*)(LoaderHandle, const char*, uint64_t, LoaderHookCallback, void*),
        "LoaderHookAddress")
    TLOU_PLUGIN_BIND(global_loaderHookTrampolineFn,
        uint64_t (*)(LoaderHandle, const char*), "LoaderHookTrampoline")
    TLOU_PLUGIN_BIND(global_loaderFindFunctionFn,
        uint64_t (*)(LoaderHandle, const char*, const char*), "LoaderFindFunction")
    TLOU_PLUGIN_BIND(global_loaderDescribeFaultFn,
        int (*)(LoaderHandle, void*, const char*, const char*), "LoaderDescribeFault")
    TLOU_PLUGIN_BIND(global_loaderReportBadArgumentFn,
        void (*)(LoaderHandle, const char*, const char*, const char*, const char*, unsigned int),
        "LoaderReportBadArgument")

    return resolved;
}

#undef TLOU_PLUGIN_BIND

static inline int loaderResolved(const char* name)
{
    if (!name) return 0;
    if (strcmp(name, "LoaderLog") == 0)               return global_loaderLogFn != 0;
    if (strcmp(name, "LoaderFindPattern") == 0)       return global_loaderFindPatternFn != 0;
    if (strcmp(name, "LoaderPatch") == 0)             return global_loaderPatchFn != 0;
    if (strcmp(name, "LoaderAllocNearGame") == 0)     return global_loaderAllocNearGameFn != 0;
    if (strcmp(name, "LoaderGameModuleBase") == 0)    return global_loaderGameModuleBaseFn != 0;
    if (strcmp(name, "LoaderGameModuleSize") == 0)    return global_loaderGameModuleSizeFn != 0;
    if (strcmp(name, "LoaderGameTextBase") == 0)      return global_loaderGameTextBaseFn != 0;
    if (strcmp(name, "LoaderGameTextSize") == 0)      return global_loaderGameTextSizeFn != 0;
    if (strcmp(name, "LoaderGameTimeDateStamp") == 0) return global_loaderGameTimeDateStampFn != 0;
    if (strcmp(name, "LoaderGameCheckSum") == 0)      return global_loaderGameCheckSumFn != 0;
    if (strcmp(name, "LoaderGameExeName") == 0)       return global_loaderGameExeNameFn != 0;
    if (strcmp(name, "LoaderGameFileVersion") == 0)   return global_loaderGameFileVersionFn != 0;
    if (strcmp(name, "LoaderGameFolder") == 0)        return global_loaderGameFolderFn != 0;
    if (strcmp(name, "LoaderPluginsFolder") == 0)     return global_loaderPluginsFolderFn != 0;
    if (strcmp(name, "LoaderLogsFolder") == 0)        return global_loaderLogsFolderFn != 0;
    if (strcmp(name, "LoaderConfigFolder") == 0)      return global_loaderConfigFolderFn != 0;
    if (strcmp(name, "LoaderModFolder") == 0)         return global_loaderModFolderFn != 0;
    if (strcmp(name, "LoaderCacheFolder") == 0)       return global_loaderCacheFolderFn != 0;
    if (strcmp(name, "LoaderHookFunction") == 0)      return global_loaderHookFunctionFn != 0;
    if (strcmp(name, "LoaderHookRegister") == 0)      return global_loaderHookRegisterFn != 0;
    if (strcmp(name, "LoaderHookSetRegister") == 0)   return global_loaderHookSetRegisterFn != 0;
    if (strcmp(name, "LoaderHookAddress") == 0)       return global_loaderHookAddressFn != 0;
    if (strcmp(name, "LoaderHookTrampoline") == 0)    return global_loaderHookTrampolineFn != 0;
    if (strcmp(name, "LoaderFindFunction") == 0)      return global_loaderFindFunctionFn != 0;
    if (strcmp(name, "LoaderDescribeFault") == 0)     return global_loaderDescribeFaultFn != 0;
    if (strcmp(name, "LoaderReportBadArgument") == 0)  return global_loaderReportBadArgumentFn != 0;
    return 0;
}

/* Each wrapper does nothing and reports failure when its function is not
   present in this loader. */

static inline void loaderLog(const char* plugin, const char* fmt, ...)
{
    if (!global_loaderLogFn) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    global_loaderLogFn(global_loaderHandle, plugin, "%s", buf);
}

/*
    A mod owns one folder inside Mods and everything under it. These answer
    for the mod whose Init is running: read them in Init and keep a copy of
    the strings.

        loaderModFolder     Mods\<mod>, where the mod's ini lives
        loaderLogsFolder    Mods\<mod>\Logs
        loaderCacheFolder   Mods\<mod>\Cache

    Logs and Cache exist before Init is called.
*/
static inline const char* loaderModFolder(void)
{
    return global_loaderModFolderFn
         ? global_loaderModFolderFn(global_loaderHandle) : "";
}

static inline const char* loaderCacheFolder(void)
{
    return global_loaderCacheFolderFn
         ? global_loaderCacheFolderFn(global_loaderHandle) : "";
}

/*
    Hooks the function carrying this signature and calls back on every entry.
    Several plugins may hook one function; the loader writes a single jump and
    calls each of them in the order they registered.

        name        the key the located address is cached under
        signature   the signature string the compiler wrote into the game
        callback    void (*)(void *registers, void *user)
        user        handed back to the callback untouched

    Answers 1 when the callback is registered, 0 when the loader is older than
    this feature or the function could not be hooked.
*/
static inline int loaderHookFunction(const char* name, const char* signature,
                                     LoaderHookCallback callback, void* user)
{
    return global_loaderHookFunctionFn
         ? global_loaderHookFunctionFn(global_loaderHandle, name, signature,
                                       callback, user) : 0;
}

/* One register of a hooked call, by the LOADER_REG_ numbers above. */
static inline uint64_t loaderHookRegister(void* registers, int which)
{
    return global_loaderHookRegisterFn
         ? global_loaderHookRegisterFn(global_loaderHandle, registers, which) : 0;
}

/*
    Writes one register of a hooked call, by the LOADER_REG_ numbers above.
    The value written is what the hooked function receives, and what the
    subscribers called after this one read. 0 where the loader predates this
    call or the register cannot be written.
*/
static inline int loaderHookSetRegister(void* registers, int which,
                                        uint64_t value)
{
    return global_loaderHookSetRegisterFn
         ? global_loaderHookSetRegisterFn(global_loaderHandle, registers,
                                          which, value) : 0;
}

/*
    Hooks an instruction rather than a function entry. address must be the
    first byte of an instruction. 0 where the loader predates this call or the
    instruction cannot be moved.
*/
static inline int loaderHookAddress(const char* name, uint64_t address,
                                    LoaderHookCallback callback, void* user)
{
    return global_loaderHookAddressFn
         ? global_loaderHookAddressFn(global_loaderHandle, name, address,
                                      callback, user) : 0;
}

/*
    The trampoline of a function the loader has hooked under this name: the
    bytes displaced from its opening, followed by a jump back past the patch.
    Cast it to a pointer with the game function's own signature and calling
    convention and call it with that function's arguments; it runs the whole
    original and returns to the caller, and no subscriber of the hook sees the
    call. 0 where the loader predates this call or has not hooked that name.
*/
static inline uint64_t loaderHookTrampoline(const char* name)
{
    return global_loaderHookTrampolineFn
         ? global_loaderHookTrampolineFn(global_loaderHandle, name) : 0;
}

/*
    For a plugin's own __except filter. Writes the caught fault into the
    loader's log:

        __except (loaderDescribeFault(GetExceptionInformation(), "the plugin",
                                      "what it was doing"))

    Answers EXCEPTION_EXECUTE_HANDLER whether or not the loader supplies it.
*/
static inline int loaderDescribeFault(void* pointers, const char* plugin,
                                      const char* during)
{
    if (global_loaderDescribeFaultFn)
        return global_loaderDescribeFaultFn(global_loaderHandle, pointers,
                                            plugin, during);
    return 1;   /* EXCEPTION_EXECUTE_HANDLER */
}

/*
    Every bounded call in this plugin reports instead of ending the process.
    strcpy_s, sprintf_s, strcat_s and the rest then return an error when what
    they are given does not fit, and the loader writes a line naming this
    plugin.

    Each plugin calls loaderWatchBadArguments for itself, from Init, after
    loaderBind.

    pluginName  what the line calls this plugin
*/
static const char* global_badArgumentPlugin = "a plugin";

static void narrowForLoader(char* out, size_t room, const wchar_t* text) {
    out[0] = 0;
    if (!text) return;
    size_t at = 0;
    for (; text[at] && at + 1 < room; ++at)
        out[at] = (text[at] > 0 && text[at] < 127) ? (char)text[at] : '?';
    out[at] = 0;
}

static void badArgumentSeen(const wchar_t* expression, const wchar_t* function,
                            const wchar_t* file, unsigned int line,
                            uintptr_t reserved)
{
    (void)reserved;
    if (!global_loaderReportBadArgumentFn) return;
    char saidWhat[256], inWhat[128], whichFile[260];
    narrowForLoader(saidWhat, sizeof(saidWhat), expression);
    narrowForLoader(inWhat, sizeof(inWhat), function);
    narrowForLoader(whichFile, sizeof(whichFile), file);
    global_loaderReportBadArgumentFn(global_loaderHandle,
                                     global_badArgumentPlugin, saidWhat,
                                     inWhat, whichFile, line);
}

static inline void loaderWatchBadArguments(const char* pluginName) {
    if (pluginName && *pluginName) global_badArgumentPlugin = pluginName;
    _set_invalid_parameter_handler(badArgumentSeen);
}

/* The address of the function carrying this signature, for calling rather
   than hooking it. 0 where the loader predates this call or the function was
   not found. */
static inline uint64_t loaderFindFunction(const char* name,
                                          const char* signature)
{
    return global_loaderFindFunctionFn
         ? global_loaderFindFunctionFn(global_loaderHandle, name, signature) : 0;
}

static inline void* loaderFindPattern(const char* pattern)
{
    return global_loaderFindPatternFn
         ? global_loaderFindPatternFn(global_loaderHandle, pattern) : 0;
}

static inline int loaderPatch(void* address, const void* bytes, size_t size,
                              const void* expect)
{
    return global_loaderPatchFn
         ? global_loaderPatchFn(global_loaderHandle, address, bytes, size, expect) : 0;
}

static inline void* loaderAllocNearGame(size_t size)
{
    return global_loaderAllocNearGameFn
         ? global_loaderAllocNearGameFn(global_loaderHandle, size) : 0;
}

static inline uint64_t loaderGameModuleBase(void)
{
    return global_loaderGameModuleBaseFn
         ? global_loaderGameModuleBaseFn(global_loaderHandle) : 0;
}

static inline uint64_t loaderGameModuleSize(void)
{
    return global_loaderGameModuleSizeFn
         ? global_loaderGameModuleSizeFn(global_loaderHandle) : 0;
}

static inline uint64_t loaderGameTextBase(void)
{
    return global_loaderGameTextBaseFn
         ? global_loaderGameTextBaseFn(global_loaderHandle) : 0;
}

static inline uint64_t loaderGameTextSize(void)
{
    return global_loaderGameTextSizeFn
         ? global_loaderGameTextSizeFn(global_loaderHandle) : 0;
}

static inline uint32_t loaderGameTimeDateStamp(void)
{
    return global_loaderGameTimeDateStampFn
         ? global_loaderGameTimeDateStampFn(global_loaderHandle) : 0;
}

static inline uint32_t loaderGameCheckSum(void)
{
    return global_loaderGameCheckSumFn
         ? global_loaderGameCheckSumFn(global_loaderHandle) : 0;
}

static inline const char* loaderGameExeName(void)
{
    return global_loaderGameExeNameFn
         ? global_loaderGameExeNameFn(global_loaderHandle) : 0;
}

/* index 0 to 3 for major, minor, build, revision; anything else gives 0. */
static inline uint32_t loaderGameFileVersion(int index)
{
    return global_loaderGameFileVersionFn
         ? global_loaderGameFileVersionFn(global_loaderHandle, index) : 0;
}

static inline const char* loaderGameFolder(void)
{
    return global_loaderGameFolderFn
         ? global_loaderGameFolderFn(global_loaderHandle) : 0;
}

static inline const char* loaderPluginsFolder(void)
{
    return global_loaderPluginsFolderFn
         ? global_loaderPluginsFolderFn(global_loaderHandle) : 0;
}

static inline const char* loaderLogsFolder(void)
{
    return global_loaderLogsFolderFn
         ? global_loaderLogsFolderFn(global_loaderHandle) : 0;
}

static inline const char* loaderConfigFolder(void)
{
    return global_loaderConfigFolderFn
         ? global_loaderConfigFolderFn(global_loaderHandle) : 0;
}

#endif /* TLOU_PLUGIN */

#ifdef __cplusplus
}
#endif
#endif
