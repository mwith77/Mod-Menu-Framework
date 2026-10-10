/*
    Mod Menu Framework.

    A .menupage file describes a page of settings. This plugin reads every one
    of them, builds a menu page for each, and gives the game a data source per
    page that answers what each row is and what it holds. Values are read from
    and written to the mod's own settings file.

    Folders, all under the game's Mods folder:

        Mod Menu Framework\mod_menu_framework.dll
        Mod Menu Framework\mod_menu_framework.ini
        Mod Menu Framework\mod_menu_framework.log (newest line first, at
                                                    most LOG_MOST_BYTES)
        Mod Menu Framework\Mod Menus\<name>.menupage
        Mod Menu Framework\Game Menus\<name>.menupage
        Mod Menu Framework\Game Menus\Menu Templates\<name>.json
*/

#include <windows.h>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* GetAsyncKeyState, GetForegroundWindow and GetWindowThreadProcessId, for a
   Keybind control's key in The Last of Us Part II. */
#pragma comment(lib, "user32.lib")
#include <cmath>
#include <ctime>
#include <malloc.h>

#undef near
#undef far

#define TLOU_PLUGIN
#include "tlou_plugin_sdk.h"

/*
    GUARD_EXCEPT_DESCRIBING writes a fault inside a guarded call into engine
    code into the loader's log, in the same form as one that ends the process.
    The handler runs whether or not the loader supplies the describer.
*/
#ifdef _MSC_VER
#define GUARD_TRY    __try
#define GUARD_EXCEPT __except (EXCEPTION_EXECUTE_HANDLER)
#define GUARD_EXCEPT_DESCRIBING(during) \
    __except (loaderDescribeFault(GetExceptionInformation(), pluginBaseName, \
                                  during))
#else
#define GUARD_TRY    if (1)
#define GUARD_EXCEPT else
#define GUARD_EXCEPT_DESCRIBING(during) else if ((void)(during), 0) {} else
#endif

static const char* const pluginBaseName = "mod_menu_framework";

/* This plugin's own folder. Mod Menus and Game Menus sit inside it. */
static char global_directory[MAX_PATH];
/* The folder holding every mod, which is the one above this plugin's own. A
   ConfigurationFile written as a path is relative to it. */
static char global_modsDirectory[MAX_PATH];
static char global_logPath[MAX_PATH];
static char global_iniPath[MAX_PATH];

/* ---------------------------------------------------------------- the log */

/*
    The log is one file, newest line first: each line is written above the
    lines already there, the earlier runs' included. It is at most
    LOG_MOST_BYTES; the oldest lines, at its end, are left out to keep it
    there. Each line is written through <log>.new, renamed over the log.
    Every line carries the machine's local date and time.
*/
#define LOG_MOST_BYTES (1024u * 1024u)
#define LOG_LINE_MOST  4096

static SRWLOCK global_logLock = SRWLOCK_INIT;

static void writeLogList(const char* fmt, va_list ap) {
    if (!global_logPath[0]) return;
    char line[LOG_LINE_MOST];
    SYSTEMTIME now;
    GetLocalTime(&now);
    int used = sprintf_s(line, "%04u-%02u-%02u %02u:%02u:%02u.%03u  ",
                         now.wYear, now.wMonth, now.wDay, now.wHour,
                         now.wMinute, now.wSecond, now.wMilliseconds);
    if (used < 0) return;
    const int text = _vsnprintf_s(line + used, sizeof(line) - used - 1,
                                  _TRUNCATE, fmt, ap);
    used = text < 0 ? (int)strlen(line) : used + text;
    line[used++] = '\n';

    AcquireSRWLockExclusive(&global_logLock);
    char* out = (char*)malloc(LOG_MOST_BYTES);
    if (out) {
        memcpy(out, line, (size_t)used);
        size_t total = (size_t)used;
        const HANDLE old = CreateFileA(global_logPath, GENERIC_READ,
                                       FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
        if (old != INVALID_HANDLE_VALUE) {
            DWORD got = 0;
            const DWORD room = (DWORD)(LOG_MOST_BYTES - total);
            if (ReadFile(old, out + total, room, &got, nullptr)) {
                LARGE_INTEGER size;
                if (GetFileSizeEx(old, &size) && size.QuadPart > (LONGLONG)got)
                    while (got > 0 && out[total + got - 1] != '\n') --got;
                total += got;
            }
            CloseHandle(old);
        }
        char fresh[MAX_PATH];
        if (sprintf_s(fresh, "%s.new", global_logPath) > 0) {
            const HANDLE file = CreateFileA(fresh, GENERIC_WRITE, 0, nullptr,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                            nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD wrote = 0;
                const BOOL ok = WriteFile(file, out, (DWORD)total, &wrote, nullptr)
                             && wrote == (DWORD)total;
                CloseHandle(file);
                if (ok) MoveFileExA(fresh, global_logPath, MOVEFILE_REPLACE_EXISTING);
                else DeleteFileA(fresh);
            }
        }
        free(out);
    }
    ReleaseSRWLockExclusive(&global_logLock);
}

static void writeLog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    writeLogList(fmt, ap);
    va_end(ap);
}

static void expressionLine(const char* fmt, ...);

/*
    Copies text into out, which has room bytes. Text that does not fit is cut
    short and a log line says so, naming the text by what. Text whose length a
    file decides is copied through here. capped writes that line as an
    expressionLine, which is capped and is not written while expressions are
    worked out quietly, for text cut again each time an expression or a
    dependency is worked out.
*/
static void copyBounded(char* out, size_t room, const char* text,
                        const char* what, int capped = 0) {
    if (!out || !room) return;
    out[0] = 0;
    if (!text) return;
    const size_t length = strlen(text);
    if (length < room) { memcpy(out, text, length + 1); return; }
    memcpy(out, text, room - 1);
    out[room - 1] = 0;
    (capped ? expressionLine : writeLog)(
        "MENU the %s is %u character(s) and there is room for %u, so it was "
        "cut short", what ? what : "text", (unsigned)length,
        (unsigned)(room - 1));
}

/* Formats into out. Answers 0 when the result does not fit in room, and out is
   then empty. */
static int formatInto(char* out, size_t room, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int written = _vsnprintf_s(out, room, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (written >= 0) return 1;
    out[0] = 0;
    return 0;
}

/* The first character of text above a space. */
static const char* pastBlanks(const char* text) {
    while (*text && (unsigned char)*text <= ' ') ++text;
    return text;
}

/* A reader's cursor moved past blanks, up to its end. */
template <class Cursor> static void skipBlanks(Cursor* c) {
    while (c->at < c->end && (unsigned char)*c->at <= ' ') ++c->at;
}

/* first and last moved inward past the blanks at either end. */
static void trimSpan(const char** first, const char** last) {
    while (*first < *last && (unsigned char)**first <= ' ') ++*first;
    while (*last > *first && (unsigned char)(*last)[-1] <= ' ') --*last;
}

static int letterOrDigit(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
        || (ch >= '0' && ch <= '9');
}

/* Whether word is spelled at from, ignoring case, before end. */
static int spelledAt(const char* from, const char* end, const char* word) {
    const size_t length = strlen(word);
    return (size_t)(end - from) >= length && _strnicmp(from, word, length) == 0;
}

/* --------------------------------------------------- the default settings */

/*
    The default settings are the RCDATA resource DEFAULT_SETTINGS: the
    plugin's ini as it stands in the project. Read as an ini, a line [name]
    opens a section, a line key = value below it is a setting of that
    section, and a line opening with ; is a comment. The blanks around a name,
    a key or a value are not part of it. Lines end in CR, LF or CR LF.
*/

/* The image this code is linked into; its address is its module handle. */
extern "C" IMAGE_DOS_HEADER __ImageBase;

/* The three bytes of a UTF-8 byte-order mark. */
static const char utf8ByteOrderMark[] = "\xEF\xBB\xBF";

/* The default settingIsPresent gives GetPrivateProfileStringA. No value in a
   settings file is this text. */
static const char missingSettingMark[] = "\x01missing\x01";

/*
    The RCDATA resource DEFAULT_SETTINGS of module, less a UTF-8 byte-order
    mark at its start: *length bytes, not ended by a zero. nullptr, with 0 in
    *length, where module holds no such resource or it holds nothing more.
*/
static const char* defaultSettingsText(HMODULE module, DWORD* length) {
    *length = 0;
    const HRSRC found = FindResourceA(module, "DEFAULT_SETTINGS",
                                      (LPCSTR)RT_RCDATA);
    const HGLOBAL loaded = found ? LoadResource(module, found) : nullptr;
    const char* text = loaded ? (const char*)LockResource(loaded) : nullptr;
    if (!text) return nullptr;
    DWORD size = SizeofResource(module, found);
    if (size >= 3 && memcmp(text, utf8ByteOrderMark, 3) == 0) {
        text += 3;
        size -= 3;
    }
    if (!size) return nullptr;
    *length = size;
    return text;
}

/*
    1 where the settings file at path holds key in section, whatever its
    value, an empty one included; 0 where Windows reports the key absent
    (ERROR_FILE_NOT_FOUND, which also stands for no such section and no such
    file); -1 where the file could not be read, for example while another
    program holds it open without read sharing (ERROR_SHARING_VIOLATION),
    with the error in GetLastError.
*/
static int settingIsPresent(const char* path, const char* section,
                            const char* key) {
    char held[sizeof(missingSettingMark) + 1];
    SetLastError(0);
    GetPrivateProfileStringA(section, key, missingSettingMark, held,
                             (DWORD)sizeof(held), path);
    const DWORD error = GetLastError();
    if (strcmp(held, missingSettingMark) != 0) return 1;
    if (error == ERROR_FILE_NOT_FOUND) return 0;
    SetLastError(error);
    return -1;
}

/* One setting of the default settings: its section, key and value, each
   without the blanks at its ends. */
struct DefaultSetting {
    char section[256];
    char key[256];
    char value[512];
};

/* The text from first up to last, without the blanks at its ends, copied
   into out with a zero after it. Answers 0, with out empty, where it does not
   fit in room. */
static int copyTrimmed(char* out, size_t room, const char* first,
                       const char* last) {
    while (first < last && (*first == ' ' || *first == '\t')) ++first;
    while (last > first && (last[-1] == ' ' || last[-1] == '\t')) --last;
    out[0] = 0;
    if ((size_t)(last - first) >= room) return 0;
    memcpy(out, first, (size_t)(last - first));
    out[last - first] = 0;
    return 1;
}

/*
    The next setting of the default settings text from *at up to end, read
    into setting, with *at moved past its line. Answers 0 once no setting is
    left. setting->section is empty before the first call and then holds the
    section last opened. A line with no =, a setting before the first section
    or below a [ line with no ], and a line too long for setting are passed
    over.
*/
static int nextDefaultSetting(const char** at, const char* end,
                              DefaultSetting* setting) {
    while (*at < end) {
        const char* first = *at;
        const char* last = first;
        while (last < end && *last != '\r' && *last != '\n') ++last;
        *at = last;
        while (*at < end && (**at == '\r' || **at == '\n')) ++*at;

        while (first < last && (*first == ' ' || *first == '\t')) ++first;
        if (first == last || *first == ';') continue;
        if (*first == '[') {
            const char* close = (const char*)memchr(first, ']',
                                                    (size_t)(last - first));
            if (!close
                || !copyTrimmed(setting->section, sizeof(setting->section),
                                first + 1, close))
                setting->section[0] = 0;
            continue;
        }
        const char* equals = (const char*)memchr(first, '=',
                                                 (size_t)(last - first));
        if (!setting->section[0] || !equals
            || !copyTrimmed(setting->key, sizeof(setting->key), first, equals)
            || !setting->key[0]
            || !copyTrimmed(setting->value, sizeof(setting->value), equals + 1,
                            last))
            continue;
        return 1;
    }
    return 0;
}

/*
    Adds to the settings file at path each setting of the default settings
    text (length bytes) that Windows reports absent from it, in its section
    with its default value, through WritePrivateProfileStringA. A setting the
    file holds, an empty value included, and every other line of the file
    are left as they are. Stops at the first setting that cannot be looked up
    or added, with one line in the log.
*/
static void addMissingSettings(const char* path, const char* text,
                               DWORD length) {
    const char* at = text;
    DefaultSetting setting = {};
    while (nextDefaultSetting(&at, text + length, &setting)) {
        const int present = settingIsPresent(path, setting.section,
                                             setting.key);
        if (present > 0) continue;
        if (present < 0) {
            const DWORD error = GetLastError();
            writeLog("SETTINGS %s could not be read (Windows error %lu), so "
                     "[%s] %s and the settings after it were not looked up, "
                     "and those it lacks take their default values", path,
                     error, setting.section, setting.key);
            return;
        }
        if (!WritePrivateProfileStringA(setting.section, setting.key,
                                        setting.value, path)) {
            const DWORD error = GetLastError();
            writeLog("SETTINGS [%s] %s could not be added to %s (Windows error "
                     "%lu), so it and the settings after it that the file "
                     "lacks take their default values", setting.section,
                     setting.key, path, error);
            return;
        }
    }
}

/*
    Gives the settings file at path the default settings built into module:
    - no file, or an empty one (0 bytes): the text whole, comments included,
      written to <path>.new, which is flushed, closed and moved to path. The
      move never replaces a file. An empty file is first deleted, through a
      handle no other may share, and only while that handle shows it empty;
      an empty file that cannot be deleted is left as it is. Where a file is
      at path at the time of the move, the .new file is deleted and the
      settings that file lacks are added instead.
    - a file that starts with a UTF-8 byte-order mark: nothing.
    - any other file: each setting it lacks, added by addMissingSettings.
    Each failure writes one line to the log; a .new file this wrote is
    deleted after one.
*/
static void writeDefaultSettings(HMODULE module, const char* path) {
    if (!path || !path[0]) return;
    DWORD length = 0;
    const char* const text = defaultSettingsText(module, &length);
    if (!text) {
        writeLog("SETTINGS the default settings are not built into this DLL, "
                 "so none were written to %s", path);
        return;
    }

    /* The file's size and first three bytes, read under a shared lock on the
       whole file, which waits while WritePrivateProfileStringA writes it. */
    int empty = 0;
    const HANDLE file = CreateFileA(path, GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE
                                        | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
            writeLog("SETTINGS %s could not be read (Windows error %lu), so "
                     "no settings were added to it, and those it lacks take "
                     "their default values", path, error);
            return;
        }
    } else {
        OVERLAPPED whole = {};
        LARGE_INTEGER size = {};
        char first[3] = {};
        DWORD got = 0;
        const BOOL locked = LockFileEx(file, 0, 0, MAXDWORD, MAXDWORD, &whole);
        const BOOL looked = locked && GetFileSizeEx(file, &size)
                            && ReadFile(file, first, (DWORD)sizeof(first),
                                        &got, nullptr);
        const DWORD error = looked ? ERROR_SUCCESS : GetLastError();
        if (locked) UnlockFileEx(file, 0, MAXDWORD, MAXDWORD, &whole);
        CloseHandle(file);
        if (!looked) {
            writeLog("SETTINGS %s could not be read (Windows error %lu), so "
                     "no settings were added to it, and those it lacks take "
                     "their default values", path, error);
            return;
        }
        if (size.QuadPart > 0) {
            if (got == sizeof(first)
                && memcmp(first, utf8ByteOrderMark, sizeof(first)) == 0)
                writeLog("SETTINGS %s starts with a UTF-8 byte-order mark, "
                         "which Windows reads as part of its first line, so no "
                         "settings were added to it", path);
            else
                addMissingSettings(path, text, length);
            return;
        }
        empty = 1;
    }

    char newPath[MAX_PATH];
    if (_snprintf_s(newPath, sizeof(newPath), _TRUNCATE, "%s.new", path) < 0) {
        writeLog("SETTINGS %s is too long a path to write the default "
                 "settings beside it", path);
        return;
    }
    const HANDLE out = CreateFileA(newPath, GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        writeLog("SETTINGS %s could not be made (Windows error %lu), so the "
                 "default settings were not written to %s", newPath, error,
                 path);
        return;
    }
    DWORD written = 0;
    DWORD error = ERROR_SUCCESS;
    if (!WriteFile(out, text, length, &written, nullptr)) error = GetLastError();
    else if (written != length) error = ERROR_WRITE_FAULT;
    else if (!FlushFileBuffers(out)) error = GetLastError();
    if (!CloseHandle(out) && error == ERROR_SUCCESS) error = GetLastError();

    if (error == ERROR_SUCCESS) {
        /* Why an empty file at path stays: ERROR_SUCCESS where it was
           deleted, is gone, or holds something by now. */
        DWORD kept = ERROR_SUCCESS;
        if (empty) {
            const HANDLE old = CreateFileA(path, DELETE | FILE_READ_ATTRIBUTES,
                                           0, nullptr, OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
            if (old == INVALID_HANDLE_VALUE) {
                if (GetLastError() != ERROR_FILE_NOT_FOUND) kept = GetLastError();
            } else {
                LARGE_INTEGER size = {};
                FILE_DISPOSITION_INFO gone = { TRUE };
                if (GetFileSizeEx(old, &size) && size.QuadPart == 0
                    && !SetFileInformationByHandle(old, FileDispositionInfo,
                                                   &gone, (DWORD)sizeof(gone)))
                    kept = GetLastError();
                CloseHandle(old);
            }
        }
        if (kept != ERROR_SUCCESS) {
            DeleteFileA(newPath);
            writeLog("SETTINGS %s is empty and could not be replaced (Windows "
                     "error %lu), so the default settings were not written to "
                     "it, and the settings take their default values", path,
                     kept);
            return;
        }
        if (MoveFileExA(newPath, path, 0)) return;
        error = GetLastError();
    }
    DeleteFileA(newPath);
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
        addMissingSettings(path, text, length);
        return;
    }
    writeLog("SETTINGS the default settings could not be written to %s "
             "(Windows error %lu), so the settings take their default values",
             path, error);
}

/* ------------------------------------------------------------ engine calls */

static const char* const listenerUpdateSignature =
    "void __cdecl ScriptManager::ListenerUpdate(void)";

/*
    jdom::ParseData(rcx the ValuePtr the result is written into, rdx the text,
                    r8 its length, r9 the allocator, null for the engine's
                    current memory scope)
*/
static const char* const parseDataSignature =
    "class jdom::Static::ValuePtr __cdecl jdom::ParseData(const char *,unsigned __int64,class HeapAllocator *)";

/* --------------------------------------------------- reading engine memory */

/* The lowest address treated as possibly a pointer, and the top of the user
   half of the address space. */
#define SMALLEST_POINTER 0x10000ull
#define LARGEST_POINTER  0x00007FFFFFFFFFFFull

static int isPointer(uint64_t address) {
    return address >= SMALLEST_POINTER && address <= LARGEST_POINTER;
}

static int readQword(uint64_t address, uint64_t* out) {
    if (!isPointer(address)) return 0;
    int ok = 0;
    GUARD_TRY { *out = *(const uint64_t*)address; ok = 1; }
    GUARD_EXCEPT { ok = 0; }
    return ok;
}

/* A byte or a dword, taken out of the aligned qword that holds it. */
static int readByte(uint64_t address, unsigned char* out) {
    uint64_t word = 0;
    if (!readQword(address & ~7ull, &word)) return 0;
    *out = (unsigned char)(word >> (8 * (address & 7)));
    return 1;
}

static int readDword(uint64_t address, uint32_t* out) {
    uint64_t word = 0;
    if (!readQword(address & ~7ull, &word)) return 0;
    *out = (uint32_t)(word >> (8 * (address & 7)));
    return 1;
}

/* Copies printable text from an address, leaving an empty string when the
   address does not hold any. */
static void copyText(char* out, size_t size, uint64_t address) {
    out[0] = 0;
    if (!isPointer(address)) return;
    GUARD_TRY {
        const char* s = (const char*)address;
        size_t i = 0;
        for (; i + 1 < size && s[i]; ++i) {
            if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e) {
                out[0] = 0;
                return;
            }
            out[i] = s[i];
        }
        out[i] = 0;
    } GUARD_EXCEPT { out[0] = 0; }
}

static uint64_t global_moduleBase = 0;
static uint64_t global_textBase = 0;
static uint64_t global_textSize = 0;

/* FNV-1a 64. Every name the engine keeps as an id is this hash of the name.
   folded hashes the text as if its letters A to Z were written in lower
   case. */
static uint64_t hashText(const char* text, int folded = 0) {
    uint64_t hash = 0xCBF29CE484222325ull;
    for (const unsigned char* at = (const unsigned char*)text; *at; ++at) {
        const unsigned char ch = folded && *at >= 'A' && *at <= 'Z'
                               ? (unsigned char)(*at - 'A' + 'a') : *at;
        hash ^= (uint64_t)ch;
        hash *= 0x100000001B3ull;
    }
    return hash;
}

typedef uint64_t (*WidgetOfWidgetFn)(uint64_t widget);

/* An engine function of one argument answering a qword, called where a fault
   is caught and answers 0. */
static uint64_t callEngine(uint64_t function, uint64_t argument,
                           const char* during) {
    uint64_t answer = 0;
    GUARD_TRY { answer = ((WidgetOfWidgetFn)function)(argument); }
    GUARD_EXCEPT_DESCRIBING(during) { answer = 0; }
    return answer;
}

/* ------------------------------------------------------ the game's builds */

/*
    Code of the game the framework compares before it uses it: the length
    bytes expected at rva. rva 0: none in that build.

    jumpLength, for code the framework hooks: how many of the opening bytes
    the loader replaces with its jump where another plugin has hooked the
    same place first (E9 and its 32-bit displacement, then 90 up to
    jumpLength); the bytes after them are still compared. 0 for code that is
    only called or compared.
*/
struct ExpectedCode {
    uint64_t       rva;
    const uint8_t* bytes;
    size_t         length;
    size_t         jumpLength;
};

struct TextLookupEnd;

/*
    What the framework needs of one build of the game, chosen by the
    executable's time stamp (installHooks). Every address is an rva.

        asComponent         (widget) -> it as a Menu2 component, or 0
        parentWidget        (widget) -> its parent widget
        handleResolve       compared only: a handle's widget, answered only
                            where the widget at rowId holds the handle's id
        firstChildWidget    compared only: passes over a child whose byte at
                            rowState has ROW_DEAD set
        focusHolder         compared only: (menu root) -> the component that
                            holds the focus, which hasFocus asks; rva 0 where
                            hasFocus reads the focus itself
        hasFocus            (component) -> a byte, not 0 where it holds the
                            menu's focus
        componentEvent      (component, event id, sixteen bytes*, sixteen
                            bytes*)
        resetPage           Page::ResetToDefaults(page), hooked
        sourcePool          the data source pool's descriptor
        textLookup          where the game's text lookup ends
        rowRecord, rowId, rowState
                            a widget's heap record, its id (32 bits), and the
                            byte whose bit ROW_DEAD is set once it is killed
        widgetGetDataSource, widgetRefreshSource
                            in a component's table: its data source (row),
                            and drawing again from it (row, 0)
        hiddenSlot to valuePointerSlot
                            the data source table's slots the framework
                            answers besides those the same in every build
                            (OUR_ITEM_DATA_SLOT and the others beside it)
        sliderTableSlots, selectorTableSlots
                            how many slots the slider's and the selector's
                            own tables hold
        safeFrameworkSources
                            how many settings of ours fit in the data source
                            pool as the game makes it; 0 where none is made
                            unless the pool is enlarged
        keybinds            1 where a Keybind control's key can be set from
                            its row: the keybind addresses (SEND_EVENT_RVA and
                            those beside it) are this build's. 0 where its row
                            only shows the key (keybindsCanBeSet)
*/
struct GameBuild {
    uint32_t             stamp;
    const char*          name;
    ExpectedCode         asComponent;
    ExpectedCode         parentWidget;
    ExpectedCode         handleResolve;
    ExpectedCode         firstChildWidget;
    ExpectedCode         focusHolder;
    ExpectedCode         hasFocus;
    ExpectedCode         componentEvent;
    ExpectedCode         resetPage;
    uint64_t             sourcePool;
    const TextLookupEnd* textLookup;
    unsigned             rowRecord;
    unsigned             rowId;
    unsigned             rowState;
    unsigned             widgetGetDataSource;
    unsigned             widgetRefreshSource;
    int                  hiddenSlot;
    int                  availabilitySlot;
    int                  labelSlot;
    int                  descriptionSlot;
    int                  dependencyTextSlot;
    int                  valueRangeSlot;
    int                  valuePointerSlot;
    int                  sliderTableSlots;
    int                  selectorTableSlots;
    int                  safeFrameworkSources;
    int                  keybinds;
};

/* The build installHooks found the game to be, or nullptr while the
   framework does nothing in the game. */
static const GameBuild* global_game = nullptr;

/* Whether a Keybind control's key can be set from its row in this build. Where
   it cannot, the row is a selector of one option, the key's name, and the
   framework never writes the key (keyShownOnly). */
static int keybindsCanBeSet() {
    return global_game && global_game->keybinds;
}

/* --------------------------------------------------------- json documents */

/*
    A parsed json document, the form a widget file takes once the engine has
    read it.

        +0x00  u8  kind     1 number, 2 float, 4 string, 5 array, 6 object. One
                            byte: the three bytes after it are not zero and must
                            not be read as part of it
        +0x04  u32 count
        +0x08  values       count qwords, one per entry
        +0x10  types        count bytes, the kind of each value
        +0x18  keys         count qwords of FNV-1a, null for an array

    A float is in the low four bytes of its value word. Keys are kept only as
    hashes.
*/
#define JDOM_ARRAY   5
#define JDOM_OBJECT  6

#define JDOM_COUNT_OFFSET  0x04
#define JDOM_VALUES_OFFSET 0x08
#define JDOM_TYPES_OFFSET  0x10
#define JDOM_KEYS_OFFSET   0x18

/* Bounds on a document walk. */
#define MAX_DOCUMENT_DEPTH   64
#define MAX_DOCUMENT_ENTRIES 262144

/* The longest widget file name, and the longest data source name, both as the
   engine writes them. */
#define MAX_WIDGET_NAME  96
#define MAX_SOURCE_NAME  128

/* ----------------------------------------------------------- the callbacks */

/* Every tick: the values put aside for a settings file its mod has written
   since the framework started are forgotten (forgetPutAsideOfWrittenFiles),
   and the keybinds' cells are kept (keybindTick). */
static void keybindTick(void);
static void forgetPutAsideOfWrittenFiles(void);

static void listenerUpdateHook(void*, void*) {
    forgetPutAsideOfWrittenFiles();
    keybindTick();
}

/* --------------------------------------------------- the widget file reader */

/*
    One function turns one jdom document into one widget file. Every menu page
    in the game is built by it.

        reader(rcx  the widget file object being filled in
               rdx  a sixteen byte scope object the caller builds
               r8   the file name, copied into the object with a 1023 byte bound
               r9   the document, a jdom collection pointer
               [rsp+0x28]  a byte flag, 1 from both known call sites)

    r9 is stored as a ValuePtr at file+0x408, that is the pointer, then a zero
    index, then a zero byte, and afterwards read only through the jdom
    accessors. Replacing r9 replaces the entire content of that one file and
    touches nothing else. r9 written with loaderHookSetRegister is what the
    reader receives.

    The reader is the only function in .text that calls
    Gui2::WidgetFile::LoadAnimations. It begins after the nearest run of int3
    padding in front of that call.

    A document the reader rejects is not a crash. It reports through its own
    strings and returns, leaving an empty file.
*/

static const char* const createDataSourceSignature =
    "class Menu2::IDataSource *__cdecl Menu2::DataSourceManager::CreateDataSource"
    "(const class StringId64,const class StringId64,const class StringId64,"
    "const class StringId64,unsigned __int64)";

static const char* const loadAnimationsSignature =
    "void __cdecl Gui2::WidgetFile::LoadAnimations(class jdom::Static::ValuePtr)";

#define READER_SEARCH_BACK  0x8000

/* The first rel32 call to target in .text at or after the byte offset from,
   or null. A byte scan: an E8 that is part of some other instruction counts
   as a call. */
static uint8_t* nextCallTo(uint64_t target, uint64_t from) {
    const uint8_t* text = (const uint8_t*)global_textBase;
    for (uint64_t i = from; global_textBase && i + 5 <= global_textSize; ++i) {
        if (text[i] != 0xE8) continue;
        int32_t relative = 0;
        memcpy(&relative, text + i + 1, 4);
        const uint64_t site = global_textBase + i;
        if ((uint64_t)((int64_t)site + 5 + relative) == target)
            return (uint8_t*)site;
    }
    return 0;
}

/* The only rel32 call to target in .text, or null when there is not exactly
   one. */
static uint8_t* soleCallSite(uint64_t target) {
    uint8_t* site = nextCallTo(target, 0);
    return site && !nextCallTo(target, (uint64_t)site - global_textBase + 1)
         ? site : 0;
}

/* The start of the function containing site: the byte after the nearest run of
   int3 padding in front of it. */
static uint8_t* functionStartBefore(uint8_t* site) {
    const uint8_t* floorAddress = (const uint8_t*)global_textBase;
    uint8_t* p = site;
    for (int back = 0; back < READER_SEARCH_BACK; ++back, --p) {
        if (p - 2 < floorAddress) return 0;
        if (p[-1] == 0xCC && p[-2] == 0xCC) return p;
    }
    return 0;
}

static uint64_t findWidgetFileReader() {
    const uint64_t animations = loaderFindFunction(
        "Gui2::WidgetFile::LoadAnimations", loadAnimationsSignature);
    if (!animations) {
        writeLog("FILE Gui2::WidgetFile::LoadAnimations was not found, so the "
                 "reader cannot be located");
        return 0;
    }
    uint8_t* site = soleCallSite(animations);
    if (!site) {
        writeLog("FILE Gui2::WidgetFile::LoadAnimations is not called exactly "
                 "once, so the reader cannot be located");
        return 0;
    }
    uint8_t* start = functionStartBefore(site);
    if (!start)
        writeLog("FILE no padding in front of the call to "
                 "Gui2::WidgetFile::LoadAnimations within %d bytes, so the "
                 "reader cannot be located", READER_SEARCH_BACK);
    return (uint64_t)start;
}

/*
    The engine's current memory scope, gs:[0x20] -> [0] -> [+0x38].
    jdom::ParseData allocates through it when it is passed a null allocator,
    and faults when it is null. It is live inside the reader and inside the
    registrar; on the engine tick it is null.
*/
static uint64_t engineScope() {
    const uint64_t block = __readgsqword(0x20);
    uint64_t first = 0, scope = 0;
    if (!block || !readQword(block, &first) || !first) return 0;
    if (!readQword(first + 0x38, &scope)) return 0;
    return scope;
}

typedef void (*ParseDataFn)(void* slot, const char* text, uint64_t length,
                            void* allocator);
static uint64_t global_parseData = 0;

/* --------------------------------------------------------------- our menus */

/*
    A widget file of our own, read from the plugin folder before the game builds
    its menus. The folders under the menu folder spell out the widget file's
    name:

        Mod Menus\mods\page-mod-config.json  ->  mods/page-mod-config

    A name the game already has is replaced when the reader asks for it. A name
    the game does not have is registered.
*/
#define MAX_MENU_FILES  256
#define MAX_MENU_BYTES  (10u * 1024u * 1024u)

struct MenuFile {
    char         name[MAX_SOURCE_NAME];   /* as the game names it, no extension */
    char*        text;
    uint32_t     length;
    volatile LONG used;                   /* the reader has been given this one */
    uint64_t     document;                /* the collection the parse produced */
    volatile LONG seen;                   /* the game asked for this name itself */
    volatile LONG registered;             /* we put this name in the table */
    LONG         attempts;
};

/*
    The slot jdom::ParseData writes its return value into. It must keep its
    room to spare and stand on its own, outside any structure. Files are handled
    one at a time on the loading thread, and one slot serves them all.
*/
static uint64_t global_parseSlot[16];

static MenuFile global_menuFiles[MAX_MENU_FILES];
static int      global_menuFileCount = 0;

/* Where the row that opens our page sits among the rows of the options page.
   Below zero leaves it where the page file puts it. The control named by
   rowPositionSetting holds it once the menupages have been read; the ini value
   stands until then. */
static int  global_menuRowPosition = -1;
static char global_rowPositionSetting[64];

static volatile LONG global_registerBusy = 0;

/* rcx at the registrar, the table every widget file lives in. */
static uint64_t global_widgetFileTable = 0;

/*
    A widget file's name as the game uses it: the path it was asked for without
    its .widget extension, "menu/page-options-v3". A file under the menu folder
    spells out the same name with its own folders.
*/
static void widgetNameOf(char* out, size_t size, const char* name) {
    strcpy_s(out, size, name);
    char* extension = strrchr(out, '.');
    if (extension && _stricmp(extension, ".widget") == 0) *extension = 0;
}

/*
    Thirteen key names are not in the executable. A document written out
    carries each as its own hash in hexadecimal, "478710ee764acc8e". After a
    parse, every key that is the hexadecimal of one of these is put back to the
    value it stands for.
*/
static const uint64_t unresolvedKeys[] = {
    0x37e257f4db3afff2ull,
    0x478710ee764acc8eull,
    0x685a017374c5cfe3ull,
    0x6f7cf7bfce77f9abull,
    0x78e2ab6dd7ac32eaull,
    0x8ee78b6fbee85971ull,
    0x95600c06e13dea40ull,
    0x9897ce4743a8f764ull,
    0x9897cf4743a8f917ull,
    0xba3f0cc4cd51d860ull,
    0xeb518f41717c03ffull,
    0xf11427bb583dc9ddull,
    0xf35e21dcdd8e242aull,
};
#define UNRESOLVED_KEY_COUNT \
    ((int)(sizeof(unresolvedKeys) / sizeof(unresolvedKeys[0])))

static uint64_t global_hexKeyHash[UNRESOLVED_KEY_COUNT];

static void prepareHexKeys() {
    for (int i = 0; i < UNRESOLVED_KEY_COUNT; ++i) {
        char text[17];
        sprintf_s(text, "%016llx", (unsigned long long)unresolvedKeys[i]);
        global_hexKeyHash[i] = hashText(text);
    }
}

static void restoreHexKeys(uint64_t collection, int depth) {
    if (depth > MAX_DOCUMENT_DEPTH || collection < SMALLEST_POINTER) return;

    uint32_t count = 0;
    uint64_t values = 0, types = 0, keys = 0;
    readDword(collection + JDOM_COUNT_OFFSET, &count);
    readQword(collection + JDOM_VALUES_OFFSET, &values);
    readQword(collection + JDOM_TYPES_OFFSET, &types);
    readQword(collection + JDOM_KEYS_OFFSET, &keys);
    if (count > MAX_DOCUMENT_ENTRIES) return;

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t hash = 0;
        if (keys >= SMALLEST_POINTER && readQword(keys + 8ull * i, &hash))
            for (int k = 0; k < UNRESOLVED_KEY_COUNT; ++k) {
                if (hash != global_hexKeyHash[k]) continue;
                GUARD_TRY {
                    *(uint64_t*)(keys + 8ull * i) = unresolvedKeys[k];
                } GUARD_EXCEPT { }
                break;
            }
        uint64_t value = 0;
        unsigned char type = 0;
        readQword(values + 8ull * i, &value);
        readByte(types + i, &type);
        if (type == JDOM_ARRAY || type == JDOM_OBJECT)
            restoreHexKeys(value, depth + 1);
    }
}

/*
    A whole file read into memory with a terminator after it, or null with a
    line in the log. tag begins the line and shown names the file in it. The
    caller frees what it answers.
*/
static char* readWholeFile(const char* path, uint32_t largest, const char* tag,
                           const char* shown, uint32_t* length) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        writeLog("%s could not open %s", tag, shown);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    const int fits = size > 0 && (uint32_t)size <= largest;
    char* text = fits ? (char*)malloc((size_t)size + 1) : 0;
    if (text) {
        *length = (uint32_t)fread(text, 1, (size_t)size, f);
        text[*length] = 0;
    } else if (!fits)
        writeLog("%s %s is %ld byte(s), which is outside 1 to %u", tag, shown,
                 size, largest);
    else
        writeLog("%s no memory for %s", tag, shown);
    fclose(f);
    return text;
}

/*
    Every .json file in folder and in the folders inside it, eight levels down
    at most, kept under prefix and its name without the extension, so that
    mods\page-x.json is kept as mods/page-x. A file or a folder whose full
    path, or whose name kept that way, does not fit is left out with a line: a
    name kept has room for the MAX_WIDGET_NAME - 1 characters of a widget
    file's name. top is what the log calls the folder first read. A line
    names the file or folder it is about by its path inside that folder,
    which is prefix and its own name, followed by " in " and top, as in
    "mods/page-x.json in Mod Menus"; the folder first read is named by top
    alone.
*/
static void readMenuFolder(const char* folder, const char* prefix, int depth,
                           const char* top) {
    if (depth > 8) return;

    char pattern[MAX_PATH];
    if (!formatInto(pattern, sizeof(pattern), "%s\\*", folder)) {
        writeLog("MENU %.*s%s%s: its full path is too long for Windows to list "
                 "the files in it, so the folder was not read",
                 prefix[0] ? (int)strlen(prefix) - 1 : 0, prefix,
                 prefix[0] ? " in " : "", top);
        return;
    }
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return;

    do {
        if (found.cFileName[0] == '.') continue;

        const int isFolder =
            (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const char* extension = strrchr(found.cFileName, '.');
        if (!isFolder && (!extension || _stricmp(extension, ".json") != 0))
            continue;

        char path[MAX_PATH];
        if (!formatInto(path, sizeof(path), "%s\\%s", folder,
                        found.cFileName)) {
            writeLog("MENU %s%s in %s: its full path is longer than the %d "
                     "characters Windows allows, so it was not read", prefix,
                     found.cFileName, top, MAX_PATH - 1);
            continue;
        }

        if (isFolder) {
            char inner[MAX_WIDGET_NAME];
            if (!formatInto(inner, sizeof(inner), "%s%s/", prefix,
                            found.cFileName)) {
                writeLog("MENU %s%s in %s: the names of the files inside it "
                         "would be longer than the %d characters a widget "
                         "file's name can have, so the folder was not read",
                         prefix, found.cFileName, top, MAX_WIDGET_NAME - 1);
                continue;
            }
            readMenuFolder(path, inner, depth + 1, top);
            continue;
        }

        if (global_menuFileCount >= MAX_MENU_FILES) {
            writeLog("MENU the table holds %d file(s), so %s%s in %s was not "
                     "read", MAX_MENU_FILES, prefix, found.cFileName, top);
            continue;
        }
        MenuFile* entry = &global_menuFiles[global_menuFileCount];
        memset(entry, 0, sizeof(*entry));
        if (!formatInto(entry->name, MAX_WIDGET_NAME, "%s%.*s", prefix,
                        (int)(extension - found.cFileName), found.cFileName)) {
            writeLog("MENU %s%s in %s: its name would be longer than the %d "
                     "characters a widget file's name can have, so it was not "
                     "read", prefix, found.cFileName, top, MAX_WIDGET_NAME - 1);
            continue;
        }
        char named[MAX_WIDGET_NAME + MAX_PATH * 2 + 8];
        formatInto(named, sizeof(named), "%s%s in %s", prefix, found.cFileName,
                   top);
        uint32_t length = 0;
        char* text = readWholeFile(path, MAX_MENU_BYTES, "MENU", named, &length);
        if (!text) continue;

        entry->text = text;
        entry->length = length;
        ++global_menuFileCount;
    } while (FindNextFileA(search, &found));
    FindClose(search);
}

/*
    Where the framework's own files live, under the plugin folder.

    Mod Menus                    a .menupage for each mod, and any widget file
                                 of ours
    Game Menus                   a .menupage that stands in for one of the
                                 game's
    Game Menus\Menu Templates    the widget file a generated page is built from

    A page read from Game Menus is one of the game's own pages rebuilt, and is
    not listed on the Mod Configuration page.
*/
#define MOD_MENU_FOLDER   "Mod Menus"
#define GAME_MENU_FOLDER  "Game Menus"
#define TEMPLATE_FOLDER   "Game Menus\\Menu Templates"
#define MENUPAGE_SUFFIX   ".menupage"

/* <plugin folder>\<leaf>, taken from the log path: the folder the log is
   in. */
static int pluginSubfolder(char* out, size_t size, const char* leaf) {
    if (!global_logPath[0]) return 0;
    strcpy_s(out, size, global_logPath);
    char* slash = strrchr(out, '\\');
    if (!slash) return 0;
    *slash = 0;
    strcat_s(out, size, "\\");
    strcat_s(out, size, leaf);
    return 1;
}

static void loadMenuFiles() {
    prepareHexKeys();
    char folder[MAX_PATH];
    if (pluginSubfolder(folder, sizeof(folder), MOD_MENU_FOLDER))
        readMenuFolder(folder, "", 0, MOD_MENU_FOLDER);
    if (pluginSubfolder(folder, sizeof(folder), TEMPLATE_FOLDER))
        readMenuFolder(folder, "", 0, TEMPLATE_FOLDER);
}

/* The file held under the game's own name for it, or nothing. */
static MenuFile* menuFileNamed(const char* name) {
    for (int i = 0; i < global_menuFileCount; ++i)
        if (_stricmp(global_menuFiles[i].name, name) == 0)
            return &global_menuFiles[i];
    return 0;
}

/* ------------------------------------------ the row on the options page */

/*
    The options page lists one button per row. Every one of them is an object
    whose source is menu/components/button-default.widget, and the row that
    opens our page is the one whose action names mods/page-mod-config.

    Both numbers that decide where a row sits are written in the file to a fixed
    width of ROW_NUMBER_WIDTH characters, and a new value is put over the old
    one in place. The file must keep its length.
*/
#define ROW_NUMBER_WIDTH 12
#define ROW_SPACING      126

static const char* const global_optionsPageName = "menu/page-options-v3";
static const char* const global_rowSource =
    "menu/components/button-default.widget";
static const char* const global_modRowAction = "mods/page-mod-config";

/* A number written over an existing one, keeping its width exactly. */
static void writeFixedNumber(char* at, double value) {
    char text[ROW_NUMBER_WIDTH + 8];
    const int whole = (int)value;
    int digits = whole < 0 ? 2 : 1;
    for (int n = whole < 0 ? -whole : whole; n >= 10; n /= 10) ++digits;
    const int decimals = ROW_NUMBER_WIDTH - digits - 1;
    sprintf_s(text, sizeof(text), "%.*f", decimals < 0 ? 0 : decimals, value);
    if ((int)strlen(text) == ROW_NUMBER_WIDTH) memcpy(at, text, ROW_NUMBER_WIDTH);
}

/* The value field of a key inside one object, or nothing. */
static char* valueField(char* from, char* end, const char* key) {
    const size_t keyLength = strlen(key);
    for (char* at = from; at + keyLength < end; ++at)
        if (memcmp(at, key, keyLength) == 0) return at + keyLength;
    return 0;
}

/* One past the '}' that closes the object starting at start, or nothing. */
static char* objectEnd(char* start, char* end) {
    int nest = 0, inString = 0;
    for (char* at = start; at < end; ++at) {
        if (inString) {
            if (*at == '\\') ++at;
            else if (*at == '"') inString = 0;
            continue;
        }
        if (*at == '"') inString = 1;
        else if (*at == '{') ++nest;
        else if (*at == '}' && --nest == 0) return at + 1;
    }
    return 0;
}

/* Just past the '[' that opens the file's item array, or nothing. */
static char* itemArrayAt(char* text, char* end) {
    for (char* at = text; (at = valueField(at, end, "\"items\":")) != 0; ) {
        while (at < end && (unsigned char)*at <= ' ') ++at;
        if (at < end && *at == '[') return at + 1;
    }
    return 0;
}

struct MenuObject {
    char*  start;        /* the '{' */
    char*  end;          /* one past the '}' */
    char*  parent;       /* the first byte of the parent value */
    size_t parentLength; /* its length, "null" included */
};

/* The value of a key inside one object, as the bytes that spell it. */
static int objectToken(const MenuObject* o, const char* key, char** at,
                       size_t* length) {
    char* found = valueField(o->start, o->end, key);
    if (!found) return 0;
    while (found < o->end && (unsigned char)*found <= ' ') ++found;
    char* past = found;
    if (past < o->end && *past == '"') {
        ++past;
        while (past < o->end && *past != '"') past += *past == '\\' ? 2 : 1;
        if (past < o->end) ++past;
    } else {
        while (past < o->end && *past != ',' && *past != '}'
               && (unsigned char)*past > ' ') ++past;
    }
    *at = found;
    *length = (size_t)(past - found);
    return 1;
}

#define MAX_MENU_OBJECTS 256

/* Every object of the file's item array, in the order it is written. */
static int readMenuObjects(char* text, uint32_t length, MenuObject* out,
                           int room) {
    char* end = text + length;
    char* at = itemArrayAt(text, end);
    int count = 0;
    while (at && count < room) {
        while (at < end && ((unsigned char)*at <= ' ' || *at == ',')) ++at;
        if (at >= end || *at != '{') break;
        char* past = objectEnd(at, end);
        if (!past) break;
        MenuObject* o = &out[count++];
        o->start = at;
        o->end = past;
        o->parent = 0;
        o->parentLength = 0;
        objectToken(o, "\"parent\":", &o->parent, &o->parentLength);
        at = past;
    }
    return count;
}

/*
    Where the row that opens our page sits among the rows of the options page.

    Gui2::Widget::UpdateLayout at 02552CE0 places the children of a container
    in the order the sibling list holds them, writing over each child's own
    pivotAs.y and not reading depth. A row is moved by moving its object inside
    the file's item array, and nothing else moves it. The two numbers are
    rewritten to agree with the new order.

    The object is moved with the comma that separates it from a neighbour; the
    array keeps its length.
*/
static int rowPositionNow();
static int pageIsClaimed(const char* name);

static void placeModRow(char* text, uint32_t length, int position) {
    if (!text || length < 16) return;

    MenuObject object[MAX_MENU_OBJECTS];
    const int count = readMenuObjects(text, length, object, MAX_MENU_OBJECTS);
    int modRow = 0;
    while (modRow < count
           && !valueField(object[modRow].start, object[modRow].end,
                          global_modRowAction)) ++modRow;
    if (modRow == count || !object[modRow].parent) {
        writeLog("MENU the options page holds %d object(s) and not the row that "
                 "opens our page, so it was left as it is", count);
        return;
    }
    const MenuObject* mod = &object[modRow];

    int sibling[MAX_MENU_OBJECTS], siblings = 0;
    for (int i = 0; i < count; ++i)
        if (i != modRow && object[i].parentLength == mod->parentLength
            && memcmp(object[i].parent, mod->parent, mod->parentLength) == 0)
            sibling[siblings++] = i;
    if (siblings < 1) {
        writeLog("MENU the row that opens our page has no row beside it, so it "
                 "was left as it is");
        return;
    }
    if (position < 0) position = 0;
    if (position > siblings) position = siblings;

    /* The object the row is put in front of, or -1 for the end of the array. */
    int anchor = -1;
    if (position < siblings) anchor = sibling[position];
    else if (sibling[siblings - 1] + 1 < count) anchor = sibling[siblings - 1] + 1;
    if (anchor == modRow || (anchor < 0 && modRow == count - 1)) return;

    /* Taken with the comma after it, or with the comma before it when the row
       is the last object the array holds. The objects are written one against
       the next, so the comma is the byte beside the brace. */
    const size_t objectLength = (size_t)(mod->end - mod->start);
    const size_t blockLength = objectLength + 1;
    char* block = (char*)malloc(blockLength);
    if (!block) return;
    char* takeFrom = mod->start;
    if (modRow == count - 1 ? takeFrom == text || takeFrom[-1] != ','
                            : mod->end[0] != ',') {
        writeLog("MENU the row that opens our page is not written against the "
                 "row %s it, so it was left as it is",
                 modRow == count - 1 ? "before" : "after");
        free(block);
        return;
    }
    if (modRow == count - 1) --takeFrom;
    memcpy(block, takeFrom, blockLength);
    char* const takeEnd = takeFrom + blockLength;
    char* to = anchor >= 0 ? object[anchor].start : object[count - 1].end;
    char* put = to;
    if (to > takeEnd) {
        memmove(takeFrom, takeEnd, (size_t)(to - takeEnd));
        put = to - blockLength;
    } else
        memmove(to + blockLength, to, (size_t)(takeFrom - to));
    /* The comma sits on whichever side the object was taken from, and goes back
       on the side the new place needs: in front of the object only when the
       object becomes the last the array holds. */
    const char* objectAt = block[0] == ',' ? block + 1 : block;
    if (anchor >= 0) {
        memcpy(put, objectAt, objectLength);
        put[objectLength] = ',';
    } else {
        put[0] = ',';
        memcpy(put + 1, objectAt, objectLength);
    }
    free(block);

    /* The two numbers, over again, in the order the rows now stand in. */
    const int again = readMenuObjects(text, length, object, MAX_MENU_OBJECTS);
    for (int i = 0, place = 0; i < again; ++i) {
        if (!valueField(object[i].start, object[i].end, global_rowSource))
            continue;
        char* d = valueField(object[i].start, object[i].end, "\"depth\":");
        char* y = valueField(object[i].start, object[i].end, "\"pivotAs.y\":");
        if (!d || !y) continue;
        writeFixedNumber(d, (double)place);
        writeFixedNumber(y, (double)(place * ROW_SPACING));
        ++place;
    }
}

/* Parse one of our files with the game's own parser. Returns the collection,
   or 0. */
static uint64_t parseMenuFile(MenuFile* menu, const char* widgetName) {
    if (!engineScope()) {
        writeLog("MENU the memory scope is null here, so \"%s\" was not parsed",
                 widgetName);
        return 0;
    }
    if (!global_parseData) {
        writeLog("MENU jdom::ParseData was not found, so \"%s\" was not parsed",
                 widgetName);
        return 0;
    }

    /* The row mover is for the game's own options page. A menupage that has
       taken that name draws the list itself, in the order its blocks ask for. */
    if (_stricmp(menu->name, global_optionsPageName) == 0
        && !pageIsClaimed(menu->name)) {
        const int position = rowPositionNow();
        if (position >= 0) placeModRow(menu->text, menu->length, position);
    }

    memset(global_parseSlot, 0, sizeof(global_parseSlot));
    int ok = 0;
    GUARD_TRY {
        ((ParseDataFn)global_parseData)(global_parseSlot, menu->text,
                                        menu->length, (void*)0);
        ok = 1;
    } GUARD_EXCEPT_DESCRIBING("parsing a widget file of our own") { ok = 0; }

    menu->document = global_parseSlot[0];
    if (!ok || !menu->document) {
        writeLog("MENU %s, %u byte(s), %s", menu->name, menu->length,
                 ok ? "gave no document when it was parsed"
                    : "faulted when it was parsed");
        return 0;
    }
    GUARD_TRY { restoreHexKeys(menu->document, 0); } GUARD_EXCEPT { }
    return menu->document;
}

static void widgetFileReadHook(void* registers, void*) {
    char name[MAX_WIDGET_NAME];
    copyText(name, sizeof(name), loaderHookRegister(registers, LOADER_REG_R8));
    char plain[MAX_SOURCE_NAME];
    widgetNameOf(plain, sizeof(plain), name);

    MenuFile* menu = menuFileNamed(plain);
    if (!menu) return;
    InterlockedExchange(&menu->seen, 1);
    if (InterlockedCompareExchange(&menu->used, 1, 0) != 0) return;

    const uint64_t document = parseMenuFile(menu, plain);
    if (document) loaderHookSetRegister(registers, LOADER_REG_R9, document);
}

/* ------------------------------------------------------------- menu pages */

/*
    A menupage file describes one menu page: which settings file it configures
    and one block per control. The page the game draws is generated from it.

    The whole grammar:

        file    key*
        key     Name ':' ( '[' value ']' | '{' block* '}' )
        block   '{' key* '}'

    A value runs to the ']' that closes it, counting nested brackets, so a
    description may hold any character and may span lines. Each line of a value
    has its leading and trailing whitespace removed; blank lines are kept. A
    value whose first character is '{' is a block, which is how a list item
    carries the control linked to it.

    Names are compared without case.
*/

static int textIsExpression(const char* text, int page);

#define MAX_MODPAGE_FILES     64
#define MAX_MODPAGE_ITEMS     2048
#define MAX_MODPAGE_OPTIONS   12
#define MODPAGE_NAME          64
#define MODPAGE_TITLE         96
#define MODPAGE_VALUE         64
#define MODPAGE_DEPENDENCY    256
#define MODPAGE_DESCRIPTION   1024

/*
    Room for a value written as an expression. A field that may carry one is
    sized for the expression, not for the answer. An expression cut short no
    longer parses.
*/
#define MODPAGE_EXPRESSION    2048
#define MAX_MODPAGE_BYTES     (10u * 1024u * 1024u)

/* Room for a setting's title, its option labels and values, its list value,
   the value its row shows and its default, and for its description and its
   dependency description. A text a block writes is cut to fit. */
#define MAX_SETTING_TEXT      128
#define MAX_DESCRIPTION_TEXT  1024

#define ITEM_NONE             0
#define ITEM_CONTROL          1
#define ITEM_SUBMENU          2
#define ITEM_LIST             3
#define ITEM_LIST_CONTROL     4
#define ITEM_LINKED           5
#define ITEM_PAGELINK         6

#define MODPAGE_TOGGLE        0
#define MODPAGE_SELECTOR      1
#define MODPAGE_SLIDER        2
#define MODPAGE_PRESET        3
#define MODPAGE_KEYBIND       4

/* Keys a block can take from its parent with InheritFromParent. */
#define INHERIT_TITLE         0x01
#define INHERIT_SECTION       0x02
#define INHERIT_DEFAULT       0x04
#define INHERIT_MARK          0x08
#define INHERIT_DEPENDENCY    0x10
#define INHERIT_DESCRIPTION   0x20
#define INHERIT_DEPENDENCY_DESCRIPTION 0x40
#define INHERIT_HTML          0x80

#define INHERIT_WORD  "InheritFromParent"

/* A character that makes a word part of something longer. */
static int joinsWord(char ch) {
    return letterOrDigit(ch) || ch == '_' || ch == '.';
}

/* Where the word stands on its own in text, or -1. */
static int wordStandsIn(const char* text, const char* word) {
    if (!text || !word || !*word) return -1;
    const size_t length = strlen(word);
    for (int i = 0; text[i]; ++i)
        if (_strnicmp(text + i, word, length) == 0
            && !(i > 0 && joinsWord(text[i - 1])) && !joinsWord(text[i + length]))
            return i;
    return -1;
}

/*
    label       what the option shows, where the menupage wrote it as text
    value       what the store keeps
    expression  the label's slot in global_labelExpression, where the menupage
                wrote it as an expression; 0 where it did not
*/
struct ModPageOption {
    char label[MODPAGE_TITLE];
    char value[MODPAGE_VALUE];
    int  expression;
};

struct ModPageItem {
    int   itemType;
    int   controlType;
    int   parent;          /* the block this one sits inside, -1 at page level */
    int   linked;          /* the control a list item carries, -1 for none */
    int   page;            /* which generated page draws this row */
    int   file;            /* which menupage file it came from */
    int   markAsModified;
    int   html;            /* markup as written: 1 yes, 0 no, -1 not said */
    int   openOverlay;     /* a page link opens its page over this one */
    int   pageIndex;       /* where the row draws, 0 when the file did not say */
    char  pageIndexSetting[MODPAGE_NAME]; /* the control that carries the index */
    int   inherited;       /* INHERIT_ bits that said InheritFromParent */
    /* The block was skipped when it was read, with a line in the log: it
       names no ItemType, or holds a key, an ItemType or a ControlType this
       format has no meaning for, or a key its ControlType does not take; it
       would make a control with an empty option or none to choose, a List
       inside a List, or a ListControl that adds nothing to its List; its
       ModPage is too long; or it is linked to a block that was skipped. It
       draws nothing and makes no setting, and no block inside it makes
       one. */
    int   refused;
    int   optionCount;
    float least, greatest, step;
    char  title[MODPAGE_EXPRESSION];
    char  section[MODPAGE_NAME];
    char  settingName[MODPAGE_NAME];
    char  listValue[MODPAGE_VALUE];
    char  fallback[MODPAGE_EXPRESSION];
    char  dependency[MODPAGE_DEPENDENCY];
    char  description[MODPAGE_EXPRESSION * 4];
    char  dependencyDescription[MODPAGE_DESCRIPTION];
    char  pageName[MODPAGE_NAME * 2];   /* the page a link opens, as the game names it */
    char  pageTextId[MODPAGE_NAME];     /* the game's own text id for the label */
    char  pageDataSource[MODPAGE_NAME]; /* what decides whether the row is shown */
    char  settingKey[MODPAGE_EXPRESSION]; /* SettingName, where it is
                                             an expression, not a name */
    /* SliderMinValue and SliderMaxValue where they are expressions rather
       than numbers, and ControlVisibility, SelectorCount and SelectorLabel.
       Empty where the file does not write one. */
    char  leastExpression[MODPAGE_EXPRESSION];
    char  greatestExpression[MODPAGE_EXPRESSION];
    char  visibility[MODPAGE_EXPRESSION];
    char  optionCountExpression[MODPAGE_EXPRESSION];
    char  optionLabelExpression[MODPAGE_EXPRESSION];
    /* SliderValueText: the text a slider shows for its value, an expression
       in which VALUE stands for the value. Empty shows the number. */
    char  valueText[MODPAGE_EXPRESSION];
    int   logarithmic;     /* SliderStepSize: [Logarithmic] */
    float smallestStep;    /* SliderSmallestStep, 0 where it is not written */
    /* A keybind: KeybindAllowMenuKeys and KeybindAllowGameplayKeys, whether
       it may take a key the game's controls bind to a menu action, and to an
       action outside the menus. 1 and 0 where the file does not say. */
    int   allowMenuKeys;
    int   allowGameplayKeys;
    /* Which of ToggleDisabledValue (1) and ToggleEnabledValue (2) the block
       gives with a label, as bits. One written with an empty label counts as
       not given, and is among emptyToggleKeys, as the same bits. */
    int   toggleKeys;
    int   emptyToggleKeys;
    /* A preset: its first preset in global_preset; its options are the
       presets, then the one that stands for settings that match none. */
    int   firstPreset;
    /* Its Off preset (PresetOff) in global_preset, plus 1; 0 for none. */
    int   offPreset;
    ModPageOption option[MAX_MODPAGE_OPTIONS];
};

struct ModPage {
    char name[MODPAGE_NAME];          /* the menupage file, without extension */
    char title[MODPAGE_TITLE];        /* what DefineMenuPage says */
    char settingsFile[MODPAGE_NAME * 2]; /* what ConfigurationFile says, which
                                            may be a path under Mods */
    char replacesPage[MODPAGE_NAME * 2]; /* the widget file this page is, when
                                            it stands in for one of the game's */
    char menuTemplate[MODPAGE_NAME];  /* the template in Menu Templates */
    int  ofTheGame;                   /* read from Game Menus */
    char storePath[MAX_PATH];
    int  firstItem, itemCount;
    /* The file was read with no fault and no block of it was skipped. */
    int  loadedWhole;
};

static ModPage     global_modPages[MAX_MODPAGE_FILES];
static int         global_modPageCount = 0;
static ModPageItem global_modItems[MAX_MODPAGE_ITEMS];
static int         global_modItemCount = 0;

/*
    What the pages read leave unknown about the settings the controls reach:
    global_reachUnknown is 1 where a menupage was not read; where a page,
    loaded or not, was read with a fault other than a block nested too deep
    and names no ConfigurationFile; where more pages were read but not loaded
    than global_refusedPagePath has room for; or where a control was left out
    for room. global_refusedPagePath holds the settings file of each page that
    was read but not loaded, empty for a page that names none.
*/
static int  global_reachUnknown = 0;
static char global_refusedPagePath[MAX_MODPAGE_FILES][MAX_PATH];
static int  global_refusedPageCount = 0;

/*
    A key kept as text: the field it goes in, the bit that says it was taken
    from the parent with InheritFromParent (0 where it cannot be), and what the
    log calls it when it is cut short. Up to three names fill the same field.
*/
struct TextKey {
    const char* name[3];
    size_t      offset;
    size_t      room;
    int         inherit;
    const char* what;
};

#define ITEM_FIELD(field) offsetof(ModPageItem, field), sizeof(ModPageItem::field)
#define PAGE_FIELD(field) offsetof(ModPage, field), sizeof(ModPage::field)

static const TextKey global_itemTextKey[] = {
    { { "ControlTitle", "SubmenuTitle", "PageTitle" }, ITEM_FIELD(title),
      INHERIT_TITLE, "title" },
    { { "Section" }, ITEM_FIELD(section), INHERIT_SECTION, "section" },
    { { "DefaultValue" }, ITEM_FIELD(fallback), INHERIT_DEFAULT, "default value" },
    { { "ControlDescription" }, ITEM_FIELD(description), INHERIT_DESCRIPTION,
      "description" },
    { { "ControlDependencyDescription", "ListDependencyDescription" },
      ITEM_FIELD(dependencyDescription), INHERIT_DEPENDENCY_DESCRIPTION,
      "dependency description" },
    { { "PageName" }, ITEM_FIELD(pageName), 0, "page name" },
    { { "PageIndexSetting" }, ITEM_FIELD(pageIndexSetting), 0,
      "page index setting" },
    { { "PageTextId" }, ITEM_FIELD(pageTextId), 0, "page text id" },
    { { "PageDataSource" }, ITEM_FIELD(pageDataSource), 0, "page data source" },
    { { "ListItemValue" }, ITEM_FIELD(listValue), 0, "list value" },
    { { "ControlVisibility" }, ITEM_FIELD(visibility), 0, "visibility" },
    { { "SelectorCount" }, ITEM_FIELD(optionCountExpression), 0, "option count" },
    { { "SelectorLabel" }, ITEM_FIELD(optionLabelExpression), 0, "option label" },
    { { "SliderValueText" }, ITEM_FIELD(valueText), 0, "slider value text" },
};

static const TextKey global_pageTextKey[] = {
    { { "DefineMenuPage" }, PAGE_FIELD(title), 0, "page title" },
    { { "ConfigurationFile" }, PAGE_FIELD(settingsFile), 0, "settings file name" },
    { { "ReplacesPage" }, PAGE_FIELD(replacesPage), 0, "replaced page name" },
    { { "MenuTemplate" }, PAGE_FIELD(menuTemplate), 0, "menu template name" },
};

/* The key of keys named name, or nothing. */
static const TextKey* textKeyNamed(const TextKey* keys, size_t count,
                                   const char* name) {
    for (size_t k = 0; k < count; ++k)
        for (const char* alias : keys[k].name)
            if (alias && !_stricmp(alias, name)) return &keys[k];
    return 0;
}

#define KEY_COUNT(keys) (sizeof(keys) / sizeof(keys[0]))

/*
    Every selector label a menupage writes as an expression, as written. Slot 0
    is never used.
*/
#define LABEL_EXPRESSION        768
#define MAX_LABEL_EXPRESSIONS   1024
static char global_labelExpression[MAX_LABEL_EXPRESSIONS][LABEL_EXPRESSION];
static int  global_labelExpressionCount = 1;

/*
    The formulas the header of a menupage defines. Every control in the same
    file can call one:

        Formula: [name(parameter, parameter) = expression]

    A call gives one value for each parameter, in order, and the expression is
    worked out with each parameter's name standing for its value. The header
    may hold any number of Formula keys, one formula each.
*/
#define MAX_PAGE_FORMULAS       256
#define MAX_FORMULA_PARAMETERS  8
#define FORMULA_TEXT            (MODPAGE_EXPRESSION * 4)

struct PageFormula {
    int  page;              /* the menupage file that defines it */
    int  parameterCount;
    char name[MODPAGE_NAME];
    char parameter[MAX_FORMULA_PARAMETERS][MODPAGE_NAME];
    char body[FORMULA_TEXT];
};

static PageFormula global_formula[MAX_PAGE_FORMULAS];
static int         global_formulaCount = 0;

/* The formula page defines under the name length characters long at name, or
   nothing. */
static const PageFormula* formulaNamed(int page, const char* name,
                                       size_t length) {
    if (page < 0 || !name || !length || length >= MODPAGE_NAME) return 0;
    for (int i = 0; i < global_formulaCount; ++i) {
        const PageFormula* f = &global_formula[i];
        if (f->page == page && strlen(f->name) == length
            && _strnicmp(f->name, name, length) == 0) return f;
    }
    return 0;
}

/* Words an expression reads before any name, which a formula, a parameter or
   a LET name therefore cannot be called. The first FUNCTION_WORDS are the
   functions, whose names make a value an expression. */
static const char* const global_reservedWord[] = {
    "IF", "IFS", "IFERROR", "CONCAT", "FRACTION", "LET", "CHOOSE", "SETTING",
    "STEPUP", "OPTION", "VALUE", "True", "False", "NOT", "AND", "NAND", "OR",
    "NOR", "XOR", "XNOR"
};
#define FUNCTION_WORDS 9

/* Whether the word length characters long at word is one of the first count
   reserved words. */
static int wordAmong(const char* word, size_t length, int count) {
    for (int i = 0; i < count; ++i)
        if (strlen(global_reservedWord[i]) == length
            && _strnicmp(global_reservedWord[i], word, length) == 0) return 1;
    return 0;
}

static int wordIsReserved(const char* word, size_t length) {
    return wordAmong(word, length, (int)KEY_COUNT(global_reservedWord));
}

/* Whether length characters at name may be what a formula, a parameter or a
   LET value is called. */
static int nameCanBeGiven(const char* name, size_t length) {
    return length && length < MODPAGE_NAME && !(*name >= '0' && *name <= '9')
        && !wordIsReserved(name, length);
}

/*
    Where the reader is up to, and the file it is reading, for the log.

    depth    how many blocks the reader is inside
    tooDeep  a block nested deeper than MODPAGE_DEPTH was found and stepped
             over unread, so the page is not kept
*/
struct ModPageCursor {
    const char* at;
    const char* end;
    const char* name;
    int         faults;
    int         depth;
    int         tooDeep;
};

static void modPageFault(ModPageCursor* c, const char* fmt, ...) {
    char what[MODPAGE_NAME + 128];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(what, sizeof(what), _TRUNCATE, fmt, ap);
    va_end(ap);
    ++c->faults;
    writeLog("MODPAGE %s: %s", c->name, what);
}

/* A name is letters and digits; the colon after it is eaten. */
static int readKeyName(ModPageCursor* c, char* out, size_t room) {
    skipBlanks(c);
    size_t at = 0;
    for (; c->at < c->end && letterOrDigit(*c->at); ++c->at)
        if (at + 1 < room) out[at++] = *c->at;
    out[at] = 0;
    if (!at) return 0;
    skipBlanks(c);
    if (c->at < c->end && *c->at == ':') { ++c->at; return 1; }
    return 0;
}

/*
    The text between '[' and the ']' that closes it. Each line is trimmed and
    the lines are joined with a newline.
*/
static void readBracketed(ModPageCursor* c, char* out, size_t room) {
    out[0] = 0;
    skipBlanks(c);
    if (c->at >= c->end || *c->at != '[') return;
    const char* const from = ++c->at;
    for (int depth = 1; c->at < c->end; ++c->at)
        if (*c->at == '[') ++depth;
        else if (*c->at == ']' && !--depth) break;
    const char* const to = c->at;
    if (c->at < c->end) ++c->at;

    size_t at = 0;
    for (const char* line = from;; ) {
        const char* lineEnd = line;
        while (lineEnd < to && *lineEnd != '\n') ++lineEnd;
        const char* first = line;
        const char* last = lineEnd;
        trimSpan(&first, &last);
        if (at && at + 1 < room) out[at++] = '\n';
        for (; first < last && at + 1 < room; ++first) out[at++] = *first;
        if (lineEnd >= to) break;
        line = lineEnd + 1;
    }
    out[at] = 0;

    /* A value written on one line comes back with no line at all before it. */
    while (at && out[at - 1] == '\n') out[--at] = 0;
    size_t start = 0;
    while (out[start] == '\n') ++start;
    if (start) memmove(out, out + start, strlen(out + start) + 1);
}

/*
    How deep blocks may nest. A block in a page's Content is one deep, and a
    block in the Content or ListItems of another, or linked to it, one deeper
    than that one. Each level takes the reader about 10 KB of stack, so the
    deepest page takes about 170 KB. A submenu is a block, so submenus nest no
    deeper than this either; generateOnePage takes about 11 KB for each, so
    drawing the pages of the deepest page takes about 210 KB.
*/
#define MODPAGE_DEPTH  16

/*
    Steps over the block at the cursor, from its '{' to the '}' that closes it,
    without reading it. A brace between '[' and the ']' that closes it belongs
    to a value and is not counted.
*/
static void stepOverBlock(ModPageCursor* c) {
    int braces = 0, brackets = 0;
    for (; c->at < c->end; ++c->at) {
        const char ch = *c->at;
        if (ch == '[') ++brackets;
        else if (ch == ']') { if (brackets) --brackets; }
        else if (brackets) continue;
        else if (ch == '{') ++braces;
        else if (ch == '}' && --braces <= 0) { ++c->at; return; }
    }
}

/*
    One level deeper, for the block at the cursor, before it is read or stepped
    over. Answers 1 where the block is within MODPAGE_DEPTH, and the caller
    takes the level off again once the block is done. Answers 0 where it is
    deeper: the level is not counted, the block has been stepped over unread
    and the page is marked to be refused, and the first such block of the page
    is logged.
*/
static int enterBlock(ModPageCursor* c) {
    if (c->depth < MODPAGE_DEPTH) {
        ++c->depth;
        return 1;
    }
    if (!c->tooDeep)
        modPageFault(c, "blocks are nested more than %d deep, so this page was "
                        "not loaded", MODPAGE_DEPTH);
    c->tooDeep = 1;
    stepOverBlock(c);
    return 0;
}

/*
    Stepping over what readBlockList and readBlock read, without keeping it:
    a value, from its '[' to the ']' that closes it or a linked block written
    '[{' ... '}]', a block, and a list of blocks. skipBlock counts the level of
    its block itself, with enterBlock. skipBlockList answers 1 where it
    reached the '}' that closes the list, and 0 where the file ends first or
    the list does not open with '{' or holds what is neither a block nor a
    key.
*/
static void skipBlock(ModPageCursor* c);

static void skipValue(ModPageCursor* c) {
    skipBlanks(c);
    if (c->at >= c->end || *c->at != '[') return;
    const char* after = c->at + 1;
    while (after < c->end && (unsigned char)*after <= ' ') ++after;
    if (after < c->end && *after == '{') {
        c->at = after;
        skipBlock(c);
        skipBlanks(c);
        if (c->at < c->end && *c->at == ']') ++c->at;
        return;
    }
    for (int depth = 0; c->at < c->end; ++c->at)
        if (*c->at == '[') ++depth;
        else if (*c->at == ']' && --depth == 0) { ++c->at; return; }
}

static int skipBlockList(ModPageCursor* c) {
    skipBlanks(c);
    if (c->at >= c->end || *c->at != '{') return 0;
    ++c->at;
    for (;;) {
        skipBlanks(c);
        if (c->at >= c->end) return 0;
        if (*c->at == '}') { ++c->at; return 1; }
        if (*c->at == '{') { skipBlock(c); continue; }
        char name[MODPAGE_NAME];
        if (!readKeyName(c, name, sizeof(name))) return 0;
        skipValue(c);
    }
}

static void skipBlock(ModPageCursor* c) {
    skipBlanks(c);
    if (c->at >= c->end || *c->at != '{' || !enterBlock(c)) return;
    ++c->at;
    for (;;) {
        skipBlanks(c);
        if (c->at >= c->end) break;
        if (*c->at == '}') { ++c->at; break; }
        char name[MODPAGE_NAME];
        if (!readKeyName(c, name, sizeof(name))) break;
        skipBlanks(c);
        if (!_stricmp(name, "Content") || !_stricmp(name, "ListItems"))
            skipBlockList(c);
        else
            skipValue(c);
    }
    --c->depth;
}

static int nameCharacter(char ch);

/* One Formula key's value, "name(parameter, ...) = expression", kept for the
   file being read, which is page. */
static void readFormula(ModPageCursor* c, int page, const char* text) {
    PageFormula f;
    memset(&f, 0, sizeof(f));
    f.page = page;

    const char* at = pastBlanks(text);
    /* Formula: [] defines nothing, and is not a fault. */
    if (!*at) return;
    const char* const nameFrom = at;
    while (*at && nameCharacter(*at)) ++at;
    const size_t nameLength = (size_t)(at - nameFrom);
    at = pastBlanks(at);
    if (!nameLength || *at != '(') {
        modPageFault(c, "a Formula begins with its name and then its parameters "
                        "in brackets, so it was not kept");
        return;
    }
    if (!nameCanBeGiven(nameFrom, nameLength)) {
        modPageFault(c, "a Formula cannot be called \"%.*s\", so it was not kept",
                     (int)(nameLength < 64 ? nameLength : 64), nameFrom);
        return;
    }
    memcpy(f.name, nameFrom, nameLength);
    if (formulaNamed(page, f.name, nameLength)) {
        modPageFault(c, "the Formula %s is defined twice; the first is kept",
                     f.name);
        return;
    }

    for (++at;;) {
        at = pastBlanks(at);
        if (*at == ')' && !f.parameterCount) { ++at; break; }
        const char* const parameterFrom = at;
        while (*at && nameCharacter(*at)) ++at;
        const size_t length = (size_t)(at - parameterFrom);
        if (!nameCanBeGiven(parameterFrom, length)) {
            modPageFault(c, "a parameter of the Formula %s is not a name it can "
                            "have, so the formula was not kept", f.name);
            return;
        }
        if (f.parameterCount >= MAX_FORMULA_PARAMETERS) {
            modPageFault(c, "the Formula %s takes more than %d parameters, so it "
                            "was not kept", f.name, MAX_FORMULA_PARAMETERS);
            return;
        }
        memcpy(f.parameter[f.parameterCount++], parameterFrom, length);
        at = pastBlanks(at);
        if (*at == ')') { ++at; break; }
        if (*at++ != ',') {
            modPageFault(c, "the parameters of the Formula %s are separated by "
                            "commas and closed with a bracket, so it was not "
                            "kept", f.name);
            return;
        }
    }
    at = pastBlanks(at);
    if (*at != '=') {
        modPageFault(c, "the Formula %s has no \"=\" after its parameters, so it "
                        "was not kept", f.name);
        return;
    }
    at = pastBlanks(at + 1);
    if (!*at) {
        modPageFault(c, "the Formula %s has no expression, so it was not kept",
                     f.name);
        return;
    }
    copyBounded(f.body, sizeof(f.body), at, "formula");
    if (global_formulaCount >= MAX_PAGE_FORMULAS) {
        writeLog("MODPAGE %s: there is room for %d formulas, so %s was not "
                 "kept", c->name, MAX_PAGE_FORMULAS, f.name);
        return;
    }
    global_formula[global_formulaCount++] = f;
}

/* A slider's limit: the number where the value is one, and otherwise the
   expression, worked out while the page is open. */
static void readLimit(const char* value, float* number, char* expression,
                      size_t room) {
    const char* at = pastBlanks(value);
    char* past = 0;
    const double read = strtod(at, &past);
    if (past && past > at && !*pastBlanks(past)) {
        *number = (float)read;
        expression[0] = 0;
        return;
    }
    copyBounded(expression, room, value, "slider limit");
}

/*
    Where the next character of stops that stands outside quotes and brackets
    is, or the end of the text. With last set, the last such character instead,
    or nothing where there is none.
*/
static const char* looseCharacter(const char* text, const char* stops,
                                  int last) {
    int nest = 0, quoted = 0;
    const char* found = 0;
    const char* at = text;
    for (; *at; ++at) {
        if (quoted) {
            if (*at == '"') quoted = 0;
            continue;
        }
        if (*at == '"') { quoted = 1; continue; }
        if (*at == '(') { ++nest; continue; }
        if (*at == ')') { if (nest) --nest; continue; }
        if (nest || !strchr(stops, *at)) continue;
        if (!last) return at;
        found = at;
    }
    return last ? found : at;
}

/* The text from first to last, trimmed and with a pair of quotes around it
   dropped, copied into out. */
static void copyUnquoted(const char* first, const char* last, char* out,
                         size_t room) {
    trimSpan(&first, &last);
    if (last > first && *first == '"' && last[-1] == '"') { ++first; --last; }
    size_t at = 0;
    for (; first < last && at + 1 < room; ++first) out[at++] = *first;
    out[at] = 0;
}

/* "OFF" = 0  ->  label OFF, value 0. Quotes around either side are dropped.
   The label ends at the last "=" outside quotes and brackets, so a label
   written as an expression may hold comparisons of its own. */
static void splitLabelledValue(const char* text, char* label, size_t labelRoom,
                               char* value, size_t valueRoom) {
    const char* equals = looseCharacter(text, "=", 1);
    const char* end = text + strlen(text);
    copyUnquoted(text, equals ? equals : end, label, labelRoom);
    value[0] = 0;
    if (equals) copyUnquoted(equals + 1, end, value, valueRoom);
}

/*
    One labelled value into one option. A label written in quotes is text. One
    without quotes whose first word names a function is an expression, worked
    out each time the option is shown; one that no longer fits among the
    expressions shows as written. Answers 1 where there was a label.
*/
static int readOption(const char* text, ModPageOption* into, int page) {
    const int quoted = *pastBlanks(text) == '"';
    char label[LABEL_EXPRESSION];
    splitLabelledValue(text, label, sizeof(label), into->value,
                       sizeof(into->value));
    into->expression = 0;
    if (!quoted && textIsExpression(label, page)) {
        if (global_labelExpressionCount < MAX_LABEL_EXPRESSIONS) {
            const int slot = global_labelExpressionCount++;
            copyBounded(global_labelExpression[slot], LABEL_EXPRESSION, label,
                        "label expression");
            into->expression = slot;
            into->label[0] = 0;
            return 1;
        }
        writeLog("MODPAGE there is room for %d label expressions, so \"%s\" "
                 "is shown as written", MAX_LABEL_EXPRESSIONS - 1, label);
    }
    copyBounded(into->label, sizeof(into->label), label, "option label");
    return into->label[0] != 0;
}

/* A comma separated run of labelled values, as a selector lists its options.
   A comma inside quotes or brackets belongs to the label. */
static int readOptions(const char* text, ModPageOption* into, int room,
                       int page) {
    int count = 0;
    for (const char* at = text; *at && count < room; ) {
        const char* comma = looseCharacter(at, ",", 0);
        char one[LABEL_EXPRESSION + MODPAGE_VALUE + 4];
        size_t length = (size_t)(comma - at);
        if (length >= sizeof(one)) length = sizeof(one) - 1;
        memcpy(one, at, length);
        one[length] = 0;
        if (readOption(one, &into[count], page)) ++count;
        if (!*comma) break;
        at = comma + 1;
    }
    return count;
}

static int itemTypeFromName(const char* text) {
    if (!_stricmp(text, "Control"))     return ITEM_CONTROL;
    if (!_stricmp(text, "Submenu"))     return ITEM_SUBMENU;
    if (!_stricmp(text, "List"))        return ITEM_LIST;
    if (!_stricmp(text, "PageLink"))    return ITEM_PAGELINK;
    if (!_stricmp(text, "ListControl")) return ITEM_LIST_CONTROL;
    return ITEM_NONE;
}

static int controlTypeFromName(const char* text) {
    if (!_stricmp(text, "Toggle"))   return MODPAGE_TOGGLE;
    if (!_stricmp(text, "Selector")) return MODPAGE_SELECTOR;
    if (!_stricmp(text, "Slider"))   return MODPAGE_SLIDER;
    if (!_stricmp(text, "Preset"))   return MODPAGE_PRESET;
    if (!_stricmp(text, "Keybind"))  return MODPAGE_KEYBIND;
    return -1;
}

/* What the log calls a block of controlType: the ControlType's own name, or
   "control" where it has none. */
static const char* nameOfControlType(int controlType) {
    switch (controlType) {
    case MODPAGE_TOGGLE:   return "Toggle";
    case MODPAGE_SELECTOR: return "Selector";
    case MODPAGE_SLIDER:   return "Slider";
    case MODPAGE_PRESET:   return "Preset";
    case MODPAGE_KEYBIND:  return "Keybind";
    default:               return "control";
    }
}

/*
    Presets: a selector whose options each set several settings at once.

        ControlType: [Preset]
        PresetValues: ["Default" = {corner = 3, size = 100, opacity = 70},
                       "Faint" = {size = 70, opacity = 30}]
        PresetCustomText: [Custom]
        PresetOff: ["OFF" = {corner = 3, size = 100, opacity = 70}]

    Each preset is a label and, in braces, the settings it sets: a name, read
    the way an expression reads one (a control of the page, or a key in its
    settings file, as key or section.key, a name without a section being read
    in the preset's own section), "=", and the value, in quotes where it holds
    a comma or a brace. The options are the presets in the order written, and
    after them one more, Custom unless PresetCustomText names it otherwise.

    A preset keeps no value of its own. The option it shows is the first preset
    all of whose settings hold its values, and Custom when none does. Choosing
    a preset sets its settings; leaving Custom for a preset first puts aside
    what every setting the presets name holds, and choosing Custom puts those
    values back. DefaultValue, where written, is the number of the preset that
    counts as its default, for MarkAsModified.

    PresetOff names one more preset, the settings' defaults, which is never
    offered: the last option shows its label in place of Custom while every
    setting it names holds its value, as Reset to Defaults leaves them. A
    preset chosen from there puts nothing aside, so Custom never brings Off
    back; only Reset to Defaults does. Where a preset has an Off, it is at its
    default while Off holds.
*/
#define MAX_PRESETS          256   /* presets in every menupage together */
#define MAX_PRESET_SETTINGS  32    /* settings one preset sets */
#define MAX_PRESET_NAMES     ((MAX_MODPAGE_OPTIONS - 1) * MAX_PRESET_SETTINGS)
#define MAX_PRESET_TEXT      (MODPAGE_EXPRESSION * 4)

struct PresetSetting {
    char name[MODPAGE_NAME * 2];
    char value[MODPAGE_VALUE];
};

struct Preset {
    int           item;
    char          label[MODPAGE_TITLE];
    int           count;
    PresetSetting setting[MAX_PRESET_SETTINGS];
};

static Preset global_preset[MAX_PRESETS];
static int    global_presetCount = 0;

/* The text from first to last, trimmed, with a pair of quotes around it
   dropped. */
static void copyPresetWord(const char* first, const char* last, char* out,
                           size_t room, const char* what) {
    while (first < last && (unsigned char)*first <= ' ') ++first;
    while (last > first && (unsigned char)last[-1] <= ' ') --last;
    if (last - first >= 2 && *first == '"' && last[-1] == '"') { ++first; --last; }
    char word[MAX_PRESET_TEXT];
    size_t at = 0;
    for (; first < last && at + 1 < sizeof(word); ++first) word[at++] = *first;
    word[at] = 0;
    copyBounded(out, room, word, what);
}

/* Where the next of stops outside quotes is, or last. */
static const char* presetStop(const char* at, const char* last,
                              const char* stops) {
    for (int quoted = 0; at < last; ++at) {
        if (*at == '"') quoted = !quoted;
        else if (!quoted && strchr(stops, *at)) return at;
    }
    return last;
}

/*
    PresetValues, into presets for the block item. Answers how many were read;
    a preset written wrongly is reported and left out, and the rest are read.
*/
static int readPresets(ModPageCursor* c, int item, const char* text) {
    int count = 0;
    const char* at = text;
    const char* const end = text + strlen(text);
    for (;;) {
        while (at < end && ((unsigned char)*at <= ' ' || *at == ',')) ++at;
        if (at >= end) break;
        const char* equals = presetStop(at, end, "={");
        if (equals >= end || *equals != '=') {
            modPageFault(c, "a preset is written \"label\" = {name = value, ...}; "
                            "the rest of PresetValues was not read");
            break;
        }
        const char* open = equals + 1;
        while (open < end && (unsigned char)*open <= ' ') ++open;
        const char* close = open < end && *open == '{'
                          ? presetStop(open + 1, end, "}") : end;
        if (open >= end || *open != '{' || close >= end) {
            modPageFault(c, "a preset's settings are written in braces after its "
                            "\"=\"; the rest of PresetValues was not read");
            break;
        }
        if (count >= MAX_MODPAGE_OPTIONS - 1 || global_presetCount >= MAX_PRESETS) {
            modPageFault(c, "there is room for %d presets on a control and %d in "
                            "all, so the rest were left out",
                         MAX_MODPAGE_OPTIONS - 1, MAX_PRESETS);
            break;
        }
        Preset* p = &global_preset[global_presetCount];
        memset(p, 0, sizeof(*p));
        p->item = item;
        copyPresetWord(at, equals, p->label, sizeof(p->label), "preset label");
        for (const char* one = open + 1; one < close; ) {
            const char* stop = presetStop(one, close, ",");
            const char* sign = presetStop(one, stop, "=");
            if (sign < stop) {
                if (p->count >= MAX_PRESET_SETTINGS) {
                    modPageFault(c, "the preset \"%s\" sets more than %d settings, "
                                    "so the rest were left out", p->label,
                                 MAX_PRESET_SETTINGS);
                    break;
                }
                PresetSetting* s = &p->setting[p->count];
                copyPresetWord(one, sign, s->name, sizeof(s->name),
                               "preset setting name");
                copyPresetWord(sign + 1, stop, s->value, sizeof(s->value),
                               "preset setting value");
                if (s->name[0]) ++p->count;
            } else {
                const char* word = one;
                while (word < stop && (unsigned char)*word <= ' ') ++word;
                if (word < stop)
                    modPageFault(c, "a setting of the preset \"%s\" has no \"=\", "
                                    "so it was left out", p->label);
            }
            one = stop < close ? stop + 1 : close;
        }
        if (!p->label[0] || !p->count)
            modPageFault(c, "a preset needs a label and at least one setting, so "
                            "\"%s\" was left out", p->label);
        else {
            ++global_presetCount;
            ++count;
        }
        at = close + 1;
    }
    return count;
}

static int readBlock(ModPageCursor* c, int parent, int page, int file);

/* Whether block i, or a block holding it, was skipped when it was read. */
static int blockIsSkipped(int i) {
    for (; i >= 0; i = global_modItems[i].parent)
        if (global_modItems[i].refused) return 1;
    return 0;
}

/* The blocks between '{' and its closing '}'. */
static void readBlockList(ModPageCursor* c, int parent, int page, int file) {
    skipBlanks(c);
    if (c->at >= c->end || *c->at != '{') {
        modPageFault(c, "a block was expected");
        return;
    }
    ++c->at;
    for (;;) {
        skipBlanks(c);
        if (c->at >= c->end) { modPageFault(c, "the file ends inside a block"); return; }
        if (*c->at == '}') { ++c->at; return; }
        if (*c->at == '{') {
            if (enterBlock(c)) {
                readBlock(c, parent, page, file);
                --c->depth;
            }
            continue;
        }
        /* A key among the blocks. Only ListDependency is read there. */
        char name[MODPAGE_NAME], value[MODPAGE_DEPENDENCY];
        if (!readKeyName(c, name, sizeof(name))) {
            modPageFault(c, "a block or a key was expected");
            return;
        }
        readBracketed(c, value, sizeof(value));
        if (parent >= 0 && !_stricmp(name, "ListDependency"))
            copyBounded(global_modItems[parent].dependency,
                        sizeof(global_modItems[parent].dependency), value,
                        "list dependency");
        else
            modPageFault(c, "a key was found where only blocks belong");
    }
}

/*
    How a block is named in the log: by its SettingName where the file writes
    it as a name rather than as an expression, or else by its title where the
    file writes the title as text rather than as an expression. The
    three parts are written one after another, as in
    " with SettingName \"" "brightness" "\"", and are all empty for a block
    named by neither.
*/
struct BlockName {
    const char* before;
    const char* name;
    const char* after;
};

static BlockName blockNameOf(const ModPageItem* block, int file) {
    if (block->settingName[0] && !block->settingKey[0])
        return { " with SettingName \"", block->settingName, "\"" };
    if (block->title[0] && !textIsExpression(block->title, file))
        return { " titled \"", block->title, "\"" };
    return { "", "", "" };
}

/* The names of the toggle keys keys holds as the bits of
   ModPageItem::toggleKeys: 1, 2 or both. */
static const char* toggleKeysNamed(int keys) {
    return keys == 1 ? "ToggleDisabledValue"
         : keys == 2 ? "ToggleEnabledValue"
                     : "ToggleDisabledValue and ToggleEnabledValue";
}

/*
    One block, from its '{' to its '}'. Returns the item it filled in, or -1.
    InheritFromParent is refused on ItemType, ControlType and SettingName.
    The block's level is counted by the caller, with enterBlock.
*/
static int readBlock(ModPageCursor* c, int parent, int page, int file) {
    skipBlanks(c);
    if (c->at >= c->end || *c->at != '{') { modPageFault(c, "a block was expected"); return -1; }
    ++c->at;

    if (global_modItemCount >= MAX_MODPAGE_ITEMS) {
        modPageFault(c, "there is room for no more controls");
        return -1;
    }
    /* The ItemType the block names, where it is not one this format has, and
       the length of a ModPage too long for a menupage's name, or 0. */
    char unknownType[MODPAGE_NAME] = "";
    int longModPage = 0;
    const int me = global_modItemCount++;
    ModPageItem* item = &global_modItems[me];
    memset(item, 0, sizeof(*item));
    item->parent = parent;
    item->linked = -1;
    item->page = page;
    item->file = file;
    item->controlType = -1;
    item->html = -1;
    item->step = 1.0f;
    item->allowMenuKeys = 1;
    item->allowGameplayKeys = 0;

    for (;;) {
        skipBlanks(c);
        if (c->at >= c->end) { modPageFault(c, "the file ends inside a block"); break; }
        if (*c->at == '}') { ++c->at; break; }

        char name[MODPAGE_NAME];
        if (!readKeyName(c, name, sizeof(name))) {
            modPageFault(c, "a key was expected");
            break;
        }
        skipBlanks(c);

        if (!_stricmp(name, "Content") || !_stricmp(name, "ListItems")) {
            readBlockList(c, me, page, file);
            continue;
        }

        /* A value that opens with a brace is the control linked to this one. */
        if (c->at < c->end && *c->at == '[') {
            const char* after = c->at + 1;
            while (after < c->end && (unsigned char)*after <= ' ') ++after;
            if (after < c->end && *after == '{') {
                c->at = after;
                if (enterBlock(c)) {
                    const int child = readBlock(c, me, page, file);
                    --c->depth;
                    if (child >= 0) {
                        global_modItems[me].linked = child;
                        global_modItems[child].itemType = ITEM_LINKED;
                    }
                }
                skipBlanks(c);
                if (c->at < c->end && *c->at == ']') ++c->at;
                continue;
            }
        }
        char value[MODPAGE_EXPRESSION * 4];
        readBracketed(c, value, sizeof(value));
        if (strlen(value) + 1 >= sizeof(value))
            modPageFault(c, "a value fills the whole of the room there is for "
                            "one, so it may have been cut short");

        /* An ItemType that is not one this format has, InheritFromParent
           among them, is kept for the line the end of the block writes. */
        if (!_stricmp(name, "ItemType")) {
            item->itemType = itemTypeFromName(value);
            formatInto(unknownType, sizeof(unknownType), "%.*s",
                       (int)sizeof(unknownType) - 1,
                       item->itemType == ITEM_NONE ? value : "");
            continue;
        }

        const int inherits = !_stricmp(value, INHERIT_WORD);
        /* In a dependency the word counts wherever it stands as a term of the
           expression. Everywhere else only the whole value counts. */
        const int isDependency = !_stricmp(name, "ControlDependency")
                              || !_stricmp(name, "ListDependency");
        const int mentions = isDependency
                          && wordStandsIn(value, INHERIT_WORD) >= 0;
        if (inherits && (!_stricmp(name, "ControlType")
                         || !_stricmp(name, "SettingName"))) {
            modPageFault(c, "%s asks to inherit, which is refused: it is what "
                            "makes the block what it is", name);
            continue;
        }
        if ((inherits || mentions) && parent < 0) {
            writeLog("MODPAGE %s: %s asks to inherit on a control with no parent, "
                     "which is wrong use of the argument; it is left empty",
                     c->name, name);
            continue;
        }

        const TextKey* key = textKeyNamed(global_itemTextKey,
                                          KEY_COUNT(global_itemTextKey), name);
        if (key) {
            if (inherits && key->inherit) item->inherited |= key->inherit;
            else copyBounded((char*)item + key->offset, key->room, value,
                             key->what);
        }
        else if (!_stricmp(name, "ControlType")) {
            item->controlType = controlTypeFromName(value);
            /* A row of a kind this framework does not draw would be drawn as
               a toggle with no options; the block goes instead. */
            if (item->controlType < 0) {
                item->refused = 1;
                writeLog("MODPAGE %s: ControlType \"%s\" is not one this "
                         "framework has, so the block holding it was skipped",
                         c->name, value);
            }
        }
        else if (!_stricmp(name, "ModPage")) {
            /* A menupage in the mod menu folder, by file name without its
               extension. The page it generates is named for the same stem,
               which has room for MODPAGE_NAME - 1 characters. */
            if (strlen(value) >= MODPAGE_NAME) {
                item->refused = 1;
                longModPage = (int)strlen(value);
                continue;
            }
            if (item->pageName[0])
                writeLog("MODPAGE %s: ModPage \"%s\" replaces the PageName "
                         "already given", c->name, value);
            sprintf_s(item->pageName, "mods/page-%s", value);
        }
        else if (!_stricmp(name, "PageIndex"))
            item->pageIndex = atoi(value);
        else if (!_stricmp(name, "OpenAs")) {
            item->openOverlay = !_stricmp(value, "Overlay");
            if (!item->openOverlay && _stricmp(value, "Fullscreen") != 0)
                writeLog("MODPAGE %s: OpenAs is \"%s\", which is neither "
                         "Fullscreen nor Overlay, so Fullscreen was used",
                         c->name, value);
        }
        else if (!_stricmp(name, "SettingName")) {
            /* An expression says which setting the control reads, and may
               change while the page is open. The row is then named
               control-<n> after the block's own number. */
            if (textIsExpression(value, file)) {
                copyBounded(item->settingKey, sizeof(item->settingKey), value,
                            "setting key");
                sprintf_s(item->settingName, "control-%d", me);
            } else
                copyBounded(item->settingName, sizeof(item->settingName), value,
                            "setting name");
        }
        else if (!_stricmp(name, "MarkAsModified")) {
            if (inherits) item->inherited |= INHERIT_MARK;
            else item->markAsModified = !_stricmp(value, "True");
        }
        else if (!_stricmp(name, "KeybindAllowMenuKeys")
                 || !_stricmp(name, "KeybindAllowGameplayKeys")) {
            int* allow = !_stricmp(name, "KeybindAllowMenuKeys")
                       ? &item->allowMenuKeys : &item->allowGameplayKeys;
            if (!_stricmp(value, "True")) *allow = 1;
            else if (!_stricmp(value, "False")) *allow = 0;
            else
                writeLog("MODPAGE %s: %s is \"%s\", which is neither True nor "
                         "False, so %s was used", c->name, name, value,
                         *allow ? "True" : "False");
        }
        else if (!_stricmp(name, "descriptionHTML")) {
            if (inherits) item->inherited |= INHERIT_HTML;
            else item->html = !_stricmp(value, "True");
        }
        else if (isDependency) {
            if (mentions) item->inherited |= INHERIT_DEPENDENCY;
            copyBounded(item->dependency, sizeof(item->dependency), value,
                        "dependency");
        }
        else if (!_stricmp(name, "SliderMinValue"))
            readLimit(value, &item->least, item->leastExpression,
                      sizeof(item->leastExpression));
        else if (!_stricmp(name, "SliderMaxValue"))
            readLimit(value, &item->greatest, item->greatestExpression,
                      sizeof(item->greatestExpression));
        else if (!_stricmp(name, "SliderStepSize")) {
            if (!_stricmp(value, "Logarithmic")) item->logarithmic = 1;
            else item->step = (float)atof(value);
        }
        else if (!_stricmp(name, "SliderSmallestStep"))
            item->smallestStep = (float)atof(value);
        else if (!_stricmp(name, "SelectorValues"))
            item->optionCount = readOptions(value, item->option,
                                            MAX_MODPAGE_OPTIONS, file);
        else if (!_stricmp(name, "PresetValues")) {
            item->firstPreset = global_presetCount;
            item->optionCount = readPresets(c, me, value);
        }
        else if (!_stricmp(name, "PresetOff")) {
            const int at = global_presetCount;
            if (readPresets(c, me, value) > 0) item->offPreset = at + 1;
        }
        else if (!_stricmp(name, "PresetCustomText"))
            /* Kept in the last option until the block ends, where the option
               after the presets takes it. */
            copyBounded(item->option[MAX_MODPAGE_OPTIONS - 1].label,
                        sizeof(item->option[0].label), value,
                        "preset custom text");
        else if (!_stricmp(name, "ToggleDisabledValue")
                 || !_stricmp(name, "ToggleEnabledValue")) {
            /* One that gives no label counts as not given. */
            const int enabled = !_stricmp(name, "ToggleEnabledValue");
            ModPageOption read;
            if (readOption(value, &read, file)) {
                item->option[enabled] = read;
                item->toggleKeys |= enabled ? 2 : 1;
                if (item->optionCount < 2) item->optionCount = 2;
            } else item->emptyToggleKeys |= enabled ? 2 : 1;
        }
        else {
            /* The block goes, not the key. Every other block on the page still
               loads. */
            item->refused = 1;
            writeLog("MODPAGE %s: \"%s\" is not a key this format has, so the "
                     "block holding it was skipped", c->name, name);
        }
    }

    if (longModPage) {
        const BlockName n = blockNameOf(item, file);
        writeLog("MODPAGE %s: an item%s%s%s has a ModPage of %d characters, "
                 "which is longer than the %d characters a menupage's name can "
                 "have, so the block holding it was skipped", c->name,
                 n.before, n.name, n.after, longModPage, MODPAGE_NAME - 1);
    }

    /* Every block names its ItemType, a linked one included: the block that
       carries a linked control marks it linked only after it has been read,
       so this is the ItemType the linked block names itself. A block already
       skipped is not named again. */
    if (item->itemType == ITEM_NONE && !item->refused) {
        item->refused = 1;
        const BlockName n = blockNameOf(item, file);
        if (!_stricmp(unknownType, INHERIT_WORD))
            writeLog("MODPAGE %s: an item%s%s%s asks to inherit its ItemType, "
                     "which cannot be inherited: it is what makes the block "
                     "what it is, so the block holding it was skipped",
                     c->name, n.before, n.name, n.after);
        else if (unknownType[0])
            writeLog("MODPAGE %s: an item%s%s%s has ItemType \"%s\", which "
                     "this format does not have, so the block holding it was "
                     "skipped", c->name, n.before, n.name, n.after,
                     unknownType);
        else
            writeLog("MODPAGE %s: an item%s%s%s has no ItemType, so the block "
                     "holding it was skipped", c->name, n.before, n.name,
                     n.after);
    }

    /* ToggleDisabledValue and ToggleEnabledValue belong to a Toggle, and to
       a block with no ControlType, which makes one. The ControlType may be
       written after them, so this is tested once the block is read. */
    if (item->toggleKeys && item->controlType >= 0
        && item->controlType != MODPAGE_TOGGLE && !item->refused) {
        item->refused = 1;
        const BlockName n = blockNameOf(item, file);
        writeLog("MODPAGE %s: a %s%s%s%s has %s, which only a Toggle takes, so "
                 "the block holding it was skipped", c->name,
                 nameOfControlType(item->controlType), n.before, n.name,
                 n.after, toggleKeysNamed(item->toggleKeys));
    }

    /* A preset's options: the presets, then the one for settings that match
       none. It is named for its block where its page does not name it. */
    if (item->itemType == ITEM_CONTROL && item->controlType == MODPAGE_PRESET
        && !item->refused) {
        const int presets = item->optionCount;
        if (presets <= 0) {
            item->refused = 1;
            const BlockName n = blockNameOf(item, file);
            writeLog("MODPAGE %s: a Preset%s%s%s lists no preset in "
                     "PresetValues, so the block holding it was skipped",
                     c->name, n.before, n.name, n.after);
        } else {
            char custom[MODPAGE_TITLE];
            copyBounded(custom, sizeof(custom),
                        item->option[MAX_MODPAGE_OPTIONS - 1].label[0]
                            ? item->option[MAX_MODPAGE_OPTIONS - 1].label
                            : "Custom", "preset custom text");
            for (int o = 0; o < presets; ++o) {
                copyBounded(item->option[o].label, sizeof(item->option[o].label),
                            global_preset[item->firstPreset + o].label,
                            "preset label");
                sprintf_s(item->option[o].value, "%d", o + 1);
                item->option[o].expression = 0;
            }
            copyBounded(item->option[presets].label,
                        sizeof(item->option[presets].label), custom,
                        "preset custom text");
            strcpy_s(item->option[presets].value,
                     sizeof(item->option[presets].value), "custom");
            item->option[presets].expression = 0;
            item->optionCount = presets + 1;
            if (!item->settingName[0])
                sprintf_s(item->settingName, "preset-%d", me);
        }
    }

    /*
        A toggle takes its options from ToggleDisabledValue and
        ToggleEnabledValue, both of them, and a selector from SelectorValues or
        SelectorCount; a block with no ControlType makes a toggle. A Preset
        takes them from PresetValues alone: SelectorCount would give it
        numbered options with no preset behind them. A toggle given only one of
        its two values, and a block that gets no option from these, would make
        a setting with an empty option or with nothing to choose, so the block
        goes.

        A List, a Submenu and a PageLink make no such setting. A control
        linked to this one with [{ }] makes one whichever ItemType it names (a
        missing or unknown one was refused when it was read), but is marked
        linked only after it has been read, so this block checks its linked
        controls again here. They were read after this block, so they follow
        it in global_modItems.
    */
    for (int k = me; k < global_modItemCount; ++k) {
        ModPageItem* block = &global_modItems[k];
        if (k != me && (block->parent != me || block->itemType != ITEM_LINKED))
            continue;
        if (block->refused || block->itemType == ITEM_LIST
            || block->itemType == ITEM_SUBMENU
            || block->itemType == ITEM_PAGELINK)
            continue;
        const int preset = block->controlType == MODPAGE_PRESET;
        const char* kind = nameOfControlType(block->controlType);
        const BlockName n = blockNameOf(block, file);
        /* A toggle key the block writes with an empty label is named as
           written so. */
        if ((block->controlType == MODPAGE_TOGGLE || block->controlType < 0)
            && (block->toggleKeys == 1 || block->toggleKeys == 2)) {
            block->refused = 1;
            const int missing = 3 - block->toggleKeys;
            const int empty = (block->emptyToggleKeys & missing) != 0;
            writeLog("MODPAGE %s: a %s%s%s%s %shas %s but %s%s%s, so the block "
                     "holding it was skipped", c->name, kind, n.before, n.name,
                     n.after,
                     block->controlType < 0 ? "has no ControlType, which makes "
                                              "it a Toggle, and " : "",
                     toggleKeysNamed(block->toggleKeys),
                     empty ? "writes " : "no ", toggleKeysNamed(missing),
                     empty ? " with an empty label" : "");
            continue;
        }
        if ((block->controlType >= 0 && block->controlType != MODPAGE_TOGGLE
             && block->controlType != MODPAGE_SELECTOR && !preset)
            || block->optionCount > 0
            || (block->optionCountExpression[0] && !preset))
            continue;
        block->refused = 1;
        const int empty = block->emptyToggleKeys;
        char toggles[160];
        if (!empty)
            formatInto(toggles, sizeof(toggles), "%sneither ToggleDisabledValue "
                       "nor ToggleEnabledValue",
                       block->controlType == MODPAGE_TOGGLE ? "has " : "");
        else
            formatInto(toggles, sizeof(toggles), "writes %s with an empty "
                       "label%s%s", toggleKeysNamed(empty),
                       empty == 3 ? "" : " and has no ",
                       empty == 3 ? "" : toggleKeysNamed(3 - empty));
        writeLog("MODPAGE %s: a %s%s%s%s %s%s, so the block holding it was "
                 "skipped", c->name, kind, n.before, n.name, n.after,
                 preset ? "lists no preset in PresetValues"
                 : block->controlType == MODPAGE_SELECTOR
                     ? "lists no option in SelectorValues and has no "
                       "SelectorCount"
                 : block->controlType == MODPAGE_TOGGLE ? ""
                 : "has no ControlType it can use, which makes it a Toggle, "
                   "and ",
                 preset || block->controlType == MODPAGE_SELECTOR ? ""
                                                                  : toggles);
    }

    /*
        A List draws a row for each of its own ListItems and none for the
        ListItems of a List among them, which would show as a row bound to
        that List. The blocks read inside a block at the top of the page are
        looked at once it is read, each after the block holding it, so that a
        List inside one already skipped is not named as well.
    */
    for (int k = me + 1; parent < 0 && k < global_modItemCount; ++k) {
        ModPageItem* block = &global_modItems[k];
        if (block->itemType != ITEM_LIST
            || global_modItems[block->parent].itemType != ITEM_LIST
            || blockIsSkipped(k))
            continue;
        block->refused = 1;
        const BlockName n = blockNameOf(block, file);
        writeLog("MODPAGE %s: a List%s%s%s is inside another List, which does "
                 "not draw the rows of a List inside it, so it was skipped",
                 c->name, n.before, n.name, n.after);
    }

    /* A ListControl adds to its List the value of the control linked to it,
       which a page writes as its ListItemValue: [{ ... }], or else its
       ListItemValue written as text. Where that control was skipped and no
       ListItemValue is written as text, it adds nothing. */
    if (item->itemType == ITEM_LIST_CONTROL && !item->refused
        && item->linked >= 0 && global_modItems[item->linked].refused
        && !item->listValue[0]) {
        item->refused = 1;
        const BlockName n = blockNameOf(item, file);
        writeLog("MODPAGE %s: a ListControl%s%s%s takes its ListItemValue from "
                 "a control that was skipped, so it would add nothing to its "
                 "List and was skipped", c->name, n.before, n.name, n.after);
    }

    /* The controls linked to a block that was skipped go with it, and so do
       the controls linked to them: every block after this one was read inside
       it, after the block it is linked to, so one pass reaches them all. */
    for (int k = me + 1; k < global_modItemCount; ++k) {
        ModPageItem* block = &global_modItems[k];
        if (block->itemType != ITEM_LINKED || block->refused
            || !global_modItems[block->parent].refused)
            continue;
        block->refused = 1;
        const BlockName n = blockNameOf(block, file);
        writeLog("MODPAGE %s: a control%s%s%s is linked to a block that was "
                 "skipped, so it was skipped too", c->name, n.before, n.name,
                 n.after);
    }
    return me;
}

/* What a block that asked to inherit takes from the one that holds it. */
static void settleInheritance() {
    for (int i = 0; i < global_modItemCount; ++i) {
        ModPageItem* item = &global_modItems[i];
        if (!item->inherited || item->parent < 0) continue;
        const ModPageItem* from = &global_modItems[item->parent];
        for (const TextKey& key : global_itemTextKey)
            if (item->inherited & key.inherit)
                strcpy_s((char*)item + key.offset, key.room,
                         (const char*)from + key.offset);
        if (item->inherited & INHERIT_MARK)
            item->markAsModified = from->markAsModified;
        if (item->inherited & INHERIT_HTML)
            item->html = from->html;
        /* The block's own dependency only. The list's reaches the control
           through the list it belongs to.

           Every standing of the word becomes the parent's condition in
           brackets. A parent with no condition of its own gives True. */
        if (!(item->inherited & INHERIT_DEPENDENCY)) continue;
        char made[MODPAGE_DEPENDENCY * 4];
        char taken[MODPAGE_DEPENDENCY + 4];
        if (from->dependency[0])
            sprintf_s(taken, "(%s)", from->dependency);
        else
            strcpy_s(taken, sizeof(taken), "True");
        const size_t word = strlen(INHERIT_WORD), takenLength = strlen(taken);
        const char* read = item->dependency;
        size_t at = 0;
        for (int found; (found = wordStandsIn(read, INHERIT_WORD)) >= 0;
             read += (size_t)found + word) {
            if (at + (size_t)found + takenLength + 1 > sizeof(made)) break;
            memcpy(made + at, read, (size_t)found);
            memcpy(made + at + found, taken, takenLength);
            at += (size_t)found + takenLength;
        }
        size_t rest = strlen(read);
        if (rest + 1 > sizeof(made) - at) rest = sizeof(made) - at - 1;
        memcpy(made + at, read, rest);
        made[at + rest] = 0;
        copyBounded(item->dependency, sizeof(item->dependency), made,
                    "dependency");
    }
}

/*
    Where the first unsafe '|' is, or -1 when there is none.

    A '|' in a string a menu displays is safe only when a second '|' closes it
    within 63 characters with no space between the two. An unsafe one ends the
    process when the string is displayed.
*/
static int fatalPipeAt(const char* text) {
    if (!text) return -1;
    for (int i = 0; text[i]; ++i) {
        if (text[i] != '|') continue;
        int closed = 0;
        for (int j = i + 1; text[j] && j - i - 1 <= 63; ++j) {
            if (text[j] == ' ' || text[j] == '\t' || text[j] == '\n'
                || text[j] == '\r') break;
            if (text[j] == '|') { closed = 1; i = j; break; }
        }
        if (!closed) return i;
    }
    return -1;
}

/*
    Where the first unsafe '<' is, or -1 when there is none.

    Text2::HtmlFragment::Parse reads '<' up to the next '>' as a tag. A tag it
    does not know that spans 127 characters or more ends the process. A '<'
    with no '>' after it is safe.
*/
#define LONGEST_TAG_SPAN  125

static int fatalTagAt(const char* text) {
    if (!text) return -1;
    for (int i = 0; text[i]; ++i) {
        if (text[i] != '<') continue;
        const char* close = strchr(text + i + 1, '>');
        if (!close) continue;
        if (close - text - i - 1 > LONGEST_TAG_SPAN) return i;
        i = (int)(close - text);
    }
    return -1;
}

static void escapeForMarkup(const char* in, char* out, size_t size);

/* What a line adds after a text it gives with "<", ">" and "&" written as
   entities. */
#define AS_ENTITIES " once \"<\", \">\" and \"&\" are written as entities"

/*
    markup says the text reaches the engine as it is written. A text that is
    not markup reaches it with its "<", ">" and "&" written as entities, and is
    checked so written; the line then gives it so written. cut says the text
    is what the file wrote cut to the room it has, which the line then says.
*/
static int textIsSafeToShow(const char* fileName, const char* where,
                            const char* what, const char* text, int markup,
                            int cut = 0) {
    const size_t room = strlen(text) * 5 + 1;
    char* escaped = markup ? nullptr : (char*)malloc(room);
    if (escaped) escapeForMarkup(text, escaped, room);
    const char* handed = escaped ? escaped : text;
    const int tag = markup ? fatalTagAt(handed) : -1;
    const int at = tag >= 0 ? -1 : fatalPipeAt(handed);
    if (tag >= 0)
        writeLog("MODPAGE %s: the %s of %s holds a \"<\" at byte %d whose \">\" "
                 "is more than %d characters away. The engine measures a tag it "
                 "does not know before putting it back and ends the process "
                 "outright when it is that long, so this page was not loaded",
                 fileName, what, where, tag, LONGEST_TAG_SPAN);
    else if (at >= 0)
        writeLog("MODPAGE %s: the %s of %s holds a \"|\" at byte %d that no "
                 "second \"|\" closes within 63 characters. The engine copies "
                 "what follows a \"|\" into a buffer of 64 bytes and ends the "
                 "process outright when it will not fit, so this page was not "
                 "loaded", fileName, what, where, at);
    if (tag >= 0 || at >= 0) {
        const int entities = escaped && strcmp(escaped, text) != 0;
        writeLog("MODPAGE %s: the text is \"%s\"%s%s%s", fileName, handed,
                 cut ? " once cut to the room it has" : "",
                 cut && entities ? " and" : "", entities ? AS_ENTITIES : "");
    }
    free(escaped);
    return tag < 0 && at < 0;
}

/*
    The text a control made from item shows under its description while its
    own dependency does not hold, into out: the dependency description written
    for it, or where none is written and it has a dependency, a sentence made
    from the dependency, and *ours is then 1. Empty where it has neither.
*/
static void dependencyTextOf(const ModPageItem* item, char* out, size_t room,
                             int* ours) {
    *ours = 0;
    if (item->dependencyDescription[0]) {
        copyBounded(out, room, item->dependencyDescription,
                    "dependency description");
        return;
    }
    out[0] = 0;
    if (!item->dependency[0]) return;
    /* The condition may hold "<", ">" and "&", which must reach the markup as
       entities. */
    char safe[MODPAGE_DEPENDENCY * 5 + 1];
    char made[sizeof(safe) + 64];
    escapeForMarkup(item->dependency, safe, sizeof(safe));
    sprintf_s(made, "To modify this setting, <b>%s</b>.", safe);
    copyBounded(out, room, made, "dependency description");
    *ours = 1;
}

/* Whether a dependency text can be drawn as lockedTextOf hands it to the
   engine. */
static int dependencyTextIsSafe(const char* text, int markup);

/* The block that gates block item where it is a List: the block holding it,
   or the one holding that where that is a ListControl, whatever its ItemType;
   -1 where it has no holder. The caller checks that it is a List. */
static int gatingBlockOf(int item) {
    int gating = global_modItems[item].parent;
    if (gating >= 0 && global_modItems[gating].itemType == ITEM_LIST_CONTROL)
        gating = global_modItems[gating].parent;
    return gating;
}

static int fatalTagWhenLockedAt(const char* handed, const char* dependency,
                                int markup);

/*
    Every string the page will display, checked before the page is kept, as a
    setting made from the block will hold it: a title or a description written
    as text cut to the room a setting has for it, and an expression as it is
    written. A Submenu's or a PageLink's title is never worked out, and cut
    that way it is what is drawn wherever the title is: the label of the row
    that opens its page, whether a setting of its own or the text database
    hands it over (buttonLabelOf), and the heading of the page a Submenu
    opens (generateOnePage), which pageHead writes as json. What a block
    inherits is settled later (settleInheritance), and
    makeSettingsFromModPages checks the descriptions again then; a Submenu
    or a PageLink whose title is InheritFromParent is checked here, where it
    draws that title, with the title it will be given, that of the nearest
    block holding it that does not inherit its title as well, which is in the
    same file. titleCut says the page's title is what DefineMenuPage wrote
    cut to the room it has.
*/
static int pageTextIsSafeToShow(const ModPage* page, const char* fileName,
                                int titleCut) {
    if (!textIsSafeToShow(fileName, "the page", "title", page->title, 1,
                          titleCut))
        return 0;
    for (int i = page->firstItem; i < page->firstItem + page->itemCount; ++i) {
        const ModPageItem* item = &global_modItems[i];
        /* For a Submenu or a PageLink, the nearest block whose title it
           draws; for any other block, the block itself. */
        const ModPageItem* titled = item;
        if (item->itemType == ITEM_SUBMENU || item->itemType == ITEM_PAGELINK)
            while ((titled->inherited & INHERIT_TITLE) && titled->parent >= 0)
                titled = &global_modItems[titled->parent];
        /* An inherited title is not drawn, and not checked, on a PageLink
           whose label the text database holds (PageTextId), or on a block
           that was skipped or sits inside one. */
        const int titleDrawn = titled == item
            || (!(item->itemType == ITEM_PAGELINK && item->pageTextId[0])
                && !blockIsSkipped(i));
        const char* where = item->settingName[0] ? item->settingName
                          : titled->title[0]     ? titled->title
                                                 : "a block with no name";
        const int markup = item->html != 0;
        const int titledAsText = !textIsExpression(item->title, item->file)
                              || item->itemType == ITEM_SUBMENU
                              || item->itemType == ITEM_PAGELINK;
        const int describedAsText = !textIsExpression(item->description,
                                                      item->file);
        char title[MAX_SETTING_TEXT], description[MAX_DESCRIPTION_TEXT];
        formatInto(title, sizeof(title), "%.*s", (int)sizeof(title) - 1,
                   titled->title);
        formatInto(description, sizeof(description), "%.*s",
                   (int)sizeof(description) - 1, item->description);
        if ((titleDrawn
             && !textIsSafeToShow(fileName, where,
                                  titled != item ? "inherited title" : "title",
                                  titledAsText ? title : item->title, 1,
                                  titledAsText
                                      && strlen(titled->title)
                                             >= sizeof(title)))
            || !textIsSafeToShow(fileName, where, "description",
                                 describedAsText ? description
                                                 : item->description, markup,
                                 describedAsText
                                     && strlen(item->description)
                                            >= sizeof(description))
            || !textIsSafeToShow(fileName, where, "dependency description",
                                 item->dependencyDescription, markup))
            return 0;
        /*
            A control's description written as markup is drawn with the text a
            lock adds after it while the control is locked: the text of its own
            dependency, or of the dependency of the List it is in.
        */
        const int gating = gatingBlockOf(i);
        const ModPageItem* lockedBy[2] = {
            item->dependency[0] ? item : nullptr,
            gating >= 0 && global_modItems[gating].itemType == ITEM_LIST
                && !global_modItems[gating].refused
                && global_modItems[gating].dependency[0]
                ? &global_modItems[gating] : nullptr };
        const int checked = markup && description[0] && !item->refused
                         && item->itemType != ITEM_LIST
                         && item->itemType != ITEM_SUBMENU
                         && item->itemType != ITEM_PAGELINK && describedAsText;
        for (const ModPageItem* by : lockedBy) {
            if (!checked || !by) continue;
            /* The text cut to the room a setting has for it. */
            char text[MODPAGE_DEPENDENCY * 5 + 65];
            int ours = 0;
            dependencyTextOf(by, text, sizeof(text), &ours);
            if (strlen(text) >= MAX_DESCRIPTION_TEXT)
                text[MAX_DESCRIPTION_TEXT - 1] = 0;
            const int textMarkup = by->html != 0 || ours;
            if (!dependencyTextIsSafe(text, textMarkup)) continue;
            const int at = fatalTagWhenLockedAt(description, text, textMarkup);
            if (at < 0) continue;
            writeLog("MODPAGE %s: the description of %s holds a \"<\" at byte "
                     "%d with no \">\" after it. While the control is locked "
                     "the engine draws the text it is locked with after the "
                     "description, and the first \">\" there is more than %d "
                     "characters away. The engine measures a tag it does not "
                     "know before putting it back and ends the process "
                     "outright when it is that long, so this page was not "
                     "loaded", fileName, where, at, LONGEST_TAG_SPAN);
            writeLog("MODPAGE %s: the text is \"%s\"%s", fileName, description,
                     strlen(item->description) >= sizeof(description)
                         ? " once cut to the room it has" : "");
            return 0;
        }
        for (int o = 0; o < item->optionCount && o < MAX_MODPAGE_OPTIONS; ++o)
            if (!item->option[o].expression
                && !textIsSafeToShow(fileName, where, "displayed value",
                                     item->option[o].label, 1)) return 0;
    }
    return 1;
}

/* Reads the menupage file fileName at path. folder is the folder it is in,
   as the lines about the file name it: Mod Menus or Game Menus. */
static void readModPageFile(const char* path, const char* fileName,
                            const char* folder) {
    if (global_modPageCount >= MAX_MODPAGE_FILES) {
        writeLog("MODPAGE there is room for %d page(s), so %s in %s was not "
                 "read", MAX_MODPAGE_FILES, fileName, folder);
        global_reachUnknown = 1;
        return;
    }
    /* The page keeps the file's name up to its last '.', and the pages
       generated for it are named from that. */
    const char* dot = strrchr(fileName, '.');
    const size_t nameLength = dot ? (size_t)(dot - fileName) : strlen(fileName);
    const size_t nameRoom = sizeof(global_modPages[0].name);
    if (nameLength >= nameRoom) {
        writeLog("MODPAGE %s in %s: the file's name without its extension is "
                 "%u character(s) and there is room for %u, so the file was "
                 "not read", fileName, folder, (unsigned)nameLength,
                 (unsigned)(nameRoom - 1));
        global_reachUnknown = 1;
        return;
    }
    char shown[MAX_PATH * 2];
    formatInto(shown, sizeof(shown), "%s in %s", fileName, folder);
    uint32_t length = 0;
    char* text = readWholeFile(path, MAX_MODPAGE_BYTES, "MODPAGE", shown,
                               &length);
    if (!text) {
        global_reachUnknown = 1;
        return;
    }

    ModPage* page = &global_modPages[global_modPageCount];
    memset(page, 0, sizeof(*page));
    memcpy(page->name, fileName, nameLength);
    page->name[nameLength] = 0;
    page->firstItem = global_modItemCount;

    ModPageCursor cursor = { text, text + length, page->name, 0 };

    /* Where the page's presets and label expressions begin, so that they can
       be taken back where the page is refused. */
    const int firstPreset = global_presetCount;
    const int firstLabelExpression = global_labelExpressionCount;

    /* The Formula keys are read before the blocks: whether a value is an
       expression depends on the names of the formulas. */
    for (;;) {
        skipBlanks(&cursor);
        if (cursor.at >= cursor.end || cursor.tooDeep) break;
        char name[MODPAGE_NAME];
        if (!readKeyName(&cursor, name, sizeof(name))) break;
        skipBlanks(&cursor);
        if (!_stricmp(name, "Content")) { skipBlockList(&cursor); continue; }
        if (_stricmp(name, "Formula") != 0) { skipValue(&cursor); continue; }
        char formula[FORMULA_TEXT];
        readBracketed(&cursor, formula, sizeof(formula));
        readFormula(&cursor, global_modPageCount, formula);
    }
    cursor.at = text;
    /* 1 where reading the Formula keys met a block nested too deep, which
       logged one fault: the page is not kept, and below its Content is
       stepped over and only the keys at the top of the file are read. A
       Content stepped over that is not closed is a fault of its own. */
    const int nestedTooDeep = cursor.tooDeep;

    /* Whether the title DefineMenuPage wrote was cut to fit. */
    int titleCut = 0;
    for (;;) {
        skipBlanks(&cursor);
        if (cursor.at >= cursor.end || (cursor.tooDeep && !nestedTooDeep))
            break;
        char name[MODPAGE_NAME];
        if (!readKeyName(&cursor, name, sizeof(name))) {
            modPageFault(&cursor, "a key was expected at the top of the file");
            break;
        }
        skipBlanks(&cursor);
        if (!_stricmp(name, "Content")) {
            if (!nestedTooDeep)
                readBlockList(&cursor, -1, global_modPageCount,
                              global_modPageCount);
            else if (!skipBlockList(&cursor))
                modPageFault(&cursor, cursor.at >= cursor.end
                                      ? "the file ends inside a block"
                                      : "a block or a key was expected");
            continue;
        }
        char value[MODPAGE_EXPRESSION * 4];
        readBracketed(&cursor, value, sizeof(value));
        const TextKey* key = textKeyNamed(global_pageTextKey,
                                          KEY_COUNT(global_pageTextKey), name);
        if (key) {
            /* A line about a value cut short names the file and its folder,
               as in "the page title of x.menupage in Mod Menus". */
            char what[sizeof(shown) + 32];
            formatInto(what, sizeof(what), "%s of %s", key->what, shown);
            copyBounded((char*)page + key->offset, key->room, value, what);
            if (key->offset == offsetof(ModPage, title))
                titleCut = strlen(value) >= key->room;
        }
        else if (_stricmp(name, "Formula") != 0)
            writeLog("MODPAGE %s: \"%s\" is not a key the top of a file has",
                     page->name, name);
    }

    page->itemCount = global_modItemCount - page->firstItem;
    /*
        ConfigurationFile names a file beside this plugin, or, when it carries
        a backslash, a path under Mods:

            ConfigurationFile: [Inventory and Pickups Modifier\its.ini]
    */
    if (page->settingsFile[0])
        sprintf_s(page->storePath, "%s\\%s",
                  strchr(page->settingsFile, '\\') ? global_modsDirectory
                                                   : global_directory,
                  page->settingsFile);
    free(text);

    /* Whether the reading may have stopped before the file's
       ConfigurationFile was read: a fault other than the one nestedTooDeep
       notes. */
    const int settingsFileUnknown = !page->settingsFile[0]
                                 && cursor.faults > nestedTooDeep;
    if (cursor.tooDeep || !pageTextIsSafeToShow(page, fileName, titleCut)) {
        if (settingsFileUnknown || global_refusedPageCount >= MAX_MODPAGE_FILES)
            global_reachUnknown = 1;
        else
            strcpy_s(global_refusedPagePath[global_refusedPageCount++],
                     MAX_PATH, page->storePath);
        global_modItemCount = page->firstItem;
        global_presetCount = firstPreset;
        global_labelExpressionCount = firstLabelExpression;
        int kept = 0;
        for (int i = 0; i < global_formulaCount; ++i)
            if (global_formula[i].page != global_modPageCount)
                global_formula[kept++] = global_formula[i];
        global_formulaCount = kept;
        memset(page, 0, sizeof(*page));
        return;
    }
    if (settingsFileUnknown) global_reachUnknown = 1;
    page->loadedWhole = !cursor.faults;
    for (int i = page->firstItem; i < global_modItemCount; ++i)
        if (global_modItems[i].refused) page->loadedWhole = 0;
    ++global_modPageCount;
}

/* Every menupage file of one folder. ofTheGame marks the pages read from Game
   Menus, which rebuild one of the game's own pages and are not listed. */
static void loadMenuPagesFrom(const char* folder, int ofTheGame) {
    char pattern[MAX_PATH];
    sprintf_s(pattern, "%s\\%s\\*%s", global_directory, folder,
              MENUPAGE_SUFFIX);
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        writeLog("MENUPAGE no %s file is in %s", MENUPAGE_SUFFIX, folder);
        return;
    }
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char path[MAX_PATH];
        if (!formatInto(path, sizeof(path), "%s\\%s\\%s", global_directory,
                        folder, found.cFileName)) {
            writeLog("MODPAGE %s in %s: its full path is longer than the %d "
                     "characters Windows allows, so the file was not read",
                     found.cFileName, folder, MAX_PATH - 1);
            global_reachUnknown = 1;
            continue;
        }
        const int before = global_modPageCount;
        readModPageFile(path, found.cFileName, folder);
        if (global_modPageCount > before)
            global_modPages[global_modPageCount - 1].ofTheGame = ofTheGame;
    } while (FindNextFileA(search, &found));
    FindClose(search);
}

static void loadModPages() {
    loadMenuPagesFrom(MOD_MENU_FOLDER, 0);
    loadMenuPagesFrom(GAME_MENU_FOLDER, 1);
    settleInheritance();
}

/* ------------------------------------------------------- generating a page */

/*
    A page is built from the widget file already in the template folder, which
    supplies everything that is not a row: the frame, the scroll container, the
    title and the info pane. Rows are written over the ones it carries.

    The template must keep its rows last: everything before the first row is
    copied as it stands and everything after the last one is discarded.
*/

#define PAGE_TEMPLATE_NAME "page-framework"
#define LANDING_PAGE_NAME  "mods/page-mod-config"
#define ROW_HEIGHT         118
/* How high a row is: the selector, slider and button rows the game's options
   pages draw are 58 high; the game's keybind rows are 150. The page lays its
   rows out one under another, so a taller row pushes the rest down. */
#define ROW_ITEM_HEIGHT     58
#define KEYBIND_ROW_HEIGHT  150
/*
    A row that opens a page takes its label from the cipherId on the page item,
    which the engine looks up in the text database when the row is built. A
    generated button labelled that way with text of ours is given a cipherId of
    its own, and the framework's store of texts answers the hash of that name
    with the label (textAnswered).
*/
static int storeText(uint64_t id, const char* text);

/* The label: a page's title, or a Submenu's or a PageLink's title cut to the
   room a setting has for a title. */
static void keepPageText(const char* cipherId, const char* text) {
    if (!cipherId || !cipherId[0] || !text || !text[0]) return;
    storeText(hashText(cipherId), text);
}

#define MAX_PAGE_BYTES     (10u * 1024u * 1024u)

#define BUTTON_WIDGET "menu/components/button-default.widget"

/* The row a keybind draws in, a widget file of ours: see keybindRowDocument. */
#define KEYBIND_ROW_NAME   "mods/keybind-row"
#define KEYBIND_ROW_WIDGET KEYBIND_ROW_NAME ".widget"
/* The event a keybind's row sends when it is chosen. */
#define KEYBIND_EVENT      "modmenu-keybind"

static const char* const global_rowWidget[5] = {
    "menu/components/selector-default_alternate.widget",
    "menu/components/selector-default.widget",
    "menu/components/slider-default.widget",
    "menu/components/selector-default.widget",   /* a preset */
    KEYBIND_ROW_WIDGET                             /* a keybind */
};

/* One row of a page, with the six things that differ between rows. */
static const char* const global_rowPattern =
    "{\"animation\":{\"keyframes\":{}},\"anchorMaxN.x\":1.0,\"anchorMaxN.y\":0.0,"
    "\"anchorMinN.x\":0.0,\"anchorMinN.y\":0.0,\"cipherId\":\"%s\","
    "\"class\":\"Reference\",\"color.a\":255,\"depth\":%d.0,\"focusable\":1,"
    "\"guid\":\"bac81ff305c5ff500b8207a6a116b7e0\",\"inheritAlpha\":1,"
    "\"inheritRotation\":1,\"inheritScale\":1,\"inheritTint\":0,\"isHoverable\":1,"
    "\"isMaskInverted\":0,\"isMaskOrClip\":0,\"layoutMode\":\"Unspecified\","
    "\"locked\":0,\"name\":\"%s\",\"noSpawn\":0,"
    "\"parent\":\"22f9fe5c15e3735b533166e08b913e39\",\"pivotAs.x\":0.0,"
    "\"pivotAs.y\":%d.0,\"pivotN.x\":0.0,\"pivotN.y\":0.0,"
    "\"rectSizeDelta.height\":%d.0,\"rectSizeDelta.width\":0.0,\"rotation\":0.0,"
    "\"scale.x\":1.0,\"scale.y\":1.0,\"source\":\"%s\",\"tags\":{%s},"
    "\"tint.blend\":0.0,\"tint.end\":4294967295,\"tint.start\":4294967295,"
    "\"visible\":1}";

/* A string as json: quotes and backslashes escaped, line breaks kept as \n,
   and the other characters below a space left out. */
static void writeJsonText(char* out, size_t room, const char* text) {
    size_t at = 0;
    for (const char* p = text; *p && at + 3 < room; ++p) {
        if (*p == '"' || *p == '\\') { out[at++] = '\\'; out[at++] = *p; }
        else if (*p == '\n') { out[at++] = '\\'; out[at++] = 'n'; }
        else if ((unsigned char)*p >= ' ') out[at++] = *p;
    }
    out[at] = 0;
}

/* The text from from to to in out, which holds *written bytes and has room,
   replaced with with, where the result fits. */
static void replaceSpan(char* out, uint32_t* written, uint32_t room, char* from,
                        const char* to, const char* with) {
    const uint32_t before = (uint32_t)(from - out);
    const uint32_t after = *written - (uint32_t)(to - out);
    const uint32_t length = (uint32_t)strlen(with);
    if (before + length + after + 1 >= room) return;
    memmove(from + length, to, after + 1);
    memcpy(from, with, length);
    *written = before + length + after;
}

/*
    The part of the template before its first row, with the page's own title put
    in place of the template's. title holds at most MAX_SETTING_TEXT - 1
    characters. Returns how much was written, or 0.
*/
static uint32_t pageHead(const MenuFile* template_, const char* title, char* out,
                         uint32_t room) {
    const char* items = strstr(template_->text, "\"items\":[");
    if (!items) {
        writeLog("PAGE the template %s has no items", template_->name);
        return 0;
    }
    const char* at = items + 8;
    const char* end = template_->text + template_->length;
    const char* firstRow = nullptr;

    /* Every object of the array in turn, until one is a row. */
    while (at < end && !firstRow) {
        while (at < end && *at != '{' && *at != ']') ++at;
        if (at >= end || *at == ']') break;
        const char* objectAt = at;
        for (int depth = 0; at < end; ++at)
            if (*at == '{') ++depth;
            else if (*at == '}' && !--depth) { ++at; break; }
        const size_t length = (size_t)(at - objectAt);
        for (size_t i = 0; i + 8 < length; ++i) {
            if (memcmp(objectAt + i, "\"tags\":{", 8) != 0) continue;
            const char* tagsAt = objectAt + i + 8;
            if ((size_t)(tagsAt - objectAt) + 12 < length
                && (memcmp(tagsAt, "\"dataSource\"", 12) == 0
                    || memcmp(tagsAt, "\"action\"", 8) == 0))
                firstRow = objectAt;
            break;
        }
    }
    if (!firstRow) {
        writeLog("PAGE the template %s carries no row to copy", template_->name);
        return 0;
    }
    /* The comma that joined the row to what came before it goes as well. */
    while (firstRow > template_->text && firstRow[-1] != ',') --firstRow;
    if (firstRow > template_->text) --firstRow;

    uint32_t written = (uint32_t)(firstRow - template_->text);
    if (written + 1 >= room) return 0;
    memcpy(out, template_->text, written);
    out[written] = 0;

    /* The scrolling list is clipped to its own height: the unmasked scroll
       container is replaced with the masked one. */
    static const char unmasked[] =
        "menu/components/options-scroll-container_no-mask.widget";
    char* listAt = strstr(out, unmasked);
    if (listAt)
        replaceSpan(out, &written, room, listAt, listAt + sizeof(unmasked) - 1,
                    "menu/components/options-scroll-container.widget");

    /*
        The prompts along the bottom. Menu2::Page reads this tag and offers the
        reset to defaults on the menu-inspect button unless the page carries
        noResetToDefaults, and it only offers it while at least one control on
        the page is available and not at its default.
    */
    char* legendAt = strstr(out, "\"legendContents\":\"");
    char* legendEnd = legendAt ? strchr(legendAt + 18, '"') : 0;
    if (legendEnd)
        replaceSpan(out, &written, room, legendAt + 18, legendEnd,
                    "[menu-inspect]MENU_RESET_TO_DEFAULTS,[menu-back]MENU_BACK");

    /* The heading the page draws, which the template gives as plain text.
       shown has room for every character of title written as json, two
       bytes each at most, so title is never cut short there. */
    char* titleAt = strstr(out, "\"name\":\"menuTitle1\"");
    char* textAt = titleAt ? strstr(titleAt, "\"text\":\"") : 0;
    char* textEnd = textAt ? strchr(textAt + 8, '"') : 0;
    if (textEnd) {
        char shown[MAX_SETTING_TEXT * 2];
        writeJsonText(shown, sizeof(shown), title);
        replaceSpan(out, &written, room, textAt + 8, textEnd, shown);
    }
    return written;
}

/* A generated page put in the table under a name of its own. */
static void keepGeneratedPage(const char* name, char* text, uint32_t length) {
    MenuFile* entry = menuFileNamed(name);
    if (entry)
        free(entry->text);
    else if (global_menuFileCount >= MAX_MENU_FILES) {
        writeLog("PAGE the table holds %d file(s), so %s was not added",
                 MAX_MENU_FILES, name);
        free(text);
        return;
    } else {
        entry = &global_menuFiles[global_menuFileCount++];
        memset(entry, 0, sizeof(*entry));
        strcpy_s(entry->name, sizeof(entry->name), name);
    }
    entry->text = text;
    entry->length = length;
    entry->document = 0;
    entry->used = 0;
    entry->registered = 0;
    entry->attempts = 0;
}

/* One row put on the end of a page being built, with r rows above it.
   Answers 0 where it does not fit. */
static int appendRow(char* text, uint32_t* at, const char* cipherId, int r,
                     const char* rowName, const char* widget, const char* tags,
                     int height = ROW_ITEM_HEIGHT) {
    char row[2048];
    const int wrote = sprintf_s(row, sizeof(row), global_rowPattern, cipherId,
                                r, rowName, r * ROW_HEIGHT, height, widget, tags);
    if (wrote <= 0 || *at + (uint32_t)wrote + 4 >= MAX_PAGE_BYTES) return 0;
    text[(*at)++] = ',';
    memcpy(text + *at, row, (size_t)wrote);
    *at += (uint32_t)wrote;
    return 1;
}

/* The page closed and kept under name. */
static void closePage(const char* name, char* text, uint32_t at) {
    text[at++] = ']';
    text[at++] = '}';
    text[at] = 0;
    keepGeneratedPage(name, text, at);
}

/* The tags of a row that opens a page: how, the page, and the data source
   that labels the row where there is one. */
static void openingTags(char* tags, size_t room, const char* how,
                        const char* target, const char* source) {
    if (source)
        sprintf_s(tags, room, "\"action\":\"%s %s\",\"allowTtsOnFocus\":\"true\","
                              "\"dataSource\":\"%s\"", how, target, source);
    else
        sprintf_s(tags, room, "\"action\":\"%s %s\",\"allowTtsOnFocus\":\"true\"",
                  how, target);
}

static int settingValueByName(const char* name, int fallback);

/*
    A data source for one row that opens a page, handing its label over as a
    plain string the way a control's title is. Answers whether one was made; a
    row without one falls back to its cipherId and the text database. onPage
    names the page the row is on, by its heading or, where it has none, its
    name; the log names the button by its title and onPage. entryRow says the
    row is on a page of the game and opens the landing page.
*/
static int makeButtonSetting(const char* sourceName, const char* title,
                             const char* onPage, int entryRow = 0);

/*
    The label of the row a Submenu or a PageLink draws, into label: its title
    cut to the room a setting has for a title, the text pageTextIsSafeToShow
    checked. Where it was cut, one line says so, naming the button by that
    text and by onPage, as makeButtonSetting takes it.
*/
static void buttonLabelOf(const ModPageItem* item, const char* onPage,
                          char (&label)[MAX_SETTING_TEXT]) {
    formatInto(label, sizeof(label), "%.*s", (int)sizeof(label) - 1,
               item->title);
    const size_t length = strlen(item->title);
    if (length >= sizeof(label))
        writeLog("MENU the button \"%s\" on the page \"%s\" has a title of %u "
                 "character(s) and there is room for %u, so it was cut short%s",
                 label, onPage, (unsigned)length, (unsigned)(sizeof(label) - 1),
                 item->itemType == ITEM_SUBMENU
                     ? " on the button and as the heading of the page it opens"
                     : "");
}

/* Whether a menupage has taken a widget file's name for itself. */
static int pageIsClaimed(const char* name) {
    if (!name || !name[0]) return 0;
    for (int p = 0; p < global_modPageCount; ++p)
        if (_stricmp(global_modPages[p].replacesPage, name) == 0) return 1;
    return 0;
}

/*
    Where a block draws, doubled.

    A block that names a PageIndex draws at twice it, so every block the file
    places sits on an even number and the odd numbers between them are free. A
    block that names a PageIndexSetting takes its place from that control and
    sits at twice its value plus one, which is always one of those free odd
    numbers: a value of N puts the block directly after the block whose
    PageIndex is N, and a value of zero puts it above the first.

    A block that names neither keeps the place the file gave it, counting only
    the blocks that draw a row, so a file that indexes nothing draws in the
    order it is written.
*/
static int rowOrderOf(const ModPageItem* item, int fileRank) {
    if (item->pageIndexSetting[0])
        return settingValueByName(item->pageIndexSetting, 0) * 2 + 1;
    if (item->pageIndex > 0) return item->pageIndex * 2;
    return fileRank * 2;
}

/*
    Every row a page draws. A submenu is a button that opens a page of its own;
    a list draws no row itself, only the controls inside it; a linked control
    draws its own row under the one that carries it.

    The blocks of the page are put in order first and expanded afterwards, so a
    list keeps its controls together and a linked control keeps its place under
    the one it belongs to.
*/
static int rowsOfPage(int pageIndex, int owner, int* rows, int room) {
    const ModPage* page = &global_modPages[pageIndex];
    const int first = page->firstItem, last = page->firstItem + page->itemCount;

    /* Answers whether block k draws a row. A block that was skipped draws
       none wherever it stands: at the top of the page, in a list, or linked
       to another control. A block whose ItemType is missing or not one this
       format has was skipped when it was read. */
    auto drawsRow = [](int k) { return !global_modItems[k].refused; };

    int block[MAX_MODPAGE_ITEMS], order[MAX_MODPAGE_ITEMS], blocks = 0;
    for (int i = first, rank = 0; i < last && blocks < MAX_MODPAGE_ITEMS; ++i) {
        const ModPageItem* item = &global_modItems[i];
        if (item->parent != owner || !drawsRow(i)
            || item->itemType == ITEM_LINKED)
            continue;
        block[blocks] = i;
        order[blocks++] = rowOrderOf(item, ++rank);
    }
    /* Steady: blocks that work out to the same place keep the order the file
       wrote them in. */
    for (int a = 1; a < blocks; ++a) {
        const int key = order[a], which = block[a];
        int b = a - 1;
        for (; b >= 0 && order[b] > key; --b) {
            order[b + 1] = order[b];
            block[b + 1] = block[b];
        }
        order[b + 1] = key;
        block[b + 1] = which;
    }

    int count = 0;
    auto add = [&](int k) {
        rows[count++] = k;
        const int linked = global_modItems[k].linked;
        if (linked >= 0 && drawsRow(linked) && count < room)
            rows[count++] = linked;
    };
    for (int n = 0; n < blocks && count < room; ++n) {
        const int i = block[n];
        if (global_modItems[i].itemType != ITEM_LIST) { add(i); continue; }
        for (int k = first; k < last && count < room; ++k)
            if (global_modItems[k].parent == i && drawsRow(k)) add(k);
    }
    return count;
}

/*
    The name of the page a submenu opens: the name of the page the submenu is
    on, a dash and the submenu's block number, "mods/page-example_mod-12".
    Answers 0 when that is longer than a widget file's name can be, and out is
    then empty. The row that opens the page and the page itself are both
    named through here, so a submenu whose name does not fit has neither.
*/
static int submenuPageName(char (&out)[MAX_WIDGET_NAME], const char* onPage,
                           int block) {
    return formatInto(out, sizeof(out), "%s-%d", onPage, block);
}

static void generateOnePage(int pageIndex, int owner, const char* name,
                            const char* title, const MenuFile* template_) {
    char* text = (char*)malloc(MAX_PAGE_BYTES);
    if (!text) { writeLog("PAGE no memory for %s", name); return; }

    uint32_t at = pageHead(template_, title, text, MAX_PAGE_BYTES);
    if (!at) { free(text); return; }
    /* What the log calls this page: its heading, or its name where it has
       none. */
    const char* onPage = title[0] ? title : name;

    int rows[MAX_MODPAGE_ITEMS];
    const int count = rowsOfPage(pageIndex, owner, rows, MAX_MODPAGE_ITEMS);

    /* How many rows have been written, which is where the next one goes. */
    int written = 0;
    for (int r = 0; r < count; ++r) {
        const ModPageItem* item = &global_modItems[rows[r]];
        /* Room for the longest tag list a row can carry: a page link's action
           with a page name of its own, the speech flag, and a data source. */
        char tags[MODPAGE_NAME * 8];
        const char* widget = BUTTON_WIDGET;
        char rowName[MODPAGE_NAME + 8];
        char cipherId[MODPAGE_NAME + 24] = "";
        char buttonSource[MODPAGE_NAME + 24];
        sprintf_s(buttonSource, "modmenu-button-%d", rows[r]);
        char label[MAX_SETTING_TEXT] = "";

        if (item->itemType == ITEM_SUBMENU) {
            char target[MAX_WIDGET_NAME];
            if (!submenuPageName(target, name, rows[r])) {
                writeLog("PAGE %s: the submenu %s would open a page named "
                         "%s-%d, which is longer than the %d characters a "
                         "widget file's name can have, so the submenu's row "
                         "and its page were left out",
                         global_modPages[pageIndex].name,
                         item->title[0] ? item->title : "with no title", name,
                         rows[r], MAX_WIDGET_NAME - 1);
                /* A control linked to the submenu draws its row directly
                   under the submenu's, and is left out with it. */
                if (item->linked >= 0 && r + 1 < count
                    && rows[r + 1] == item->linked)
                    ++r;
                continue;
            }
            sprintf_s(rowName, "submenu%d", rows[r]);
            buttonLabelOf(item, onPage, label);
            const int labelled = makeButtonSetting(buttonSource, label, onPage);
            openingTags(tags, sizeof(tags), "push-fullscreen", target,
                        labelled ? buttonSource : 0);
            if (!labelled) {
                sprintf_s(cipherId, "MODMENU_SUBMENU_%d", rows[r]);
                keepPageText(cipherId, label);
            }
        } else if (item->itemType == ITEM_PAGELINK) {
            /* The row the game draws for one of its own pages: the same button,
               told which page to open and how. "push" lays the page over this
               one the way Accessibility does; "push-fullscreen" replaces it the
               way Display does. A page named here is written through as it
               stands, whether it is the game's or ours.

               A row can carry one data source. PageDataSource names one of the
               game's, which decides whether the row is shown at all, and
               PageTextId names one of the game's labels; either of those wins,
               and the label then comes from the text database as the game's own
               rows do. Without either the title is written as given, cut as
               buttonLabelOf cuts it. */
            sprintf_s(rowName, "pagelink%d", rows[r]);
            if (item->pageTextId[0])
                copyBounded(cipherId, sizeof(cipherId), item->pageTextId,
                            "page text id");
            else buttonLabelOf(item, onPage, label);
            const int entryRow = global_modPages[pageIndex].ofTheGame
                && _stricmp(item->pageName, LANDING_PAGE_NAME) == 0;
            const char* source =
                item->pageDataSource[0] ? item->pageDataSource
                : !cipherId[0]
                  && makeButtonSetting(buttonSource, label, onPage, entryRow)
                ? buttonSource : 0;
            openingTags(tags, sizeof(tags),
                        item->openOverlay ? "push" : "push-fullscreen",
                        item->pageName, source);
            if (!cipherId[0] && !strstr(tags, buttonSource)) {
                sprintf_s(cipherId, "MODMENU_PAGELINK_%d", rows[r]);
                keepPageText(cipherId, label);
            }
        } else {
            /* A keybind's row is a button whose choosing sends KEYBIND_EVENT,
               on which the framework takes the next key held. Where keys
               cannot be set in this build it is a selector's row, which
               shows the key's name as its one option. */
            const int keyRow = item->controlType == MODPAGE_KEYBIND
                            && keybindsCanBeSet();
            const int drawnAs = item->controlType < 0 ? MODPAGE_TOGGLE
                              : item->controlType == MODPAGE_KEYBIND && !keyRow
                              ? MODPAGE_SELECTOR : item->controlType;
            widget = global_rowWidget[drawnAs];
            if (keyRow)
                sprintf_s(tags, "\"action\":\"send-event %s\","
                                "\"allowTtsOnFocus\":\"true\","
                                "\"dataSource\":\"%s\"", KEYBIND_EVENT,
                          item->settingName);
            else
                sprintf_s(tags, "\"dataSource\":\"%s\"", item->settingName);
            sprintf_s(rowName, "row%d", rows[r]);
        }
        const int height = item->itemType != ITEM_SUBMENU
                           && item->itemType != ITEM_PAGELINK
                           && item->controlType == MODPAGE_KEYBIND
                           && keybindsCanBeSet()
                         ? KEYBIND_ROW_HEIGHT : ROW_ITEM_HEIGHT;
        if (!appendRow(text, &at, cipherId, written, rowName, widget, tags,
                       height))
            break;
        ++written;
    }
    if (at + 4 >= MAX_PAGE_BYTES) {
        free(text);
        writeLog("PAGE %s is too large", name);
        return;
    }
    closePage(name, text, at);

    /* A submenu becomes a page of its own, drawn the same way, headed with
       its title as the row that opens it is labelled: cut to the room a
       setting has for a title, the text pageTextIsSafeToShow checked, which
       pageHead does not cut again. A submenu whose page name does not fit
       was left out of the rows above and gets no page. */
    for (int r = 0; r < count; ++r) {
        if (global_modItems[rows[r]].itemType != ITEM_SUBMENU) continue;
        char childName[MAX_WIDGET_NAME];
        if (!submenuPageName(childName, name, rows[r])) continue;
        char heading[MAX_SETTING_TEXT];
        formatInto(heading, sizeof(heading), "%.*s", (int)sizeof(heading) - 1,
                   global_modItems[rows[r]].title);
        generateOnePage(pageIndex, rows[r], childName, heading, template_);
    }
}

/* The page that lists one button per mod, which the options list opens. */
static void generateLandingPage(const MenuFile* template_) {
    char* text = (char*)malloc(MAX_PAGE_BYTES);
    if (!text) return;
    const char* const heading = "MOD CONFIGURATION";
    uint32_t at = pageHead(template_, heading, text, MAX_PAGE_BYTES);
    if (!at) { free(text); return; }

    /* The row on the options list that opens this page, whose cipherId the
       substituted menu/page-options-v3 carries. */
    keepPageText("MODMENU_ROOT", heading);

    for (int p = 0; p < global_modPageCount; ++p) {
        const ModPage* page = &global_modPages[p];
        if (page->ofTheGame) continue;
        char tags[MODPAGE_NAME * 3], rowName[MODPAGE_NAME + 8];
        char target[MODPAGE_NAME + 16], cipherId[MODPAGE_NAME + 24] = "";
        char buttonSource[MODPAGE_NAME + 24];
        sprintf_s(buttonSource, "modmenu-mod-%d", p);
        sprintf_s(rowName, "mod%d", p);
        sprintf_s(target, "mods/page-%s", page->name);
        const int labelled = makeButtonSetting(buttonSource, page->title,
                                               heading);
        openingTags(tags, sizeof(tags), "push-fullscreen", target,
                    labelled ? buttonSource : 0);
        if (!labelled) {
            sprintf_s(cipherId, "MODMENU_PAGE_%d", p);
            keepPageText(cipherId, page->title);
        }
        if (!appendRow(text, &at, cipherId, p, rowName, BUTTON_WIDGET, tags))
            break;
    }
    closePage(LANDING_PAGE_NAME, text, at);
}

/*
    The row a keybind draws in: the game's own row from its Customise Controls
    page, rebuilt from three widget files of ours, each the game's file with
    its engine class taken out where that class works only on the game's own
    bindings.

    mods/keybind-row        menu/list-items/item-button-remap-default-pc, 150
                            high, its root a Menu2::Button (so choosing it runs
                            its action) in place of a Menu2::Component, with
                            the row's hover background, its background for a
                            row whose key is being set, and its states normal,
                            focused, secondary_focused, disabled and
                            focused_disabled
    mods/keybind-row-ref    item-button-remap-default-ref-pc: the title
                            (label0, menu-button-smaller) and one cell,
                            keybindCell, where the game's rightmost column
                            stands; the game's row has three (two keys and a
                            pad button)
    mods/keybind-cell       item-button-remap-default-column-prim-kbm-pc
                            without Menu2::RemapColumnItems: the cell's frame
                            (columnItemRefPrimKbm, the game's ...-column-item-
                            pc), the key's icon (primaryKeyboardIcon), a name
                            for a key the game has no icon for (keyName, ours),
                            and the binding texts (bindingTexts, the game's
                            ...-default-text-pc), with the states normal,
                            hovered, disabled and hovered_disabled

    The framework fills the cell and plays the states the game's page plays
    (keybindTick).
*/
#define KEYBIND_REF_NAME   "mods/keybind-row-ref"
#define KEYBIND_CELL_NAME  "mods/keybind-cell"

static const char global_keybindRowDocument[] =
    "{\"size\":[2000.0,150.0],\"grid\":[100,100],\"lang\":\"eng\",\"defaultEngineCla"
    "ss\":\"\",\"defaultScript\":\"\",\"fps\":60.0,\"478710ee764acc8e\":30.0,\"currentF"
    "rame\":0.0,\"textTracking.x\":0.0,\"textTracking.y\":0.0,\"animation\":{\"labe"
    "ls\":[{\"label\":\"disabled\",\"loop\":0,\"range\":[1.5,1.5],\"rate\":1.0,\"revCur"
    "ve\":0},{\"label\":\"focused\",\"loop\":0,\"range\":[0.5,0.5],\"rate\":1.0,\"revCu"
    "rve\":0},{\"label\":\"focused_disabled\",\"loop\":0,\"range\":[2.0,2.0],\"rate\":"
    "1.0,\"revCurve\":0},{\"label\":\"normal\",\"loop\":0,\"range\":[0.0,0.0],\"rate\":"
    "1.0,\"revCurve\":0},{\"label\":\"secondary_focused\",\"loop\":0,\"range\":[0.75,"
    "0.75],\"rate\":1.0,\"revCurve\":0},{\"label\":\"disabled^focused_disabled\",\"l"
    "oop\":0,\"range\":[1.5,2.0],\"rate\":2.0,\"revCurve\":0},{\"label\":\"focused^no"
    "rmal\",\"loop\":0,\"range\":[0.5,0.0],\"rate\":2.0,\"revCurve\":0},{\"label\":\"fo"
    "cused^secondary_focused\",\"loop\":0,\"range\":[0.5,0.75],\"rate\":1.0,\"revCu"
    "rve\":0},{\"label\":\"focused_disabled^disabled\",\"loop\":0,\"range\":[2.0,1.5"
    "],\"rate\":2.0,\"revCurve\":0},{\"label\":\"normal^focused\",\"loop\":0,\"range\":"
    "[0.0,0.5],\"rate\":2.0,\"revCurve\":0},{\"label\":\"secondary_focused^focused"
    "\",\"loop\":0,\"range\":[0.75,0.5],\"rate\":1.0,\"revCurve\":0}]},\"items\":[{\"an"
    "imation\":{\"keyframes\":{}},\"anchorMaxN.x\":1.0,\"anchorMaxN.y\":0.5,\"ancho"
    "rMinN.x\":0.0,\"anchorMinN.y\":0.5,\"class\":\"Group\",\"color.a\":255,\"depth\":"
    "0.0,\"engineClass\":\"Menu2::Button\",\"flowDirection\":\"Vertical\",\"flowGrid"
    "Size.x\":0,\"flowGridSize.y\":0,\"flowMargins.b\":0.0,\"flowMargins.l\":0.0,\""
    "flowMargins.r\":0.0,\"flowMargins.t\":0.0,\"flowSpacing\":0.0,\"focusable\":1"
    ",\"guid\":\"780ba25d863da4448f9d404b2f5d0d68\",\"inheritAlpha\":1,\"inheritRo"
    "tation\":1,\"inheritScale\":1,\"inheritTint\":0,\"isHoverable\":1,\"isMaskInve"
    "rted\":0,\"isMaskOrClip\":0,\"layoutMode\":\"Unspecified\",\"locked\":0,\"name\":"
    "\"root\",\"noSpawn\":0,\"parent\":null,\"pivotAs.x\":0.0,\"pivotAs.y\":-75.0,\"pi"
    "votN.x\":0.100000001,\"pivotN.y\":0.0,\"rectSizeDelta.height\":150.0,\"rectS"
    "izeDelta.width\":0.0,\"resizeDirection\":\"Vertical\",\"rotation\":0.0,\"scale"
    ".x\":1.0,\"scale.y\":1.0,\"tint.blend\":0.0,\"tint.end\":4294967295,\"tint.sta"
    "rt\":4294967295,\"visible\":1},{\"animation\":{\"keyframes\":{\"fcolor.a\":{\"ty"
    "pe\":\"float\",\"keys\":[0.0,0.5,0.75,1.5,2.0],\"values\":[1.0,1.0,0.0,1.0,1."
    "0],\"curveTypes\":[null,null,null,null,null]}}},\"anchorMaxN.x\":1.0,\"anch"
    "orMaxN.y\":1.0,\"anchorMinN.x\":0.0,\"anchorMinN.y\":0.0,\"class\":\"Reference"
    "\",\"color.a\":255,\"depth\":0.0,\"focusable\":1,\"guid\":\"da3de1ed867b954581d7"
    "ddb457958776\",\"inheritAlpha\":1,\"inheritRotation\":1,\"inheritScale\":1,\"i"
    "nheritTint\":0,\"isHoverable\":1,\"isMaskInverted\":0,\"isMaskOrClip\":0,\"lay"
    "outMode\":\"Exclude\",\"locked\":0,\"name\":\"hoverBg\",\"noSpawn\":0,\"parent\":\"7"
    "80ba25d863da4448f9d404b2f5d0d68\",\"pivotAs.x\":17.0,\"pivotAs.y\":0.0,\"piv"
    "otN.x\":0.5,\"pivotN.y\":0.5,\"rectSizeDelta.height\":16.0,\"rectSizeDelta.w"
    "idth\":34.0,\"rotation\":0.0,\"scale.x\":1.0,\"scale.y\":1.0,\"source\":\"menu/c"
    "omponents/hover-bg_hide-unhovered.widget\",\"tint.blend\":0.0,\"tint.end\":"
    "4294967295,\"tint.start\":4294967295,\"visible\":1},{\"animation\":{\"keyfram"
    "es\":{\"fcolor.a\":{\"type\":\"float\",\"keys\":[0.0,0.5,1.5,2.0],\"values\":[1.0"
    ",1.0,1.0,1.0],\"curveTypes\":[\"EaseInOut2\",\"EaseInOut2\",\"EaseInOut2\",\"Ea"
    "seInOut2\"]},\"tint\":{\"type\":\"QColor\",\"keys\":[0.0,0.5,1.5,2.0],\"values\":"
    "[\"t2-menu-normal\",\"t2-menu-focused\",\"t2-menu-disabled\",\"t2-menu-focuse"
    "d-disabled\"],\"curveTypes\":[\"EaseInOut2\",\"EaseInOut2\",\"EaseInOut2\",\"Eas"
    "eInOut2\"]}}},\"anchorMaxN.x\":1.0,\"anchorMaxN.y\":0.0,\"anchorMinN.x\":0.0,"
    "\"anchorMinN.y\":0.0,\"class\":\"Reference\",\"color.a\":255,\"depth\":1.0,\"focu"
    "sable\":1,\"guid\":\"3af04c11d633e54b86488698890f9fb6\",\"inheritAlpha\":1,\"i"
    "nheritRotation\":1,\"inheritScale\":1,\"inheritTint\":0,\"isHoverable\":0,\"is"
    "MaskInverted\":0,\"isMaskOrClip\":0,\"layoutMode\":\"Exclude\",\"locked\":0,\"na"
    "me\":\"reference\",\"noSpawn\":0,\"parent\":\"780ba25d863da4448f9d404b2f5d0d68"
    "\",\"pivotAs.x\":0.0,\"pivotAs.y\":20.0,\"pivotN.x\":0.0,\"pivotN.y\":0.0,\"rect"
    "SizeDelta.height\":110.0,\"rectSizeDelta.width\":0.0,\"rotation\":0.0,\"scal"
    "e.x\":1.0,\"scale.y\":1.0,\"source\":\"mods/keybind-row-ref.widget\",\"tint.bl"
    "end\":0.0,\"tint.end\":\"t2-menu-disabled\",\"tint.start\":\"t2-menu-focused\","
    "\"visible\":1},{\"animation\":{\"keyframes\":{\"fcolor.a\":{\"type\":\"float\",\"ke"
    "ys\":[0.0,0.5,0.75,1.5,2.0],\"values\":[0.0,0.0,1.0,0.0,0.0],\"curveTypes\""
    ":[null,null,null,null,null]}}},\"anchorMaxN.x\":1.0,\"anchorMaxN.y\":1.0,\""
    "anchorMinN.x\":0.0,\"anchorMinN.y\":0.0,\"class\":\"Group\",\"color.a\":0,\"dept"
    "h\":2.0,\"flowDirection\":\"NoFlow\",\"flowGridSize.x\":0,\"flowGridSize.y\":0,"
    "\"flowMargins.b\":0.0,\"flowMargins.l\":0.0,\"flowMargins.r\":0.0,\"flowMargi"
    "ns.t\":0.0,\"flowSpacing\":0.0,\"focusable\":1,\"guid\":\"fc612ed2e49ab64fb4eb"
    "0be152353e7c\",\"inheritAlpha\":1,\"inheritRotation\":1,\"inheritScale\":1,\"i"
    "nheritTint\":0,\"isHoverable\":0,\"isMaskInverted\":0,\"isMaskOrClip\":0,\"lay"
    "outMode\":\"Exclude\",\"locked\":0,\"name\":\"bgGroup\",\"noSpawn\":0,\"parent\":\"7"
    "80ba25d863da4448f9d404b2f5d0d68\",\"pivotAs.x\":17.0,\"pivotAs.y\":0.0,\"piv"
    "otN.x\":0.5,\"pivotN.y\":0.5,\"rectSizeDelta.height\":16.0,\"rectSizeDelta.w"
    "idth\":34.0,\"resizeDirection\":\"NoResize\",\"rotation\":0.0,\"scale.x\":1.0,\""
    "scale.y\":1.0,\"tint.blend\":0.0,\"tint.end\":4294967295,\"tint.start\":42949"
    "67295,\"visible\":1},{\"animation\":{\"keyframes\":{}},\"alignH\":0,\"alignV\":0"
    ",\"anchorMaxN.x\":1.0,\"anchorMaxN.y\":1.0,\"anchorMinN.x\":0.0,\"anchorMinN."
    "y\":0.0,\"autoSize\":0,\"class\":\"Image\",\"color.a\":255,\"depth\":2.0,\"effect\""
    ":\"Regular\",\"explicitSize\":0,\"focusable\":0,\"guid\":\"999743b9760cb04aad94"
    "178bddeda571\",\"inheritAlpha\":1,\"inheritRotation\":1,\"inheritScale\":1,\"i"
    "nheritTint\":0,\"isHoverable\":0,\"isMaskInverted\":0,\"isMaskOrClip\":0,\"lay"
    "outMode\":\"Unspecified\",\"locked\":0,\"name\":\"hoverBgFilling\",\"noSpawn\":0,"
    "\"parent\":\"fc612ed2e49ab64fb4eb0be152353e7c\",\"path\":\"general-textures/s"
    "quare-32px.png\",\"pivotAs.x\":0.0,\"pivotAs.y\":0.0,\"pivotN.x\":0.5,\"pivotN"
    ".y\":0.5,\"rasterSize.height\":0.0,\"rasterSize.width\":0.0,\"rectSizeDelta."
    "height\":0.0,\"rectSizeDelta.width\":0.0,\"rotation\":0.0,\"scale.x\":1.0,\"sc"
    "ale.y\":1.0,\"scaleMode\":\"Stretch\",\"slices\":[0.328099996,0.328099996,0.3"
    "28099996,0.328099996],\"tint.blend\":0.0,\"tint.end\":\"t2-slot-bg-white\",\""
    "tint.start\":\"t2-slot-bg-white\",\"visible\":1},{\"animation\":{\"keyframes\":"
    "{\"fcolor.a\":{\"type\":\"float\",\"keys\":[0.0,0.25,0.5,0.75],\"values\":[1.0,1"
    ".0,1.0,0.101960786],\"curveTypes\":[null,null,null,null]}}},\"anchorMaxN."
    "x\":1.0,\"anchorMaxN.y\":1.0,\"anchorMinN.x\":0.0,\"anchorMinN.y\":0.0,\"capSt"
    "yle\":\"Flat\",\"class\":\"Polyline\",\"closed\":1,\"color.a\":255,\"depth\":3.0,\"e"
    "ffect\":\"Regular\",\"focusable\":0,\"guid\":\"7f9e4a1a71f9e045af4ea79c9eef3c4"
    "b\",\"inheritAlpha\":1,\"inheritRotation\":1,\"inheritScale\":1,\"inheritTint\""
    ":0,\"isHoverable\":0,\"isMaskInverted\":0,\"isMaskOrClip\":0,\"joinStyle\":\"Mi"
    "ter\",\"layoutMode\":\"Exclude\",\"lineWidth\":2.0,\"locked\":0,\"name\":\"hoverBg"
    "Outline\",\"noSpawn\":0,\"parent\":\"fc612ed2e49ab64fb4eb0be152353e7c\",\"pivo"
    "tAs.x\":0.0,\"pivotAs.y\":0.0,\"pivotN.x\":0.5,\"pivotN.y\":0.5,\"pointsUv\":[0"
    ".0,0.0,0.0,1.0,1.0,1.0,1.0,0.0],\"rectSizeDelta.height\":-2.0,\"rectSizeD"
    "elta.width\":-2.0,\"rotation\":0.0,\"scale.x\":1.0,\"scale.y\":1.0,\"tint.blen"
    "d\":0.0,\"tint.end\":\"t2-white\",\"tint.start\":\"t2-white\",\"visible\":1}]}";

static const char global_keybindRefDocument[] =
    "{\"size\":[2000.0,160.0],\"grid\":[100,100],\"lang\":\"eng\",\"defaultEngineCla"
    "ss\":\"\",\"defaultScript\":\"\",\"fps\":60.0,\"478710ee764acc8e\":30.0,\"currentF"
    "rame\":15.0,\"textTracking.x\":0.0,\"textTracking.y\":0.0,\"685a017374c5cfe3"
    "\":1,\"animation\":{\"labels\":[{\"label\":\"binding\",\"loop\":0,\"range\":[0.25,0"
    ".25],\"rate\":1.0,\"revCurve\":0},{\"label\":\"bound\",\"loop\":0,\"range\":[0.5,0"
    ".5],\"rate\":1.0,\"revCurve\":0},{\"label\":\"unbound\",\"loop\":0,\"range\":[1.0,"
    "1.0],\"rate\":1.0,\"revCurve\":0},{\"label\":\"unbound_binding\",\"loop\":0,\"ran"
    "ge\":[0.75,0.75],\"rate\":1.0,\"revCurve\":0},{\"label\":\"binding^bound\",\"loo"
    "p\":0,\"range\":[0.25,0.5],\"rate\":1.0,\"revCurve\":0},{\"label\":\"bound^bindi"
    "ng\",\"loop\":0,\"range\":[0.5,0.25],\"rate\":1.0,\"revCurve\":0},{\"label\":\"unb"
    "ound^unbound_binding\",\"loop\":0,\"range\":[1.0,0.75],\"rate\":1.0,\"revCurve"
    "\":0},{\"label\":\"unbound_binding^bound\",\"loop\":0,\"range\":[0.75,0.5],\"rat"
    "e\":1.0,\"revCurve\":0},{\"label\":\"unbound_binding^unbound\",\"loop\":0,\"rang"
    "e\":[0.75,1.0],\"rate\":1.0,\"revCurve\":0}]},\"items\":[{\"animation\":{\"keyfr"
    "ames\":{}},\"anchorMaxN.x\":1.0,\"anchorMaxN.y\":0.0,\"anchorMinN.x\":0.0,\"an"
    "chorMinN.y\":0.0,\"class\":\"Group\",\"color.a\":255,\"depth\":0.0,\"flowDirecti"
    "on\":\"NoFlow\",\"flowGridSize.x\":0,\"flowGridSize.y\":0,\"flowMargins.b\":0.0"
    ",\"flowMargins.l\":68.0,\"flowMargins.r\":400.0,\"flowMargins.t\":0.0,\"flowS"
    "pacing\":0.0,\"focusable\":0,\"guid\":\"780ba25d863da4448f9d404b2f5d0d68\",\"i"
    "nheritAlpha\":1,\"inheritRotation\":1,\"inheritScale\":1,\"inheritTint\":1,\"i"
    "sHoverable\":1,\"isMaskInverted\":0,\"isMaskOrClip\":0,\"layoutMode\":\"Unspec"
    "ified\",\"locked\":0,\"name\":\"root\",\"noSpawn\":0,\"parent\":null,\"pivotAs.x\":"
    "0.0,\"pivotAs.y\":0.0,\"pivotN.x\":0.0,\"pivotN.y\":0.0,\"rectSizeDelta.heigh"
    "t\":160.0,\"rectSizeDelta.width\":0.0,\"resizeDirection\":\"NoResize\",\"rotat"
    "ion\":0.0,\"scale.x\":1.0,\"scale.y\":1.0,\"tags\":{\"noAnimStateLogic\":\"true\""
    "},\"tint.blend\":0.0,\"tint.end\":4294967295,\"tint.start\":4294967295,\"visi"
    "ble\":1},{\"animation\":{\"keyframes\":{\"fcolor.a\":{\"type\":\"float\",\"keys\":["
    "-0.25],\"values\":[1.0],\"curveTypes\":[null]},\"pivotAs.x\":{\"type\":\"float\""
    ",\"keys\":[-0.25,0.0,0.5],\"values\":[68.0,50.0,50.0],\"curveTypes\":[null,n"
    "ull,null]},\"pivotAs.y\":{\"type\":\"float\",\"keys\":[-0.25,0.0,0.5],\"values\""
    ":[0.0,0.0,0.0],\"curveTypes\":[null,null,null]},\"tint\":{\"type\":\"QColor\","
    "\"keys\":[-0.25],\"values\":[\"t2-white\"],\"curveTypes\":[null]}}},\"alignH\":0"
    ",\"alignV\":1,\"anchorMaxN.x\":1.0,\"anchorMaxN.y\":1.0,\"anchorMinN.x\":0.0,\""
    "anchorMinN.y\":0.0,\"autoSize\":0,\"autoSizeMode\":\"NoAutoSize\",\"cipherId\":"
    "\"\",\"class\":\"Text\",\"color.a\":255,\"depth\":0.0,\"effect\":\"Regular\",\"faceId"
    "\":\"Alternate\",\"focusable\":0,\"guid\":\"9615fc6ae430da48bc2f600bbfa46ce4\","
    "\"inheritAlpha\":1,\"inheritRotation\":1,\"inheritScale\":1,\"inheritTint\":1,"
    "\"isHoverable\":0,\"isMaskInverted\":0,\"isMaskOrClip\":0,\"layoutMode\":\"Excl"
    "ude\",\"locked\":1,\"multiline\":1,\"name\":\"label0\",\"noSpawn\":0,\"parent\":\"78"
    "0ba25d863da4448f9d404b2f5d0d68\",\"pivotAs.x\":50.0,\"pivotAs.y\":0.0,\"pivo"
    "tN.x\":0.0,\"pivotN.y\":0.5,\"pointSize\":44,\"rectSizeDelta.height\":0.0,\"re"
    "ctSizeDelta.width\":-1070.0,\"rotation\":0.0,\"scale.x\":1.0,\"scale.y\":1.0,"
    "\"style\":\"menu-button-smaller\",\"text\":\"Interact\",\"tint.blend\":0.0,\"tint"
    ".end\":\"t2-white\",\"tint.start\":\"t2-white\",\"tracking.x\":0.0,\"tracking.y\""
    ":0.0,\"visible\":1},{\"animation\":{\"keyframes\":{\"fcolor.a\":{\"type\":\"float"
    "\",\"keys\":[1.75],\"values\":[1.0],\"curveTypes\":[null]},\"tint\":{\"type\":\"QC"
    "olor\",\"keys\":[1.75],\"values\":[4294967295],\"curveTypes\":[null]}}},\"anch"
    "orMaxN.x\":1.0,\"anchorMaxN.y\":0.5,\"anchorMinN.x\":1.0,\"anchorMinN.y\":0.5"
    ",\"class\":\"Reference\",\"color.a\":255,\"depth\":1.0,\"focusable\":0,\"guid\":\"0"
    "7c050f82a48b940a9875c5434573688\",\"inheritAlpha\":1,\"inheritRotation\":1,"
    "\"inheritScale\":1,\"inheritTint\":0,\"isHoverable\":0,\"isMaskInverted\":0,\"i"
    "sMaskOrClip\":0,\"layoutMode\":\"Unspecified\",\"locked\":0,\"name\":\"keybindCe"
    "ll\",\"noSpawn\":0,\"parent\":\"780ba25d863da4448f9d404b2f5d0d68\",\"pivotAs.x"
    "\":-162.0,\"pivotAs.y\":0.0,\"pivotN.x\":0.5,\"pivotN.y\":0.5,\"rectSizeDelta."
    "height\":92.0,\"rectSizeDelta.width\":288.0,\"rotation\":0.0,\"scale.x\":1.0,"
    "\"scale.y\":1.0,\"source\":\"mods/keybind-cell.widget\",\"tint.blend\":0.0,\"ti"
    "nt.end\":4294967295,\"tint.start\":4294967295,\"visible\":1}]}";

static const char global_keybindCellDocument[] =
    "{\"size\":[288.0,108.0],\"grid\":[100,100],\"lang\":\"eng\",\"defaultEngineClas"
    "s\":\"\",\"defaultScript\":\"\",\"fps\":60.0,\"478710ee764acc8e\":0,\"currentFrame"
    "\":0.0,\"textTracking.x\":0.0,\"textTracking.y\":0.0,\"animation\":{\"labels\":"
    "[{\"label\":\"disabled\",\"loop\":0,\"range\":[0.5,0.5],\"rate\":1.0,\"revCurve\":"
    "0},{\"label\":\"hovered\",\"loop\":0,\"range\":[0.25,0.25],\"rate\":1.0,\"revCurv"
    "e\":0},{\"label\":\"hovered_disabled\",\"loop\":0,\"range\":[0.75,0.75],\"rate\":"
    "1.0,\"revCurve\":0},{\"label\":\"normal\",\"loop\":0,\"range\":[0.0,0.0],\"rate\":"
    "1.0,\"revCurve\":0},{\"label\":\"disabled^hovered_disabled\",\"loop\":0,\"range"
    "\":[0.5,0.75],\"rate\":1.0,\"revCurve\":0},{\"label\":\"hovered^normal\",\"loop\""
    ":0,\"range\":[0.25,0.0],\"rate\":1.0,\"revCurve\":0},{\"label\":\"hovered_disab"
    "led^disabled\",\"loop\":0,\"range\":[0.75,0.5],\"rate\":1.0,\"revCurve\":0},{\"l"
    "abel\":\"normal^hovered\",\"loop\":0,\"range\":[0.0,0.25],\"rate\":1.0,\"revCurv"
    "e\":0}]},\"items\":[{\"animation\":{\"keyframes\":{}},\"anchorMaxN.x\":0.5,\"anc"
    "horMaxN.y\":0.5,\"anchorMinN.x\":0.5,\"anchorMinN.y\":0.5,\"class\":\"Group\",\""
    "color.a\":255,\"depth\":0.0,\"flowDirection\":\"NoFlow\",\"flowGridSize.x\":0,\""
    "flowGridSize.y\":0,\"flowMargins.b\":0.0,\"flowMargins.l\":0.0,\"flowMargins"
    ".r\":0.0,\"flowMargins.t\":0.0,\"flowSpacing\":0.0,\"focusable\":0,\"guid\":\"5a"
    "41ca793a2c3d47b810017f0b86e807\",\"inheritAlpha\":1,\"inheritRotation\":1,\""
    "inheritScale\":1,\"inheritTint\":0,\"isHoverable\":0,\"isMaskInverted\":0,\"is"
    "MaskOrClip\":0,\"layoutMode\":\"Unspecified\",\"locked\":0,\"name\":\"root\",\"noS"
    "pawn\":0,\"parent\":null,\"pivotAs.x\":0.0,\"pivotAs.y\":0.0,\"pivotN.x\":0.5,\""
    "pivotN.y\":0.5,\"rectSizeDelta.height\":108.0,\"rectSizeDelta.width\":288.0"
    ",\"resizeDirection\":\"NoResize\",\"rotation\":0.0,\"scale.x\":1.0,\"scale.y\":1"
    ".0,\"tint.blend\":0.0,\"tint.end\":4294967295,\"tint.start\":4294967295,\"vis"
    "ible\":1},{\"animation\":{\"keyframes\":{}},\"anchorMaxN.x\":0.5,\"anchorMaxN."
    "y\":0.5,\"anchorMinN.x\":0.5,\"anchorMinN.y\":0.5,\"class\":\"Reference\",\"colo"
    "r.a\":255,\"depth\":0.0,\"engineClass\":\"Gui2::Widget\",\"focusable\":1,\"guid\""
    ":\"7fd322958d8fb746b800f56c1399b92b\",\"inheritAlpha\":1,\"inheritRotation\""
    ":1,\"inheritScale\":1,\"inheritTint\":0,\"isHoverable\":0,\"isMaskInverted\":0"
    ",\"isMaskOrClip\":0,\"layoutMode\":\"Unspecified\",\"locked\":0,\"name\":\"column"
    "ItemRefPrimKbm\",\"noSpawn\":0,\"parent\":\"5a41ca793a2c3d47b810017f0b86e807"
    "\",\"pivotAs.x\":0.0,\"pivotAs.y\":0.0,\"pivotN.x\":0.5,\"pivotN.y\":0.5,\"rectS"
    "izeDelta.height\":108.0,\"rectSizeDelta.width\":288.0,\"rotation\":0.0,\"sca"
    "le.x\":1.0,\"scale.y\":1.0,\"source\":\"menu/list-items/item-button-remap-de"
    "fault-column-item-pc.widget\",\"tint.blend\":0.0,\"tint.end\":4294967295,\"t"
    "int.start\":4294967295,\"visible\":1},{\"animation\":{\"keyframes\":{\"fcolor."
    "a\":{\"type\":\"float\",\"keys\":[0.0,0.25,0.5,0.75],\"values\":[1.0,1.0,1.0,1."
    "0],\"curveTypes\":[null,null,null,null]},\"tint\":{\"type\":\"QColor\",\"keys\":"
    "[0.0,0.25,0.5,0.75],\"values\":[\"t2-menu-normal\",\"t2-menu-focused\",\"t2-m"
    "enu-disabled\",\"t2-menu-focused-disabled\"],\"curveTypes\":[null,null,null"
    ",null]}}},\"alignH\":1,\"alignV\":1,\"anchorMaxN.x\":1.0,\"anchorMaxN.y\":1.0,"
    "\"anchorMinN.x\":0.0,\"anchorMinN.y\":0.0,\"autoSize\":0,\"class\":\"Image\",\"co"
    "lor.a\":255,\"depth\":1.0,\"effect\":\"Regular\",\"explicitSize\":0,\"focusable\""
    ":0,\"guid\":\"e4a0ab555afc4f498ec602ef95852e2f\",\"inheritAlpha\":1,\"inherit"
    "Rotation\":1,\"inheritScale\":1,\"inheritTint\":0,\"isHoverable\":0,\"isMaskIn"
    "verted\":0,\"isMaskOrClip\":0,\"layoutMode\":\"Unspecified\",\"locked\":0,\"name"
    "\":\"primaryKeyboardIcon\",\"noSpawn\":0,\"parent\":\"5a41ca793a2c3d47b810017f"
    "0b86e807\",\"path\":\"\",\"pivotAs.x\":0.0,\"pivotAs.y\":0.0,\"pivotN.x\":0.5,\"pi"
    "votN.y\":0.5,\"rasterSize.height\":0.0,\"rasterSize.width\":0.0,\"rectSizeDe"
    "lta.height\":0.0,\"rectSizeDelta.width\":0.0,\"rotation\":0.0,\"scale.x\":1.0"
    ",\"scale.y\":1.0,\"scaleMode\":\"Original\",\"slices\":[0.328125,0.328125,0.32"
    "8125,0.328125],\"tint.blend\":0.0,\"tint.end\":\"t2-menu-focused\",\"tint.sta"
    "rt\":\"t2-menu-normal\",\"visible\":1},{\"animation\":{\"keyframes\":{\"fcolor.a"
    "\":{\"type\":\"float\",\"keys\":[0.0,0.25,0.5,0.75],\"values\":[1.0,1.0,1.0,1.0"
    "],\"curveTypes\":[null,null,null,null]},\"tint\":{\"type\":\"QColor\",\"keys\":["
    "0.0,0.25,0.5,0.75],\"values\":[\"t2-menu-normal\",\"t2-menu-focused\",\"t2-me"
    "nu-disabled\",\"t2-menu-focused-disabled\"],\"curveTypes\":[null,null,null,"
    "null]}}},\"alignH\":1,\"alignV\":1,\"anchorMaxN.x\":1.0,\"anchorMaxN.y\":1.0,\""
    "anchorMinN.x\":0.0,\"anchorMinN.y\":0.0,\"autoSize\":0,\"autoSizeMode\":\"NoAu"
    "toSize\",\"cipherId\":\"\",\"class\":\"Text\",\"color.a\":255,\"depth\":1.0,\"effect"
    "\":\"Regular\",\"faceId\":\"Alternate\",\"focusable\":0,\"guid\":\"6b0f6c2e1d3a4b5"
    "c8d9e0f1a2b3c4d5e\",\"inheritAlpha\":1,\"inheritRotation\":1,\"inheritScale\""
    ":1,\"inheritTint\":0,\"isHoverable\":0,\"isMaskInverted\":0,\"isMaskOrClip\":0"
    ",\"layoutMode\":\"Unspecified\",\"locked\":0,\"multiline\":0,\"name\":\"keyName\","
    "\"noSpawn\":0,\"parent\":\"5a41ca793a2c3d47b810017f0b86e807\",\"pivotAs.x\":0."
    "0,\"pivotAs.y\":0.0,\"pivotN.x\":0.5,\"pivotN.y\":0.5,\"pointSize\":35,\"rectSi"
    "zeDelta.height\":0.0,\"rectSizeDelta.width\":-20.0,\"rotation\":0.0,\"scale."
    "x\":1.0,\"scale.y\":1.0,\"text\":\"\",\"tint.blend\":0.0,\"tint.end\":\"t2-menu-fo"
    "cused\",\"tint.start\":\"t2-menu-normal\",\"tracking.x\":0.0,\"tracking.y\":0.0"
    ",\"visible\":0},{\"animation\":{\"keyframes\":{}},\"anchorMaxN.x\":0.5,\"anchor"
    "MaxN.y\":0.5,\"anchorMinN.x\":0.5,\"anchorMinN.y\":0.5,\"capStyle\":\"Flat\",\"c"
    "lass\":\"Polyline\",\"closed\":1,\"color.a\":255,\"depth\":3.0,\"effect\":\"Regula"
    "r\",\"focusable\":0,\"guid\":\"d12ee678d79e2a4f82e4e784f4f4370e\",\"inheritAlp"
    "ha\":1,\"inheritRotation\":1,\"inheritScale\":1,\"inheritTint\":0,\"isHoverabl"
    "e\":0,\"isMaskInverted\":0,\"isMaskOrClip\":0,\"joinStyle\":\"Miter\",\"layoutMo"
    "de\":\"Exclude\",\"lineWidth\":2.0,\"locked\":0,\"name\":\"columnItemOutlineUnbo"
    "undKeyboard\",\"noSpawn\":0,\"parent\":\"5a41ca793a2c3d47b810017f0b86e807\",\""
    "pivotAs.x\":0.0,\"pivotAs.y\":0.0,\"pivotN.x\":0.5,\"pivotN.y\":0.5,\"pointsUv"
    "\":[0.0,0.0,0.0,1.0,1.0,1.0,1.0,0.0],\"rectSizeDelta.height\":108.0,\"rect"
    "SizeDelta.width\":288.0,\"rotation\":0.0,\"scale.x\":1.0,\"scale.y\":1.0,\"tin"
    "t.blend\":0.0,\"tint.end\":\"t2-pure-red\",\"tint.start\":\"t2-pure-red\",\"visi"
    "ble\":0},{\"animation\":{\"keyframes\":{}},\"anchorMaxN.x\":0.5,\"anchorMaxN.y"
    "\":0.5,\"anchorMinN.x\":0.5,\"anchorMinN.y\":0.5,\"class\":\"Reference\",\"color"
    ".a\":255,\"depth\":4.0,\"focusable\":1,\"guid\":\"cad4c08fe7a99a4086a696260921"
    "c59b\",\"inheritAlpha\":1,\"inheritRotation\":1,\"inheritScale\":1,\"inheritTi"
    "nt\":0,\"isHoverable\":0,\"isMaskInverted\":0,\"isMaskOrClip\":0,\"layoutMode\""
    ":\"Unspecified\",\"locked\":0,\"name\":\"bindingTexts\",\"noSpawn\":0,\"parent\":\""
    "5a41ca793a2c3d47b810017f0b86e807\",\"pivotAs.x\":0.0,\"pivotAs.y\":0.0,\"piv"
    "otN.x\":0.5,\"pivotN.y\":0.5,\"rectSizeDelta.height\":108.0,\"rectSizeDelta."
    "width\":358.0,\"rotation\":0.0,\"scale.x\":1.0,\"scale.y\":1.0,\"source\":\"menu"
    "/list-items/item-button-remap-default-text-pc.widget\",\"tint.blend\":0.0"
    ",\"tint.end\":4294967295,\"tint.start\":4294967295,\"visible\":1}]}";

static int keybindRemapWidgetsUnderPc();

/* One of the keybind row's widget files, put in the table with the pages. */
static void keepKeybindFile(const char* name, const char* document,
                            size_t length) {
    static const char from[] = "\"source\":\"menu/list-items/item-button-remap-";
    static const char to[] = "\"source\":\"pc/menu/list-items/item-button-remap-";
    const int underPc = keybindRemapWidgetsUnderPc();
    size_t moved = 0;
    if (underPc)
        for (const char* at = strstr(document, from); at; at = strstr(at + 1, from))
            ++moved;
    const size_t room = length + moved * (sizeof(to) - sizeof(from)) + 1;
    char* text = (char*)malloc(room);
    if (!text) {
        writeLog("PAGE no memory for %s, so no keybind row can be drawn", name);
        return;
    }
    size_t used = 0;
    const char* rest = document;
    for (const char* at = underPc ? strstr(rest, from) : nullptr; at;
         at = strstr(rest, from)) {
        memcpy(text + used, rest, (size_t)(at - rest));
        used += (size_t)(at - rest);
        memcpy(text + used, to, sizeof(to) - 1);
        used += sizeof(to) - 1;
        rest = at + sizeof(from) - 1;
    }
    const size_t tail = length - (size_t)(rest - document);
    memcpy(text + used, rest, tail + 1);
    used += tail;
    keepGeneratedPage(name, text, (uint32_t)used);
}

/* The keybind row's widget files, when a menupage has a keybind and keys can
   be set from its row in this build. */
static void generateKeybindRow() {
    if (!keybindsCanBeSet()) return;
    int any = 0;
    for (int i = 0; i < global_modItemCount && !any; ++i)
        any = !global_modItems[i].refused
           && global_modItems[i].controlType == MODPAGE_KEYBIND;
    if (!any) return;
    keepKeybindFile(KEYBIND_ROW_NAME, global_keybindRowDocument,
                    sizeof(global_keybindRowDocument) - 1);
    keepKeybindFile(KEYBIND_REF_NAME, global_keybindRefDocument,
                    sizeof(global_keybindRefDocument) - 1);
    keepKeybindFile(KEYBIND_CELL_NAME, global_keybindCellDocument,
                    sizeof(global_keybindCellDocument) - 1);
}

static void noteSettingsOnNoPage();

static void generateMenuPages() {
    if (!global_modPageCount) return;
    generateKeybindRow();
    const MenuFile* fallback = menuFileNamed(PAGE_TEMPLATE_NAME);
    if (!fallback || !fallback->text) {
        writeLog("PAGE %s is not in %s, so no page can be generated",
                 PAGE_TEMPLATE_NAME, TEMPLATE_FOLDER);
        noteSettingsOnNoPage();
        return;
    }
    for (int p = 0; p < global_modPageCount; ++p) {
        const ModPage* page = &global_modPages[p];
        char name[MODPAGE_NAME * 2 + 16];
        if (page->replacesPage[0])
            strcpy_s(name, sizeof(name), page->replacesPage);
        else
            sprintf_s(name, "mods/page-%s", page->name);

        const MenuFile* built = page->menuTemplate[0]
                              ? menuFileNamed(page->menuTemplate) : fallback;
        if (!built) {
            writeLog("PAGE %s asks for the template \"%s\", which is not in %s, "
                     "so %s was used", page->name, page->menuTemplate,
                     TEMPLATE_FOLDER, PAGE_TEMPLATE_NAME);
            built = fallback;
        }
        generateOnePage(p, -1, name, page->title, built);
    }
    generateLandingPage(fallback);
    noteSettingsOnNoPage();
}

/* ------------------------------------------------------- registering a name */

/*
    0243A180 is find or create. It takes the table every widget file lives in, a
    name, and a document:

        registrar(rcx the file table, rdx the name, r8 the document) -> the file

    It hashes the name and looks it up. A name already in the table is built if
    it is not built already. A name that is not there gets a new index and is
    built into it.

    Exactly two functions call the reader, and the registrar is the one that
    calls it twice, once for each branch.
*/
#define MAX_READER_CALLERS   8

typedef uint64_t (*RegisterFileFn)(void* table, const char* name,
                                   void* document);

/* The loader's trampoline for the registrar: the whole original, without the
   hook in front of it. */
static uint64_t global_registerFile = 0;

static uint64_t findWidgetFileRegistrar(uint64_t reader) {
    if (!global_textBase || !global_textSize || !reader) return 0;

    uint8_t* starts[MAX_READER_CALLERS];
    int counts[MAX_READER_CALLERS], found = 0;
    for (uint8_t* site = nextCallTo(reader, 0); site;
         site = nextCallTo(reader, (uint64_t)site - global_textBase + 1)) {
        uint8_t* start = functionStartBefore(site);
        if (!start) continue;
        int at = 0;
        while (at < found && starts[at] != start) ++at;
        if (at == found) {
            if (found >= MAX_READER_CALLERS) continue;
            starts[found] = start;
            counts[found++] = 0;
        }
        ++counts[at];
    }

    int chosen = -1, twice = 0;
    for (int k = 0; k < found; ++k)
        if (counts[k] >= 2) { chosen = k; ++twice; }
    if (twice != 1) {
        writeLog("MENU %d function(s) call the reader more than once, so the "
                 "registrar cannot be told apart", twice);
        return 0;
    }
    return (uint64_t)starts[chosen];
}

/*
    Puts one name that is ours alone into the table per call. Runs from inside
    the registrar, where the engine memory scope the parser allocates through is
    live. The nested call goes through the loader's trampoline, not back through
    this hook.
*/
#define MAX_REGISTER_ATTEMPTS 4

static void registerMenuFiles() {
    if (!global_widgetFileTable || global_menuFileCount <= 0) return;
    if (InterlockedCompareExchange(&global_registerBusy, 1, 0) != 0) return;

    for (int i = 0; i < global_menuFileCount; ++i) {
        MenuFile* entry = &global_menuFiles[i];
        if (entry->seen || entry->registered
            || entry->attempts >= MAX_REGISTER_ATTEMPTS) continue;
        ++entry->attempts;

        if (!global_registerFile) {
            writeLog("MENU the loader has no trampoline for the registrar, so "
                     "\"%s\" cannot be added", entry->name);
            break;
        }

        /* The reader runs for this name inside the nested call and must find it
           already claimed. */
        InterlockedExchange(&entry->used, 1);
        const uint64_t document = parseMenuFile(entry, entry->name);
        if (!document) {
            InterlockedExchange(&entry->used, 0);
            break;
        }

        uint64_t file = 0;
        int ok = 0;
        GUARD_TRY {
            file = ((RegisterFileFn)global_registerFile)(
                (void*)global_widgetFileTable, entry->name, (void*)document);
            ok = 1;
        } GUARD_EXCEPT_DESCRIBING("registering a widget file of our own") {
            ok = 0;
        }
        if (ok && file) InterlockedExchange(&entry->registered, 1);
        else writeLog("MENU \"%s\" %s", entry->name,
                      ok ? "was not registered: the registrar answered nothing"
                         : "faulted while being registered");
        break;
    }
    InterlockedExchange(&global_registerBusy, 0);
}

static void widgetFileRegistrarHook(void* registers, void*) {
    if (!global_widgetFileTable)
        global_widgetFileTable = loaderHookRegister(registers, LOADER_REG_RCX);
    registerMenuFiles();
}

/* -------------------------------------------------------------- data sources */

/*
    Menu2::DataSourceManager::CreateDataSource is how a name in a widget's
    dataSource tag becomes an object the engine reads values from.

        CreateDataSource(rcx the manager, rdx a class id, r8 an instance id,
                         r9 a third id, [rsp+0x28] a fourth id,
                         [rsp+0x30] a flag) -> the data source

    It looks the instance id up first and returns what it finds; only when there
    is nothing does it build one, by finding the class id in a registry,
    allocating through a pointer in the class record and calling the object's
    first vtable slot. An unknown class id is reported by the engine itself:
    "Invalid typeId %s - maybe forgot MENU2_DATA_SOURCE_DECLARE".

    A source of ours is made by calling it ourselves.

    The ids are hashes of names, the same FNV-1a used for widget names and jdom
    keys.
*/

#define MAX_FRAMEWORK_SOURCES 512

#define MAX_SOURCE_TEXT        64

/*
    The class each kind of row is built from. Every method that reads the
    instance id is answered by us; these supply the interface a Menu2::Selector
    or a Menu2::Slider widget expects to be talking to.
*/
#define SELECTOR_CLASS "Menu2::DataSourceSelector"
#define SLIDER_CLASS   "Menu2::DataSourceSlider"

/* How many named options a selector may offer. */
#define MAX_OPTIONS            12

/*
    What a row is. A toggle is a selector of two. A list composes the values
    of the controls in it and draws no row. A button opens a page: it holds no
    value and is never written to a store, and its title is given as a plain
    string the same way a control's is.
*/
#define KIND_TOGGLE             0
#define KIND_SELECTOR           1
#define KIND_SLIDER             2
#define KIND_LIST               3
#define KIND_BUTTON             4
#define KIND_PRESET             5   /* a selector of presets; keeps no value */
#define KIND_KEYBIND            6   /* a key, kept as its virtual-key code in
                                       hexadecimal; its row is a button, or
                                       where keys cannot be set a selector
                                       (keyShownOnly) */

/*
    A setting of ours. The engine's own CreateDataSource builds it as an object
    of a class the engine already knows, registered in the map a widget's
    dataSource tag is looked up in.

        name         what a widget's dataSource tag says
        id           the FNV-1a of that name, which the engine matches on
        className    the engine class to build, by name. TypeFactory holds one
                     record per class and matches on the hash of the name
        classId      the FNV-1a of that name
        title        what the row is called. Not a localisation key: a row's
                     title is one of the values slot 9 hands back, a plain
                     string with tag 0x13, displayed as it is written
        description  what the pane on the right says. Plain text like a title;
                     the pane asks for it by id, and a setting answers
                     with the hash of the text
        storeKey     the key this setting is kept under in the store
        object       what the engine built
        vtable       the table it was built with, before ours replaces it
*/
struct OurSetting {
    char     name[MAX_SOURCE_TEXT];
    char     className[MAX_SOURCE_TEXT];
    char     title[MAX_SETTING_TEXT];
    char     storeKey[MAX_SOURCE_TEXT];
    char     storeSection[MAX_SOURCE_TEXT];
    char     storePath[MAX_PATH];
    char     description[MAX_DESCRIPTION_TEXT];
    char     dependencyDescription[MAX_DESCRIPTION_TEXT];
    char     option[MAX_OPTIONS][MAX_SETTING_TEXT];      /* what a row shows */
    char     optionValue[MAX_OPTIONS][MAX_SETTING_TEXT]; /* what the store keeps */
    char     dependency[MODPAGE_DEPENDENCY];
    char     listValue[MAX_SETTING_TEXT];
    char     shown[MAX_SETTING_TEXT];  /* what the row reads before it is touched */
    char     fallbackText[MAX_SETTING_TEXT];   /* DefaultValue, as the file writes it */
    int      optionCount;
    int      markAsModified;
    int      html;             /* the description text is markup as written */
    int      dependencyIsOurs; /* the wording under a lock was made here */
    int      listOwner;                /* the list this one belongs to, or -1 */
    int      gatingList;               /* the list whose dependency also applies */
    uint64_t rowRecord;                /* the heap record of the row it draws in, or 0 */
    uint32_t rowId;                    /* that row's id */
    int      linkedSetting;            /* the control this one carries, or -1 */
    int      modItem;                  /* the block it was made from */
    /* What the menupage wrote, where it wrote an expression rather than a
       literal. Empty means the field beside it is what the file said. */
    char     keyExpression[MODPAGE_EXPRESSION];
    char     defaultExpression[MODPAGE_EXPRESSION];
    char     titleExpression[MODPAGE_EXPRESSION];
    char     descriptionExpression[MODPAGE_EXPRESSION * 4];
    int      mismatch;                 /* the store held something unusable */
    /* Whether the setting name changed the last time it was worked out, and
       whether the control was asked to draw itself again with no row to draw
       in. Answered by ModMenuDescribeControl. */
    int      keyFollowed;
    int      hadNoRow;
    int      touched;
    int      locked;                   /* the last LOCKED_BY_ answer given */
    /* That answer was not applied to a key of its settings file, as the
       write was put off while Init ran (writeIsPutOff). settleLocks and
       GetAvailability apply it again. */
    int      lockPutOff;
    float    least;
    float    greatest;
    float    step;
    float    fallback;
    int      logarithmic;              /* steps on the logarithmic scale */
    float    smallestStep;             /* its finest step there, 0 for none */
    int      optionCapacity;           /* the most options it can offer */
    int      hidden;                   /* the last answer the hidden slot gave */
    /* Its limits, its option count or its value moved while being worked out
       again, and its row has not been drawn again since. */
    int      rangeMoved;
    /* Its value was brought inside its limits or its option count and has not
       been written to the store since. */
    int      unsaved;
    /* Slot 20 answered shown after answering hidden, and the row has not been
       drawn again since. The engine chooses a row's look before it clears the
       hidden bit, and draws a hidden row as disabled, so a row that is shown
       must be drawn once more. */
    int      shownAgain;
    /* A button: the row on a page of the game that opens the landing page;
       and the page it is on, by its heading or, where it has none, its
       name, by which and its title the log names it. */
    int      entryRow;
    char     onPage[MAX_SETTING_TEXT];
    /* No widget file of ours names it as a data source, so no page draws its
       row and no data source is made for it (noteSettingsOnNoPage). */
    int      onNoPage;
    int      kind;
    int      index;                    /* which option a row sits on */
    /* A preset: the store writes its shown option was worked out at, and the
       preset last chosen, which is asked first, or -1. */
    LONG     presetSeen;
    int      presetChosen;
    /* A keybind: the row its cell was last filled for, the key and the focus
       it was filled with, and the parts of the cell found there; and whether
       it may take a key the game binds in its menus, and outside them. */
    uint64_t keybindRow;
    int      keybindKey;
    int      keybindFocus;
    int      allowMenuKeys;
    int      allowGameplayKeys;
    uint64_t keybindIcon;
    uint64_t keybindName;
    uint64_t keybindFrame;
    uint64_t keybindTexts;
    float    value;
    float    asFloat;
    uint64_t titleKeyId;
    uint64_t descriptionKeyId;
    uint64_t id;
    uint64_t classId;
    uint64_t object;
    uint64_t vtable;
};

static OurSetting    global_ourSources[MAX_FRAMEWORK_SOURCES];
static int           global_ourSourceCount = 0;

/*
    The engine keeps its data sources in a slot pool of a fixed size, and a
    source made when the pool is full ends the process. The game fills the pool
    nearly to the top by itself: in Part I it takes 531 of the 541 nodes, and
    the build's safeFrameworkSources (6) of ours fit in the rest with a few to
    spare; in Part II it takes 752 of the 809 once the player menu has opened,
    and none of ours is made unless the pool is enlarged.

    maxFrameworkSources is how many to make, read from the settings file.
    Before the first is made, enlargeDataSourcePool lowers it, where it asks
    for more, to what the pool has room for: the safe number where the pool
    is left as the engine made it, the nodes added and the safe number where
    it is enlarged, and none where the pool may be half changed. Past it, a
    setting is left without a source: its row shows the text its widget was
    authored with.
*/
static int           global_maxFrameworkSources = 0;

/* Whether maxFrameworkSources was lowered to what the pool has room for. The
   log then already says how many settings of ours that leaves out, where it
   leaves any out. */
static int           global_poolLimitedSettings = 0;

static void giveOurSettingsOwnVtable();
static volatile LONG global_settingsMade = 0;

/* Whether a setting holds a value, which a list, a button and a preset do
   not: a preset shows the preset its settings match. */
static int holdsValue(const OurSetting* s) {
    return s->kind != KIND_LIST && s->kind != KIND_BUTTON
        && s->kind != KIND_PRESET;
}

/*
    Whether s is a keybind whose key cannot be set in this build
    (keybindsCanBeSet). Its row is a selector of one option, the key's name;
    the framework never writes its key, whether its row is used, its page is
    reset, a preset names it or it locks and unlocks; a reset never counts
    it as one to put back.
*/
static int keyShownOnly(const OurSetting* s) {
    return s && s->kind == KIND_KEYBIND && !keybindsCanBeSet();
}

/*
    Every setting of ours that no widget file of ours names in a dataSource
    tag is marked as on no page. A generated page that was not kept names
    none, and the page of a submenu whose page name does not fit is not
    generated.
*/
static void noteSettingsOnNoPage() {
    static const char tag[] = "\"dataSource\":\"";
    for (int i = 0; i < global_ourSourceCount; ++i)
        global_ourSources[i].onNoPage = 1;
    for (int f = 0; f < global_menuFileCount; ++f) {
        const char* text = global_menuFiles[f].text;
        for (const char* at = text ? strstr(text, tag) : nullptr; at;
             at = strstr(at, tag)) {
            at += sizeof(tag) - 1;
            const char* end = strchr(at, '"');
            if (!end) break;
            const size_t length = (size_t)(end - at);
            for (int i = 0; i < global_ourSourceCount; ++i) {
                OurSetting* s = &global_ourSources[i];
                if (strlen(s->name) == length
                    && memcmp(s->name, at, length) == 0)
                    s->onNoPage = 0;
            }
            at = end;
        }
    }
}

/* ------------------------------------------------------------- the store */

/*
    Where a setting of ours keeps its value: the ini its menupage names with
    ConfigurationFile, one key per setting, in the section its block names or
    under [settings] when it names none.
*/
#define STORE_SECTION "settings"

static const char* storeSectionOf(const char* section) {
    return section && section[0] ? section : STORE_SECTION;
}

/* A value is text on the way in and on the way out. */
static void storedText(const char* path, const char* section, const char* key,
                       const char* fallback, char* out, size_t room) {
    out[0] = 0;
    if (!path || !path[0] || !key || !key[0]) {
        if (fallback) strcpy_s(out, room, fallback);
        return;
    }
    GetPrivateProfileStringA(storeSectionOf(section), key,
                             fallback ? fallback : "", out, (DWORD)room, path);
    const char* first = out;
    const char* last = out + strlen(out);
    trimSpan(&first, &last);
    memmove(out, first, (size_t)(last - first));
    out[last - first] = 0;
}

/* Counts every write to a store, so a preset works out the preset its
   settings match only after something may have changed. */
static volatile LONG global_storeWrites = 0;

/* Where Mod_Settings_Cache.ini is, empty where there is no folder for it. */
static char global_settingsCachePath[MAX_PATH];

/*
    The files a write to failed, each kept as hashText of its path with how
    many lines were written about it, so that each file has up to
    WRITE_LINES_PER_FILE of them. Past MAX_FAILING_FILES files, one line says
    that no more are written.
*/
#define MAX_FAILING_FILES     32
#define WRITE_LINES_PER_FILE  16

static uint64_t      global_failingFile[MAX_FAILING_FILES];
static volatile LONG global_failingFileLines[MAX_FAILING_FILES];
static volatile LONG global_failingFileCount = 0;
static volatile LONG global_failingFilesFull = 0;

/* 1 while Init runs installHooks. */
static int global_starting = 0;

/* 1 where Windows answers no attributes for path, as for a file that does
   not exist, or where the file there holds 0 bytes. */
static int fileIsMissingOrEmpty(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA about;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &about)) return 1;
    return !(about.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        && !about.nFileSizeLow && !about.nFileSizeHigh;
}

/* Whether a write to the file at path is put off: while Init runs
   installHooks, a settings file that is missing or empty
   (fileIsMissingOrEmpty) is not written. A write to Mod_Settings_Cache.ini
   never is. */
static int writeIsPutOff(const char* path) {
    return global_starting && path && path[0]
        && _stricmp(path, global_settingsCachePath) != 0
        && fileIsMissingOrEmpty(path);
}

static void forgetPutAsideOfNewFiles(int keepMissing = 0);

/* value written under key, or the key taken out where value is nullptr.
   Answers 0 where the file could not be written, or where the write is put
   off (writeIsPutOff), which writes nothing and no line; and 1 otherwise,
   including where there is no file or no key to write to. Once Init is over,
   the settings files noted as new are looked at before anything is written
   (forgetPutAsideOfNewFiles). */
static int keepText(const char* path, const char* section, const char* key,
                    const char* value) {
    if (!path || !path[0] || !key || !key[0]) return 1;
    if (!global_starting) forgetPutAsideOfNewFiles();
    InterlockedIncrement(&global_storeWrites);
    if (writeIsPutOff(path)) return 0;
    if (WritePrivateProfileStringA(storeSectionOf(section), key, value, path))
        return 1;
    const DWORD error = GetLastError();
    const uint64_t file = hashText(path);
    LONG at = 0;
    const LONG count = global_failingFileCount;
    while (at < count && global_failingFile[at] != file) ++at;
    if (at == count && count < MAX_FAILING_FILES) {
        global_failingFile[at] = file;
        global_failingFileLines[at] = 0;
        InterlockedIncrement(&global_failingFileCount);
    }
    if (at < MAX_FAILING_FILES) {
        if (InterlockedIncrement(&global_failingFileLines[at])
            <= WRITE_LINES_PER_FILE)
            writeLog("STORE %s could not be written to %s, error %lu", key,
                     path, error);
        else InterlockedDecrement(&global_failingFileLines[at]);
    } else if (InterlockedExchange(&global_failingFilesFull, 1) == 0)
        writeLog("STORE %d files could not be written, so a failure to write "
                 "any other file is not logged", MAX_FAILING_FILES);
    return 0;
}

/* A number as short as it can be written, so 3 is "3" and 2.5 is "2.5". */
static void writeNumber(char* out, size_t room, float value) {
    sprintf_s(out, room, "%.4f", (double)value);
    size_t at = strlen(out);
    while (at > 1 && out[at - 1] == '0') out[--at] = 0;
    if (at > 1 && out[at - 1] == '.') out[--at] = 0;
}

/* The largest denominator a fraction is written with. */
#define LARGEST_DENOMINATOR 64

/* A number as a whole number or as a fraction in its lowest terms, so 1.25 is
   "5/4" and 0.5 is "1/2". One that no denominator up to LARGEST_DENOMINATOR
   gives exactly is written as writeNumber writes it. */
static void writeFraction(char* out, size_t room, double value) {
    for (int below = 1; value == value && value <= 1e12 && value >= -1e12
                        && below <= LARGEST_DENOMINATOR; ++below) {
        const double above = value * below;
        const long long whole = (long long)(above < 0 ? above - 0.5
                                                      : above + 0.5);
        const double off = above - (double)whole;
        if (off > 0.0001 || off < -0.0001) continue;
        if (below == 1) sprintf_s(out, room, "%lld", whole);
        else sprintf_s(out, room, "%lld/%d", whole, below);
        return;
    }
    writeNumber(out, room, (float)value);
}

static int sameNumber(float a, float b) {
    const float difference = a > b ? a - b : b - a;
    return difference < 0.0001f;
}

/*
    A comparison's answer, 1 or 0, from the four orderings of its two sides
    and whether they are equal; -1 where how is not one of =, !=, <, >, <= and
    >=.
*/
static int compared(const char* how, int equal, int less, int greater,
                    int atMost, int atLeast) {
    if (!strcmp(how, "="))  return equal;
    if (!strcmp(how, "!=")) return !equal;
    if (!strcmp(how, "<"))  return less;
    if (!strcmp(how, ">"))  return greater;
    if (!strcmp(how, "<=")) return atMost;
    if (!strcmp(how, ">=")) return atLeast;
    return -1;
}

/*
    The logarithmic scale a slider steps on with SliderStepSize: [Logarithmic].
    Below 10 the step is 0.1, and from each power of ten up it is a tenth of
    that power: 1 from 10 to 100, 10 from 100 to 1000, and so on. smallest is
    the finest step allowed, 0 for none. Negative values mirror positive ones.

    A value on the scale is a whole number of its step, and every power of ten
    is on it.
*/
#define MAX_SCALE_POSITIONS 100000

static double decadeStep(double magnitude, double smallest) {
    double step = 0.1;
    if (magnitude >= 10.0) {
        step = 1.0;
        for (double top = 100.0; magnitude >= top && top < 1e18; top *= 10.0)
            step *= 10.0;
    }
    return step < smallest ? smallest : step;
}

/* The power of ten a magnitude's step ends at: 10 below 10, and otherwise the
   first power of ten above it. */
static double decadeTop(double magnitude) {
    double top = 10.0;
    while (magnitude >= top && top < 1e18) top *= 10.0;
    return top;
}

/* The power of ten a magnitude's step begins at, 0 below 10. */
static double decadeBottom(double magnitude) {
    if (magnitude < 10.0) return 0.0;
    double bottom = 10.0;
    while (magnitude >= bottom * 10.0 && bottom < 1e17) bottom *= 10.0;
    return bottom;
}

/* A value on the scale with the error adding tenths leaves taken off. */
static double onScale(double value) {
    return floor(value * 1e6 + 0.5) / 1e6;
}

static double logStepDown(double value, double smallest);

/* The next value on the scale above value. */
static double logStepUp(double value, double smallest) {
    if (value < 0.0) return -logStepDown(-value, smallest);
    const double step = decadeStep(value, smallest);
    const double next = (floor(value / step + 1e-9) + 1.0) * step;
    const double top = decadeTop(value);
    return onScale(next > top ? top : next);
}

/* The next value on the scale below value. */
static double logStepDown(double value, double smallest) {
    if (value <= 0.0) return -logStepUp(-value, smallest);
    const double inside = value - value * 1e-9;
    const double step = decadeStep(inside, smallest);
    const double previous = (ceil(value / step - 1e-9) - 1.0) * step;
    const double bottom = decadeBottom(inside);
    return onScale(previous < bottom ? bottom : previous);
}

/* The value after here on a logarithmic slider that ends at greatest. */
static double logNext(double here, double greatest, double smallest) {
    const double next = logStepUp(here, smallest);
    return next < greatest ? next : greatest;
}

/* How many values a logarithmic slider has from least to greatest, both
   counted. Neither has to be on the scale. */
static int logPositions(double least, double greatest, double smallest) {
    int count = 1;
    for (double value = least;
         value < greatest - 1e-9 && count < MAX_SCALE_POSITIONS; ++count)
        value = logNext(value, greatest, smallest);
    return count;
}

/* The value at position, counted from least at 0. */
static double logValueAt(double least, double greatest, double smallest,
                         int position) {
    double value = least;
    for (int p = 0; p < position && value < greatest - 1e-9; ++p)
        value = logNext(value, greatest, smallest);
    return value;
}

/* Where value sits among the positions: a whole number on one, a fraction
   between the two it falls between, 0 below least and the last position
   above greatest. */
static double logPositionOf(double least, double greatest, double smallest,
                            double value) {
    if (value <= least) return 0.0;
    double here = least;
    int p = 0;
    for (; here < greatest - 1e-9 && p < MAX_SCALE_POSITIONS; ++p) {
        const double next = logNext(here, greatest, smallest);
        if (value < next - 1e-9) return p + (value - here) / (next - here);
        here = next;
    }
    return (double)p;
}

/* The positions a logarithmic slider of ours has now, and where a value of it
   sits among them. */
static int positionsOf(const OurSetting* s) {
    return logPositions(s->least, s->greatest, s->smallestStep);
}

static double positionOfValue(const OurSetting* s, double value) {
    return logPositionOf(s->least, s->greatest, s->smallestStep, value);
}

/* The value one step above value on a slider's own scale. */
static double stepAbove(const OurSetting* s, double value) {
    if (s->logarithmic) return logStepUp(value, s->smallestStep);
    return value + (s->step > 0.0f ? (double)s->step : 1.0);
}

/* One more value on a comma separated list, as much of it as there is room for. */
static void appendBounded(char* out, size_t room, const char* text) {
    if (!out || !room || !text || !text[0]) return;
    size_t at = strlen(out);
    if (at && at + 2 < room) { out[at++] = ','; out[at++] = ' '; out[at] = 0; }
    for (const char* p = text; *p && at + 1 < room; ++p) out[at++] = *p;
    out[at] = 0;
}

/* The setting of ours the engine built as this object, if any. */
static OurSetting* ourSourceByObject(uint64_t object) {
    if (!object) return 0;
    for (int i = 0; i < global_ourSourceCount; ++i)
        if (global_ourSources[i].object == object) return &global_ourSources[i];
    return 0;
}

static OurSetting* settingNamed(const char* path, const char* section,
                                const char* key) {
    OurSetting* loose = 0;
    for (int i = 0; i < global_ourSourceCount; ++i) {
        OurSetting* s = &global_ourSources[i];
        if (_stricmp(s->storeKey, key) != 0) continue;
        if (path && path[0] && _stricmp(s->storePath, path) != 0) continue;
        if (section && section[0] && _stricmp(s->storeSection, section) != 0) {
            if (!loose) loose = s;
            continue;
        }
        return s;
    }
    return loose;
}

/*
    A name written section.key, held in whole, split at its first dot, or at
    its last where last is set. A name without a dot is read in the section of
    the control that asks, where there is one. Answers the key. A section cut
    short is logged as a capped line (copyBounded's capped).
*/
static const char* splitSettingName(char* whole, int last,
                                    const OurSetting* owner, char* section,
                                    size_t room, const char* what) {
    char* dot = last ? strrchr(whole, '.') : strchr(whole, '.');
    section[0] = 0;
    if (dot) {
        *dot = 0;
        copyBounded(section, room, whole, what, 1);
        return dot + 1;
    }
    if (owner) copyBounded(section, room, owner->storeSection, what, 1);
    return whole;
}

/*
    A dependency, as the menu file writes it:

        examplesetting1 = 1
        (examplesetting1 = 1) AND (examplesetting2 < 3)
        (settings.examplesetting1 = 0) OR NOT (settings.examplesetting2 = -4)

    Comparisons are =, !=, <, >, <= and >=. The left side names a setting and
    the right side is the value to compare it with. = and != compare a number
    as a number and anything else as text, ignoring case; the others compare
    numbers.

    The logic functions, tightest first:

        NOT                 one operand, written in front of it
        AND   NAND          both true, and its negation
        XOR   XNOR          exactly one true, and its negation
        OR    NOR           either true, and its negation

    Each binds left to right within its own level, and brackets override the
    order. The words are matched ignoring case and must stand alone, so a
    setting named "android" is a name and not an AND.

    A name with no section is looked for in the section the control that needs
    it lives in. A dependency naming something that matches nothing does not
    hold, and the log says so.
*/
struct DependencyCursor {
    const char* at;
    const char* end;
    const OurSetting* owner;
    int faults;
};

/* The words of the three levels that join two operands, tightest first: the
   negation of each, then the word itself. */
static const char* const global_logicPair[3][2] = {
    { "NAND", "AND" }, { "XNOR", "XOR" }, { "NOR", "OR" }
};

/*
    Whether word stands at from, as a whole word. A word ends at the end of the
    text, at a space, or at a bracket. After is where the text carries on.
*/
static int wordStandsAt(const char* from, const char* end, const char* word,
                        const char** after) {
    if (!spelledAt(from, end, word)) return 0;
    const char* past = from + strlen(word);
    if (past < end && (unsigned char)*past > ' ' && *past != '(' && *past != ')')
        return 0;
    if (after) *after = past;
    return 1;
}

/* Whether a logic function begins at from, once any spaces are stepped over. */
static int logicWordAt(const char* from, const char* end) {
    while (from < end && (unsigned char)*from <= ' ') ++from;
    if (wordStandsAt(from, end, "NOT", 0)) return 1;
    for (const auto& pair : global_logicPair)
        if (wordStandsAt(from, end, pair[0], 0)
            || wordStandsAt(from, end, pair[1], 0)) return 1;
    return 0;
}

/*
    Text up to the next bracket, the next logic function, or one of the
    characters in stopAt. Spaces inside are kept; spaces at either end are not.
*/
static void readDependencyText(DependencyCursor* c, const char* stopAt,
                               char* out, size_t room) {
    size_t at = 0;
    skipBlanks(c);
    for (; c->at < c->end && *c->at != ')' && *c->at != '('; ++c->at) {
        if (strchr(stopAt, *c->at)) break;
        if ((unsigned char)*c->at <= ' ' && logicWordAt(c->at, c->end)) break;
        if (at + 1 < room) out[at++] = *c->at;
    }
    while (at && (unsigned char)out[at - 1] <= ' ') --at;
    out[at] = 0;
}

static float settingNumber(const OurSetting* s) {
    if (s->kind == KIND_SLIDER) return s->value;
    if (s->index >= 0 && s->index < s->optionCount)
        return (float)atof(s->optionValue[s->index]);
    return s->value;
}

/* The comparison operator at the cursor, stepped over: its first character
   and an "=" after it. */
static void readOperator(const char** at, const char* end, char how[3]) {
    how[0] = how[1] = how[2] = 0;
    if (*at < end && **at && strchr("=<>!", **at)) {
        how[0] = *(*at)++;
        if (*at < end && **at == '=') how[1] = *(*at)++;
    }
}

static int readComparison(DependencyCursor* c) {
    char name[MODPAGE_NAME * 2] = {0};
    readDependencyText(c, "=<>!", name, sizeof(name));

    char section[MODPAGE_NAME];
    const char* key = splitSettingName(name, 0, c->owner, section,
                                       sizeof(section), "dependency section");
    char how[3];
    skipBlanks(c);
    readOperator(&c->at, c->end, how);
    char wanted[MODPAGE_VALUE] = {0};
    readDependencyText(c, "", wanted, sizeof(wanted));

    const OurSetting* other =
        settingNamed(c->owner ? c->owner->storePath : 0, section, key);
    if (!other) { ++c->faults; return 0; }

    char heldNumber[32];
    const char* held = other->kind == KIND_SLIDER || other->index < 0
                     ? 0 : other->optionValue[other->index];
    if (!held) { writeNumber(heldNumber, sizeof(heldNumber), other->value); held = heldNumber; }

    const float left = settingNumber(other);
    const float right = (float)atof(wanted);
    const int numeric = (wanted[0] == '-' || wanted[0] == '.'
                         || (wanted[0] >= '0' && wanted[0] <= '9'));
    const int answer = compared(how, numeric ? sameNumber(left, right)
                                             : !_stricmp(held, wanted),
                                left < right, left > right, left <= right,
                                left >= right);
    if (answer < 0) ++c->faults;
    return answer > 0;
}

/* Whether the named function stands next. It is stepped over when it does. */
static int takeLogicWord(DependencyCursor* c, const char* word) {
    skipBlanks(c);
    const char* past = 0;
    if (!wordStandsAt(c->at, c->end, word, &past)) return 0;
    c->at = past;
    return 1;
}

static int readLogic(DependencyCursor* c, int level);

/*
    True and False stand where a comparison would. A setting of either spelling
    cannot be compared against.
*/
static int readFactor(DependencyCursor* c) {
    if (takeLogicWord(c, "NOT")) return !readFactor(c);
    if (takeLogicWord(c, "True"))  return 1;
    if (takeLogicWord(c, "False")) return 0;
    skipBlanks(c);
    if (c->at < c->end && *c->at == '(') {
        ++c->at;
        const int answer = readLogic(c, 2);
        skipBlanks(c);
        if (c->at < c->end && *c->at == ')') ++c->at;
        return answer;
    }
    return readComparison(c);
}

/* One level of the joining words, 0 AND, 1 XOR and 2 OR, each operand a level
   tighter. */
static int readLogic(DependencyCursor* c, int level) {
    int answer = level ? readLogic(c, level - 1) : readFactor(c);
    for (;;) {
        const int negated = takeLogicWord(c, global_logicPair[level][0]);
        if (!negated && !takeLogicWord(c, global_logicPair[level][1]))
            return answer;
        const int right = level ? readLogic(c, level - 1) : readFactor(c);
        const int result = level == 0 ? right && answer
                         : level == 1 ? right != answer
                                      : right || answer;
        answer = negated ? !result : result;
    }
}

/*
    Whether a control may be used. A control with no dependency always may.

    The count caps the lines written about a dependency that names something
    the page does not have.
*/
static volatile LONG global_dependencyFaultLines = 0;

static int dependencyHolds(const OurSetting* s) {
    if (!s->dependency[0]) return 1;
    DependencyCursor c = { s->dependency, s->dependency + strlen(s->dependency),
                           s, 0 };
    const int answer = readLogic(&c, 2);
    if (c.faults && InterlockedIncrement(&global_dependencyFaultLines) <= 16)
        writeLog("SETTING \"%s\" depends on \"%s\", which names something this "
                 "page does not have, so the control is locked", s->name,
                 s->dependency);
    return c.faults ? 0 : answer;
}

/*
    Why a control cannot be used. A control is locked by the list it sits in
    before it is locked by anything of its own, and the list's dependency
    description is then the one shown.
*/
#define LOCKED_BY_NOTHING   0
#define LOCKED_BY_ITS_LIST  1
#define LOCKED_BY_ITSELF    2

static int whyLocked(const OurSetting* s) {
    if (s->gatingList >= 0 && s->gatingList < global_ourSourceCount
        && !dependencyHolds(&global_ourSources[s->gatingList]))
        return LOCKED_BY_ITS_LIST;
    return dependencyHolds(s) ? LOCKED_BY_NOTHING : LOCKED_BY_ITSELF;
}

/*
    A value from the store, or from the cache, taken into the control. A slider
    keeps the number; a selector and a toggle keep the position of the option
    whose value matches, and fall back to the position of the default when
    nothing matches. complain writes a line when the text cannot be shown as it
    stands: 1 the first time a value is read, 0 when one is put back.
*/
/* The Windows virtual-key code a keybind's text names, written in
   hexadecimal with or without 0x in front (F9 is 0x78); 0 for none. */
static int keybindKeyOf(const char* text) {
    if (!text) return 0;
    const char* at = pastBlanks(text);
    char* end = 0;
    const unsigned long key = strtoul(at, &end, 16);
    if (!end || end == at || *pastBlanks(end)) return 0;
    return key >= 0x01 && key <= 0xFE ? (int)key : 0;
}

static void takeValue(OurSetting* s, const char* held, int complain) {
    s->unsaved = 0;
    if (s->kind == KIND_KEYBIND) {
        const int key = keybindKeyOf(held);
        s->mismatch = held[0] && !key;
        if (s->mismatch && complain)
            writeLog("STORE \"%s\" holds \"%s\", which is not a key's "
                     "virtual-key code in hexadecimal: it shows its default "
                     "until a key is set", s->name, held);
        const int shown = key ? key : keybindKeyOf(s->fallbackText);
        s->value = (float)shown;
        s->asFloat = s->value;
        s->index = 0;
        sprintf_s(s->shown, sizeof(s->shown), "0x%02X", shown);
        return;
    }
    if (s->kind == KIND_SLIDER) {
        s->value = held[0] ? (float)atof(held) : s->fallback;
        s->mismatch = held[0] && (s->value < s->least || s->value > s->greatest);
        if (s->mismatch && complain)
            writeLog("STORE \"%s\" holds %s, which is outside the %g to %g "
                     "this control covers: the number is shown as it stands "
                     "and the bar is drawn at its end", s->name, held,
                     (double)s->least, (double)s->greatest);
        s->asFloat = s->value;
        if (held[0]) strcpy_s(s->shown, sizeof(s->shown), held);
        else writeNumber(s->shown, sizeof(s->shown), s->value);
        return;
    }
    /* Every option the control can offer, not only those its count allows at
       the moment: a value past the count is brought inside it by
       refreshLimits. */
    const int options = s->optionCapacity > s->optionCount ? s->optionCapacity
                                                           : s->optionCount;
    int fallbackAt = 0;
    s->index = -1;
    for (int o = 0; o < options; ++o) {
        if (s->index < 0 && !_stricmp(s->optionValue[o], held)) s->index = o;
        if (!_stricmp(s->optionValue[o], s->fallbackText)) fallbackAt = o;
    }
    if (s->index >= 0) s->mismatch = 0;
    else {
        if (held[0]) {
            s->mismatch = 1;
            if (complain)
                writeLog("STORE \"%s\" holds \"%s\", which is not one of the "
                         "%d value(s) this control offers: it shows its "
                         "default until it is used", s->name, held,
                         s->optionCount);
        }
        s->index = fallbackAt;
    }
    s->value = (float)s->index;
    s->asFloat = s->value;
    if (s->index < options)
        strcpy_s(s->shown, sizeof(s->shown), s->optionValue[s->index]);
}

/*
    Mod_Settings_Cache.ini, beside this plugin's own settings file. One section
    per mod, named after the settings file the mod keeps its values in, and two
    keys for each setting a control can reach:

        <section>.<setting>.stored    the value put aside while it is locked
        <section>.<setting>.default   the default written to the settings
                                      file in its place

    When a control locks, what the settings file holds, or the default where
    it holds nothing, is written to .stored, unless the value already there
    belongs to the lock (readPutAside says when it does); then the default is
    written to the settings file and to .default, each where it does not hold
    it already. Where either cannot be written, the settings file is left
    holding what it held and the control keeps its value while locked. A
    preset chosen while a control that put a value aside is locked writes its
    value to .stored where the value there belongs to the lock or the control
    has a default, and the default as a lock does where the value there did
    not belong to the lock or the settings file does not hold the default, as
    when it could not be written when the control locked; where the default
    cannot be written to the settings file then, the unlock takes what the
    settings file holds, not the preset's value. A reset writes the
    default to .stored. When the control unlocks, the value in .stored is
    written back to the settings file where it belongs to the lock, and both
    keys are taken out. They stay after an unlock that could not write the
    value back, and a control that is still locked when the game is closed
    finds them again on the next run. While Init runs installHooks, a lock,
    or an unlock that would write a value back, whose settings file is
    missing or empty (writeIsPutOff), leaves both files as they are and the
    control as it is; the lock state is applied again once Init is over, the
    first time the control is asked whether it is available or the locks are
    settled after a change. A preset keeps the values it puts aside in the
    same section, under <preset>.custom.<setting>, with <preset>.custom set to
    1 once it has.

    The keys ending in .stored and .default of a settings file that was
    missing or empty when the framework started are taken out once the mod
    has written it (forgetPutAsideOfNewFiles), and those no control reaches
    are taken out at start (forgetUnreachedPutAside).
*/
#define SETTINGS_CACHE_NAME "Mod_Settings_Cache.ini"

static void buildSettingsCachePath() {
    const char* slash = strrchr(global_iniPath, '\\');
    if (!slash || !formatInto(global_settingsCachePath,
                              sizeof(global_settingsCachePath), "%.*s\\%s",
                              (int)(slash - global_iniPath), global_iniPath,
                              SETTINGS_CACHE_NAME))
        global_settingsCachePath[0] = 0;
}

/* The mod a setting belongs to, named after its settings file without the
   extension, and the two keys that control's values are kept under. */
struct CacheNames {
    char section[MAX_PATH];
    char defaultKey[MAX_SOURCE_TEXT * 2 + 16];
    char storedKey[MAX_SOURCE_TEXT * 2 + 16];
};

/* The cache section of the controls whose settings file is at storePath: the
   file's name without its extension, empty for no settings file. */
static void cacheSectionOf(const char* storePath, char* out, size_t room) {
    const char* slash = strrchr(storePath, '\\');
    copyBounded(out, room, slash ? slash + 1 : storePath, "cache section");
    char* dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

static void cacheNames(const OurSetting* s, const char* key, CacheNames* out) {
    cacheSectionOf(s->storePath, out->section, sizeof(out->section));
    sprintf_s(out->defaultKey, "%s.%s.default", s->storeSection, key);
    sprintf_s(out->storedKey, "%s.%s.stored", s->storeSection, key);
}

/*
    Every settings file key one control can address, with the default that
    belongs to each.

    A control whose setting name is written as an expression addresses a
    different key as the controls that expression reads change, and only one of
    them at a time. Each selector the expression reads is stepped through its
    own values, and for every one of them the name and the default are worked
    out together. The stepping is held for this thread alone
    (global_steppedControl), and the selectors are not changed.

    The key the control is reading now is always the first answered.

    Where an expression cannot be worked out here, no line says so. A key or
    a default worked out longer than the room for it is cut short as
    refreshExpressions cuts it, with cutToRoom's one line for each control
    and text.

    everyOption 1 also answers the key the name gives now, steps a toggle as
    well as a selector, answers each option's key whether or not the
    settings file holds it, and answers a key whose default cannot be worked
    out, with an empty default. complete, where given, receives 1 where the
    keys answered are every key the name can give: the name is written
    outright, or it was worked out, read one selector (or toggle, with
    everyOption) and nothing else for each of that control's options, read
    no key of a settings file that no control holds, and every key fitted in
    room; and 0 otherwise.

    Answers how many were filled in, which is 1 for a control whose name is
    written outright.
*/
/* Room for the controls one expression is noted as having read. An expression
   reading more than this is worked out no differently; only the note is cut. */
#define EXPRESSION_READS 8

#define MAX_KEYS_OF_ONE 16

/* The trailing three say which controls the expression read, and may be left
   out. */
static int evaluateExpression(const char* text, const OurSetting* owner,
                              char* out, size_t room,
                              const OurSetting** read = nullptr,
                              int* readCount = nullptr, int readRoom = 0);

/* expression, a setting key's where isKey is 1 and a default's where it is
   0, worked out for s into out, which has room bytes, with no line where it
   cannot be worked out, and cut to fit as refreshExpressions cuts it. read,
   readCount and readRoom are as evaluateExpression takes them. Answers
   whether it was worked out. */
static int workOutQuietly(const OurSetting* s, const char* expression,
                          int isKey, char* out, size_t room,
                          const OurSetting** read = nullptr,
                          int* readCount = nullptr, int readRoom = 0);

struct KeyAndDefault {
    char key[MAX_SOURCE_TEXT];
    char fallback[MAX_SETTING_TEXT];
};

/* Counts every name an expression has read from a settings file, as no
   control of that name holds it. */
static volatile LONG global_namesReadFromStore = 0;

/* For each thread, the selector or toggle everyKeyOf is stepping through its
   options, null for none, and the option it stands at, counted from 0: an
   expression worked out on that thread reads that option's value for it
   (valueOfName) in place of the one the control holds. */
static thread_local const OurSetting* global_steppedControl = nullptr;
static thread_local int               global_steppedOption = 0;

static int everyKeyOf(OurSetting* s, KeyAndDefault* out, int room,
                      int everyOption = 0, int* complete = nullptr) {
    if (complete) *complete = 0;
    if (room < 1) return 0;
    strcpy_s(out[0].key, sizeof(out[0].key), s->storeKey);
    strcpy_s(out[0].fallback, sizeof(out[0].fallback), s->fallbackText);
    int count = out[0].key[0] ? 1 : 0;
    if (!s->keyExpression[0]) {
        if (complete) *complete = 1;
        return count;
    }

    const LONG storeReads = global_namesReadFromStore;
    const OurSetting* read[EXPRESSION_READS];
    int readCount = 0;
    char key[sizeof(out[0].key)];
    if (!workOutQuietly(s, s->keyExpression, 1, key, sizeof(key), read,
                        &readCount, EXPRESSION_READS))
        return count;
    int whole = readCount <= 1;
    if (everyOption && key[0] && (!count || _stricmp(out[0].key, key) != 0)) {
        if (count < room) {
            strcpy_s(out[count].key, sizeof(out[count].key), key);
            out[count].fallback[0] = 0;
            ++count;
        } else whole = 0;
    }

    for (int r = 0; r < readCount && count < room; ++r) {
        const OurSetting* chooser = read[r];
        /* A selector whose count is an expression is stepped through every
           option it can offer. An option past the count in use reaches a key
           only where the settings file already holds that key. */
        const int wasCount = chooser->optionCount;
        const int reach = chooser->optionCapacity > wasCount
                        ? chooser->optionCapacity : wasCount;
        const int stepped = chooser->kind == KIND_SELECTOR
                         || (everyOption && chooser->kind == KIND_TOGGLE);
        if (chooser == s || !stepped || reach < 2) {
            whole = 0;
            continue;
        }

        global_steppedControl = chooser;
        for (int o = 0; o < reach; ++o) {
            global_steppedOption = o;
            const OurSetting* readNow[EXPRESSION_READS];
            int readNowCount = 0;
            if (!workOutQuietly(s, s->keyExpression, 1, key, sizeof(key),
                                readNow, &readNowCount, EXPRESSION_READS)
                || !key[0]) continue;
            if (readNowCount != 1 || readNow[0] != chooser) whole = 0;

            int already = 0;
            for (int k = 0; k < count && !already; ++k)
                already = _stricmp(out[k].key, key) == 0;
            if (already) continue;
            if (o >= wasCount && !everyOption) {
                char held[MAX_SETTING_TEXT];
                storedText(s->storePath, s->storeSection, key, "", held,
                           sizeof(held));
                if (!held[0]) continue;
            }

            char fallback[sizeof(out[0].fallback)];
            if (s->defaultExpression[0]) {
                if (!workOutQuietly(s, s->defaultExpression, 0, fallback,
                                    sizeof(fallback))) {
                    if (!everyOption) continue;
                    fallback[0] = 0;
                }
            } else strcpy_s(fallback, sizeof(fallback), s->fallbackText);
            if (!fallback[0] && !everyOption) continue;

            if (count >= room) {
                whole = 0;
                break;
            }
            strcpy_s(out[count].key, sizeof(out[count].key), key);
            strcpy_s(out[count].fallback, sizeof(out[count].fallback),
                     fallback);
            ++count;
        }
        global_steppedControl = nullptr;
    }
    if (complete)
        *complete = whole && global_namesReadFromStore == storeReads;
    return count;
}

/*
    The values put aside that an unlock could not take out of
    Mod_Settings_Cache.ini during this run, each kept as hashText of its cache
    section and key written as "[section]key". A value left behind belongs to
    the lock again only where the store holds that same value, and otherwise
    the control's value is put aside afresh when it locks again. Past
    MAX_LEFT_BEHIND of them, and for a value left behind in an earlier run,
    only what the store holds decides.
*/
#define MAX_LEFT_BEHIND 1024

static uint64_t global_leftBehind[MAX_LEFT_BEHIND];
static int      global_leftBehindCount = 0;

/* Answers where id stands among the values left behind, or -1. */
static int leftBehindAt(uint64_t id) {
    for (int i = 0; i < global_leftBehindCount; ++i)
        if (global_leftBehind[i] == id) return i;
    return -1;
}

/* id noted as left behind where left is 1, and taken off the note where left
   is 0. */
static void noteLeftBehind(uint64_t id, int left) {
    const int at = leftBehindAt(id);
    if (left && at < 0 && global_leftBehindCount < MAX_LEFT_BEHIND)
        global_leftBehind[global_leftBehindCount++] = id;
    else if (!left && at >= 0)
        global_leftBehind[at] = global_leftBehind[--global_leftBehindCount];
}

/* ----------------------------------------- values put aside, forgotten */

/*
    A set of keys of Mod_Settings_Cache.ini on the heap: room of them, count
    filled, each kept as hashText of the key folded to lower case. Sorted
    (sortKeySet) before it is searched (keySetHolds).
*/
struct CacheKeySet {
    uint64_t* id;
    int       count;
    int       room;
};

/* An empty set with room for the two cache keys of every setting key each
   control can reach. Its id is null where there is no memory for it. */
static CacheKeySet newKeySet() {
    CacheKeySet set = {};
    set.room = 2 * MAX_KEYS_OF_ONE
             * (global_ourSourceCount > 0 ? global_ourSourceCount : 1);
    set.id = (uint64_t*)malloc(sizeof(uint64_t) * (size_t)set.room);
    return set;
}

static int compareKeyIds(const void* a, const void* b) {
    const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void sortKeySet(CacheKeySet* set) {
    if (set->id && set->count > 1)
        qsort(set->id, (size_t)set->count, sizeof(uint64_t), compareKeyIds);
}

static int keySetHolds(const CacheKeySet* set, const char* key) {
    if (!set->id || !set->count) return 0;
    const uint64_t id = hashText(key, 1);
    return bsearch(&id, set->id, (size_t)set->count, sizeof(uint64_t),
                   compareKeyIds) != nullptr;
}

/*
    The cache keys, .stored and .default, of every setting key s can reach
    (everyKeyOf with everyOption), added to set. Answers 1 where those are
    every key s can reach (everyKeyOf's complete) and all fitted in set.
*/
static int addReachedKeys(OurSetting* s, CacheKeySet* set) {
    KeyAndDefault reach[MAX_KEYS_OF_ONE];
    int complete = 0;
    const int count = everyKeyOf(s, reach, MAX_KEYS_OF_ONE, 1, &complete);
    for (int k = 0; k < count; ++k) {
        if (!set->id || set->count + 2 > set->room) return 0;
        CacheNames names;
        cacheNames(s, reach[k].key, &names);
        set->id[set->count++] = hashText(names.storedKey, 1);
        set->id[set->count++] = hashText(names.defaultKey, 1);
    }
    return complete;
}

/* The section of Mod_Settings_Cache.ini the controls whose settings file is
   at storePath use, as keepText names it (storeSectionOf), hashed ignoring
   case. */
static uint64_t cacheSectionId(const char* storePath) {
    char section[MAX_PATH];
    cacheSectionOf(storePath, section, sizeof(section));
    return hashText(storeSectionOf(section), 1);
}

/* Whether key, in the cache section section (cacheSectionId), is a value put
   aside or a default noted beside one: it ends in .stored or .default, and
   is not a key a preset of that section keeps its own values under
   (<preset>.custom, and <preset>.custom.<setting>). */
static int isPutAsideKey(const char* key, uint64_t section) {
    const size_t length = strlen(key);
    if (!(length >= 7 && !_stricmp(key + length - 7, ".stored"))
        && !(length >= 8 && !_stricmp(key + length - 8, ".default")))
        return 0;
    for (int i = 0; i < global_ourSourceCount; ++i) {
        const OurSetting* s = &global_ourSources[i];
        if (s->kind != KIND_PRESET || cacheSectionId(s->storePath) != section)
            continue;
        char custom[MAX_SOURCE_TEXT + 8];
        formatInto(custom, sizeof(custom), "%s.custom", s->name);
        const size_t n = strlen(custom);
        if (n && !_strnicmp(key, custom, n) && (!key[n] || key[n] == '.'))
            return 0;
    }
    return 1;
}

#define MAX_CACHE_SECTION_BYTES (10u * 1024u * 1024u)

/*
    Takes out of Mod_Settings_Cache.ini, in the cache section of the controls
    whose settings file is at storePath, every key isPutAsideKey answers for
    that only holds, where only is given, and keep does not hold, where keep
    is given. The section is written once, with the lines that stay, or
    taken out whole where no line stays; a line with no '=' stays. One line
    says so where the section cannot be read whole or cannot be written.
*/
static void takeOutPutAside(const char* storePath, const CacheKeySet* only,
                            const CacheKeySet* keep) {
    if (!global_settingsCachePath[0]) return;
    char named[MAX_PATH];
    cacheSectionOf(storePath, named, sizeof(named));
    const char* section = storeSectionOf(named);
    const uint64_t sectionId = hashText(section, 1);

    /* Every line of the section, key=value for a key, each ended by a zero
       and the last by two, in lines, which has held bytes. */
    char* lines = nullptr;
    DWORD held = 0, length = 0;
    for (DWORD room = 64 * 1024;; room *= 2) {
        char* larger = (char*)realloc(lines, room);
        if (!larger) break;
        lines = larger;
        held = room;
        length = GetPrivateProfileSectionA(section, lines, held,
                                           global_settingsCachePath);
        if (length + 2 < held || held >= MAX_CACHE_SECTION_BYTES) break;
    }
    if (!lines || length + 2 >= held) {
        writeLog("STORE the [%s] section of %s could not be read whole, so the "
                 "values put aside there were left as they are", section,
                 SETTINGS_CACHE_NAME);
        free(lines);
        return;
    }

    /* The lines that stay moved up over those that go, in place. */
    int going = 0, staying = 0;
    char* kept = lines;
    for (char* line = lines; *line;) {
        const size_t size = strlen(line) + 1;
        char* equals = strchr(line, '=');
        int goes = 0;
        if (equals) {
            *equals = 0;
            goes = isPutAsideKey(line, sectionId)
                && (!only || keySetHolds(only, line))
                && !(keep && keySetHolds(keep, line));
            *equals = '=';
        }
        if (goes) ++going;
        else {
            ++staying;
            memmove(kept, line, size);
            kept += size;
        }
        line += size;
    }
    *kept = 0;

    if (going) {
        InterlockedIncrement(&global_storeWrites);
        const BOOL written = staying
            ? WritePrivateProfileSectionA(section, lines,
                                          global_settingsCachePath)
            : WritePrivateProfileStringA(section, nullptr, nullptr,
                                         global_settingsCachePath);
        if (!written)
            writeLog("STORE the values put aside in the [%s] section of %s "
                     "could not be taken out, error %lu", section,
                     SETTINGS_CACHE_NAME, GetLastError());
    }
    free(lines);
}

/*
    The values put aside for the controls whose settings file is at path
    forgotten, as for a file just written with its defaults: every key of its
    cache section that isPutAsideKey answers for. Where another settings file
    may share that section (another page's settings file of the same name, a
    page of that section that was not loaded, or global_reachUnknown), only
    the keys the controls of path reach and no other control reaches.
*/
static void forgetPutAsideOf(const char* path) {
    const uint64_t section = cacheSectionId(path);
    int shared = global_reachUnknown;
    for (int p = 0; p < global_modPageCount && !shared; ++p)
        shared = _stricmp(global_modPages[p].storePath, path) != 0
              && cacheSectionId(global_modPages[p].storePath) == section;
    for (int r = 0; r < global_refusedPageCount && !shared; ++r)
        shared = cacheSectionId(global_refusedPagePath[r]) == section;
    if (!shared) {
        takeOutPutAside(path, nullptr, nullptr);
        return;
    }
    CacheKeySet mine = newKeySet(), others = newKeySet();
    if (mine.id && others.id) {
        for (int i = 0; i < global_ourSourceCount; ++i) {
            OurSetting* s = &global_ourSources[i];
            if (s->modItem < 0 || cacheSectionId(s->storePath) != section)
                continue;
            addReachedKeys(s, _stricmp(s->storePath, path) == 0 ? &mine
                                                               : &others);
        }
        sortKeySet(&mine);
        sortKeySet(&others);
        takeOutPutAside(path, &mine, &others);
    } else
        writeLog("STORE there was no memory to forget the values put aside "
                 "for %s", path);
    free(mine.id);
    free(others.id);
}

/*
    The settings files that were missing or empty when the framework started:
    its own, looked at before writeDefaultSettings writes it, and every other
    one a page names, looked at once the pages are read. A file stays noted
    until forgetPutAsideOfNewFiles takes it off.
*/
#define MAX_NEW_SETTINGS_FILES (MAX_MODPAGE_FILES + 1)

static char global_newSettingsFile[MAX_NEW_SETTINGS_FILES][MAX_PATH];
static int  global_newSettingsFileCount = 0;

/* path noted, where it is missing or empty now and is not noted already. */
static void noteNewSettingsFile(const char* path) {
    if (!path || !path[0]) return;
    for (int i = 0; i < global_newSettingsFileCount; ++i)
        if (_stricmp(global_newSettingsFile[i], path) == 0) return;
    if (global_newSettingsFileCount < MAX_NEW_SETTINGS_FILES
        && fileIsMissingOrEmpty(path))
        strcpy_s(global_newSettingsFile[global_newSettingsFileCount++],
                 MAX_PATH, path);
}

/*
    Each settings file noteNewSettingsFile noted, looked at again. One that is
    no longer missing or empty has been written by its mod: the values put
    aside for it are forgotten (forgetPutAsideOf), and it is no longer noted.
    One still missing or empty stays noted where keepMissing is 1, and
    otherwise is no longer noted, with its values put aside kept. While Init
    runs installHooks only the framework's own file is looked at. Called with
    keepMissing 0 as Init ends (makeSettingsFromModPages), before any value
    put aside is read (readPutAside, resetOpenPage) and, once Init is over,
    before the framework writes any file (keepText); and with keepMissing 1
    from the tick (forgetPutAsideOfWrittenFiles). The tick's thread and the
    thread of the menu's callbacks may call it at the same time:
    global_forgetLock is held throughout, nothing it calls may call it again,
    and nothing it calls may change a control, or global_quietExpressions on
    another thread (everyKeyOf, workOutQuietly).
*/
static SRWLOCK global_forgetLock = SRWLOCK_INIT;

static void forgetPutAsideOfNewFiles(int keepMissing) {
    AcquireSRWLockExclusive(&global_forgetLock);
    for (int i = 0; i < global_newSettingsFileCount;) {
        if (global_starting
            && _stricmp(global_newSettingsFile[i], global_iniPath) != 0) {
            ++i;
            continue;
        }
        const int missing = fileIsMissingOrEmpty(global_newSettingsFile[i]);
        if (missing && keepMissing) {
            ++i;
            continue;
        }
        char path[MAX_PATH];
        strcpy_s(path, global_newSettingsFile[i]);
        if (i != --global_newSettingsFileCount)
            strcpy_s(global_newSettingsFile[i], MAX_PATH,
                     global_newSettingsFile[global_newSettingsFileCount]);
        if (!missing) forgetPutAsideOf(path);
    }
    ReleaseSRWLockExclusive(&global_forgetLock);
}

/* When the tick last looked at the settings files noted as new, as
   GetTickCount64 gives it; 0 before it first has. */
static ULONGLONG global_newFilesLookedAt = 0;

/*
    Every tick: once Init is over and while a settings file is noted as new,
    at most once a second, the values put aside for each noted file that is
    no longer missing or empty are forgotten, and the files still missing or
    empty stay noted (forgetPutAsideOfNewFiles with keepMissing 1).
*/
static void forgetPutAsideOfWrittenFiles(void) {
    if (global_starting || !global_newSettingsFileCount) return;
    const ULONGLONG now = GetTickCount64();
    if (global_newFilesLookedAt && now - global_newFilesLookedAt < 1000)
        return;
    global_newFilesLookedAt = now;
    forgetPutAsideOfNewFiles(1);
}

/*
    Takes out of Mod_Settings_Cache.ini the values put aside, and the
    defaults noted beside them, that no control reaches: for the cache
    section of each settings file a loaded page names, every key
    isPutAsideKey answers for that no control of that section reaches
    (addReachedKeys). A section is left as it is where one of its pages,
    those that name no settings file included, was not loaded, or was loaded
    with a fault or a skipped block; where the keys of one of its controls
    are not all known; or where its controls reach no key. Every section is
    left as it is while global_reachUnknown is set.
*/
static void forgetUnreachedPutAside() {
    if (global_reachUnknown || !global_settingsCachePath[0]) return;
    CacheKeySet reached = newKeySet();
    if (!reached.id) {
        writeLog("STORE there was no memory to find the values put aside that "
                 "no control reaches");
        return;
    }
    for (int p = 0; p < global_modPageCount; ++p) {
        if (!global_modPages[p].storePath[0]) continue;
        const uint64_t section = cacheSectionId(global_modPages[p].storePath);
        int done = 0, whole = 1;
        for (int q = 0; q < global_modPageCount; ++q) {
            if (cacheSectionId(global_modPages[q].storePath) != section)
                continue;
            if (q < p && global_modPages[q].storePath[0]) done = 1;
            if (!global_modPages[q].loadedWhole) whole = 0;
        }
        for (int r = 0; r < global_refusedPageCount; ++r)
            if (cacheSectionId(global_refusedPagePath[r]) == section) whole = 0;
        if (done || !whole) continue;
        reached.count = 0;
        for (int i = 0; i < global_ourSourceCount && whole; ++i) {
            OurSetting* s = &global_ourSources[i];
            if (s->modItem >= 0 && cacheSectionId(s->storePath) == section)
                whole = addReachedKeys(s, &reached);
        }
        if (!whole || !reached.count) continue;
        sortKeySet(&reached);
        takeOutPutAside(global_modPages[p].storePath, nullptr, &reached);
    }
    free(reached.id);
}

/*
    What a control holds while its dependency does not, and what it goes back
    to when the dependency holds again. Answers whether the value the control
    holds was changed.
*/
static void composeList(int owner);
static int presetValuesMatch(const char* held, const char* wanted);

/* The list whose value s's value is composed into, as its place in
   global_ourSources: the list s belongs to, or the list of the list item that
   carries s as its linked control. -1 for none. */
static int listOf(const OurSetting* s) {
    if (s->listOwner >= 0) return s->listOwner;
    const int self = (int)(s - global_ourSources);
    for (int i = 0; i < global_ourSourceCount; ++i)
        if (global_ourSources[i].linkedSetting == self)
            return global_ourSources[i].listOwner;
    return -1;
}

/*
    What Mod_Settings_Cache.ini and the settings file hold for the setting key
    a control reaches, as readPutAside reads them.

        names    the cache section and the keys the value is kept under
        entry    hashText of "[section]<stored key>", as global_leftBehind
                 keeps it
        stored   the value put aside, empty where there is none
        noted    the default noted beside it, empty where there is none
        held     what the settings file holds, empty where it holds nothing
        belongs  the value put aside belongs to the lock

    A value put aside belongs to the lock where the settings file holds that
    same value, or where it has not been left behind and the settings file
    holds nothing, the default noted beside it or the control's default. Two
    numbers are the same value however they are written. Anything else in the
    settings file was written there after the value was put aside, as happens
    once an unlock could not take that value out of the cache, and it is the
    settings file's value that counts.
*/
struct PutAside {
    CacheNames names;
    uint64_t   entry;
    char       stored[MAX_SETTING_TEXT];
    char       noted[MAX_SETTING_TEXT];
    char       held[MAX_SETTING_TEXT];
    int        belongs;
};

/* fallback is the default of the setting key of s. The settings files noted
   as new are looked at first (forgetPutAsideOfNewFiles). */
static void readPutAside(const OurSetting* s, const char* key,
                         const char* fallback, PutAside* out) {
    forgetPutAsideOfNewFiles();
    cacheNames(s, key, &out->names);
    storedText(global_settingsCachePath, out->names.section,
               out->names.storedKey, "", out->stored, sizeof(out->stored));
    char entryName[sizeof(out->names.section) + sizeof(out->names.storedKey)
                   + 2];
    formatInto(entryName, sizeof(entryName), "[%s]%s", out->names.section,
               out->names.storedKey);
    out->entry = hashText(entryName);
    storedText(s->storePath, s->storeSection, key, "", out->held,
               sizeof(out->held));
    storedText(global_settingsCachePath, out->names.section,
               out->names.defaultKey, "", out->noted, sizeof(out->noted));
    out->belongs = out->stored[0]
                && (presetValuesMatch(out->held, out->stored)
                    || (leftBehindAt(out->entry) < 0
                        && (!out->held[0]
                            || presetValuesMatch(out->held, out->noted)
                            || presetValuesMatch(out->held, fallback))));
}

/*
    The default of the setting key of s, fallback, written to its settings
    file where the file does not hold it already, and noted beside the value
    put aside where it is not noted already, as a lock leaves them. Where it
    cannot be noted the settings file is put back to what it held: a settings
    file holding a default that is not noted no longer belongs to the lock
    once the default changes again. Answers whether both were done.
*/
static int writeLockedDefault(const OurSetting* s, const char* key,
                              const char* fallback, const PutAside* aside) {
    const int rewrite = _stricmp(aside->held, fallback) != 0;
    /* Whether the settings file holds the key, an empty value included, or
       could not be read; putting the file back takes out only a key it did
       not hold. */
    const int hadKey = rewrite
        && (aside->held[0]
            || settingIsPresent(s->storePath, storeSectionOf(s->storeSection),
                                key) != 0);
    if (rewrite && !keepText(s->storePath, s->storeSection, key, fallback))
        return 0;
    if (_stricmp(aside->noted, fallback) != 0
        && !keepText(global_settingsCachePath, aside->names.section,
                     aside->names.defaultKey, fallback)) {
        if (rewrite)
            keepText(s->storePath, s->storeSection, key,
                     hadKey ? aside->held : nullptr);
        return 0;
    }
    return 1;
}

static volatile LONG global_emptyDefaultLines = 0;
static volatile LONG global_putAsideLines = 0;
static volatile LONG global_giveBackLines = 0;

static int applyLockState(OurSetting* s, int why) {
    if (!global_settingsCachePath[0] || !holdsValue(s) || keyShownOnly(s))
        return 0;

    /*
        Every key the control can address, not only the one it is reading. A
        control whose name is an expression can be locked while reading one key
        and unlocked while reading another.
    */
    KeyAndDefault reach[MAX_KEYS_OF_ONE];
    const int count = everyKeyOf(s, reach, MAX_KEYS_OF_ONE);
    int did = 0, putOff = 0;

    for (int k = 0; k < count; ++k) {
        const int onScreen = _stricmp(reach[k].key, s->storeKey) == 0;
        const char* fallback = reach[k].fallback;
        PutAside aside;
        readPutAside(s, reach[k].key, fallback, &aside);
        const CacheNames& names = aside.names;
        const char* const stored = aside.stored;
        const char* const held = aside.held;
        const int belongs = aside.belongs;

        /*
            An empty default is never written. A control whose DefaultValue is
            an expression has none until the expression has been worked out.
        */
        if (why != LOCKED_BY_NOTHING && !fallback[0]) {
            if (InterlockedIncrement(&global_emptyDefaultLines) <= 16)
                writeLog("STORE \"%s\" is locked and has no default to write, "
                         "so what it holds was left alone", s->name);
            continue;
        }
        if (why == LOCKED_BY_NOTHING && !stored[0]) continue;
        /*
            Where the settings file would be written and the write is put off
            (writeIsPutOff), as at a lock or a value given back, nothing is
            put aside, written, noted or taken for this key: the cache keeps
            what it holds, the control what it holds, and lockPutOff has the
            lock state applied again once Init is over.
        */
        if (writeIsPutOff(s->storePath)
            && (why != LOCKED_BY_NOTHING
                || (belongs && _stricmp(held, stored) != 0))) {
            putOff = 1;
            continue;
        }
        const float was = s->value;

        if (why != LOCKED_BY_NOTHING) {
            /*
                Where nothing put aside belongs to the lock, what the store
                holds is put aside. The default is then written to the store
                and noted beside the value put aside, as writeLockedDefault
                does, on this lock and on every lock after it. Where the value
                could not be put aside, or the store could not be written, the
                store is left as it is and the control keeps its value while
                locked.
            */
            if (!belongs
                && !keepText(global_settingsCachePath, names.section,
                             names.storedKey, held[0] ? held : fallback)) {
                if (InterlockedIncrement(&global_putAsideLines) == 1)
                    writeLog("STORE \"%s\" is locked but what it holds could "
                             "not be put aside in %s, so its default was not "
                             "written and it keeps that value while locked, as "
                             "does any control that locks while the file "
                             "cannot be written", s->name, SETTINGS_CACHE_NAME);
                continue;
            }
            noteLeftBehind(aside.entry, 0);
            if (!writeLockedDefault(s, reach[k].key, fallback, &aside))
                continue;
            if (onScreen) takeValue(s, fallback, 0);
        } else {
            /*
                The value put aside is given back where the store holds a
                different one, and taken out of the cache; one that does not
                belong to the lock is only taken out, and the control takes
                what the store holds. One that cannot be given back stays in
                the cache for a later unlock or a later run, and one that
                cannot be taken out is left behind.
            */
            if (belongs && _stricmp(held, stored) != 0
                && !keepText(s->storePath, s->storeSection, reach[k].key,
                             stored)) {
                if (InterlockedIncrement(&global_giveBackLines) == 1)
                    writeLog("STORE \"%s\" is unlocked but the value it held "
                             "before it locked could not be written back to "
                             "its settings file, so that value stays in %s "
                             "and is written back on a later unlock or a "
                             "later run, as is any other value in that case",
                             s->name, SETTINGS_CACHE_NAME);
                continue;
            }
            if (belongs && onScreen) takeValue(s, stored, 0);
            else if (!belongs && onScreen && held[0]) takeValue(s, held, 0);
            const int takenOut = keepText(global_settingsCachePath,
                                          names.section, names.storedKey,
                                          nullptr);
            noteLeftBehind(aside.entry, !takenOut);
            keepText(global_settingsCachePath, names.section, names.defaultKey,
                     nullptr);
        }
        if (!sameNumber(was, s->value)) did = 1;
    }

    s->lockPutOff = putOff;
    if (did) composeList(listOf(s));
    return did;
}

/*
    Every control, pass after pass until nothing more changes. The number of
    passes is bounded.
*/
static volatile LONG global_settling = 0;

static void refreshControl(OurSetting* s);

/* The value a control of ours holds, as a whole number. */
static int settingValueByName(const char* name, int fallback) {
    if (name && name[0])
        for (int i = 0; i < global_ourSourceCount; ++i)
            if (_stricmp(global_ourSources[i].name, name) == 0)
                return (int)global_ourSources[i].value;
    return fallback;
}

/*
    Where the row that opens our page should sit. The control named by
    rowPositionSetting holds it once the menupages have been read, which happens
    before the options page is parsed; the ini value stands until then.
*/
static int rowPositionNow() {
    return settingValueByName(global_rowPositionSetting, global_menuRowPosition);
}

/* A control's lock state changed to why: kept, and the control drawn again. */
static void takeLockState(OurSetting* s, int why) {
    s->locked = why;
    applyLockState(s, why);
    refreshControl(s);
}

static void settleLocks() {
    if (InterlockedCompareExchange(&global_settling, 1, 0) != 0) return;
    for (int pass = 0, moved = 1; pass < 8 && moved; ++pass) {
        moved = 0;
        for (int i = 0; i < global_ourSourceCount; ++i) {
            OurSetting* s = &global_ourSources[i];
            if (!holdsValue(s)) continue;
            const int why = whyLocked(s);
            if (why == s->locked && !s->lockPutOff) continue;
            takeLockState(s, why);
            moved = 1;
        }
    }
    InterlockedExchange(&global_settling, 0);
}

static void applyEveryLockState() {
    if (!global_settingsCachePath[0]) {
        writeLog("STORE there is no folder to keep %s in, so a locked control "
                 "keeps whatever it holds", SETTINGS_CACHE_NAME);
        return;
    }
    for (int pass = 0; pass < 8; ++pass) {
        int changed = 0;
        for (int i = 0; i < global_ourSourceCount; ++i) {
            OurSetting* s = &global_ourSources[i];
            if (!holdsValue(s)) continue;
            s->locked = whyLocked(s);
            changed += applyLockState(s, s->locked);
        }
        if (!changed) return;
    }
    writeLog("STORE the dependencies did not settle in eight passes, so one of "
             "them depends on a control that depends on it");
}

/*
    The row a control draws in, and how it is made to draw itself again.

    Menu2::IDataSource::PopulateComponentFromItem puts the answer to the
    availability slot onto the component, and the engine reaches it only
    through Gui2::Widget::RefreshFromDataSource (Part I 0254E6D0), which a
    component reaches through its own slot widgetRefreshSource. The engine
    does not call it on a built row by itself, so a lock that changes is drawn
    only by calling it here.

    The widget handed to GetItemData is not always the row itself. The row is
    the first component at or above it whose data source is the one asked
    about (the build's asComponent, parentWidget and widgetGetDataSource).

    Every widget is a process in the engine's relocatable heap, so a row is
    held as the engine holds a process, by its heap record and its id:

        widget + rowRecord       the widget's heap record
        widget + rowId           the widget's id, 32 bits
        widget + rowState        bit ROW_DEAD set once it has been killed
        record + RECORD_OBJECT   where the widget is now, 0 once it is freed
        record + 0x28            its parent's record

    The record names the row only while the widget there carries the id; a
    freed record is given to the next widget made. Each place is read out of
    the build's parentWidget, handleResolve and firstChildWidget. These three
    and asComponent must stand byte for byte at their addresses before any row
    is held or asComponent or parentWidget is called.
*/
#define ROW_DEAD       0x01
#define RECORD_OBJECT  0x08

/*
    tlou-i.exe 1.1.5.0: rowRecord 0x70, rowId 0x98, rowState 0x10.

    The parent widget (01EF8BCC): record = [widget + rowRecord], parent =
    [record + 0x28], answer [parent + RECORD_OBJECT].
*/
static const uint8_t partOneParentWidgetBytes[] = {
    0x48,0x8B,0x49,0x70, 0x33,0xC0, 0x48,0x85,0xC9, 0x74,0x0D,
    0x48,0x8B,0x49,0x28, 0x48,0x85,0xC9, 0x74,0x04,
    0x48,0x8B,0x41,0x08, 0xC3
};

/* The handle resolve (011B3F10): the widget at [record + RECORD_OBJECT],
   answered only where its rowId matches the id the handle holds. */
static const uint8_t partOneHandleResolveBytes[] = {
    0x8B,0x51,0x08, 0x85,0xD2, 0x0F,0x84,0x1F,0x20,0x22,0xFF,
    0x49,0x8B,0x48,0x08, 0x48,0x85,0xC9, 0x0F,0x84,0x12,0x20,0x22,0xFF,
    0x39,0x91,0x98,0x00,0x00,0x00, 0x48,0x0F,0x44,0xC1, 0xC3
};

/* The first child widget (02549FC0), which passes over a child whose
   rowState has ROW_DEAD set. */
static const uint8_t partOneFirstChildWidgetBytes[] = {
    0x48,0x83,0xEC,0x28, 0xE8,0x03,0xE9,0x9A,0xFF, 0xEB,0x10,
    0x48,0x85,0xC0, 0x74,0x06, 0xF6,0x41,0x10,0x01, 0x74,0x12,
    0xE8,0x5D,0xEB,0x9A,0xFF, 0x48,0x8B,0xC8, 0xE8,0x11,0xF0,0xFF,0xFF,
    0x48,0x85,0xC9, 0x75,0xE3, 0x48,0x83,0xC4,0x28, 0xC3
};

/* The component check (02661C00), the whole function. */
static const uint8_t partOneAsComponentBytes[] = {
    0x33,0xC0, 0x48,0x85,0xC9, 0x74,0x25, 0x44,0x8B,0x0D,0x6A,0xE7,0x02,0x01,
    0x48,0x8B,0x51,0x50, 0x45,0x8B,0xC1, 0x49,0xC1,0xE9,0x06,
    0x41,0x83,0xE0,0x3F, 0xC4,0x22,0xBB,0xF7,0x44,0xCA,0x28,
    0x41,0xF6,0xC0,0x01, 0x48,0x0F,0x45,0xC1, 0xC3
};

/*
    tlou-ii.exe and tlou-ii-l.exe 1.6.0: rowRecord 0x80, rowId 0x94, rowState
    0x0C.

    The parent widget (tlou-ii.exe 01340BF0, tlou-ii-l.exe 013549E0), the same
    bytes in both.
*/
static const uint8_t partTwoParentWidgetBytes[] = {
    0x48,0x8B,0x81,0x80,0x00,0x00,0x00, 0x48,0x85,0xC0, 0x74,0x0E,
    0x48,0x8B,0x40,0x28, 0x48,0x85,0xC0, 0x74,0x05,
    0x48,0x8B,0x40,0x08, 0xC3, 0x33,0xC0, 0xC3
};

/* A handle resolve, one of the game's identical copies (tlou-ii.exe
   003383E0, tlou-ii-l.exe 002A7D40), the same bytes in both. */
static const uint8_t partTwoHandleResolveBytes[] = {
    0x48,0x8B,0x11, 0x33,0xC0, 0x48,0x85,0xD2, 0x74,0x1D,
    0x44,0x8B,0x41,0x08, 0x45,0x85,0xC0, 0x74,0x14,
    0x48,0x8B,0x4A,0x08, 0x48,0x85,0xC9, 0x74,0x0B,
    0x44,0x39,0x81,0x94,0x00,0x00,0x00, 0x48,0x0F,0x44,0xC1, 0xC3
};

/* The first child widget, up to its test of the child's rowState and the
   jump that follows it: tlou-ii.exe 01E544E0, then tlou-ii-l.exe 01E7D200. */
static const uint8_t partTwoFirstChildWidgetBytes[] = {
    0x48,0x83,0xEC,0x28, 0xE8,0x27,0xC6,0x4E,0xFF, 0x48,0x85,0xC0, 0x74,0x27,
    0x44,0x8B,0x05,0xD3,0xBA,0x64,0x07, 0x48,0x8B,0x48,0x40, 0x41,0x8B,0xD0,
    0x48,0xC1,0xEA,0x06, 0x41,0x83,0xE0,0x3F, 0xC4,0xE2,0xBB,0xF7,0x4C,0xD1,0x28,
    0xF6,0xC1,0x01, 0x74,0x05, 0x4C,0x8B,0xC8, 0xEB,0x0B, 0x45,0x33,0xC9,
    0x48,0x85,0xC0, 0x74,0x43, 0x0F,0x1F,0x00, 0x4D,0x85,0xC9, 0x74,0x06,
    0xF6,0x40,0x0C,0x01, 0x74,0x35
};

static const uint8_t partTwoLFirstChildWidgetBytes[] = {
    0x48,0x83,0xEC,0x28, 0xE8,0xF7,0x76,0x4D,0xFF, 0x48,0x85,0xC0, 0x74,0x27,
    0x8B,0x0D,0xB4,0xAD,0x65,0x07, 0x48,0x8B,0x50,0x40, 0x44,0x8B,0xC1,
    0x49,0xC1,0xE8,0x06, 0x80,0xE1,0x3F, 0x4E,0x8B,0x44,0xC2,0x28, 0x49,0xD3,0xE8,
    0x41,0xF6,0xC0,0x01, 0x74,0x05, 0x4C,0x8B,0xC8, 0xEB,0x0B, 0x45,0x33,0xC9,
    0x48,0x85,0xC0, 0x74,0x46, 0x0F,0x1F,0x00, 0x4D,0x85,0xC9, 0x74,0x06,
    0xF6,0x40,0x0C,0x01, 0x74,0x38
};

/* The component check, the whole function: tlou-ii.exe 01FF6540, then
   tlou-ii-l.exe 02025710. */
static const uint8_t partTwoAsComponentBytes[] = {
    0x33,0xC0, 0x48,0x85,0xC9, 0x74,0x25, 0x44,0x8B,0x0D,0x4A,0xEC,0x58,0x07,
    0x48,0x8B,0x51,0x40, 0x45,0x8B,0xC1, 0x49,0xC1,0xE9,0x06,
    0x41,0x83,0xE0,0x3F, 0xC4,0x22,0xBB,0xF7,0x44,0xCA,0x28,
    0x41,0xF6,0xC0,0x01, 0x48,0x0F,0x45,0xC1, 0xC3
};

static const uint8_t partTwoLAsComponentBytes[] = {
    0x33,0xC0, 0x4C,0x8B,0xC9, 0x48,0x85,0xC9, 0x74,0x26,
    0x44,0x8B,0x05,0x77,0x7A,0x59,0x07, 0x49,0x8B,0x51,0x40,
    0x41,0x0F,0xB6,0xC8, 0x49,0xC1,0xE8,0x06, 0x80,0xE1,0x3F,
    0x4E,0x8B,0x44,0xC2,0x28, 0x49,0xD3,0xE8, 0x41,0xF6,0xC0,0x01,
    0x49,0x0F,0x45,0xC1, 0xC3
};

/* 1 once the build's asComponent, parentWidget, handleResolve and
   firstChildWidget stood at their addresses. Rows are held, and asComponent
   and parentWidget called, only then. */
static int global_rowsFollowed = 0;

static int sameBytesAt(uint64_t rva, const uint8_t* bytes, size_t length) {
    int same = 0;
    GUARD_TRY {
        same = memcmp((const void*)(global_moduleBase + rva), bytes, length) == 0;
    } GUARD_EXCEPT_DESCRIBING("comparing the game's code") { same = 0; }
    return same;
}

/* Whether code stands at its address as expected. */
static int codeAsExpected(const ExpectedCode& code) {
    return code.rva && code.bytes
        && sameBytesAt(code.rva, code.bytes, code.length);
}

static void checkRowLayout() {
    const int parent = codeAsExpected(global_game->parentWidget);
    const int resolve = codeAsExpected(global_game->handleResolve);
    const int child = codeAsExpected(global_game->firstChildWidget);
    const int component = codeAsExpected(global_game->asComponent);
    global_rowsFollowed = parent && resolve && child && component;
    if (!global_rowsFollowed)
        writeLog("LOCK the game's %s is not the code expected, so no row is "
                 "held and a lock is drawn only when its page is opened again",
                 !parent ? "parent widget lookup"
                 : !resolve ? "handle resolve"
                 : !child ? "child widget walk"
                          : "component check");
}

typedef uint64_t (*RefreshFromSourceFn)(uint64_t widget, uint64_t source);

static uint64_t widgetMethod(uint64_t widget, unsigned slot) {
    uint64_t table = 0, method = 0;
    if (!readQword(widget, &table) || !table) return 0;
    if (!readQword(table + slot, &method)) return 0;
    return method;
}

/* The data source a component holds, or 0. */
static uint64_t sourceOfComponent(uint64_t component, const char* during) {
    if (!global_game) return 0;
    const uint64_t get = widgetMethod(component,
                                      global_game->widgetGetDataSource);
    return get ? callEngine(get, component, during) : 0;
}

static uint64_t componentOfWidget(uint64_t widget) {
    if (!widget || !global_moduleBase || !global_game || !global_rowsFollowed)
        return 0;
    return callEngine(global_moduleBase + global_game->asComponent.rva, widget,
                      "asking whether a widget is a component");
}

static uint64_t parentOfWidget(uint64_t widget) {
    if (!widget || !global_moduleBase || !global_game || !global_rowsFollowed)
        return 0;
    return callEngine(global_moduleBase + global_game->parentWidget.rva,
                      widget, "asking for a widget's parent");
}

/* The widget the handle names now, or 0 where it has been freed, the record
   holds another widget, or the widget has been killed. Reads only. */
static uint64_t widgetOfHandle(uint64_t record, uint32_t id) {
    if (!global_rowsFollowed || !id || !isPointer(record)) return 0;
    uint64_t widget = 0;
    GUARD_TRY {
        widget = *(volatile uint64_t*)(record + RECORD_OBJECT);
        if (!isPointer(widget)
            || *(volatile uint32_t*)(widget + global_game->rowId) != id
            || (*(volatile uint8_t*)(widget + global_game->rowState)
                & ROW_DEAD))
            widget = 0;
    } GUARD_EXCEPT_DESCRIBING("following a row by its handle") { widget = 0; }
    return widget;
}

/* Where the control's row is now, or 0. */
static uint64_t rowNow(const OurSetting* s) {
    return widgetOfHandle(s->rowRecord, s->rowId);
}

/* Holds the row at component by its handle. 1 where the handle names it. */
static int holdRow(OurSetting* s, uint64_t component) {
    if (!global_rowsFollowed || !isPointer(component)) return 0;
    uint64_t record = 0;
    uint32_t id = 0;
    GUARD_TRY {
        record = *(volatile uint64_t*)(component + global_game->rowRecord);
        id = *(volatile uint32_t*)(component + global_game->rowId);
    } GUARD_EXCEPT_DESCRIBING("taking a row's handle") { record = 0; id = 0; }
    if (widgetOfHandle(record, id) != component) return 0;
    s->rowRecord = record;
    s->rowId = id;
    return 1;
}

/* The first widget at or above this one that is a component holding source. */
static uint64_t rowOf(uint64_t widget, uint64_t source) {
    for (int step = 0; widget && step < 16; ++step) {
        const uint64_t component = componentOfWidget(widget);
        if (component
            && sourceOfComponent(component, "asking a component for its "
                                            "source") == source)
            return component;
        widget = parentOfWidget(widget);
    }
    return 0;
}

/* 1 where the row is drawn on page: the page is the row or a widget above it. */
static int rowIsOnPage(uint64_t row, uint64_t page) {
    for (int step = 0; row && page && step < 64; ++step) {
        if (row == page) return 1;
        row = parentOfWidget(row);
    }
    return 0;
}

/* The row draws itself again from the data source, which is what applies the
   answer to the availability slot. A row that has gone, or that no longer
   holds this control's data source, is let go. */
static void refreshControl(OurSetting* s) {
    const uint64_t row = rowNow(s);
    s->hadNoRow = !row;
    if (!row || sourceOfComponent(row, "asking a row for its source before a "
                                       "refresh") != s->object) {
        s->rowRecord = 0;
        s->rowId = 0;
        return;
    }
    const uint64_t refresh = widgetMethod(row,
                                          global_game->widgetRefreshSource);
    if (!refresh) return;
    GUARD_TRY {
        ((RefreshFromSourceFn)refresh)(row, 0);
    } GUARD_EXCEPT_DESCRIBING("making a row draw itself again") { }
}

/*
    Focus, and moving it off a row that is hidden while it has it.

        hasFocus         whether a component is the one the menu root's focus
                         is on: (component) -> a byte
        componentEvent   an event from a component, handed to every widget
                         above it: (component, event id, sixteen bytes,
                         sixteen bytes)

    A container handed focused-component-disabled by one of its own children
    moves focus to the first child after it that can take focus, or to its
    default. A hidden row cannot take focus. SetDisabled sends the same event,
    with both sixteen byte values zero, when it disables the row that has
    focus.

    Both are called only where the build's bytes stand at their addresses,
    and its focusHolder too where it has one.
*/

/* tlou-i.exe 1.1.5.0: hasFocus (0266275C) and componentEvent (02661028). */
static const uint8_t partOneHasFocusBytes[] = {
    0x48,0x83,0xEC,0x28, 0x4C,0x8B,0xC9, 0xE8,0x10,0xEB,0xD0,0xFF,
    0x48,0x8D,0x88,0x40,0x05,0x00,0x00, 0xE8,0xCC,0x04,0x11,0xFF,
    0x49,0x3B,0xC1, 0x0F,0x94,0xC0, 0x48,0x83,0xC4,0x28, 0xC3
};

static const uint8_t partOneComponentEventBytes[] = {
    0x48,0x89,0x5C,0x24,0x10, 0x48,0x89,0x74,0x24,0x18, 0x55, 0x57,
    0x41,0x54, 0x41,0x56, 0x41,0x57, 0x48,0x8D,0xAC,0x24,0x30,0xFF,0xFF,0xFF,
    0x48,0x81,0xEC,0xD0,0x01,0x00,0x00, 0x48,0x8B,0x05,0xB8,0xEB,0xF5,0x00,
    0x48,0x33,0xC4, 0x48,0x89,0x85,0xC8,0x00,0x00,0x00, 0x4D,0x8B,0xF1,
    0x4D,0x8B,0xF8, 0x48,0x8B,0xDA, 0x48,0x8B,0xF1
};

/*
    tlou-ii.exe and tlou-ii-l.exe 1.6.0. hasFocus asks the menu root's focus
    holder (tlou-ii.exe 01B3E830, tlou-ii-l.exe 01B5FDE0), which reads the
    holder at root+0x4C0 and its id at root+0x4C8: its opening, the same in
    both. Then hasFocus (tlou-ii.exe 01FF7500, tlou-ii-l.exe 02026700) and the
    opening of componentEvent (tlou-ii.exe 01FF57E0, tlou-ii-l.exe 02024990)
    of each.
*/
static const uint8_t partTwoFocusHolderBytes[] = {
    0x48,0x8B,0x81,0xC0,0x04,0x00,0x00, 0x48,0x85,0xC0, 0x74,0x22,
    0x8B,0x89,0xC8,0x04,0x00,0x00
};

static const uint8_t partTwoHasFocusBytes[] = {
    0x40,0x53, 0x48,0x83,0xEC,0x20, 0x48,0x8B,0xD9, 0xE8,0xE2,0x6C,0xB4,0xFF,
    0x48,0x8B,0xC8, 0xE8,0x1A,0x73,0xB4,0xFF, 0x48,0x3B,0xC3, 0x0F,0x94,0xC0,
    0x48,0x83,0xC4,0x20, 0x5B, 0xC3
};

static const uint8_t partTwoLHasFocusBytes[] = {
    0x40,0x53, 0x48,0x83,0xEC,0x20, 0x48,0x8B,0xD9, 0xE8,0x92,0x90,0xB3,0xFF,
    0x48,0x8B,0xC8, 0xE8,0xCA,0x96,0xB3,0xFF, 0x48,0x3B,0xC3, 0x0F,0x94,0xC0,
    0x48,0x83,0xC4,0x20, 0x5B, 0xC3
};

static const uint8_t partTwoComponentEventBytes[] = {
    0x40,0x53, 0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x56, 0x41,0x57,
    0x48,0x81,0xEC,0xA0,0x01,0x00,0x00, 0x48,0x8B,0x05,0x3F,0x3A,0x89,0x01,
    0x48,0x33,0xC4, 0x48,0x89,0x84,0x24,0x98,0x01,0x00,0x00,
    0x4D,0x8B,0xF1, 0x49,0x8B,0xE8, 0x48,0x8B,0xDA, 0x48,0x8B,0xF1
};

static const uint8_t partTwoLComponentEventBytes[] = {
    0x40,0x53, 0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x56, 0x41,0x57,
    0x48,0x81,0xEC,0xA0,0x01,0x00,0x00, 0x48,0x8B,0x05,0x9F,0xC8,0x89,0x01,
    0x48,0x33,0xC4, 0x48,0x89,0x84,0x24,0x98,0x01,0x00,0x00,
    0x4D,0x8B,0xF1, 0x49,0x8B,0xE8, 0x48,0x8B,0xDA, 0x48,0x8B,0xF1
};

typedef uint8_t (*HasFocusFn)(uint64_t component);
typedef void    (*ComponentEventFn)(uint64_t component, uint64_t eventId,
                                    const void* first, const void* second);

static int      global_focusKnown = 0;
static uint64_t global_focusedComponentDisabledId = 0;

static void checkFocusCalls() {
    global_focusedComponentDisabledId = hashText("focused-component-disabled");
    const GameBuild* g = global_game;
    global_focusKnown = codeAsExpected(g->hasFocus)
                     && codeAsExpected(g->componentEvent)
                     && (!g->focusHolder.rva || codeAsExpected(g->focusHolder));
    if (!global_focusKnown)
        writeLog("FOCUS the game's focus check or component event is not the "
                 "code expected, so a row hidden while it has focus keeps it");
}

/* Whether row is the component the menu's focus is on. */
static int rowHasFocus(uint64_t row) {
    if (!global_focusKnown || !row) return 0;
    int focused = 0;
    GUARD_TRY {
        focused = ((HasFocusFn)(global_moduleBase
                                + global_game->hasFocus.rva))(row) != 0;
    } GUARD_EXCEPT_DESCRIBING("asking whether a row has focus") { focused = 0; }
    return focused;
}

/* Focus moved off row, as SetDisabled moves it off a row it disables. */
static void moveFocusOff(uint64_t row) {
    if (!global_focusKnown || !row) return;
    __declspec(align(16)) uint8_t first[16];
    __declspec(align(16)) uint8_t second[16];
    memset(first, 0, sizeof(first));
    memset(second, 0, sizeof(second));
    GUARD_TRY {
        ((ComponentEventFn)(global_moduleBase
                            + global_game->componentEvent.rva))(
            row, global_focusedComponentDisabledId, first, second);
    } GUARD_EXCEPT_DESCRIBING("moving focus off a row that was hidden") { }
}

/* The comma separated list a set of items composes, written in file order. */
static void composeList(int owner) {
    if (owner < 0 || owner >= global_ourSourceCount) return;
    const OurSetting* list = &global_ourSources[owner];
    char text[MAX_DESCRIPTION_TEXT] = "";
    for (int i = 0; i < global_ourSourceCount; ++i) {
        const OurSetting* item = &global_ourSources[i];
        /* The disabled position adds nothing. */
        if (item->listOwner != owner || item->index < 1) continue;
        const char* adds = item->listValue;
        char number[32];
        if (item->linkedSetting >= 0) {
            const OurSetting* linked = &global_ourSources[item->linkedSetting];
            if (linked->kind == KIND_SLIDER) {
                writeNumber(number, sizeof(number), linked->value);
                adds = number;
            } else if (linked->index >= 0 && linked->index < linked->optionCount)
                adds = linked->optionValue[linked->index];
        }
        appendBounded(text, sizeof(text), adds);
    }
    keepText(list->storePath, list->storeSection, list->storeKey, text);
}

/*
    Text2::ShapedString::Shape hands every string it lays out to
    Text2::HtmlFragment::Parse: markup in the text itself is what colours a run
    and what breaks a line. The tags the scanner takes are font, b, br, wbr,
    big, small, strike, hang, spanish and img, plus &...; entities. A font tag's
    colour is "#RRGGBB", "#RRGGBBAA", or the name of a palette entry, which is
    hashed and looked up; the game's own palette holds t2-red.

    An unclosed "|" ends the process, and the text must be checked for one
    before any of this is put around it.
*/
#define MAX_LOCKED_COLOUR     32
#define MAX_LOCKED_MARKUP     96
/* Room for text whose every byte turned into an entity. */
#define MAX_SHOWN_TEXT        (MAX_DESCRIPTION_TEXT * 5 + 64)

static char global_lockedTextColour[MAX_LOCKED_COLOUR] = "t2-red";
static int  global_lockedTextBreaks = 2;
static int  global_lockedTextBold = 0;

/*
    The three characters the markup reads, written as the entities that draw
    them.

    out needs five bytes for every byte of in, plus one.
*/
static void escapeForMarkup(const char* in, char* out, size_t size) {
    size_t at = 0;
    for (; in && *in; ++in) {
        const char one[2] = { *in, 0 };
        const char* put = *in == '&' ? "&amp;" : *in == '<' ? "&lt;"
                        : *in == '>' ? "&gt;" : one;
        const size_t length = strlen(put);
        if (at + length + 1 > size) break;
        memcpy(out + at, put, length);
        at += length;
    }
    out[at] = 0;
}

/*
    out     receives the dependency text with the markup around it
    size    bytes at out, the length of text plus MAX_LOCKED_MARKUP or more
*/
static void dressLockedText(const char* text, char* out, size_t size) {
    const char* colour = global_lockedTextColour;
    const int breaks = global_lockedTextBreaks < 0 ? 0
                     : global_lockedTextBreaks > 4 ? 4 : global_lockedTextBreaks;
    out[0] = 0;
    for (int i = 0; i < breaks; ++i) strcat_s(out, size, "<br>");
    if (colour[0]) {
        strcat_s(out, size, "<font color=\"");
        strcat_s(out, size, colour);
        strcat_s(out, size, "\">");
    }
    if (global_lockedTextBold) strcat_s(out, size, "<b>");
    strcat_s(out, size, text);
    if (global_lockedTextBold) strcat_s(out, size, "</b>");
    if (colour[0]) strcat_s(out, size, "</font>");
}

/*
    What the engine is handed below a control's description while the control
    is locked, into out, as ourGetDependencyDescriptionId hands it over: text,
    the dependency text the control shows, as it is written where markup is 1
    and with "<", ">" and "&" written as entities where it is 0, dressed by
    dressLockedText.

    room    bytes at out, MAX_SHOWN_TEXT + MAX_LOCKED_MARKUP or more
*/
static void lockedTextOf(const char* text, int markup, char* out, size_t room) {
    char shown[MAX_SHOWN_TEXT];
    if (markup)
        copyBounded(shown, sizeof(shown), text, "dependency description");
    else escapeForMarkup(text, shown, sizeof(shown));
    dressLockedText(shown, out, room);
}

/*
    Whether text, the dependency text a control shows while it is locked, can
    be drawn as the engine is handed it: as lockedTextOf makes it from text and
    markup, with "<", ">" and "&" written as entities where markup is 0, then
    dressed by dressLockedText. A control whose text cannot be drawn shows
    none while it is locked.
*/
static int dependencyTextIsSafe(const char* text, int markup) {
    char drawn[MAX_SHOWN_TEXT + MAX_LOCKED_MARKUP];
    lockedTextOf(text, markup, drawn, sizeof(drawn));
    return fatalPipeAt(drawn) < 0 && fatalTagAt(drawn) < 0;
}

/*
    While a control is locked, the description pane draws its description, a
    space, and the text lockedTextOf makes from the control's dependency text,
    as one piece of markup. handed is the description as the engine is handed
    it, as markup, and dependency and markup are as for lockedTextOf.

    Answers where the first "<" in handed is that the engine cannot draw in
    that piece, or -1. A "<" after handed's end is not answered. Answers -1
    where dependency is empty, as the pane then draws the description alone.
*/
static int fatalTagWhenLockedAt(const char* handed, const char* dependency,
                                int markup) {
    if (!dependency[0]) return -1;
    char joined[MODPAGE_EXPRESSION * 4 + 2 + MAX_SHOWN_TEXT
                + MAX_LOCKED_MARKUP];
    if (!formatInto(joined, sizeof(joined), "%s ", handed)) return -1;
    const size_t length = strlen(joined) - 1;
    lockedTextOf(dependency, markup, joined + length + 1,
                 sizeof(joined) - length - 1);
    const int at = fatalTagAt(joined);
    return at >= 0 && (size_t)at < length ? at : -1;
}

/* ----------------------------------------------------------- expressions */

/*
    A value written in a menupage may be an expression rather than a literal.
    One is recognised by its first word: a value whose first characters name
    one of the functions below, or a formula of the file's, followed by a
    bracket is evaluated, and anything else is taken as it is written. So an
    ordinary title or description needs no escaping. The label of a selector's
    option may be an expression in the same way, and is worked out each time
    the option is shown.

        IF(condition, when true, when false)
        IFS(condition, value, condition, value, ...)
        IFERROR(value, value when the first could not be worked out)
        CONCAT(value, value, ...)   the values joined as text
        FRACTION(number)            the number as a whole number or as a
                                    fraction in its lowest terms: 1.25 is 5/4
        LET(name, value, name, value, ..., result)
                                    the result, with each name standing for
                                    its value. A value may use the names before
                                    it, and is worked out only when its name,
                                    or a name after it, is read
        CHOOSE(position, value, value, ...)
                                    the value at the position, counted from 1
        SETTING(text)               the control or settings file key the text
                                    names, as a name written in its place would
        STEPUP(number)              the value one step above the number on the
                                    scale of the slider the expression is
                                    written on
        name(value, ...)            a formula the file's header defines

    OPTION stands for the option being labelled in a SelectorLabel, counted
    from 1. VALUE stands for the slider's value in a SliderValueText.

    A condition is the same language ControlDependency is written in - the
    comparisons =, !=, <, >, <= and >=, the words AND, NAND, OR, NOR, XOR,
    XNOR and NOT, brackets, and True and False - and either side of a
    comparison may itself be an expression, so a condition can do arithmetic.

    A value is a number, a string in double quotes, the name of a control or a
    key in the settings file, arithmetic with + - * / and brackets, or another
    function.

    A bare name is looked for among the controls of the page first and in the
    settings file second, so a control that has not been drawn yet still reads
    the value the file holds. A name written as section.key names the section
    to read it from; a name without one uses the section the control asking
    belongs to.

    Only the argument that is used is worked out. IF evaluates one of its two
    values, IFS stops at the first condition that holds, and IFERROR evaluates
    its second value only when the first could not be worked out.
*/

#define EXPRESSION_TEXT   1024
/*
    How deep an expression may nest. Every value counts one level while it is
    being worked out, and so does every unary minus, the minus of a negative
    number such as -1 included, and every NOT. What is inside brackets, a
    function's values and a formula's expression are worked out inside the
    value that holds them, and a LET name's value inside the value that first
    reads the name or a name after it in the same LET, and again inside a less
    deep one where it faulted or met a value nested too deep (see
    bindingWantsWorking). A level takes up to about 16 KB of stack, so the
    deepest expression takes about 220 KB.
*/
#define EXPRESSION_DEPTH  13

struct ExpressionValue {
    int    isNumber;
    double number;
    char   text[EXPRESSION_TEXT];
};

/*
    A name a LET or a formula call gives a value to.

    from/to  the text of a LET name's value, worked out the first time the
             name or a name after it in the same LET is read, and again where
             bindingWantsWorking says; both null for a formula's parameter,
             whose value is worked out before the call
    state    0 not worked out, 1 being worked out, 2 worked out, 3 worked out
             and faulted
    depth    for a LET name in state 2 or 3, the depth of the cursor it was
             last worked out from
    tooDeep  for states 2 and 3, working it out met a value nested deeper than
             EXPRESSION_DEPTH, whether or not IFERROR answered in its place;
             0 for a formula's parameter
    fault    for state 3, the fault
*/
struct ExpressionBinding {
    char            name[MODPAGE_NAME];
    const char*     from;
    const char*     to;
    int             state;
    int             depth;
    int             tooDeep;
    char            fault[160];
    ExpressionValue value;
};

/*
    The names one LET or one formula call gives values to, and the scope they
    stand inside. A formula's parameters stand inside nothing. outerLimit is
    how many of the outer scope's names count.
*/
struct ExpressionScope {
    ExpressionScope*   outer;
    int                outerLimit;
    ExpressionBinding* binding;
    int                count;
};

/*
    owner       the control the expression was written on, whose section an
                unqualified name is read in and whose settings file is used
    fault       what could not be worked out, for the log and for IFERROR
    page        the menupage whose formulas can be called, -1 for none
    option      the option a SelectorLabel is being worked out for, counted
                from 1, 0 where there is none
    hasValue    VALUE has a value: the slider's value a SliderValueText is
    value       being worked out for
    scope       the names in force; scopeLimit is how many of its own count,
                since a LET value reads only the names before its own
    depth       how many levels deep the cursor is, up to EXPRESSION_DEPTH
    tooDeep     a value nested deeper than EXPRESSION_DEPTH was met. IFERROR
                clears the fault that raises, but not this
*/
struct ExpressionCursor {
    const char*       at;
    const char*       end;
    const OurSetting* owner;
    int               page;
    int               option;
    int               hasValue;
    double            value;
    ExpressionScope*  scope;
    int               scopeLimit;
    int               depth;
    int               tooDeep;
    int               faulted;
    char              fault[160];
    /* Which controls the expression read. Filled whether or not anyone
       asked. */
    const OurSetting* read[EXPRESSION_READS];
    int               readCount;
};

/*
    Joins the two halves of a fault into the room there is, cutting what does
    not fit. about is whatever the expression still held when the fault
    happened, which has no length limit.
*/
static void describeFault(char* out, size_t room, const char* what,
                          const char* about) {
    if (!out || !room) return;
    _snprintf_s(out, room, _TRUNCATE, about && about[0] ? "%s: %s" : "%s",
                what ? what : "", about);
}

static void expressionFault(ExpressionCursor* c, const char* what,
                            const char* about) {
    if (c->faulted) return;
    c->faulted = 1;
    describeFault(c->fault, sizeof(c->fault), what, about);
}

/* The fault enterLevel raises. */
#define NESTED_TOO_DEEP "this is nested deeper than an expression may be"

/* One level deeper. Answers 0, with the fault raised and the cursor marked
   tooDeep, where that is deeper than EXPRESSION_DEPTH. The caller takes the
   level off again either way. */
static int enterLevel(ExpressionCursor* c) {
    if (++c->depth <= EXPRESSION_DEPTH) return 1;
    c->tooDeep = 1;
    expressionFault(c, NESTED_TOO_DEEP, 0);
    return 0;
}

static void valueFromNumber(ExpressionValue* v, double number) {
    v->isNumber = 1;
    v->number = number;
    v->text[0] = 0;
}

static void valueFromText(ExpressionValue* v, const char* text) {
    v->isNumber = 0;
    v->number = 0.0;
    copyBounded(v->text, sizeof(v->text), text, "expression value");
}

/* A value as text, a number written as short as it can be. capped is as
   copyBounded takes it. */
static void valueText(const ExpressionValue* v, char* out, size_t room,
                      const char* what, int capped = 0) {
    if (v->isNumber) writeNumber(out, room, (float)v->number);
    else copyBounded(out, room, v->text, what, capped);
}

/* Text that spells a number counts as one, so a setting kept as "2.5" compares
   and multiplies as a number. */
static int valueAsNumber(const ExpressionValue* v, double* out) {
    if (v->isNumber) { *out = v->number; return 1; }
    const char* at = pastBlanks(v->text);
    if (!*at) return 0;
    char* past = 0;
    const double number = strtod(at, &past);
    if (!past || past == at || *pastBlanks(past)) return 0;
    *out = number;
    return 1;
}

/* A value as true or false: 0 where it is neither, which *neither says. */
static int truthOf(const ExpressionValue* v, int* neither) {
    double number = 0.0;
    *neither = 0;
    if (valueAsNumber(v, &number)) return number != 0.0;
    if (!_stricmp(v->text, "true")) return 1;
    if (!_stricmp(v->text, "false") || !v->text[0]) return 0;
    *neither = 1;
    return 0;
}

static int valueTruth(ExpressionCursor* c, const ExpressionValue* v) {
    int neither = 0;
    const int truth = truthOf(v, &neither);
    if (neither) expressionFault(c, "this is neither true nor false", v->text);
    return truth;
}

/* A name ends at anything that cannot be part of one, so a word is only taken
   when it stands alone and a control named "android" is not read as AND. */
static int nameCharacter(char ch) {
    return letterOrDigit(ch) || ch == '_' || ch == '-' || ch == '.';
}

static int expressionWordAt(ExpressionCursor* c, const char* word) {
    skipBlanks(c);
    if (!spelledAt(c->at, c->end, word)) return 0;
    const size_t n = strlen(word);
    if (c->at + n < c->end && nameCharacter(c->at[n])) return 0;
    c->at += n;
    return 1;
}

static int expressionCharacterAt(ExpressionCursor* c, char ch) {
    skipBlanks(c);
    if (c->at >= c->end || *c->at != ch) return 0;
    ++c->at;
    return 1;
}

/* The character stepped over, or the fault where it is not next. */
static int expect(ExpressionCursor* c, char ch, const char* fault) {
    if (expressionCharacterAt(c, ch)) return 1;
    expressionFault(c, fault, 0);
    return 0;
}

/*
    Step over one argument without working it out. Stops at the comma or the
    bracket that ends it, counting nested brackets and ignoring anything in
    quotes.
*/
static void skipArgument(ExpressionCursor* c) {
    for (int nest = 0, quoted = 0; c->at < c->end; ++c->at) {
        const char ch = *c->at;
        if (quoted) {
            if (ch == '\\' && c->at + 1 < c->end) ++c->at;
            else if (ch == '"') quoted = 0;
        }
        else if (ch == '"') quoted = 1;
        else if (ch == '(') ++nest;
        else if (ch == ')') { if (!nest) return; --nest; }
        else if (ch == ',' && !nest) return;
    }
}

static void expressionOr(ExpressionCursor* c, ExpressionValue* out);

/* The value a name stands for: a control of this page, or failing that a key
   in the settings file the page configures. */
static void valueOfName(ExpressionCursor* c, const char* name,
                        ExpressionValue* out) {
    char section[MODPAGE_NAME], whole[MODPAGE_NAME * 2];
    copyBounded(whole, sizeof(whole), name, "setting name in an expression");
    const char* key = splitSettingName(whole, 1, c->owner, section,
                                       sizeof(section),
                                       "section in an expression");

    const char* path = c->owner ? c->owner->storePath : 0;
    const OurSetting* s = settingNamed(path, section, key);
    if (s) {
        int already = 0;
        for (int i = 0; i < c->readCount && !already; ++i) already = c->read[i] == s;
        if (!already && c->readCount < EXPRESSION_READS)
            c->read[c->readCount++] = s;
        if (s->kind == KIND_SLIDER) valueFromNumber(out, (double)s->value);
        else if (s == global_steppedControl)
            valueFromText(out, s->optionValue[global_steppedOption]);
        else valueFromText(out, s->index >= 0 && s->index < s->optionCount
                                ? s->optionValue[s->index] : s->shown);
        return;
    }

    InterlockedIncrement(&global_namesReadFromStore);
    char held[MAX_SETTING_TEXT];
    storedText(path, section, key, "", held, sizeof(held));
    if (held[0]) { valueFromText(out, held); return; }
    expressionFault(c, "this page has no control or setting of that name", name);
    valueFromNumber(out, 0.0);
}

/* The word, where a bracket follows it. The cursor is left where it was
   otherwise, so a setting of the same name still reads as a name. */
static int functionWordAt(ExpressionCursor* c, const char* word) {
    const char* const at = c->at;
    if (!expressionWordAt(c, word)) return 0;
    skipBlanks(c);
    if (c->at < c->end && *c->at == '(') return 1;
    c->at = at;
    return 0;
}

/*
    The text from..to worked out as one value on a cursor that begins as a copy
    of c, with scope and the first limit of its names in force. The controls
    it read are taken into c. Answers whether it faulted; the fault is in
    inner. inner->tooDeep says whether a value nested too deep was met there,
    and is left to the caller to take into c.
*/
static int workOutInside(ExpressionCursor* c, ExpressionCursor* inner,
                         const char* from, const char* to,
                         ExpressionScope* scope, int limit,
                         ExpressionValue* out) {
    *inner = *c;
    inner->at = from;
    inner->end = to;
    inner->scope = scope;
    inner->scopeLimit = limit;
    inner->tooDeep = 0;
    inner->faulted = 0;
    inner->fault[0] = 0;
    expressionOr(inner, out);
    if (!inner->faulted) {
        skipBlanks(inner);
        if (inner->at < inner->end)
            expressionFault(inner, "there is more here than one value",
                            inner->at);
    }
    int at = 0;
    for (; at < inner->readCount && at < EXPRESSION_READS; ++at)
        c->read[at] = inner->read[at];
    c->readCount = at;
    return inner->faulted;
}

/*
    Binding i of scope worked out from the cursor c: state 2 with its value,
    or state 3 with its fault, which is not raised on c. Answers whether it
    was worked out.
*/
static int workOutBinding(ExpressionCursor* c, ExpressionScope* scope, int i) {
    ExpressionBinding* b = &scope->binding[i];
    b->state = 1;
    ExpressionCursor inner;
    const int faulted = workOutInside(c, &inner, b->from, b->to, scope, i,
                                      &b->value);
    b->state = faulted ? 3 : 2;
    b->depth = c->depth;
    b->tooDeep = inner.tooDeep;
    strcpy_s(b->fault, sizeof(b->fault), inner.fault);
    return !faulted;
}

/*
    Whether binding b is worked out where it is read from the cursor c: where
    it has not been, and where it faulted, or met a value nested deeper than
    EXPRESSION_DEPTH, when it was worked out from a cursor deeper than c.
*/
static int bindingWantsWorking(const ExpressionBinding* b,
                               const ExpressionCursor* c) {
    return b->state == 0
        || ((b->state == 3 || (b->state == 2 && b->tooDeep))
            && c->depth < b->depth);
}

/*
    A LET name or a formula parameter in force at the cursor. Answers 1 where
    name is one, with its value in out.

    Before a LET name is worked out, the names before it in the same LET that
    bindingWantsWorking picks are worked out, in order and each from this
    cursor, so a name whose value reads the name before it is worked out one
    level inside the reader however long the chain. One of them that faults
    keeps its fault, which is raised only where its own name is read. A name
    that faulted or met a value nested too deep is worked out again where it
    is read from a cursor less deep than the one it was worked out from.
*/
static int valueOfLocalName(ExpressionCursor* c, const char* name,
                            ExpressionValue* out) {
    ExpressionScope* scope = c->scope;
    for (int limit = c->scopeLimit; scope;
         limit = scope->outerLimit, scope = scope->outer)
        for (int i = 0; i < limit && i < scope->count; ++i) {
            ExpressionBinding* b = &scope->binding[i];
            if (_stricmp(b->name, name) != 0) continue;
            if (b->state == 1) {
                expressionFault(c, "a LET name is worked out from itself", name);
                return 1;
            }
            if (bindingWantsWorking(b, c)) {
                for (int j = 0; j < i; ++j)
                    if (bindingWantsWorking(&scope->binding[j], c))
                        workOutBinding(c, scope, j);
                workOutBinding(c, scope, i);
            }
            if (b->tooDeep) c->tooDeep = 1;
            if (b->state == 3) {
                expressionFault(c, b->fault, 0);
                return 1;
            }
            *out = b->value;
            return 1;
        }
    return 0;
}

/* The names a LET may give, and so how many values it may take. */
#define MAX_LET_NAMES 16

/* LET(name, value, name, value, ..., result), after its bracket. */
static void expressionLet(ExpressionCursor* c, ExpressionValue* out) {
    const char* from[MAX_LET_NAMES * 2 + 1];
    const char* to[MAX_LET_NAMES * 2 + 1];
    int arguments = 0;
    for (;;) {
        if (arguments >= MAX_LET_NAMES * 2 + 1) {
            expressionFault(c, "LET gives more names than it may", 0);
            return;
        }
        skipBlanks(c);
        from[arguments] = c->at;
        skipArgument(c);
        to[arguments++] = c->at;
        if (c->at >= c->end) {
            expressionFault(c, "LET was not closed", 0);
            return;
        }
        if (*c->at == ')') break;
        ++c->at;
    }
    if (arguments < 3 || arguments % 2 == 0) {
        expressionFault(c, "LET takes pairs of a name and a value, and then the "
                           "result", 0);
        return;
    }

    /* The names are kept on the heap. Each holds a whole value, about 1 KB,
       and the stack EXPRESSION_DEPTH allows for a level has no room for 16 of
       them. */
    const int names = (arguments - 1) / 2;
    ExpressionBinding* binding =
        (ExpressionBinding*)malloc(sizeof(ExpressionBinding) * (size_t)names);
    if (!binding) {
        expressionFault(c, "there was no memory for the names LET gives", 0);
        return;
    }
    for (int n = 0; n < names; ++n) {
        const char* nameFrom = from[2 * n];
        const char* nameTo = nameFrom;
        while (nameTo < to[2 * n] && nameCharacter(*nameTo)) ++nameTo;
        const char* rest = nameTo;
        while (rest < to[2 * n] && (unsigned char)*rest <= ' ') ++rest;
        const size_t length = (size_t)(nameTo - nameFrom);
        if (rest < to[2 * n] || !nameCanBeGiven(nameFrom, length)) {
            c->at = nameFrom;
            expressionFault(c, "LET gives a value to something that is not a "
                               "name it can have", nameFrom);
            free(binding);
            return;
        }
        ExpressionBinding* b = &binding[n];
        memcpy(b->name, nameFrom, length);
        b->name[length] = 0;
        b->from = from[2 * n + 1];
        b->to = to[2 * n + 1];
        b->state = 0;
        b->tooDeep = 0;
    }

    ExpressionScope scope = { c->scope, c->scopeLimit, binding, names };
    c->scope = &scope;
    c->scopeLimit = names;
    c->at = from[arguments - 1];
    expressionOr(c, out);
    c->scope = scope.outer;
    c->scopeLimit = scope.outerLimit;
    free(binding);
    if (!c->faulted) expect(c, ')', "LET was not closed");
}

/*
    Whether fault already holds said, "in the formula <name>", where
    callFormula puts it: among the names it puts before a fault, each followed
    by ": ", as in "in the formula a: in the formula b: <fault>", or among
    those it puts after the depth refusal, each after ", ", as in
    "<NESTED_TOO_DEEP>, in the formula a, in the formula b". Text the menupage
    wrote, which a fault may quote after its own words, is not looked at.
*/
static int faultNamesFormula(const char* fault, const char* said) {
    static const char lead[] = "in the formula ";
    const size_t leadLength = sizeof(lead) - 1, length = strlen(said);
    const size_t refusalLength = strlen(NESTED_TOO_DEEP);
    const char* at = fault;
    while (strncmp(at, lead, leadLength) == 0) {
        if (strncmp(at, said, length) == 0 && at[length] == ':') return 1;
        for (at += leadLength; nameCharacter(*at); ++at) {}
        if (strncmp(at, ": ", 2) != 0) return 0;
        at += 2;
    }
    if (strncmp(at, NESTED_TOO_DEEP, refusalLength) != 0) return 0;
    for (at += refusalLength; strncmp(at, ", ", 2) == 0
                              && strncmp(at + 2, lead, leadLength) == 0;) {
        at += 2;
        if (strncmp(at, said, length) == 0
            && (at[length] == ',' || !at[length])) return 1;
        for (at += leadLength; nameCharacter(*at); ++at) {}
    }
    return 0;
}

/* A call of a formula, after its bracket. */
static void callFormula(ExpressionCursor* c, const PageFormula* f,
                        ExpressionValue* out) {
    char what[MODPAGE_NAME + 48];
    const int count = f->parameterCount;
    /* The parameters are kept on the heap, as LET keeps its names. */
    ExpressionBinding* binding = count
        ? (ExpressionBinding*)malloc(sizeof(ExpressionBinding) * (size_t)count)
        : 0;
    if (count && !binding) {
        sprintf_s(what, "there was no memory to call the formula %s", f->name);
        expressionFault(c, what, 0);
        return;
    }
    for (int p = 0; p <= count; ++p) {
        if (p == count ? !expressionCharacterAt(c, ')')
                       : p && !expressionCharacterAt(c, ',')) {
            sprintf_s(what, "the formula %s takes %d value(s)", f->name, count);
            expressionFault(c, what, 0);
            free(binding);
            return;
        }
        if (p == count) break;
        ExpressionBinding* b = &binding[p];
        strcpy_s(b->name, sizeof(b->name), f->parameter[p]);
        b->from = 0;
        b->to = 0;
        b->state = 2;
        b->tooDeep = 0;
        expressionOr(c, &b->value);
        if (c->faulted) {
            free(binding);
            return;
        }
    }

    ExpressionScope scope = { 0, 0, binding, count };
    ExpressionCursor inner;
    const int faulted = workOutInside(c, &inner, f->body,
                                      f->body + strlen(f->body), &scope, count,
                                      out);
    if (inner.tooDeep) c->tooDeep = 1;
    if (faulted) {
        /* A fault is said to be in this formula, after the fault where it is
           the depth refusal, so that its cause comes first, and before it
           otherwise. One that this function has already said is in this
           formula, as one in a formula that calls itself is, is not said to
           be in it again. */
        sprintf_s(what, "in the formula %s", f->name);
        if (faultNamesFormula(inner.fault, what))
            expressionFault(c, inner.fault, 0);
        else if (strncmp(inner.fault, NESTED_TOO_DEEP,
                         strlen(NESTED_TOO_DEEP)) == 0) {
            const size_t used = strlen(inner.fault);
            _snprintf_s(inner.fault + used, sizeof(inner.fault) - used,
                        _TRUNCATE, ", %s", what);
            expressionFault(c, inner.fault, 0);
        } else
            expressionFault(c, what, inner.fault);
    }
    free(binding);
}

/*
    The one value a function of word takes, after its bracket, and the bracket
    that closes it. number says the value must be a number. Answers 0 where
    it faulted.
*/
static int oneValue(ExpressionCursor* c, const char* word, int number,
                    ExpressionValue* out) {
    char what[40];
    expressionOr(c, out);
    if (c->faulted) return 0;
    double ignored = 0.0;
    if (number && !valueAsNumber(out, &ignored)) {
        sprintf_s(what, "%s takes a number", word);
        expressionFault(c, what, out->text);
        return 0;
    }
    if (!expressionCharacterAt(c, ')')) {
        sprintf_s(what, "%s takes one value", word);
        expressionFault(c, what, 0);
        return 0;
    }
    return 1;
}

static void expressionFunction(ExpressionCursor* c, ExpressionValue* out,
                               int* wasFunction) {
    *wasFunction = 1;

    if (functionWordAt(c, "LET")) {
        expressionCharacterAt(c, '(');
        expressionLet(c, out);
        return;
    }

    if (functionWordAt(c, "CHOOSE")) {
        expressionCharacterAt(c, '(');
        ExpressionValue which;
        expressionOr(c, &which);
        if (c->faulted) return;
        double number = 0.0;
        if (!valueAsNumber(&which, &number)) {
            expressionFault(c, "CHOOSE takes a position first", which.text);
            return;
        }
        const double rounded = floor(number + 0.5);
        if (rounded < 1.0 || number - rounded > 0.0001
            || rounded - number > 0.0001) {
            expressionFault(c, "CHOOSE takes a whole position from 1", 0);
            return;
        }
        const long position = (long)rounded;
        for (long k = 1; expressionCharacterAt(c, ','); ++k) {
            if (k < position) { skipArgument(c); continue; }
            expressionOr(c, out);
            if (c->faulted) return;
            while (expressionCharacterAt(c, ',')) skipArgument(c);
            expect(c, ')', "CHOOSE was not closed");
            return;
        }
        char what[64];
        sprintf_s(what, "CHOOSE has no value at position %ld", position);
        expressionFault(c, what, 0);
        return;
    }

    if (functionWordAt(c, "SETTING")) {
        expressionCharacterAt(c, '(');
        ExpressionValue named;
        if (!oneValue(c, "SETTING", 0, &named)) return;
        char name[MODPAGE_NAME * 2];
        valueText(&named, name, sizeof(name), "name given to SETTING", 1);
        if (!name[0]) {
            expressionFault(c, "SETTING was given no name", 0);
            return;
        }
        valueOfName(c, name, out);
        return;
    }

    if (functionWordAt(c, "STEPUP")) {
        expressionCharacterAt(c, '(');
        if (!oneValue(c, "STEPUP", 1, out)) return;
        if (!c->owner || c->owner->kind != KIND_SLIDER) {
            expressionFault(c, "STEPUP is written only on a slider", 0);
            return;
        }
        double number = 0.0;
        valueAsNumber(out, &number);
        valueFromNumber(out, stepAbove(c->owner, number));
        return;
    }

    if (expressionWordAt(c, "IFERROR")) {
        if (!expect(c, '(', "IFERROR needs a bracket after it")) return;
        /* A fault leaves the cursor wherever it happened, part way through the
           value. The argument is then stepped over from its start. */
        const char* const firstValue = c->at;
        expressionOr(c, out);
        const int itFaulted = c->faulted;
        if (itFaulted) {
            c->faulted = 0;
            c->fault[0] = 0;
            c->at = firstValue;
            skipArgument(c);
        }
        if (!expect(c, ',', "IFERROR needs two values")) return;
        if (itFaulted) expressionOr(c, out);
        else skipArgument(c);
        expect(c, ')', "IFERROR takes no more than two values");
        return;
    }

    if (expressionWordAt(c, "IFS")) {
        if (!expect(c, '(', "IFS needs a bracket after it")) return;
        for (int pair = 0; pair < MAX_OPTIONS * 2; ++pair) {
            ExpressionValue condition;
            expressionOr(c, &condition);
            if (c->faulted) return;
            const int holds = valueTruth(c, &condition);
            if (c->faulted) return;
            if (!expect(c, ',', "IFS needs a value after every condition"))
                return;
            if (holds) {
                expressionOr(c, out);
                while (!c->faulted && expressionCharacterAt(c, ',')) {
                    skipArgument(c);
                    if (!expressionCharacterAt(c, ',')) break;
                    skipArgument(c);
                }
                expect(c, ')', "IFS was not closed");
                return;
            }
            skipArgument(c);
            if (expressionCharacterAt(c, ')')) {
                expressionFault(c, "no condition in IFS held", 0);
                return;
            }
            if (!expect(c, ',', "IFS was not closed")) return;
        }
        expressionFault(c, "IFS names more conditions than a control may have", 0);
        return;
    }

    if (expressionWordAt(c, "IF")) {
        if (!expect(c, '(', "IF needs a bracket after it")) return;
        ExpressionValue condition;
        expressionOr(c, &condition);
        if (c->faulted) return;
        const int holds = valueTruth(c, &condition);
        if (c->faulted) return;
        if (!expect(c, ',', "IF needs a value for when it holds")) return;
        if (holds) expressionOr(c, out); else skipArgument(c);
        if (c->faulted
            || !expect(c, ',', "IF needs a value for when it does not hold"))
            return;
        if (holds) skipArgument(c); else expressionOr(c, out);
        expect(c, ')', "IF takes no more than two values");
        return;
    }

    if (functionWordAt(c, "CONCAT")) {
        expressionCharacterAt(c, '(');
        ExpressionValue part;
        valueFromText(out, "");
        do {
            expressionOr(c, &part);
            if (c->faulted) return;
            char number[64] = "";
            if (part.isNumber)
                writeNumber(number, sizeof(number), (float)part.number);
            strncat_s(out->text, sizeof(out->text),
                      part.isNumber ? number : part.text, _TRUNCATE);
        } while (expressionCharacterAt(c, ','));
        expect(c, ')', "CONCAT was not closed");
        return;
    }

    if (functionWordAt(c, "FRACTION")) {
        expressionCharacterAt(c, '(');
        if (!oneValue(c, "FRACTION", 1, out)) return;
        double number = 0.0;
        valueAsNumber(out, &number);
        char text[64];
        writeFraction(text, sizeof(text), number);
        valueFromText(out, text);
        return;
    }

    const char* const at = c->at;
    skipBlanks(c);
    const char* const nameFrom = c->at;
    while (c->at < c->end && nameCharacter(*c->at)) ++c->at;
    const PageFormula* f =
        formulaNamed(c->page, nameFrom, (size_t)(c->at - nameFrom));
    if (f && expressionCharacterAt(c, '(')) {
        callFormula(c, f, out);
        return;
    }
    c->at = at;
    *wasFunction = 0;
}

/* A number, a string, a word, a function, a bracketed expression or a name. */
static void primaryValue(ExpressionCursor* c, ExpressionValue* out) {
    skipBlanks(c);
    if (c->at >= c->end) {
        expressionFault(c, "a value was expected and the expression ended", 0);
        return;
    }

    if (*c->at == '(') {
        ++c->at;
        expressionOr(c, out);
        expect(c, ')', "a bracket was not closed");
        return;
    }

    if (*c->at == '"') {
        char text[EXPRESSION_TEXT];
        size_t at = 0;
        for (++c->at; c->at < c->end && *c->at != '"'; ++c->at) {
            if (*c->at == '\\' && c->at + 1 < c->end) ++c->at;
            if (at + 1 < sizeof(text)) text[at++] = *c->at;
        }
        text[at] = 0;
        if (c->at < c->end) ++c->at;
        else expressionFault(c, "a quote was not closed", 0);
        valueFromText(out, text);
        return;
    }

    if (expressionWordAt(c, "True"))  { valueFromNumber(out, 1.0); return; }
    if (expressionWordAt(c, "False")) { valueFromNumber(out, 0.0); return; }
    if (expressionWordAt(c, "OPTION")) {
        if (c->option > 0) valueFromNumber(out, (double)c->option);
        else expressionFault(c, "OPTION has a value only in a SelectorLabel", 0);
        return;
    }
    if (expressionWordAt(c, "VALUE")) {
        if (c->hasValue) valueFromNumber(out, c->value);
        else expressionFault(c, "VALUE has a value only in a SliderValueText", 0);
        return;
    }

    int wasFunction = 0;
    expressionFunction(c, out, &wasFunction);
    if (wasFunction) return;

    if ((*c->at >= '0' && *c->at <= '9') || *c->at == '.') {
        char* past = 0;
        const double number = strtod(c->at, &past);
        if (past && past > c->at) {
            c->at = past;
            valueFromNumber(out, number);
            return;
        }
    }

    char name[MODPAGE_NAME * 2];
    size_t at = 0;
    for (; c->at < c->end && nameCharacter(*c->at); ++c->at)
        if (at + 1 < sizeof(name)) name[at++] = *c->at;
    name[at] = 0;
    if (!at) {
        expressionFault(c, "this is not something an expression can read",
                        c->at < c->end ? c->at : "");
        return;
    }
    if (!valueOfLocalName(c, name, out)) valueOfName(c, name, out);
}

static void expressionPrimary(ExpressionCursor* c, ExpressionValue* out) {
    valueFromNumber(out, 0.0);
    if (enterLevel(c)) primaryValue(c, out);
    --c->depth;
}

static void expressionUnary(ExpressionCursor* c, ExpressionValue* out) {
    skipBlanks(c);
    if (c->at >= c->end || *c->at != '-') {
        expressionPrimary(c, out);
        return;
    }
    ++c->at;
    valueFromNumber(out, 0.0);
    if (enterLevel(c)) expressionUnary(c, out);
    --c->depth;
    double number = 0.0;
    if (!valueAsNumber(out, &number))
        expressionFault(c, "this cannot be made negative", out->text);
    else
        valueFromNumber(out, -number);
}

/* Products where sums is 0, and sums of products where it is 1. */
static void expressionArithmetic(ExpressionCursor* c, ExpressionValue* out,
                                 int sums) {
    if (sums) expressionArithmetic(c, out, 0);
    else expressionUnary(c, out);
    for (;;) {
        skipBlanks(c);
        if (c->faulted || c->at >= c->end) return;
        const char op = *c->at;
        if (!op || !strchr(sums ? "+-" : "*/", op)) return;
        ++c->at;
        ExpressionValue other;
        if (sums) expressionArithmetic(c, &other, 0);
        else expressionUnary(c, &other);
        if (c->faulted) return;
        double a = 0.0, b = 0.0;
        if (!valueAsNumber(out, &a) || !valueAsNumber(&other, &b)) {
            expressionFault(c, sums ? "only numbers can be added or subtracted"
                                    : "only numbers can be multiplied or divided",
                            0);
            return;
        }
        if (op == '/' && b == 0.0) {
            expressionFault(c, "this divides by zero", 0);
            return;
        }
        valueFromNumber(out, op == '*' ? a * b : op == '/' ? a / b
                           : op == '+' ? a + b : a - b);
    }
}

/* Two numbers compare as numbers and anything else as text, ignoring case. */
static void expressionCompare(ExpressionCursor* c, ExpressionValue* out) {
    expressionArithmetic(c, out, 1);
    if (c->faulted) return;
    skipBlanks(c);
    char how[3];
    readOperator(&c->at, c->end, how);
    if (!how[0]) return;
    if (how[0] == '!' && !how[1]) {
        expressionFault(c, "a lone exclamation mark is not a comparison", 0);
        return;
    }

    ExpressionValue other;
    expressionArithmetic(c, &other, 1);
    if (c->faulted) return;

    double a = 0.0, b = 0.0;
    int answer = 0;
    if (valueAsNumber(out, &a) && valueAsNumber(&other, &b))
        answer = compared(how, sameNumber((float)a, (float)b), a < b, a > b,
                          a <= b, a >= b);
    else {
        char left[EXPRESSION_TEXT], right[EXPRESSION_TEXT];
        valueText(out, left, sizeof(left), "text compared");
        valueText(&other, right, sizeof(right), "text compared");
        const int order = _stricmp(left, right);
        answer = compared(how, order == 0, order < 0, order > 0, order <= 0,
                          order >= 0);
    }
    valueFromNumber(out, answer > 0 ? 1.0 : 0.0);
}

static void expressionNot(ExpressionCursor* c, ExpressionValue* out) {
    if (!expressionWordAt(c, "NOT")) {
        expressionCompare(c, out);
        return;
    }
    valueFromNumber(out, 0.0);
    if (enterLevel(c)) expressionNot(c, out);
    --c->depth;
    if (!c->faulted) valueFromNumber(out, valueTruth(c, out) ? 0.0 : 1.0);
}

/* One level of the joining words, 0 AND, 1 XOR and 2 OR, each operand a level
   tighter. The right operand of AND is not asked whether it is true when the
   left is false, nor that of OR when the left is true. */
static void expressionLogic(ExpressionCursor* c, ExpressionValue* out,
                            int level) {
    if (level) expressionLogic(c, out, level - 1);
    else expressionNot(c, out);
    for (;;) {
        if (c->faulted) return;
        const int negated = expressionWordAt(c, global_logicPair[level][0]);
        if (!negated && !expressionWordAt(c, global_logicPair[level][1])) return;
        const int left = valueTruth(c, out);
        ExpressionValue other;
        if (level) expressionLogic(c, &other, level - 1);
        else expressionNot(c, &other);
        if (c->faulted) return;
        const int result = level == 1 ? left != valueTruth(c, &other)
                         : level == 0 ? left && valueTruth(c, &other)
                                      : left || valueTruth(c, &other);
        valueFromNumber(out, (negated ? !result : result) ? 1.0 : 0.0);
    }
}

static void expressionOr(ExpressionCursor* c, ExpressionValue* out) {
    expressionLogic(c, out, 2);
}

/* Whether a value written in a menupage is an expression rather than text:
   whether its first word is a function, or a formula of page's, with a
   bracket after it. */
static int textIsExpression(const char* text, int page) {
    if (!text) return 0;
    text = pastBlanks(text);
    const char* past = text;
    while (*past && nameCharacter(*past)) ++past;
    const size_t length = (size_t)(past - text);
    if (!length || *pastBlanks(past) != '(') return 0;
    return wordAmong(text, length, FUNCTION_WORDS)
        || formulaNamed(page, text, length) != 0;
}

static volatile LONG global_expressionFaultLines = 0;

/* Set while makeSettingsFromModPages works the expressions out for the first
   of its two times, and while workOutQuietly works one out, for the thread
   doing so alone. While it is set, expressionLine writes and counts nothing,
   evaluateWith writes and notes nothing about a value nested too deep, and
   cutToRoom writes and notes nothing, on that thread. */
static thread_local int global_quietExpressions = 0;

/* A line about an expression, while fewer than 64 have been written. */
static void expressionLine(const char* fmt, ...) {
    if (global_quietExpressions) return;
    if (InterlockedIncrement(&global_expressionFaultLines) > 64) return;
    va_list ap;
    va_start(ap, fmt);
    writeLogList(fmt, ap);
    va_end(ap);
}

static const char* nameOfOwner(const OurSetting* owner) {
    return owner && owner->name[0] ? owner->name : "a control";
}

/*
    The expressions that met a value nested deeper than EXPRESSION_DEPTH and
    were still worked out, IFERROR answering in its place, each kept as
    hashText of the expression's text so that each is logged once. Past
    MAX_TOO_DEEP_TEXTS of them no more are logged.
*/
#define MAX_TOO_DEEP_TEXTS 64

static uint64_t global_tooDeepText[MAX_TOO_DEEP_TEXTS];
static int      global_tooDeepTextCount = 0;

/*
    text      the expression, as the menupage wrote it
    owner     the control it was written on, or nothing
    option    what OPTION stands for, 0 for nothing
    value     what VALUE stands for, or nothing
    answer    receives what it came to
    read      receives the controls the expression read, or nothing
    readCount receives how many, or nothing
    readRoom  how many read has room for

    Answers 1 where it was worked out, 0 where it was not, which is written to
    the log and is what IFERROR answers to. An expression worked out with
    IFERROR answering for a value nested too deep is written to the log too,
    once. Nothing is written while global_quietExpressions is set.
*/
static int evaluateWith(const char* text, const OurSetting* owner, int option,
                        const double* value, ExpressionValue* answer,
                        const OurSetting** read, int* readCount, int readRoom) {
    ExpressionCursor c;
    c.at = text;
    c.end = text + strlen(text);
    c.owner = owner;
    c.page = owner && owner->modItem >= 0 ? global_modItems[owner->modItem].file
                                          : -1;
    c.option = option;
    c.hasValue = value != 0;
    c.value = value ? *value : 0.0;
    c.scope = 0;
    c.scopeLimit = 0;
    c.depth = 0;
    c.tooDeep = 0;
    c.faulted = 0;
    c.fault[0] = 0;
    c.readCount = 0;

    valueFromNumber(answer, 0.0);
    expressionOr(&c, answer);
    if (!c.faulted) {
        skipBlanks(&c);
        if (c.at < c.end)
            expressionFault(&c, "there is more here than one value", c.at);
    }
    if (c.faulted) {
        expressionLine("EXPRESSION \"%s\" on %s could not be worked out, %s",
                       text, nameOfOwner(owner), c.fault);
        return 0;
    }
    if (c.tooDeep && !global_quietExpressions) {
        const uint64_t id = hashText(text);
        int logged = 0;
        for (int i = 0; i < global_tooDeepTextCount && !logged; ++i)
            logged = global_tooDeepText[i] == id;
        if (!logged && global_tooDeepTextCount < MAX_TOO_DEEP_TEXTS) {
            global_tooDeepText[global_tooDeepTextCount++] = id;
            writeLog("EXPRESSION \"%s\" on %s holds a value nested deeper "
                     "than an expression may be, and IFERROR answered with "
                     "its second value in its place", text,
                     nameOfOwner(owner));
        }
    }
    if (read && readCount) {
        int at = 0;
        for (; at < c.readCount && at < readRoom; ++at) read[at] = c.read[at];
        *readCount = at;
    }
    return 1;
}

static int evaluateValue(const char* text, const OurSetting* owner, int option,
                         ExpressionValue* answer, const OurSetting** read,
                         int* readCount, int readRoom) {
    return evaluateWith(text, owner, option, 0, answer, read, readCount,
                        readRoom);
}

static int evaluateExpression(const char* text, const OurSetting* owner,
                              char* out, size_t room,
                              const OurSetting** read, int* readCount,
                              int readRoom) {
    ExpressionValue answer;
    if (!evaluateValue(text, owner, 0, &answer, read, readCount, readRoom))
        return 0;
    valueText(&answer, out, room, "expression's answer");
    return 1;
}

/* The expression's answer as a number. Answers 0 where it could not be
   worked out or is not a number. */
static int evaluateNumber(const char* text, const OurSetting* owner,
                          double* out) {
    ExpressionValue answer;
    if (!evaluateValue(text, owner, 0, &answer, 0, 0, 0)) return 0;
    if (valueAsNumber(&answer, out)) return 1;
    expressionLine("EXPRESSION \"%s\" on %s came to \"%s\", which is not a "
                   "number", text, nameOfOwner(owner), answer.text);
    return 0;
}

/* Whether a condition holds. One that cannot be worked out holds. */
static int conditionHolds(const char* text, const OurSetting* owner) {
    ExpressionValue answer;
    if (!evaluateValue(text, owner, 0, &answer, 0, 0, 0)) return 1;
    int neither = 0;
    const int truth = truthOf(&answer, &neither);
    if (!neither) return truth;
    expressionLine("EXPRESSION \"%s\" on %s came to \"%s\", which is neither "
                   "true nor false, so it holds", text, nameOfOwner(owner),
                   answer.text);
    return 1;
}

/* Counts every time a control's setting name is worked out again. */
static volatile LONG global_controlChanges = 0;

/* A value brought inside the control's limits or option count: shown as it
   now is, and marked to be written to the store. */
static void broughtInside(OurSetting* s) {
    s->asFloat = s->value;
    s->mismatch = 0;
    s->unsaved = 1;
}

/*
    A slider's limits worked out again where they are expressions, and a
    selector's option count where SelectorCount gives it. Where the two limits
    cross, the maximum is raised to the minimum. A value the new limits or
    count leave out is brought inside them unless the control is locked, and
    marked unsaved; settleLimits writes it to the store. Answers whether the
    limits, the count or the value moved.
*/
static int refreshLimits(OurSetting* s) {
    if (!s || s->modItem < 0) return 0;
    const ModPageItem* item = &global_modItems[s->modItem];
    int moved = 0;
    if (s->kind == KIND_SLIDER
        && (item->leastExpression[0] || item->greatestExpression[0])) {
        double least = item->least, greatest = item->greatest, number = 0.0;
        if (item->leastExpression[0])
            least = evaluateNumber(item->leastExpression, s, &number)
                  ? number : (double)s->least;
        if (item->greatestExpression[0])
            greatest = evaluateNumber(item->greatestExpression, s, &number)
                     ? number : (double)s->greatest;
        if (greatest < least) greatest = least;
        if (!sameNumber((float)least, s->least)
            || !sameNumber((float)greatest, s->greatest)) {
            s->least = (float)least;
            s->greatest = (float)greatest;
            moved = 1;
        }
        if (s->locked == LOCKED_BY_NOTHING
            && (s->value < s->least - 0.0001f
                || s->value > s->greatest + 0.0001f)) {
            s->value = s->value < s->least ? s->least : s->greatest;
            writeNumber(s->shown, sizeof(s->shown), s->value);
            broughtInside(s);
            moved = 1;
        }
    }
    if (s->kind != KIND_SLIDER && item->optionCountExpression[0]) {
        double number = 0.0;
        if (evaluateNumber(item->optionCountExpression, s, &number)) {
            int count = (int)floor(number + 0.5);
            if (count < 1) count = 1;
            if (count > s->optionCapacity) count = s->optionCapacity;
            if (count != s->optionCount) {
                s->optionCount = count;
                moved = 1;
            }
        }
        if (s->locked == LOCKED_BY_NOTHING && s->index >= s->optionCount) {
            s->index = s->optionCount - 1;
            s->value = (float)s->index;
            strcpy_s(s->shown, sizeof(s->shown), s->optionValue[s->index]);
            broughtInside(s);
            moved = 1;
        }
    }
    return moved;
}

/*
    Two bytes for each setting of ours, indexed as global_ourSources is. Each
    text has a bit of its own, and CUT_SHORT makes another of it four places
    higher. A REFUSED_ bit is set the first time the text it stands for is
    worked out to text the engine cannot draw, and the CUT_SHORT bit of a text
    the first time that text is taken cut short to the room it has. A line is
    written only then. A TEXT_ text is never refused, so only its CUT_SHORT
    bit is ever set.
*/
#define REFUSED_TITLE        1
#define REFUSED_DESCRIPTION  2
#define REFUSED_LABEL        4    /* an option's label */
#define REFUSED_VALUE_TEXT   8    /* a slider's SliderValueText */
#define TEXT_KEY             256  /* the setting key */
#define TEXT_DEFAULT         512  /* the default value */
#define CUT_SHORT(refused)   ((refused) << 4)

static unsigned short global_refusedTexts[MAX_FRAMEWORK_SOURCES];

/* Whether bit of global_refusedTexts was not yet set for s, which it now is:
   1 the first time it is asked for each control and bit, whichever threads
   ask. */
static int firstTimeNoted(const OurSetting* s, unsigned bit) {
    const ptrdiff_t which = s - global_ourSources;
    if (which < 0 || which >= MAX_FRAMEWORK_SOURCES) return 0;
    const short before = InterlockedOr16(
        (volatile short*)&global_refusedTexts[which], (short)bit);
    return ((unsigned short)before & bit) == 0;
}

/*
    worked, cut to the room bytes at out. The first time for each control and
    text that it does not fit, a line says so, refused being the bit of
    global_refusedTexts that stands for the text; while
    global_quietExpressions is set, no line is written and nothing is noted.
    what is what the line calls the text.
*/
static void cutToRoom(const OurSetting* s, char* out, size_t room,
                      const char* worked, const char* what,
                      unsigned refused) {
    formatInto(out, room, "%.*s", (int)room - 1, worked);
    const size_t length = strlen(worked);
    if (length >= room && !global_quietExpressions
        && firstTimeNoted(s, CUT_SHORT(refused)))
        writeLog("EXPRESSION the %s of %s came to %u character(s) and there is "
                 "room for %u, so it is cut short", what, nameOfOwner(s),
                 (unsigned)length, (unsigned)(room - 1));
}

static int workOutQuietly(const OurSetting* s, const char* expression,
                          int isKey, char* out, size_t room,
                          const OurSetting** read, int* readCount,
                          int readRoom) {
    char worked[EXPRESSION_TEXT];
    const int wasQuiet = global_quietExpressions;
    global_quietExpressions = 1;
    const int done = evaluateExpression(expression, s, worked, sizeof(worked),
                                        read, readCount, readRoom);
    global_quietExpressions = wasQuiet;
    if (done)
        cutToRoom(s, out, room, worked, isKey ? "setting key" : "default value",
                  isKey ? TEXT_KEY : TEXT_DEFAULT);
    return done;
}

/*
    Where the first "<" of handed, the description of s as the engine is
    handed it as markup, is that the engine cannot draw while s is locked, by
    its own dependency or by its list's, or -1.
*/
static int fatalTagWhenLockedFor(const OurSetting* s, const char* handed) {
    int at = -1;
    if (s->dependency[0])
        at = fatalTagWhenLockedAt(handed, s->dependencyDescription,
                                  s->html || s->dependencyIsOurs);
    if (at < 0 && s->gatingList >= 0 && s->gatingList < global_ourSourceCount) {
        const OurSetting* list = &global_ourSources[s->gatingList];
        at = fatalTagWhenLockedAt(handed, list->dependencyDescription,
                                  list->html || list->dependencyIsOurs);
    }
    return at;
}

/*
    s          the control the text was worked out for
    field      its title or its description, which has room bytes
    worked     what the expression came to
    markup     1 where the engine is handed the text as it is written, 0 where
               it is handed the text with "<", ">" and "&" written as entities
    underLock  1 where, while s is locked, the engine draws the text the lock
               adds after this one, as it does after a description
    what       what the text is called in the log
    refused    the bit of global_refusedTexts that stands for this text

    The text, cut to room, goes into field only where the engine can draw it
    as it is handed over, and where underLock is 1 and the text is markup,
    also with the text a lock adds after it. Otherwise field keeps what it
    held, and a line gives the text as it was handed the first time for each
    control and each text. A text taken cut short is said to be so once in the
    same way.
*/
static void takeWorkedText(OurSetting* s, char* field, size_t room,
                           const char* worked, int markup, int underLock,
                           const char* what, unsigned refused) {
    char kept[EXPRESSION_TEXT];
    const size_t keptRoom = room < sizeof(kept) ? room : sizeof(kept);
    formatInto(kept, keptRoom, "%.*s", (int)keptRoom - 1, worked);
    const char* handed = kept;
    char escaped[MAX_SHOWN_TEXT];
    if (!markup) {
        escapeForMarkup(kept, escaped, sizeof(escaped));
        handed = escaped;
    }
    const int drawn = fatalPipeAt(handed) < 0 && fatalTagAt(handed) < 0;
    if (drawn
        && (!markup || !underLock || fatalTagWhenLockedFor(s, handed) < 0)) {
        cutToRoom(s, field, keptRoom, worked, what, refused);
        return;
    }
    if (!firstTimeNoted(s, refused)) return;
    const int entities = handed != kept && strcmp(handed, kept) != 0;
    char measured[160];
    formatInto(measured, sizeof(measured), "%s%s%s",
               strlen(worked) >= keptRoom ? " once cut to the room it has" : "",
               strlen(worked) >= keptRoom && entities ? " and" : "",
               entities ? AS_ENTITIES : "");
    const char* when = drawn ? " while the control is locked" : "";
    if (field[0])
        writeLog("EXPRESSION the %s of %s came to \"%s\"%s, which holds text "
                 "the engine cannot draw%s, so the %s it had is kept", what,
                 nameOfOwner(s), handed, measured, when, what);
    else
        writeLog("EXPRESSION the %s of %s came to \"%s\"%s, which holds text "
                 "the engine cannot draw%s, so %s is shown with no %s", what,
                 nameOfOwner(s), handed, measured, when, nameOfOwner(s), what);
}

/*
    The expressions a control carries, worked out again. Answers whether the
    setting it reads has changed, in which case its value is taken from the
    store again.

    The control's name does not move: it is what a widget's dataSource tag says
    and what the engine matches the row on. Only what it reads, what it falls
    back to and what it says can follow another control.

    withTexts 0 leaves the description and the title as they stand. A
    description or a title worked out to text the engine cannot draw is not
    taken, and the one the control had stays.
*/
static int refreshExpressions(OurSetting* s, int withTexts = 1) {
    if (!s) return 0;
    int keyChanged = 0;
    char worked[EXPRESSION_TEXT];

    /* The key worked out is cut to the room for it, and compared with the key
       held as it is kept. */
    if (s->keyExpression[0]
        && evaluateExpression(s->keyExpression, s, worked, sizeof(worked))
        && worked[0]) {
        char key[sizeof(s->storeKey)];
        cutToRoom(s, key, sizeof(key), worked, "setting key", TEXT_KEY);
        if (_stricmp(s->storeKey, key) != 0) {
            strcpy_s(s->storeKey, sizeof(s->storeKey), key);
            keyChanged = 1;
        }
    }
    if (s->defaultExpression[0]
        && evaluateExpression(s->defaultExpression, s, worked, sizeof(worked))) {
        cutToRoom(s, s->fallbackText, sizeof(s->fallbackText), worked,
                  "default value", TEXT_DEFAULT);
        s->fallback = (float)atof(worked);
        /* A slider whose store held a number outside its range keeps showing
           that number until it is used. The two fields have the same room. */
        if (!s->touched && !(s->mismatch && s->kind == KIND_SLIDER))
            strcpy_s(s->shown, sizeof(s->shown), s->fallbackText);
    }
    if (withTexts && s->descriptionExpression[0]
        && evaluateExpression(s->descriptionExpression, s, worked,
                              sizeof(worked)))
        takeWorkedText(s, s->description, sizeof(s->description), worked,
                       s->html, 1, "description", REFUSED_DESCRIPTION);
    if (withTexts && s->titleExpression[0]
        && evaluateExpression(s->titleExpression, s, worked, sizeof(worked)))
        takeWorkedText(s, s->title, sizeof(s->title), worked, 1, 0, "title",
                       REFUSED_TITLE);

    if (keyChanged) {
        char held[MAX_SETTING_TEXT];
        storedText(s->storePath, s->storeSection, s->storeKey, "", held,
                   sizeof(held));
        takeValue(s, held, 0);
        s->touched = 0;
    }
    if (s->keyExpression[0]) {
        s->keyFollowed = keyChanged;
        InterlockedIncrement(&global_controlChanges);
    }
    if (refreshLimits(s)) {
        s->rangeMoved = 1;
        InterlockedIncrement(&global_controlChanges);
    }
    return keyChanged;
}

/* The label of option at, worked out again where the menupage wrote it as an
   expression, and taken cut to the room an option has where the engine can
   draw it so cut. A line says so the first time a label of the control is
   refused, and the first time one is taken cut short. */
static void refreshOptionLabel(OurSetting* s, int at) {
    if (!s || s->modItem < 0 || at < 0 || at >= MAX_OPTIONS) return;
    const ModPageItem* item = &global_modItems[s->modItem];
    const char* text = item->optionLabelExpression[0]
                     ? item->optionLabelExpression
                     : at < MAX_MODPAGE_OPTIONS && item->option[at].expression
                     ? global_labelExpression[item->option[at].expression] : 0;
    ExpressionValue answer;
    if (!text || !evaluateValue(text, s, at + 1, &answer, 0, 0, 0)) return;
    char worked[EXPRESSION_TEXT];
    valueText(&answer, worked, sizeof(worked), "option label");
    /* Checked as it is shown: cut to the room an option has. */
    char shown[sizeof(s->option[0])];
    formatInto(shown, sizeof(shown), "%.*s", (int)sizeof(shown) - 1, worked);
    if (fatalPipeAt(shown) >= 0 || fatalTagAt(shown) >= 0) {
        if (firstTimeNoted(s, REFUSED_LABEL))
            writeLog("EXPRESSION a label of %s came to \"%s\"%s, which holds "
                     "text the engine cannot draw, so it is not shown", s->name,
                     shown, strcmp(shown, worked) != 0
                                ? " once cut to the room it has" : "");
        return;
    }
    cutToRoom(s, s->option[at], sizeof(s->option[at]), worked, "label",
              REFUSED_LABEL);
}

/*
    The text a slider shows for its value where SliderValueText gives one: the
    expression worked out with VALUE standing for the value, so
    IF(VALUE > 30, "Persistent", VALUE) shows Persistent at 31 and the number
    below it, cut to room. Answers 0 where the slider has none, or it could not
    be worked out, or cut to room it is empty or holds text the engine cannot
    draw, and the number is then shown. A line says so the first time the
    slider's text is refused, and the first time it is taken cut short.
*/
static int sliderValueText(const OurSetting* s, char* out, size_t room) {
    if (!s || s->modItem < 0 || s->kind != KIND_SLIDER) return 0;
    const ModPageItem* item = &global_modItems[s->modItem];
    if (!item->valueText[0]) return 0;
    const double value = (double)s->value;
    ExpressionValue answer;
    if (!evaluateWith(item->valueText, s, 0, &value, &answer, 0, 0, 0))
        return 0;
    char worked[EXPRESSION_TEXT];
    valueText(&answer, worked, sizeof(worked), "slider's value text");
    /* Checked as it is shown: cut to room. */
    char shown[EXPRESSION_TEXT];
    const size_t shownRoom = room < sizeof(shown) ? room : sizeof(shown);
    formatInto(shown, shownRoom, "%.*s", (int)shownRoom - 1, worked);
    if (!shown[0] || fatalPipeAt(shown) >= 0 || fatalTagAt(shown) >= 0) {
        if (firstTimeNoted(s, REFUSED_VALUE_TEXT))
            writeLog("EXPRESSION the value text of %s came to \"%s\"%s, which "
                     "is empty or holds text the engine cannot draw, so the "
                     "number is shown", s->name, shown,
                     strcmp(shown, worked) != 0
                         ? " once cut to the room it has" : "");
        return 0;
    }
    cutToRoom(s, out, shownRoom, worked, "value text", REFUSED_VALUE_TEXT);
    return 1;
}

/* Whether ControlVisibility hides the control now. */
static int controlIsHidden(const OurSetting* s) {
    if (!s || s->modItem < 0 || s->kind == KIND_BUTTON) return 0;
    const ModPageItem* item = &global_modItems[s->modItem];
    return item->visibility[0] && !conditionHolds(item->visibility, s);
}

/* Whether a control's limits or option count are expressions. */
static int hasRangeExpressions(const OurSetting* s) {
    if (!s || s->modItem < 0) return 0;
    const ModPageItem* item = &global_modItems[s->modItem];
    if (s->kind == KIND_SLIDER)
        return item->leastExpression[0] || item->greatestExpression[0];
    return holdsValue(s) && item->optionCountExpression[0];
}

/* Whether a selector's labels are expressions. */
static int hasLabelExpressions(const OurSetting* s) {
    if (!s || s->modItem < 0 || s->kind == KIND_SLIDER) return 0;
    const ModPageItem* item = &global_modItems[s->modItem];
    if (item->optionLabelExpression[0]) return 1;
    for (int o = 0; o < item->optionCount && o < MAX_MODPAGE_OPTIONS; ++o)
        if (item->option[o].expression) return 1;
    return 0;
}

/*
    Every control whose limits or option count are expressions, brought inside
    them, and each value that was brought inside written to the store, pass
    after pass until nothing moves. A value moved by one control's limits moves
    the limits of the controls that read it, so one change runs on through
    every control that follows from it. The store is not read: a value is
    written only where refreshLimits moved it, here or when the engine asked.
    Answers how many values were written.
*/
#define LIMIT_PASSES 16

static int settleLimits() {
    int written = 0;
    for (int pass = 0, moved = 1; pass < LIMIT_PASSES && moved; ++pass) {
        moved = 0;
        for (int i = 0; i < global_ourSourceCount; ++i) {
            OurSetting* s = &global_ourSources[i];
            if (!hasRangeExpressions(s)) continue;
            refreshExpressions(s, 0);
            if (s->locked != LOCKED_BY_NOTHING || !s->unsaved
                || keyShownOnly(s))
                continue;

            char now[MAX_SETTING_TEXT];
            if (s->kind == KIND_SLIDER) writeNumber(now, sizeof(now), s->value);
            else if (s->index >= 0 && s->index < s->optionCount)
                copyBounded(now, sizeof(now), s->optionValue[s->index], "value");
            else continue;

            keepText(s->storePath, s->storeSection, s->storeKey, now);
            s->unsaved = 0;
            s->rangeMoved = 1;
            composeList(listOf(s));
            ++moved;
        }
        written += moved;
    }
    return written;
}

/*
    Every control whose visibility changed, drawn again so the engine asks
    the hidden slot and hides or shows its row. A row hidden while it has
    focus hands focus on first.
*/
static void settleVisibility() {
    for (int i = 0; i < global_ourSourceCount; ++i) {
        OurSetting* s = &global_ourSources[i];
        if (s->modItem < 0 || !global_modItems[s->modItem].visibility[0])
            continue;
        const int hidden = controlIsHidden(s);
        if (hidden == s->hidden) continue;
        const uint64_t row = rowNow(s);
        const int hadFocus = hidden && row && rowHasFocus(row);
        s->hidden = hidden;
        if (!hidden) s->shownAgain = 1;
        refreshControl(s);
        if (hadFocus) moveFocusOff(row);
        InterlockedIncrement(&global_controlChanges);
    }
}

/*
    Everything that follows from a change of value: the controls whose setting
    names read it load the key they now name, locks are settled, limits and
    option counts are worked out again and kept, and rows are hidden, shown
    and drawn again. changing is the control the engine is changing, which it
    draws again itself; it may be nothing.
*/
static void settleAfterChange(const OurSetting* changing) {
    for (int round = 0; round < 8; ++round) {
        for (int i = 0; i < global_ourSourceCount; ++i) {
            OurSetting* other = &global_ourSources[i];
            if (other != changing && other->keyExpression[0]
                && refreshExpressions(other, 0))
                refreshControl(other);
        }
        settleLocks();
        if (!settleLimits()) break;
    }
    settleVisibility();
    /* A row is drawn again where its limits, its count or its value moved,
       where it has just been shown, or where the label of the option it shows
       now reads differently. */
    for (int i = 0; i < global_ourSourceCount; ++i) {
        OurSetting* s = &global_ourSources[i];
        int redraw = s->rangeMoved || s->shownAgain;
        s->rangeMoved = 0;
        s->shownAgain = 0;
        if (s == changing || !s->rowRecord) continue;
        if (!redraw && hasLabelExpressions(s) && s->index >= 0
            && s->index < s->optionCount && s->index < MAX_OPTIONS) {
            char was[MAX_SETTING_TEXT];
            copyBounded(was, sizeof(was), s->option[s->index],
                        "option label");
            refreshOptionLabel(s, s->index);
            redraw = strcmp(was, s->option[s->index]) != 0;
        }
        if (redraw) refreshControl(s);
    }
}

/* ------------------------------------------------------------- presets */

/*
    What a setting a preset names holds for the player now, into out: a
    control's value; or while the control is locked and has a value put aside
    for it in Mod_Settings_Cache.ini, that value where it belongs to the lock,
    and otherwise what its settings file holds where that holds a value; or
    the text of a settings file key no control reads. A value an unlock left
    in the cache is not read while the control is unlocked. Answers the
    control, or nothing.
*/
static OurSetting* presetHeld(const OurSetting* preset, const char* name,
                              char* out, size_t room) {
    char whole[MODPAGE_NAME * 2], section[MODPAGE_NAME];
    copyBounded(whole, sizeof(whole), name, "setting name in a preset");
    const char* key = splitSettingName(whole, 1, preset, section,
                                       sizeof(section), "section in a preset");
    OurSetting* s = settingNamed(preset->storePath, section, key);
    out[0] = 0;
    if (!s) {
        storedText(preset->storePath, section, key, "", out, room);
        return 0;
    }
    if (s->locked != LOCKED_BY_NOTHING && global_settingsCachePath[0]) {
        PutAside aside;
        readPutAside(s, s->storeKey, s->fallbackText, &aside);
        if (aside.stored[0] && (aside.belongs || aside.held[0])) {
            copyBounded(out, room, aside.belongs ? aside.stored : aside.held,
                        "value of a setting in a preset");
            return s;
        }
    }
    if (s->kind == KIND_SLIDER) writeNumber(out, room, s->value);
    else if (s->index >= 0 && s->index < s->optionCount)
        copyBounded(out, room, s->optionValue[s->index],
                    "value of a setting in a preset");
    else storedText(s->storePath, s->storeSection, s->storeKey, "", out, room);
    return s;
}

/* Two numbers compare as numbers and anything else as text, ignoring case. */
static int presetValuesMatch(const char* held, const char* wanted) {
    char* past = 0;
    const double a = strtod(held, &past);
    const int heldIsNumber = past && past != held && !*pastBlanks(past);
    const double b = strtod(wanted, &past);
    const int wantedIsNumber = past && past != wanted && !*pastBlanks(past);
    if (heldIsNumber && wantedIsNumber) return sameNumber((float)a, (float)b);
    return _stricmp(held, wanted) == 0;
}

/* Whether every setting a preset of s sets holds the preset's value. */
static int presetHoldsPreset(const OurSetting* s, const Preset* preset) {
    for (int k = 0; k < preset->count; ++k) {
        char held[MAX_SETTING_TEXT];
        presetHeld(s, preset->setting[k].name, held, sizeof(held));
        if (!presetValuesMatch(held, preset->setting[k].value)) return 0;
    }
    return 1;
}

/* Whether every setting preset p of s sets holds the preset's value. */
static int presetHolds(const OurSetting* s, int p) {
    return presetHoldsPreset(
        s, &global_preset[global_modItems[s->modItem].firstPreset + p]);
}

/* s's Off preset, or 0 where it has none. */
static const Preset* presetOff(const OurSetting* s) {
    if (!s || s->modItem < 0) return 0;
    const int off = global_modItems[s->modItem].offPreset;
    return off > 0 ? &global_preset[off - 1] : 0;
}

/* Whether s has an Off preset and every setting it names holds its value. */
static int presetOffHolds(const OurSetting* s) {
    const Preset* off = presetOff(s);
    return off && presetHoldsPreset(s, off);
}

/* The option a preset shows: the preset last chosen where it still holds,
   else the first that holds, else the last option, for settings that match
   none. */
static int presetMatch(const OurSetting* s) {
    const int presets = s->optionCount - 1;
    if (s->modItem < 0 || presets <= 0) return 0;
    if (s->presetChosen >= 0 && s->presetChosen < presets
        && presetHolds(s, s->presetChosen))
        return s->presetChosen;
    for (int p = 0; p < presets; ++p)
        if (presetHolds(s, p)) return p;
    return presets;
}

/* A preset's shown option, worked out again where the store has been written
   since it last was. */
static void presetFollow(OurSetting* s) {
    if (!s || s->kind != KIND_PRESET) return;
    const LONG writes = global_storeWrites;
    if (s->presetSeen == writes) return;
    s->presetSeen = writes;
    const int at = presetMatch(s);
    s->index = at;
    s->value = (float)at;
    s->asFloat = s->value;
    copyBounded(s->shown, sizeof(s->shown), s->optionValue[at],
                "value a preset shows");
    /* The last option: Off's label while Off holds, else Custom's. */
    const int last = s->optionCount - 1;
    const Preset* off = presetOff(s);
    if (off && last >= 0 && last < MAX_OPTIONS)
        copyBounded(s->option[last], sizeof(s->option[last]),
                    at == last && presetHoldsPreset(s, off)
                        ? off->label
                        : global_modItems[s->modItem].option[last].label,
                    "label of a preset's last option");
}

/* The next setting of ours, empty, with nothing it belongs to. The caller
   counts it once it is filled in. */
static OurSetting* newSetting() {
    OurSetting* s = &global_ourSources[global_ourSourceCount];
    memset(s, 0, sizeof(*s));
    s->listOwner = -1;
    s->gatingList = -1;
    s->linkedSetting = -1;
    return s;
}

/*
    A data source for one row that opens a page, built from the selector class
    like a toggle. Every method that would read a value is answered by us, and a
    button has none.
*/
static int makeButtonSetting(const char* sourceName, const char* title,
                             const char* onPage, int entryRow) {
    if (!sourceName || !sourceName[0] || !title || !title[0]) return 0;
    if (global_ourSourceCount >= MAX_FRAMEWORK_SOURCES) {
        writeLog("SETTING there is room for %d, so the button \"%s\" on the "
                 "page \"%s\" keeps its label in the text database",
                 MAX_FRAMEWORK_SOURCES, title, onPage);
        return 0;
    }
    OurSetting* s = newSetting();
    s->kind = KIND_BUTTON;
    s->entryRow = entryRow;
    s->modItem = -1;
    s->optionCount = 1;
    copyBounded(s->name, sizeof(s->name), sourceName, "button source name");
    copyBounded(s->title, sizeof(s->title), title, "button title");
    copyBounded(s->onPage, sizeof(s->onPage), onPage, "page heading");
    strcpy_s(s->className, sizeof(s->className), SELECTOR_CLASS);
    s->id = hashText(s->name);
    s->classId = hashText(s->className);
    ++global_ourSourceCount;
    return 1;
}

/* A value the menupage wrote, copied into plain or, where it is an expression,
   into expression. */
static void textOrExpression(const char* text, int page, char* plain,
                             size_t plainRoom, const char* plainWhat,
                             char* expression, size_t expressionRoom,
                             const char* expressionWhat) {
    if (textIsExpression(text, page))
        copyBounded(expression, expressionRoom, text, expressionWhat);
    else copyBounded(plain, plainRoom, text, plainWhat);
}

/*
    Every control a menupage describes becomes a setting, but for a block that
    was skipped or is inside a block that was. A value the store holds that
    the control cannot represent is left alone and reported: a slider shows
    the number as it stands, any other control its default as it stood when
    the store was read, until it is used, and only then is the store
    written.
*/
static void makeSettingsFromModPages() {
    for (int p = 0; p < global_modPageCount; ++p) {
        const ModPage* page = &global_modPages[p];
        for (int i = page->firstItem; i < page->firstItem + page->itemCount; ++i) {
            const ModPageItem* item = &global_modItems[i];
            if (blockIsSkipped(i) || item->itemType == ITEM_SUBMENU
                || item->itemType == ITEM_PAGELINK) continue;
            if (global_ourSourceCount >= MAX_FRAMEWORK_SOURCES) {
                writeLog("SETTING there is room for %d, so \"%s\" was left out",
                         MAX_FRAMEWORK_SOURCES, item->settingName);
                global_reachUnknown = 1;
                break;
            }
            OurSetting* s = newSetting();
            copyBounded(s->keyExpression, sizeof(s->keyExpression),
                        item->settingKey, "setting key expression");
            s->index = -1;
            s->modItem = i;
            s->html = item->html != 0;

            copyBounded(s->name, sizeof(s->name), item->settingName, "setting name");
            textOrExpression(item->title, p, s->title, sizeof(s->title), "title",
                             s->titleExpression, sizeof(s->titleExpression),
                             "title expression");
            copyBounded(s->storeKey, sizeof(s->storeKey), item->settingName, "key");
            copyBounded(s->storeSection, sizeof(s->storeSection), item->section,
                        "section");
            copyBounded(s->storePath, sizeof(s->storePath), page->storePath,
                        "store path");
            textOrExpression(item->description, p, s->description,
                             sizeof(s->description), "description",
                             s->descriptionExpression,
                             sizeof(s->descriptionExpression),
                             "description expression");
            copyBounded(s->dependency, sizeof(s->dependency), item->dependency,
                        "dependency");
            /* A dependency with no wording of its own is written out as it
               stands, after the sentence that says what it is. The text,
               shown only while the dependency does not hold, is checked only
               where there is a dependency. */
            dependencyTextOf(item, s->dependencyDescription,
                             sizeof(s->dependencyDescription),
                             &s->dependencyIsOurs);
            if (s->dependency[0]
                && !dependencyTextIsSafe(s->dependencyDescription,
                                         s->html || s->dependencyIsOurs)) {
                writeLog("MODPAGE \"%s\" would show \"%s\" while it is locked, "
                         "which holds text the engine cannot draw, so it shows "
                         "nothing instead", s->name, s->dependencyDescription);
                s->dependencyDescription[0] = 0;
            }
            copyBounded(s->listValue, sizeof(s->listValue), item->listValue,
                        "list value");
            textOrExpression(item->fallback, p, s->fallbackText,
                             sizeof(s->fallbackText), "default value",
                             s->defaultExpression, sizeof(s->defaultExpression),
                             "default value expression");
            s->markAsModified = item->markAsModified;

            if (item->itemType == ITEM_LIST) {
                s->kind = KIND_LIST;
                ++global_ourSourceCount;
                continue;
            }

            s->kind = item->controlType == MODPAGE_SLIDER   ? KIND_SLIDER
                    : item->controlType == MODPAGE_SELECTOR ? KIND_SELECTOR
                    : item->controlType == MODPAGE_PRESET   ? KIND_PRESET
                    : item->controlType == MODPAGE_KEYBIND  ? KIND_KEYBIND
                                                            : KIND_TOGGLE;
            /* A preset keeps nothing in the store: it shows the preset its
               settings match, worked out when first asked. */
            if (s->kind == KIND_PRESET) {
                s->storeKey[0] = 0;
                s->keyExpression[0] = 0;
                s->presetSeen = -1;
                s->presetChosen = -1;
            }
            strcpy_s(s->className, sizeof(s->className),
                     s->kind == KIND_SLIDER ? SLIDER_CLASS : SELECTOR_CLASS);
            s->least = item->least;
            s->greatest = item->greatest;
            s->step = item->step > 0.0f ? item->step : 1.0f;
            s->logarithmic = item->logarithmic;
            s->smallestStep = item->smallestStep > 0.0f ? item->smallestStep
                                                        : 0.0f;
            s->optionCount = item->optionCount;
            for (int o = 0; o < item->optionCount && o < MAX_OPTIONS; ++o) {
                copyBounded(s->option[o], sizeof(s->option[o]),
                            item->option[o].label, "option");
                copyBounded(s->optionValue[o], sizeof(s->optionValue[o]),
                            item->option[o].value, "option value");
            }
            s->optionCapacity = s->optionCount;
            /* A selector whose count is an expression and which lists no
               options offers them numbered from 1, each kept as its number.
               refreshLimits sets the count. */
            if (s->kind != KIND_SLIDER && item->optionCountExpression[0]) {
                if (!item->optionCount) {
                    for (int o = 0; o < MAX_OPTIONS; ++o) {
                        sprintf_s(s->option[o], "%d", o + 1);
                        sprintf_s(s->optionValue[o], "%d", o + 1);
                    }
                    s->optionCapacity = MAX_OPTIONS;
                }
                s->optionCount = s->optionCapacity;
            }
            /* A keybind offers one option, which no row draws: what the engine
               asks of a selector is answered without it. A row that only
               shows the key (keyShownOnly) shows the key's name in its
               place. */
            if (s->kind == KIND_KEYBIND) {
                s->optionCount = s->optionCapacity = 1;
                strcpy_s(s->option[0], sizeof(s->option[0]), "Key");
                copyBounded(s->optionValue[0], sizeof(s->optionValue[0]),
                            s->fallbackText, "option value");
                s->keybindKey = -1;
                s->keybindFocus = -1;
                s->allowMenuKeys = item->allowMenuKeys;
                s->allowGameplayKeys = item->allowGameplayKeys;
            }
            /* fallbackText, not the block's own field, which still holds the
               expression where the default is written as one.
               refreshExpressions fills both in afterwards. */
            s->fallback = (float)atof(s->fallbackText);
            copyBounded(s->shown, sizeof(s->shown), s->fallbackText,
                        "value a row shows before it is used");

            s->id      = hashText(s->name);
            s->classId = hashText(s->className);
            char textKey[MAX_SOURCE_TEXT + 24];
            sprintf_s(textKey, "%s.description", s->name);
            s->descriptionKeyId = hashText(textKey);
            sprintf_s(textKey, "%s.title", s->name);
            s->titleKeyId = hashText(textKey);
            ++global_ourSourceCount;
        }
    }

    /*
        What belongs to which list, which control a list item carries, and which
        list's dependency also applies. A control the file nests inside a list
        item is gated by the same list as the item that carries it, and is not a
        member of the list.
    */
    for (int i = 0; i < global_ourSourceCount; ++i) {
        const int mine = global_ourSources[i].modItem;
        if (mine < 0) continue;
        const int owner = global_modItems[mine].parent;
        const int gating = gatingBlockOf(mine);
        for (int k = 0; k < global_ourSourceCount; ++k) {
            const OurSetting* other = &global_ourSources[k];
            if (other->kind != KIND_LIST) {
                if (global_modItems[mine].linked >= 0
                    && other->modItem == global_modItems[mine].linked)
                    global_ourSources[i].linkedSetting = k;
                continue;
            }
            if (owner >= 0 && other->modItem == owner)
                global_ourSources[i].listOwner = k;
            if (gating >= 0 && other->modItem == gating && other->dependency[0])
                global_ourSources[i].gatingList = k;
        }
    }

    /*
        A description written as text was checked when its page was read, but
        not as it is once a block has taken what it inherits: the description
        itself, whether it is markup, and the dependencies whose text a lock
        draws after it. Each is checked again as the engine is handed it, and
        one the engine cannot draw is emptied. The text with its "<", ">" and
        "&" written as entities is kept on the heap, off the stack the
        expressions worked out below take.
    */
    char* escaped = (char*)malloc(MAX_SHOWN_TEXT);
    for (int i = 0; i < global_ourSourceCount; ++i) {
        OurSetting* s = &global_ourSources[i];
        if (s->modItem < 0 || s->kind == KIND_LIST || !s->description[0]
            || s->descriptionExpression[0])
            continue;
        const char* handed = s->description;
        if (!s->html) {
            if (!escaped) continue;
            escapeForMarkup(s->description, escaped, MAX_SHOWN_TEXT);
            handed = escaped;
        }
        const int drawn = fatalPipeAt(handed) < 0 && fatalTagAt(handed) < 0;
        if (drawn && (!s->html || fatalTagWhenLockedFor(s, handed) < 0))
            continue;
        const char* measured = handed != s->description
                               && strcmp(handed, s->description) != 0
                             ? AS_ENTITIES : "";
        writeLog("MODPAGE %s: the description of %s, with what it inherits, "
                 "is \"%s\"%s, which holds text the engine cannot draw%s, so "
                 "%s is shown with no description",
                 global_modPages[global_modItems[s->modItem].file].name,
                 s->name, handed, measured,
                 drawn ? " while the control is locked" : "", s->name);
        s->description[0] = 0;
    }
    free(escaped);

    /*
        What the store already holds, and whether the control can show it. The
        expressions other than the title and the description come first: one
        of them decides which key that is. They are worked out without a line,
        and every value is taken without a line. Then every expression, the
        title and the description among them, is worked out from the values
        taken, since any of them may name another control, with a line for
        each that fails, and only then is every value taken again and one the
        control cannot show reported.
    */
    for (int complain = 0; complain < 2; ++complain) {
        global_quietExpressions = !complain;
        for (int i = 0; i < global_ourSourceCount; ++i)
            refreshExpressions(&global_ourSources[i], complain);
        global_quietExpressions = 0;
        for (int i = 0; i < global_ourSourceCount; ++i) {
            OurSetting* s = &global_ourSources[i];
            if (!holdsValue(s)) continue;
            char held[MAX_SETTING_TEXT];
            storedText(s->storePath, s->storeSection, s->storeKey, "", held,
                       sizeof(held));
            takeValue(s, held, complain);
        }
    }
    for (int p = 0; p < global_modPageCount; ++p)
        noteNewSettingsFile(global_modPages[p].storePath);
    forgetUnreachedPutAside();
    forgetPutAsideOfNewFiles();
    applyEveryLockState();
}

/*
    Menu2::DataSourceManager::CreateDataSource(StringId64, StringId64,
    StringId64, StringId64, unsigned __int64). Four ids and then a plain word,
    so the fourth id is at rsp+0x28 on entry and the word at rsp+0x30.
*/
typedef uint64_t (*CreateDataSourceFn)(void* manager, uint64_t classId,
                                       uint64_t instanceId, uint64_t thirdId,
                                       uint64_t fourthId, uint64_t flags);

#define CREATE_DATA_SOURCE "Menu2::DataSourceManager::CreateDataSource"

/* The loader's trampoline for CreateDataSource. */
static uint64_t global_createDataSource = 0;

/* Whether a setting of ours wants a data source made: one with a name and a
   class whose row a page draws. */
static int wantsSource(const OurSetting* s) {
    return s->id && s->classId && !s->onNoPage;
}

/* How many settings of ours want a data source made. */
static int settingsWantingSources() {
    int wanting = 0;
    for (int i = 0; i < global_ourSourceCount; ++i)
        if (wantsSource(&global_ourSources[i])) ++wanting;
    return wanting;
}

/*
    Built from inside CreateDataSource, through the loader's trampoline, where
    the engine memory scope the allocation needs is live. manager is the one
    CreateDataSource was called with. Once each, and a failure of one does not
    stop the rest. Where maxFrameworkSources has room for every one they are
    made in the order of global_ourSources. Where it has not, the row on a
    page of the game that opens the landing page is made first, then the
    other buttons that open pages, from the last to the first, so that the
    buttons of the landing page come next; then the other settings, in order,
    so that those left out are the last controls. A line names a button by
    its title and the page it is on, and any other setting by its name.
*/
static void makeOurSettings(uint64_t manager) {
    if (!manager) return;
    if (InterlockedCompareExchange(&global_settingsMade, 1, 0) != 0) return;

    if (!global_createDataSource) {
        writeLog("SETTING the loader has no trampoline for CreateDataSource, so "
                 "none of ours can be made");
        return;
    }

    int tried = 0, leftOut = 0;
    char named[MAX_SETTING_TEXT * 2 + 32], firstLeftOut[sizeof(named)] = "";
    /* Where some are left out, n goes through the settings three times:
       forwards making the rows that open the landing page from a page of the
       game, backwards making the other buttons, then forwards making the
       rest. */
    const int count = global_ourSourceCount;
    const int passes = settingsWantingSources() > global_maxFrameworkSources
                     ? 3 : 1;
    for (int n = 0; n < passes * count; ++n) {
        const int pass = passes == 1 ? 2 : n / count;
        OurSetting* s =
            &global_ourSources[pass == 1 ? count - 1 - n % count : n % count];
        const int button = s->kind == KIND_BUTTON;
        if (!wantsSource(s)
            || (passes > 1 && (pass == 0 ? !button || !s->entryRow
                               : pass == 1 ? !button || s->entryRow
                                           : button)))
            continue;
        if (button)
            formatInto(named, sizeof(named), "the button \"%s\" on the page "
                       "\"%s\"", s->title, s->onPage);
        else formatInto(named, sizeof(named), "\"%s\"", s->name);
        if (tried >= global_maxFrameworkSources) {
            if (!leftOut) strcpy_s(firstLeftOut, sizeof(firstLeftOut), named);
            ++leftOut;
            continue;
        }
        ++tried;

        uint64_t made = 0;
        int ok = 0;
        GUARD_TRY {
            made = ((CreateDataSourceFn)global_createDataSource)(
                (void*)manager, s->classId, s->id, s->titleKeyId,
                s->descriptionKeyId, 0);
            ok = 1;
        } GUARD_EXCEPT_DESCRIBING("creating a setting of our own") {
            ok = 0;
        }

        s->object = made;
        if (made) readQword(made, &s->vtable);
        else writeLog("SETTING %s was not made as %s: %s", named,
                      s->className, ok ? "nothing was returned"
                                       : "it faulted");
    }
    /* Where the limit was lowered for the pool, a POOL line has said this. */
    if (leftOut && !global_poolLimitedSettings)
        writeLog("SETTING %d setting(s) of ours, %s the first of them, were not "
                 "made, as maxFrameworkSources allows no more than %d: their "
                 "rows show the text their widgets were authored with",
                 leftOut, firstLeftOut, global_maxFrameworkSources);
    giveOurSettingsOwnVtable();
}

/*
    Every data source the engine builds is registered in one hash map. The map's
    node pool is a fixed block of nodes with a bitmap of which are in use, and
    when every bit is set its allocator answers nothing and the insert writes
    through that answer, which ends the process.

    The pool descriptor, at the build's sourcePool:

        +0x00  void*  the node memory
        +0x08  void*  the bitmap allocation, kept only so it can be freed
        +0x10  u64*   the bitmap words
        +0x18  u64    how many bits, which is the node count
        +0x20  u64    the bit count rounded up to 64
        +0x28  u32    how many bitmap words
        +0x30  u64    how many nodes
        +0x38  u64    the stride
        +0x40  u64    how many are live
        +0x48  u64    the alignment
        +0x50  u64    the last index handed out

    Nodes are 32 bytes, and the pool holds 541 in Part I and 809 in Part II.
    The map chains, and with a larger pool the bucket array stays as it is and
    the chains get longer.

    The block and the bitmap are replaced, not extended, and this may only be
    done while the pool is empty. +0x08 is left pointing at the engine's own
    bitmap.
*/
#define SOURCE_POOL_MEMORY     0x00
#define SOURCE_POOL_BITS       0x10
#define SOURCE_POOL_BITCOUNT   0x18
#define SOURCE_POOL_BITSROUND  0x20
#define SOURCE_POOL_WORDS      0x28
#define SOURCE_POOL_COUNT      0x30
#define SOURCE_POOL_STRIDE     0x38
#define SOURCE_POOL_LIVE       0x40
#define SOURCE_POOL_ALIGN      0x48
#define SOURCE_POOL_LAST       0x50

#define SOURCE_POOL_NODE_BYTES 32
#define SOURCE_POOL_ALIGNMENT  16

/* Nodes to add to the pool. 0 leaves the engine's pool alone. */
static int  global_extraSourceSlots = 0;
/* Set by the first call of enlargeDataSourcePool, which settles the pool and
   how many settings of ours it has room for. */
static LONG global_poolSettled = 0;

/*
    Lowers maxFrameworkSources to what the data source pool has room for, where
    it allows more: the build's safeFrameworkSources and the nodes added, which
    are none where the pool was left as the engine made it. why says what
    became of the pool, and is logged with how many settings are made where
    some are left out.
*/
static void limitOurSettings(int added, const char* why) {
    const int room = added + global_game->safeFrameworkSources;
    if (room >= global_maxFrameworkSources) return;
    global_maxFrameworkSources = room;
    global_poolLimitedSettings = 1;
    const int wanting = settingsWantingSources();
    if (wanting > room)
        writeLog("POOL %s, so no more than %d of the %d settings of ours will "
                 "be made, and the rows of the other %d show the text their "
                 "widgets were authored with", why, room, wanting,
                 wanting - room);
}

/*
    The pool is left as the engine made it, for reason. In a build with a
    safe number of settings (Part I), line, where there is one, is logged, and
    the settings of ours are limited to that number (limitOurSettings, with
    limit as its why). In a build without one (Part II), no setting of ours
    is made, and where any wants a data source one line says why.
*/
static void leavePoolAlone(const char* reason, const char* line,
                           const char* limit) {
    if (global_game->safeFrameworkSources > 0) {
        if (line) writeLog("POOL %s", line);
        limitOurSettings(0, limit);
        return;
    }
    global_maxFrameworkSources = 0;
    global_poolLimitedSettings = 1;
    if (settingsWantingSources() > 0)
        writeLog("POOL in Part II the data source pool must be enlarged and it "
                 "was not (%s), so none of the framework's settings are made",
                 reason);
}

/* Runs once, from the first CreateDataSource and before any setting of ours
   is made. Enlarges the pool by extraSourceSlots where it can, and settles how
   many settings of ours are made. */
static void enlargeDataSourcePool() {
    if (!global_game) return;
    if (InterlockedCompareExchange(&global_poolSettled, 1, 0) != 0) return;
    static const char* const leftAlone = "the data source pool was left alone";
    if (global_extraSourceSlots <= 0) {
        leavePoolAlone("extraSourceSlots is 0", nullptr,
                       "extraSourceSlots is 0, which leaves the data source "
                       "pool as the engine made it");
        return;
    }

    const uint64_t pool = global_moduleBase + global_game->sourcePool;
    uint64_t memory = 0, bits = 0, count = 0, stride = 0, live = 0, align = 0;
    if (!readQword(pool + SOURCE_POOL_MEMORY, &memory)
        || !readQword(pool + SOURCE_POOL_BITS, &bits)
        || !readQword(pool + SOURCE_POOL_COUNT, &count)
        || !readQword(pool + SOURCE_POOL_STRIDE, &stride)
        || !readQword(pool + SOURCE_POOL_LIVE, &live)
        || !readQword(pool + SOURCE_POOL_ALIGN, &align)) {
        leavePoolAlone("it could not be read",
                       "the data source pool could not be read, so it was "
                       "left alone", leftAlone);
        return;
    }
    char line[160], reason[96];
    if (stride != SOURCE_POOL_NODE_BYTES || align != SOURCE_POOL_ALIGNMENT
        || !memory || !bits || !count) {
        formatInto(line, sizeof(line), "that is not the shape this build "
                   "expects, %d bytes a node aligned to %d, so the pool was "
                   "left alone", SOURCE_POOL_NODE_BYTES, SOURCE_POOL_ALIGNMENT);
        formatInto(reason, sizeof(reason), "it is not the shape the framework "
                   "expects, %d bytes a node aligned to %d",
                   SOURCE_POOL_NODE_BYTES, SOURCE_POOL_ALIGNMENT);
        leavePoolAlone(reason, line, leftAlone);
        return;
    }
    if (live != 0) {
        formatInto(line, sizeof(line), "%llu node(s) are already in use, and "
                   "the block cannot be moved with anything in it, so the pool "
                   "was left alone", (unsigned long long)live);
        formatInto(reason, sizeof(reason), "%llu node(s) were already in use",
                   (unsigned long long)live);
        leavePoolAlone(reason, line, leftAlone);
        return;
    }

    const uint64_t wanted = count + (uint64_t)global_extraSourceSlots;
    const uint64_t words  = (wanted + 63) / 64;
    void* block = VirtualAlloc(nullptr, (SIZE_T)(wanted * SOURCE_POOL_NODE_BYTES),
                               MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    void* bitmap = VirtualAlloc(nullptr, (SIZE_T)(words * 8),
                                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!block || !bitmap) {
        if (block)  VirtualFree(block, 0, MEM_RELEASE);
        if (bitmap) VirtualFree(bitmap, 0, MEM_RELEASE);
        formatInto(line, sizeof(line), "there was no memory for %llu node(s), "
                   "so the pool was left alone", (unsigned long long)wanted);
        formatInto(reason, sizeof(reason), "there was no memory for %llu "
                   "node(s)", (unsigned long long)wanted);
        leavePoolAlone(reason, line, leftAlone);
        return;
    }

    int ok = 0;
    GUARD_TRY {
        *(uint64_t*)(pool + SOURCE_POOL_MEMORY)    = (uint64_t)block;
        *(uint64_t*)(pool + SOURCE_POOL_BITS)      = (uint64_t)bitmap;
        *(uint64_t*)(pool + SOURCE_POOL_BITCOUNT)  = wanted;
        *(uint64_t*)(pool + SOURCE_POOL_BITSROUND) = words * 64;
        *(uint32_t*)(pool + SOURCE_POOL_WORDS)     = (uint32_t)words;
        *(uint64_t*)(pool + SOURCE_POOL_COUNT)     = wanted;
        *(uint64_t*)(pool + SOURCE_POOL_LAST)      = wanted - 1;
        ok = 1;
    } GUARD_EXCEPT_DESCRIBING("giving the data source pool a larger block") {
        ok = 0;
    }
    if (!ok) {
        writeLog("POOL the descriptor could not be written, so the pool may be "
                 "in a half changed state and no source of ours will be made");
        global_maxFrameworkSources = 0;
        global_poolLimitedSettings = 1;
        return;
    }
    /* The pool now counts wanted nodes, and the allocator hands out none past
       that count. */
    const int added = (int)(wanted - count);
    char why[80];
    formatInto(why, sizeof(why), "the data source pool was given %d more "
               "node(s)", added);
    limitOurSettings(added, why);
}

static void createDataSourceHook(void* registers, void*) {
    /* Before the first source goes in, while the pool still holds nothing. */
    enlargeDataSourcePool();
    makeOurSettings(loaderHookRegister(registers, LOADER_REG_RCX));
}

/* -------------------------------------------------- a setting of our own */

/*
    Each setting of ours is given a copy of its class's table, this many words
    long. The slots below answer from the setting, the words past the class's
    own table are replaced, and every other slot runs the engine's code.
*/
#define OWN_VTABLE_SLOTS  64

/*
    The slots ours answers. Eleven is the value setter. One and nine are the
    pair that decides what a row shows. These and AT_DEFAULT_SLOT are the same
    in every build. The others are the build's, Part I's and Part II's:

        hiddenSlot            20  21
        availabilitySlot      21  22
        labelSlot             24  25   GetLabelTextId
        descriptionSlot       25  26   GetDescriptionTextId
        dependencyTextSlot    26  27   GetDependencyDescriptionId
        valueRangeSlot        28  29
        valuePointerSlot      30  31   a slider's GetValue, a selector's
                                       GetValuePtr

    Part II has one slot more, at 16, so every slot from Part I's 16 up is one
    higher there. The slot numbers and addresses below are Part I's.
*/
#define SET_ITEM_VALUE_SLOT  11
#define FIELD_CONFIG_SLOT     1
#define OUR_ITEM_DATA_SLOT    9

/*
    How the engine locks a setting of its own.

    Menu2::IDataSource::PopulateComponentFromItem at rva 025D8B18 runs for every
    control on every refresh. It asks the control's data source slot 21 for one
    number and acts on it:

        0   SetDisabled(true),  SetAppearDisabled(false)
        1   SetDisabled(false), SetAppearDisabled(false)   the ordinary state
        2   SetDisabled(false), SetAppearDisabled(true)
        3+  neither is called

    SetAppearDisabled writes bit 4 of the byte at +0x560 on the component and
    then asks it for the animation state it should be in. Menu2::Component::
    IsDisabled at 026653D4 reads that bit, so the component answers "disabled",
    and menu/components/menu-item-animations.widget keyframes that state at
    frame 1.5: the padlock becomes visible, the title, the value and the bar all
    take the tint t2-menu-disabled, and the row's own text drops to no alpha.
    The selector's update at 026BDE90 and the slider's at 026A8961 both read the
    same bit and take the branch that ignores input.

    The text is the other half. Slot 27 of the same table builds what the
    description pane shows, and its first act is to ask slot 21: an answer of 1
    leaves the pane alone, and anything else joins two strings with the text
    template SPACE_SEPARATED_TEXTS_2 - the description, from the component's
    +0x4D8 which asks slot 25, and the dependency description, from +0x4E0 which
    asks slot 26. So one number drives the padlock, the refusal and the red text
    together, and slot 26 supplies the wording.

    Slot 20 answers whether a row is hidden. PopulateComponentFromItem asks it
    on every refresh, straight after slot 21, and hands the answer to 02664F34,
    which keeps it on the component: a component it says is hidden is not
    drawn, is left out of its container's layout so the rows below close up,
    cannot take focus, and is passed over by the page's reset check.
*/

/*
    Slot 13 answers whether a control is at its default.

    Menu2::IDataSource::GetBoxedLabelText at 025D71B0 asks it, and writes the
    title through "\xe2\x80\xa2 %s" at 0313B568 when the answer is no, which is
    the bullet. Menu2::Page asks it too, at 01C79B00, over every control on the
    page: the reset to defaults prompt is offered only while at least one
    control that is available answers no.
*/
#define AT_DEFAULT_SLOT      13

/*
    Slot 27 builds what the description pane shows while a control is not
    available. It answers with a boxed array, not a string: 0048DCE0 ends by
    calling 01FD3278 over a list whose first element is the text id of the
    template it used, boxed as type 7, and whose second is the composed text,
    boxed as type 0x13. Anything answered here has to be that shape.
*/
#define AVAILABLE            1
#define APPEARS_DISABLED     2

/*
    The two slots that decide what a row shows.

    Slot 1 is asked to fill in a field configuration: which named child widgets
    of the row are to be written, and how many values each takes. Slot 9 is then
    asked for the values, and the engine walks them in the order the
    configuration named. The count in the list is never read: what is read is
    the sum over the configuration, so the two have to agree exactly or the
    engine reads past the end of the list.

    A row of this shape names two children:

        label0  takes one value, the title, a plain string with tag 0x13
        text0   takes one value, the option, a StringId64 with tag 7

    A title is not a localisation key. It is a pointer to text.

    A value is sixteen bytes: the payload first, then a type tag at +0x0C. What
    slot 9 returns is a value of tag 0x15 whose payload is a list, and the list
    holds pointers to the values themselves.
*/
#define FIELD_WIDGET_NAMES    0x000
#define FIELD_VALUE_NAMES     0x100
#define FIELD_VALUES_PER      0x280
#define FIELD_WIDGET_COUNT    0x2A0
#define FIELD_VALUE_COUNT     0x2A4
#define FIELD_MOST_PER_WIDGET 0x2A8

#define BOXED_INT                2
#define BOXED_FLOAT              6
#define BOXED_STRING          0x13
#define BOXED_LIST            0x15

struct __declspec(align(16)) BoxedValue {
    const void* payload;
    uint32_t    spare;
    uint32_t    type;
};

/* A float sits in the low four bytes of the payload. */
static void boxFloat(BoxedValue* box, float f) {
    box->payload = 0;
    memcpy(&box->payload, &f, sizeof f);
    box->spare = 0;
    box->type  = BOXED_FLOAT;
}

static void boxString(BoxedValue* box, const char* text) {
    box->payload = text;
    box->spare = 0;
    box->type = BOXED_STRING;
}

struct __declspec(align(16)) BoxedList {
    const BoxedValue* const* items;
    int32_t                  count;
    int32_t                  zero;
};

/*
    What one item hands back, held in a ring per setting. The engine reads
    through the pointers after the call returns, and asks about several items
    while one row is drawn.
*/
struct ItemData {
    /* The two character buffers together are a whole number of sixteen byte
       units. */
    char              text[MAX_SETTING_TEXT];
    char              title[MAX_SETTING_TEXT + 16];
    BoxedValue        value[4];
    const BoxedValue* items[4];
    BoxedList         list;
    BoxedValue        boxed;
};

#define ITEMS_HELD 16

static ItemData global_itemData[MAX_FRAMEWORK_SOURCES][ITEMS_HELD];

/* The first count values of d boxed as the list the engine walks. */
static uint64_t boxItems(ItemData* d, int count) {
    for (int i = 0; i < count; ++i) d->items[i] = &d->value[i];
    d->list.items = d->items;
    d->list.count = count;
    d->list.zero  = 0;
    d->boxed.payload = &d->list;
    d->boxed.spare   = 0;
    d->boxed.type    = BOXED_LIST;
    return (uint64_t)&d->boxed;
}

/* The children a row writes, and the values they take. */
static uint64_t global_labelWidgetId = 0;
static uint64_t global_textWidgetId  = 0;
static uint64_t global_barWidgetId   = 0;
static uint64_t global_valueId       = 0;
static uint64_t global_percentId     = 0;

/* The bar takes two values: percent, a fill from zero to one, and the count of
   intervals in the range. */
#define BAR_INTERVALS_ID 0xfaffc910bc555e6bull

/* ------------------------------------------- the framework's own texts */

/*
    The texts the framework hands the game by id: a control's description,
    the reason a control is greyed out, and a button's label. Each is kept
    under its id in a store of the framework's own, and the game's text
    lookup is answered from the store where the lookup found nothing
    (textAnswered).

    global_textArena    TEXT_STORE_BYTES, filled from the front. A text is
                        copied in whole with its terminator and is never
                        changed or freed, so a pointer to it that the game
                        holds stays valid while the game runs.
    global_storedText   TEXT_STORE_SLOTS slots, which must be a power of two,
                        each an id and the text it is answered with. An id
                        is looked for from the slot id & (TEXT_STORE_SLOTS
                        - 1) onwards, one slot at a time, until it or an
                        empty slot (id 0) is found. At most TEXT_STORE_MOST
                        ids are held, so a search always meets an empty
                        slot.

    storeText writes only while it holds global_textStoreLock exclusively.
    textOfId takes no lock: a slot's text is written before its id, and a
    text is replaced by swapping the pointer whole, so a reader that finds an
    id finds a whole text.
*/
#define TEXT_STORE_BYTES  (10u * 1024u * 1024u)
#define TEXT_STORE_SLOTS  16384
#define TEXT_STORE_MOST   (TEXT_STORE_SLOTS / 4 * 3)

struct StoredText {
    volatile uint64_t    id;
    const char* volatile text;
};

static StoredText global_storedText[TEXT_STORE_SLOTS];
static int        global_storedTextCount = 0;
static char*      global_textArena = nullptr;
static size_t     global_textArenaUsed = 0;
/* The store had no room for a text: it keeps nothing more, and the line
   saying so has been written. */
static int        global_textStoreFull = 0;
static SRWLOCK    global_textStoreLock = SRWLOCK_INIT;

/* The arena, made once, before any text is stored. Where there is no memory
   for it a line says so, and no text is stored. */
static void openTextStore() {
    if (global_textArena) return;
    global_textArena = (char*)VirtualAlloc(nullptr, TEXT_STORE_BYTES,
                                           MEM_COMMIT | MEM_RESERVE,
                                           PAGE_READWRITE);
    if (!global_textArena)
        writeLog("TEXT there was no memory for the framework's store of texts "
                 "(%u megabytes), so descriptions, the reasons controls are "
                 "greyed out, and the labels of buttons that have no setting "
                 "of their own are not shown",
                 TEXT_STORE_BYTES / (1024u * 1024u));
}

/* The text the store answers id with, or nullptr. */
static const char* textOfId(uint64_t id) {
    if (!id) return nullptr;
    for (uint64_t probe = 0; probe < TEXT_STORE_SLOTS; ++probe) {
        const StoredText* slot =
            &global_storedText[(id + probe) & (TEXT_STORE_SLOTS - 1)];
        const uint64_t held = slot->id;
        if (held == id) return slot->text;
        if (!held) return nullptr;
    }
    return nullptr;
}

/*
    text kept under id. An id the store holds with another text is given this
    one. Answers 1 where the store now answers id with text, and 0 where it
    cannot: id is 0, there is no arena, or the store is full. The store is
    full from the first text there is no room for in the arena or the table,
    which one line says; from then on it keeps nothing more.
*/
static int storeText(uint64_t id, const char* text) {
    if (!id || !text || !global_textArena) return 0;
    AcquireSRWLockExclusive(&global_textStoreLock);
    StoredText* slot = nullptr;
    for (uint64_t probe = 0; probe < TEXT_STORE_SLOTS && !slot; ++probe) {
        StoredText* at =
            &global_storedText[(id + probe) & (TEXT_STORE_SLOTS - 1)];
        if (at->id == id || !at->id) slot = at;
    }
    int stored = 0;
    if (slot && slot->id == id && strcmp(slot->text, text) == 0)
        stored = 1;
    else if (slot && !global_textStoreFull) {
        const size_t length = strlen(text) + 1;
        const int newId = !slot->id;
        if (length <= TEXT_STORE_BYTES - global_textArenaUsed
            && (!newId || global_storedTextCount < TEXT_STORE_MOST)) {
            char* kept = global_textArena + global_textArenaUsed;
            memcpy(kept, text, length);
            global_textArenaUsed += length;
            if (newId) {
                slot->text = kept;
                InterlockedExchange64((volatile LONG64*)&slot->id, (LONG64)id);
                ++global_storedTextCount;
            } else
                InterlockedExchangePointer((PVOID volatile*)&slot->text, kept);
            stored = 1;
        } else {
            global_textStoreFull = 1;
            writeLog("TEXT the framework's store of texts is full, with %d "
                     "texts in %u of its %u bytes, so from now on a "
                     "description, a reason a control is greyed out or a "
                     "button label that it does not hold already is not "
                     "shown", global_storedTextCount,
                     (unsigned)global_textArenaUsed, TEXT_STORE_BYTES);
        }
    }
    ReleaseSRWLockExclusive(&global_textStoreLock);
    return stored;
}

/*
    Where the game's text lookup ends, for one build of the game: the
    instruction every lookup passes with its answer (in Part I, every lookup
    but those of the two ids that stand for no text).

        answeredRva       that instruction, where the hook is placed
        answeredBytes     its bytes, answeredLength of them
        databaseRva       the game's main text database
        idRegister        the LOADER_REG_ number of the register that holds,
                          at that instruction, the id looked up, wherever the
                          answer is 0
        databaseRegister  the same for the text database it was looked up in
        answerRegister    the same for the answer: the text, or 0 where none
                          was found
*/
struct TextLookupEnd {
    uint64_t       answeredRva;
    const uint8_t* answeredBytes;
    size_t         answeredLength;
    uint64_t       databaseRva;
    int            idRegister;
    int            databaseRegister;
    int            answerRegister;
};

/* tlou-i.exe 1.1.5.0: TextDb::FindText (0048E750) ends at mov rdi,[rsp+80h]
   (0048E87C) with the id in rbx, the text database in rbp and the answer in
   rax. The main text database is 0346B8C0. */
static const uint8_t partOneTextAnsweredBytes[] = {
    0x48, 0x8B, 0xBC, 0x24, 0x80, 0x00, 0x00, 0x00 };

static const TextLookupEnd partOneTextLookup = {
    0x0048E87Cull, partOneTextAnsweredBytes, sizeof(partOneTextAnsweredBytes),
    0x0346B8C0ull, LOADER_REG_RBX, LOADER_REG_RBP, LOADER_REG_RAX };

/*
    tlou-ii.exe 1.6.0: TextDb::FindText (014AF010) ends at mov rcx,[rsp+50h]
    (014AF2AA) with the answer in rax and the text database in rdi. Where the
    answer is 0, rcx holds the id looked up: the key FindText made of it, the
    id itself for every id but some of the game's own. The main text database
    is 036B3070. tlou-ii-l.exe 1.6.0: the same, FindText 014C5E00 ending at
    014C609A, the main text database 036EB070.
*/
static const uint8_t partTwoTextAnsweredBytes[] = {
    0x48, 0x8B, 0x4C, 0x24, 0x50 };

static const TextLookupEnd partTwoTextLookup = {
    0x014AF2AAull, partTwoTextAnsweredBytes, sizeof(partTwoTextAnsweredBytes),
    0x036B3070ull, LOADER_REG_RCX, LOADER_REG_RDI, LOADER_REG_RAX };

static const TextLookupEnd partTwoLTextLookup = {
    0x014C609Aull, partTwoTextAnsweredBytes, sizeof(partTwoTextAnsweredBytes),
    0x036EB070ull, LOADER_REG_RCX, LOADER_REG_RDI, LOADER_REG_RAX };

/* The text lookup of the build installHooks checked. */
static const TextLookupEnd* global_textLookup = nullptr;

/* The hook where the lookup ends is placed, so the store's texts are
   answered. */
static int global_textLookupHooked = 0;

/* How many lookups were answered with a text of the store's. */
static volatile LONG global_textAnswers = 0;

/*
    Where the game's text lookup ends. A lookup the game answered is left as
    it is. One in the main text database that found nothing is answered with
    the text the store holds for its id, where it holds one.
*/
static void textAnswered(void* registers, void*) {
    const TextLookupEnd* place = global_textLookup;
    if (!place || loaderHookRegister(registers, place->answerRegister)) return;
    if (loaderHookRegister(registers, place->databaseRegister)
        != global_moduleBase + place->databaseRva)
        return;
    const char* text =
        textOfId(loaderHookRegister(registers, place->idRegister));
    if (!text) return;
    loaderHookSetRegister(registers, place->answerRegister,
                          (uint64_t)(uintptr_t)text);
    InterlockedIncrement(&global_textAnswers);
}

/* Whether the count bytes at at are the loader's jump for another plugin's
   hook at the same place: E9 and its 32-bit displacement, then 90 for each
   byte after the fifth. */
static int loaderJumpOver(const uint8_t* at, size_t count) {
    if (count < 5 || at[0] != 0xE9) return 0;
    for (size_t i = 5; i < count; ++i)
        if (at[i] != 0x90) return 0;
    return 1;
}

/* The hook where the game's text lookup ends, placed where the game's bytes
   are the ones expected, or where the loader's jump for another plugin's
   hook stands over them. */
static void hookTextLookup() {
    static const char* const lost =
        "descriptions, the reasons controls are greyed out, and the labels "
        "of buttons that have no setting of their own are not shown";
    const TextLookupEnd* place = global_textLookup;
    if (!place) return;
    const uint64_t address = global_moduleBase + place->answeredRva;
    int expected = 0;
    GUARD_TRY {
        const uint8_t* at = (const uint8_t*)(uintptr_t)address;
        expected = memcmp(at, place->answeredBytes, place->answeredLength) == 0
                || loaderJumpOver(at, place->answeredLength);
    } GUARD_EXCEPT_DESCRIBING("comparing the game's text lookup") {
        expected = 0;
    }
    if (!expected) {
        writeLog("TEXT the game's code where its text lookup ends is not what "
                 "the framework expects, so %s", lost);
        return;
    }
    if (!loaderHookAddress("modmenu_text_answered", address, textAnswered,
                           nullptr)) {
        writeLog("TEXT the loader refused the hook where the game's text "
                 "lookup ends, so %s", lost);
        return;
    }
    global_textLookupHooked = 1;
}

static uint64_t global_ownVtable[MAX_FRAMEWORK_SOURCES][OWN_VTABLE_SLOTS];

/* GetFieldConfig(FieldConfig* out, int index). */
static void ourGetFieldConfig(uint64_t source, uint64_t config, uint64_t) {
    if (!config) return;
    GUARD_TRY {
        uint64_t* widgets = (uint64_t*)(config + FIELD_WIDGET_NAMES);
        uint64_t* values  = (uint64_t*)(config + FIELD_VALUE_NAMES);
        uint8_t*  per     = (uint8_t*)(config + FIELD_VALUES_PER);
        const OurSetting* s = ourSourceByObject(source);

        /*
            The byte the class keeps at +0x31, which slot 18 (Part II 19)
            answers with. The data source's own Init writes 1 into it for
            every source the engine builds. The component reads it once, five
            slots after its own Init, to decide whether to bind the source
            again.
        */
        if (s && s->object) *(uint8_t*)(s->object + 0x31) = 1;

        /*
            The widgets are taken in this order and each is given as many of the
            item's values as per says, counted from the front. The list
            GetItemData builds must be in the same order.

            A button names one child and takes one value, its label. Every other
            kind names label0 for the title and text0 for the value, and a
            slider names the bar as well, which takes two.
        */
        int widgetCount = 1, valueCount = 1, mostPerWidget = 1;
        widgets[0] = global_labelWidgetId;
        values[0]  = global_valueId;
        per[0]     = 1;
        /* A keybind's row is a button: its cell is filled by the framework
           itself (keybindTick). Where keys cannot be set it is a selector's
           row, and text0 shows the key's name. */
        if (!s || (s->kind != KIND_BUTTON
                   && (s->kind != KIND_KEYBIND || keyShownOnly(s)))) {
            widgets[1] = global_textWidgetId;
            values[1]  = global_valueId;
            per[1]     = 1;
            widgetCount = valueCount = 2;
        }
        if (s && s->kind == KIND_SLIDER) {
            widgets[2] = global_barWidgetId;
            values[2]  = global_percentId;
            values[3]  = BAR_INTERVALS_ID;
            per[2]     = 2;
            widgetCount = 3;
            valueCount = 4;
            mostPerWidget = 2;
        }
        *(int32_t*)(config + FIELD_WIDGET_COUNT)    = widgetCount;
        *(int32_t*)(config + FIELD_VALUE_COUNT)     = valueCount;
        *(int32_t*)(config + FIELD_MOST_PER_WIDGET) = mostPerWidget;
    } GUARD_EXCEPT_DESCRIBING("writing a field configuration of ours") { }
}

/*
    Reset to defaults.

    Every control drawn on the page being reset is put back to its
    DefaultValue. For a locked control, every value it put aside is put back
    to its default first, one for each setting it can reach. A locked control
    that put a value aside for the setting it reads and whose settings file
    holds its default is left as it is. Any other locked control, one that put
    nothing aside or whose settings file holds something else, as when the
    default could not be written when it locked, is put back to its default,
    and its settings file written with it. A keybind whose key cannot be set
    in this build (keyShownOnly) is left as it is.
*/
static int settingIsAtDefault(const OurSetting* s);

/*
    Every setting a control reaches through its name, put back to its default.
    The one it is reading now is left to the caller.
*/
static void resetEveryKeyOf(OurSetting* s) {
    KeyAndDefault reach[MAX_KEYS_OF_ONE];
    const int count = everyKeyOf(s, reach, MAX_KEYS_OF_ONE);
    for (int k = 0; k < count; ++k) {
        if (_stricmp(reach[k].key, s->storeKey) == 0) continue;
        char held[MAX_SETTING_TEXT];
        storedText(s->storePath, s->storeSection, reach[k].key, "", held,
                   sizeof(held));
        if (!held[0] || _stricmp(held, reach[k].fallback) != 0)
            keepText(s->storePath, s->storeSection, reach[k].key,
                     reach[k].fallback);
    }
}

/*
    Every value a locked control put aside, one for each setting it can reach,
    put back to that setting's default.
*/
static void resetEveryPutAside(OurSetting* s) {
    KeyAndDefault reach[MAX_KEYS_OF_ONE];
    const int count = everyKeyOf(s, reach, MAX_KEYS_OF_ONE);
    for (int k = 0; k < count; ++k) {
        CacheNames names;
        cacheNames(s, reach[k].key, &names);
        char stored[MAX_SETTING_TEXT];
        storedText(global_settingsCachePath, names.section, names.storedKey, "",
                   stored, sizeof(stored));
        if (stored[0] && reach[k].fallback[0]
            && _stricmp(stored, reach[k].fallback) != 0)
            keepText(global_settingsCachePath, names.section, names.storedKey,
                     reach[k].fallback);
    }
}

/* page is the page being reset. */
static void resetOpenPage(uint64_t page) {
    forgetPutAsideOfNewFiles();
    /* Which controls are locked is read before anything is put back. */
    int wasLocked[MAX_FRAMEWORK_SOURCES];
    for (int i = 0; i < global_ourSourceCount; ++i)
        wasLocked[i] = holdsValue(&global_ourSources[i])
                     ? whyLocked(&global_ourSources[i]) : LOCKED_BY_NOTHING;

    for (int i = 0; i < global_ourSourceCount; ++i) {
        OurSetting* s = &global_ourSources[i];
        if (!holdsValue(s) || keyShownOnly(s) || !rowIsOnPage(rowNow(s), page))
            continue;
        /* The settings this control reaches through its name but is not
           reading at the moment, whether or not it is locked. */
        resetEveryKeyOf(s);
        if (wasLocked[i] != LOCKED_BY_NOTHING) {
            resetEveryPutAside(s);
            PutAside aside;
            readPutAside(s, s->storeKey, s->fallbackText, &aside);
            if (aside.stored[0]
                && presetValuesMatch(aside.held, s->fallbackText))
                continue;
        } else if (settingIsAtDefault(s) && s->markAsModified) continue;
        takeValue(s, s->fallbackText, 0);
        s->touched = 1;
        keepText(s->storePath, s->storeSection, s->storeKey, s->fallbackText);
        composeList(listOf(s));
    }
    settleAfterChange(0);
    for (int i = 0; i < global_ourSourceCount; ++i)
        if (global_ourSources[i].rowRecord) refreshControl(&global_ourSources[i]);
}

/*
    Page::ResetToDefaults, reached only by a confirmed reset: from the ok
    branch of the message box handler and from the reset-page-defaults script
    verb. The game's own options pages are put back through it as well. A
    cancelled reset does not reach it.

        rcx  the page being put back
*/
static void resetPageHook(void* registers, void*) {
    resetOpenPage(loaderHookRegister(registers, LOADER_REG_RCX));
}

/* Page::ResetToDefaults from its opening through its first call, in
   tlou-i.exe 1.1.5.0 (01C7C268), then in tlou-ii.exe and tlou-ii-l.exe 1.6.0
   (01E3D7F0 and 01E664C0, the same bytes in both). The loader's jump replaces
   the first instruction, 5 bytes, in each. */
static const uint8_t partOneResetPageBytes[] = {
    0x48,0x89,0x5C,0x24,0x10, 0x48,0x89,0x74,0x24,0x18, 0x48,0x89,0x7C,0x24,0x20,
    0x55, 0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57, 0x48,0x8B,0xEC,
    0x48,0x83,0xEC,0x50, 0x48,0x8B,0xD1, 0x4C,0x8B,0xF1, 0xE8,0x5E,0xFF,0xFF,0xFF
};

static const uint8_t partTwoResetPageBytes[] = {
    0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x6C,0x24,0x10, 0x48,0x89,0x74,0x24,0x18,
    0x48,0x89,0x7C,0x24,0x20, 0x41,0x56, 0x48,0x83,0xEC,0x60, 0x48,0x8B,0xD1,
    0x4C,0x8B,0xF1, 0xE8,0x5B,0xFF,0xFF,0xFF
};

/* Whether code the framework hooks stands at its address as expected, or
   with the loader's jump for another plugin's hook over its first
   jumpLength bytes and the rest as expected. */
static int hookedCodeAsExpected(const ExpectedCode& code) {
    if (codeAsExpected(code)) return 1;
    if (!code.rva || !code.bytes || code.jumpLength < 5
        || code.jumpLength > code.length)
        return 0;
    int same = 0;
    GUARD_TRY {
        const uint8_t* at = (const uint8_t*)(uintptr_t)(global_moduleBase
                                                        + code.rva);
        same = loaderJumpOver(at, code.jumpLength)
            && memcmp(at + code.jumpLength, code.bytes + code.jumpLength,
                      code.length - code.jumpLength) == 0;
    } GUARD_EXCEPT_DESCRIBING("comparing the game's code") { same = 0; }
    return same;
}

static void watchResetButton() {
    if (!hookedCodeAsExpected(global_game->resetPage)) {
        writeLog("RESET the game's code that puts a page back to its defaults "
                 "is not what the framework expects, so the reset prompt does "
                 "nothing");
        return;
    }
    if (!loaderHookAddress("Page::ResetToDefaults",
                           global_moduleBase + global_game->resetPage.rva,
                           resetPageHook, nullptr))
        writeLog("RESET the loader refused Page::ResetToDefaults, so the reset "
                 "prompt does nothing");
}

/*
    GetItemData(int index, const Gui2::Widget*).

    A row shows the title in label0 and the text for this item in text0, both
    plain strings, and a slider's bar takes two more values. A toggle and a
    selector take their text from the options named in the menu file; a slider
    has no named options, so the number itself is written out. A keybind whose
    key cannot be set (keyShownOnly) shows its key's name as its one option.
*/
static const char* keyNameOf(int key, char* out, size_t size);

static uint64_t ourGetItemData(uint64_t source, uint64_t index,
                               uint64_t widget) {
    OurSetting* s = ourSourceByObject(source);
    if (!s) return 0;
    refreshExpressions(s);
    presetFollow(s);

    const int which = (int)(s - &global_ourSources[0]);
    if (which < 0 || which >= MAX_FRAMEWORK_SOURCES) return 0;
    if (widget && global_rowsFollowed) {
        const uint64_t row = rowOf(widget, source);
        if (row && row != rowNow(s)) holdRow(s, row);
    }

    const int at = (int)(int32_t)(uint32_t)index;
    if (at < 0) return 0;
    if (s->kind == KIND_BUTTON
        || (s->kind == KIND_KEYBIND && !keyShownOnly(s))) {
        if (at > 0) return 0;
        ItemData* b = &global_itemData[which][0];
        /* A keybind carries the bullet a changed setting carries. */
        if (s->kind == KIND_BUTTON || settingIsAtDefault(s))
            strcpy_s(b->title, sizeof(b->title), s->title);
        else
            sprintf_s(b->title, "\xe2\x80\xa2 %s", s->title);
        boxString(&b->value[0], b->title);
        return boxItems(b, 1);
    }
    if (s->kind != KIND_SLIDER && at >= s->optionCount) return 0;

    ItemData* d = &global_itemData[which][at % ITEMS_HELD];
    if (s->kind == KIND_SLIDER) {
        /* A value the store held that the range cannot reach is shown as it
           stands until the control is used, and as a number where the engine
           cannot draw it as it stands. */
        if (s->mismatch && !s->touched) {
            if (fatalPipeAt(s->shown) < 0 && fatalTagAt(s->shown) < 0)
                strcpy_s(d->text, sizeof(d->text), s->shown);
            else
                writeNumber(d->text, sizeof(d->text), s->value);
        } else if (!sliderValueText(s, d->text, sizeof(d->text)))
            writeNumber(d->text, sizeof(d->text), s->value);
    } else if (s->kind == KIND_KEYBIND) {
        keyNameOf((int)s->value, d->text, sizeof(d->text));
    } else {
        refreshOptionLabel(s, at);
        strcpy_s(d->text, sizeof(d->text), s->option[at]);
    }

    /* The bullet a changed setting carries, in front of the title. */
    if (settingIsAtDefault(s))
        strcpy_s(d->title, sizeof(d->title), s->title);
    else
        sprintf_s(d->title, "\xe2\x80\xa2 %s", s->title);
    boxString(&d->value[0], d->title);
    boxString(&d->value[1], d->text);
    if (s->kind != KIND_SLIDER) return boxItems(d, 2);

    float part = 0.0f, intervals = 0.0f;
    if (s->logarithmic) {
        const int positions = positionsOf(s);
        intervals = (float)(positions - 1);
        if (positions > 1)
            part = (float)(positionOfValue(s, s->value) / (positions - 1));
    } else {
        const float span = s->greatest - s->least;
        part = span > 0.0f ? (s->value - s->least) / span : 0.0f;
        intervals = s->step > 0.0f ? span / s->step : 0.0f;
    }
    /* The bar clamps; the number does not. */
    boxFloat(&d->value[2], part < 0.0f ? 0.0f : part > 1.0f ? 1.0f : part);
    boxFloat(&d->value[3], intervals);
    return boxItems(d, 4);
}

/*
    The range, which is also what the engine works the number of positions out
    from. The two classes want it differently.

    A selector is given three integers: least, greatest, and the value to fall
    back on. Its item count is greatest - least + 1.

    A slider is given four floats: least, greatest, how many steps lie between
    them, and the value to fall back on. The third is a count of steps, not a
    step size.

    A logarithmic slider is given its positions instead: 0 to the last, one
    step each, with its value answered as the position it sits at. The engine
    steps, drags and clicks through positions, and ourSetItemValue turns the
    position it is handed back into a value.
*/
static void ourGetValueRange(uint64_t source, uint64_t least, uint64_t greatest,
                             uint64_t third, uint64_t fourth) {
    OurSetting* s = ourSourceByObject(source);
    if (!s) return;
    refreshExpressions(s);
    presetFollow(s);
    /* A control with no options has no range to give, and nothing is
       written. */
    if (s->kind != KIND_SLIDER && s->optionCount <= 0) return;
    GUARD_TRY {
        if (s->kind == KIND_SLIDER && s->logarithmic) {
            const int positions = positionsOf(s);
            if (least)    *(float*)least    = 0.0f;
            if (greatest) *(float*)greatest = (float)(positions - 1);
            if (third)    *(float*)third    = (float)(positions - 1);
            if (fourth)   *(float*)fourth   = (float)positionOfValue(s, s->fallback);
        } else if (s->kind == KIND_SLIDER) {
            const float span = s->greatest - s->least;
            if (least)    *(float*)least    = s->least;
            if (greatest) *(float*)greatest = s->greatest;
            if (third)    *(float*)third    = s->step > 0.0f ? span / s->step : 1.0f;
            if (fourth)   *(float*)fourth   = s->fallback;
        } else {
            int fallbackAt = 0;
            for (int o = 0; o < s->optionCount; ++o)
                if (!_stricmp(s->optionValue[o], s->shown)) fallbackAt = o;
            if (least)    *(int*)least    = 0;
            if (greatest) *(int*)greatest = s->optionCount - 1;
            if (third)    *(int*)third    = fallbackAt;
        }
    } GUARD_EXCEPT_DESCRIBING("giving the range of a setting of ours") { }
}

/*
    Slot 30. GetValue(const Gui2::Widget*) on a slider answers with the value
    itself as a float, in xmm0. A selector is answered by ourGetValuePtr, with
    the address of its position, an int.
*/
static float ourGetValue(uint64_t source, uint64_t) {
    OurSetting* s = ourSourceByObject(source);
    if (!s) return 0.0f;
    refreshExpressions(s);
    if (s->kind == KIND_SLIDER && s->logarithmic)
        return (float)positionOfValue(s, s->value);
    return s->asFloat;
}

static uint64_t ourGetValuePtr(uint64_t source, uint64_t) {
    OurSetting* s = ourSourceByObject(source);
    if (!s) return 0;
    refreshExpressions(s);
    presetFollow(s);
    return (uint64_t)&s->index;
}

/* A text id handed back through out, the way the engine asks for one. */
static uint64_t answerTextId(uint64_t out, uint64_t id, const char* during) {
    GUARD_TRY {
        if (out) *(uint64_t*)out = id;
    } GUARD_EXCEPT_DESCRIBING(during) { }
    return out;
}

/*
    GetDescriptionTextId(StringId64* out, ...). The pane asks by id: the text
    is kept in the framework's store of texts under the hash of the text
    itself, one id per wording, and that id handed back. 0, which shows no
    description, where the store cannot answer the game with it.
*/
static uint64_t ourGetDescriptionTextId(uint64_t source, uint64_t out,
                                        uint64_t, uint64_t) {
    OurSetting* s = ourSourceByObject(source);
    if (!s) return out;
    refreshExpressions(s);
    uint64_t id = 0;
    if (s->description[0] && global_textLookupHooked) {
        char shown[MAX_SHOWN_TEXT];
        if (s->html) copyBounded(shown, sizeof(shown), s->description,
                                 "description");
        else escapeForMarkup(s->description, shown, sizeof(shown));
        id = hashText(shown);
        if (!storeText(id, shown)) id = 0;
    }
    return answerTextId(out, id,
                        "giving the description id of a setting of ours");
}

/*
    IsAtDefault(widget). A control the menu file does not mark with
    MarkAsModified True always answers yes, so no bullet is ever drawn on it and
    it is never counted when the page decides whether to offer its reset.
*/
static int settingIsAtDefault(const OurSetting* s) {
    if (!s->markAsModified || s->kind == KIND_BUTTON) return 1;
    if (s->kind == KIND_SLIDER) return sameNumber(s->value, s->fallback);
    /* A preset with an Off is at its default while Off holds; one with no
       DefaultValue has no default to differ from. */
    if (s->kind == KIND_PRESET && presetOff(s)) return presetOffHolds(s);
    if (s->kind == KIND_PRESET && !s->fallbackText[0]) return 1;
    if (s->kind == KIND_KEYBIND)
        return (int)s->value == keybindKeyOf(s->fallbackText);
    if (s->index >= 0 && s->index < s->optionCount)
        return _stricmp(s->optionValue[s->index], s->fallbackText) == 0;
    return 1;
}

/* A keybind whose key cannot be set (keyShownOnly) answers yes, since a reset
   leaves it as it is; its title still carries the bullet where its key is
   not its default. */
static uint8_t ourIsAtDefault(uint64_t source, uint64_t, uint64_t) {
    OurSetting* s = ourSourceByObject(source);
    presetFollow(s);
    return (uint8_t)(!s || keyShownOnly(s) || settingIsAtDefault(s) ? 1 : 0);
}

/*
    GetAvailability(). Asked on every refresh of every control.
*/
static uint32_t ourGetAvailability(uint64_t source) {
    OurSetting* s = ourSourceByObject(source);
    if (!s || s->kind == KIND_BUTTON) return AVAILABLE;
    const int why = whyLocked(s);
    if (why != s->locked || s->lockPutOff) takeLockState(s, why);
    return why == LOCKED_BY_NOTHING ? (uint32_t)AVAILABLE
                                    : (uint32_t)APPEARS_DISABLED;
}

/*
    Slot 20: whether the row is hidden. Asked on every refresh of every
    control, and by the page when it decides whether to offer its reset.
*/
static uint8_t ourIsHidden(uint64_t source) {
    OurSetting* s = ourSourceByObject(source);
    if (!s) return 0;
    const int hidden = controlIsHidden(s);
    if (s->hidden && !hidden) s->shownAgain = 1;
    s->hidden = hidden;
    return (uint8_t)(hidden ? 1 : 0);
}

/*
    GetDependencyDescriptionId(StringId64* out, ...). The pane joins this to the
    description whenever the answer above is not the ordinary state. It answers
    zero while the control is not locked.

    A control closed by the list it sits in shows the list's description rather
    than its own.
*/
static uint64_t ourGetDependencyDescriptionId(uint64_t source, uint64_t out,
                                              uint64_t, uint64_t) {
    OurSetting* s = ourSourceByObject(source);
    if (!s) return out;
    refreshExpressions(s);
    const int why = whyLocked(s);
    OurSetting* says = why == LOCKED_BY_ITS_LIST ? &global_ourSources[s->gatingList]
                     : why == LOCKED_BY_ITSELF   ? s : nullptr;
    uint64_t id = 0;
    if (says && says->dependencyDescription[0] && global_textLookupHooked) {
        char drawn[MAX_SHOWN_TEXT + MAX_LOCKED_MARKUP];
        lockedTextOf(says->dependencyDescription,
                     says->html || says->dependencyIsOurs, drawn,
                     sizeof(drawn));
        id = hashText(drawn);
        if (!storeText(id, drawn)) id = 0;
    }
    return answerTextId(out, id,
                        "giving the dependency description id of a setting "
                        "of ours");
}

/*
    GetLabelTextId(StringId64* out, ...). Menu2::IDataSource::GetBoxedLabelText
    asks for this first and gives up when it is zero, and then writes nothing:
    the title a row draws is given as a plain string in the item list.
*/
static uint64_t ourGetLabelTextId(uint64_t source, uint64_t out, uint64_t,
                                  uint64_t) {
    if (!ourSourceByObject(source)) return out;
    return answerTextId(out, 0, "giving the title id of a setting of ours");
}

/*
    Every slot copied past the class's own table is replaced with this, which
    logs the call and answers zero. A line from here means the engine asked for
    a method the class it was told about does not have.
*/
static volatile LONG global_unknownSlotLines = 0;

static uint64_t ourUnknownSlot(uint64_t source) {
    if (InterlockedIncrement(&global_unknownSlotLines) <= 32) {
        const OurSetting* s = ourSourceByObject(source);
        writeLog("SETTING a slot past the end of the class's table was called "
                 "on %s", s ? s->name : "an object not ours");
    }
    return 0;
}

/* Where the row that opens our page sits follows the control named for it. */
static void followRowPosition(const OurSetting* s) {
    if (global_rowPositionSetting[0]
        && _stricmp(s->name, global_rowPositionSetting) == 0)
        global_menuRowPosition = (int)s->value;
}

/*
    One setting a preset names, given value. A control takes it as if it had
    been chosen on its row, and is drawn again; a settings file key no control
    reads is written as it is.

    A locked control that put a value aside takes the preset's value as the
    value it goes back to when it unlocks, where the value put aside belongs
    to the lock or the control has a default. Where the value put aside does
    not belong to the lock, or the settings file does not hold the default, as
    when it could not be written when the control locked, the default is also
    written to the settings file and noted, as a lock leaves them, and the
    unlock then gives the preset's value back. Where the default cannot be
    written to the settings file, the preset's value is not kept for the
    unlock, which takes what the settings file holds. Any other locked
    control, one that put nothing aside or one with no default whose value
    put aside does not belong to the lock, takes the value as an unlocked
    control does. A keybind whose key cannot be set (keyShownOnly) is left as
    it is.
*/
static void presetSet(const OurSetting* preset, const char* name,
                      const char* value) {
    char whole[MODPAGE_NAME * 2], section[MODPAGE_NAME];
    copyBounded(whole, sizeof(whole), name, "setting name in a preset");
    const char* key = splitSettingName(whole, 1, preset, section,
                                       sizeof(section), "section in a preset");
    OurSetting* t = settingNamed(preset->storePath, section, key);
    if (!t) {
        keepText(preset->storePath, section, key, value);
        return;
    }
    if (!holdsValue(t) || keyShownOnly(t)) return;
    if (t->locked != LOCKED_BY_NOTHING && global_settingsCachePath[0]) {
        PutAside aside;
        readPutAside(t, t->storeKey, t->fallbackText, &aside);
        if (aside.stored[0] && (aside.belongs || t->fallbackText[0])) {
            if (!keepText(global_settingsCachePath, aside.names.section,
                          aside.names.storedKey, value)
                || (aside.belongs
                    && (!t->fallbackText[0]
                        || presetValuesMatch(aside.held, t->fallbackText))))
                return;
            noteLeftBehind(aside.entry, 0);
            if (writeLockedDefault(t, t->storeKey, t->fallbackText, &aside)) {
                takeValue(t, t->fallbackText, 0);
                composeList(listOf(t));
                t->rangeMoved = 1;   /* its row is drawn again */
            }
            return;
        }
    }
    takeValue(t, value, 1);
    t->touched = 1;
    keepText(t->storePath, t->storeSection, t->storeKey, value);
    composeList(listOf(t));
    followRowPosition(t);
    t->rangeMoved = 1;   /* its row is drawn again */
}

/* Every setting any preset of s names, once each, in the order first named. */
static int presetNames(const OurSetting* s, const char** out, int room) {
    const ModPageItem* item = &global_modItems[s->modItem];
    int count = 0;
    for (int p = 0; p < s->optionCount - 1; ++p) {
        const Preset* preset = &global_preset[item->firstPreset + p];
        for (int k = 0; k < preset->count && count < room; ++k) {
            int seen = 0;
            for (int n = 0; n < count && !seen; ++n)
                seen = !_stricmp(out[n], preset->setting[k].name);
            if (!seen) out[count++] = preset->setting[k].name;
        }
    }
    return count;
}

/* Where the values a preset puts aside are kept: the settings cache, in the
   mod's own section, under the preset's name. */
static void presetAsideKey(const OurSetting* s, const char* name,
                           CacheNames* names, char* key, size_t room) {
    cacheNames(s, "custom", names);
    if (name) sprintf_s(key, room, "%s.custom.%s", s->name, name);
    else sprintf_s(key, room, "%s.custom", s->name);
}

/* What every setting the presets name holds, put aside before a preset is
   chosen over settings that match none. */
static void presetPutAside(const OurSetting* s) {
    if (!global_settingsCachePath[0]) return;
    const char* name[MAX_PRESET_NAMES];
    const int count = presetNames(s, name, MAX_PRESET_NAMES);
    CacheNames names;
    char key[MODPAGE_NAME * 4];
    for (int n = 0; n < count; ++n) {
        char held[MAX_SETTING_TEXT];
        presetHeld(s, name[n], held, sizeof(held));
        presetAsideKey(s, name[n], &names, key, sizeof(key));
        keepText(global_settingsCachePath, names.section, key, held);
    }
    presetAsideKey(s, 0, &names, key, sizeof(key));
    keepText(global_settingsCachePath, names.section, key, "1");
}

/* Nothing put aside: a preset chosen from Off leaves Custom nothing to put
   back. */
static void presetForgetAside(const OurSetting* s) {
    if (!global_settingsCachePath[0]) return;
    CacheNames names;
    char key[MODPAGE_NAME * 4];
    presetAsideKey(s, 0, &names, key, sizeof(key));
    keepText(global_settingsCachePath, names.section, key, "");
}

/* The values put aside, put back. Nothing where none were. */
static void presetPutBack(const OurSetting* s) {
    if (!global_settingsCachePath[0]) return;
    CacheNames names;
    char key[MODPAGE_NAME * 4], held[MAX_SETTING_TEXT];
    presetAsideKey(s, 0, &names, key, sizeof(key));
    storedText(global_settingsCachePath, names.section, key, "", held,
               sizeof(held));
    if (!held[0]) return;
    const char* name[MAX_PRESET_NAMES];
    const int count = presetNames(s, name, MAX_PRESET_NAMES);
    for (int n = 0; n < count; ++n) {
        presetAsideKey(s, name[n], &names, key, sizeof(key));
        storedText(global_settingsCachePath, names.section, key, "", held,
                   sizeof(held));
        if (held[0]) presetSet(s, name[n], held);
    }
}

/*
    A preset's option chosen on its row: a preset's settings are set, the
    values put aside first where the settings matched no preset; the last
    option puts those values back. Everything that follows from the changes
    is settled as after any change. Answers whether anything was chosen.
*/
static uint64_t presetChoose(OurSetting* s, int now) {
    presetFollow(s);
    const int presets = s->optionCount - 1;
    if (now < 0) now = 0;
    if (now > presets) now = presets;
    if (now == s->index) return 0;
    if (now < presets) {
        if (s->index == presets) {
            if (presetOffHolds(s)) presetForgetAside(s);
            else presetPutAside(s);
        }
        const Preset* preset = &global_preset[global_modItems[s->modItem]
                                                  .firstPreset + now];
        for (int k = 0; k < preset->count; ++k)
            presetSet(s, preset->setting[k].name, preset->setting[k].value);
        s->presetChosen = now;
    } else {
        presetPutBack(s);
        s->presetChosen = -1;
    }
    s->touched = 1;
    s->presetSeen = -1;
    settleAfterChange(s);
    presetFollow(s);
    return 1;
}

/*
    SetItemValue(int index, const BoxedValue* value, ...). Answers whether the
    value moved. The engine calls this once a frame for as long as a slider is
    being dragged.

    A selector is given the position chosen. A slider's number arrives boxed
    in the third argument. Nothing is written unless the value moved.
*/
static uint64_t ourSetItemValue(uint64_t source, uint64_t value, uint64_t a2,
                                uint64_t) {
    OurSetting* s = ourSourceByObject(source);
    if (!s || s->kind == KIND_BUTTON || s->kind == KIND_KEYBIND) return 0;
    refreshExpressions(s);
    /* A control with no options has no position to take, and nothing is
       written. */
    if (s->kind != KIND_SLIDER && s->optionCount <= 0) return 0;
    if (s->kind == KIND_PRESET)
        return presetChoose(s, (int)(int32_t)(uint32_t)value);

    char keeps[MAX_SETTING_TEXT];
    if (s->kind == KIND_SLIDER) {
        float now = s->logarithmic ? (float)positionOfValue(s, s->value)
                                   : s->value;
        if (a2 >= SMALLEST_POINTER) {
            GUARD_TRY {
                const uint32_t type = *(const uint32_t*)(a2 + 0x0C);
                if (type == BOXED_FLOAT)    now = *(const float*)a2;
                else if (type == BOXED_INT) now = (float)*(const int*)a2;
            } GUARD_EXCEPT_DESCRIBING("reading the value a slider was given") { }
        }
        if (s->logarithmic) {
            const int positions = positionsOf(s);
            int position = (int)floor((double)now + 0.5);
            if (position < 0) position = 0;
            if (position > positions - 1) position = positions - 1;
            now = (float)logValueAt(s->least, s->greatest, s->smallestStep,
                                    position);
        } else if (s->step > 0.0f) {
            const float steps = (now - s->least) / s->step;
            now = s->least + s->step * (float)(int)(steps < 0 ? steps - 0.5f
                                                              : steps + 0.5f);
        }
        if (now < s->least)    now = s->least;
        if (now > s->greatest) now = s->greatest;
        if (sameNumber(now, s->value) && s->touched) return 0;
        s->value = now;
        writeNumber(keeps, sizeof(keeps), now);
    } else {
        int now = (int)(int32_t)(uint32_t)value;
        if (now < 0) now = 0;
        if (now >= s->optionCount) now = s->optionCount - 1;
        if (now == s->index && s->touched) return 0;
        s->index = now;
        s->value = (float)now;
        strcpy_s(keeps, sizeof(keeps), s->optionValue[now]);
    }

    s->asFloat = s->value;
    s->touched = 1;
    s->mismatch = 0;
    s->unsaved = 0;
    strcpy_s(s->shown, sizeof(s->shown), keeps);
    keepText(s->storePath, s->storeSection, s->storeKey, keeps);
    composeList(listOf(s));
    followRowPosition(s);
    settleAfterChange(s);
    return 1;
}

static void giveOurSettingsOwnVtable() {
    const GameBuild* g = global_game;
    if (!g) return;
    for (int i = 0; i < global_ourSourceCount; ++i) {
        OurSetting* s = &global_ourSources[i];
        if (!s->object || !s->vtable) continue;

        uint64_t* copy = global_ownVtable[i];
        int slots = 0;
        while (slots < OWN_VTABLE_SLOTS
               && readQword(s->vtable + 8ull * (uint64_t)slots, &copy[slots]))
            ++slots;
        if (slots <= g->valuePointerSlot) {
            writeLog("SETTING \"%s\": only %d vtable slot(s) could be read, so "
                     "the table was not replaced", s->name, slots);
            continue;
        }

        /* Where the class's own table ends. */
        for (int at = s->kind == KIND_SLIDER ? g->sliderTableSlots
                                             : g->selectorTableSlots;
             at < slots; ++at)
            copy[at] = (uint64_t)&ourUnknownSlot;

        copy[SET_ITEM_VALUE_SLOT]   = (uint64_t)&ourSetItemValue;
        copy[FIELD_CONFIG_SLOT]     = (uint64_t)&ourGetFieldConfig;
        copy[OUR_ITEM_DATA_SLOT]    = (uint64_t)&ourGetItemData;
        copy[g->labelSlot]          = (uint64_t)&ourGetLabelTextId;
        copy[g->descriptionSlot]    = (uint64_t)&ourGetDescriptionTextId;
        copy[AT_DEFAULT_SLOT]       = (uint64_t)&ourIsAtDefault;
        copy[g->availabilitySlot]   = (uint64_t)&ourGetAvailability;
        copy[g->hiddenSlot]         = (uint64_t)&ourIsHidden;
        copy[g->dependencyTextSlot] = (uint64_t)&ourGetDependencyDescriptionId;
        copy[g->valueRangeSlot]     = (uint64_t)&ourGetValueRange;
        copy[g->valuePointerSlot]   = s->kind == KIND_SLIDER
            ? (uint64_t)&ourGetValue : (uint64_t)&ourGetValuePtr;

        followRowPosition(s);
        int ok = 0;
        GUARD_TRY {
            *(uint64_t*)s->object = (uint64_t)copy;
            ok = 1;
        } GUARD_EXCEPT_DESCRIBING("giving a setting of ours its own table") {
            ok = 0;
        }
        if (!ok)
            writeLog("SETTING \"%s\" could not be given a table of its own",
                     s->name);
    }
}

/* ------------------------------------------------------------- keybinds */

/*
    A keybind control: a key a mod reads, kept in its settings file as the
    key's Windows virtual-key code in hexadecimal (F9 is 0x78), and set the
    way the game's own Customise Controls page sets one. Its row is the game's
    keybind row rebuilt with one cell (keybindRowDocument).

    The framework plays what the game's page plays on its rows:

      - While the row has focus its cell is hovered (the game's
        Menu2::RemapColumnItems plays the state on the cell and on its frame,
        01C49244), and the mouse cursor shows the keyboard (the menu root's
        mouse-cursor, the file hud/pc-cursor, played keyboard by 01C93978, as
        01C4B49C does for a keyboard column); focus leaving puts the cursor
        back to normal. A locked control's cell is disabled.
      - Choosing the row makes the button send an event to its page through
        02660DA0, which the framework hears there, and the row waits for a key
        (01C49340): the row is secondary_focused, the cell's binding texts show
        the instruction (focused), the page ignores its own input (its +0x920
        bit 3, as the game's page sets it), and sfx-open plays. Every frame
        while it waits, the menu's commands are turned aside to their -vc names
        (0202DC00), as the game's page turns them, so the keyboard does not
        move or close the menu.
      - A key held past KEYBIND_HOLD_SECONDS shows the binding texts' binding
        state, the old key's icon, the arrow and the new key's icon, plays
        sfx-menu-new-button-assign-hold, and fills the bar over
        KEYBIND_FILL_SECONDS, set each frame as the game's page sets it
        (01C47478, 01C47CF8). Let go first, the hold sound is ended,
        sfx-menu-new-button-assign-cancel plays, the bar runs back to empty at
        the game's pace (01C47A38), and the wait goes on. A full bar sets the
        key, written to the settings file, and plays
        sfx-menu-new-button-assign-complete (01C3FDD0).
      - A key the game's own controls bind where the keybind does not allow
        it (keybindBoundInGame) is refused: sfx-menu-error plays as it goes
        down, the game's sound for a choice that cannot be made, and it is
        passed over until it is let go. The menupage's KeybindAllowMenuKeys
        (True unless it says False) allows keys the game binds to its menu
        actions, and KeybindAllowGameplayKeys (False unless it says True)
        keys it binds to any other action.
      - Esc, focus leaving the row, or the row locking ends the wait with
        sfx-back. The binding texts go back to normal, the row to focused (or
        normal), and the page takes its input again.

    Keys that are not taken: the mouse buttons, Esc, the Windows keys, Shift,
    Ctrl and Alt as one key (their left and right keys are taken unless the
    game binds them), and the keys the game's controls bind where the keybind
    does not allow them (by default, those bound outside its menus).

    Every address and offset is the build's own (KeybindPlaces): tlou-i.exe
    1.1.5.0's and tlou-ii.exe 1.6.0's, used only in a build whose keybinds is
    1. Elsewhere a keybind's row is a selector that shows its key's name
    (keyShownOnly), installKeybinds places no hook and keybindTick does
    nothing. Part II differs:
      - It fills no frame copy of the keyboard state; a key is read with
        GetAsyncKeyState, and only while the window in front is the game's.
      - Its send-event command (01FF4D90) does what SEND_EVENT does without
        calling it, so both are hooked.
      - It has no menu-root getter that leaves the root unmarked; the root is
        resolved from its handle (menuRootRecord, menuRootId).
      - A control scheme's keyboard map holds 24 bytes an action (its id, then
        its primary and secondary keys in 64 bits each).
      - The remap widgets the cell document names are under pc/.
*/
struct KeybindPlaces {
    uint32_t stamp;                 /* the build's time stamp */
    uint64_t sendEvent;             /* (component, event, ...) */
    uint64_t sendEventCommand;      /* the send-event command, 0 where it calls sendEvent */
    uint64_t findChild;             /* (widget, name, 0) -> the widget of that name below it */
    uint64_t playLabel;             /* (widget, label, ...) plays an animation label */
    uint64_t pageOf;                /* (widget) -> the page it is on */
    unsigned pageFlags;             /* a page's flags; bit 3: it ignores its input */
    uint64_t inputOf;               /* (component) -> the input its page reads */
    uint64_t turnCommand;           /* (input, command, as), for the next frame */
    uint64_t asProgressBar;         /* (widget) -> it as an IProgressBar, or 0 */
    uint64_t keyIconText;           /* writes a key's icon as text into a builder */
    uint64_t iconBuilderType;       /* that builder's class */
    uint64_t keyboardState;         /* the frame's copy of GetKeyboardState; 0 for none */
    uint64_t menuObject;            /* () -> the object the menu root is; 0 for none */
    uint64_t menuRootRecord;        /* where menuObject is 0: the root's heap record */
    uint64_t menuRootId;            /* and its id; the object is valid where its id matches */
    uint64_t asMenuRoot;            /* (object) -> it as the menu root, or 0 */
    unsigned menuRootChanged;       /* byte the game's menus set after a change */
    uint64_t playCursor;            /* (menu root, label) plays the mouse cursor's state */
    uint64_t sfxSpawnInfo;          /* (info, sound, 0, &zero) a sound's spawn info */
    uint64_t spawnProcess;          /* Process::SpawnProcess(info, 0) */
    uint64_t killTree;              /* Process::KillTree(process) */
    uint64_t processManager;        /* -> the process manager */
    unsigned processesKillable;     /* its byte: processes may be killed now */
    unsigned progressShowSlot;      /* (bar, shown) */
    unsigned progressSetSlot;       /* (bar, fraction, seconds, curve, 0) */
    uint64_t progressEase;          /* (bar, fraction, per second, curve, least seconds) */
    uint64_t controlSchemes;        /* -> the game's control schemes */
    uint64_t controlSchemeInUse;    /* the index of the scheme in use */
    uint64_t controlSchemeCount;    /* how many schemes there are */
    unsigned controlSchemeBytes;
    unsigned schemeKeyboardMap;     /* a scheme's keyboard bindings: the entries, */
    unsigned schemeMapCount;        /* and how many, after them */
    unsigned schemeEntryBytes;      /* an entry: id, primary key, secondary key */
    unsigned widgetSetPropertySlot;
    int      remapWidgetsUnderPc;   /* the cell document's remap widgets are under pc/ */
};

static const KeybindPlaces global_partOneKeybind = {
    0x685BEBE1u,
    0x02660DA0ull, 0,
    0x025482C8ull, 0x0254AD24ull, 0x0254A274ull, 0x920,
    0x02662094ull, 0x0202DC00ull, 0x025FEB64ull,
    0x020E0EE4ull, 0x02C79208ull,
    0x07FE7E00ull,
    0x023712A4ull, 0, 0,
    0x01C8A3B8ull, 0x7D0, 0x01C93978ull,
    0x024FEBC4ull, 0x01FB367Cull, 0x01EF9270ull, 0x06433198ull, 0x60,
    0x318, 0x3F8, 0x025FF9B8ull,
    0x080DE940ull, 0x080DE948ull, 0x080DE94Cull, 5000, 0xD10, 0x10, 16,
    0x380,
    0
};

static const KeybindPlaces global_partTwoKeybind = {
    0x687DF976u,
    0x01FF5340ull, 0x01FF4D90ull,
    0x01E51810ull, 0x01E55680ull, 0x01FF6E10ull, 0x8A0,
    0x01FF6BA0ull, 0x015920C0ull, 0x01F5D870ull,
    0x016C37F0ull, 0x02AFB4E8ull,
    0,
    0, 0x092D89B8ull, 0x092D89C0ull,
    0x00FEABC0ull, 0x750, 0x00FF5A00ull,
    0x01DD72C0ull, 0x014CD790ull, 0x013414C0ull, 0x04250C48ull, 0x40,
    0x2E8, 0x3C8, 0x01F5E860ull,
    0x08F1EF30ull, 0x08F1EF38ull, 0x08F1EF3Cull, 0x2378, 0x13C8, 0x10, 24,
    0x350,
    1
};

/* The places of the build the framework runs in; Part I's where it has
   none, which only a build whose keybinds is 0 can be, and such a build
   never uses them. */
static const KeybindPlaces* keybindPlaces() {
    if (global_game && global_game->stamp == global_partTwoKeybind.stamp)
        return &global_partTwoKeybind;
    return &global_partOneKeybind;
}

/* The cell document's remap widgets are under pc/ in this build. */
static int keybindRemapWidgetsUnderPc() {
    return keybindPlaces()->remapWidgetsUnderPc;
}

#define SEND_EVENT_RVA           (keybindPlaces()->sendEvent)
#define FIND_CHILD_RVA           (keybindPlaces()->findChild)
#define PLAY_LABEL_RVA           (keybindPlaces()->playLabel)
#define PAGE_OF_RVA              (keybindPlaces()->pageOf)
#define PAGE_FLAGS               (keybindPlaces()->pageFlags)
#define PAGE_IGNORES_INPUT       0x08
#define INPUT_OF_RVA             (keybindPlaces()->inputOf)
#define TURN_COMMAND_RVA         (keybindPlaces()->turnCommand)
#define AS_PROGRESS_BAR_RVA      (keybindPlaces()->asProgressBar)
#define KEY_ICON_TEXT_RVA        (keybindPlaces()->keyIconText)
#define ICON_BUILDER_TYPE_RVA    (keybindPlaces()->iconBuilderType)
#define AS_MENU_ROOT_RVA         (keybindPlaces()->asMenuRoot)
#define MENU_ROOT_CHANGED        (keybindPlaces()->menuRootChanged)
#define PLAY_CURSOR_RVA          (keybindPlaces()->playCursor)
#define SFX_SPAWN_INFO_RVA       (keybindPlaces()->sfxSpawnInfo)
#define SPAWN_PROCESS_RVA        (keybindPlaces()->spawnProcess)
#define KILL_TREE_RVA            (keybindPlaces()->killTree)
#define PROCESS_MANAGER_RVA      (keybindPlaces()->processManager)
#define PROCESSES_KILLABLE       (keybindPlaces()->processesKillable)
#define SFX_SPAWN_INFO_BYTES     0x400  /* Part I's info is 0x320, Part II's at most 0x320 */
#define PROGRESS_SHOW_SLOT       (keybindPlaces()->progressShowSlot)
#define PROGRESS_SET_SLOT        (keybindPlaces()->progressSetSlot)
#define PROGRESS_EASE_RVA        (keybindPlaces()->progressEase)
#define PROGRESS_CURVE           0x0C           /* the curve the game's page fills its bar by */
#define CONTROL_SCHEMES_RVA      (keybindPlaces()->controlSchemes)
#define CONTROL_SCHEME_IN_USE_RVA (keybindPlaces()->controlSchemeInUse)
#define CONTROL_SCHEME_COUNT_RVA (keybindPlaces()->controlSchemeCount)
#define CONTROL_SCHEME_BYTES     (keybindPlaces()->controlSchemeBytes)
#define SCHEME_KEYBOARD_MAP      (keybindPlaces()->schemeKeyboardMap)
#define SCHEME_MAP_COUNT         (keybindPlaces()->schemeMapCount)
#define SCHEME_MAP_MOST          4096
#define WIDGET_SET_PROPERTY_SLOT (keybindPlaces()->widgetSetPropertySlot)
#define BOXED_BOOL_TYPE          1
#define KEYBIND_HOLD_SECONDS     0.3
#define KEYBIND_FILL_SECONDS     1.5
#define KEYBIND_ESCAPE           0x1B

typedef uint64_t (*FindChildFn)(uint64_t, const char*, int);
typedef void (*PlayLabelFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                            uint64_t, float, float);
typedef uint64_t (*PageOfFn)(uint64_t);
typedef uint64_t (*InputOfFn)(uint64_t);
typedef void (*TurnCommandFn)(uint64_t, uint64_t, uint64_t);
typedef uint64_t (*AsProgressBarFn)(uint64_t);
typedef void (*ProgressShowFn)(uint64_t, uint8_t);
/* The fraction and the seconds to take over it are floats (xmm1, xmm2), the
   curve a byte (r9b), as 01C47CF8 and 025FF9B8 call it; 0 seconds sets the
   fraction at once. */
typedef void (*ProgressSetFn)(uint64_t, float, float, uint8_t, uint8_t);
typedef void (*ProgressEaseFn)(uint64_t, float, float, uint8_t, float);
typedef void (*SetPropertyFn)(uint64_t, uint64_t, const BoxedValue*);
typedef uint64_t (*MenuObjectFn)(void);
typedef uint64_t (*AsMenuRootFn)(uint64_t);
typedef void (*PlayCursorFn)(uint64_t, uint64_t);
typedef uint64_t (*SfxSpawnInfoFn)(void*, uint64_t, uint64_t, uint64_t*);
typedef uint64_t (*SpawnProcessFn)(void*, uint64_t);
typedef void (*KillTreeFn)(uint64_t);

/* The text builder 020E1020 makes on its stack for 020E0EE4: its class, its
   capacity (256), the length written, and the text. */
struct IconBuilder {
    uint64_t type;
    uint64_t capacity;
    uint64_t length;
    char     text[0x108];
};
typedef void (*KeyIconTextFn)(uint64_t, IconBuilder*, uint64_t);

/* The game's key code for each Windows key, 0 where it has none. */
static const uint8_t global_gameCodeOfKey[256] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    0xAB, 0x8F, 0, 0, 0, 0x99, 0, 0,
    0, 0, 0, 0xA9, 0, 0, 0, 0,
    0, 0, 0, 0x9C, 0, 0, 0, 0,
    0x20, 0xAD, 0xAE, 0xB0, 0xAF, 0xB1, 0xB3, 0xB2,
    0xB4, 0, 0, 0, 0, 0xAC, 0xAA, 0,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0, 0, 0, 0, 0, 0,
    0, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x5A, 0, 0, 0, 0, 0,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8C, 0x8A, 0, 0x8B, 0x8E, 0x8D,
    0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4,
    0xA5, 0xA6, 0xA7, 0xA8, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0x90, 0x91, 0x93, 0x94, 0x96, 0x97, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0x3B, 0x3D, 0x2C, 0x2D, 0x2E, 0x2F,
    0x60, 0xB7, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0x5B, 0x5C, 0x5D, 0x27, 0xB8,
    0, 0, 0xB9, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
};

/* A Windows key, by the name on it, for a key the game has no icon for. */
static const char* keyNameOf(int key, char* out, size_t size) {
    static const struct { int key; const char* name; } named[] = {
        { 0x08, "Backspace" }, { 0x09, "Tab" }, { 0x0D, "Enter" },
        { 0x13, "Pause" }, { 0x14, "Caps Lock" }, { 0x20, "Space" },
        { 0x21, "Page Up" }, { 0x22, "Page Down" }, { 0x23, "End" },
        { 0x24, "Home" }, { 0x25, "Left" }, { 0x26, "Up" }, { 0x27, "Right" },
        { 0x28, "Down" }, { 0x2C, "Print Screen" }, { 0x2D, "Insert" },
        { 0x2E, "Delete" }, { 0x5D, "Menu" }, { 0x6A, "Num *" },
        { 0x6B, "Num +" }, { 0x6D, "Num -" }, { 0x6E, "Num ." },
        { 0x6F, "Num /" }, { 0x90, "Num Lock" }, { 0x91, "Scroll Lock" },
        { 0xA0, "Left Shift" }, { 0xA1, "Right Shift" }, { 0xA2, "Left Ctrl" },
        { 0xA3, "Right Ctrl" }, { 0xA4, "Left Alt" }, { 0xA5, "Right Alt" },
        { 0xBA, ";" }, { 0xBB, "=" }, { 0xBC, "," }, { 0xBD, "-" },
        { 0xBE, "." }, { 0xBF, "/" }, { 0xC0, "`" }, { 0xDB, "[" },
        { 0xDC, "\\" }, { 0xDD, "]" }, { 0xDE, "'" },
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); ++i)
        if (named[i].key == key) {
            strncpy_s(out, size, named[i].name, _TRUNCATE);
            return out;
        }
    if ((key >= 0x30 && key <= 0x39) || (key >= 0x41 && key <= 0x5A))
        sprintf_s(out, size, "%c", key);
    else if (key >= 0x60 && key <= 0x69)
        sprintf_s(out, size, "Num %d", key - 0x60);
    else if (key >= 0x70 && key <= 0x87)
        sprintf_s(out, size, "F%d", key - 0x6F);
    else
        sprintf_s(out, size, "Key %02X", key);
    return out;
}

/* Whether a key can be taken as a keybind. */
static int keybindTakes(int key) {
    if (key <= 0x06 || key == KEYBIND_ESCAPE || key >= 0xFF) return 0;
    if (key >= 0x10 && key <= 0x12) return 0;   /* Shift, Ctrl, Alt as one */
    if (key == 0x5B || key == 0x5C) return 0;   /* the Windows keys */
    return 1;
}

/*
    One wait for a key at a time: the keybind waiting (its place in
    global_ourSources, -1 for none), the key held and since when, whether the
    progress is shown, the hold sound's handle, the handle of the page told to
    ignore its input, and the keys that were down when the wait began, which
    are not taken until they are let go.
*/
struct KeybindWait {
    int       setting;
    int       holding;
    ULONGLONG heldSince;
    int       progressShown;
    uint64_t  holdSoundRecord;
    uint32_t  holdSoundId;
    uint64_t  pageRecord;
    uint32_t  pageId;
    uint8_t   ignored[32];
};

static int           global_keybindsKnown = 0;   /* the hook on a row's event is in */
static volatile LONG global_keybindAsked = -1;   /* a keybind chosen on its row */
static KeybindWait   global_keybindWait = { -1, 0, 0, 0, 0, 0, 0, 0, { 0 } };
static int           global_keybindCursor = 0;   /* the cursor shows the keyboard for us */
static char          global_keyIconPath[256][96];  /* each key's icon, kept alive */
static uint8_t       global_keyIconAsked[256];
static char          global_keyNameText[256][24];  /* each key's name, kept alive */

static int keyHeldNow(int key) {
    unsigned char state = 0;
    if (key <= 0 || key > 0xFF) return 0;
    if (!keybindPlaces()->keyboardState) {
        HWND front = GetForegroundWindow();
        DWORD process = 0;
        if (front) GetWindowThreadProcessId(front, &process);
        return process == GetCurrentProcessId()
            && (GetAsyncKeyState(key) & 0x8000) != 0;
    }
    GUARD_TRY {
        state = *(const volatile unsigned char*)(global_moduleBase
                                                 + keybindPlaces()->keyboardState
                                                 + (uint64_t)key);
    } GUARD_EXCEPT { state = 0; }
    return (state & 0x80) != 0;
}

/* The widget of this name below widget, or 0. */
static uint64_t keybindPart(uint64_t widget, const char* name) {
    if (!widget) return 0;
    uint64_t part = 0;
    GUARD_TRY {
        part = ((FindChildFn)(global_moduleBase + FIND_CHILD_RVA))(widget, name, 0);
    } GUARD_EXCEPT_DESCRIBING("finding a part of a keybind's row") { part = 0; }
    return part;
}

/* Plays an animation label of the widget file a part was made from. */
static void keybindPlay(uint64_t part, const char* label) {
    if (!part) return;
    GUARD_TRY {
        ((PlayLabelFn)(global_moduleBase + PLAY_LABEL_RVA))(
            part, hashText(label), 0, 0, 0, 0, -1.0f, 1.0f);
    } GUARD_EXCEPT_DESCRIBING("playing a state of a keybind's row") { }
}

/* Sets a part's property through its own setter. */
static void keybindSet(uint64_t part, const char* name, const BoxedValue* value) {
    if (!part) return;
    const uint64_t setter = widgetMethod(part, WIDGET_SET_PROPERTY_SLOT);
    if (!setter) return;
    GUARD_TRY { ((SetPropertyFn)setter)(part, hashText(name), value); }
    GUARD_EXCEPT_DESCRIBING("setting a part of a keybind's cell") { }
}

static void keybindSetText(uint64_t part, const char* name, const char* text) {
    BoxedValue value;
    boxString(&value, text);
    keybindSet(part, name, &value);
}

static void keybindSetShown(uint64_t part, int shown) {
    BoxedValue value;
    value.payload = (const void*)(uintptr_t)(shown ? 1 : 0);
    value.spare = 0;
    value.type = BOXED_BOOL_TYPE;
    keybindSet(part, "visible", &value);
}

/* The object the menu root is, and it as the menu root, or 0. */
static uint64_t menuObject(void) {
    const KeybindPlaces* k = keybindPlaces();
    uint64_t object = 0;
    if (!k->menuObject) {
        uint64_t record = 0;
        uint32_t id = 0;
        if (!readQword(global_moduleBase + k->menuRootRecord, &record)
            || !readDword(global_moduleBase + k->menuRootId, &id))
            return 0;
        return widgetOfHandle(record, id);
    }
    GUARD_TRY { object = ((MenuObjectFn)(global_moduleBase + k->menuObject))(); }
    GUARD_EXCEPT_DESCRIBING("asking for the menu root") { object = 0; }
    return object;
}

/* Tells the menus something changed, as the game's own menus do after
   playing a state. */
static void keybindMarkChanged(void) {
    const uint64_t object = menuObject();
    if (!isPointer(object)) return;
    GUARD_TRY { *(volatile uint8_t*)(object + MENU_ROOT_CHANGED) = 1; }
    GUARD_EXCEPT { }
}

/* The mouse cursor's state: keyboard while a keybind row has focus. */
static void keybindCursor(const char* label) {
    const uint64_t object = menuObject();
    if (!object) return;
    GUARD_TRY {
        const uint64_t root = ((AsMenuRootFn)(global_moduleBase
                                              + AS_MENU_ROOT_RVA))(object);
        if (root)
            ((PlayCursorFn)(global_moduleBase + PLAY_CURSOR_RVA))(root,
                                                                 hashText(label));
    } GUARD_EXCEPT_DESCRIBING("setting the mouse cursor's state") { }
    keybindMarkChanged();
}

/* One of the game's menu sounds, played once. The process is answered, for a
   sound that may need ending. */
static uint64_t keybindSound(const char* name) {
    __declspec(align(16)) uint8_t info[SFX_SPAWN_INFO_BYTES];
    memset(info, 0, sizeof(info));
    uint64_t zero = 0, process = 0;
    GUARD_TRY {
        ((SfxSpawnInfoFn)(global_moduleBase + SFX_SPAWN_INFO_RVA))(
            info, hashText(name), 0, &zero);
        process = ((SpawnProcessFn)(global_moduleBase + SPAWN_PROCESS_RVA))(info, 0);
    } GUARD_EXCEPT_DESCRIBING("playing a menu sound") { process = 0; }
    return process;
}

/* The hold sound, ended where it still plays and processes may be killed. */
static void keybindEndHoldSound(void) {
    KeybindWait* w = &global_keybindWait;
    const uint64_t process = widgetOfHandle(w->holdSoundRecord, w->holdSoundId);
    w->holdSoundRecord = 0;
    w->holdSoundId = 0;
    if (!process) return;
    uint64_t manager = 0;
    if (!readQword(global_moduleBase + PROCESS_MANAGER_RVA, &manager)
        || !isPointer(manager))
        return;
    unsigned char killable = 0;
    readByte(manager + PROCESSES_KILLABLE, &killable);
    if (!killable) return;
    GUARD_TRY { ((KillTreeFn)(global_moduleBase + KILL_TREE_RVA))(process); }
    GUARD_EXCEPT_DESCRIBING("ending the hold sound") { }
}

/* The icon the game gives a key, as the path of its picture; empty where it
   has none. Asked once for each key; the text stays where it is. A letter's
   icon is its lower case code's. */
static const char* keyIconOf(int key) {
    if (key <= 0 || key > 0xFF) return "";
    if (global_keyIconAsked[key]) return global_keyIconPath[key];
    global_keyIconAsked[key] = 1;
    char* path = global_keyIconPath[key];
    path[0] = 0;
    uint8_t code = global_gameCodeOfKey[key];
    if (!code) return path;
    if (code >= 0x41 && code <= 0x5A) code = (uint8_t)(code + 0x20);
    IconBuilder builder;
    memset(&builder, 0, sizeof(builder));
    builder.type = global_moduleBase + ICON_BUILDER_TYPE_RVA;
    builder.capacity = 0x100;
    int ok = 1;
    GUARD_TRY {
        ((KeyIconTextFn)(global_moduleBase + KEY_ICON_TEXT_RVA))(
            (uint64_t)code, &builder, 0);
    } GUARD_EXCEPT_DESCRIBING("asking for a key's icon") { ok = 0; }
    builder.text[sizeof(builder.text) - 1] = 0;
    if (ok && builder.length < 0x100 && strstr(builder.text, ".svg")
        && !strchr(builder.text, '<') && !strchr(builder.text, '|'))
        strncpy_s(path, sizeof(global_keyIconPath[0]), builder.text, _TRUNCATE);
    return path;
}

/* The cell shows key: its icon, or its name where the game has no icon. */
static void keybindShowKey(uint64_t cell, int key) {
    const uint64_t icon = keybindPart(cell, "primaryKeyboardIcon");
    const uint64_t name = keybindPart(cell, "keyName");
    const char* path = keyIconOf(key);
    if (path[0]) {
        keybindSetText(icon, "value", path);
        keybindSetShown(icon, 1);
        keybindSetShown(name, 0);
    } else {
        char* text = global_keyNameText[key & 0xFF];
        keyNameOf(key, text, sizeof(global_keyNameText[0]));
        keybindSetText(name, "value", text);
        keybindSetShown(name, 1);
        keybindSetShown(icon, 0);
    }
}

/* A state of the cell, played on the cell and on its frame, as the game's
   Menu2::RemapColumnItems plays one (01C49244). */
static void keybindCellState(uint64_t cell, const char* label) {
    keybindPlay(cell, label);
    keybindPlay(keybindPart(cell, "columnItemRefPrimKbm"), label);
}

/* A page's flags: it ignores its own input, or takes it again. */
static void keybindPageFlag(uint64_t page, int ignores) {
    if (!isPointer(page)) return;
    GUARD_TRY {
        volatile uint8_t* flags = (volatile uint8_t*)(page + PAGE_FLAGS);
        *flags = ignores ? (uint8_t)(*flags | PAGE_IGNORES_INPUT)
                         : (uint8_t)(*flags & ~PAGE_IGNORES_INPUT);
    } GUARD_EXCEPT_DESCRIBING("setting whether a page takes its input") { }
}

/* The page a row is on is told to ignore its own input while a key is waited
   for, and is held by its handle, so it is told to take it again even where
   the row has gone. */
static void keybindPageIgnoresInput(uint64_t row) {
    KeybindWait* w = &global_keybindWait;
    uint64_t page = 0;
    GUARD_TRY { page = ((PageOfFn)(global_moduleBase + PAGE_OF_RVA))(row); }
    GUARD_EXCEPT_DESCRIBING("asking which page a row is on") { page = 0; }
    w->pageRecord = 0;
    w->pageId = 0;
    if (!isPointer(page)) return;
    GUARD_TRY {
        w->pageRecord = *(volatile uint64_t*)(page + global_game->rowRecord);
        w->pageId = *(volatile uint32_t*)(page + global_game->rowId);
    } GUARD_EXCEPT { w->pageRecord = 0; w->pageId = 0; }
    keybindPageFlag(page, 1);
}

static void keybindPageTakesInput(void) {
    KeybindWait* w = &global_keybindWait;
    keybindPageFlag(widgetOfHandle(w->pageRecord, w->pageId), 0);
    w->pageRecord = 0;
    w->pageId = 0;
}

/* The menu's own commands turned aside for the next frame, as the game's
   page turns them aside while it rebinds. */
static void keybindTurnCommandsAside(uint64_t row) {
    static const char* const command[][2] = {
        { "pause", "pause-vc" }, { "menu-inspect", "menu-inspect-vc" },
        { "menu-back", "menu-back-vc" }, { "menu-up", "menu-up-vc" },
        { "menu-left", "menu-left-vc" }, { "menu-right", "menu-right-vc" },
        { "menu-down", "menu-down-vc" },
    };
    uint64_t input = 0;
    GUARD_TRY { input = ((InputOfFn)(global_moduleBase + INPUT_OF_RVA))(row); }
    GUARD_EXCEPT_DESCRIBING("asking which input a page reads") { input = 0; }
    if (!isPointer(input)) return;
    for (size_t i = 0; i < sizeof(command) / sizeof(command[0]); ++i) {
        GUARD_TRY {
            ((TurnCommandFn)(global_moduleBase + TURN_COMMAND_RVA))(
                input, hashText(command[i][0]), hashText(command[i][1]));
        } GUARD_EXCEPT_DESCRIBING("turning a menu command aside") { }
    }
}

/* The binding texts' bar (an IProgressBar::ProgressBar, its fill barFull), or
   0. */
static uint64_t keybindBar(uint64_t texts) {
    const uint64_t part = keybindPart(texts, "progressBar");
    if (!part) return 0;
    uint64_t bar = 0;
    GUARD_TRY {
        bar = ((AsProgressBarFn)(global_moduleBase + AS_PROGRESS_BAR_RVA))(part);
    } GUARD_EXCEPT_DESCRIBING("asking for a keybind's bar") { bar = 0; }
    return bar;
}

/* The bar filled to fraction at once, as the game's page fills it each frame
   while a key is held (01C47CF8). */
static void keybindProgress(uint64_t texts, float fraction) {
    const uint64_t bar = keybindBar(texts);
    if (!bar) return;
    uint64_t table = 0, show = 0, set = 0;
    if (!readQword(bar, &table) || !readQword(table + PROGRESS_SHOW_SLOT, &show)
        || !readQword(table + PROGRESS_SET_SLOT, &set))
        return;
    GUARD_TRY {
        ((ProgressShowFn)show)(bar, 1);
        ((ProgressSetFn)set)(bar, fraction, 0.0f, PROGRESS_CURVE, 0);
    } GUARD_EXCEPT_DESCRIBING("filling a keybind's bar") { }
}

/* The bar run back to empty at a whole bar a second, as the game's page runs
   it back when a key is let go too soon (01C47A38). */
static void keybindProgressEmpties(uint64_t texts) {
    const uint64_t bar = keybindBar(texts);
    if (!bar) return;
    GUARD_TRY {
        ((ProgressEaseFn)(global_moduleBase + PROGRESS_EASE_RVA))(
            bar, 0.0f, 1.0f, PROGRESS_CURVE, 0.0f);
    } GUARD_EXCEPT_DESCRIBING("emptying a keybind's bar") { }
}

/*
    The game's menu actions, the 29 of its control scheme whose names begin
    menu: (test run 10's BINDINGS lines), photo mode's among them; every other
    action is a gameplay action. An action's id is its name's FNV-1a; the ids
    are worked out when first asked.
*/
static const char* const global_menuActionName[] = {
    "menu:action", "menu:back", "menu:camera-in", "menu:camera-lock",
    "menu:down", "menu:inspect", "menu:left", "menu:nav-neg", "menu:nav-pos",
    "menu:next", "menu:pan", "menu:pause", "menu:photo-screenshot",
    "menu:player", "menu:prev", "menu:right", "menu:select", "menu:slot0",
    "menu:slot1", "menu:slot2", "menu:slot3", "menu:slot4", "menu:slot5",
    "menu:slot6", "menu:slot7", "menu:slot8", "menu:thirds", "menu:up",
    "menu:zoom",
};
#define MENU_ACTIONS (sizeof(global_menuActionName) / sizeof(global_menuActionName[0]))

static uint64_t global_menuActionId[MENU_ACTIONS];
static int      global_menuActionIdsMade = 0;

static int keybindMenuAction(uint64_t action) {
    if (!global_menuActionIdsMade) {
        for (size_t i = 0; i < MENU_ACTIONS; ++i)
            global_menuActionId[i] = hashText(global_menuActionName[i]);
        global_menuActionIdsMade = 1;
    }
    for (size_t i = 0; i < MENU_ACTIONS; ++i)
        if (global_menuActionId[i] == action) return 1;
    return 0;
}

/*
    Whether a keybind refuses a key because the game's own controls bind it:
    the keyboard bindings of the control scheme in use. A binding to one of
    the game's menu actions (keybindMenuAction) refuses the key unless the
    keybind allows menu keys (KeybindAllowMenuKeys, True unless the menupage
    says False), and a binding to any other action unless it allows gameplay
    keys (KeybindAllowGameplayKeys, False unless the menupage says True), so a
    mod that needs a key the game uses for something else can say so.

    The schemes are at [CONTROL_SCHEMES_RVA],
    CONTROL_SCHEME_BYTES each, and the one in use is the one the index at
    CONTROL_SCHEME_IN_USE_RVA names, as the game's input (0202EFB8) and its
    button prompts (020E2C94) take it; the Customise Controls page edits a
    copy (edit-schemes) only while it is open. A scheme's keyboard map
    (SCHEME_KEYBOARD_MAP: the entries, and their count SCHEME_MAP_COUNT after
    them) holds sixteen bytes an action: its id, then its primary and
    secondary keys as the game's key codes in 32 bits each, 0 for none
    (020CA004). A letter is kept by its lower case code; its upper case code
    is matched too. Where the scheme cannot be read, nothing is refused.
*/
static int keybindBoundInGame(const OurSetting* s, int key) {
    if (key <= 0 || key > 0xFF || (s->allowMenuKeys && s->allowGameplayKeys))
        return 0;
    uint32_t code = global_gameCodeOfKey[key];
    if (!code) return 0;
    uint32_t other = code;
    if (code >= 0x41 && code <= 0x5A) code += 0x20;
    if (code >= 0x61 && code <= 0x7A) other = code - 0x20;
    uint64_t schemes = 0, entries = 0, count = 0;
    uint32_t inUse = 0, schemeCount = 0;
    if (!readQword(global_moduleBase + CONTROL_SCHEMES_RVA, &schemes)
        || !readDword(global_moduleBase + CONTROL_SCHEME_IN_USE_RVA, &inUse)
        || !readDword(global_moduleBase + CONTROL_SCHEME_COUNT_RVA, &schemeCount)
        || !isPointer(schemes) || inUse >= schemeCount)
        return 0;
    const uint64_t scheme = schemes + (uint64_t)inUse * CONTROL_SCHEME_BYTES;
    if (!readQword(scheme + SCHEME_KEYBOARD_MAP, &entries)
        || !readQword(scheme + SCHEME_KEYBOARD_MAP + SCHEME_MAP_COUNT, &count)
        || !isPointer(entries))
        return 0;
    if (count > SCHEME_MAP_MOST) count = SCHEME_MAP_MOST;
    const uint64_t stride = keybindPlaces()->schemeEntryBytes;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t action = 0, keys = 0, second = 0;
        if (!readQword(entries + stride * i, &action)
            || !readQword(entries + stride * i + 8, &keys))
            return 0;
        if (stride == 24 && !readQword(entries + stride * i + 16, &second))
            return 0;
        if (!action || action == ~0ull) continue;
        const uint32_t primary = (uint32_t)keys;
        const uint32_t secondary = stride == 24 ? (uint32_t)second
                                                : (uint32_t)(keys >> 32);
        if (primary != code && secondary != code && primary != other
            && secondary != other)
            continue;
        if (keybindMenuAction(action) ? !s->allowMenuKeys : !s->allowGameplayKeys)
            return 1;
    }
    return 0;
}

/* The state of a cell: hovered while its row has focus, disabled while its
   control is locked. */
static int keybindCellStateOf(int focus, int locked) {
    return (focus ? 1 : 0) | (locked ? 2 : 0);
}

static const char* const global_keybindCellLabel[4] = {
    "normal", "hovered", "disabled", "hovered_disabled"
};

/* A keybind's cell, filled whole: its key, and its state. */
static void keybindFillCell(OurSetting* s, uint64_t row, int focus) {
    const int key = (int)s->value;
    const int state = keybindCellStateOf(focus, whyLocked(s) != LOCKED_BY_NOTHING);
    /* A row drawn again from a new handle is filled again. */
    const uint64_t handle = s->rowRecord ^ (uint64_t)s->rowId;
    const uint64_t cell = keybindPart(row, "keybindCell");
    if (handle != s->keybindRow) {
        s->keybindRow = handle;
        s->keybindKey = -1;
        s->keybindFocus = -1;
        keybindPlay(keybindPart(cell, "bindingTexts"), "normal");
    }
    if (key != s->keybindKey) {
        keybindShowKey(cell, key);
        s->keybindKey = key;
    }
    if (state != s->keybindFocus) {
        keybindCellState(cell, global_keybindCellLabel[state]);
        s->keybindFocus = state;
        keybindMarkChanged();
    }
}

/* The wait for a key ends. cancelled plays sfx-back, as the game's page does
   when its wait ends without a key. */
static void keybindStopWaiting(OurSetting* s, uint64_t row, int cancelled) {
    KeybindWait* w = &global_keybindWait;
    keybindEndHoldSound();
    w->setting = -1;
    w->holding = 0;
    w->progressShown = 0;
    keybindPageTakesInput();
    if (cancelled) keybindSound("sfx-back");
    if (!row) return;
    const uint64_t cell = keybindPart(row, "keybindCell");
    keybindPlay(keybindPart(cell, "bindingTexts"), "normal");
    keybindPlay(row, rowHasFocus(row) ? "focused" : "normal");
    keybindMarkChanged();
    s->keybindKey = -1;   /* shown again */
    s->keybindFocus = -1;
    keybindFillCell(s, row, rowHasFocus(row));
}

/* The wait for a key begins, on the row of keybind which. */
static void keybindStartWaiting(int which, uint64_t row) {
    KeybindWait* w = &global_keybindWait;
    w->setting = which;
    w->holding = 0;
    w->heldSince = 0;
    w->progressShown = 0;
    w->holdSoundRecord = 0;
    w->holdSoundId = 0;
    memset(w->ignored, 0, sizeof(w->ignored));
    for (int key = 1; key < 0xFF; ++key)
        if (keyHeldNow(key)) w->ignored[key >> 3] |= (uint8_t)(1u << (key & 7));
    const uint64_t cell = keybindPart(row, "keybindCell");
    keybindPlay(keybindPart(cell, "bindingTexts"), "focused");
    keybindPlay(row, "secondary_focused");
    keybindPageIgnoresInput(row);
    keybindSound("sfx-open");
    keybindMarkChanged();
}

/* The key is set: written to the store and taken by the control. */
static void keybindSetKey(OurSetting* s, int key) {
    char text[16];
    sprintf_s(text, "0x%02X", key);
    takeValue(s, text, 0);
    s->touched = 1;
    keepText(s->storePath, s->storeSection, s->storeKey, text);
    settleAfterChange(s);
    refreshControl(s);   /* the title takes or drops its bullet */
}

/* Every frame while a key is waited for. */
static void keybindWaitForKey(OurSetting* s, uint64_t row) {
    KeybindWait* w = &global_keybindWait;
    if (!rowHasFocus(row) || whyLocked(s) != LOCKED_BY_NOTHING) {
        keybindStopWaiting(s, row, 1);
        return;
    }
    keybindTurnCommandsAside(row);
    for (int key = 1; key < 0xFF; ++key)
        if (!keyHeldNow(key)) w->ignored[key >> 3] &= (uint8_t)~(1u << (key & 7));
    const int escapeIgnored = (w->ignored[KEYBIND_ESCAPE >> 3]
                               >> (KEYBIND_ESCAPE & 7)) & 1;
    if (keyHeldNow(KEYBIND_ESCAPE) && !escapeIgnored) {
        keybindStopWaiting(s, row, 1);
        return;
    }
    const ULONGLONG now = GetTickCount64();
    const uint64_t texts = keybindPart(keybindPart(row, "keybindCell"),
                                       "bindingTexts");
    if (w->holding) {
        if (!keyHeldNow(w->holding)) {
            /* Let go too soon: the wait goes on. */
            w->holding = 0;
            if (w->progressShown) {
                w->progressShown = 0;
                keybindEndHoldSound();
                keybindSound("sfx-menu-new-button-assign-cancel");
                keybindProgressEmpties(texts);
                keybindPlay(texts, "focused");
                keybindMarkChanged();
            }
            return;
        }
        const double held = (double)(now - w->heldSince) / 1000.0;
        if (held < KEYBIND_HOLD_SECONDS) return;
        if (!w->progressShown) {
            w->progressShown = 1;
            keybindSetText(keybindPart(texts, "oldBindingIcon"), "value",
                           keyIconOf((int)s->value));
            keybindSetText(keybindPart(texts, "newBindingIcon"), "value",
                           keyIconOf(w->holding));
            keybindPlay(texts, "binding");
            const uint64_t sound = keybindSound("sfx-menu-new-button-assign-hold");
            uint64_t record = 0;
            uint32_t id = 0;
            if (isPointer(sound)
                && readQword(sound + global_game->rowRecord, &record)) {
                GUARD_TRY {
                    id = *(volatile uint32_t*)(sound + global_game->rowId);
                }
                GUARD_EXCEPT { id = 0; }
                w->holdSoundRecord = record;
                w->holdSoundId = id;
            }
            keybindMarkChanged();
        }
        double fraction = (held - KEYBIND_HOLD_SECONDS) / KEYBIND_FILL_SECONDS;
        if (fraction > 1.0) fraction = 1.0;
        keybindProgress(texts, (float)fraction);
        if (fraction >= 1.0) {
            const int key = w->holding;
            keybindSetKey(s, key);
            keybindSound("sfx-menu-new-button-assign-complete");
            keybindStopWaiting(s, row, 0);
        }
        return;
    }
    for (int key = 1; key < 0xFF; ++key) {
        if (!keybindTakes(key) || !keyHeldNow(key)
            || ((w->ignored[key >> 3] >> (key & 7)) & 1))
            continue;
        if (keybindBoundInGame(s, key)) {
            /* Refused once as it goes down, then passed over until let go. */
            w->ignored[key >> 3] |= (uint8_t)(1u << (key & 7));
            keybindSound("sfx-menu-error");
            continue;
        }
        w->holding = key;
        w->heldSince = now;
        break;
    }
}

/* Every frame: each keybind's cell filled, the cursor kept, and the wait for
   a key kept. */
static void keybindTick(void) {
    if (!global_keybindsKnown) return;
    const LONG asked = InterlockedExchange(&global_keybindAsked, -1);
    if (asked >= 0 && asked < global_ourSourceCount
        && global_keybindWait.setting < 0) {
        OurSetting* s = &global_ourSources[asked];
        const uint64_t row = rowNow(s);
        if (s->kind == KIND_KEYBIND && row && whyLocked(s) == LOCKED_BY_NOTHING)
            keybindStartWaiting((int)asked, row);
    }
    int anyFocus = 0;
    for (int i = 0; i < global_ourSourceCount; ++i) {
        OurSetting* s = &global_ourSources[i];
        if (s->kind != KIND_KEYBIND) continue;
        const uint64_t row = rowNow(s);
        if (!row) {
            s->keybindRow = 0;
            if (global_keybindWait.setting == i) keybindStopWaiting(s, 0, 0);
            continue;
        }
        const int focus = rowHasFocus(row);
        anyFocus |= focus;
        if (global_keybindWait.setting == i) keybindWaitForKey(s, row);
        else keybindFillCell(s, row, focus);
    }
    /* The cursor shows the keyboard over a keybind row, as over the game's
       own keyboard columns, and is put back when no keybind row has focus. */
    if (anyFocus && !global_keybindCursor) {
        keybindCursor("keyboard");
        global_keybindCursor = 1;
    } else if (!anyFocus && global_keybindCursor) {
        keybindCursor("normal");
        global_keybindCursor = 0;
    }
}

/*
    02660DA0, an event sent from a component to its page: a keybind's row was
    chosen. A Menu2::Button sends one when menu-select is pressed on it
    (026BAE74, its slot 140): the event of its own instance name (026BB29C),
    and its action, send-event KEYBIND_EVENT, sends that one. Any event from a
    keybind's row counts, whichever comes. Only noted here; keybindTick acts
    on it.
*/
static void sendEventHook(void* registers, void*) {
    const uint64_t component = loaderHookRegister(registers, LOADER_REG_RCX);
    if (!isPointer(component)) return;
    OurSetting* s = ourSourceByObject(sourceOfComponent(
        component, "asking a chosen row for its source"));
    if (!s || s->kind != KIND_KEYBIND) return;
    InterlockedExchange(&global_keybindAsked, (LONG)(s - global_ourSources));
}

/* The hook a keybind's row needs, where a menupage has one, in a build whose
   keybinds is 1. Its address is that build's. */
static void installKeybinds() {
    if (!keybindsCanBeSet()) return;
    int any = 0;
    for (int i = 0; i < global_ourSourceCount && !any; ++i)
        any = global_ourSources[i].kind == KIND_KEYBIND;
    if (!any) return;
    if (!loaderHookAddress("modmenu_send_event", global_moduleBase + SEND_EVENT_RVA,
                           sendEventHook, nullptr)) {
        writeLog("KEYBIND the loader refused the hook on the event a row sends, "
                 "so no keybind can be set");
        return;
    }
    const uint64_t command = keybindPlaces()->sendEventCommand;
    if (command
        && !loaderHookAddress("modmenu_send_event_command",
                              global_moduleBase + command, sendEventHook,
                              nullptr))
        writeLog("KEYBIND the loader refused the hook on the send-event "
                 "command, so a keybind row chosen through it is not seen");
    global_keybindsKnown = 1;
}

/* ----------------------------------------------------------- the entry point */

static int iniNumber(const char* section, const char* key, int fallback) {
    return (int)GetPrivateProfileIntA(section, key, fallback, global_iniPath);
}

static void iniText(const char* key, const char* fallback, char* out,
                    size_t room) {
    GetPrivateProfileStringA("settings", key, fallback, out, (DWORD)room,
                             global_iniPath);
}

/* Everything the framework reads for itself. A page's own values live in the
   settings file that page names, not here. */
static void readSettings() {
    iniText("rowPositionSetting", "menuPosition", global_rowPositionSetting,
            sizeof(global_rowPositionSetting));
    global_menuRowPosition = iniNumber("settings", "rowPosition", -1);
    iniText("lockedTextColour", "t2-red", global_lockedTextColour,
            sizeof(global_lockedTextColour));
    global_lockedTextBreaks = iniNumber("settings", "lockedTextBreaks", 2);
    global_lockedTextBold = iniNumber("settings", "lockedTextBold", 0);

    global_maxFrameworkSources = iniNumber("dataSources", "maxFrameworkSources",
                                           512);
    if (global_maxFrameworkSources < 0) global_maxFrameworkSources = 0;
    global_extraSourceSlots = iniNumber("dataSources", "extraSourceSlots", 512);
    if (global_extraSourceSlots < 0) global_extraSourceSlots = 0;
    if (global_extraSourceSlots > 4096) global_extraSourceSlots = 4096;
}

/*
    The builds the framework has addresses for, by their time stamps.

    A class table's length runs to where the next table begins, the address
    the next class's constructor loads: in Part I the slider's table begins at
    02E83ED8 and the selector's at 02E83FF0, the next at 02E840E8; in
    tlou-ii.exe the selector's at 02F4CB68, the next at 02F4CC68, and the
    slider's at 02F4CD88, the next at 02F4CEB8. Words copied past them are the
    next class's methods and are replaced.
*/
static const GameBuild partOneBuild = {
    0x685BEBE1u, "tlou-i.exe 1.1.5.0",
    { 0x02661C00ull, partOneAsComponentBytes,
      sizeof(partOneAsComponentBytes) },
    { 0x01EF8BCCull, partOneParentWidgetBytes,
      sizeof(partOneParentWidgetBytes) },
    { 0x011B3F10ull, partOneHandleResolveBytes,
      sizeof(partOneHandleResolveBytes) },
    { 0x02549FC0ull, partOneFirstChildWidgetBytes,
      sizeof(partOneFirstChildWidgetBytes) },
    { 0, nullptr, 0 },
    { 0x0266275Cull, partOneHasFocusBytes, sizeof(partOneHasFocusBytes) },
    { 0x02661028ull, partOneComponentEventBytes,
      sizeof(partOneComponentEventBytes) },
    { 0x01C7C268ull, partOneResetPageBytes, sizeof(partOneResetPageBytes), 5 },
    0x0368BBC8ull,
    &partOneTextLookup,
    0x70, 0x98, 0x10,
    0x350, 0x358,
    20, 21, 24, 25, 26, 28, 30,
    35, 31,
    6,
    1
};

static const GameBuild partTwoBuild = {
    0x687DF976u, "tlou-ii.exe 1.6.0",
    { 0x01FF6540ull, partTwoAsComponentBytes,
      sizeof(partTwoAsComponentBytes) },
    { 0x01340BF0ull, partTwoParentWidgetBytes,
      sizeof(partTwoParentWidgetBytes) },
    { 0x003383E0ull, partTwoHandleResolveBytes,
      sizeof(partTwoHandleResolveBytes) },
    { 0x01E544E0ull, partTwoFirstChildWidgetBytes,
      sizeof(partTwoFirstChildWidgetBytes) },
    { 0x01B3E830ull, partTwoFocusHolderBytes, sizeof(partTwoFocusHolderBytes) },
    { 0x01FF7500ull, partTwoHasFocusBytes, sizeof(partTwoHasFocusBytes) },
    { 0x01FF57E0ull, partTwoComponentEventBytes,
      sizeof(partTwoComponentEventBytes) },
    { 0x01E3D7F0ull, partTwoResetPageBytes, sizeof(partTwoResetPageBytes), 5 },
    0x0949FD68ull,
    &partTwoTextLookup,
    0x80, 0x94, 0x0C,
    0x320, 0x328,
    21, 22, 25, 26, 27, 29, 31,
    38, 32,
    0,
    1
};

static const GameBuild partTwoLBuild = {
    0x687DFAA4u, "tlou-ii-l.exe 1.6.0",
    { 0x02025710ull, partTwoLAsComponentBytes,
      sizeof(partTwoLAsComponentBytes) },
    { 0x013549E0ull, partTwoParentWidgetBytes,
      sizeof(partTwoParentWidgetBytes) },
    { 0x002A7D40ull, partTwoHandleResolveBytes,
      sizeof(partTwoHandleResolveBytes) },
    { 0x01E7D200ull, partTwoLFirstChildWidgetBytes,
      sizeof(partTwoLFirstChildWidgetBytes) },
    { 0x01B5FDE0ull, partTwoFocusHolderBytes, sizeof(partTwoFocusHolderBytes) },
    { 0x02026700ull, partTwoLHasFocusBytes, sizeof(partTwoLHasFocusBytes) },
    { 0x02024990ull, partTwoLComponentEventBytes,
      sizeof(partTwoLComponentEventBytes) },
    { 0x01E664C0ull, partTwoResetPageBytes, sizeof(partTwoResetPageBytes), 5 },
    0x094D7D68ull,
    &partTwoLTextLookup,
    0x80, 0x94, 0x0C,
    0x320, 0x328,
    21, 22, 25, 26, 27, 29, 31,
    38, 32,
    0,
    0
};

/* The builds the framework works in. */
static const GameBuild* const knownBuilds[] = {
    &partOneBuild, &partTwoBuild
};

/*
    The builds the framework has a table for but has not been tested in: in
    these it does nothing, with one line. The list ends with nullptr, which
    stays when the list is otherwise empty. partTwoLBuild (tlou-ii-l.exe) is
    kept here, unused, until it has been tested: moving &partTwoLBuild from
    here into knownBuilds is the one change that switches it on.
*/
static const GameBuild* const untestedBuilds[] = {
    &partTwoLBuild, nullptr
};

/* The build whose time stamp is stamp, or nullptr. */
static const GameBuild* gameBuildOf(uint32_t stamp) {
    for (const GameBuild* build : knownBuilds)
        if (build->stamp == stamp) return build;
    return nullptr;
}

/* The untested build whose time stamp is stamp, or nullptr. */
static const GameBuild* untestedBuildOf(uint32_t stamp) {
    for (const GameBuild* const* build = untestedBuilds; *build; ++build)
        if ((*build)->stamp == stamp) return *build;
    return nullptr;
}

/* The date and the time a time stamp names, written into out as
   YYYY-MM-DD HH:MM:SS UTC. The stamp counts seconds from the start of
   1970-01-01, UTC. */
static void writeStampDate(char* out, size_t room, uint32_t stamp) {
    const __time64_t seconds = stamp;
    struct tm when;
    if (_gmtime64_s(&when, &seconds) != 0) {
        formatInto(out, room, "an unknown date");
        return;
    }
    formatInto(out, room, "%04d-%02d-%02d %02d:%02d:%02d UTC",
               when.tm_year + 1900, when.tm_mon + 1, when.tm_mday,
               when.tm_hour, when.tm_min, when.tm_sec);
}

/*
    Everything the framework does, in the order it has to be done in. The build
    is checked first. On a build it has no addresses for or has not been tested
    in, or where the loader cannot say which build the game is (it gives no
    time stamp, or a stamp of 0), no page is read, the settings file is neither
    written nor read, and nothing in the game is hooked, written or called. The
    settings file is written or completed before it is read. The pages are
    read, the controls made from them and the widget files generated before the
    reader and the registrar are hooked.
*/
static void installHooks() {
    const uint32_t stamp = loaderResolved("LoaderGameTimeDateStamp")
                         ? loaderGameTimeDateStamp() : 0;
    if (!stamp) {
        writeLog("HOOK the loader does not give the game's time stamp, so the "
                 "game's build cannot be checked and the framework does "
                 "nothing");
        return;
    }
    const GameBuild* game = gameBuildOf(stamp);
    const GameBuild* untested = game ? nullptr : untestedBuildOf(stamp);
    if (untested) {
        writeLog("HOOK the framework has not been tested with %s yet, so it "
                 "does nothing there", untested->name);
        return;
    }
    if (!game) {
        char built[40], known[160] = "";
        writeStampDate(built, sizeof(built), stamp);
        const size_t count = sizeof(knownBuilds) / sizeof(knownBuilds[0]);
        for (size_t i = 0; i < count; ++i) {
            if (i)
                strcat_s(known, sizeof(known), i + 1 < count ? ", " : " and ");
            strcat_s(known, sizeof(known), knownBuilds[i]->name);
        }
        writeLog("HOOK the game was built on %s, which is not a build the "
                 "framework has addresses for (%s), so the framework does "
                 "nothing", built, known);
        return;
    }
    global_moduleBase = (uint64_t)GetModuleHandleW(nullptr);
    global_textBase = loaderGameTextBase();
    global_textSize = loaderGameTextSize();
    if (!global_textSize) {
        writeLog("HOOK the loader did not supply the .text bounds, so nothing "
                 "can be hooked");
        return;
    }

    global_game = game;
    global_textLookup = game->textLookup;

    noteNewSettingsFile(global_iniPath);
    writeDefaultSettings((HMODULE)&__ImageBase, global_iniPath);
    readSettings();
    checkRowLayout();
    checkFocusCalls();
    openTextStore();
    hookTextLookup();

    global_labelWidgetId = hashText("label0");
    global_textWidgetId  = hashText("text0");
    global_barWidgetId   = hashText("bar");
    global_valueId       = hashText("value");
    global_percentId     = hashText("percent");

    global_parseData = loaderFindFunction("jdom::ParseData", parseDataSignature);
    if (!global_parseData)
        writeLog("HOOK jdom::ParseData was not found, so no menu file of ours "
                 "can be parsed");

    loadMenuFiles();
    loadModPages();
    makeSettingsFromModPages();
    generateMenuPages();
    installKeybinds();

    const uint64_t reader = findWidgetFileReader();
    if (reader) {
        if (!loaderHookAddress("WidgetFile_Read", reader, widgetFileReadHook,
                               nullptr))
            writeLog("HOOK the loader refused the widget file reader, so no "
                     "page of the game's is replaced by one of ours");

        const uint64_t registrar = findWidgetFileRegistrar(reader);
        if (registrar
            && loaderHookAddress("WidgetFile_Register", registrar,
                                 widgetFileRegistrarHook, nullptr))
            global_registerFile = loaderHookTrampoline("WidgetFile_Register");
        else if (registrar)
            writeLog("HOOK the loader refused the widget file registrar, so no "
                     "page of our own is added");
    }

    if (!loaderHookFunction("ScriptManager::ListenerUpdate",
                            listenerUpdateSignature, listenerUpdateHook,
                            nullptr))
        writeLog("HOOK the loader refused ScriptManager::ListenerUpdate, so %s"
                 "the values put aside for a settings file its mod writes "
                 "after the framework starts are kept unless the mod has "
                 "written it by the first time a row is drawn, a setting "
                 "changed, a page reset or a preset chosen",
                 keybindsCanBeSet()
                     ? "no keybind's cell shows its key and no keybind can be "
                       "set, and "
                     : "");

    if (loaderHookFunction(CREATE_DATA_SOURCE, createDataSourceSignature,
                           createDataSourceHook, nullptr))
        global_createDataSource = loaderHookTrampoline(CREATE_DATA_SOURCE);
    else
        writeLog("HOOK the loader refused %s, so none of our settings can be "
                 "made", CREATE_DATA_SOURCE);

    watchResetButton();
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);

    /* Fallback paths for being injected without the loader. Init replaces the
       log and ini paths with the ones the loader supplies. */
    const DWORD length = GetModuleFileNameA(self, global_directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        global_directory[0] = 0;
        return TRUE;
    }
    char* slash = strrchr(global_directory, '\\');
    if (slash) *slash = 0;

    formatInto(global_modsDirectory, sizeof(global_modsDirectory), "%s",
               global_directory);
    slash = strrchr(global_modsDirectory, '\\');
    if (slash) *slash = 0;

    formatInto(global_logPath, sizeof(global_logPath), "%s\\%s.log",
               global_directory, pluginBaseName);
    formatInto(global_iniPath, sizeof(global_iniPath), "%s\\%s.ini",
               global_directory, pluginBaseName);
    return TRUE;
}

/*
    What the framework is holding for one control, for anything that wants to
    ask.

    at      which control, counted from zero, up to what ModMenuControlCount
            answers. The order is the order the controls were made in.
    out     receives one line of plain text naming the control, the settings
            file key it now reads and the section it reads it in, whether that
            key changed the last time its setting name was worked out, what the
            store holds under it now, what the control shows now, what its
            default is, whether the value is one it cannot show, and whether it
            has a row on screen to draw in.
    room    how much room out has. A line too long for it is cut.

    Answers 1 where there is such a control, 0 where there is not.
*/
extern "C" TLOU_PLUGIN_EXPORT int ModMenuDescribeControl(int at, char* out,
                                                         unsigned room) {
    if (!out || !room) return 0;
    out[0] = 0;
    if (at < 0 || at >= global_ourSourceCount) return 0;
    const OurSetting* s = &global_ourSources[at];
    char nowHeld[MAX_SETTING_TEXT];
    storedText(s->storePath, s->storeSection, s->storeKey, "", nowHeld,
               sizeof(nowHeld));
    /* Its limits and steps, or the options it offers now, and whether it is
       hidden. */
    char more[160] = "";
    if (s->kind == KIND_SLIDER && s->logarithmic)
        sprintf_s(more, ", its limits are %g to %g on the logarithmic scale%s",
                  (double)s->least, (double)s->greatest,
                  s->smallestStep > 0.0f ? ", never finer than its smallest step"
                                         : "");
    else if (s->kind == KIND_SLIDER)
        sprintf_s(more, ", its limits are %g to %g in steps of %g",
                  (double)s->least, (double)s->greatest, (double)s->step);
    else if (s->kind == KIND_SELECTOR || s->kind == KIND_TOGGLE)
        sprintf_s(more, ", it offers %d option(s)", s->optionCount);
    else if (s->kind == KIND_PRESET)
        sprintf_s(more, ", it offers %d preset(s), and shows the last option "
                        "while none matches", s->optionCount - 1);
    else if (s->kind == KIND_KEYBIND)
        sprintf_s(more, ", it is a keybind%s",
                  keyShownOnly(s) ? " whose row only shows its key"
                  : global_keybindWait.setting == at ? " waiting for a key"
                  : "");
    if (s->hidden) strcat_s(more, sizeof(more), ", and it is hidden");
    char line[MAX_SETTING_TEXT * 6 + 480];
    sprintf_s(line, "\"%s\" reads %s in [%s], %s; the store held \"%s\", it "
                   "shows \"%s\", its default is \"%s\"%s%s%s%s%s",
              s->name,
              s->storeKey[0] ? s->storeKey : "(no key)",
              s->storeSection[0] ? s->storeSection : "(no section)",
              s->keyExpression[0]
                  ? (s->keyFollowed ? "which it was not reading before"
                                    : "which it was already reading")
                  : "which its page names outright",
              nowHeld, s->shown, s->fallbackText,
              more,
              s->mismatch ? ", and that value is one it cannot show" : "",
              s->hadNoRow ? ", and it was asked to draw itself again with no "
                            "row to draw in" : "",
              rowNow(s) ? "" : ", and it has no row at the moment",
              s->onNoPage ? ", and no page draws its row, so it has no data "
                            "source" : "");
    copyBounded(out, room, line, "description of a control");
    return 1;
}

/* How many controls there are to ask about. */
extern "C" TLOU_PLUGIN_EXPORT int ModMenuControlCount(void) {
    return global_ourSourceCount;
}

/* Counts up every time a control's setting name is worked out again. A change
   means there is something new to ask about. */
extern "C" TLOU_PLUGIN_EXPORT unsigned ModMenuControlChanges(void) {
    return (unsigned)global_controlChanges;
}

/* 1 where the framework's store of texts holds a text for id, which the
   game's text lookup is then answered with, and 0 where it does not. */
extern "C" TLOU_PLUGIN_EXPORT int ModMenuOwnsText(uint64_t id) {
    return textOfId(id) != nullptr;
}

/* Counts up every time the game's text lookup is answered with a text of the
   framework's. */
extern "C" TLOU_PLUGIN_EXPORT unsigned ModMenuTextAnswers(void) {
    return (unsigned)global_textAnswers;
}

extern "C" TLOU_PLUGIN_EXPORT const char* PluginName(void)    { return "Mod Menu Framework"; }
extern "C" TLOU_PLUGIN_EXPORT const char* PluginVersion(void) { return "v1.2.0"; }
extern "C" TLOU_PLUGIN_EXPORT const char* PluginAuthor(void)  { return "mwith77"; }

/* Answers 1 on every game. installHooks checks the build, and on another one
   logs it and does nothing. */
extern "C" TLOU_PLUGIN_EXPORT int SupportsGame(LoaderHandle) { return 1; }

extern "C" TLOU_PLUGIN_EXPORT int Init(LoaderHandle loader) {
    loaderBind(loader);
    /* Before anything else: reports a bounded call handed too much. */
    loaderWatchBadArguments(PluginName());
    const char* config = loaderConfigFolder();
    if (config && *config)
        formatInto(global_logPath, sizeof(global_logPath), "%s\\%s.log", config,
                   pluginBaseName);
    if (config && *config)
        formatInto(global_iniPath, sizeof(global_iniPath), "%s\\%s.ini", config,
                   pluginBaseName);
    buildSettingsCachePath();

    global_starting = 1;
    installHooks();
    global_starting = 0;
    return 1;
}
