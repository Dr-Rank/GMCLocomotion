// Specs for the requested slope slide decisions (bSlopeSlideAutoStart off): a slide starts on a request (e.g. a crouch
// press while sprinting) downhill on a floor of SlopeSlideTriggerMinAngle or more, and releasing the request ends it.
// Ported from CLEAR (2026-10-10).

#include "Misc/AutomationTest.h"
#include "Components/GMCMotion.h"

#if WITH_AUTOMATION_WORKER

namespace
{
	constexpr float MinAngle = 20.f;
	constexpr double MinSpeed = 450.0;
	constexpr float StopAngle = 28.f;
	constexpr double ExitSpeed = 250.0;
}

BEGIN_DEFINE_SPEC(FSlopeSlideTriggerSpec,
	"GMCMotion.Unit.SlopeSlideTrigger",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FSlopeSlideTriggerSpec)

void FSlopeSlideTriggerSpec::Define()
{
	Describe("ShouldStartRequestedSlopeSlide", [this]()
	{
		It("starts when requested, grounded, steep enough and sprinting downhill", [this]()
		{
			TestTrue("starts", UGMCMotion::ShouldStartRequestedSlopeSlide(true, true, 25.f, MinAngle, 552.0, MinSpeed));
		});

		It("never starts without a request, however steep and fast", [this]()
		{
			TestFalse("no request", UGMCMotion::ShouldStartRequestedSlopeSlide(false, true, 40.f, MinAngle, 800.0, MinSpeed));
		});

		It("does not start on a slope gentler than the trigger angle", [this]()
		{
			TestFalse("19.9 deg", UGMCMotion::ShouldStartRequestedSlopeSlide(true, true, 19.9f, MinAngle, 552.0, MinSpeed));
			TestTrue("exactly 20 deg", UGMCMotion::ShouldStartRequestedSlopeSlide(true, true, 20.f, MinAngle, 552.0, MinSpeed));
		});

		It("does not start at running speed (only sprinting)", [this]()
		{
			TestFalse("run 375", UGMCMotion::ShouldStartRequestedSlopeSlide(true, true, 30.f, MinAngle, 375.0, MinSpeed));
		});

		It("does not start moving uphill or across the slope", [this]()
		{
			TestFalse("uphill", UGMCMotion::ShouldStartRequestedSlopeSlide(true, true, 30.f, MinAngle, -552.0, MinSpeed));
			TestFalse("across", UGMCMotion::ShouldStartRequestedSlopeSlide(true, true, 30.f, MinAngle, 0.0, MinSpeed));
		});

		It("does not start in the air", [this]()
		{
			TestFalse("airborne", UGMCMotion::ShouldStartRequestedSlopeSlide(true, false, 30.f, MinAngle, 552.0, MinSpeed));
		});
	});

	Describe("ShouldKeepSlopeSlide", [this]()
	{
		It("keeps sliding while held on a steep slope", [this]()
		{
			TestTrue("held, steep", UGMCMotion::ShouldKeepSlopeSlide(true, true, 35.f, StopAngle, 100.0, ExitSpeed));
		});

		It("keeps sliding while held and still fast on a gentle slope (run-out)", [this]()
		{
			TestTrue("held, fast", UGMCMotion::ShouldKeepSlopeSlide(true, true, 10.f, StopAngle, 400.0, ExitSpeed));
		});

		It("ends on a gentle slope once slow, even while held", [this]()
		{
			TestFalse("held, slow, gentle", UGMCMotion::ShouldKeepSlopeSlide(true, true, 10.f, StopAngle, 200.0, ExitSpeed));
		});

		It("ends as soon as crouch is released when release-to-stop is on", [this]()
		{
			TestFalse("released, steep and fast", UGMCMotion::ShouldKeepSlopeSlide(false, true, 40.f, StopAngle, 800.0, ExitSpeed));
		});

		It("ignores the release when release-to-stop is off", [this]()
		{
			TestTrue("released, steep", UGMCMotion::ShouldKeepSlopeSlide(false, false, 40.f, StopAngle, 100.0, ExitSpeed));
		});
	});
}

#endif // WITH_AUTOMATION_WORKER
