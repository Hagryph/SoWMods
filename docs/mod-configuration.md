# Global Mod Configuration

Include `shared/ModConfig.h`. During `SoWMod_Init(page)`, create a `sowmod::Config`
instance using the Loader-assigned page. Keep the instance and typed option handles
for subsequent callbacks:

```cpp
sowmod::Config config;
sowmod::Option<bool> ignoreLevel;

void InitializeSettings(int page) {
    config = sowmod::Config(page);
    ignoreLevel = config.Register<bool>("Recruitment", "IgnoreRecruitmentLevel", false);
}

bool enabled = config.Read(ignoreLevel);
bool saved = config.Update(ignoreLevel, true);
```

The Loader owns the registry, cached reads, synchronization, validation and INI
persistence. No mod-side filesystem code or config deployment is needed. Each
loaded DLL gets `<game>/mods/cfg/<DLL-stem>.ini` on its first successful option
registration. Mods with no registered options create no INI. Settings are global
across saves. Existing valid entries are retained; missing/invalid entries are
written with their registered defaults. Disk edits are loaded on the next launch.

Supported C++ types: `bool`, `int`, `int64_t`, `float`, `double`, `std::string`.
Each registration requires a section, key, type and default. Sections/keys permit
ASCII letters, digits, dots, hyphens and underscores (1-128 characters).
Names are case-insensitive, like Windows INI names. Re-registering a key must use
the same storage type and default. Typed handles prevent accidental type changes;
handles from a different mod or unregistered handles cannot be used by Config.
Choose one C++ type per key; an `int` read of an out-of-range `int64_t` value fails.

Registration/read failures throw `std::runtime_error`; catch during initialization
and disable the affected feature. Update returns false on validation/persistence
failure and retains the previous cached value. Reads do not access disk. Values
are limited to 4095 UTF-8 bytes; NUL bytes and nonfinite real numbers are rejected.
Ordinary ASCII strings are human-readable; Unicode strings and strings requiring
whitespace/quote/newline preservation use an `@utf8:` hex representation in the INI,
so legacy ANSI INI files cannot corrupt UTF-8 values.

The versioned flat `SoWConfig_GetAPI(1)` export carries only primitive values and
caller-owned buffers across DLL boundaries. The header resolves it automatically;
no import library is required. This service does not change the Loader's legacy
internal console configuration.
