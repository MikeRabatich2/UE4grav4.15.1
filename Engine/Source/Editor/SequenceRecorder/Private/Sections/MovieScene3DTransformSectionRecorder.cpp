// Copyright 1998-2017 Epic Games, Inc. All Rights Reserved.

#include "Sections/MovieScene3DTransformSectionRecorder.h"
#include "Misc/ScopedSlowTask.h"
#include "GameFramework/Character.h"
#include "KeyParams.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "SequenceRecorder.h"
#include "SequenceRecorderSettings.h"

TSharedPtr<IMovieSceneSectionRecorder> FMovieScene3DTransformSectionRecorderFactory::CreateSectionRecorder(const FActorRecordingSettings& InActorRecordingSettings) const
{
	return nullptr;
}

TSharedPtr<FMovieScene3DTransformSectionRecorder> FMovieScene3DTransformSectionRecorderFactory::CreateSectionRecorder(bool bRecordTransforms, TSharedPtr<class FMovieSceneAnimationSectionRecorder> InAnimRecorder) const
{
	return MakeShareable(new FMovieScene3DTransformSectionRecorder(bRecordTransforms, InAnimRecorder));
}

bool FMovieScene3DTransformSectionRecorderFactory::CanRecordObject(UObject* InObjectToRecord) const
{
	if(USceneComponent* SceneComponent = Cast<USceneComponent>(InObjectToRecord))
	{
		// Dont record the root component transforms as this will be taken into account by the actor transform track
		// Also dont record transforms of skeletal mesh components as they will be taken into account in the actor transform
		bool bIsCharacterSkelMesh = false;
		if (SceneComponent->IsA<USkeletalMeshComponent>() && SceneComponent->GetOwner()->IsA<ACharacter>())
		{
			ACharacter* Character = CastChecked<ACharacter>(SceneComponent->GetOwner());
			bIsCharacterSkelMesh = SceneComponent == Character->GetMesh();
		}

		return (SceneComponent != SceneComponent->GetOwner()->GetRootComponent() && !bIsCharacterSkelMesh);
	}
	else 
	{
		return InObjectToRecord->IsA<AActor>();
	}
}

void FMovieScene3DTransformSectionRecorder::CreateSection(UObject* InObjectToRecord, UMovieScene* InMovieScene, const FGuid& Guid, float Time) 
{
	ObjectToRecord = InObjectToRecord;
	bWasAttached = false;
		
	MovieScene = InMovieScene;
	MovieSceneTrack = InMovieScene->AddTrack<UMovieScene3DTransformTrack>(Guid);
	if(MovieSceneTrack.IsValid())
	{
		MovieSceneSection = Cast<UMovieScene3DTransformSection>(MovieSceneTrack->CreateNewSection());

		MovieSceneTrack->AddSection(*MovieSceneSection);

		const bool bUnwindRotation = false;

		DefaultTransform = GetTransformToRecord();
		FVector EulerRotation = DefaultTransform.GetRotation().Rotator().Euler();

		MovieSceneSection->SetDefault(FTransformKey(EKey3DTransformChannel::Translation, EAxis::X, DefaultTransform.GetTranslation().X, bUnwindRotation));
		MovieSceneSection->SetDefault(FTransformKey(EKey3DTransformChannel::Translation, EAxis::Y, DefaultTransform.GetTranslation().Y, bUnwindRotation));
		MovieSceneSection->SetDefault(FTransformKey(EKey3DTransformChannel::Translation, EAxis::Z, DefaultTransform.GetTranslation().Z, bUnwindRotation));

		MovieSceneSection->SetDefault(FTransformKey(EKey3DTransformChannel::Rotation, EAxis::X, EulerRotation.X, bUnwindRotation));
		MovieSceneSection->SetDefault(FTransformKey(EKey3DTransformChannel::Rotation, EAxis::Y, EulerRotation.Y, bUnwindRotation));
		MovieSceneSection->SetDefault(FTransformKey(EKey3DTransformChannel::Rotation, EAxis::Z, EulerRotation.Z, bUnwindRotation));

		MovieSceneSection->SetDefault(FTransformKey(EKey3DTransformChannel::Scale, EAxis::X, DefaultTransform.GetScale3D().X, bUnwindRotation));
		MovieSceneSection->SetDefault(FTransformKey(EKey3DTransformChannel::Scale, EAxis::Y, DefaultTransform.GetScale3D().Y, bUnwindRotation));
		MovieSceneSection->SetDefault(FTransformKey(EKey3DTransformChannel::Scale, EAxis::Z, DefaultTransform.GetScale3D().Z, bUnwindRotation));

		MovieSceneSection->SetStartTime(Time);
		MovieSceneSection->SetIsInfinite(true);
	}
}

void FMovieScene3DTransformSectionRecorder::FinalizeSection()
{
	FScopedSlowTask SlowTask(4.0f, NSLOCTEXT("SequenceRecorder", "ProcessingTransforms", "Processing Transforms"));

	bRecording = false;

	// if we have a valid animation recorder, we need to build our transforms from the animation
	// so we properly synchronize our keyframes
	if(AnimRecorder.IsValid())
	{
		check(BufferedTransforms.Num() == 0);

		UAnimSequence* AnimSequence = AnimRecorder->GetAnimSequence();
		USkeletalMeshComponent* SkeletalMeshComponent = AnimRecorder->GetSkeletalMeshComponent();
		if (SkeletalMeshComponent)
		{
			USkeletalMesh* SkeletalMesh = SkeletalMeshComponent->MasterPoseComponent != nullptr ? SkeletalMeshComponent->MasterPoseComponent->SkeletalMesh : SkeletalMeshComponent->SkeletalMesh;
			if (AnimSequence && SkeletalMesh)
			{
				// find the root bone
				int32 RootIndex = INDEX_NONE;
				USkeleton* AnimSkeleton = AnimSequence->GetSkeleton();
				for (int32 TrackIndex = 0; TrackIndex < AnimSequence->GetRawAnimationData().Num(); ++TrackIndex)
				{
					// verify if this bone exists in skeleton
					int32 BoneTreeIndex = AnimSequence->GetSkeletonIndexFromRawDataTrackIndex(TrackIndex);
					if (BoneTreeIndex != INDEX_NONE)
					{
						int32 BoneIndex = AnimSkeleton->GetMeshBoneIndexFromSkeletonBoneIndex(SkeletalMesh, BoneTreeIndex);
						int32 ParentIndex = SkeletalMesh->RefSkeleton.GetParentIndex(BoneIndex);
						if (ParentIndex == INDEX_NONE)
						{
							// found root
							RootIndex = BoneIndex;
							break;
						}
					}
				}

				check(RootIndex != INDEX_NONE);

				const float StartTime = MovieSceneSection->GetStartTime();

				// we may need to offset the transform here if the animation was not recorded on the root component
				FTransform InvComponentTransform = AnimRecorder->GetComponentTransform().Inverse();

				const FRawAnimSequenceTrack& RawTrack = AnimSequence->GetRawAnimationData()[RootIndex];
				const int32 KeyCount = FMath::Max(FMath::Max(RawTrack.PosKeys.Num(), RawTrack.RotKeys.Num()), RawTrack.ScaleKeys.Num());
				for (int32 KeyIndex = 0; KeyIndex < KeyCount; KeyIndex++)
				{
					FTransform Transform;
					if (RawTrack.PosKeys.IsValidIndex(KeyIndex))
					{
						Transform.SetTranslation(RawTrack.PosKeys[KeyIndex]);
					}
					else if (RawTrack.PosKeys.Num() > 0)
					{
						Transform.SetTranslation(RawTrack.PosKeys[0]);
					}

					if (RawTrack.RotKeys.IsValidIndex(KeyIndex))
					{
						Transform.SetRotation(RawTrack.RotKeys[KeyIndex]);
					}
					else if (RawTrack.RotKeys.Num() > 0)
					{
						Transform.SetRotation(RawTrack.RotKeys[0]);
					}

					if (RawTrack.ScaleKeys.IsValidIndex(KeyIndex))
					{
						Transform.SetScale3D(RawTrack.ScaleKeys[KeyIndex]);
					}
					else if (RawTrack.ScaleKeys.Num() > 0)
					{
						Transform.SetScale3D(RawTrack.ScaleKeys[0]);
					}

					BufferedTransforms.Add(FBufferedTransformKey(InvComponentTransform * Transform, StartTime + AnimSequence->GetTimeAtFrame(KeyIndex)));
				}
			}
		}
	}

	SlowTask.EnterProgressFrame();

	// Try to 're-wind' rotations that look like axis flips
	// We need to do this as a post-process because the recorder cant reliably access 'wound' rotations:
	// - Net quantize may use quaternions.
	// - Scene components cache transforms as quaternions.
	// - Gameplay is free to clamp/fmod rotations as it sees fit.
	int32 TransformCount = BufferedTransforms.Num();
	for(int32 TransformIndex = 0; TransformIndex < TransformCount - 1; TransformIndex++)
	{
		FRotator& Rotator = BufferedTransforms[TransformIndex].WoundRotation;
		FRotator& NextRotator = BufferedTransforms[TransformIndex + 1].WoundRotation;

		FMath::WindRelativeAnglesDegrees(Rotator.Pitch, NextRotator.Pitch);
		FMath::WindRelativeAnglesDegrees(Rotator.Yaw, NextRotator.Yaw);
		FMath::WindRelativeAnglesDegrees(Rotator.Roll, NextRotator.Roll);
	}

	SlowTask.EnterProgressFrame();

	// never unwind rotations
	const bool bUnwindRotation = false;
	// If we are syncing to an animation, use linear interpolation to avoid foot sliding etc. 
	// Otherwise use cubic for better quality (much better for projectiles etc.)
	const EMovieSceneKeyInterpolation Interpolation = AnimRecorder.IsValid() ? EMovieSceneKeyInterpolation::Linear : EMovieSceneKeyInterpolation::Auto;

	// add buffered transforms
	TArray<float> KeyTimes; KeyTimes.SetNum(BufferedTransforms.Num());
	TArray<float> TranslationXValues; TranslationXValues.SetNum(BufferedTransforms.Num());
	TArray<float> TranslationYValues; TranslationYValues.SetNum(BufferedTransforms.Num());
	TArray<float> TranslationZValues; TranslationZValues.SetNum(BufferedTransforms.Num());
	TArray<float> RotationXValues; RotationXValues.SetNum(BufferedTransforms.Num());
	TArray<float> RotationYValues; RotationYValues.SetNum(BufferedTransforms.Num());
	TArray<float> RotationZValues; RotationZValues.SetNum(BufferedTransforms.Num());
	TArray<float> ScaleXValues; ScaleXValues.SetNum(BufferedTransforms.Num());
	TArray<float> ScaleYValues; ScaleYValues.SetNum(BufferedTransforms.Num());
	TArray<float> ScaleZValues; ScaleZValues.SetNum(BufferedTransforms.Num());

	int32 Index = 0;
	for(const FBufferedTransformKey& BufferedTransform : BufferedTransforms)
	{
		const FVector Translation = BufferedTransform.Transform.GetTranslation();
		const FVector Rotation = BufferedTransform.WoundRotation.Euler();
		const FVector Scale = BufferedTransform.Transform.GetScale3D();

		KeyTimes[Index] = BufferedTransform.KeyTime;

		TranslationXValues[Index] = Translation.X;
		TranslationYValues[Index] = Translation.Y;
		TranslationZValues[Index] = Translation.Z;

		RotationXValues[Index] = Rotation.X;
		RotationYValues[Index] = Rotation.Y;
		RotationZValues[Index] = Rotation.Z;

		ScaleXValues[Index] = Scale.X;
		ScaleYValues[Index] = Scale.Y;
		ScaleZValues[Index] = Scale.Z;

		++Index;
	}
	
	MovieSceneSection->GetTranslationCurve(EAxis::X).SetKeys(KeyTimes, TranslationXValues);
	MovieSceneSection->GetTranslationCurve(EAxis::Y).SetKeys(KeyTimes, TranslationYValues);
	MovieSceneSection->GetTranslationCurve(EAxis::Z).SetKeys(KeyTimes, TranslationZValues);
	MovieSceneSection->GetRotationCurve(EAxis::X).SetKeys(KeyTimes, RotationXValues);
	MovieSceneSection->GetRotationCurve(EAxis::Y).SetKeys(KeyTimes, RotationYValues);
	MovieSceneSection->GetRotationCurve(EAxis::Z).SetKeys(KeyTimes, RotationZValues);
	MovieSceneSection->GetScaleCurve(EAxis::X).SetKeys(KeyTimes, ScaleXValues);
	MovieSceneSection->GetScaleCurve(EAxis::Y).SetKeys(KeyTimes, ScaleYValues);
	MovieSceneSection->GetScaleCurve(EAxis::Z).SetKeys(KeyTimes, ScaleZValues);

	BufferedTransforms.Empty();

	SlowTask.EnterProgressFrame();

	// now remove linear keys
	TPair<FRichCurve*, float> CurvesAndTolerances[] =
	{
		TPairInitializer<FRichCurve*, float>(&MovieSceneSection->GetTranslationCurve(EAxis::X), KINDA_SMALL_NUMBER),
		TPairInitializer<FRichCurve*, float>(&MovieSceneSection->GetTranslationCurve(EAxis::Y), KINDA_SMALL_NUMBER),
		TPairInitializer<FRichCurve*, float>(&MovieSceneSection->GetTranslationCurve(EAxis::Z), KINDA_SMALL_NUMBER),
		TPairInitializer<FRichCurve*, float>(&MovieSceneSection->GetRotationCurve(EAxis::X), KINDA_SMALL_NUMBER),
		TPairInitializer<FRichCurve*, float>(&MovieSceneSection->GetRotationCurve(EAxis::Y), KINDA_SMALL_NUMBER),
		TPairInitializer<FRichCurve*, float>(&MovieSceneSection->GetRotationCurve(EAxis::Z), KINDA_SMALL_NUMBER),
		TPairInitializer<FRichCurve*, float>(&MovieSceneSection->GetScaleCurve(EAxis::X), KINDA_SMALL_NUMBER),
		TPairInitializer<FRichCurve*, float>(&MovieSceneSection->GetScaleCurve(EAxis::Y), KINDA_SMALL_NUMBER),
		TPairInitializer<FRichCurve*, float>(&MovieSceneSection->GetScaleCurve(EAxis::Z), KINDA_SMALL_NUMBER),
	};

	const USequenceRecorderSettings* Settings = GetDefault<USequenceRecorderSettings>();
	if (Settings->bReduceKeys) 
	{
		for (TPair<FRichCurve*, float>& CurveAndTolerance : CurvesAndTolerances)
		{
			CurveAndTolerance.Key->RemoveRedundantKeys(CurveAndTolerance.Value);
		}
	}

	// we cant remove redundant tracks if we were attached as the playback relies on update order of
	// transform tracks. Without this track, relative transforms would accumulate.
	if(!bWasAttached)
	{
		// now we have reduced our keys, if we dont have any, remove the section as it is redundant
		if( MovieSceneSection->GetTranslationCurve(EAxis::X).GetNumKeys() == 0 && 
			MovieSceneSection->GetTranslationCurve(EAxis::Y).GetNumKeys() == 0 &&
			MovieSceneSection->GetTranslationCurve(EAxis::Z).GetNumKeys() == 0 &&
			MovieSceneSection->GetRotationCurve(EAxis::X).GetNumKeys() == 0 &&
			MovieSceneSection->GetRotationCurve(EAxis::Y).GetNumKeys() == 0 &&
			MovieSceneSection->GetRotationCurve(EAxis::Z).GetNumKeys() == 0 &&
			MovieSceneSection->GetScaleCurve(EAxis::X).GetNumKeys() == 0 &&
			MovieSceneSection->GetScaleCurve(EAxis::Y).GetNumKeys() == 0 &&
			MovieSceneSection->GetScaleCurve(EAxis::Z).GetNumKeys() == 0)
		{
			if(DefaultTransform.Equals(FTransform::Identity))
			{
				MovieScene->RemoveTrack(*MovieSceneTrack.Get());
			}
		}
	}

	SlowTask.EnterProgressFrame();
}

void FMovieScene3DTransformSectionRecorder::Record(float CurrentTime)
{
	if(ObjectToRecord.IsValid())
	{
		if(USceneComponent* SceneComponent = Cast<USceneComponent>(ObjectToRecord.Get()))
		{
			// dont record non-registered scene components
			if(!SceneComponent->IsRegistered())
			{
				return;
			}
		}

		MovieSceneSection->SetEndTime(CurrentTime);

		if(bRecording)
		{
			// don't record from the transform of the component/actor if we are synchronizing with an animation
			if(!AnimRecorder.IsValid())
			{
				BufferedTransforms.Add(FBufferedTransformKey(GetTransformToRecord(), CurrentTime));
			}
		}
	}
}

FTransform FMovieScene3DTransformSectionRecorder::GetTransformToRecord()
{
	if(USceneComponent* SceneComponent = Cast<USceneComponent>(ObjectToRecord.Get()))
	{
		return SceneComponent->GetRelativeTransform();
	}
	else if(AActor* Actor = Cast<AActor>(ObjectToRecord.Get()))
	{
		bool bCaptureWorldSpaceTransform = false;

		USceneComponent* RootComponent = Actor->GetRootComponent();
		USceneComponent* AttachParent = RootComponent ? RootComponent->GetAttachParent() : nullptr;

		bWasAttached = AttachParent != nullptr;
		if (AttachParent)
		{
			// We capture world space transforms for actors if they're attached, but we're not recording the attachment parent
			bCaptureWorldSpaceTransform = !FSequenceRecorder::Get().FindRecording(AttachParent->GetOwner());
		}

		return (bCaptureWorldSpaceTransform || !RootComponent) ? Actor->ActorToWorld() : RootComponent->GetRelativeTransform();
	}

	return FTransform::Identity;
}
