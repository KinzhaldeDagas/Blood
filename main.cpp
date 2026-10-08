#include "config.h"

#include "obse/GameAPI.h"
#include "obse/GameObjects.h"
#include "obse/GameProcess.h"
#include "obse/NiNodes.h"
#include "obse/PluginAPI.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <shlobj.h>
#include <windows.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

IDebugLog gLog("Blood.log");

PluginHandle g_pluginHandle = kPluginHandle_Invalid;
OBSEMessagingInterface* g_messaging = nullptr;

namespace
{
    static const UInt32 kActorKillHandleDeathStateCall = 0x00600B61;
    static const UInt32 kActorHandleDeathState = 0x005E6680;
    static const UInt32 kActorShouldEmitBloodEffects = 0x005E1B30;
    static const UInt32 kActorGetBloodDecalTexturePath = 0x005E1BB0;
    static const UInt32 kActorHandleHitVisualEffects = 0x005EE760;
    static const UInt32 kDecalProjectToSceneGeometry = 0x004D3B10;
    static const UInt32 kGameSettingMinBloodDamageCombat = 0x00B148DC;
    static const UInt32 kGameSettingDecalLifetimeDisplay = 0x00B097C8;
    static const UInt32 kGameSettingMaxDecalsPerFrameDisplay = 0x00B097D0;
    static const UInt32 kNativeHitBloodGateFlag = 0x00B333B8;
    static const UInt32 kNativePlayerDistanceCheckControllerField14 = 0x00B3B914;
    static const UInt32 kGameSettingMaxHiPerfCombatCountCombat = 0x00B148E4;
    static const UInt32 kNativePlayerCharacterSingleton = 0x00B333C4;
    static const UInt32 kMainLoopTickPatch = 0x0040F1A3;
    static UInt32 s_mainLoopTickReturn = 0x0040F1A8;

    static const UInt8 kMainLoopTickExpectedBytes[5] = { 0x8B, 0x08, 0x8B, 0x51, 0x0C };
    static const UInt8 kHitVisualsExpectedBytes[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
    static const float kNativeDecalProjectionRayLength = 80.0f;
    static const float kMaxFloorProjectionHeight = 72.0f;
    static const float kMaxBloodDurationSeconds = 600.0f;
    static const float kMaxDecalLifetimeSeconds = 31536000.0f;
    static const float kCellScopedDecalLifetimeSeconds = kMaxDecalLifetimeSeconds;
    static const UInt32 kUnlimitedDecalsPerFrame = 0x7FFFFFFF;
    static const UInt32 kMaxLimbLeakSections = 13;
    static const UInt32 kMaxBoneNamesPerLimbSection = 4;
    static const UInt32 kDefaultOutwardBloodSpurtsPerLimbSection = 2;
    static const UInt32 kMaxOutwardBloodSpurtsPerLimbSection = 6;
    static const UInt32 kDefaultBloodProjectionsPerLimbSection = 1 + kDefaultOutwardBloodSpurtsPerLimbSection;
    static const UInt32 kMaxBloodProjectionsPerLimbSection = 1 + kMaxOutwardBloodSpurtsPerLimbSection;
    static const UInt32 kDefaultWoundProjectionsPerDrip = kDefaultBloodProjectionsPerLimbSection;
    static const UInt32 kMaxWoundProjectionsPerDrip = kMaxBloodProjectionsPerLimbSection;
    static const UInt32 kDefaultCorpseMovementDropsPerLimbUpdate = 12;
    static const UInt32 kMaxCorpseMovementDropsPerLimbUpdate = 64;
    static const UInt32 kMovementDirectionMask = 0x0000000F;
    static const UInt32 kMovementWalkRunMask = 0x00000300;
    static const UInt32 kWoundMovementConfirmationGraceMs = 250;
    static const float kMinimumWoundFrameMovementDistance = 0.05f;
    static const float kMaximumWoundFrameMovementDistance = 256.0f;
    static const char kIniFileName[] = "Blood.ini";

    typedef void (__thiscall* ActorHandleDeathStateFn)(Actor* actor, UInt32 newDeadState);
    typedef UInt8 (__thiscall* ActorShouldEmitBloodEffectsFn)(TESObjectREFR* actor);
    typedef const char* (__thiscall* ActorGetBloodDecalTexturePathFn)(TESObjectREFR* actor);
    typedef void (__thiscall* ActorHandleHitVisualEffectsFn)(
        Actor* actor,
        TESObjectREFR* source,
        float arg4,
        float arg8,
        void* localStart,
        void* localEnd);
    typedef void (__thiscall* DecalProjectToSceneGeometryFn)(
        TESObjectCELL* cell,
        float x,
        float y,
        float z,
        float directionX,
        float directionY,
        float directionZ,
        const char* texturePath,
        TESObjectREFR* explicitTarget,
        SInt32 selector,
        UInt32 alternateGeometry);

    struct Settings
    {
        bool enabled;
        bool includePlayer;
        bool logSpawns;
        bool setGameDecalLifetime;
        bool extendHitBloodDecals;
        bool setGameDecalLimit;
        UInt32 totalDecals;
        UInt32 maxSplatterDecals;
        UInt32 maxWoundTrailDecals;
        UInt32 woundProjectionsPerDrip;
        UInt32 outwardBloodSpurtsPerLimbSection;
        UInt32 maxCorpseMovementDropsPerLimbUpdate;
        UInt32 maxDecalsPerFrame;
        UInt32 leakDurationMs;
        UInt32 woundTrailDurationMs;
        UInt32 woundDripMinIntervalMs;
        UInt32 woundDripMaxIntervalMs;
        UInt32 settleMs;
        UInt32 unloadedTtlMs;
        float decalLifetimeSeconds;
        float spillRadius;
        float leakLength;
        float projectionHeight;
        float settleMoveTolerance;
        float corpseMoveDropDistance;
        float woundTrailRadiusScale;
        float corpseMoveRadiusScale;
    };

    struct LeakTracker
    {
        UInt32 refId;
        UInt32 stableSinceTick;
        UInt32 leakStartTick;
        UInt32 lastPulseTick;
        UInt32 lastLoadedTick;
        UInt32 pulseIndex;
        UInt32 cycleIndex;
        UInt32 splatterCount;
        bool hasPosition;
        bool completed;
        bool initialPulsePending;
        bool leakStarted;
        bool hasPulseTime;
        float lastX;
        float lastY;
        float lastZ;
        float anchorX;
        float anchorY;
        float anchorZ;
        float anchorFloorZ;
        UInt32 limbCount;
        struct LimbAnchor
        {
            bool active;
            UInt32 sectionIndex;
            float x;
            float y;
            float z;
            float floorZ;
            float lastMoveX;
            float lastMoveY;
            float lastMoveZ;
        } limbs[kMaxLimbLeakSections];
    };

    struct WoundTracker
    {
        UInt32 refId;
        UInt32 woundStartTick;
        UInt32 lastLoadedTick;
        UInt32 movementIntervalStartTick;
        UInt32 nextDripIntervalMs;
        UInt32 lastConfirmedMovementTick;
        UInt32 projectionCount;
        UInt32 cycleIndex;
        bool hasPosition;
        bool wasWalkingOrRunning;
        bool hasConfirmedMovement;
        float lastX;
        float lastY;
        float lastZ;
        bool hasHitDirection;
        float hitDirectionX;
        float hitDirectionY;
    };

    struct LimbSectionDef
    {
        const char* label;
        const char* boneNames[kMaxBoneNamesPerLimbSection];
        float rightOffset;
        float forwardOffset;
        float heightOffset;
        bool allowFallback;
    };

    static const LimbSectionDef kLimbSections[kMaxLimbLeakSections] =
    {
        { "head", { "Bip01 Head", "Bip02 Head", "Bip01 Neck", nullptr }, 0.0f, 46.0f, 56.0f, true },
        { "torso", { "Bip01 Spine2", "Bip01 Spine1", "Bip01 Spine", "Bip01 Pelvis" }, 0.0f, 16.0f, 36.0f, true },
        { "left upper arm", { "Bip01 L UpperArm", "Bip01 L Clavicle", nullptr, nullptr }, -24.0f, 18.0f, 38.0f, true },
        { "left forearm", { "Bip01 L Forearm", "Bip01 L ForearmTwist", "Bip01 L Hand", nullptr }, -42.0f, 10.0f, 28.0f, true },
        { "right upper arm", { "Bip01 R UpperArm", "Bip01 R Clavicle", nullptr, nullptr }, 24.0f, 18.0f, 38.0f, true },
        { "right forearm", { "Bip01 R Forearm", "Bip01 R ForearmTwist", "Bip01 R Hand", nullptr }, 42.0f, 10.0f, 28.0f, true },
        { "left thigh", { "Bip01 L Thigh", nullptr, nullptr, nullptr }, -16.0f, -34.0f, 24.0f, true },
        { "left calf", { "Bip01 L Calf", nullptr, nullptr, nullptr }, -16.0f, -54.0f, 14.0f, true },
        { "left foot", { "Bip01 L Foot", "Bip01 L Toe0", nullptr, nullptr }, -16.0f, -76.0f, 4.0f, true },
        { "right thigh", { "Bip01 R Thigh", nullptr, nullptr, nullptr }, 16.0f, -34.0f, 24.0f, true },
        { "right calf", { "Bip01 R Calf", nullptr, nullptr, nullptr }, 16.0f, -54.0f, 14.0f, true },
        { "right foot", { "Bip01 R Foot", "Bip01 R Toe0", nullptr, nullptr }, 16.0f, -76.0f, 4.0f, true },
        { "tail", { "Bip01 Tail", nullptr, nullptr, nullptr }, 0.0f, -92.0f, 12.0f, false }
    };

    static ActorHandleDeathStateFn s_actorHandleDeathState =
        reinterpret_cast<ActorHandleDeathStateFn>(kActorHandleDeathState);
    static const ActorShouldEmitBloodEffectsFn ActorShouldEmitBloodEffects =
        reinterpret_cast<ActorShouldEmitBloodEffectsFn>(kActorShouldEmitBloodEffects);
    static const ActorGetBloodDecalTexturePathFn ActorGetBloodDecalTexturePath =
        reinterpret_cast<ActorGetBloodDecalTexturePathFn>(kActorGetBloodDecalTexturePath);
    static const DecalProjectToSceneGeometryFn DecalProjectToSceneGeometry =
        reinterpret_cast<DecalProjectToSceneGeometryFn>(kDecalProjectToSceneGeometry);
    static ActorHandleHitVisualEffectsFn s_actorHandleHitVisualEffects =
        reinterpret_cast<ActorHandleHitVisualEffectsFn>(kActorHandleHitVisualEffects);

    static const Settings kEmbeddedDefaults =
    {
        true,
        true,
        false,
        true,
        true,
        true,
        24,
        4096,
        4096,
        kDefaultWoundProjectionsPerDrip,
        kDefaultOutwardBloodSpurtsPerLimbSection,
        kDefaultCorpseMovementDropsPerLimbUpdate,
        kUnlimitedDecalsPerFrame,
        18000,
        180000,
        2000,
        3000,
        0,
        30000,
        kCellScopedDecalLifetimeSeconds,
        128.0f,
        160.0f,
        48.0f,
        4.0f,
        18.0f,
        0.40f,
        0.55f
    };

    static Settings s_settings = kEmbeddedDefaults;

    static bool s_deathHookInstalled = false;
    static bool s_hitVisualsHookInstalled = false;
    static bool s_frameHookInstalled = false;
    __declspec(align(16)) static UInt8 s_frameHookFxState[512] = {};
    static std::map<UInt32, LeakTracker> s_leaks;
    static std::map<UInt32, WoundTracker> s_wounds;
    static char s_iniPath[MAX_PATH] = {};
    static bool s_iniLoadedFromFile = false;

    static UInt32 ClampUInt32(UInt32 value, UInt32 minimum, UInt32 maximum)
    {
        if (value < minimum)
            return minimum;
        if (value > maximum)
            return maximum;
        return value;
    }

    static bool IsFinite(float value)
    {
        return std::isfinite(value) != 0;
    }

    static float ClampFloat(float value, float minimum, float maximum)
    {
        if (!IsFinite(value))
            return minimum;
        if (value < minimum)
            return minimum;
        if (value > maximum)
            return maximum;
        return value;
    }

    static bool IsFinitePoint(float x, float y, float z)
    {
        return IsFinite(x) && IsFinite(y) && IsFinite(z);
    }

    static bool NormalizeVector(float& x, float& y, float& z)
    {
        const float lengthSq = (x * x) + (y * y) + (z * z);
        if (!IsFinite(lengthSq) || lengthSq < 0.0001f)
            return false;

        const float invLength = 1.0f / std::sqrt(lengthSq);
        x *= invLength;
        y *= invLength;
        z *= invLength;
        return IsFinitePoint(x, y, z);
    }

    static UInt32 ElapsedMs(UInt32 now, UInt32 then)
    {
        return now - then;
    }

    static UInt32 RemainingCapacity(UInt32 count, UInt32 cap)
    {
        return count < cap ? cap - count : 0;
    }

    static float DistanceSquared(float ax, float ay, float az, float bx, float by, float bz)
    {
        const float dx = ax - bx;
        const float dy = ay - by;
        const float dz = az - bz;
        return dx * dx + dy * dy + dz * dz;
    }

    static UInt32 Mix(UInt32 value)
    {
        value ^= value >> 16;
        value *= 0x7FEB352Du;
        value ^= value >> 15;
        value *= 0x846CA68Bu;
        value ^= value >> 16;
        return value;
    }

    static float UnitHash(UInt32 seed)
    {
        return static_cast<float>(Mix(seed) & 0xFFFFu) / 65535.0f;
    }

    static const char kDefaultIniText[] =
        "; BloodOnDeath runtime settings\r\n"
        "; Place this file beside Blood.dll in Data\\OBSE\\Plugins.\r\n"
        "; Booleans accept 0/1, true/false, yes/no, and on/off.\r\n"
        "\r\n"
        "[General]\r\n"
        "bEnabled=1\r\n"
        "bIncludePlayer=1\r\n"
        "bLogSpawns=0\r\n"
        "\r\n"
        "[Decals]\r\n"
        "; One year by default, so blood effectively persists until cell reset/unload cleanup.\r\n"
        "bSetGameDecalLifetime=1\r\n"
        "fDecalLifetimeSeconds=31536000.0\r\n"
        "bExtendHitBloodDecals=1\r\n"
        "bSetGameDecalLimit=1\r\n"
        "iMaxDecalsPerFrame=2147483647\r\n"
        "\r\n"
        "[DeathBlood]\r\n"
        "; iTotalDecals is the timed pulse count per resolved body section.\r\n"
        "; iMaxSplatterDecals is a legacy name: it caps native projector calls, not attached geometry objects.\r\n"
        "iTotalDecals=24\r\n"
        "iMaxSplatterDecals=4096\r\n"
        "iLeakDurationMs=18000\r\n"
        "iSettleMs=0\r\n"
        "iUnloadedTtlMs=30000\r\n"
        "fSpillRadius=128.0\r\n"
        "fLeakLength=160.0\r\n"
        "fProjectionHeight=48.0\r\n"
        "fSettleMoveTolerance=4.0\r\n"
        "; Applies to death spills, corpse hits, wound trails, and moved-corpse limb clusters.\r\n"
        "iOutwardBloodSpurtsPerLimbSection=2\r\n"
        "\r\n"
        "[WoundTrails]\r\n"
        "; The two *Decals keys cap native projector calls; one call may attach to multiple geometry objects.\r\n"
        "iMaxWoundTrailDecals=4096\r\n"
        "; Legacy key name: projections emitted by one timed wound drip.\r\n"
        "iWoundTrailDecalsPerDrop=3\r\n"
        "iWoundTrailDurationMs=180000\r\n"
        "iWoundDripMinIntervalMs=2000\r\n"
        "iWoundDripMaxIntervalMs=3000\r\n"
        "fWoundTrailRadiusScale=0.40\r\n"
        "\r\n"
        "[CorpseMovement]\r\n"
        "fCorpseMoveDropDistance=18.0\r\n"
        "fCorpseMoveRadiusScale=0.55\r\n"
        "iMaxDropsPerLimbUpdate=12\r\n";

    static bool IsAsciiSpace(char value)
    {
        return value == ' ' || value == '\t' || value == '\r' || value == '\n';
    }

    static void TrimAsciiInPlace(char* value)
    {
        if (!value)
            return;

        char* start = value;
        while (*start && IsAsciiSpace(*start))
            ++start;

        if (start != value)
            std::memmove(value, start, std::strlen(start) + 1);

        UInt32 length = static_cast<UInt32>(std::strlen(value));
        while (length > 0 && IsAsciiSpace(value[length - 1]))
        {
            value[length - 1] = '\0';
            --length;
        }
    }

    static bool ResolveSettingsIniPath()
    {
        s_iniPath[0] = '\0';

        char modulePath[MAX_PATH] = {};
        DWORD length = GetModuleFileNameA(reinterpret_cast<HMODULE>(&__ImageBase), modulePath, sizeof(modulePath));
        if (!length || length >= sizeof(modulePath))
        {
            HMODULE module = GetModuleHandleA("Blood.dll");
            length = module ? GetModuleFileNameA(module, modulePath, sizeof(modulePath)) : 0;
        }

        if (!length || length >= sizeof(modulePath))
            return false;

        char* slash = std::strrchr(modulePath, '\\');
        char* forwardSlash = std::strrchr(modulePath, '/');
        if (forwardSlash && (!slash || forwardSlash > slash))
            slash = forwardSlash;
        if (!slash)
            return false;

        slash[1] = '\0';
        if (std::strlen(modulePath) + std::strlen(kIniFileName) >= sizeof(s_iniPath))
            return false;

        strcpy_s(s_iniPath, sizeof(s_iniPath), modulePath);
        strcat_s(s_iniPath, sizeof(s_iniPath), kIniFileName);
        return true;
    }

    static bool WriteDefaultIniFile(const char* iniPath)
    {
        if (!iniPath || !iniPath[0])
            return false;

        HANDLE file = CreateFileA(
            iniPath,
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (file == INVALID_HANDLE_VALUE)
            return false;

        DWORD bytesWritten = 0;
        const DWORD bytesToWrite = static_cast<DWORD>(sizeof(kDefaultIniText) - 1);
        const BOOL wrote = WriteFile(file, kDefaultIniText, bytesToWrite, &bytesWritten, nullptr);
        CloseHandle(file);
        return wrote && bytesWritten == bytesToWrite;
    }

    static bool PrepareSettingsIni()
    {
        s_iniLoadedFromFile = false;
        if (!ResolveSettingsIniPath())
        {
            _MESSAGE("BloodOnDeath could not resolve Blood.ini path; using embedded defaults");
            return false;
        }

        DWORD attributes = GetFileAttributesA(s_iniPath);
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            if (WriteDefaultIniFile(s_iniPath))
            {
                _MESSAGE("BloodOnDeath created default settings file: %s", s_iniPath);
                attributes = GetFileAttributesA(s_iniPath);
            }
            else
            {
                _MESSAGE("BloodOnDeath settings file not found and could not be created: %s; using embedded defaults", s_iniPath);
                return false;
            }
        }

        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            _MESSAGE("BloodOnDeath settings path is not a file: %s; using embedded defaults", s_iniPath);
            return false;
        }

        s_iniLoadedFromFile = true;
        return true;
    }

    static bool ReadIniValue(const char* section, const char* key, char* value, UInt32 valueSize)
    {
        if (!s_iniLoadedFromFile || !section || !key || !value || !valueSize)
            return false;

        static const char kMissingValue[] = "__BloodOnDeathMissingValue__";
        GetPrivateProfileStringA(section, key, kMissingValue, value, valueSize, s_iniPath);
        value[valueSize - 1] = '\0';
        if (std::strcmp(value, kMissingValue) == 0)
            return false;

        TrimAsciiInPlace(value);
        return true;
    }

    static bool ReadIniBool(const char* section, const char* key, bool defaultValue)
    {
        char value[64] = {};
        if (!ReadIniValue(section, key, value, sizeof(value)))
            return defaultValue;

        if (!_stricmp(value, "1") || !_stricmp(value, "true") || !_stricmp(value, "yes") || !_stricmp(value, "on") || !_stricmp(value, "enabled"))
            return true;
        if (!_stricmp(value, "0") || !_stricmp(value, "false") || !_stricmp(value, "no") || !_stricmp(value, "off") || !_stricmp(value, "disabled"))
            return false;

        _MESSAGE("BloodOnDeath ignored invalid boolean setting [%s] %s=%s", section, key, value);
        return defaultValue;
    }

    static UInt32 ReadIniUInt32(const char* section, const char* key, UInt32 defaultValue)
    {
        char value[64] = {};
        if (!ReadIniValue(section, key, value, sizeof(value)))
            return defaultValue;

        if (!_stricmp(value, "unlimited"))
            return kUnlimitedDecalsPerFrame;
        if (value[0] == '-')
        {
            _MESSAGE("BloodOnDeath ignored negative integer setting [%s] %s=%s", section, key, value);
            return defaultValue;
        }

        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end == value || (end && *end))
        {
            _MESSAGE("BloodOnDeath ignored invalid integer setting [%s] %s=%s", section, key, value);
            return defaultValue;
        }

        return static_cast<UInt32>(parsed);
    }

    static float ReadIniFloat(const char* section, const char* key, float defaultValue)
    {
        char value[64] = {};
        if (!ReadIniValue(section, key, value, sizeof(value)))
            return defaultValue;

        char* end = nullptr;
        const float parsed = static_cast<float>(std::strtod(value, &end));
        if (end == value || (end && *end) || !IsFinite(parsed))
        {
            _MESSAGE("BloodOnDeath ignored invalid float setting [%s] %s=%s", section, key, value);
            return defaultValue;
        }

        return parsed;
    }

    static void ApplyIniSettings()
    {
        s_settings.enabled = ReadIniBool("General", "bEnabled", s_settings.enabled);
        s_settings.includePlayer = ReadIniBool("General", "bIncludePlayer", s_settings.includePlayer);
        s_settings.logSpawns = ReadIniBool("General", "bLogSpawns", s_settings.logSpawns);

        s_settings.setGameDecalLifetime = ReadIniBool("Decals", "bSetGameDecalLifetime", s_settings.setGameDecalLifetime);
        s_settings.decalLifetimeSeconds = ReadIniFloat("Decals", "fDecalLifetimeSeconds", s_settings.decalLifetimeSeconds);
        s_settings.extendHitBloodDecals = ReadIniBool("Decals", "bExtendHitBloodDecals", s_settings.extendHitBloodDecals);
        s_settings.setGameDecalLimit = ReadIniBool("Decals", "bSetGameDecalLimit", s_settings.setGameDecalLimit);
        s_settings.maxDecalsPerFrame = ReadIniUInt32("Decals", "iMaxDecalsPerFrame", s_settings.maxDecalsPerFrame);

        s_settings.totalDecals = ReadIniUInt32("DeathBlood", "iTotalDecals", s_settings.totalDecals);
        s_settings.maxSplatterDecals = ReadIniUInt32("DeathBlood", "iMaxSplatterDecals", s_settings.maxSplatterDecals);
        s_settings.leakDurationMs = ReadIniUInt32("DeathBlood", "iLeakDurationMs", s_settings.leakDurationMs);
        s_settings.settleMs = ReadIniUInt32("DeathBlood", "iSettleMs", s_settings.settleMs);
        s_settings.unloadedTtlMs = ReadIniUInt32("DeathBlood", "iUnloadedTtlMs", s_settings.unloadedTtlMs);
        s_settings.spillRadius = ReadIniFloat("DeathBlood", "fSpillRadius", s_settings.spillRadius);
        s_settings.leakLength = ReadIniFloat("DeathBlood", "fLeakLength", s_settings.leakLength);
        s_settings.projectionHeight = ReadIniFloat("DeathBlood", "fProjectionHeight", s_settings.projectionHeight);
        s_settings.settleMoveTolerance = ReadIniFloat("DeathBlood", "fSettleMoveTolerance", s_settings.settleMoveTolerance);
        s_settings.outwardBloodSpurtsPerLimbSection = ReadIniUInt32("DeathBlood", "iOutwardBloodSpurtsPerLimbSection", s_settings.outwardBloodSpurtsPerLimbSection);

        s_settings.maxWoundTrailDecals = ReadIniUInt32("WoundTrails", "iMaxWoundTrailDecals", s_settings.maxWoundTrailDecals);
        s_settings.woundProjectionsPerDrip = ReadIniUInt32("WoundTrails", "iWoundTrailDecalsPerDrop", s_settings.woundProjectionsPerDrip);
        s_settings.woundTrailDurationMs = ReadIniUInt32("WoundTrails", "iWoundTrailDurationMs", s_settings.woundTrailDurationMs);
        s_settings.woundDripMinIntervalMs = ReadIniUInt32("WoundTrails", "iWoundDripMinIntervalMs", s_settings.woundDripMinIntervalMs);
        s_settings.woundDripMaxIntervalMs = ReadIniUInt32("WoundTrails", "iWoundDripMaxIntervalMs", s_settings.woundDripMaxIntervalMs);
        s_settings.woundTrailRadiusScale = ReadIniFloat("WoundTrails", "fWoundTrailRadiusScale", s_settings.woundTrailRadiusScale);
        // Preserve the pre-v1.1.12 key as a fallback, then prefer the cleaned-up corpse-specific key.
        s_settings.maxCorpseMovementDropsPerLimbUpdate = ReadIniUInt32(
            "WoundTrails",
            "iMaxMovementTrailDropsPerUpdate",
            s_settings.maxCorpseMovementDropsPerLimbUpdate);

        s_settings.corpseMoveDropDistance = ReadIniFloat("CorpseMovement", "fCorpseMoveDropDistance", s_settings.corpseMoveDropDistance);
        s_settings.corpseMoveRadiusScale = ReadIniFloat("CorpseMovement", "fCorpseMoveRadiusScale", s_settings.corpseMoveRadiusScale);
        s_settings.maxCorpseMovementDropsPerLimbUpdate = ReadIniUInt32(
            "CorpseMovement",
            "iMaxDropsPerLimbUpdate",
            s_settings.maxCorpseMovementDropsPerLimbUpdate);
    }

    static void LoadSettings()
    {
        s_settings = kEmbeddedDefaults;
        if (PrepareSettingsIni())
            ApplyIniSettings();

        s_settings.totalDecals = ClampUInt32(s_settings.totalDecals, 1, 256);
        s_settings.maxSplatterDecals = ClampUInt32(s_settings.maxSplatterDecals, 1, 8192);
        s_settings.maxWoundTrailDecals = ClampUInt32(s_settings.maxWoundTrailDecals, 1, 4096);
        s_settings.woundProjectionsPerDrip = ClampUInt32(s_settings.woundProjectionsPerDrip, 1, kMaxWoundProjectionsPerDrip);
        s_settings.outwardBloodSpurtsPerLimbSection = ClampUInt32(s_settings.outwardBloodSpurtsPerLimbSection, 0, kMaxOutwardBloodSpurtsPerLimbSection);
        s_settings.maxCorpseMovementDropsPerLimbUpdate = ClampUInt32(
            s_settings.maxCorpseMovementDropsPerLimbUpdate,
            1,
            kMaxCorpseMovementDropsPerLimbUpdate);
        s_settings.maxDecalsPerFrame = ClampUInt32(s_settings.maxDecalsPerFrame, 1, kUnlimitedDecalsPerFrame);
        s_settings.leakDurationMs = ClampUInt32(s_settings.leakDurationMs, 1000, static_cast<UInt32>(kMaxBloodDurationSeconds * 1000.0f));
        s_settings.woundTrailDurationMs = ClampUInt32(s_settings.woundTrailDurationMs, 1000, static_cast<UInt32>(kMaxBloodDurationSeconds * 1000.0f));
        s_settings.woundDripMinIntervalMs = ClampUInt32(s_settings.woundDripMinIntervalMs, 250, 60000);
        s_settings.woundDripMaxIntervalMs = ClampUInt32(s_settings.woundDripMaxIntervalMs, s_settings.woundDripMinIntervalMs, 60000);
        s_settings.settleMs = ClampUInt32(s_settings.settleMs, 0, 5000);
        s_settings.unloadedTtlMs = ClampUInt32(s_settings.unloadedTtlMs, 1000, 300000);
        s_settings.decalLifetimeSeconds = ClampFloat(s_settings.decalLifetimeSeconds, 1.0f, kMaxDecalLifetimeSeconds);
        s_settings.spillRadius = ClampFloat(s_settings.spillRadius, 0.0f, 256.0f);
        s_settings.leakLength = ClampFloat(s_settings.leakLength, 0.0f, 256.0f);
        s_settings.projectionHeight = ClampFloat(s_settings.projectionHeight, 8.0f, kMaxFloorProjectionHeight);
        s_settings.settleMoveTolerance = ClampFloat(s_settings.settleMoveTolerance, 1.0f, 64.0f);
        s_settings.corpseMoveDropDistance = ClampFloat(s_settings.corpseMoveDropDistance, 4.0f, 256.0f);
        s_settings.woundTrailRadiusScale = ClampFloat(s_settings.woundTrailRadiusScale, 0.05f, 2.0f);
        s_settings.corpseMoveRadiusScale = ClampFloat(s_settings.corpseMoveRadiusScale, 0.05f, 2.0f);

        _MESSAGE(
            "BloodOnDeath config: %s enabled=%u includePlayer=%u setGameDecalLifetime=%u extendHitBloodDecals=%u setGameDecalLimit=%u totalDecals=%u maxSplatterDecals=%u maxWoundTrailDecals=%u woundProjectionsPerDrip=%u outwardSpurts=%u maxCorpseDropsPerLimbUpdate=%u maxDecalsPerFrame=%u durationMs=%u woundDurationMs=%u woundDripMinMs=%u woundDripMaxMs=%u settleMs=%u unloadedTtlMs=%u decalLifetime=%.2f radius=%.2f leak=%.2f height=%.2f settleTolerance=%.2f corpseMoveDrop=%.2f woundRadiusScale=%.2f corpseRadiusScale=%.2f log=%u",
            s_iniLoadedFromFile ? s_iniPath : "embedded",
            s_settings.enabled ? 1 : 0,
            s_settings.includePlayer ? 1 : 0,
            s_settings.setGameDecalLifetime ? 1 : 0,
            s_settings.extendHitBloodDecals ? 1 : 0,
            s_settings.setGameDecalLimit ? 1 : 0,
            s_settings.totalDecals,
            s_settings.maxSplatterDecals,
            s_settings.maxWoundTrailDecals,
            s_settings.woundProjectionsPerDrip,
            s_settings.outwardBloodSpurtsPerLimbSection,
            s_settings.maxCorpseMovementDropsPerLimbUpdate,
            s_settings.maxDecalsPerFrame,
            s_settings.leakDurationMs,
            s_settings.woundTrailDurationMs,
            s_settings.woundDripMinIntervalMs,
            s_settings.woundDripMaxIntervalMs,
            s_settings.settleMs,
            s_settings.unloadedTtlMs,
            s_settings.decalLifetimeSeconds,
            s_settings.spillRadius,
            s_settings.leakLength,
            s_settings.projectionHeight,
            s_settings.settleMoveTolerance,
            s_settings.corpseMoveDropDistance,
            s_settings.woundTrailRadiusScale,
            s_settings.corpseMoveRadiusScale,
            s_settings.logSpawns ? 1 : 0);
    }

    static UInt32 GetPulseIntervalMs()
    {
        return ClampUInt32(s_settings.leakDurationMs / s_settings.totalDecals, 100, 60000);
    }

    static SettingInfo* ResolveDecalLifetimeSetting()
    {
        SettingInfo* setting = nullptr;
        if (GetGameSetting("fDecalLifetime:Display", &setting) &&
            setting &&
            setting->Type() == SettingInfo::kSetting_Float)
        {
            return setting;
        }

        if (GetGameSetting("fDecalLifetime", &setting) &&
            setting &&
            setting->Type() == SettingInfo::kSetting_Float)
        {
            return setting;
        }

        SettingInfo* decodedSetting = reinterpret_cast<SettingInfo*>(kGameSettingDecalLifetimeDisplay);
        if (decodedSetting &&
            decodedSetting->name &&
            std::strcmp(decodedSetting->name, "fDecalLifetime:Display") == 0 &&
            decodedSetting->Type() == SettingInfo::kSetting_Float)
        {
            return decodedSetting;
        }

        return nullptr;
    }

    static bool IsIntegerSetting(SettingInfo* setting)
    {
        if (!setting)
            return false;

        const SettingInfo::EType type = setting->Type();
        return type == SettingInfo::kSetting_Integer || type == SettingInfo::kSetting_Unsigned;
    }

    static SettingInfo* ResolveMaxDecalsPerFrameSetting()
    {
        SettingInfo* setting = nullptr;
        if (GetGameSetting("iMaxDecalsPerFrame:Display", &setting) && IsIntegerSetting(setting))
            return setting;

        if (GetGameSetting("iMaxDecalsPerFrame", &setting) && IsIntegerSetting(setting))
            return setting;

        SettingInfo* decodedSetting = reinterpret_cast<SettingInfo*>(kGameSettingMaxDecalsPerFrameDisplay);
        if (decodedSetting &&
            decodedSetting->name &&
            std::strcmp(decodedSetting->name, "iMaxDecalsPerFrame:Display") == 0 &&
            IsIntegerSetting(decodedSetting))
        {
            return decodedSetting;
        }

        return nullptr;
    }

    static SettingInfo* ResolveMinBloodDamageSetting()
    {
        SettingInfo* setting = nullptr;
        if (GetGameSetting("fMinBloodDamage:Combat", &setting) &&
            setting &&
            setting->Type() == SettingInfo::kSetting_Float)
        {
            return setting;
        }

        if (GetGameSetting("fMinBloodDamage", &setting) &&
            setting &&
            setting->Type() == SettingInfo::kSetting_Float)
        {
            return setting;
        }

        SettingInfo* decodedSetting = reinterpret_cast<SettingInfo*>(kGameSettingMinBloodDamageCombat);
        if (decodedSetting &&
            decodedSetting->name &&
            std::strcmp(decodedSetting->name, "fMinBloodDamage:Combat") == 0 &&
            decodedSetting->Type() == SettingInfo::kSetting_Float)
        {
            return decodedSetting;
        }

        return nullptr;
    }

    static float GetNativeBloodDamageThreshold()
    {
        SettingInfo* setting = ResolveMinBloodDamageSetting();
        if (!setting)
            return 1.0f;

        // Native x87 code substitutes 1.0 only when the configured value is
        // ordered below 1.0. NaN and +infinity are retained and consequently
        // fail the later ordered damage comparison.
        return setting->f < 1.0f ? 1.0f : setting->f;
    }

    static UInt32 ReadIntegerSettingValue(SettingInfo* setting)
    {
        if (!setting)
            return 0;

        return setting->Type() == SettingInfo::kSetting_Unsigned ? setting->u : static_cast<UInt32>(setting->i);
    }

    static void WriteIntegerSettingValue(SettingInfo* setting, UInt32 value)
    {
        if (!setting)
            return;

        if (setting->Type() == SettingInfo::kSetting_Unsigned)
            setting->u = value;
        else
            setting->i = static_cast<int>(value);
    }

    static bool IntegerSettingValueIsBelow(SettingInfo* setting, UInt32 value)
    {
        if (!setting)
            return false;

        if (setting->Type() == SettingInfo::kSetting_Unsigned)
            return setting->u < value;

        // Decal_AttachToGeometryRecursive uses signed JG for the native
        // counter/setting comparison, so preserve signed ordering here too.
        return setting->i < static_cast<SInt32>(value);
    }

    struct ScopedDecalCreationSettings
    {
        SettingInfo* lifetimeSetting;
        SettingInfo* maxPerFrameSetting;
        float originalLifetime;
        UInt32 originalMaxPerFrame;
        bool changedLifetime;
        bool changedMaxPerFrame;

        explicit ScopedDecalCreationSettings(bool extendLifetime, bool raisePerFrameLimit)
            : lifetimeSetting(nullptr),
              maxPerFrameSetting(nullptr),
              originalLifetime(0.0f),
              originalMaxPerFrame(0),
              changedLifetime(false),
              changedMaxPerFrame(false)
        {
            if (!s_settings.enabled)
                return;

            if (extendLifetime && s_settings.setGameDecalLifetime)
            {
                lifetimeSetting = ResolveDecalLifetimeSetting();
                if (lifetimeSetting)
                {
                    originalLifetime = lifetimeSetting->f;
                    if (lifetimeSetting->f < s_settings.decalLifetimeSeconds)
                    {
                        lifetimeSetting->f = s_settings.decalLifetimeSeconds;
                        changedLifetime = true;
                    }
                }
            }

            if (raisePerFrameLimit && s_settings.setGameDecalLimit)
            {
                maxPerFrameSetting = ResolveMaxDecalsPerFrameSetting();
                if (maxPerFrameSetting)
                {
                    originalMaxPerFrame = ReadIntegerSettingValue(maxPerFrameSetting);
                    if (IntegerSettingValueIsBelow(maxPerFrameSetting, s_settings.maxDecalsPerFrame))
                    {
                        WriteIntegerSettingValue(maxPerFrameSetting, s_settings.maxDecalsPerFrame);
                        changedMaxPerFrame = true;
                    }
                }
            }
        }

        ~ScopedDecalCreationSettings()
        {
            if (maxPerFrameSetting && changedMaxPerFrame)
                WriteIntegerSettingValue(maxPerFrameSetting, originalMaxPerFrame);

            if (lifetimeSetting && changedLifetime)
                lifetimeSetting->f = originalLifetime;
        }
    };

    static const char* ResolveBloodDecalTexturePath(Actor* actor)
    {
        if (!actor)
            return nullptr;

        const char* result = nullptr;

        __try
        {
            // Native hit blood first applies the combined effects gate, then
            // independently asks the actor base for its decal texture.  The
            // creature implementation returns null when its no-decal setting
            // is set, even if a separate configured spray/particle is allowed.
            if (!ActorShouldEmitBloodEffects(actor))
                return nullptr;

            const char* path = ActorGetBloodDecalTexturePath(actor);
            result = (path && path[0]) ? path : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            _ERROR("BloodOnDeath blood eligibility/texture lookup faulted for ref=0x%08X", actor->refID);
            result = nullptr;
        }

        return result;
    }

    static bool HasNativeBloodDecalProfile(Actor* actor)
    {
        return ResolveBloodDecalTexturePath(actor) != nullptr;
    }

    static Actor* LookupActor(UInt32 refId)
    {
        TESForm* form = LookupFormByID(refId);
        if (!form)
            return nullptr;

        return OBLIVION_CAST(form, TESForm, Actor);
    }

    static bool ResolveActorWorldPosition(Actor* actor, float& x, float& y, float& z)
    {
        if (!actor)
            return false;

        NiNode* root = actor->GetNiNode();
        if (root && IsFinitePoint(root->m_worldTranslate.x, root->m_worldTranslate.y, root->m_worldTranslate.z))
        {
            x = root->m_worldTranslate.x;
            y = root->m_worldTranslate.y;
            z = root->m_worldTranslate.z;
        }
        else
        {
            x = actor->posX;
            y = actor->posY;
            z = actor->posZ;
        }

        return IsFinitePoint(x, y, z);
    }

    static bool ResolveReferenceWorldPosition(TESObjectREFR* ref, float& x, float& y, float& z)
    {
        if (!ref)
            return false;

        NiNode* root = ref->GetNiNode();
        if (root && IsFinitePoint(root->m_worldTranslate.x, root->m_worldTranslate.y, root->m_worldTranslate.z))
        {
            x = root->m_worldTranslate.x;
            y = root->m_worldTranslate.y;
            z = root->m_worldTranslate.z;
        }
        else
        {
            x = ref->posX;
            y = ref->posY;
            z = ref->posZ;
        }

        return IsFinitePoint(x, y, z);
    }

    static bool ResolveActorReferencePosition(Actor* actor, float& x, float& y, float& z)
    {
        if (!actor)
            return false;

        if (IsFinitePoint(actor->posX, actor->posY, actor->posZ))
        {
            x = actor->posX;
            y = actor->posY;
            z = actor->posZ;
            return true;
        }

        return ResolveActorWorldPosition(actor, x, y, z);
    }

    static NiAVObject* FindNamedAVObject(NiNode* root, const char* name)
    {
        if (!root || !name || !name[0])
            return nullptr;

        NiObjectNET* foundByVirtual = root->GetObject(name);
        return foundByVirtual ? static_cast<NiAVObject*>(foundByVirtual) : nullptr;
    }

    static bool ResolveNamedBonePosition(NiNode* root, const char* name, float& x, float& y, float& z)
    {
        NiAVObject* bone = FindNamedAVObject(root, name);
        if (!bone || !IsFinitePoint(bone->m_worldTranslate.x, bone->m_worldTranslate.y, bone->m_worldTranslate.z))
            return false;

        x = bone->m_worldTranslate.x;
        y = bone->m_worldTranslate.y;
        z = bone->m_worldTranslate.z;
        return true;
    }

    static bool ResolveFallbackLimbPosition(Actor* actor, const LimbSectionDef& section, float& x, float& y, float& z)
    {
        float baseX = 0.0f;
        float baseY = 0.0f;
        float baseZ = 0.0f;
        if (!ResolveActorReferencePosition(actor, baseX, baseY, baseZ))
            return false;

        const float heading = actor ? actor->rotZ : 0.0f;
        const float forwardX = std::cos(heading);
        const float forwardY = std::sin(heading);
        const float rightX = std::cos(heading + 1.570796327f);
        const float rightY = std::sin(heading + 1.570796327f);

        x = baseX + (rightX * section.rightOffset) + (forwardX * section.forwardOffset);
        y = baseY + (rightY * section.rightOffset) + (forwardY * section.forwardOffset);
        z = baseZ + section.heightOffset;
        return IsFinitePoint(x, y, z);
    }

    static bool ResolveLimbSectionPosition(Actor* actor, NiNode* root, const LimbSectionDef& section, float& x, float& y, float& z)
    {
        if (root)
        {
            for (UInt32 i = 0; i < kMaxBoneNamesPerLimbSection; ++i)
            {
                const char* boneName = section.boneNames[i];
                if (boneName && ResolveNamedBonePosition(root, boneName, x, y, z))
                    return true;
            }
        }

        if (section.allowFallback)
            return ResolveFallbackLimbPosition(actor, section, x, y, z);

        return false;
    }

    static TESObjectREFR* GetNativePlayerCharacterSingleton()
    {
        return *reinterpret_cast<TESObjectREFR**>(kNativePlayerCharacterSingleton);
    }

    static bool IsNativePlayerCharacterReference(TESObjectREFR* ref)
    {
        TESObjectREFR* player = GetNativePlayerCharacterSingleton();
        return ref && player && ref == player;
    }

    static bool CanUseActor(Actor* actor)
    {
        if (!s_settings.enabled || !actor)
            return false;

        // Actor_HandleHitVisualEffects exits before all hit visuals when the
        // reference carries ExtraGhost (0x005EE7BD through 0x005EE7C4).
        if (actor->baseExtraList.HasType(kExtraData_Ghost))
            return false;

        if (!s_settings.includePlayer && IsNativePlayerCharacterReference(actor))
            return false;

        return true;
    }

    static bool HasLoadedBloodGeometry(Actor* actor)
    {
        // A parent cell can remain assigned after the actor's 3D is unloaded.
        return actor && actor->parentCell && actor->GetNiNode();
    }

    static bool ShouldStartTrackingActor(Actor* actor)
    {
        return CanUseActor(actor) && actor->DeadState != 0 && HasNativeBloodDecalProfile(actor);
    }

    static bool ShouldContinueTrackingActor(Actor* actor)
    {
        return CanUseActor(actor) && actor->DeadState != 0 && HasNativeBloodDecalProfile(actor);
    }

    static bool ShouldTrackWoundedActor(Actor* actor)
    {
        return CanUseActor(actor) && actor->DeadState == 0 && HasNativeBloodDecalProfile(actor);
    }

    static bool NativeHitBloodPerformanceGateAllows(TESObjectREFR* actor, TESObjectREFR* source)
    {
        const UInt8 gateFlag = *reinterpret_cast<const UInt8*>(kNativeHitBloodGateFlag);
        if (gateFlag == 0)
        {
            const SInt32 playerDistanceCheckField14 =
                *reinterpret_cast<const SInt32*>(kNativePlayerDistanceCheckControllerField14);
            const SInt32 maxHiPerfCombatCount =
                *reinterpret_cast<const SInt32*>(kGameSettingMaxHiPerfCombatCountCombat);
            if (playerDistanceCheckField14 <= maxHiPerfCombatCount)
                return true;
        }

        return IsNativePlayerCharacterReference(actor) || IsNativePlayerCharacterReference(source);
    }

    static bool ShouldStartWoundTrailForHit(Actor* actor, TESObjectREFR* source, float damage)
    {
        if (!ShouldTrackWoundedActor(actor))
            return false;

        // Ordered strict comparison matches the native FCOMPP/status-word
        // branch: equal or unordered operands suppress blood.
        if (!(damage > GetNativeBloodDamageThreshold()) || !NativeHitBloodPerformanceGateAllows(actor, source))
            return false;

        // After the damage/performance gates, native hit blood requires both
        // references to have loaded 3D (0x005EEDF1 through 0x005EEE13).
        return source && actor->GetNiNode() && source->GetNiNode();
    }

    static bool ResolveHitImpulseDirection(Actor* actor, TESObjectREFR* source, float& directionX, float& directionY)
    {
        if (!actor || !source || source == actor)
            return false;

        float actorX = 0.0f;
        float actorY = 0.0f;
        float actorZ = 0.0f;
        float sourceX = 0.0f;
        float sourceY = 0.0f;
        float sourceZ = 0.0f;
        if (!ResolveActorWorldPosition(actor, actorX, actorY, actorZ) ||
            !ResolveReferenceWorldPosition(source, sourceX, sourceY, sourceZ))
        {
            return false;
        }

        directionX = actorX - sourceX;
        directionY = actorY - sourceY;
        const float lengthSq = (directionX * directionX) + (directionY * directionY);
        if (!IsFinite(lengthSq) || lengthSq < 16.0f)
            return false;

        const float invLength = 1.0f / std::sqrt(lengthSq);
        directionX *= invLength;
        directionY *= invLength;
        return IsFinite(directionX) && IsFinite(directionY);
    }

    static float ResolveFloorProjectionZ(Actor* actor, float referenceZ)
    {
        if (actor && IsFinite(actor->posZ))
            return actor->posZ;

        return referenceZ;
    }

    static void PreserveOrInitializeLimbMotion(
        LeakTracker::LimbAnchor& limb,
        const LeakTracker::LimbAnchor* previousLimbs,
        UInt32 previousCount)
    {
        const UInt32 sectionIndex = limb.sectionIndex;
        for (UInt32 i = 0; previousLimbs && i < previousCount && i < kMaxLimbLeakSections; ++i)
        {
            if (previousLimbs[i].active &&
                previousLimbs[i].sectionIndex == sectionIndex &&
                IsFinitePoint(previousLimbs[i].lastMoveX, previousLimbs[i].lastMoveY, previousLimbs[i].lastMoveZ))
            {
                limb.lastMoveX = previousLimbs[i].lastMoveX;
                limb.lastMoveY = previousLimbs[i].lastMoveY;
                limb.lastMoveZ = previousLimbs[i].lastMoveZ;
                return;
            }
        }

        limb.lastMoveX = limb.x;
        limb.lastMoveY = limb.y;
        limb.lastMoveZ = limb.z;
    }

    static void ProjectBloodLikeHitDecal(
        TESObjectREFR* ref,
        const char* texturePath,
        float x,
        float y,
        float z,
        float directionX,
        float directionY,
        float directionZ,
        TESObjectREFR* explicitTarget)
    {
        if (!ref || !ref->parentCell || !texturePath || !texturePath[0])
            return;

        if (!IsFinitePoint(x, y, z) || !IsFinitePoint(directionX, directionY, directionZ))
            return;

        // Matches Actor_HandleHitVisualEffects: position vector, direction vector,
        // blood texture, optional explicit target, selector -1, alternate geometry 0.
        ScopedDecalCreationSettings decalSettings(true, true);
        DecalProjectToSceneGeometry(
            ref->parentCell,
            x,
            y,
            z,
            directionX,
            directionY,
            directionZ,
            texturePath,
            explicitTarget,
            -1,
            0);
    }

    static float ClampDecalSourceZToNativeRay(float sourceZ, float floorZ)
    {
        const float minimumSourceZ = floorZ + 4.0f;
        const float fallbackSourceZ = floorZ + s_settings.projectionHeight;
        const float maximumSourceZ = floorZ + (kNativeDecalProjectionRayLength - 2.0f);

        if (!IsFinite(sourceZ) || sourceZ < minimumSourceZ)
            sourceZ = fallbackSourceZ;

        if (sourceZ > maximumSourceZ)
            sourceZ = maximumSourceZ;

        if (sourceZ < minimumSourceZ)
            sourceZ = minimumSourceZ;

        return sourceZ;
    }

    static void ProjectLimbBloodToGroundLikeHitDecal(
        Actor* actor,
        const char* texturePath,
        float limbX,
        float limbY,
        float limbZ,
        float floorZ,
        float targetX,
        float targetY)
    {
        if (!actor || !IsFinitePoint(limbX, limbY, limbZ) || !IsFinitePoint(targetX, targetY, floorZ))
            return;

        const float sourceZ = ClampDecalSourceZToNativeRay(limbZ, floorZ);
        const float usableRayLength = kNativeDecalProjectionRayLength - 2.0f;
        const float verticalDistance = sourceZ - floorZ;

        float directionX = targetX - limbX;
        float directionY = targetY - limbY;
        float directionZ = floorZ - sourceZ;

        const float horizontalSq = (directionX * directionX) + (directionY * directionY);
        const float maxHorizontalSq = (usableRayLength * usableRayLength) - (verticalDistance * verticalDistance);
        if (horizontalSq > 0.0001f && maxHorizontalSq > 0.0001f && horizontalSq > maxHorizontalSq)
        {
            const float scale = std::sqrt(maxHorizontalSq) / std::sqrt(horizontalSq);
            directionX *= scale;
            directionY *= scale;
        }
        else if (maxHorizontalSq <= 0.0001f)
        {
            directionX = 0.0f;
            directionY = 0.0f;
        }

        if (!NormalizeVector(directionX, directionY, directionZ))
        {
            directionX = 0.0f;
            directionY = 0.0f;
            directionZ = -1.0f;
        }

        ProjectBloodLikeHitDecal(
            actor,
            texturePath,
            limbX,
            limbY,
            sourceZ,
            directionX,
            directionY,
            directionZ,
            nullptr);
    }

    static void ProjectBloodSpatterToGeometryLikeHitDecal(
        Actor* actor,
        const char* texturePath,
        float limbX,
        float limbY,
        float limbZ,
        float floorZ,
        float targetBaseX,
        float targetBaseY,
        UInt32 seed,
        float radiusScale)
    {
        const float angle = UnitHash(seed + 11u) * 6.283185307f;
        const float offset = s_settings.spillRadius * radiusScale * (0.15f + 0.85f * UnitHash(seed + 53u));
        const float targetX = targetBaseX + (std::cos(angle) * offset);
        const float targetY = targetBaseY + (std::sin(angle) * offset);

        ProjectLimbBloodToGroundLikeHitDecal(
            actor,
            texturePath,
            limbX,
            limbY,
            limbZ,
            floorZ,
            targetX,
            targetY);
    }

    static void ProjectOutwardBloodSpurtFromCenterLikeHitDecal(
        Actor* actor,
        const char* texturePath,
        float limbX,
        float limbY,
        float limbZ,
        float floorZ,
        float centerX,
        float centerY,
        bool hasCenter,
        bool hasPreferredDirection,
        float preferredDirectionX,
        float preferredDirectionY,
        UInt32 seed,
        float radiusScale)
    {
        if (!actor || !texturePath || !texturePath[0])
            return;
        if (!IsFinitePoint(limbX, limbY, limbZ) || !IsFinite(floorZ))
            return;

        const float sourceZ = ClampDecalSourceZToNativeRay(limbZ, floorZ);
        const float verticalDrop = sourceZ - floorZ;
        const float usableRayLength = kNativeDecalProjectionRayLength - 2.0f;
        const float downwardBias = ClampFloat((verticalDrop / usableRayLength) + 0.10f, 0.24f, 0.86f);
        const float horizontalBias = std::sqrt(ClampFloat(1.0f - (downwardBias * downwardBias), 0.01f, 1.0f));

        float baseAngle = actor->rotZ;
        float centerZ = 0.0f;
        const float preferredSq = (preferredDirectionX * preferredDirectionX) + (preferredDirectionY * preferredDirectionY);
        if (hasPreferredDirection && IsFinite(preferredSq) && preferredSq > 0.0001f)
        {
            baseAngle = std::atan2(preferredDirectionY, preferredDirectionX);
        }
        else if (hasCenter || ResolveActorWorldPosition(actor, centerX, centerY, centerZ))
        {
            const float outwardX = limbX - centerX;
            const float outwardY = limbY - centerY;
            const float outwardSq = (outwardX * outwardX) + (outwardY * outwardY);
            if (IsFinite(outwardSq) && outwardSq > 16.0f)
                baseAngle = std::atan2(outwardY, outwardX);
        }

        const float spreadScale = ClampFloat(radiusScale, 0.15f, 1.0f);
        const float spread = (UnitHash(seed + 197u) - 0.5f) * (1.6f + spreadScale);
        const float angle = baseAngle + spread;

        float directionX = std::cos(angle) * horizontalBias;
        float directionY = std::sin(angle) * horizontalBias;
        float directionZ = -downwardBias;
        if (!NormalizeVector(directionX, directionY, directionZ))
            return;

        ProjectBloodLikeHitDecal(
            actor,
            texturePath,
            limbX,
            limbY,
            sourceZ,
            directionX,
            directionY,
            directionZ,
            nullptr);
    }

    static void ProjectOutwardBloodSpurtLikeHitDecal(
        Actor* actor,
        const char* texturePath,
        float limbX,
        float limbY,
        float limbZ,
        float floorZ,
        UInt32 seed,
        float radiusScale)
    {
        ProjectOutwardBloodSpurtFromCenterLikeHitDecal(
            actor,
            texturePath,
            limbX,
            limbY,
            limbZ,
            floorZ,
            0.0f,
            0.0f,
            false,
            false,
            0.0f,
            0.0f,
            seed,
            radiusScale);
    }

    static UInt32 ProjectOutwardBloodSpurtFanFromCenterLikeHitDecal(
        Actor* actor,
        const char* texturePath,
        float limbX,
        float limbY,
        float limbZ,
        float floorZ,
        float centerX,
        float centerY,
        bool hasCenter,
        bool hasPreferredDirection,
        float preferredDirectionX,
        float preferredDirectionY,
        UInt32 seedBase,
        float radiusScale,
        UInt32 maxSpurts)
    {
        UInt32 spawned = 0;
        while (spawned < s_settings.outwardBloodSpurtsPerLimbSection && spawned < maxSpurts)
        {
            ProjectOutwardBloodSpurtFromCenterLikeHitDecal(
                actor,
                texturePath,
                limbX,
                limbY,
                limbZ,
                floorZ,
                centerX,
                centerY,
                hasCenter,
                hasPreferredDirection,
                preferredDirectionX,
                preferredDirectionY,
                seedBase ^ ((spawned + 1) * 0x6D2B79F5u),
                radiusScale);
            ++spawned;
        }

        return spawned;
    }

    static UInt32 ResolveLimbAnchors(Actor* actor, LeakTracker& tracker)
    {
        LeakTracker::LimbAnchor previousLimbs[kMaxLimbLeakSections] = {};
        const UInt32 previousCount = tracker.limbCount;
        for (UInt32 i = 0; i < previousCount && i < kMaxLimbLeakSections; ++i)
            previousLimbs[i] = tracker.limbs[i];

        tracker.limbCount = 0;
        if (!actor)
            return 0;

        // Resolve once for this snapshot; never retain scene pointers across frames.
        NiNode* root = actor->GetNiNode();
        for (UInt32 i = 0; i < kMaxLimbLeakSections; ++i)
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            if (!ResolveLimbSectionPosition(actor, root, kLimbSections[i], x, y, z))
                continue;

            LeakTracker::LimbAnchor& limb = tracker.limbs[tracker.limbCount];
            limb.active = true;
            limb.sectionIndex = i;
            limb.x = x;
            limb.y = y;
            limb.z = z;
            limb.floorZ = ResolveFloorProjectionZ(actor, z);
            PreserveOrInitializeLimbMotion(limb, previousLimbs, previousCount);
            ++tracker.limbCount;
        }

        if (!tracker.limbCount)
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            if (ResolveActorReferencePosition(actor, x, y, z))
            {
                LeakTracker::LimbAnchor& limb = tracker.limbs[0];
                limb.active = true;
                limb.sectionIndex = kMaxLimbLeakSections;
                limb.x = x;
                limb.y = y;
                limb.z = z;
                limb.floorZ = ResolveFloorProjectionZ(actor, z);
                PreserveOrInitializeLimbMotion(limb, previousLimbs, previousCount);
                tracker.limbCount = 1;
            }
        }

        return tracker.limbCount;
    }

    static UInt32 SpawnBloodAtLimbAnchors(
        Actor* actor,
        const LeakTracker& tracker,
        UInt32 maxSpatters,
        UInt32 seedBase,
        float radiusScale,
        bool hasSpurtCenter,
        float spurtCenterX,
        float spurtCenterY,
        bool hasSpurtDirection,
        float spurtDirectionX,
        float spurtDirectionY)
    {
        if (!actor || !actor->parentCell || !tracker.limbCount)
            return 0;
        if (maxSpatters == 0)
            return 0;

        const char* texturePath = ResolveBloodDecalTexturePath(actor);
        if (!texturePath || !texturePath[0])
            return 0;

        UInt32 spawned = 0;
        // First pass preserves one floor mark from every limb under tight caps.
        for (UInt32 i = 0; i < tracker.limbCount && i < kMaxLimbLeakSections; ++i)
        {
            const LeakTracker::LimbAnchor& limb = tracker.limbs[i];
            if (!limb.active || !IsFinitePoint(limb.x, limb.y, limb.z))
                continue;
            if (spawned >= maxSpatters)
                break;

            ProjectBloodSpatterToGeometryLikeHitDecal(
                actor,
                texturePath,
                limb.x,
                limb.y,
                limb.z,
                limb.floorZ,
                limb.x,
                limb.y,
                seedBase ^ ((i + 1) * 0x9E3779B9u),
                radiusScale);
            ++spawned;
        }

        for (UInt32 i = 0; i < tracker.limbCount && i < kMaxLimbLeakSections; ++i)
        {
            const LeakTracker::LimbAnchor& limb = tracker.limbs[i];
            if (!limb.active || !IsFinitePoint(limb.x, limb.y, limb.z))
                continue;
            if (spawned >= maxSpatters)
                break;

            spawned += ProjectOutwardBloodSpurtFanFromCenterLikeHitDecal(
                actor,
                texturePath,
                limb.x,
                limb.y,
                limb.z,
                limb.floorZ,
                spurtCenterX,
                spurtCenterY,
                hasSpurtCenter,
                hasSpurtDirection,
                spurtDirectionX,
                spurtDirectionY,
                seedBase ^ ((i + 1) * 0xA24BAED5u),
                radiusScale,
                maxSpatters - spawned);
        }

        return spawned;
    }

    static UInt32 SpawnInitialLimbSpray(Actor* actor, LeakTracker& tracker, UInt32 maxSpatters)
    {
        if (!actor || !actor->parentCell)
            return 0;

        ResolveLimbAnchors(actor, tracker);
        return SpawnBloodAtLimbAnchors(
            actor,
            tracker,
            maxSpatters,
            tracker.refId ^ ((tracker.cycleIndex + 1) * 0x7F4A7C15u),
            s_settings.corpseMoveRadiusScale,
            false,
            0.0f,
            0.0f,
            false,
            0.0f,
            0.0f);
    }

    static UInt32 SpawnActorLimbBloodClusterAtPosition(
        Actor* actor,
        UInt32 maxSpatters,
        UInt32 seedBase,
        float radiusScale,
        float rootX,
        float rootY,
        float rootZ,
        bool hasSpurtDirection,
        float spurtDirectionX,
        float spurtDirectionY)
    {
        if (!actor || maxSpatters == 0 || !IsFinitePoint(rootX, rootY, rootZ))
            return 0;

        float currentX = 0.0f;
        float currentY = 0.0f;
        float currentZ = 0.0f;
        if (!ResolveActorWorldPosition(actor, currentX, currentY, currentZ))
            return 0;

        LeakTracker scratch = {};
        scratch.refId = actor->refID;
        ResolveLimbAnchors(actor, scratch);

        const float offsetX = rootX - currentX;
        const float offsetY = rootY - currentY;
        const float offsetZ = rootZ - currentZ;
        for (UInt32 i = 0; i < scratch.limbCount && i < kMaxLimbLeakSections; ++i)
        {
            LeakTracker::LimbAnchor& limb = scratch.limbs[i];
            if (!limb.active)
                continue;

            limb.x += offsetX;
            limb.y += offsetY;
            limb.z += offsetZ;
            limb.floorZ += offsetZ;
        }

        return SpawnBloodAtLimbAnchors(
            actor,
            scratch,
            maxSpatters,
            seedBase,
            radiusScale,
            true,
            rootX,
            rootY,
            hasSpurtDirection,
            spurtDirectionX,
            spurtDirectionY);
    }

    static UInt32 SpawnMovedLimbTrailCluster(
        Actor* actor,
        const char* texturePath,
        float limbX,
        float limbY,
        float limbZ,
        float floorZ,
        float directionX,
        float directionY,
        UInt32 seedBase,
        float radiusScale,
        UInt32 maxSpatters)
    {
        if (!actor || !texturePath || !texturePath[0] || maxSpatters == 0)
            return 0;

        UInt32 spawned = 0;
        ProjectBloodSpatterToGeometryLikeHitDecal(
            actor,
            texturePath,
            limbX,
            limbY,
            limbZ,
            floorZ,
            limbX,
            limbY,
            seedBase ^ 0xD1B54A35u,
            radiusScale);
        ++spawned;

        if (spawned >= maxSpatters)
            return spawned;

        spawned += ProjectOutwardBloodSpurtFanFromCenterLikeHitDecal(
            actor,
            texturePath,
            limbX,
            limbY,
            limbZ,
            floorZ,
            0.0f,
            0.0f,
            false,
            true,
            directionX,
            directionY,
            seedBase ^ 0xA24BAED5u,
            radiusScale,
            maxSpatters - spawned);

        return spawned;
    }

    static void ResetLeakCycle(LeakTracker& tracker, UInt32 now, float x, float y, float z)
    {
        tracker.stableSinceTick = now;
        tracker.leakStartTick = 0;
        tracker.lastPulseTick = 0;
        tracker.leakStarted = false;
        tracker.hasPulseTime = false;
        tracker.pulseIndex = 0;
        tracker.splatterCount = 0;
        tracker.completed = false;
        tracker.initialPulsePending = true;
        tracker.hasPosition = true;
        tracker.lastX = x;
        tracker.lastY = y;
        tracker.lastZ = z;
        tracker.anchorX = x;
        tracker.anchorY = y;
        tracker.anchorZ = z;
        tracker.anchorFloorZ = z;
        tracker.limbCount = 0;
        for (UInt32 i = 0; i < kMaxLimbLeakSections; ++i)
        {
            tracker.limbs[i].active = false;
            tracker.limbs[i].sectionIndex = i;
            tracker.limbs[i].lastMoveX = x;
            tracker.limbs[i].lastMoveY = y;
            tracker.limbs[i].lastMoveZ = z;
        }
    }

    static void ReanchorLeakCycle(LeakTracker& tracker, UInt32 now, float x, float y, float z)
    {
        const UInt32 spent = tracker.splatterCount;
        const bool completed = tracker.completed;
        ResetLeakCycle(tracker, now, x, y, z);
        tracker.splatterCount = spent;
        tracker.completed = completed || spent >= s_settings.maxSplatterDecals;
        if (spent >= s_settings.maxSplatterDecals)
            tracker.initialPulsePending = false;
    }

    static void BeginLeakCycle(Actor* actor, LeakTracker& tracker, UInt32 now, float x, float y, float z)
    {
        tracker.leakStartTick = now;
        tracker.lastPulseTick = 0;
        tracker.leakStarted = true;
        tracker.hasPulseTime = false;
        tracker.pulseIndex = 0;
        tracker.completed = false;
        tracker.initialPulsePending = false;
        tracker.anchorX = x;
        tracker.anchorY = y;
        tracker.anchorZ = z;
        tracker.anchorFloorZ = ResolveFloorProjectionZ(actor, z);
        ResolveLimbAnchors(actor, tracker);
        ++tracker.cycleIndex;

        if (s_settings.logSpawns)
        {
            _MESSAGE(
                "BloodOnDeath leak started: actor=0x%08X cycle=%u anchor=(%.2f, %.2f, %.2f) floorZ=%.2f limbs=%u",
                tracker.refId,
                tracker.cycleIndex,
                tracker.anchorX,
                tracker.anchorY,
                tracker.anchorZ,
                tracker.anchorFloorZ,
                tracker.limbCount);
        }
    }

    static void SpawnLeakPulse(Actor* actor, LeakTracker& tracker, UInt32 now)
    {
        if (tracker.pulseIndex >= s_settings.totalDecals)
            return;
        if (tracker.splatterCount >= s_settings.maxSplatterDecals)
            return;

        const char* texturePath = ResolveBloodDecalTexturePath(actor);
        if (!texturePath || !texturePath[0])
            return;

        const UInt32 elapsed = ElapsedMs(now, tracker.leakStartTick);
        float t = static_cast<float>(elapsed) / static_cast<float>(s_settings.leakDurationMs);
        t = ClampFloat(t, 0.0f, 1.0f);

        const UInt32 seed = tracker.refId ^
            (tracker.cycleIndex * 0x85EBCA6Bu) ^
            (tracker.pulseIndex * 0x9E3779B9u);

        UInt32 spawned = 0;
        for (UInt32 i = 0; i < tracker.limbCount && i < kMaxLimbLeakSections; ++i)
        {
            const LeakTracker::LimbAnchor& limb = tracker.limbs[i];
            if (!limb.active || !IsFinitePoint(limb.x, limb.y, limb.z))
                continue;

            float x = limb.x;
            float y = limb.y;
            if (tracker.pulseIndex > 0)
            {
                const UInt32 limbSeed = seed ^ ((i + 1) * 0xD168AAADu);
                const float angle = UnitHash(limbSeed + 17u) * 6.283185307f;
                const float poolGrowth = std::sqrt(ClampFloat(t, 0.05f, 1.0f));
                const float radius = s_settings.spillRadius * poolGrowth * (0.15f + 0.85f * UnitHash(limbSeed + 31u));
                const float forwardT = t * UnitHash(limbSeed + 47u);
                const float heading = actor ? actor->rotZ : 0.0f;

                x += std::cos(angle) * radius;
                y += std::sin(angle) * radius;
                if (s_settings.leakLength > 0.0f)
                {
                    x += std::cos(heading) * s_settings.leakLength * forwardT;
                    y += std::sin(heading) * s_settings.leakLength * forwardT;
                }
            }

            if (IsFinitePoint(x, y, limb.z))
            {
                ProjectBloodSpatterToGeometryLikeHitDecal(
                    actor,
                    texturePath,
                    limb.x,
                    limb.y,
                    limb.z,
                    limb.floorZ,
                    x,
                    y,
                    seed ^ ((i + 1) * 0xD1B54A35u),
                    0.25f + (0.75f * t));
                ++spawned;
                ++tracker.splatterCount;

                const UInt32 outwardSpawned = ProjectOutwardBloodSpurtFanFromCenterLikeHitDecal(
                    actor,
                    texturePath,
                    limb.x,
                    limb.y,
                    limb.z,
                    limb.floorZ,
                    0.0f,
                    0.0f,
                    false,
                    false,
                    0.0f,
                    0.0f,
                    seed ^ ((i + 1) * 0xA24BAED5u),
                    0.25f + (0.75f * t),
                    RemainingCapacity(tracker.splatterCount, s_settings.maxSplatterDecals));
                spawned += outwardSpawned;
                tracker.splatterCount += outwardSpawned;

                if (tracker.splatterCount >= s_settings.maxSplatterDecals)
                    break;
            }
        }

        if (spawned)
        {
            tracker.lastPulseTick = now;
            tracker.hasPulseTime = true;
            ++tracker.pulseIndex;
        }

        if (tracker.splatterCount >= s_settings.maxSplatterDecals && !tracker.completed)
        {
            tracker.completed = true;
            if (s_settings.logSpawns)
            {
                _MESSAGE(
                    "BloodOnDeath leak reached splatter cap: actor=0x%08X splatters=%u cap=%u",
                    tracker.refId,
                    tracker.splatterCount,
                    s_settings.maxSplatterDecals);
            }
        }
    }

    static UInt32 SpawnMovedCorpseLimbBlood(Actor* actor, LeakTracker& tracker, UInt32 now)
    {
        if (!actor || !actor->parentCell)
            return 0;

        const UInt32 available = RemainingCapacity(tracker.splatterCount, s_settings.maxSplatterDecals);
        if (!available)
            return 0;

        ResolveLimbAnchors(actor, tracker);
        if (!tracker.limbCount)
            return 0;

        const char* texturePath = ResolveBloodDecalTexturePath(actor);
        if (!texturePath || !texturePath[0])
            return 0;

        const float moveDistanceSq = s_settings.corpseMoveDropDistance * s_settings.corpseMoveDropDistance;
        const UInt32 seed = tracker.refId ^
            (tracker.cycleIndex * 0x85EBCA6Bu) ^
            (tracker.splatterCount * 0x9E3779B9u) ^
            Mix(now);

        UInt32 spawned = 0;
        for (UInt32 i = 0; i < tracker.limbCount && i < kMaxLimbLeakSections; ++i)
        {
            LeakTracker::LimbAnchor& limb = tracker.limbs[i];
            if (!limb.active ||
                !IsFinitePoint(limb.x, limb.y, limb.z) ||
                !IsFinitePoint(limb.lastMoveX, limb.lastMoveY, limb.lastMoveZ))
            {
                continue;
            }

            if (DistanceSquared(limb.x, limb.y, limb.z, limb.lastMoveX, limb.lastMoveY, limb.lastMoveZ) < moveDistanceSq)
                continue;

            float cursorX = limb.lastMoveX;
            float cursorY = limb.lastMoveY;
            float cursorZ = limb.lastMoveZ;
            UInt32 dropsForLimb = 0;

            while (dropsForLimb < s_settings.maxCorpseMovementDropsPerLimbUpdate &&
                spawned < available &&
                tracker.splatterCount < s_settings.maxSplatterDecals)
            {
                const float deltaX = limb.x - cursorX;
                const float deltaY = limb.y - cursorY;
                const float deltaZ = limb.z - cursorZ;
                const float distanceSq = (deltaX * deltaX) + (deltaY * deltaY) + (deltaZ * deltaZ);
                if (!IsFinite(distanceSq) || distanceSq < moveDistanceSq)
                    break;

                const float distance = std::sqrt(distanceSq);
                if (!IsFinite(distance) || distance < 0.0001f)
                    break;

                const float ratio = s_settings.corpseMoveDropDistance / distance;
                const float dropX = cursorX + (deltaX * ratio);
                const float dropY = cursorY + (deltaY * ratio);
                const float dropZ = cursorZ + (deltaZ * ratio);
                const float horizontalSq = (deltaX * deltaX) + (deltaY * deltaY);
                float directionX = 0.0f;
                float directionY = 0.0f;
                if (IsFinite(horizontalSq) && horizontalSq > 0.0001f)
                {
                    const float invHorizontal = 1.0f / std::sqrt(horizontalSq);
                    directionX = deltaX * invHorizontal;
                    directionY = deltaY * invHorizontal;
                }
                else
                {
                    directionX = std::cos(actor->rotZ);
                    directionY = std::sin(actor->rotZ);
                }

                const UInt32 spawnedForDrop = SpawnMovedLimbTrailCluster(
                    actor,
                    texturePath,
                    dropX,
                    dropY,
                    dropZ,
                    ResolveFloorProjectionZ(actor, dropZ),
                    directionX,
                    directionY,
                    seed ^ ((i + 1) * 0x9E3779B9u) ^ ((dropsForLimb + 1) * 0x6D2B79F5u),
                    s_settings.corpseMoveRadiusScale,
                    available - spawned);
                if (!spawnedForDrop)
                    break;

                spawned += spawnedForDrop;
                tracker.splatterCount += spawnedForDrop;
                cursorX = dropX;
                cursorY = dropY;
                cursorZ = dropZ;
                ++dropsForLimb;
            }

            limb.lastMoveX = cursorX;
            limb.lastMoveY = cursorY;
            limb.lastMoveZ = cursorZ;

            if (spawned >= available || tracker.splatterCount >= s_settings.maxSplatterDecals)
                break;
        }

        if (spawned)
        {
            tracker.lastPulseTick = now;
            tracker.hasPulseTime = true;
            if (tracker.splatterCount >= s_settings.maxSplatterDecals)
                tracker.completed = true;
        }

        return spawned;
    }

    static bool UpdateLeakTracker(LeakTracker& tracker, UInt32 now)
    {
        Actor* actor = LookupActor(tracker.refId);
        if (!actor)
            return false;

        if (!ShouldContinueTrackingActor(actor))
            return false;

        if (!HasLoadedBloodGeometry(actor))
        {
            if (ElapsedMs(now, tracker.lastLoadedTick) > s_settings.unloadedTtlMs)
                return false;
            tracker.hasPosition = false;
            tracker.limbCount = 0;
            return true;
        }

        // Keep the spent budget until the tracker expires, without more bone work.
        if (tracker.splatterCount >= s_settings.maxSplatterDecals)
        {
            tracker.lastLoadedTick = now;
            tracker.completed = true;
            tracker.initialPulsePending = false;
            return true;
        }

        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (!ResolveActorWorldPosition(actor, x, y, z))
        {
            tracker.hasPosition = false;
            tracker.limbCount = 0;
            return ElapsedMs(now, tracker.lastLoadedTick) <= s_settings.unloadedTtlMs;
        }
        tracker.lastLoadedTick = now;

        if (!tracker.hasPosition)
        {
            ReanchorLeakCycle(tracker, now, x, y, z);
            return true;
        }

        if (tracker.initialPulsePending)
        {
            ResolveLimbAnchors(actor, tracker);
            const UInt32 available = RemainingCapacity(tracker.splatterCount, s_settings.maxSplatterDecals);
            const UInt32 initialSpawn = SpawnBloodAtLimbAnchors(
                actor,
                tracker,
                available,
                tracker.refId ^ ((tracker.cycleIndex + 1) * 0x7F4A7C15u),
                s_settings.corpseMoveRadiusScale,
                false,
                0.0f,
                0.0f,
                false,
                0.0f,
                0.0f);
            if (initialSpawn > 0)
            {
                tracker.splatterCount += initialSpawn;
                if (tracker.splatterCount >= s_settings.maxSplatterDecals)
                {
                    tracker.completed = true;
                    if (s_settings.logSpawns)
                    {
                        _MESSAGE(
                            "BloodOnDeath initial splatter reached cap: actor=0x%08X splatters=%u cap=%u",
                            tracker.refId,
                            tracker.splatterCount,
                            s_settings.maxSplatterDecals);
                    }
                }
            }
            tracker.initialPulsePending = false;
        }

        const float corpseMoveDistanceSq = s_settings.corpseMoveDropDistance * s_settings.corpseMoveDropDistance;
        if (DistanceSquared(x, y, z, tracker.anchorX, tracker.anchorY, tracker.anchorZ) > corpseMoveDistanceSq)
        {
            const UInt32 movedTrailSpawns = SpawnMovedCorpseLimbBlood(actor, tracker, now);
            ReanchorLeakCycle(tracker, now, x, y, z);
            tracker.initialPulsePending = false;

            if (tracker.splatterCount < s_settings.maxSplatterDecals)
            {
                BeginLeakCycle(actor, tracker, now, x, y, z);
                SpawnLeakPulse(actor, tracker, now);
            }

            if (s_settings.logSpawns)
            {
                _MESSAGE(
                    "BloodOnDeath corpse movement blood emitted: actor=0x%08X threshold=%.2f trailSplatters=%u limbs=%u splatters=%u",
                    tracker.refId,
                    s_settings.corpseMoveDropDistance,
                    movedTrailSpawns,
                    tracker.limbCount,
                    tracker.splatterCount);
            }
            return true;
        }

        const UInt32 movedLimbSpawns = SpawnMovedCorpseLimbBlood(actor, tracker, now);
        if (movedLimbSpawns && s_settings.logSpawns)
        {
            _MESSAGE(
                "BloodOnDeath moved corpse limb blood emitted: actor=0x%08X limbs=%u splatters=%u",
                tracker.refId,
                movedLimbSpawns,
                tracker.splatterCount);
        }

        if (tracker.leakStarted)
        {
            if (ElapsedMs(now, tracker.leakStartTick) <= s_settings.leakDurationMs)
            {
                if (tracker.pulseIndex < s_settings.totalDecals &&
                    (!tracker.hasPulseTime || ElapsedMs(now, tracker.lastPulseTick) >= GetPulseIntervalMs()))
                {
                    SpawnLeakPulse(actor, tracker, now);
                }
            }
            else if (!tracker.completed)
            {
                tracker.completed = true;
                if (s_settings.logSpawns)
                    _MESSAGE("BloodOnDeath leak completed: actor=0x%08X pulses=%u", tracker.refId, tracker.pulseIndex);
            }

            tracker.lastX = x;
            tracker.lastY = y;
            tracker.lastZ = z;
            return true;
        }

        const float settleDistanceSq = s_settings.settleMoveTolerance * s_settings.settleMoveTolerance;
        if (DistanceSquared(x, y, z, tracker.lastX, tracker.lastY, tracker.lastZ) > settleDistanceSq)
        {
            tracker.stableSinceTick = now;
            tracker.lastX = x;
            tracker.lastY = y;
            tracker.lastZ = z;
            return true;
        }

        tracker.lastX = x;
        tracker.lastY = y;
        tracker.lastZ = z;

        if (ElapsedMs(now, tracker.stableSinceTick) >= s_settings.settleMs)
        {
            BeginLeakCycle(actor, tracker, now, x, y, z);
            SpawnLeakPulse(actor, tracker, now);
        }

        return true;
    }

    static void TrackDeadActor(Actor* actor, bool restartCompletedCycle)
    {
        if (!ShouldStartTrackingActor(actor))
            return;

        const UInt32 now = GetTickCount();
        LeakTracker& tracker = s_leaks[actor->refID];
        if (!restartCompletedCycle && tracker.refId == actor->refID && tracker.completed)
            return;

        tracker.refId = actor->refID;
        tracker.lastLoadedTick = now;
        if (!tracker.cycleIndex || restartCompletedCycle)
            tracker.cycleIndex = 0;

        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (HasLoadedBloodGeometry(actor) && ResolveActorWorldPosition(actor, x, y, z))
        {
            ResetLeakCycle(tracker, now, x, y, z);
            const UInt32 available = RemainingCapacity(tracker.splatterCount, s_settings.maxSplatterDecals);
            const UInt32 initialSpatter = SpawnInitialLimbSpray(actor, tracker, available);
            tracker.splatterCount += initialSpatter;
            if (tracker.splatterCount >= s_settings.maxSplatterDecals)
            {
                tracker.completed = true;
                if (s_settings.logSpawns)
                {
                    _MESSAGE(
                        "BloodOnDeath initial splatter reached cap: actor=0x%08X splatters=%u cap=%u",
                        tracker.refId,
                        tracker.splatterCount,
                        s_settings.maxSplatterDecals);
                }
            }
            if (initialSpatter)
                tracker.initialPulsePending = false;
        }
        else
        {
            tracker.hasPosition = false;
            tracker.stableSinceTick = now;
            tracker.leakStartTick = 0;
            tracker.lastPulseTick = 0;
            tracker.leakStarted = false;
            tracker.hasPulseTime = false;
            tracker.pulseIndex = 0;
            tracker.splatterCount = 0;
            tracker.completed = false;
            tracker.initialPulsePending = true;
            tracker.limbCount = 0;
        }

        if (s_settings.logSpawns)
            _MESSAGE("BloodOnDeath tracked dead actor: actor=0x%08X", actor->refID);
    }

    static void StartCorpseBlood(Actor* actor, bool restartCompletedCycle, const char* reason)
    {
        if (!ShouldStartTrackingActor(actor))
            return;

        s_wounds.erase(actor->refID);
        TrackDeadActor(actor, restartCompletedCycle);

        if (s_settings.logSpawns)
        {
            _MESSAGE(
                "BloodOnDeath corpse blood queued: actor=0x%08X reason=%s",
                actor->refID,
                reason ? reason : "unknown");
        }
    }

    static LeakTracker* EnsureCorpseHitTracker(Actor* actor, UInt32 now)
    {
        if (!ShouldStartTrackingActor(actor))
            return nullptr;

        LeakTracker& tracker = s_leaks[actor->refID];
        const bool newTracker = tracker.refId != actor->refID;
        tracker.refId = actor->refID;
        if (newTracker)
            tracker.lastLoadedTick = now;

        if (!HasLoadedBloodGeometry(actor))
        {
            tracker.hasPosition = false;
            tracker.limbCount = 0;
            return nullptr;
        }
        if (tracker.splatterCount >= s_settings.maxSplatterDecals)
        {
            tracker.lastLoadedTick = now;
            return &tracker;
        }

        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (!ResolveActorWorldPosition(actor, x, y, z))
        {
            tracker.hasPosition = false;
            tracker.limbCount = 0;
            if (newTracker)
            {
                tracker.hasPosition = false;
                tracker.stableSinceTick = now;
                tracker.leakStartTick = 0;
                tracker.lastPulseTick = 0;
                tracker.leakStarted = false;
                tracker.hasPulseTime = false;
                tracker.pulseIndex = 0;
                tracker.splatterCount = 0;
                tracker.completed = false;
                tracker.initialPulsePending = true;
                tracker.limbCount = 0;
            }
            return nullptr;
        }
        tracker.lastLoadedTick = now;

        if (newTracker || !tracker.hasPosition)
        {
            ReanchorLeakCycle(tracker, now, x, y, z);
            const UInt32 available = RemainingCapacity(tracker.splatterCount, s_settings.maxSplatterDecals);
            const UInt32 initialSpatter = SpawnInitialLimbSpray(actor, tracker, available);
            tracker.splatterCount += initialSpatter;
            tracker.initialPulsePending = !initialSpatter;
        }
        else
        {
            tracker.hasPosition = true;
            tracker.lastX = x;
            tracker.lastY = y;
            tracker.lastZ = z;
            tracker.stableSinceTick = now;
            if (tracker.splatterCount < s_settings.maxSplatterDecals)
                tracker.completed = false;
        }

        return &tracker;
    }

    static void StartCorpseHitBlood(Actor* actor, TESObjectREFR* source, bool useSourceDirection, const char* reason)
    {
        if (!ShouldStartTrackingActor(actor))
            return;

        s_wounds.erase(actor->refID);

        const UInt32 now = GetTickCount();
        LeakTracker* tracker = EnsureCorpseHitTracker(actor, now);
        if (!tracker)
            return;

        const UInt32 available = RemainingCapacity(tracker->splatterCount, s_settings.maxSplatterDecals);
        if (!available)
            return;

        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (!ResolveActorWorldPosition(actor, x, y, z))
            return;

        float directionX = 0.0f;
        float directionY = 0.0f;
        const bool hasDirection = useSourceDirection && ResolveHitImpulseDirection(actor, source, directionX, directionY);
        const UInt32 spawned = SpawnActorLimbBloodClusterAtPosition(
            actor,
            available,
            tracker->refId ^ ((tracker->cycleIndex + 1) * 0xA24BAED5u) ^ Mix(now) ^ (tracker->splatterCount * 0x85EBCA6Bu),
            s_settings.corpseMoveRadiusScale,
            x,
            y,
            z,
            hasDirection,
            directionX,
            directionY);

        if (!spawned)
            return;

        tracker->splatterCount += spawned;
        tracker->lastPulseTick = now;
        tracker->hasPulseTime = true;
        tracker->initialPulsePending = false;
        if (tracker->splatterCount >= s_settings.maxSplatterDecals)
            tracker->completed = true;

        if (s_settings.logSpawns)
        {
            _MESSAGE(
                "BloodOnDeath corpse-hit blood emitted: actor=0x%08X reason=%s spawned=%u total=%u directed=%u",
                actor->refID,
                reason ? reason : "unknown",
                spawned,
                tracker->splatterCount,
                hasDirection ? 1 : 0);
        }
    }

    static UInt32 SelectWoundDripIntervalMs(const WoundTracker& tracker, UInt32 now)
    {
        const UInt32 minimum = s_settings.woundDripMinIntervalMs;
        const UInt32 maximum = s_settings.woundDripMaxIntervalMs;
        if (maximum <= minimum)
            return minimum;

        const UInt32 seed = tracker.refId ^
            (tracker.cycleIndex * 0x85EBCA6Bu) ^
            (tracker.projectionCount * 0x9E3779B9u) ^
            Mix(now);
        return minimum + (Mix(seed) % ((maximum - minimum) + 1));
    }

    static bool HasNativeWalkOrRunIntent(Actor* actor)
    {
        if (!actor || !actor->process)
            return false;

        const UInt32 movementFlags = actor->process->GetMovementFlags();
        return (movementFlags & kMovementDirectionMask) != 0 &&
            (movementFlags & kMovementWalkRunMask) != 0;
    }

    static bool SpawnWoundDrip(Actor* actor, WoundTracker& tracker, UInt32 now, const char* reason)
    {
        const UInt32 available = RemainingCapacity(tracker.projectionCount, s_settings.maxWoundTrailDecals);
        if (!actor || !available)
            return false;

        const char* texturePath = ResolveBloodDecalTexturePath(actor);
        if (!texturePath || !texturePath[0])
            return false;

        LeakTracker scratch = {};
        scratch.refId = actor->refID;
        if (!ResolveLimbAnchors(actor, scratch) || !scratch.limbCount)
            return false;

        const UInt32 maxForDrip = available < s_settings.woundProjectionsPerDrip
            ? available
            : s_settings.woundProjectionsPerDrip;
        const UInt32 seed = tracker.refId ^
            (tracker.cycleIndex * 0x85EBCA6Bu) ^
            (tracker.projectionCount * 0x9E3779B9u) ^
            Mix(now);

        const UInt32 firstLimb = Mix(seed ^ 0xD1B54A35u) % scratch.limbCount;
        const LeakTracker::LimbAnchor* selectedLimb = nullptr;
        for (UInt32 offset = 0; offset < scratch.limbCount; ++offset)
        {
            const LeakTracker::LimbAnchor& candidate = scratch.limbs[(firstLimb + offset) % scratch.limbCount];
            if (candidate.active && IsFinitePoint(candidate.x, candidate.y, candidate.z))
            {
                selectedLimb = &candidate;
                break;
            }
        }

        if (!selectedLimb)
            return false;

        const UInt32 spawned = SpawnMovedLimbTrailCluster(
            actor,
            texturePath,
            selectedLimb->x,
            selectedLimb->y,
            selectedLimb->z,
            selectedLimb->floorZ,
            tracker.hasHitDirection ? tracker.hitDirectionX : 0.0f,
            tracker.hasHitDirection ? tracker.hitDirectionY : 0.0f,
            seed,
            s_settings.woundTrailRadiusScale,
            maxForDrip);

        if (!spawned)
            return false;

        tracker.projectionCount += spawned;

        if (s_settings.logSpawns)
        {
            _MESSAGE(
                "BloodOnDeath wound drip: actor=0x%08X reason=%s spawned=%u total=%u limb=%u at=(%.2f, %.2f, %.2f)",
                tracker.refId,
                reason ? reason : "movement",
                spawned,
                tracker.projectionCount,
                selectedLimb->sectionIndex,
                selectedLimb->x,
                selectedLimb->y,
                selectedLimb->z);
        }

        return true;
    }

    static void StartWoundTrail(Actor* actor, TESObjectREFR* source, const char* reason)
    {
        if (!ShouldTrackWoundedActor(actor) || !HasLoadedBloodGeometry(actor))
            return;

        const char* texturePath = ResolveBloodDecalTexturePath(actor);
        if (!texturePath || !texturePath[0])
            return;

        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (!ResolveActorWorldPosition(actor, x, y, z))
            return;

        const UInt32 now = GetTickCount();
        WoundTracker& tracker = s_wounds[actor->refID];
        if (tracker.refId != actor->refID)
            tracker = WoundTracker();

        tracker.refId = actor->refID;
        tracker.woundStartTick = now;
        tracker.lastLoadedTick = now;
        tracker.projectionCount = 0;
        tracker.hasPosition = true;
        tracker.wasWalkingOrRunning = false;
        tracker.lastX = x;
        tracker.lastY = y;
        tracker.lastZ = z;
        tracker.lastConfirmedMovementTick = 0;
        tracker.hasConfirmedMovement = false;
        tracker.hasHitDirection = ResolveHitImpulseDirection(actor, source, tracker.hitDirectionX, tracker.hitDirectionY);
        if (!tracker.hasHitDirection)
        {
            tracker.hitDirectionX = 0.0f;
            tracker.hitDirectionY = 0.0f;
        }
        ++tracker.cycleIndex;
        if (!tracker.cycleIndex)
            ++tracker.cycleIndex;

        SpawnWoundDrip(actor, tracker, now, reason ? reason : "hit");
        tracker.movementIntervalStartTick = now;
        tracker.nextDripIntervalMs = SelectWoundDripIntervalMs(tracker, now);

        if (s_settings.logSpawns)
        {
            _MESSAGE(
                "BloodOnDeath wound tracking started: actor=0x%08X durationMs=%u dripIntervalMs=%u-%u",
                tracker.refId,
                s_settings.woundTrailDurationMs,
                s_settings.woundDripMinIntervalMs,
                s_settings.woundDripMaxIntervalMs);
        }
    }

    static bool UpdateWoundTracker(WoundTracker& tracker, UInt32 now)
    {
        if (tracker.projectionCount >= s_settings.maxWoundTrailDecals)
            return false;

        Actor* actor = LookupActor(tracker.refId);
        if (!actor)
            return false;

        if (!ShouldTrackWoundedActor(actor))
            return false;

        if (ElapsedMs(now, tracker.woundStartTick) > s_settings.woundTrailDurationMs)
            return false;

        if (!HasLoadedBloodGeometry(actor))
        {
            if (ElapsedMs(now, tracker.lastLoadedTick) > s_settings.unloadedTtlMs)
                return false;
            tracker.hasPosition = false;
            tracker.wasWalkingOrRunning = false;
            tracker.lastConfirmedMovementTick = 0;
            tracker.hasConfirmedMovement = false;
            return true;
        }

        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (!ResolveActorWorldPosition(actor, x, y, z))
        {
            tracker.hasPosition = false;
            tracker.wasWalkingOrRunning = false;
            tracker.hasConfirmedMovement = false;
            tracker.lastConfirmedMovementTick = 0;
            return ElapsedMs(now, tracker.lastLoadedTick) <= s_settings.unloadedTtlMs;
        }
        tracker.lastLoadedTick = now;

        if (!tracker.hasPosition)
        {
            tracker.hasPosition = true;
            tracker.lastX = x;
            tracker.lastY = y;
            tracker.lastZ = z;
            tracker.wasWalkingOrRunning = false;
            tracker.lastConfirmedMovementTick = 0;
            tracker.hasConfirmedMovement = false;
            tracker.movementIntervalStartTick = now;
            tracker.nextDripIntervalMs = SelectWoundDripIntervalMs(tracker, now);
            return true;
        }

        const float deltaX = x - tracker.lastX;
        const float deltaY = y - tracker.lastY;
        const float deltaZ = z - tracker.lastZ;
        const float horizontalMovementSq = (deltaX * deltaX) + (deltaY * deltaY);
        const float movementSq = horizontalMovementSq + (deltaZ * deltaZ);

        tracker.lastX = x;
        tracker.lastY = y;
        tracker.lastZ = z;

        const float minimumMovementSq =
            kMinimumWoundFrameMovementDistance * kMinimumWoundFrameMovementDistance;
        const float maximumMovementSq =
            kMaximumWoundFrameMovementDistance * kMaximumWoundFrameMovementDistance;
        const bool nativeWalkOrRunIntent = HasNativeWalkOrRunIntent(actor);

        // A teleport/invalid step must invalidate the grace window immediately.
        if (!IsFinite(movementSq) || movementSq > maximumMovementSq)
        {
            tracker.lastConfirmedMovementTick = 0;
            tracker.hasConfirmedMovement = false;
        }

        if (nativeWalkOrRunIntent &&
            IsFinite(movementSq) &&
            horizontalMovementSq >= minimumMovementSq &&
            movementSq <= maximumMovementSq)
        {
            tracker.lastConfirmedMovementTick = now;
            tracker.hasConfirmedMovement = true;
        }

        const bool walkingOrRunning = nativeWalkOrRunIntent &&
            tracker.hasConfirmedMovement &&
            ElapsedMs(now, tracker.lastConfirmedMovementTick) <= kWoundMovementConfirmationGraceMs;

        if (!walkingOrRunning)
        {
            if (tracker.wasWalkingOrRunning)
                tracker.movementIntervalStartTick = now;
            tracker.wasWalkingOrRunning = false;
            return tracker.projectionCount < s_settings.maxWoundTrailDecals;
        }

        if (!tracker.wasWalkingOrRunning)
        {
            tracker.wasWalkingOrRunning = true;
            tracker.movementIntervalStartTick = now;
            tracker.nextDripIntervalMs = SelectWoundDripIntervalMs(tracker, now);
            return tracker.projectionCount < s_settings.maxWoundTrailDecals;
        }

        if (ElapsedMs(now, tracker.movementIntervalStartTick) >= tracker.nextDripIntervalMs)
        {
            SpawnWoundDrip(actor, tracker, now, "walk-run");
            tracker.movementIntervalStartTick = now;
            tracker.nextDripIntervalMs = SelectWoundDripIntervalMs(tracker, now);
        }

        return tracker.projectionCount < s_settings.maxWoundTrailDecals;
    }

    static void UpdateTrackedLeaks()
    {
        if (s_leaks.empty() && s_wounds.empty())
            return;

        const UInt32 now = GetTickCount();
        std::map<UInt32, LeakTracker>::iterator leakIt = s_leaks.begin();
        while (leakIt != s_leaks.end())
        {
            bool keep = false;
            const UInt32 refId = leakIt->first;

            __try
            {
                keep = UpdateLeakTracker(leakIt->second, now);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                keep = false;
                _ERROR("BloodOnDeath tracker faulted and was removed: actor=0x%08X", refId);
            }

            if (keep)
                ++leakIt;
            else
                leakIt = s_leaks.erase(leakIt);
        }

        std::map<UInt32, WoundTracker>::iterator woundIt = s_wounds.begin();
        while (woundIt != s_wounds.end())
        {
            bool keep = false;
            const UInt32 refId = woundIt->first;

            __try
            {
                keep = UpdateWoundTracker(woundIt->second, now);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                keep = false;
                _ERROR("BloodOnDeath wound tracker faulted and was removed: actor=0x%08X", refId);
            }

            if (keep)
                ++woundIt;
            else
                woundIt = s_wounds.erase(woundIt);
        }
    }

    static void ClearTrackedLeaks(const char* reason)
    {
        if (!s_leaks.empty() || !s_wounds.empty())
        {
            _MESSAGE(
                "BloodOnDeath cleared %u corpse leaks and %u wound trails: %s",
                static_cast<UInt32>(s_leaks.size()),
                static_cast<UInt32>(s_wounds.size()),
                reason ? reason : "unknown");
        }
        s_leaks.clear();
        s_wounds.clear();
    }

    static ActorHandleDeathStateFn ReadCurrentDeathStateCallTarget()
    {
        if (*reinterpret_cast<UInt8*>(kActorKillHandleDeathStateCall) != 0xE8)
            return nullptr;

        const SInt32 rel = *reinterpret_cast<SInt32*>(kActorKillHandleDeathStateCall + 1);
        return reinterpret_cast<ActorHandleDeathStateFn>(kActorKillHandleDeathStateCall + 5 + rel);
    }

    static void __fastcall HandleDeathStateHook(Actor* actor, void*, UInt32 newDeadState)
    {
        const UInt32 oldDeadState = actor ? actor->DeadState : 0;

        if (s_actorHandleDeathState)
            s_actorHandleDeathState(actor, newDeadState);

        if (!actor || newDeadState == 0 || oldDeadState != 0 || actor->DeadState == 0)
            return;

        __try
        {
            StartCorpseBlood(actor, true, "death");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            _ERROR(
                "BloodOnDeath death tracking faulted: actor=0x%08X state=%u",
                actor ? actor->refID : 0,
                newDeadState);
        }
    }

    static void CallActorHandleHitVisualEffectsWithDecalSettings(
        Actor* actor,
        TESObjectREFR* source,
        float arg4,
        float arg8,
        void* localStart,
        void* localEnd)
    {
        if (!s_actorHandleHitVisualEffects)
            return;

        // Lifetime extension and the per-frame projection budget are separate
        // INI controls. ScopedDecalCreationSettings applies each control's own
        // setting internally.
        ScopedDecalCreationSettings decalSettings(s_settings.extendHitBloodDecals, true);
        s_actorHandleHitVisualEffects(actor, source, arg4, arg8, localStart, localEnd);
    }

    static void __fastcall HandleHitVisualEffectsHook(
        Actor* actor,
        void*,
        TESObjectREFR* source,
        float arg4,
        float arg8,
        void* localStart,
        void* localEnd)
    {
        CallActorHandleHitVisualEffectsWithDecalSettings(actor, source, arg4, arg8, localStart, localEnd);

        if (!actor)
            return;

        __try
        {
            if (actor->DeadState != 0)
            {
                const bool playerSource = IsNativePlayerCharacterReference(source);
                StartCorpseHitBlood(actor, source, playerSource, playerSource ? "player-corpse-hit" : "corpse-hit");
            }
            else if (ShouldStartWoundTrailForHit(actor, source, arg4))
                StartWoundTrail(actor, source, "hit");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            _ERROR(
                "BloodOnDeath hit tracking faulted: actor=0x%08X state=%u",
                actor ? actor->refID : 0,
                actor ? actor->DeadState : 0);
        }
    }

    static __declspec(naked) void MainLoopTickHook()
    {
        __asm
        {
            mov     ecx, [eax]
            mov     edx, [ecx+0Ch]
            pushfd
            pushad
            mov     eax, offset s_frameHookFxState
            fxsave  [eax]
            call    UpdateTrackedLeaks
            mov     eax, offset s_frameHookFxState
            fxrstor [eax]
            popad
            popfd
            jmp     [s_mainLoopTickReturn]
        }
    }

    static void MessageHandler(OBSEMessagingInterface::Message* msg);

    struct HookPatch
    {
        UInt32 address;
        UInt32 size;
        UInt8 replacement[6];
        DWORD oldProtection;
        bool writable;
    };

    static bool IsExecutableHookTarget(UInt32 address)
    {
        MEMORY_BASIC_INFORMATION memory = {};
        return address && VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) &&
            memory.State == MEM_COMMIT && !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
    }

    static void EncodeHookBranch(UInt8* bytes, UInt32 address, UInt8 opcode, UInt32 target)
    {
        bytes[0] = opcode;
        const UInt32 displacement = target - address - 5;
        std::memcpy(bytes + 1, &displacement, sizeof(displacement));
    }

    static void RestoreHookProtections(HookPatch* patches, UInt32 count)
    {
        // Reverse order also restores correctly if two sites ever share a page.
        while (count)
        {
            HookPatch& patch = patches[--count];
            if (!patch.writable)
                continue;
            DWORD ignored = 0;
            if (!VirtualProtect(reinterpret_cast<void*>(patch.address), patch.size, patch.oldProtection, &ignored))
                _ERROR("BloodOnDeath could not restore hook page protection at 0x%08X", patch.address);
            patch.writable = false;
        }
    }

    static bool InstallHooks()
    {
        if (s_deathHookInstalled && s_hitVisualsHookInstalled && s_frameHookInstalled)
            return true;
        if (s_deathHookInstalled || s_hitVisualsHookInstalled || s_frameHookInstalled)
        {
            _ERROR("BloodOnDeath found inconsistent existing hook state; retaining module without further patches");
            return true;
        }
        if (!g_messaging || !g_messaging->RegisterListener)
        {
            _ERROR("BloodOnDeath requires OBSE lifecycle messaging; no hooks installed");
            return false;
        }

        ActorHandleDeathStateFn currentTarget = ReadCurrentDeathStateCallTarget();
        const UInt32 deathTarget = reinterpret_cast<UInt32>(currentTarget);
        if (!IsExecutableHookTarget(deathTarget) || deathTarget == reinterpret_cast<UInt32>(&HandleDeathStateHook) ||
            std::memcmp(reinterpret_cast<void*>(kActorHandleHitVisualEffects), kHitVisualsExpectedBytes, sizeof(kHitVisualsExpectedBytes)) != 0 ||
            std::memcmp(reinterpret_cast<void*>(kMainLoopTickPatch), kMainLoopTickExpectedBytes, sizeof(kMainLoopTickExpectedBytes)) != 0)
        {
            _ERROR("BloodOnDeath hook conflict or invalid death target; no hooks installed");
            return false;
        }

        const UInt32 overwriteSize = sizeof(kHitVisualsExpectedBytes);
        UInt8* trampoline = static_cast<UInt8*>(VirtualAlloc(nullptr, overwriteSize + 5,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!trampoline)
        {
            _ERROR("BloodOnDeath trampoline allocation failed; no hooks installed");
            return false;
        }
        std::memcpy(trampoline, reinterpret_cast<void*>(kActorHandleHitVisualEffects), overwriteSize);
        EncodeHookBranch(trampoline + overwriteSize, reinterpret_cast<UInt32>(trampoline) + overwriteSize,
            0xE9, kActorHandleHitVisualEffects + overwriteSize);
        DWORD previousProtection = 0;
        if (!VirtualProtect(trampoline, overwriteSize + 5, PAGE_EXECUTE_READ, &previousProtection) ||
            !FlushInstructionCache(GetCurrentProcess(), trampoline, overwriteSize + 5))
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            _ERROR("BloodOnDeath trampoline preparation failed; no hooks installed");
            return false;
        }

        HookPatch patches[] = {
            { kActorKillHandleDeathStateCall, 5, {} },
            { kActorHandleHitVisualEffects, overwriteSize, {} },
            { kMainLoopTickPatch, sizeof(kMainLoopTickExpectedBytes), {} }
        };
        const UInt32 count = sizeof(patches) / sizeof(patches[0]);
        EncodeHookBranch(patches[0].replacement, patches[0].address, 0xE8, reinterpret_cast<UInt32>(&HandleDeathStateHook));
        EncodeHookBranch(patches[1].replacement, patches[1].address, 0xE9, reinterpret_cast<UInt32>(&HandleHitVisualEffectsHook));
        patches[1].replacement[5] = 0x90;
        EncodeHookBranch(patches[2].replacement, patches[2].address, 0xE9, reinterpret_cast<UInt32>(&MainLoopTickHook));

        // Complete every fallible prerequisite before exposing any branch hook.
        for (UInt32 i = 0; i < count; ++i)
        {
            HookPatch& patch = patches[i];
            if (!VirtualProtect(reinterpret_cast<void*>(patch.address), patch.size,
                PAGE_EXECUTE_READWRITE, &patch.oldProtection))
            {
                RestoreHookProtections(patches, count);
                VirtualFree(trampoline, 0, MEM_RELEASE);
                _ERROR("BloodOnDeath hook page preparation failed; no hooks installed");
                return false;
            }
            patch.writable = true;
        }
        if (!g_messaging->RegisterListener(g_pluginHandle, "OBSE", MessageHandler))
        {
            RestoreHookProtections(patches, count);
            VirtualFree(trampoline, 0, MEM_RELEASE);
            _ERROR("BloodOnDeath lifecycle registration failed; no hooks installed");
            return false;
        }

        // After registration/commit, keep the DLL loaded: OBSE owns the callback
        // and branch sites can refer into this module even if a cleanup API fails.
        s_actorHandleDeathState = currentTarget;
        s_actorHandleHitVisualEffects = reinterpret_cast<ActorHandleHitVisualEffectsFn>(trampoline);
        for (UInt32 i = 0; i < count; ++i)
        {
            const HookPatch& patch = patches[i];
            std::memcpy(reinterpret_cast<void*>(patch.address), patch.replacement, patch.size);
            if (!FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(patch.address), patch.size))
                _ERROR("BloodOnDeath instruction-cache flush failed at 0x%08X; retaining loaded hooks", patch.address);
        }
        RestoreHookProtections(patches, count);
        s_deathHookInstalled = s_hitVisualsHookInstalled = s_frameHookInstalled = true;
        _MESSAGE("BloodOnDeath hooks and lifecycle listener installed; death target=%08X trampoline=%08X",
            deathTarget, reinterpret_cast<UInt32>(trampoline));
        return true;
    }

    static void MessageHandler(OBSEMessagingInterface::Message* msg)
    {
        if (!msg)
            return;

        switch (msg->type)
        {
            case OBSEMessagingInterface::kMessage_PreLoadGame:
                ClearTrackedLeaks("pre-load");
                break;
            case OBSEMessagingInterface::kMessage_LoadGame:
                ClearTrackedLeaks("load");
                break;
            case OBSEMessagingInterface::kMessage_ExitToMainMenu:
                ClearTrackedLeaks("main-menu");
                break;
            case OBSEMessagingInterface::kMessage_ExitGame:
            case OBSEMessagingInterface::kMessage_ExitGame_Console:
                ClearTrackedLeaks("exit");
                break;
        }
    }
}

extern "C"
{
    bool OBSEPlugin_Query(const OBSEInterface* obse, PluginInfo* info)
    {
        gLog.OpenRelative(CSIDL_MYDOCUMENTS, PLUGIN_LOG_FILE);
        gLog.SetLogLevel(IDebugLog::kLevel_Message);
        gLog.SetPrintLevel(IDebugLog::kLevel_Message);
        _MESSAGE("%s %s query", PLUGIN_NAME_SHORT, PLUGIN_VERSION_LABEL);

        info->infoVersion = PluginInfo::kInfoVersion;
        info->name = PLUGIN_NAME_LONG;
        info->version = PLUGIN_VERSION_DLL;

        if (obse->isEditor)
            return true;

        if (obse->oblivionVersion != OBLIVION_VERSION)
        {
            _ERROR("Unsupported Oblivion version %08X; expected %08X", obse->oblivionVersion, OBLIVION_VERSION);
            return false;
        }

        return true;
    }

    bool OBSEPlugin_Load(const OBSEInterface* obse)
    {
        gLog.OpenRelative(CSIDL_MYDOCUMENTS, PLUGIN_LOG_FILE);
        gLog.SetLogLevel(IDebugLog::kLevel_Message);
        gLog.SetPrintLevel(IDebugLog::kLevel_Message);
        _MESSAGE("%s %s load", PLUGIN_NAME_SHORT, PLUGIN_VERSION_LABEL);

        g_pluginHandle = obse->GetPluginHandle();

        if (!obse->isEditor)
        {
            if (obse->oblivionVersion != OBLIVION_VERSION)
            {
                _ERROR("Unsupported Oblivion runtime; no hooks installed");
                return false;
            }
            LoadSettings();
            if (!s_settings.enabled)
            {
                _MESSAGE("BloodOnDeath disabled by configuration; no hooks installed");
                return true;
            }
            g_messaging = static_cast<OBSEMessagingInterface*>(obse->QueryInterface(kInterface_Messaging));
            return InstallHooks();
        }

        return true;
    }
}
