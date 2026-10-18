// Copyright 2026 Execute Games, all rights reserved

#include "GMCMotionRagdollComponent.h"
#include "Misc/AutomationTest.h"
#include "Serialization/BitReader.h"
#include "Serialization/BitWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

// The wire format of the streamed ragdoll (unreliable multicast at PoseSendRate, and the replicated FinalPose): a pelvis
// in world space, every other body as an int16 offset of 0.5 cm steps, rotations as three int16 components.
BEGIN_DEFINE_SPEC(FGMCMotionRagdollNetPoseSpec, "GMCMotion.Unit.RagdollNetPose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
	static bool RoundTrip(const FGMCMotionRagdollNetPose& In, FGMCMotionRagdollNetPose& Out, bool& bOutSuccess)
	{
		FBitWriter Writer(0, true);
		bool bWriteOk = false;
		const_cast<FGMCMotionRagdollNetPose&>(In).NetSerialize(Writer, nullptr, bWriteOk);
		FBitReader Reader(Writer.GetData(), Writer.GetNumBits());
		bOutSuccess = false;
		Out.NetSerialize(Reader, nullptr, bOutSuccess);
		return bWriteOk && !Reader.IsError();
	}

	static FGMCMotionRagdollNetPose MakePose(int32 Count, uint8 RootIndex)
	{
		FGMCMotionRagdollNetPose Pose;
		Pose.ServerTime = 1234.5678;
		Pose.RootIndex = RootIndex;
		const FVector Pelvis(-4844.37, 4363.91, 112.25);
		for (int32 i = 0; i < Count; ++i)
		{
			const FRotator Rotation(i * 13.7 - 80., i * 29.3 - 170., i * 7.1 - 45.);
			const FVector Offset = i == RootIndex ? FVector::ZeroVector : FVector(FMath::Sin(i * 1.3) * 60., FMath::Cos(i * 0.7) * 45., i * 4.3 - 90.);
			Pose.Bodies.Add(FTransform(Rotation, Pelvis + Offset));
		}
		return Pose;
	}
END_DEFINE_SPEC(FGMCMotionRagdollNetPoseSpec)

void FGMCMotionRagdollNetPoseSpec::Define()
{
	It("round-trips a full body to within the quantization (0.25 cm offsets, 0.1 cm pelvis, < 0.01 rad)", [this]()
	{
		const FGMCMotionRagdollNetPose In = MakePose(22, 3);
		FGMCMotionRagdollNetPose Out;
		bool bSuccess = false;
		TestTrue("archive ok", RoundTrip(In, Out, bSuccess));
		TestTrue("NetSerialize success", bSuccess);
		TestEqual("server time", Out.ServerTime, In.ServerTime);
		TestEqual("root index", Out.RootIndex, In.RootIndex);
		if (!TestEqual("body count", Out.Bodies.Num(), In.Bodies.Num()))
		{
			return;
		}
		double WorstLocation = 0., WorstAngle = 0.;
		for (int32 i = 0; i < In.Bodies.Num(); ++i)
		{
			WorstLocation = FMath::Max(WorstLocation, FVector::Dist(In.Bodies[i].GetLocation(), Out.Bodies[i].GetLocation()));
			WorstAngle = FMath::Max(WorstAngle, In.Bodies[i].GetRotation().AngularDistance(Out.Bodies[i].GetRotation()));
		}
		// Pelvis 0.05 cm per axis (SerializePackedVector<10>) plus the offset's 0.25 cm per axis.
		TestTrue(FString::Printf(TEXT("worst location error %.4f cm <= 0.52"), WorstLocation), WorstLocation <= 0.52);
		TestTrue(FString::Printf(TEXT("worst rotation error %.5f rad < 0.01"), WorstAngle), WorstAngle < 0.01);
	});

	It("clamps a body further than 163 m from the pelvis instead of wrapping", [this]()
	{
		FGMCMotionRagdollNetPose In = MakePose(4, 0);
		In.Bodies[2].SetLocation(In.Bodies[0].GetLocation() + FVector(20000., 0., 0.));
		FGMCMotionRagdollNetPose Out;
		bool bSuccess = false;
		RoundTrip(In, Out, bSuccess);
		TestTrue("clamped on the far side, not wrapped negative", Out.Bodies[2].GetLocation().X - Out.Bodies[0].GetLocation().X > 16000.);
	});

	It("reads an empty pose as success with no bodies", [this]()
	{
		FGMCMotionRagdollNetPose Out = MakePose(5, 0);
		bool bSuccess = false;
		TestTrue("archive ok", RoundTrip(FGMCMotionRagdollNetPose(), Out, bSuccess));
		TestTrue("success", bSuccess);
		TestEqual("no bodies", Out.Bodies.Num(), 0);
	});

	It("reports failure for a root index outside the bodies", [this]()
	{
		FGMCMotionRagdollNetPose In = MakePose(4, 0);
		In.RootIndex = 9;
		FGMCMotionRagdollNetPose Out;
		bool bSuccess = true;
		RoundTrip(In, Out, bSuccess);
		TestFalse("success flag", bSuccess);
	});
}

#endif
