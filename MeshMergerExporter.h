// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/StaticMeshActor.h"
#include "MeshMergerExporter.generated.h"

UCLASS()
class SAFRAN_APP_API AMeshMergerExporter : public AActor
{
	GENERATED_BODY()

public:
	// Sets default values for this actor's properties
	AMeshMergerExporter();

	UFUNCTION(BlueprintCallable, Category = "Mesh Merger")
	void MergeAndExportMeshes(const FString& ExportPath, bool bExportAsGLTF = true);

private:
	void CollectStaticMeshActors(TArray<AStaticMeshActor*>& OutActors);
	bool MergeMeshes(const TArray<AStaticMeshActor*>& Actors, UStaticMesh*& OutMergedMesh);
	bool ExportToOBJ(UStaticMesh* Mesh, const FString& FilePath);
	bool ExportToGLTF(UStaticMesh* Mesh, const FString& FilePath);
	void ExportMaterials(UStaticMesh* Mesh, const FString& BasePath, const FString& OBJFileName);
	FString SanitizeFileName(const FString& FileName);
};
