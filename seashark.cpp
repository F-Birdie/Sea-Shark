#include "plugin.h"
#include "CModelInfo.h"
#include "CVehicleModelInfo.h"
#include "CVehicle.h"
#include "CBoat.h"
#include "CPed.h"
#include "CAnimManager.h"
#include "CAnimBlendAssociation.h"
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
#include <cstring>

using namespace plugin;

enum {
    QUAD_RIDE_0 = 194
};

static CRideAnimData g_RideData;
static tBikeHandlingData* g_BikeHandling = nullptr;
static const float QUAD_HBSTEER_ANIM_MULT = -0.2f;

// KeepWaterOut hook state
static unsigned char g_BoatRenderOriginal[5];
static bool g_BoatRenderHooked = false;

static RwFrame* FindHandlebarFrame(RpClump* clump)
{
    if (!clump)
        return nullptr;
    return CClumpModelInfo::GetFrameFromName(clump, const_cast<char*>("handlebars"));
}

static void UpdateHandlebars(CVehicle* veh)
{
    if (!veh || !veh->m_pRwClump)
        return;

    RwFrame* hb = FindHandlebarFrame(reinterpret_cast<RpClump*>(veh->m_pRwClump));
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

    CPad* pad = CPad::GetPad(0);
    if (pad)
    {
        float target = float(-pad->GetSteeringUpDown()) / 128.0f;
        float* leanFwd = reinterpret_cast<float*>(reinterpret_cast<char*>(&g_RideData) + 0x10);
        *leanFwd += (target - *leanFwd) * CTimer::ms_fTimeStep / 5.0f;
        if (*leanFwd > 1.0f) *leanFwd = 1.0f;
        if (*leanFwd < -1.0f) *leanFwd = -1.0f;
    }
}

static void ApplyBaseAndRider(CVehicle* veh, CPed* driver)
{
    if (!veh || !driver || !driver->m_pRwClump || !g_BikeHandling)
        return;

    RpClump* clump = reinterpret_cast<RpClump*>(driver->m_pRwClump);

    // Locked base sit
    CAnimBlendAssociation* base = CAnimManager::BlendAnimation(clump, 10, QUAD_RIDE_0, 1000.0f);
    if (base)
    {
        base->SetBlend(1.0f, 0.0f);
        base->m_fCurrentTime = 0.0f;
    }

    // Rider anims (steering) on top
    UpdateRideDataLikeQuad(veh);

    using Fn = void(__cdecl*)(CPed*, CVehicle*, CRideAnimData*, tBikeHandlingData*, short);
    static Fn ProcessRiderAnims = (Fn)0x6B7280;
    ProcessRiderAnims(driver, veh, &g_RideData, g_BikeHandling, 0);

    // Handlebars after rider data is updated
    UpdateHandlebars(veh);
}

// ============================================================
// Disable KeepWaterOut square for Dinghy (same idea as Skimmer)
// CBoat::Render @ 0x6F0210
// ============================================================
static void __fastcall HookedBoatRender(CBoat* self, void* /*edx*/)
{
    if (self && self->m_nModelIndex == 473)
    {
        self->m_nTimeTillWeNeedThisCar = CTimer::m_snTimeInMilliseconds + 3000;
        // Draw the boat mesh only – skip the invisible water-out quad
        reinterpret_cast<void(__thiscall*)(CVehicle*)>(0x6D0E60)(self);
        return;
    }

    // Other boats: original Render
    memcpy(reinterpret_cast<void*>(0x6F0210), g_BoatRenderOriginal, 5);
    reinterpret_cast<void(__thiscall*)(CBoat*)>(0x6F0210)(self);
    plugin::patch::RedirectJump(0x6F0210, (void*)HookedBoatRender);
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

                g_RideData = {};
                g_RideData.m_nAnimGroup = 10;

                unsigned int idx = quad->m_nHandlingId;
                if (idx >= 13) idx = 0;
                g_BikeHandling = &gHandlingDataMgr.m_aBikeHandling[idx];

                // Install KeepWaterOut disable once
                if (!g_BoatRenderHooked)
                {
                    memcpy(g_BoatRenderOriginal, reinterpret_cast<void*>(0x6F0210), 5);
                    plugin::patch::RedirectJump(0x6F0210, (void*)HookedBoatRender);
                    g_BoatRenderHooked = true;
                }
            };

        static auto origGetRide = (CRideAnimData * (__thiscall*)(CVehicle*))0x871F3C;
        plugin::patch::RedirectJump(0x871F3C, (void*)+[](CVehicle* v) -> CRideAnimData*
            {
                if (v && v->m_nModelIndex == 473)
                    return &g_RideData;
                return origGetRide(v);
            });

        Events::gameProcessEvent += []
            {
                for (CVehicle* veh : CPools::ms_pVehiclePool)
                {
                    if (!veh || veh->m_nModelIndex != 473 || !veh->m_pDriver)
                        continue;

                    if (veh->m_pHandlingData)
                        veh->m_pHandlingData->m_bSitInBoat = true;

                    ApplyBaseAndRider(veh, veh->m_pDriver);
                }
            };
    }
} jetSkiWayA;
