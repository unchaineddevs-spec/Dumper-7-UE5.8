
# Dumper-7 UE5.8

Fork of [Encryqed/Dumper-7](https://github.com/Encryqed/Dumper-7) with enhanced support for **Unreal Engine 5.8**.

## What is Dumper-7?

An SDK generator for Unreal Engine games. It is compiled as an x64 DLL, injected into the target game process, and generates a full C++ SDK from the engine's live reflection data (GObjects, GNames, UClass, FField, etc.).

Supported versions: **UE4.11 through UE5.8**. Tested against ARK: Survival Ascended v97 (UE 5.8.3).

## UE 5.8 Changes in This Fork

UE 5.8 introduced several internal changes that affect SDK generators:

### GObjects / FChunkedFixedUObjectArray
- Added layout variants for UE 5.8 development and shipping builds, where the header field order was rearranged (`NumElements`/`MaxElements`/`NumChunks`/`MaxChunks` offsets differ from UE 5.7)

### FField Vtable
- UE 5.8 removed the virtual functions `Bind()`, `PostLoad()`, and `BeginDestroy()` from FField, changing the vtable slot layout
- The dumper dynamically discovers FField offsets, so this is handled automatically

### FNamePool / GNames
- `FNamePool::FNamePool` in UE 5.8 zero-initializes its `FRWLock` with a plain store instead of calling `InitializeSRWLock`, and the inlined `GetNamePool()` lazy-init sites look like `lea rcx, NamePoolData; <spill>; jne; call FNamePool::FNamePool`. Upstream's two GNames finders depend on exactly those two details, so they fail on UE 5.8 games (verified on ARK: Survival Ascended v97)
- New finder `NameArray::TryFindNamePoolCtorXref_Windows()`: locates the constructor through its reference to the hardcoded `"ByteProperty"` name (registered right after `"None"`), resolves the function start through the exception directory (`RtlLookupFunctionEntry`), scans `.text` for calls to it and takes the `lea rcx` in front of each call as `NamePoolData`. Candidates are validated by checking that `Blocks[0]` starts with the `None` entry
- The pool layout itself is unchanged (`CurrentBlock` at +0x8, `Blocks` at +0x10, 0x20000-byte blocks = 16 offset bits); `FNameBlockOffsetBits` is still determined dynamically, with bounds checking to prevent infinite loops
- The brute-force fallback no longer requires a NUL byte after `None`, since UE5 name entries are not NUL-terminated
- A `Dumper7_debug.log` with every discovery step is written next to the game executable

### FFieldClass
- UE 5.8 inserts a type-descriptor pointer at offset 0, shifting `Name` (0x08), `ClassFlags` (0x10), `Id` (0x18), `CastFlags` (0x20) and `SuperClass` (0x28). The new layout is detected at runtime when the dynamically found `CastFlags` lands at or beyond the old `SuperClass` offset (ported from upstream [PR #594](https://github.com/Encryqed/Dumper-7/pull/594) / [issue #544](https://github.com/Encryqed/Dumper-7/issues/544))

### UScriptStruct
- Added `STRUCT_MoveAssignNative` flag (0x01000000) and corresponding `MoveAssign` fake-vtable entry
- Full `EStructFlags` enum added to the dumper

### UEnum
- `EUnderlyingType` support (added in late UE 5.x) is already present and detected automatically

## Usage

1. Build the DLL as **x64 Release** using Visual Studio (v145 toolset / VS 2025, or v143 / VS 2022)
2. Inject the DLL into your target UE 5.8 game process
3. The SDK is generated to the path set in `Settings::SDKGenerationPath` (default: `C:\Dumper-7`)
4. Press **F6** to unload, or set `ShouldAutoUnload=true` in `Dumper-7.ini`
5. **See [UsingTheSDK](UsingTheSDK.md) for a guide on using the generated SDK**

### Configuration

Create `Dumper-7.ini` next to the game executable, or at `C:\Dumper-7\Dumper-7.ini`:

```ini
[Settings]
SleepTimeout=5
SDKGenerationPath=C:/Dumper-7
ShouldAutoUnload=false
```

### Manual Overrides

If automatic offset detection fails, override in `Generator::InitEngineCore()` inside `Generator.cpp`:

```cpp
// GObjects override (chunked, UE4.21+)
ObjectArray::Init(/*GObjectsOffset*/, /*ChunkSize*/, /*Layout = Default*/);

// FName override
FName::Init(/*Offset, OverrideType, bIsNamePool*/);

// ProcessEvent index override  
Off::InSDK::ProcessEvent::InitPE(/*PEIndex*/);
```

For GObjects encryption (some games XOR the object pointers):
```cpp
InitObjectArrayDecryption([](void* ObjPtr) -> uint8* {
    return reinterpret_cast<uint8*>(uint64(ObjPtr) ^ 0xYOUR_KEY);
});
```

### Overriding GObjects Layout

Only add a new layout if GObjects isn't automatically found for your game. Layout overrides are at roughly line 30 of `ObjectArray.cpp`:

```cpp
FChunkedFixedUObjectArrayLayout // UE5.8 Development Build
{
    .ObjectsOffset = 0x00, 
    .MaxElementsOffset = 0x0C,
    .NumElementsOffset = 0x08,
    .MaxChunksOffset = 0x14,
    .NumChunksOffset = 0x10,
}
```

## Building

### Visual Studio
Open `Dumper-7.sln` and build in **x64 Release** configuration.

### CMake
```bash
cmake --preset release
cmake --build --preset release
```

## Credits

- [Encryqed/Dumper-7](https://github.com/Encryqed/Dumper-7) - Original project
- UE 5.8 fork and adaptations
