#include "plugin.h"
#include "CModelInfo.h"
#include "CVehicleModelInfo.h"
#include "CVehicle.h"
#include "CBoat.h"
#include "CPed.h"
#include "CAnimManager.h"
#include "CAnimBlendAssociation.h"
#include "CAnimBlendHierarchy.h"
#include "CRideAnimData.h"
#include "cHandlingDataMgr.h"
#include "tHandlingData.h"
#include "tBikeHandlingData.h"
#include "CPools.h"
#include "CTimer.h"
#include "CPad.h"
#include "common.h"
#include "RenderWare.h"
#include "CMatrix.h"
#include "CWaterLevel.h"
#include "Fx_c.h"
#include "FxPrtMult_c.h"
#include "CTaskSimpleCarSetPedOut.h"
#include "CCamera.h"
#include <cstring>

using namespace plugin;

enum {
    QUAD_RIDE_0 = 194,
    QUAD_GETOFF_LHS_0 = 373,
    QUAD_GETOFF_B_0 = 377
};

static CRideAnimData g_RideData;
static tBikeHandlingData* g_BikeHandling = nullptr;
static const float QUAD_HBSTEER_ANIM_MULT = -0.2f;
static const float LEAN_PITCH_FWD = 0.003f;
static const float LEAN_PITCH_BACK = 0.001f;

static const float GETOFF_LHS_SIDE = 1.0f;
static const float GETOFF_LHS_Y = 0.0f;
static const float GETOFF_LHS_Z = 0.8f;

static const float GETOFF_B_HOLD_SIDE = 0.0f;
static const float GETOFF_B_HOLD_FWD = -0.4f;
static const float GETOFF_B_HOLD_Z = 0.8f;

static const float GETOFF_B_SPAWN_SIDE = 0.0f;
static const float GETOFF_B_SPAWN_FWD = -2.0f;
static const float GETOFF_B_SPAWN_Z = 0.0f;

static const float GETOFF_MOVING_SPEED = 0.2f;

// B: stop following dinghy after this many seconds
static const float GETOFF_B_HOLD_SEC = 0.5f;
// B: total time the jump anim is forced (can be longer than hold)
static const float GETOFF_B_ANIM_SEC = 0.8f;

static unsigned char g_BoatRenderOriginal[5];
static bool g_BoatRenderHooked = false;
static unsigned char g_GetRideOriginal[5];
static bool g_GetRideHooked = false;
static unsigned char g_LeaveBoatFirstOriginal[5];
static bool g_LeaveBoatFirstHooked = false;
static unsigned char g_SetPedPosOriginal[5];
static bool g_SetPedPosHooked = false;
static bool g_GameReady = false;

static bool g_GetOffPlaying = false;   // on vehicle (hold + seat lock)
static bool g_GetOffStarted = false;
static bool g_GetOffUseB = false;
static bool g_GetOffAnimAfter = false; // off vehicle, keep B anim
static CPed* g_ExitPed = nullptr;
static CVehicle* g_ExitVeh = nullptr;
static unsigned int g_GetOffHoldEndMs = 0;
static unsigned int g_GetOffAnimEndMs = 0;

static void ResetExitState()
{
    g_GetOffPlaying = false;
    g_GetOffStarted = false;
    g_GetOffUseB = false;
    g_GetOffAnimAfter = false;
    g_ExitPed = nullptr;
    g_ExitVeh = nullptr;
    g_GetOffHoldEndMs = 0;
    g_GetOffAnimEndMs = 0;
}

static CVector GetOffHoldPos(CVehicle* veh)
{
    CVector pos = veh->GetPosition();
    if (g_GetOffUseB)
    {
        pos += veh->GetMatrix().GetRight() * (-GETOFF_B_HOLD_SIDE);
        pos += veh->GetMatrix().GetForward() * GETOFF_B_HOLD_FWD;
        pos.z += GETOFF_B_HOLD_Z;
    }
    else
    {
        pos += veh->GetMatrix().GetRight() * (-GETOFF_LHS_SIDE);
        pos.y += GETOFF_LHS_Y;
        pos.z += GETOFF_LHS_Z;
    }
    return pos;
}

static CVector GetOffSpawnPos(CVehicle* veh)
{
    CVector pos = veh->GetPosition();
    if (g_GetOffUseB)
    {
        pos += veh->GetMatrix().GetRight() * (-GETOFF_B_SPAWN_SIDE);
        pos += veh->GetMatrix().GetForward() * GETOFF_B_SPAWN_FWD;
        pos.z += GETOFF_B_SPAWN_Z;
    }
    else
    {
        pos += veh->GetMatrix().GetRight() * (-GETOFF_LHS_SIDE);
        pos.y += GETOFF_LHS_Y;
        pos.z += GETOFF_LHS_Z;
    }
    return pos;
}

static tBikeHandlingData* FindQuadBikeHandling(unsigned int generalHandlingId)
{
    for (int i = 0; i < 13; ++i)
    {
        if (gHandlingDataMgr.m_aBikeHandling[i].m_nVehicleId == static_cast<unsigned char>(generalHandlingId) ||
            gHandlingDataMgr.m_aBikeHandling[i].m_nVehicleId == generalHandlingId)
            return &gHandlingDataMgr.m_aBikeHandling[i];
    }
    return nullptr;
}

static RwFrame* FindFrame(RpClump* clump, const char* name)
{
    if (!clump || !name)
        return nullptr;
    return CClumpModelInfo::GetFrameFromName(clump, const_cast<char*>(name));
}

static void UpdateHandlebars(CVehicle* veh)
{
    if (!veh || !veh->m_pRwClump)
        return;
    RwFrame* hb = FindFrame(reinterpret_cast<RpClump*>(veh->m_pRwClump), "handlebars");
    if (!hb)
        return;
    float animLeanLeft = *reinterpret_cast<float*>(reinterpret_cast<char*>(&g_RideData) + 0x14);
    if (animLeanLeft == 0.0f && veh->m_fSteerAngle != 0.0f)
        animLeanLeft = veh->m_fSteerAngle;
    CMatrix mat;
    mat.Attach(RwFrameGetMatrix(hb), false);
    mat.SetRotateZOnly(QUAD_HBSTEER_ANIM_MULT * animLeanLeft);
    mat.UpdateRW();
}

static void UpdateRideDataLikeQuad(CVehicle* veh)
{
    if (!veh || !g_BikeHandling)
        return;
    g_RideData.m_nAnimGroup = 10;
    g_RideData.m_fSteerAngle = veh->m_fSteerAngle;
    float steeringLockRad = 0.6f;
    if (veh->m_pHandlingData)
        steeringLockRad = veh->m_pHandlingData->m_fSteeringLock * (3.14159265f / 180.0f);
    if (steeringLockRad < 0.1f)
        steeringLockRad = 0.1f;
    float fullAnimLean = g_BikeHandling->m_fFullAnimLean;
    float desLean = g_BikeHandling->m_fDesLean;
    float fValue = powf(desLean, CTimer::ms_fTimeStep);
    float& leanAngle = g_RideData.m_fAnimLean;
    leanAngle = fValue * leanAngle
        - fullAnimLean * veh->m_fSteerAngle / steeringLockRad * (1.0f - fValue);
    float* leanFwd = reinterpret_cast<float*>(reinterpret_cast<char*>(&g_RideData) + 0x10);
    CPad* pad = CPad::GetPad(0);
    float target = 0.0f;
    if (pad)
        target = float(-pad->GetSteeringUpDown()) / 128.0f;
    *leanFwd += (target - *leanFwd) * CTimer::ms_fTimeStep / 5.0f;
    if (*leanFwd > 1.0f) *leanFwd = 1.0f;
    if (*leanFwd < -1.0f) *leanFwd = -1.0f;
    if (*leanFwd > -0.02f && *leanFwd < 0.02f)
        *leanFwd = 0.0f;
}

static void ApplyPhysicalLean(CVehicle* veh)
{
    if (!veh)
        return;
    float leanFwd = *reinterpret_cast<float*>(reinterpret_cast<char*>(&g_RideData) + 0x10);
    if (leanFwd == 0.0f)
        return;
    float strength = (leanFwd > 0.0f) ? LEAN_PITCH_FWD : LEAN_PITCH_BACK;
    float ts = CTimer::ms_fTimeStep;
    float pitchForce = -leanFwd * veh->m_fTurnMass * strength * ts;
    CVector force = veh->GetMatrix().GetUp() * pitchForce;
    CVector point = veh->GetMatrix().GetForward();
    veh->ApplyTurnForce(force, point);
}

static void ManualExhaustFromFrame(CVehicle* veh)
{
    if (!g_GameReady || !veh || !veh->bEngineOn || !veh->m_pRwClump)
        return;
    if (veh->m_fGasPedal < 0.05f && veh->m_fGasPedal > -0.05f)
        return;
    RwFrame* frame = FindFrame(reinterpret_cast<RpClump*>(veh->m_pRwClump), "exhaust");
    if (!frame)
        return;
    RwMatrix* ltm = RwFrameGetLTM(frame);
    if (!ltm)
        return;
    CVector pos(ltm->pos.x, ltm->pos.y, ltm->pos.z);
    CVector vel;
    if (veh->m_vecMoveSpeed.Magnitude() >= 0.05f)
        vel = veh->m_vecMoveSpeed * 30.0f;
    else
        vel = veh->GetMatrix().GetForward() * -1.2f;
    float speed = veh->m_vecMoveSpeed.Magnitude() * 0.5f;
    float alpha = (0.25f - speed > 0.0f) ? (0.25f - speed) : 0.0f;
    float life = (0.2f - speed > 0.0f) ? (0.2f - speed) : 0.0f;
    bool underWater = false;
    float waterZ = 0.0f;
    CVector dummyNormal;
    if (veh->bTouchingWater &&
        CWaterLevel::GetWaterLevel(pos.x, pos.y, pos.z, &waterZ, true, &dummyNormal) &&
        waterZ >= pos.z)
        underWater = true;
    FxPrtMult_c fx(0.9f, 0.9f, 1.0f, alpha, 0.2f, 1.0f, life);
    FxSystem_c* sys = g_fx.m_pPrtSmokeII3expand;
    if (underWater)
    {
        fx.m_color.alpha = alpha * 0.5f;
        fx.m_fSize = 0.6f;
        if (g_fx.m_pPrtBubble)
            sys = g_fx.m_pPrtBubble;
    }
    if (sys)
        sys->AddParticle(&pos, &vel, 0.0f, &fx, -1.0f, veh->m_fContactSurfaceBrightness, 0.0f, 0);
}

static void ApplyBaseAndRider(CVehicle* veh, CPed* driver)
{
    if (!veh || !driver || !driver->m_pRwClump || !g_BikeHandling)
        return;
    if (g_GetOffPlaying || g_GetOffAnimAfter)
        return;

    RpClump* clump = reinterpret_cast<RpClump*>(driver->m_pRwClump);
    CAnimBlendAssociation* base = CAnimManager::BlendAnimation(clump, 10, QUAD_RIDE_0, 1000.0f);
    if (base)
    {
        base->SetBlend(1.0f, 0.0f);
        base->m_fCurrentTime = 0.0f;
    }
    UpdateRideDataLikeQuad(veh);
    using Fn = void(__cdecl*)(CPed*, CVehicle*, CRideAnimData*, tBikeHandlingData*, short);
    static Fn ProcessRiderAnims = (Fn)0x6B7280;
    ProcessRiderAnims(driver, veh, &g_RideData, g_BikeHandling, 0);
    UpdateHandlebars(veh);
    ApplyPhysicalLean(veh);
}

static void MaintainGetOffBAnim(CPed* ped)
{
    if (!ped || !ped->m_pRwClump)
        return;
    RpClump* clump = reinterpret_cast<RpClump*>(ped->m_pRwClump);
    CAnimBlendAssociation* assoc =
        CAnimManager::BlendAnimation(clump, 100, QUAD_GETOFF_B_0, 1000.0f);
    if (assoc)
    {
        assoc->m_nFlags &= ~static_cast<unsigned short>(2);
        assoc->SetBlend(1.0f, 0.0f);
    }
}

static void PlayGetOffOnce(CPed* ped)
{
    if (!ped || !ped->m_pRwClump || g_GetOffStarted)
        return;

    g_GetOffUseB = (g_ExitVeh &&
        g_ExitVeh->m_vecMoveSpeed.Magnitude() > GETOFF_MOVING_SPEED);

    unsigned int animId = g_GetOffUseB ? QUAD_GETOFF_B_0 : QUAD_GETOFF_LHS_0;

    RpClump* clump = reinterpret_cast<RpClump*>(ped->m_pRwClump);
    CAnimBlendAssociation* assoc =
        CAnimManager::BlendAnimation(clump, 100, animId, 1000.0f);

    float lhsDurationSec = 0.70f;
    if (assoc)
    {
        assoc->m_nFlags &= ~static_cast<unsigned short>(2);
        assoc->m_fSpeed = 1.0f;
        assoc->SetCurrentTime(0.0f);
        assoc->SetBlend(1.0f, 0.0f);
        if (assoc->m_pHierarchy && assoc->m_pHierarchy->m_fTotalTime > 0.05f)
            lhsDurationSec = assoc->m_pHierarchy->m_fTotalTime;
    }

    unsigned int now = CTimer::m_snTimeInMilliseconds;

    if (g_GetOffUseB)
    {
        // Separate: leave vehicle vs keep anim
        g_GetOffHoldEndMs = now + static_cast<unsigned int>(GETOFF_B_HOLD_SEC * 1000.0f);
        g_GetOffAnimEndMs = now + static_cast<unsigned int>(GETOFF_B_ANIM_SEC * 1000.0f);
        if (g_GetOffAnimEndMs < g_GetOffHoldEndMs)
            g_GetOffAnimEndMs = g_GetOffHoldEndMs;
    }
    else
    {
        g_GetOffHoldEndMs = now + static_cast<unsigned int>(lhsDurationSec * 1000.0f);
        g_GetOffAnimEndMs = g_GetOffHoldEndMs;
    }

    g_GetOffStarted = true;
}

static void FinishWithSetPedOut()
{
    CPed* ped = g_ExitPed;
    CVehicle* veh = g_ExitVeh;
    if (!ped || !veh)
    {
        ResetExitState();
        return;
    }

    CVector spawnPos = GetOffSpawnPos(veh);
    float heading = atan2f(
        -veh->GetMatrix().GetForward().x,
        veh->GetMatrix().GetForward().y);

    const bool continueB = g_GetOffUseB &&
        (CTimer::m_snTimeInMilliseconds < g_GetOffAnimEndMs);

    // Stop vehicle follow / seat lock
    g_GetOffPlaying = false;
    g_ExitVeh = nullptr;

    CTaskSimpleCarSetPedOut* setOut = new CTaskSimpleCarSetPedOut(veh, 0, false);
    reinterpret_cast<bool(__thiscall*)(CTaskSimpleCarSetPedOut*, CPed*)>(0x647D10)(setOut, ped);
    delete setOut;

    ped->SetPosn(spawnPos);
    ped->SetHeading(heading);
    ped->GetMatrix().UpdateRW();
    ped->UpdateRwFrame();

    if (ped == FindPlayerPed())
        TheCamera.RestoreWithJumpCut();

    if (continueB)
    {
        g_GetOffAnimAfter = true;
        g_ExitPed = ped;
        MaintainGetOffBAnim(ped);
    }
    else
    {
        ResetExitState();
    }
}

static void __fastcall HookedSetPedPositionInCar(CPed* self, void* /*edx*/)
{
    memcpy(reinterpret_cast<void*>(0x5DF910), g_SetPedPosOriginal, 5);
    reinterpret_cast<void(__thiscall*)(CPed*)>(0x5DF910)(self);
    plugin::patch::RedirectJump(0x5DF910, (void*)HookedSetPedPositionInCar);

    if (!g_GetOffPlaying || self != g_ExitPed || !g_ExitVeh)
        return;

    CVector pos = GetOffHoldPos(g_ExitVeh);
    self->SetPosn(pos);
    self->GetMatrix().UpdateRW();
    self->UpdateRwFrame();
}

static CTask* __fastcall HookedLeaveBoatCreateFirst(void* self, void* /*edx*/, CPed* ped)
{
    CVehicle* veh = *reinterpret_cast<CVehicle**>(reinterpret_cast<char*>(self) + 0xC);

    if (veh && veh->m_nModelIndex == 473)
    {
        if (!g_GetOffPlaying && !g_GetOffAnimAfter && ped)
        {
            ResetExitState();
            g_GetOffPlaying = true;
            g_ExitPed = ped;
            g_ExitVeh = veh;
            PlayGetOffOnce(ped);
        }
        return nullptr;
    }

    memcpy(reinterpret_cast<void*>(0x642270), g_LeaveBoatFirstOriginal, 5);
    CTask* result = reinterpret_cast<CTask * (__thiscall*)(void*, CPed*)>(0x642270)(self, ped);
    plugin::patch::RedirectJump(0x642270, (void*)HookedLeaveBoatCreateFirst);
    return result;
}

static void __fastcall HookedBoatRender(CBoat* self, void* /*edx*/)
{
    if (self && self->m_nModelIndex == 473)
    {
        self->m_nTimeTillWeNeedThisCar = CTimer::m_snTimeInMilliseconds + 3000;
        reinterpret_cast<void(__thiscall*)(CVehicle*)>(0x6D0E60)(self);
        return;
    }
    memcpy(reinterpret_cast<void*>(0x6F0210), g_BoatRenderOriginal, 5);
    reinterpret_cast<void(__thiscall*)(CBoat*)>(0x6F0210)(self);
    plugin::patch::RedirectJump(0x6F0210, (void*)HookedBoatRender);
}

static CRideAnimData* __fastcall HookedGetRide(CVehicle* self, void* /*edx*/)
{
    if (self && self->m_nModelIndex == 473)
        return &g_RideData;
    memcpy(reinterpret_cast<void*>(0x871F3C), g_GetRideOriginal, 5);
    CRideAnimData* result = reinterpret_cast<CRideAnimData * (__thiscall*)(CVehicle*)>(0x871F3C)(self);
    plugin::patch::RedirectJump(0x871F3C, (void*)HookedGetRide);
    return result;
}

class JetSkiWayA
{
public:
    JetSkiWayA()
    {
        Events::initGameEvent += []
            {
                CVehicleModelInfo* dinghy = reinterpret_cast<CVehicleModelInfo*>(CModelInfo::GetModelInfo(473));
                CVehicleModelInfo* quad = reinterpret_cast<CVehicleModelInfo*>(CModelInfo::GetModelInfo(471));
                if (!dinghy || !quad)
                    return;

                dinghy->m_pAnimBlock = quad->m_pAnimBlock;
                tHandlingData& dh = gHandlingDataMgr.m_aVehicleHandling[dinghy->m_nHandlingId];
                tHandlingData& qh = gHandlingDataMgr.m_aVehicleHandling[quad->m_nHandlingId];
                dh.m_nAnimGroup = qh.m_nAnimGroup;
                dh.m_bSitInBoat = true;
                dh.m_bNoExhaust = false;

                g_RideData = {};
                g_RideData.m_nAnimGroup = 10;
                g_BikeHandling = FindQuadBikeHandling(quad->m_nHandlingId);

                if (!g_BoatRenderHooked)
                {
                    memcpy(g_BoatRenderOriginal, reinterpret_cast<void*>(0x6F0210), 5);
                    plugin::patch::RedirectJump(0x6F0210, (void*)HookedBoatRender);
                    g_BoatRenderHooked = true;
                }
                if (!g_GetRideHooked)
                {
                    memcpy(g_GetRideOriginal, reinterpret_cast<void*>(0x871F3C), 5);
                    plugin::patch::RedirectJump(0x871F3C, (void*)HookedGetRide);
                    g_GetRideHooked = true;
                }
                if (!g_LeaveBoatFirstHooked)
                {
                    memcpy(g_LeaveBoatFirstOriginal, reinterpret_cast<void*>(0x642270), 5);
                    plugin::patch::RedirectJump(0x642270, (void*)HookedLeaveBoatCreateFirst);
                    g_LeaveBoatFirstHooked = true;
                }
                if (!g_SetPedPosHooked)
                {
                    memcpy(g_SetPedPosOriginal, reinterpret_cast<void*>(0x5DF910), 5);
                    plugin::patch::RedirectJump(0x5DF910, (void*)HookedSetPedPositionInCar);
                    g_SetPedPosHooked = true;
                }

                ResetExitState();
                g_GameReady = true;
            };

        Events::gameProcessEvent += []
            {
                if (!g_GameReady)
                    return;

                unsigned int now = CTimer::m_snTimeInMilliseconds;

                // 1) Leave dinghy (stop follow) — hold timer only
                if (g_GetOffPlaying &&
                    g_GetOffHoldEndMs != 0 &&
                    now >= g_GetOffHoldEndMs)
                {
                    FinishWithSetPedOut();
                }

                // 2) After eject: keep B anim until anim timer
                if (g_GetOffAnimAfter && g_ExitPed)
                {
                    if (now >= g_GetOffAnimEndMs)
                    {
                        ResetExitState();
                    }
                    else
                    {
                        MaintainGetOffBAnim(g_ExitPed);
                    }
                }

                for (CVehicle* veh : CPools::ms_pVehiclePool)
                {
                    if (!veh || veh->m_nModelIndex != 473)
                        continue;
                    ManualExhaustFromFrame(veh);
                    if (veh->m_pDriver)
                        ApplyBaseAndRider(veh, veh->m_pDriver);
                }
            };
    }
} jetSkiWayA;
