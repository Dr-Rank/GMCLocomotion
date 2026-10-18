// Specs for UGMCMotion::ComputeGetUpViewYaw. Ported from CLEAR (CLEAR.Ragdoll.GetUpViewYaw).

#include "Components/GMCMotion.h"
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_WORKER

// The first-person view turning to the get-up facing (UGMCMotion::GetUpViewAlignTime): it must start where
// the view was, end exactly on the body's facing, and always turn the short way round - a get-up facing 234 from a view
// at 180 is a 54 deg turn, and one facing -170 from a view at 170 is 20 deg, not 340.
BEGIN_DEFINE_SPEC(FGetUpViewYawSpec, "GMCMotion.Unit.GetUpViewYaw",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
	static float Yaw(float From, float To, float Alpha) { return UGMCMotion::ComputeGetUpViewYaw(From, To, Alpha); }
END_DEFINE_SPEC(FGetUpViewYawSpec)

void FGetUpViewYawSpec::Define()
{
	It("starts on the view and ends on the facing", [this]()
	{
		TestEqual("start", Yaw(180.f, -126.3f, 0.f), 180.f, 1.e-3f);
		TestEqual("end", Yaw(180.f, -126.3f, 1.f), -126.3f, 1.e-3f);
	});

	It("turns the short way round across 180", [this]()
	{
		// 170 -> -170 is +20 through 180, never -340.
		const float Mid = Yaw(170.f, -170.f, 0.5f);
		TestEqual("half way is 180", FMath::Abs(Mid), 180.f, 1.e-2f);
	});

	It("turns the short way round across 0", [this]()
	{
		TestEqual("half way is 0", Yaw(-20.f, 20.f, 0.5f), 0.f, 1.e-3f);
	});

	It("eases: little movement at the start, most of the turn by three quarters", [this]()
	{
		TestTrue("10% in, under 10% of the turn", Yaw(0.f, 90.f, 0.1f) < 9.f);
		TestTrue("75% in, over 75% of the turn", Yaw(0.f, 90.f, 0.75f) > 67.5f);
	});

	It("clamps alpha outside 0..1", [this]()
	{
		TestEqual("below", Yaw(10.f, 50.f, -1.f), 10.f, 1.e-3f);
		TestEqual("above", Yaw(10.f, 50.f, 2.f), 50.f, 1.e-3f);
	});

	It("does not move when already facing that way", [this]()
	{
		TestEqual("same", Yaw(45.f, 405.f, 0.5f), 45.f, 1.e-3f);
	});
}

#endif // WITH_AUTOMATION_WORKER
