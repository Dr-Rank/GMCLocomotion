// Editor-only asset helpers exposed to Python / Blueprint (no-ops in non-editor builds).

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
class USkeleton;

#include "GMCGASPAssetTools.generated.h"

UCLASS()
class GMCGASP_API UGMCGASPAssetTools : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// Rewrites every reference to a key of ReplacementMap into the matching value, inside the packages that own
	// Targets (all objects of each package, sub-objects included). Same mechanism as the editor's "Consolidate",
	// but the replaced assets are left untouched. Dirty packages are NOT saved. Returns the number of references
	// replaced, -1 outside the editor.
	UFUNCTION(BlueprintCallable, Category = "GMCGASP|Editor")
	static int32 ReplaceObjectReferences(const TArray<UObject*>& Targets, const TMap<UObject*, UObject*>& ReplacementMap);

	// Duplicates animation assets (sequences, montages, blend spaces, anim blueprints) into FolderPath and re-assigns
	// the duplicates to NewSkeleton by bone name, without space conversion: same mechanism as the editor's
	// "replace missing skeleton" (EditorAnimUtils::RetargetAnimations), meant for skeletons sharing the same bone
	// hierarchy (e.g. two copies of SK_Mannequin). Assets referenced by the duplicates (a montage's sequences...) are
	// duplicated too when bRetargetReferredAssets is set, otherwise the references are cleared. Names get
	// Prefix/Suffix and are made unique in the folder. Returns the duplicates in the order of Assets (null when an
	// entry was not an animation asset). Nothing is saved. Empty outside the editor.
	UFUNCTION(BlueprintCallable, Category = "GMCGASP|Editor")
	static TArray<UObject*> DuplicateAnimationsOnSkeleton(const TArray<UObject*>& Assets, USkeleton* NewSkeleton, const FString& FolderPath, const FString& Prefix, const FString& Suffix, bool bRetargetReferredAssets = true);

	// Sets any reflected member of Object from its ImportText form, including plain UPROPERTY() members that Python /
	// Blueprints cannot write (e.g. UChooserTable::ResultsStructs, NestedObjects, RootChooser). Wrapped like an editor
	// edit: Modify + PreEditChange + PostEditChangeProperty. Returns false (warning logged) when the property does not
	// exist or the text does not fully parse. Nothing is saved. Always false outside the editor.
	UFUNCTION(BlueprintCallable, Category = "GMCGASP|Editor")
	static bool ImportPropertyText(UObject* Object, FName PropertyName, const FString& Text);

	// ExportText form of any reflected member of Object (empty if missing). Round-trips with ImportPropertyText.
	UFUNCTION(BlueprintCallable, Category = "GMCGASP|Editor")
	static FString ExportPropertyText(UObject* Object, FName PropertyName);
};
