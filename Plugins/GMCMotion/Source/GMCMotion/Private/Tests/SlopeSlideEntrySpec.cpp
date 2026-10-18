// Specs for UGMCMotion::ComputeSlopeSlideEntryVelocity — the velocity a pawn carries into a slope slide from the ground.
//
// Measured 2026-10-10 (CLEAR PIE and upstream GMCLocomotion, same ramp): running in at 375 cm/s, the first slide frame
// read 239 horizontal / 301 along the slope. PhysicsSlopeSlide projects Velocity onto the floor plane, and a grounded
// pawn's Velocity is horizontal, so the projection cost a factor of cos(slope): 375 * cos 35 = 307. The pawn visibly
// stalled while the slide entry clip was still upright; sprinting in (552 -> 452) hid it.
// Ported from CLEAR (2026-10-10).

#include "Misc/AutomationTest.h"
#include "Components/GMCMotion.h"

#if WITH_AUTOMATION_WORKER

namespace
{
	/** Floor normal of a slope that descends toward +X at the given angle. */
	FVector DownhillPlusX(double AngleDeg)
	{
		const double A = FMath::DegreesToRadians(AngleDeg);
		return FVector(FMath::Sin(A), 0.0, FMath::Cos(A));
	}
}

BEGIN_DEFINE_SPEC(FSlopeSlideEntrySpec,
	"GMCMotion.Unit.SlopeSlideEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FSlopeSlideEntrySpec)

void FSlopeSlideEntrySpec::Define()
{
	Describe("ComputeSlopeSlideEntryVelocity", [this]()
	{
		It("keeps the horizontal speed of a pawn running straight down a 35 degree slope", [this]()
		{
			const FVector N = DownhillPlusX(35.0);
			const FVector V = UGMCMotion::ComputeSlopeSlideEntryVelocity(FVector(375.0, 0.0, 0.0), N);
			TestEqual("horizontal X kept", V.X, 375.0, 0.01);
			TestEqual("no sideways speed", V.Y, 0.0, 0.01);
			TestTrue("heads downhill", V.Z < 0.0);
			TestEqual("lies on the floor plane", V | N, 0.0, 0.01);
			// The old projection gave 307 along the slope; lifted onto the plane it is 375 / cos 35.
			TestEqual("along-slope speed", V.Size(), 375.0 / FMath::Cos(FMath::DegreesToRadians(35.0)), 0.05);
		});

		It("leaves speed across the slope untouched", [this]()
		{
			const FVector N = DownhillPlusX(40.0);
			const FVector V = UGMCMotion::ComputeSlopeSlideEntryVelocity(FVector(0.0, 250.0, 0.0), N);
			TestEqual("Y kept", V.Y, 250.0, 0.01);
			TestEqual("no vertical component", V.Z, 0.0, 0.01);
		});

		It("goes up the plane for a pawn moving uphill", [this]()
		{
			const FVector N = DownhillPlusX(35.0);
			const FVector V = UGMCMotion::ComputeSlopeSlideEntryVelocity(FVector(-200.0, 0.0, 0.0), N);
			TestEqual("horizontal X kept", V.X, -200.0, 0.01);
			TestTrue("heads uphill", V.Z > 0.0);
			TestEqual("lies on the floor plane", V | N, 0.0, 0.01);
		});

		It("ignores any vertical component the ground velocity carries", [this]()
		{
			const FVector N = DownhillPlusX(35.0);
			const FVector A = UGMCMotion::ComputeSlopeSlideEntryVelocity(FVector(300.0, 0.0, 0.0), N);
			const FVector B = UGMCMotion::ComputeSlopeSlideEntryVelocity(FVector(300.0, 0.0, -80.0), N);
			TestTrue("same result", A.Equals(B, 0.01));
		});

		It("returns the input unchanged on a flat floor", [this]()
		{
			const FVector V = UGMCMotion::ComputeSlopeSlideEntryVelocity(FVector(375.0, 120.0, 0.0), FVector::UpVector);
			TestTrue("unchanged", V.Equals(FVector(375.0, 120.0, 0.0), 0.01));
		});

		It("falls back to projecting onto a near-vertical surface instead of blowing up", [this]()
		{
			const FVector N = DownhillPlusX(89.0);
			const FVector V = UGMCMotion::ComputeSlopeSlideEntryVelocity(FVector(375.0, 0.0, 0.0), N);
			TestTrue("finite", !V.ContainsNaN() && V.Size() < 1000.0);
			TestEqual("lies on the floor plane", V | N, 0.0, 0.01);
		});
	});
}

#endif // WITH_AUTOMATION_WORKER
