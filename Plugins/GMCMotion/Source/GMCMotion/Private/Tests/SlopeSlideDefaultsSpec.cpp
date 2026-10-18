// Specs for what an existing GMCMotion user gets by default after the requested-slide and camera-limit additions
// (2026-10-18): unchanged behaviour. The slide still starts by itself, and no camera limit is applied until a range is
// set. Also the camera yaw window, which must stay unnormalised (APlayerCameraManager::LimitViewYaw clamps through
// FMath::ClampAngle, which handles a window crossing 0/360).

#include "Misc/AutomationTest.h"
#include "Components/GMCMotion.h"

#if WITH_AUTOMATION_WORKER

BEGIN_DEFINE_SPEC(FSlopeSlideDefaultsSpec,
	"GMCMotion.Unit.SlopeSlideDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FSlopeSlideDefaultsSpec)

void FSlopeSlideDefaultsSpec::Define()
{
	Describe("Defaults", [this]()
	{
		It("keeps the automatic slide on, so existing users see no change", [this]()
		{
			const UGMCMotion* Defaults = GetDefault<UGMCMotion>();
			TestTrue("bSlopeSlideAutoStart", Defaults->bSlopeSlideAutoStart);
			TestTrue("bSlopeSlideEnabled", Defaults->bSlopeSlideEnabled);
		});

		It("applies no camera limit until a range is set", [this]()
		{
			const UGMCMotion* Defaults = GetDefault<UGMCMotion>();
			TestEqual("SlopeSlideCameraYawRange", Defaults->SlopeSlideCameraYawRange, 0.f);
			TestEqual("KnockdownCameraYawRange", Defaults->KnockdownCameraYawRange, 0.f);
			TestTrue("no camera condition", Defaults->CameraYawLimitConditionProperty.IsNone());
		});

		It("reads no request bool until one is named", [this]()
		{
			TestTrue("SlopeSlideRequestProperty", GetDefault<UGMCMotion>()->SlopeSlideRequestProperty.IsNone());
		});

		It("ignores the release while the automatic slide is on (the effective switch)", [this]()
		{
			// UpdateSlopeSlideMode passes bSlopeSlideEndOnRelease && !bSlopeSlideAutoStart, i.e. false with the defaults.
			const UGMCMotion* Defaults = GetDefault<UGMCMotion>();
			const bool bEffective = Defaults->bSlopeSlideEndOnRelease && !Defaults->bSlopeSlideAutoStart;
			TestFalse("effective end-on-release", bEffective);
			TestTrue("an unrequested slide keeps going on a steep slope",
				UGMCMotion::ShouldKeepSlopeSlide(false, bEffective, 40.f, Defaults->SlopeSlideStopAngle, 100.0,
					Defaults->SlopeSlideExitSpeed));
		});
	});

	Describe("ComputeCameraYawWindow", [this]()
	{
		It("centres the window on the facing", [this]()
		{
			float Min = 0.f, Max = 0.f;
			UGMCMotion::ComputeCameraYawWindow(180.f, 90.f, Min, Max);
			TestEqual("min", Min, 90.f);
			TestEqual("max", Max, 270.f);
		});

		It("does not normalise a window crossing 0", [this]()
		{
			float Min = 0.f, Max = 0.f;
			UGMCMotion::ComputeCameraYawWindow(10.f, 90.f, Min, Max);
			TestEqual("min", Min, -80.f);
			TestEqual("max", Max, 100.f);
			// What LimitViewYaw does with it: a yaw of 350 (= -10) is inside, 200 is clamped to the nearer edge.
			TestEqual("350 inside", (float)FRotator::NormalizeAxis(FMath::ClampAngle(350.f, Min, Max)), -10.f, 1.e-3f);
			TestEqual("200 clamped", (float)FRotator::NormalizeAxis(FMath::ClampAngle(200.f, Min, Max)), -80.f, 1.e-3f);
		});

		It("does not normalise a window crossing 180", [this]()
		{
			float Min = 0.f, Max = 0.f;
			UGMCMotion::ComputeCameraYawWindow(-170.f, 30.f, Min, Max);
			TestEqual("min", Min, -200.f);
			TestEqual("max", Max, -140.f);
			TestEqual("175 inside", (float)FRotator::NormalizeAxis(FMath::ClampAngle(175.f, Min, Max)), 175.f, 1.e-3f);
		});
	});
}

#endif // WITH_AUTOMATION_WORKER
