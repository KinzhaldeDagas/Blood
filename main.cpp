#include "config.h"

#include "obse/GameAPI.h"
#include "obse/GameObjects.h"
#include "obse/NiNodes.h"
#include "obse/PluginAPI.h"
#include "obse_common/SafeWrite.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <shlobj.h>
#include <windows.h>

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
    static const UInt32 kGameSettingDecalLifetimeDisplay = 0x00B097C8;
    static const UInt32 kGameSettingMaxDecalsPerFrameDisplay = 0x00B097D0;
    static const UInt32 kMainLoopTickPatch = 0x0040F1A3;
    static UInt32 s_mainLoopTickReturn = 0x0040F1A8;

    static const UInt8 kMainLoopTickExpectedBytes[5] = { 0x8B, 0x08, 0x8B, 0x51, 0x0C };
    static const UInt8 kHitVisualsExpectedBytes[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
    static const float kNativeDecalProjectionRayLength = 80.0f;
    static const float kMaxFloorProjectionHeight = 72.0f;
    static const float kDefaultBloodDurationSeconds = 8.0f;
    static const float kMaxBloodDurationSeconds = 600.0f;
    static const float kCellScopedDecalLifetimeSeconds = 604800.0f;
    static const float kMaxDecalLifetimeSeconds = 31536000.0f;
    static const UInt32 kUnlimitedDecalsPerFrame = 0x7FFFFFFF;
    static const UInt32 kMaxLimbLeakSections = 13;
    static const UInt32 kMaxBoneNamesPerLimbSection = 4;

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
        UInt32 maxDecalsPerFrame;
        UInt32 leakDurationMs;
        UInt32 settleMs;
        UInt32 unloadedTtlMs;
        float decalLifetimeSeconds;
        float spillRadius;
        float leakLength;
        float projectionHeight;
        float settleMoveTolerance;
        float moveRestartDistance;
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
        bool hasPosition;
        bool completed;
        bool initialPulsePending;
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
            float x;
            float y;
            float z;
            float floorZ;
        } limbs[kMaxLimbLeakSections];
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
        8,
        kUnlimitedDecalsPerFrame,
        8000,
        0,
        30000,
        kCellScopedDecalLifetimeSeconds,
        72.0f,
        72.0f,
        32.0f,
        8.0f,
        32.0f
    };

    static Settings s_settings = kEmbeddedDefaults;

    static bool s_deathHookInstalled = false;
    static bool s_hitVisualsHookInstalled = false;
    static bool s_frameHookInstalled = false;
    static bool s_updateDisabledByFault = false;
    __declspec(align(16)) static UInt8 s_frameHookFxState[512] = {};
    static std::map<UInt32, LeakTracker> s_leaks;

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

    static void LoadSettings()
    {
        s_settings = kEmbeddedDefaults;

        s_settings.totalDecals = ClampUInt32(s_settings.totalDecals, 1, 96);
        s_settings.maxDecalsPerFrame = ClampUInt32(s_settings.maxDecalsPerFrame, 1, kUnlimitedDecalsPerFrame);
        s_settings.leakDurationMs = ClampUInt32(
            static_cast<UInt32>(ClampFloat(kDefaultBloodDurationSeconds, 1.0f, kMaxBloodDurationSeconds) * 1000.0f),
            1000,
            static_cast<UInt32>(kMaxBloodDurationSeconds * 1000.0f));
        s_settings.settleMs = ClampUInt32(s_settings.settleMs, 0, 5000);
        s_settings.unloadedTtlMs = ClampUInt32(s_settings.unloadedTtlMs, 1000, 300000);
        s_settings.decalLifetimeSeconds = ClampFloat(s_settings.decalLifetimeSeconds, 1.0f, kMaxDecalLifetimeSeconds);
        s_settings.spillRadius = ClampFloat(s_settings.spillRadius, 0.0f, 256.0f);
        s_settings.leakLength = ClampFloat(s_settings.leakLength, 0.0f, 256.0f);
        s_settings.projectionHeight = ClampFloat(s_settings.projectionHeight, 8.0f, kMaxFloorProjectionHeight);
        s_settings.settleMoveTolerance = ClampFloat(s_settings.settleMoveTolerance, 1.0f, 64.0f);
        s_settings.moveRestartDistance = ClampFloat(s_settings.moveRestartDistance, 4.0f, 256.0f);

        _MESSAGE(
            "BloodOnDeath config: %s enabled=%u includePlayer=%u setGameDecalLifetime=%u extendHitBloodDecals=%u setGameDecalLimit=%u totalDecals=%u maxDecalsPerFrame=%u durationMs=%u settleMs=%u unloadedTtlMs=%u decalLifetime=%.2f radius=%.2f leak=%.2f height=%.2f settleTolerance=%.2f restartDistance=%.2f log=%u",
            "embedded",
            s_settings.enabled ? 1 : 0,
            s_settings.includePlayer ? 1 : 0,
            s_settings.setGameDecalLifetime ? 1 : 0,
            s_settings.extendHitBloodDecals ? 1 : 0,
            s_settings.setGameDecalLimit ? 1 : 0,
            s_settings.totalDecals,
            s_settings.maxDecalsPerFrame,
            s_settings.leakDurationMs,
            s_settings.settleMs,
            s_settings.unloadedTtlMs,
            s_settings.decalLifetimeSeconds,
            s_settings.spillRadius,
            s_settings.leakLength,
            s_settings.projectionHeight,
            s_settings.settleMoveTolerance,
            s_settings.moveRestartDistance,
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
                    if (originalMaxPerFrame < s_settings.maxDecalsPerFrame)
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

    static const char* GetDefaultBloodDecalTexturePath()
    {
        SettingInfo* setting = nullptr;
        if (GetGameSetting("sBloodTextureDefault", &setting) &&
            setting &&
            setting->Type() == SettingInfo::kSetting_String &&
            setting->s &&
            setting->s[0])
        {
            return setting->s;
        }

        return "Effects\\blooddecal.dds";
    }

    static const char* ResolveBloodDecalTexturePath(TESObjectREFR* ref)
    {
        if (!ref)
            return GetDefaultBloodDecalTexturePath();

        const char* result = nullptr;

        __try
        {
            if (!ActorShouldEmitBloodEffects(ref))
                return nullptr;

            const char* path = ActorGetBloodDecalTexturePath(ref);
            result = (path && path[0]) ? path : GetDefaultBloodDecalTexturePath();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            _ERROR("BloodOnDeath blood eligibility/texture lookup faulted for ref=0x%08X", ref->refID);
            result = nullptr;
        }

        return result;
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

    static bool ResolveLimbSectionPosition(Actor* actor, const LimbSectionDef& section, float& x, float& y, float& z)
    {
        NiNode* root = actor ? actor->GetNiNode() : nullptr;
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

    static bool CanUseActor(Actor* actor)
    {
        if (!s_settings.enabled || !actor)
            return false;

        if (!s_settings.includePlayer && g_thePlayer && *g_thePlayer == actor)
            return false;

        return true;
    }

    static bool ShouldStartTrackingActor(Actor* actor)
    {
        return CanUseActor(actor) && actor->DeadState != 0;
    }

    static bool ShouldContinueTrackingActor(Actor* actor)
    {
        return CanUseActor(actor) && actor->DeadState != 0;
    }

    static float ResolveFloorProjectionZ(Actor* actor, float referenceZ)
    {
        if (actor && IsFinite(actor->posZ))
            return actor->posZ;

        return referenceZ;
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

    static UInt32 ResolveLimbAnchors(Actor* actor, LeakTracker& tracker)
    {
        tracker.limbCount = 0;
        if (!actor)
            return 0;

        for (UInt32 i = 0; i < kMaxLimbLeakSections; ++i)
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            if (!ResolveLimbSectionPosition(actor, kLimbSections[i], x, y, z))
                continue;

            LeakTracker::LimbAnchor& limb = tracker.limbs[tracker.limbCount];
            limb.active = true;
            limb.x = x;
            limb.y = y;
            limb.z = z;
            limb.floorZ = ResolveFloorProjectionZ(actor, z);
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
                limb.x = x;
                limb.y = y;
                limb.z = z;
                limb.floorZ = ResolveFloorProjectionZ(actor, z);
                tracker.limbCount = 1;
            }
        }

        return tracker.limbCount;
    }

    static bool SpawnBloodAtLimbAnchors(Actor* actor, const LeakTracker& tracker)
    {
        if (!actor || !actor->parentCell || !tracker.limbCount)
            return false;

        const char* texturePath = ResolveBloodDecalTexturePath(actor);
        if (!texturePath || !texturePath[0])
            return false;

        bool spawned = false;
        for (UInt32 i = 0; i < tracker.limbCount && i < kMaxLimbLeakSections; ++i)
        {
            const LeakTracker::LimbAnchor& limb = tracker.limbs[i];
            if (!limb.active || !IsFinitePoint(limb.x, limb.y, limb.z))
                continue;

            ProjectBloodSpatterToGeometryLikeHitDecal(
                actor,
                texturePath,
                limb.x,
                limb.y,
                limb.z,
                limb.floorZ,
                limb.x,
                limb.y,
                actor->refID ^ ((i + 1) * 0x9E3779B9u),
                0.18f);
            spawned = true;
        }

        return spawned;
    }

    static bool SpawnInitialLimbSpray(Actor* actor, LeakTracker& tracker)
    {
        if (!actor || !actor->parentCell)
            return false;

        ResolveLimbAnchors(actor, tracker);
        return SpawnBloodAtLimbAnchors(actor, tracker);
    }

    static void ResetLeakCycle(LeakTracker& tracker, UInt32 now, float x, float y, float z)
    {
        tracker.stableSinceTick = now;
        tracker.leakStartTick = 0;
        tracker.lastPulseTick = 0;
        tracker.pulseIndex = 0;
        tracker.completed = false;
        tracker.initialPulsePending = true;
        tracker.hasPosition = true;
        tracker.lastX = x;
        tracker.lastY = y;
        tracker.lastZ = z;
        tracker.limbCount = 0;
        for (UInt32 i = 0; i < kMaxLimbLeakSections; ++i)
            tracker.limbs[i].active = false;
    }

    static void BeginLeakCycle(Actor* actor, LeakTracker& tracker, UInt32 now, float x, float y, float z)
    {
        tracker.leakStartTick = now;
        tracker.lastPulseTick = 0;
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
            }
        }

        if (spawned)
        {
            tracker.lastPulseTick = now;
            ++tracker.pulseIndex;
        }
    }

    static bool UpdateLeakTracker(LeakTracker& tracker, UInt32 now)
    {
        Actor* actor = LookupActor(tracker.refId);
        if (!actor)
            return false;

        if (!ShouldContinueTrackingActor(actor))
            return false;

        if (!actor->parentCell)
        {
            if (tracker.lastLoadedTick && ElapsedMs(now, tracker.lastLoadedTick) > s_settings.unloadedTtlMs)
                return false;
            tracker.hasPosition = false;
            return true;
        }

        tracker.lastLoadedTick = now;

        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (!ResolveActorWorldPosition(actor, x, y, z))
            return true;

        if (!tracker.hasPosition)
        {
            ResetLeakCycle(tracker, now, x, y, z);
            return true;
        }

        if (tracker.initialPulsePending)
        {
            ResolveLimbAnchors(actor, tracker);
            SpawnBloodAtLimbAnchors(actor, tracker);
            tracker.initialPulsePending = false;
        }

        if (tracker.leakStartTick)
        {
            const float restartDistanceSq = s_settings.moveRestartDistance * s_settings.moveRestartDistance;
            if (DistanceSquared(x, y, z, tracker.anchorX, tracker.anchorY, tracker.anchorZ) > restartDistanceSq)
            {
                ResetLeakCycle(tracker, now, x, y, z);
                if (s_settings.logSpawns)
                    _MESSAGE("BloodOnDeath leak reset after corpse movement: actor=0x%08X", tracker.refId);
                return true;
            }

            if (ElapsedMs(now, tracker.leakStartTick) <= s_settings.leakDurationMs)
            {
                if (tracker.pulseIndex < s_settings.totalDecals &&
                    (!tracker.lastPulseTick || ElapsedMs(now, tracker.lastPulseTick) >= GetPulseIntervalMs()))
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
        if (ResolveActorWorldPosition(actor, x, y, z))
        {
            ResetLeakCycle(tracker, now, x, y, z);
            if (SpawnInitialLimbSpray(actor, tracker))
                tracker.initialPulsePending = false;
        }
        else
        {
            tracker.hasPosition = false;
            tracker.stableSinceTick = now;
            tracker.leakStartTick = 0;
            tracker.lastPulseTick = 0;
            tracker.pulseIndex = 0;
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

        TrackDeadActor(actor, restartCompletedCycle);

        if (s_settings.logSpawns)
        {
            _MESSAGE(
                "BloodOnDeath corpse blood queued: actor=0x%08X reason=%s",
                actor->refID,
                reason ? reason : "unknown");
        }
    }

    static void UpdateTrackedLeaks()
    {
        if (s_updateDisabledByFault || s_leaks.empty())
            return;

        const UInt32 now = GetTickCount();
        std::map<UInt32, LeakTracker>::iterator it = s_leaks.begin();
        while (it != s_leaks.end())
        {
            bool keep = false;
            const UInt32 refId = it->first;

            __try
            {
                keep = UpdateLeakTracker(it->second, now);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                keep = false;
                _ERROR("BloodOnDeath tracker faulted and was removed: actor=0x%08X", refId);
            }

            if (keep)
                ++it;
            else
                it = s_leaks.erase(it);
        }
    }

    static void ClearTrackedLeaks(const char* reason)
    {
        if (!s_leaks.empty())
            _MESSAGE("BloodOnDeath cleared %u tracked leaks: %s", static_cast<UInt32>(s_leaks.size()), reason ? reason : "unknown");
        s_leaks.clear();
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

        ScopedDecalCreationSettings decalSettings(s_settings.extendHitBloodDecals, s_settings.extendHitBloodDecals);
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

        if (!actor || actor->DeadState == 0)
            return;

        __try
        {
            StartCorpseBlood(actor, true, "corpse-hit");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            _ERROR(
                "BloodOnDeath corpse-hit tracking faulted: actor=0x%08X state=%u",
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

    static bool InstallDeathHook()
    {
        if (s_deathHookInstalled)
            return true;

        ActorHandleDeathStateFn currentTarget = ReadCurrentDeathStateCallTarget();
        if (!currentTarget)
        {
            _ERROR("BloodOnDeath death hook skipped: 0x%08X is not a relative call", kActorKillHandleDeathStateCall);
            return false;
        }

        if (reinterpret_cast<UInt32>(currentTarget) != reinterpret_cast<UInt32>(&HandleDeathStateHook))
            s_actorHandleDeathState = currentTarget;

        WriteRelCall(kActorKillHandleDeathStateCall, reinterpret_cast<UInt32>(&HandleDeathStateHook));
        s_deathHookInstalled = true;

        _MESSAGE(
            "BloodOnDeath death hook installed: Actor_Kill call 0x%08X original=0x%08X hook=0x%08X",
            kActorKillHandleDeathStateCall,
            reinterpret_cast<UInt32>(s_actorHandleDeathState),
            reinterpret_cast<UInt32>(&HandleDeathStateHook));
        return true;
    }

    static bool InstallHitVisualsHook()
    {
        if (s_hitVisualsHookInstalled)
            return true;

        const UInt32 overwriteSize = sizeof(kHitVisualsExpectedBytes);
        if (std::memcmp(reinterpret_cast<void*>(kActorHandleHitVisualEffects), kHitVisualsExpectedBytes, overwriteSize) != 0)
        {
            _ERROR("BloodOnDeath hit-visual hook skipped: 0x%08X bytes did not match expected function prologue", kActorHandleHitVisualEffects);
            return false;
        }

        UInt8* trampoline = static_cast<UInt8*>(
            VirtualAlloc(nullptr, overwriteSize + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!trampoline)
        {
            _ERROR("BloodOnDeath hit-visual hook skipped: trampoline allocation failed");
            return false;
        }

        std::memcpy(trampoline, reinterpret_cast<void*>(kActorHandleHitVisualEffects), overwriteSize);
        trampoline[overwriteSize] = 0xE9;
        *reinterpret_cast<SInt32*>(trampoline + overwriteSize + 1) =
            static_cast<SInt32>((kActorHandleHitVisualEffects + overwriteSize) -
                (reinterpret_cast<UInt32>(trampoline) + overwriteSize + 5));

        s_actorHandleHitVisualEffects = reinterpret_cast<ActorHandleHitVisualEffectsFn>(trampoline);

        WriteRelJump(kActorHandleHitVisualEffects, reinterpret_cast<UInt32>(&HandleHitVisualEffectsHook));
        SafeWrite8(kActorHandleHitVisualEffects + 5, 0x90);
        s_hitVisualsHookInstalled = true;

        _MESSAGE(
            "BloodOnDeath hit-visual hook installed: Actor_HandleHitVisualEffects 0x%08X trampoline=0x%08X hook=0x%08X",
            kActorHandleHitVisualEffects,
            reinterpret_cast<UInt32>(trampoline),
            reinterpret_cast<UInt32>(&HandleHitVisualEffectsHook));
        return true;
    }

    static bool InstallFrameHook()
    {
        if (s_frameHookInstalled)
            return true;

        if (std::memcmp(reinterpret_cast<void*>(kMainLoopTickPatch), kMainLoopTickExpectedBytes, sizeof(kMainLoopTickExpectedBytes)) != 0)
        {
            _ERROR("BloodOnDeath frame hook skipped: 0x%08X bytes did not match expected foreground-frame site", kMainLoopTickPatch);
            return false;
        }

        WriteRelJump(kMainLoopTickPatch, reinterpret_cast<UInt32>(&MainLoopTickHook));
        s_frameHookInstalled = true;

        _MESSAGE(
            "BloodOnDeath frame hook installed: main loop site 0x%08X return=0x%08X hook=0x%08X",
            kMainLoopTickPatch,
            s_mainLoopTickReturn,
            reinterpret_cast<UInt32>(&MainLoopTickHook));
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
        _MESSAGE("%s query", PLUGIN_NAME_SHORT);

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
        _MESSAGE("%s load", PLUGIN_NAME_SHORT);

        g_pluginHandle = obse->GetPluginHandle();

        if (!obse->isEditor)
        {
            LoadSettings();
            InstallDeathHook();
            InstallHitVisualsHook();
            InstallFrameHook();

            g_messaging = static_cast<OBSEMessagingInterface*>(obse->QueryInterface(kInterface_Messaging));
            if (g_messaging && g_messaging->RegisterListener(g_pluginHandle, "OBSE", MessageHandler))
                _MESSAGE("BloodOnDeath messaging listener registered");
        }

        return true;
    }
}
