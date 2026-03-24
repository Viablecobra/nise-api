#pragma once


// ============================================================
//  library includes
// ============================================================
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <functional>

// ============================================================
//  GamePwange helper libraries (compiled into fakelib)
// ============================================================
extern "C" {
#include "mem.h"        // writeMemory / readMemory primitives
#include "proc.h"       // proc_map type and /proc/self/maps helpers
#include "nop.h"        // NOP-patch utilities
#include "armhook.h"    // ARM/ARM64 branch-hook primitives
#include "inlinehook.h" // hook_handle type and inline-hook 
#include "memscan.h"    // sigscan_handle type and signature-scan 
}

// ============================================================
//  FMOD forward declarations
// ============================================================
namespace FMOD {
    class System;
    class Sound;
    class Channel;
    class ChannelControl;
    class ChannelGroup;
}
struct FMOD_CREATESOUNDEXINFO;

// ============================================================
//  Globally accessible Minecraft library base address
//  Resolved by HookManager::getMinecraftBaseAddress().
// ============================================================
extern uintptr_t mclib_baseaddr;


// ============================================================
//  HookPriority
// ============================================================

/**
 * @brief Priority level for hook slot ownership.
 *
 * When two callers try to hook the same address or name, the one with the
 * higher priority wins. A lower-priority request is rejected by
 * HookManager::canHookWithPriority() if a higher-priority hook already owns
 * the slot.
 */
enum class HookPriority {
    LOW    = 0, ///< Background / optional hooks; can be displaced by MEDIUM or HIGH.
    MEDIUM = 1, ///< Default priority for most hooks.
    HIGH   = 2  ///< Critical hooks; cannot be displaced by lower-priority callers.
};

// ============================================================
//  HookEntry  (public struct used by HookManager internals,
//              exposed so consumers can inspect hook state)
// ============================================================

/**
 * @brief Describes a single registered inline hook.
 */
struct HookEntry {
    std::string  name;         ///< Unique name assigned by the caller.
    void*        targetAddr;   ///< Address of the original function that was hooked.
    void*        hookFunc;     ///< Address of the replacement function.
    void**       originalFunc; ///< Points to the trampoline for calling the original.
    HookPriority priority;     ///< Priority at which this hook was installed.
    bool         enabled;      ///< Whether the hook is currently active.
    hook_handle* hookHandle;   ///< Opaque handle returned by the inline-hook engine.
};


// ============================================================
//  SystemUtils
// ============================================================

/** Opaque handle type for a loaded shared library. */
typedef void* LibraryHandle;

/**
 * @brief Thread-safe singleton that manages loading, tracking, and unloading
 *        of native shared libraries (.so files) via dlopen / dlsym / dlclose.
 *
 * Libraries are registered under a caller-supplied string ID so they can be
 * retrieved later by name rather than by raw handle.  The same library is
 * never loaded twice under the same ID.
 *
 * Obtain the instance with getInstance(); release everything with
 * destroyInstance() when the host process is shutting down.
 */
class SystemUtils {
public:

    // ----------------------------------------------------------
    //  LoadFlags – passed as the `flags` parameter to loadLibrary()
    // ----------------------------------------------------------

    /**
     * @brief Mirrors the RTLD_* flags from <dlfcn.h> with portable names.
     *
     * These values can be OR-combined just like the underlying RTLD constants.
     */
    enum LoadFlags {
        LAZY      = 0x001, ///< Resolve symbols lazily on first use (RTLD_LAZY). Default.
        NOW       = 0x002, ///< Resolve all symbols immediately on load (RTLD_NOW).
        GLOBAL    = 0x100, ///< Export the library's symbols into the global namespace (RTLD_GLOBAL).
        LOCAL     = 0x000, ///< Keep symbols private to this library (RTLD_LOCAL). Default.
        NODELETE  = 0x200, ///< Prevent unloading even after dlclose (RTLD_NODELETE).
        NOLOAD    = 0x400, ///< Do not load; only return a handle if already resident (RTLD_NOLOAD).
        DEEPBIND  = 0x800  ///< Ignore as its not needed in android.
    };

    // ----------------------------------------------------------
    //  LibraryInfo – metadata for a registered library
    // ----------------------------------------------------------

    /**
     * @brief Snapshot of registration metadata for a single managed library.
     */
    struct LibraryInfo {
        LibraryHandle handle; ///< Raw dlopen handle; nullptr if not currently loaded.
        std::string   path;   ///< Filesystem path that was passed to loadLibrary().
        int           flags;  ///< LoadFlags value used when the library was opened.
        bool          loaded; ///< true if the library is currently resident in memory.

        LibraryInfo();
        LibraryInfo(LibraryHandle h, const std::string& p, int f);
    };

    // ----------------------------------------------------------
    //  Singleton lifecycle
    // ----------------------------------------------------------

    /**
     * @brief Returns the process-wide SystemUtils instance, constructing it
     *        on the first call. Thread-safe.
     * @return Non-null pointer to the singleton.
     */
    static SystemUtils* getInstance();

    /**
     * @brief Unloads all managed libraries, clears the registry, and destroys
     *        the singleton instance.
     *
     * After this call, the next getInstance() will construct a fresh instance.
     * Safe to call even if getInstance() was never invoked.
     */
    static void destroyInstance();

    // ----------------------------------------------------------
    //  Library management
    // ----------------------------------------------------------

    /**
     * @brief Loads a shared library and registers it under @p libraryId.
     *
     * Calls dlopen(@p path, translated(@p flags)). If a library with the same
     * @p libraryId is already registered the call is a no-op and returns true.
     *
     * @param libraryId  Unique string key for later retrieval
     *                   (e.g. "libfmod", "libminecraft").
     * @param path       Filesystem path to the .so file (absolute or relative).
     * @param flags      One or more LoadFlags OR-combined. Defaults to LAZY.
     * @return true on success; false if dlopen failed – call getLastError() for details.
     */
    bool loadLibrary(const std::string& libraryId, const std::string& path, int flags = LAZY);

    /**
     * @brief Unloads a managed library and removes it from the registry.
     *
     * Calls dlclose on the stored handle. Has no effect if @p libraryId is not
     * found in the registry.
     *
     * @param libraryId  Key supplied to loadLibrary().
     * @return true if the library was found and dlclose succeeded.
     */
    bool unloadLibrary(const std::string& libraryId);

    /**
     * @brief Checks whether a library is currently loaded in the registry.
     *
     * @param libraryId  Key supplied to loadLibrary().
     * @return true if the library is registered and its handle is non-null.
     */
    bool isLibraryLoaded(const std::string& libraryId) const;

    /**
     * @brief Looks up an exported symbol in a specific registered library.
     *
     * Equivalent to dlsym(handle_of(libraryId), symbolName).
     *
     * @param libraryId   Key of the library to search.
     * @param symbolName  Mangled or unmangled export name.
     * @return Symbol address on success; nullptr if the library or symbol was not found.
     */
    void* getSymbol(const std::string& libraryId, const std::string& symbolName);

    /**
     * @brief Searches all registered libraries for a symbol and returns the
     *        first match.
     *
     * Useful when the containing library is not known in advance.
     *
     * @param symbolName  Mangled or unmangled export name.
     * @return Symbol address, or nullptr if not found in any registered library.
     */
    void* getSymbolAddress(const std::string& symbolName);

    /**
     * @brief Returns a copy of the LibraryInfo record for a registered library.
     *
     * @param libraryId  Key supplied to loadLibrary().
     * @return LibraryInfo for the requested library. If the key is not found,
     *         the returned struct has loaded == false and a null handle.
     */
    LibraryInfo getLibraryInfo(const std::string& libraryId) const;

    /**
     * @brief Returns the IDs of all currently registered libraries.
     *
     * @return Vector of libraryId strings in no guaranteed order.
     */
    std::vector<std::string> getLoadedLibraries() const;

    // ----------------------------------------------------------
    //  Raw dl* wrappers  (static – no registry involvement)
    // ----------------------------------------------------------

    /**
     * @brief Thin static wrapper around ::dlopen.
     *
     * Unlike loadLibrary(), the returned handle is NOT tracked in the registry.
     * Use when you need a short-lived, unregistered handle.
     *
     * @param path   Path to the shared library.
     * @param flags  dlopen flags (RTLD_* or LoadFlags values).
     * @return Opaque library handle, or nullptr on failure.
     */
    static void* dlOpen(const std::string& path, int flags);

    /**
     * @brief Thin static wrapper around ::dlsym.
     *
     * @param handle  Handle obtained from dlOpen() or any dlopen-compatible source.
     * @param symbol  Symbol name to resolve.
     * @return Symbol address, or nullptr if not found.
     */
    static void* dlSym(void* handle, const std::string& symbol);

    /**
     * @brief Thin static wrapper around ::dlclose.
     *
     * @param handle  Handle to close.
     * @return true on success; false if ::dlclose returned an error.
     */
    static bool dlClose(void* handle);

    /**
     * @brief Returns and clears the last error string from the dl* subsystem.
     *
     * Equivalent to ::dlerror(). Call immediately after a failed dlOpen / dlSym
     * / dlClose to retrieve the human-readable message.
     *
     * @return Error string, or an empty string if there is no pending error.
     */
    static std::string dlError();

    // ----------------------------------------------------------
    //  Error helpers
    // ----------------------------------------------------------

    /**
     * @brief Returns the last error string recorded by any SystemUtils operation.
     * @return Human-readable error description, or an empty string if no error.
     */
    std::string getLastError() const;

    /**
     * @brief Clears the stored last-error string.
     */
    void clearError();

private:
    SystemUtils();
    ~SystemUtils();
    SystemUtils(const SystemUtils&)            = delete;
    SystemUtils& operator=(const SystemUtils&) = delete;
};


// ============================================================
//  HookManager
// ============================================================

/**
 * @brief Singleton that provides inline function hooking, NOP patching,
 *        memory read/write, signature scanning, and process-map inspection.
 *
 * All public methods are guarded by an internal mutex and are safe to call
 * from any thread after initialize() returns true.
 *
 * Typical usage:
 * @code
 *   auto& hm = HookManager::getInstance();
 *   hm.initialize();
 *
 *   void* origFn = nullptr;
 *   hm.hookAddr("my_hook", targetAddr, (void*)myHook,
 *               &origFn, 0, HookPriority::MEDIUM);
 *   // ... later ...
 *   hm.unhookAddr("my_hook");
 *   hm.cleanup();
 * @endcode
 */
class HookManager {
public:

    // ----------------------------------------------------------
    //  Singleton lifecycle
    // ----------------------------------------------------------

    /**
     * @brief Returns the process-wide HookManager instance.
     * @return Reference to the singleton.
     */
    static HookManager& getInstance();

    /**
     * @brief One-time initialisation: sets up the inline-hook engine, resolves
     *        the Minecraft base address, and loads the hook blacklist.
     * @return true if all subsystems initialised successfully.
     */
    bool initialize();

    /**
     * @brief Removes every active hook and NOP patch, then frees internal
     *        resources.
     *
     * Call this before the host process exits to avoid dangling trampolines.
     */
    void cleanup();

    /**
     * @brief undocumented; does nothing in Release builds.
     */
    void debugStatus();

    // ----------------------------------------------------------
    //  Core hook API
    // ----------------------------------------------------------

    /**
     * @brief Resolves and caches the load address of libminecraft.so.
     *
     * Called automatically by initialize(). Exposed publicly so the address
     * can be re-queried if the library is reloaded.
     *
     * Sets the global ::mclib_baseaddr on success.
     *
     * @return true if libminecraft.so was found in /proc/self/maps.
     */
    bool getMinecraftBaseAddress();

    /**
     * @brief Installs an inline hook at @p targetAddr, redirecting execution
     *        to @p hookFunc.
     *
     * A trampoline is generated so the original function can still be called
     * through *@p originalFunc. The hook is registered under @p name so it
     * can be removed later with unhookAddr().
     *
     * The call is rejected (returns 0) if:
     *  - @p name or @p targetAddr is blacklisted.
     *  - A higher-priority hook already owns the slot.
     *
     * @param name          Unique identifier for this hook.
     * @param targetAddr    Address of the function to hook.
     * @param hookFunc      Your replacement function – must exactly match the
     *                      target's calling convention and parameter types.
     * @param originalFunc  Out-param written with the trampoline address so you
     *                      can call the original. Pass nullptr if not needed.
     * @param hookType      Hook type constant; pass 0 for the default
     *                      inline hook.
     * @param priority      Priority level. Defaults to HookPriority::MEDIUM.
     * @return Trampoline address on success; 0 on failure.
     */
    uintptr_t hookAddr(const std::string& name,
                       void*              targetAddr,
                       void*              hookFunc,
                       void**             originalFunc,
                       int                hookType,
                       HookPriority       priority = HookPriority::MEDIUM);

    /**
     * @brief Removes a hook installed by hookAddr() and restores the original
     *        bytes at the target address.
     *
     * @param name  The name string passed to hookAddr().
     * @return true if the hook was found and successfully removed.
     */
    bool unhookAddr(const std::string& name);

    /**
     * @brief Overwrites @p size bytes at @p addr with NOP instructions,
     *        saving the original bytes for later restoration.
     *
     * Handles page-permission changes automatically. The patch is registered
     * under @p name.
     *
     * @param name  Unique identifier for this patch (used with restoreNopPatch).
     * @param addr  Start address of the region to NOP out.
     * @param size  Number of bytes to overwrite with NOPs.
     * @return true if the memory was patched successfully.
     */
    bool patchNop(const std::string& name, void* addr, size_t size);

    /**
     * @brief Restores the bytes that were overwritten by patchNop().
     *
     * @param name  The name string passed to patchNop().
     * @return true if the patch was found and the original bytes were restored.
     */
    bool restoreNopPatch(const std::string& name);



    // ----------------------------------------------------------
    //  Priority management
    // ----------------------------------------------------------

    /**
     * @brief Tests whether a new hook with the given priority may claim the
     *        slot identified by @p name.
     *
     * A slot can be claimed if it is free, or if the requesting priority is
     * strictly higher than the existing owner's priority.
     *
     * @param name      Hook name (slot identifier).
     * @param priority  Priority of the incoming hook attempt.
     * @return true if the hook is permitted to proceed.
     */
    bool canHookWithPriority(const std::string& name, HookPriority priority);

    /**
     * @brief Releases the priority claim on a slot without removing the hook.
     *
     * After this call a lower-priority hook may reclaim the same slot.
     *
     * @param name  Hook name whose priority claim should be relinquished.
     */
    void releasePriority(const std::string& name);


    // ----------------------------------------------------------
    //  Memory utilities
    // ----------------------------------------------------------

    /**
     * @brief Writes @p len bytes from @p src to @p dest, temporarily remapping
     *        the destination page as writable if necessary.
     *
     * Safe to use on read-only or execute-only pages.
     *
     * @param dest  Destination address.
     * @param src   Source buffer.
     * @param len   Number of bytes to write.
     * @return true on success.
     */
    bool writeMemory(void* dest, void* src, size_t len);

    /**
     * @brief Reads @p len bytes from @p src into @p dest, remapping the source
     *        page as readable if necessary.
     *
     * Safe to use on execute-only pages.
     *
     * @param dest  Destination buffer.
     * @param src   Source address to read from.
     * @param len   Number of bytes to read.
     * @return true on success.
     */
    bool readMemory(void* dest, void* src, size_t len);

    /**
     * @brief Resolves a multi-level pointer chain.
     *
     * Starting from @p baseAddr, applies each element of @p offsets in turn,
     * dereferencing the resulting pointer at each step.
     *
     * Example – three-level pointer:
     * @code
     *   uintptr_t offs[] = { 0x10, 0x28, 0x08 };
     *   uintptr_t result = hm.getAddress(mclib_baseaddr, offs, 3);
     * @endcode
     *
     * @param baseAddr     Starting absolute address (e.g. mclib_baseaddr + offset).
     * @param offsets      Array of byte offsets applied at each dereference step.
     * @param totalOffset  Number of elements in @p offsets.
     * @return Final resolved address; 0 if any intermediate dereference is null.
     */
    uintptr_t getAddress(uintptr_t baseAddr, uintptr_t offsets[], int totalOffset);

    /**
     * @brief Allocates an anonymous executable memory region as close as
     *        possible to @p hint – intended for near-trampolines.
     *
     * Stays within ±2 GB of @p hint so that relative 26-bit branches (B/BL on
     * AArch64, E9 JMP on x86-64) can reach the allocated stub.
     *
     * @param hint  Address the allocation should be near.
     * @param size  Number of bytes to allocate.
     * @param prot  mmap protection flags (e.g. PROT_READ | PROT_EXEC).
     * @return Pointer to the allocated region; nullptr on failure.
     */
    void* mmapNear(void* hint, size_t size, int prot);

    /**
     * @brief Finds the base address of a named module mapping from
     *        /proc/self/maps, optionally filtered by permission string.
     *
     * @param moduleName   Substring matched against the mapping pathname
     *                     (e.g. "libminecraft.so").
     * @param permissions  Optional permission string to match (e.g. "r-xp").
     *                     Pass "" (default) to accept any permissions.
     * @return Base address of the first matching mapping; nullptr if not found.
     */
    void* getModuleAddr(const std::string& moduleName,
                        const std::string& permissions = "");

    /**
     * @brief Returns the current mmap protection flags for the page containing
     *        @p addr.
     *
     * Parsed from /proc/self/maps.
     *
     * @param addr  Any address inside the page of interest.
     * @return PROT_* bitmask (e.g. PROT_READ | PROT_EXEC); -1 if not mapped.
     */
    int getProtection(uintptr_t addr);

    /**
     * @brief Finds an unmapped virtual-address region near @p target that is
     *        at least @p size bytes in length.
     *
     * Used internally by mmapNear() to locate a suitable free slot.
     *
     * @param target  Address the result should be as close to as possible.
     * @param size    Minimum required size of the free region in bytes.
     * @return Start address of a suitable unmapped region; nullptr if none found.
     */
    void* findUnmapped(void* target, size_t size);

    // ----------------------------------------------------------
    //  Signature scanning
    // ----------------------------------------------------------

    /**
     * @brief Sets up a signature scan using a human-readable IDA-style pattern.
     *
     * Pattern format: space-separated hex bytes with "??" as wildcards.
     * Example: "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0A"
     *
     * The scan searches the memory region(s) mapped for @p libName. Call
     * getSigScanResult() to execute the scan and retrieve the result, then
     * sigScanCleanup() to free the handle regardless of outcome.
     *
     * @param signature  IDA-style pattern string.
     * @param libName    Library whose mapped region to search (e.g. "libminecraft.so").
     * @param flags      gamepwange specific scan flags (0 for defaults).
     * @return Opaque sigscan_handle on success; nullptr if setup failed.
     */
    sigscan_handle* sigScanSetup(const std::string& signature,
                                 const std::string& libName,
                                 int                flags);

    /**
     * @brief Sets up a signature scan using raw byte arrays instead of a
     *        pattern string.
     *
     * @param sigByte  Pattern byte array to search for.
     * @param mask     Mask array of the same length as @p sigByte.
     *                 0xFF = byte must match exactly; 0x00 = wildcard (skip).
     * @param sigSize  Length of both @p sigByte and @p mask in bytes.
     * @param libName  Library whose mapped region to search.
     * @param flags    Engine-specific scan flags (0 for defaults).
     * @return Opaque sigscan_handle on success; nullptr if setup failed.
     */
    sigscan_handle* sigScanSetupRaw(uint8_t*           sigByte,
                                    uint8_t*           mask,
                                    size_t             sigSize,
                                    const std::string& libName,
                                    int                flags);

    /**
     * @brief Frees all resources associated with a scan handle.
     *
     * Must be called after getSigScanResult() to avoid memory leaks, regardless
     * of whether a match was found.
     *
     * @param handle  Handle returned by sigScanSetup() or sigScanSetupRaw().
     */
    void sigScanCleanup(sigscan_handle* handle);

    /**
     * @brief Executes the prepared scan and returns the address of the first match.
     *
     * Blocks the calling thread until the scan completes. Call sigScanCleanup()
     * afterwards to free the handle.
     *
     * @param handle  Handle returned by sigScanSetup() or sigScanSetupRaw().
     * @return Address of the first matching byte sequence; nullptr if not found.
     */
    void* getSigScanResult(sigscan_handle* handle);

    // ----------------------------------------------------------
    //  ARM64 hook helper
    // ----------------------------------------------------------

    /**
     * UNDOCUMETED Read GamePwange Docs
     */
    uintptr_t armHook64(uintptr_t addr, uintptr_t branchAddr, size_t len);

    // ----------------------------------------------------------
    //  Process map inspection
    // ----------------------------------------------------------

    /**
     * @brief Parses /proc/self/maps and returns all entries whose pathname
     *        contains @p module.
     *
     * The returned array is heap-allocated. The caller must delete[] it when
     * done. @p mapCount is set to the number of entries.
     *
     * @param module    Substring matched against mapping pathnames
     *                  (e.g. "libminecraft.so").
     * @param mapCount  Out-param; set to the number of proc_map entries returned.
     * @return Heap-allocated array of proc_map structs; nullptr on error or if
     *         no entries match. Caller is responsible for delete[].
     */
    proc_map* getProcMap(const std::string& module, unsigned int& mapCount);

    /**
     * @brief Returns the number of /proc/self/maps entries whose pathname
     *        contains @p module, without allocating the full array.
     *
     * Cheaper than getProcMap() when only the count is needed.
     *
     * @param module  Substring matched against mapping pathnames.
     * @return Number of matching entries.
     */
    unsigned int getProcMapCount(const std::string& module);

private:
    HookManager()  = default;
    ~HookManager() = default;
    HookManager(const HookManager&)            = delete;
    HookManager& operator=(const HookManager&) = delete;
};


// ============================================================
//  FMODHook
// ============================================================

/**
 * @brief Hooks into the FMOD Core library loaded by Minecraft to intercept
 *        audio stream creation, playback, and channel management.
 *
 * Hooks installed:
 *  - FMOD::System::createStream   – intercepts file-open calls, enabling path overrides
 *  - FMOD::System::playSound      – tracks which channel a sound is playing on
 *  - FMOD::Sound::release         – cleans up tracking state when a sound is freed
 *  - FMOD::ChannelControl::stop   – detects when playback stops
 *  - FMOD::ChannelControl::setPaused – detects pause/resume state changes
 */
class FMODHook {
public:

    /**
     * @brief Runtime state of a tracked FMOD sound stream.
     */
    struct TrackInfo {
        std::string    original_path;  ///< Path as received by createStream (before override).
        std::string    override_path;  ///< Path actually opened after applying any override; empty if no override.
        FMOD::Sound*   sound;          ///< FMOD sound object for this stream.
        FMOD::Channel* channel;        ///< Channel the sound is (or was last) playing on; nullptr if not yet played.
        bool           is_playing;     ///< true if the channel is currently playing (not paused, not stopped).
        bool           is_paused;      ///< true if the channel is currently paused.
        float          volume;         ///< Last-known channel volume in [0.0, 1.0].

        TrackInfo();
    };

    /**
     * @brief Callback fired on the FMOD audio thread whenever the active track
     *        changes (new track starts, or playback stops entirely).
     *
     * Keep the handler short and thread-safe.
     *
     * @param path     Path of the new track (before any override), or empty if
     *                 playback stopped.
     * @param sound    FMOD::Sound* for the new track; nullptr if playback stopped.
     * @param channel  FMOD::Channel* the new track is playing on; nullptr if stopped.
     */
    using TrackChangeCallback = std::function<void(const std::string& path,
                                                   FMOD::Sound*       sound,
                                                   FMOD::Channel*     channel)>;

    // ----------------------------------------------------------
    //  Singleton lifecycle
    // ----------------------------------------------------------

    /**
     * @brief Returns the process-wide FMODHook instance.
     * @return Reference to the singleton.
     */
    static FMODHook& getInstance();


    // ----------------------------------------------------------
    //  Path overrides
    // ----------------------------------------------------------

    /**
     * @brief Redirects an FMOD audio file open from @p original_path to
     *        @p custom_path on disk.
     *
     * Whenever FMOD's createStream hook intercepts a call where the path
     * matches @p original_path, it transparently substitutes @p custom_path
     * so FMOD opens the replacement file instead.
     *
     * @param original_path  Path string as Minecraft passes it to createStream
     *                       (e.g. "assets/music/game/creative.ogg").
     * @param custom_path    Absolute path on the device to serve instead
     *                       (e.g. "/sdcard/MyMod/music/creative.ogg").
     */
    void addPathOverride(const std::string& original_path, const std::string& custom_path);

    /**
     * @brief Removes a single path override registered by addPathOverride().
     *
     * After this call, @p original_path is forwarded to FMOD unmodified.
     * Has no effect if the path was never registered.
     *
     * @param original_path  The original path key to remove.
     */
    void removePathOverride(const std::string& original_path);

    /**
     * @brief Removes all registered path overrides.
     *
     * Equivalent to calling removePathOverride() for every registered entry.
     */
    void clearPathOverrides();

    // ----------------------------------------------------------
    //  Playback control
    // ----------------------------------------------------------

    /**
     * @brief Pauses the channel of the currently tracked active track.
     *
     * Internally calls FMOD::ChannelControl::setPaused(true) via the original
     * (non-hooked) function pointer. Has no effect if no track is playing.
     *
     * @return true if a playing track was found and paused successfully.
     */
    bool pauseCurrentTrack();

    /**
     * @brief Resumes the channel of the currently tracked active track.
     *
     * Calls FMOD::ChannelControl::setPaused(false). Has no effect if no
     * track is paused.
     *
     * @return true if a paused track was found and resumed successfully.
     */
    bool resumeCurrentTrack();

    /**
     * @brief Stops only the currently tracked active track.
     *
     * Calls FMOD::ChannelControl::stop() on the current channel. The track
     * cannot be resumed after this.
     *
     * @return true if an active track was found and stopped successfully.
     */
    bool stopCurrentTrack();

    /**
     * @brief Stops every FMOD channel that is currently playing.
     *
     * More aggressive than stopCurrentTrack(); affects all channels managed by
     * Minecraft's FMOD instance, not just the one tracked internally.
     *
     * @return true if the stop command was dispatched successfully.
     */
    bool stopAll();

    // ----------------------------------------------------------
    //  Track information
    // ----------------------------------------------------------

    /**
     * @brief Returns a read-only pointer to the TrackInfo for the currently
     *        active sound stream.
     *
     * The pointer is valid until the next track change event or until cleanup()
     * is called. Do not store it beyond the scope of the current call.
     *
     * @return Pointer to the current TrackInfo; nullptr if no track is active.
     */
    const TrackInfo* getCurrentTrack() const;

    /**
     * @brief Returns the original (pre-override) path of the current track.
     *
     * @return Original path string; empty string if no track is active.
     */
    std::string getCurrentTrackPath() const;

    /**
     * @brief Returns whether a track is currently in the playing (non-paused,
     *        non-stopped) state.
     *
     * @return true if is_playing is true on the current TrackInfo.
     */
    bool isTrackPlaying() const;

    // ----------------------------------------------------------
    //  Volume
    // ----------------------------------------------------------

    /**
     * @brief Sets the volume on the currently active channel.
     *
     * Calls FMOD::ChannelControl::setVolume() via the original function pointer.
     * Values outside [0.0, 1.0] are clamped by FMOD.
     *
     * @param volume  Desired volume level in [0.0, 1.0].
     * @return true if a current channel was found and the volume was set.
     */
    bool setVolume(float volume);

    /**
     * @brief Returns the last-known volume of the current track.
     *
     * @return Volume in [0.0, 1.0], or -1.0f if no track is active or the
     *         hook is not initialised.
     */
    float getVolume() const;

    // ----------------------------------------------------------
    //  Callbacks
    // ----------------------------------------------------------

    /**
     * @brief Registers a callback invoked whenever the active track changes.
     *
     * The callback fires on the FMOD audio thread when:
     *  - A new stream is opened and begins playing.
     *  - The current channel is stopped or released.
     *
     * Replaces any previously registered callback. The handler must be
     * short and thread-safe.
     *
     * @param callback  Function matching the TrackChangeCallback signature.
     *                  Receives the new track's original path, Sound*, and Channel*,
     *                  or empty/nullptr values when playback stops.
     */
    void setTrackChangeCallback(TrackChangeCallback callback);

    /**
     * @brief Removes the callback registered by setTrackChangeCallback().
     *
     * Safe to call even if no callback was registered.
     */
    void clearTrackChangeCallback();

private:
    FMODHook()  = default;
    ~FMODHook() = default;
    FMODHook(const FMODHook&)            = delete;
    FMODHook& operator=(const FMODHook&) = delete;
};


// ============================================================
//  VirtualAssets
// ============================================================

/**
 * @defgroup VirtualAssets VirtualAssets API
 *
 * @brief Hooks the Android AAssetManager to layer a virtual filesystem
 *        transparently on top of the real APK asset tree.
 *
 * ### How the hook works
 * The NDK's AAssetManager_open (and its internal siblings) is intercepted via
 * HookManager. Every asset path opened by the game passes through the
 * interceptor, which checks the virtual registry before forwarding to the
 * real AAssetManager zip reader.
 *
 * ### Interception order (highest to lowest priority)
 *  1. **Blocked** – path returns failure immediately; the game sees the file
 *     as non-existent even though it is in the APK.
 *  2. **Virtual entry** – data registered by AddFile / AddTextFile / LoadDir /
 *     ReplaceFile is served to the caller instead of the APK data.
 *  3. **Fall-through** – the call is forwarded to the real AAssetManager and
 *     the APK data is returned unmodified.
 *
 * ### Thread safety
 * All functions are thread-safe after init_virtual_assets() returns. Internal
 * state is protected by a mutex; concurrent calls from the render thread,
 * audio thread, or mod worker threads are safe.
 *
 * @{
 */


/**
 * @brief Blocks an asset path so that the game cannot open it.
 *
 * Any AAssetManager_open call for @p path will return failure (null asset),
 * as if the file does not exist in the APK.
 *
 * Common uses:
 *  - Suppressing vanilla assets you are completely replacing.
 *  - Preventing a file from loading that crashes with your mod installed.
 *
 * Blocking has higher priority than virtual entries: even if a virtual entry
 * is registered for the same path, the file appears missing while blocked.
 * Lift the block with VirtualAssets_UnblockFile().
 *
 * @param path  Asset path relative to the APK assets root
 *              (e.g. "textures/terrain/grass.png"). Must not be nullptr.
 */
void VirtualAssets_BlockFile(const char* path);

/**
 * @brief Lifts a block previously applied by VirtualAssets_BlockFile().
 *
 * After this call the path resumes normal interception priority:
 * a virtual entry is served if one exists, otherwise the real APK data is returned.
 *
 * Has no effect if @p path is not currently blocked.
 *
 * @param path  Asset path to unblock; must match the string passed to BlockFile.
 */
void VirtualAssets_UnblockFile(const char* path);

/**
 * @brief Injects a binary blob as a virtual asset at @p path.
 *
 * The @p data buffer is copied into an internal store immediately; the caller
 * may free or reuse the buffer right after this call returns.
 *
 * If a virtual entry already exists at @p path it is replaced atomically.
 * If the path is currently blocked the entry is stored but invisible until
 * VirtualAssets_UnblockFile() is called.
 *
 * @param path  Virtual asset path to register (e.g. "shaders/glsl/terrain.vertex").
 * @param data  Raw bytes to inject. Must not be nullptr.
 * @param size  Number of bytes in @p data.
 */
void VirtualAssets_AddFile(const char* path, const void* data, size_t size);

/**
 * @brief Injects a UTF-8 text string as a virtual asset at @p path.
 *
 * Convenience wrapper around VirtualAssets_AddFile() for plain-text content
 * (JSON, XML, shader source, CSV, etc.). The content is copied internally; the
 * caller retains ownership of the @p content pointer. The null terminator is
 * NOT included in the stored byte count.
 *
 * @param path     Virtual asset path (e.g. "config/mod_settings.json").
 * @param content  Null-terminated UTF-8 string to store. Must not be nullptr.
 */
void VirtualAssets_AddTextFile(const char* path, const char* content);

/**
 * @brief Removes a virtual entry from the registry.
 *
 * After this call, opens for @p path fall through to the real AAssetManager
 * (or fail if the path is still blocked).
 *
 * Has no effect if @p path has no registered virtual entry.
 *
 * @param path  Virtual asset path to remove.
 */
void VirtualAssets_RemoveFile(const char* path);

/**
 * @brief Returns whether a virtual entry is registered for @p path.
 *
 * Returns true even if the path is currently blocked. Does NOT query whether
 * the file exists in the real APK.
 *
 * @param path  Virtual asset path to query.
 * @return true if a virtual entry exists for @p path.
 */
bool VirtualAssets_HasFile(const char* path);

/**
 * @brief Bulk-registers all files in an on-device directory as virtual assets.
 *
 * Walks @p storageDir on the real device filesystem and registers each file
 * as a virtual asset under @p virtualBaseDir, preserving relative subdirectory
 * structure.  Files are loaded **lazily** — the virtual entry stores the real
 * path and bytes are read only when the game actually opens the asset.  This
 * means you can edit files on storage and the changes are visible immediately
 * without calling LoadDir again.
 *
 * Example:
 * @code
 *   // Device:    /sdcard/MyMod/textures/grass.png
 *   // Served as: assets/textures/grass.png  in the virtual asset tree
 *   VirtualAssets_LoadDir("/sdcard/MyMod/textures", "textures", true);
 * @endcode
 *
 * @param storageDir      Absolute path to the source directory on the device
 *                        (e.g. "/sdcard/MyMod/assets"). Must exist and be readable.
 * @param virtualBaseDir  Prefix in the virtual asset tree where files are placed
 *                        (e.g. "textures/custom"). Pass "" to map directly under
 *                        the asset root.
 * @param recursive       true = traverse subdirectories and register their contents
 *                        recursively, preserving folder structure.
 *                        false = register only the immediate files in @p storageDir.
 * @return Number of files successfully registered; -1 if @p storageDir does not
 *         exist, is not a directory, or cannot be read.
 */
int VirtualAssets_LoadDir(const char* storageDir, const char* virtualBaseDir, bool recursive);

/**
 * @brief Points a virtual asset entry at an on-device file, replacing its contents.
 *
 * Creates or updates the virtual entry at @p virtualPath so that it is backed
 * by @p storagePath on the real filesystem. The file at @p storagePath is
 * read each time the asset is opened, so disk edits are visible immediately
 * without calling ReplaceFile again.
 *
 * Prefer this over VirtualAssets_AddFile() when the source data is a file on
 * device storage that may be updated while the game is running.
 *
 * @param virtualPath  Destination in the virtual asset tree
 *                     (e.g. "assets/textures/terrain/grass.png").
 * @param storagePath  Absolute path to the replacement file on the device
 *                     (e.g. "/sdcard/MyMod/grass.png"). Must be readable.
 * @return true if the virtual entry was created or updated successfully.
 *         false if @p storagePath cannot be accessed.
 */
bool VirtualAssets_ReplaceFile(const char* virtualPath, const char* storagePath);

/**
 * @brief Reads a virtual asset into a newly heap-allocated buffer.
 *
 * Only reads from the virtual registry — does NOT fall through to the real
 * AAssetManager. The caller is responsible for freeing the returned buffer
 * with free().
 *
 * @param path     Virtual asset path to read.
 * @param outSize  Out-param set to the size of the returned buffer in bytes.
 *                 Set to 0 on failure. Must not be nullptr.
 * @return Pointer to a malloc'd buffer containing the raw file bytes on success;
 *         nullptr if @p path is not in the virtual registry, is blocked, or
 *         memory allocation fails. Caller must free() the pointer.
 */
void* VirtualAssets_ReadFile(const char* path, size_t* outSize);

/**
 * @brief Overwrites the stored bytes of an existing virtual asset entry.
 *
 * Replaces the internally held data for @p path with @p data. The new bytes
 * are copied internally; the caller retains ownership of @p data.
 *
 * The entry at @p path must already exist. If it does not, call
 * VirtualAssets_AddFile() first to create it.
 *
 * @param path  Virtual asset path of the entry to update.
 * @param data  Pointer to the new raw bytes. Must not be nullptr.
 * @param size  Number of bytes in @p data.
 */
void VirtualAssets_EditFile(const char* path, const void* data, size_t size);

/** @} */ // end of VirtualAssets group


// ============================================================
//  Miscellaneous utilities
// ============================================================

/**
 * @brief Returns a pointer to the active EGL rendering context.
 *
 * Tracked by the egl_hook layer. Useful when a mod needs to issue raw
 * OpenGL/ES calls that must execute on the render context (creating textures,
 * sampling the framebuffer, etc.).
 *
 * @return Opaque pointer to the current EGLContext; nullptr if the render
 *         context has not been set up yet or the EGL hook is not installed.
 */
void* GetGLContextPointer();

/**
 * @brief Returns the renderer's current frames-per-second measurement.
 *
 * Read from the same frame-timing counter Minecraft uses internally, so the
 * value reflects actual rendered frames rather than a fixed game-tick rate.
 *
 * @return Instantaneous FPS as a float (e.g. 59.94f); 0.0f if no frame has
 *         been rendered yet.
 */
float GetCurrentFPS();

/**
 * @brief Emits a log message through the Apps client logger.
 *
 *
 * @param threadName  Human-readable name of the calling thread, used as a log
 *                    prefix (e.g. "RenderThread", "ModInit"). Does not need to
 *                    match the OS thread name.
 * @param tag         Category tag for log filtering
 *                    (e.g. "VirtualAssets", "MyMod", "HookManager").
 * @param message     Null-terminated UTF-8 message string to emit.
 */
extern "C" void ClientLog(const char* threadName, const char* tag, const char* message);
