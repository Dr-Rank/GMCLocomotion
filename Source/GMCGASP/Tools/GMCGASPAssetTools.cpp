#include "Tools/GMCGASPAssetTools.h"

#if WITH_EDITOR
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "EditorAnimUtils.h"
#include "Animation/Skeleton.h"
#include "Misc/StringOutputDevice.h"
#endif

int32 UGMCGASPAssetTools::ReplaceObjectReferences(const TArray<UObject*>& Targets, const TMap<UObject*, UObject*>& ReplacementMap)
{
#if WITH_EDITOR
	TMap<UObject*, UObject*> Map;
	for (const TPair<UObject*, UObject*>& Pair : ReplacementMap)
	{
		if (Pair.Key && Pair.Value && Pair.Key != Pair.Value)
		{
			Map.Add(Pair.Key, Pair.Value);
		}
	}
	if (Map.IsEmpty())
	{
		return 0;
	}

	TSet<UPackage*> Packages;
	for (UObject* Target : Targets)
	{
		if (Target)
		{
			Packages.Add(Target->GetOutermost());
		}
	}

	int32 Replaced = 0;
	for (UPackage* Package : Packages)
	{
		TArray<UObject*> Objects;
		GetObjectsWithPackage(Package, Objects, /*bIncludeNestedObjects*/ true);

		int32 PackageReplaced = 0;
		for (UObject* Object : Objects)
		{
			// Same flags as ObjectTools::ConsolidateObjects: never rewrite the outer chain or the archetype link.
			FArchiveReplaceObjectRef<UObject> ReplaceAr(Object, Map,
				EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
			PackageReplaced += ReplaceAr.GetCount();
		}
		if (PackageReplaced > 0)
		{
			Package->MarkPackageDirty();
			for (UObject* Object : Objects)
			{
				if (Object->IsAsset())
				{
					Object->PostEditChange();
				}
			}
		}
		Replaced += PackageReplaced;
	}
	return Replaced;
#else
	return -1;
#endif
}

TArray<UObject*> UGMCGASPAssetTools::DuplicateAnimationsOnSkeleton(const TArray<UObject*>& Assets, USkeleton* NewSkeleton, const FString& FolderPath, const FString& Prefix, const FString& Suffix, bool bRetargetReferredAssets)
{
	TArray<UObject*> Out;
#if WITH_EDITOR
	if (!NewSkeleton)
	{
		return Out;
	}
	TArray<TWeakObjectPtr<UObject>> ToRetarget;
	for (UObject* Asset : Assets)
	{
		if (Asset)
		{
			ToRetarget.Add(Asset);
		}
	}
	EditorAnimUtils::FNameDuplicationRule NameRule;
	NameRule.Prefix = Prefix;
	NameRule.Suffix = Suffix;
	NameRule.FolderPath = FolderPath.IsEmpty() ? FString(TEXT("/Game")) : FolderPath;
	// bConvertSpace = false: pure re-assignment by bone name (identical hierarchies), no old skeleton needed
	EditorAnimUtils::FAnimationRetargetContext Context(ToRetarget, bRetargetReferredAssets, /*bConvertSpaces*/ false, NameRule);
	EditorAnimUtils::RetargetAnimations(nullptr, NewSkeleton, Context, bRetargetReferredAssets, &NameRule);
	for (UObject* Asset : Assets)
	{
		Out.Add(Asset ? Context.GetDuplicate(Asset) : nullptr);
	}
#endif
	return Out;
}

bool UGMCGASPAssetTools::ImportPropertyText(UObject* Object, FName PropertyName, const FString& Text)
{
#if WITH_EDITOR
	FProperty* Property = Object ? Object->GetClass()->FindPropertyByName(PropertyName) : nullptr;
	if (!Property)
	{
		UE_LOG(LogTemp, Warning, TEXT("ImportPropertyText: no property %s on %s"), *PropertyName.ToString(), *GetPathNameSafe(Object));
		return false;
	}

	Object->Modify();
	Object->PreEditChange(Property);
	FStringOutputDevice Errors;
	const TCHAR* End = Property->ImportText_Direct(*Text, Property->ContainerPtrToValuePtr<void>(Object), Object, PPF_None, &Errors);
	FPropertyChangedEvent ChangedEvent(Property, EPropertyChangeType::ValueSet);
	Object->PostEditChangeProperty(ChangedEvent);

	if (!End || !Errors.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("ImportPropertyText: %s.%s did not parse: %s"), *GetPathNameSafe(Object), *PropertyName.ToString(), *Errors);
		return false;
	}
	return true;
#else
	return false;
#endif
}

FString UGMCGASPAssetTools::ExportPropertyText(UObject* Object, FName PropertyName)
{
	FString Out;
#if WITH_EDITOR
	if (const FProperty* Property = Object ? Object->GetClass()->FindPropertyByName(PropertyName) : nullptr)
	{
		const void* Value = Property->ContainerPtrToValuePtr<void>(Object);
		Property->ExportText_Direct(Out, Value, Value, Object, PPF_None);
	}
#endif
	return Out;
}
