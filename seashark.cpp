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
#include "CWaterLevel.h"
#include "Fx_c.h"
#include "FxPrtMult_c.h"
#include <cstring>

using namespace plugin;

enum {
    QUAD_RIDE_0 = 194
};

static CRideAnimData g_RideData;
static tBikeHandlingData* g_BikeHandling = nullptr;
static const float QUAD_HBSTEER_ANIM_MULT = -0.2f;

// Separate pitch strength — start low, raise slowly
static const float LEAN_PITCH_FWD = 0.003f; // forward
static const float LEAN_PITCH_BACK = 0.001f; // back

static unsigned char g_BoatRenderOriginal[5];
static bool g_BoatRenderHooked = false;
static bool g_GameReady = false;

static tBikeHandlingData* FindQuadBikeHandling(unsigned char generalHandlingId)
{
    for (int i = 0; i < 13; ++i)
    {
        if (gHandlingDataMgr.m_aBikeHandling[i].m_nVehicleId == generalHandlingId)
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

    // Moved up from the bottom of the function: this is the common case
    // (no gas pressed most frames), so bail before doing the frame lookup
    // and velocity math instead of after.
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
    {
        underWater = true;
    }

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
                g_BikeHandling = FindQuadBikeHandling(static_cast<unsigned char>(quad->m_nHandlingId));

                if (!g_BoatRenderHooked)
                {
                    memcpy(g_BoatRenderOriginal, reinterpret_cast<void*>(0x6F0210), 5);
                    plugin::patch::RedirectJump(0x6F0210, (void*)HookedBoatRender);
                    g_BoatRenderHooked = true;
                }

                g_GameReady = true;
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
                    if (!veh || veh->m_nModelIndex != 473)
                        continue;

                    if (veh->m_pHandlingData)
                    {
                        veh->m_pHandlingData->m_bSitInBoat = true;
                        veh->m_pHandlingData->m_bNoExhaust = false;
                    }

                    ManualExhaustFromFrame(veh);

                    if (veh->m_pDriver)
                        ApplyBaseAndRider(veh, veh->m_pDriver);
                }
            };
    }
} jetSkiWayA;
