// Fill out your copyright notice in the Description page of Project Settings.

#include "MeshMergerExporter.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "IMeshMergeUtilities.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "MeshDescription.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "ImageUtils.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "MeshMergeModule.h"
#include "Engine/Texture2D.h"
#include "Rendering/SkeletalMeshLODImporterData.h"
#include "Modules/ModuleManager.h"
#include "TextureResource.h"

AMeshMergerExporter::AMeshMergerExporter()
{
    PrimaryActorTick.bCanEverTick = false;
}

void AMeshMergerExporter::CollectStaticMeshActors(TArray<AStaticMeshActor*>& OutActors)
{
    OutActors.Empty();

    UWorld* World = GetWorld();
    if (!World) return;

    for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
    {
        AStaticMeshActor* Actor = *It;
        if (Actor && Actor->GetStaticMeshComponent() && Actor->GetStaticMeshComponent()->GetStaticMesh())
        {
            OutActors.Add(Actor);
        }
    }

    UE_LOG(LogTemp, Log, TEXT("Collected %d static mesh actors"), OutActors.Num());
}

bool AMeshMergerExporter::MergeMeshes(const TArray<AStaticMeshActor*>& Actors, UStaticMesh*& OutMergedMesh)
{
    if (Actors.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("No actors to merge"));
        return false;
    }

    // Prepare mesh merge settings
    FMeshMergingSettings MergeSettings;
    MergeSettings.bMergePhysicsData = false;
    MergeSettings.bMergeMaterials = true;
    MergeSettings.bBakeVertexDataToMesh = false;
    MergeSettings.bUseVertexDataForBakingMaterial = false;
    MergeSettings.bGenerateLightMapUV = false;

    // Collect components
    TArray<UPrimitiveComponent*> ComponentsToMerge;
    for (AStaticMeshActor* Actor : Actors)
    {
        if (UStaticMeshComponent* SMC = Actor->GetStaticMeshComponent())
        {
            ComponentsToMerge.Add(SMC);
        }
    }

    if (ComponentsToMerge.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("No valid components to merge"));
        return false;
    }

    UE_LOG(LogTemp, Log, TEXT("Merging %d components..."), ComponentsToMerge.Num());

    // Get mesh utilities module
    const IMeshMergeUtilities& MeshUtilities = FModuleManager::Get().LoadModuleChecked<IMeshMergeModule>("MeshMergeUtilities").GetUtilities();

    // Use transient package for runtime merging
    UPackage* Package = GetTransientPackage();

    // Prepare output parameters
    TArray<UObject*> OutAssetsToSync;
    FVector OutMergedActorLocation = FVector::ZeroVector;

    // Build the merged mesh
    MeshUtilities.MergeComponentsToStaticMesh(
        ComponentsToMerge,
        GetWorld(),
        MergeSettings,
        nullptr, // InBaseMaterial
        Package,
        TEXT("MergedMesh"),
        OutAssetsToSync,
        OutMergedActorLocation,
        1.0f, // ScreenAreaSize
        false // bSilent
    );

    UE_LOG(LogTemp, Log, TEXT("MergeComponentsToStaticMesh returned %d assets"), OutAssetsToSync.Num());

    // Check if merge was successful
    if (OutAssetsToSync.Num() > 0)
    {
        for (UObject* Asset : OutAssetsToSync)
        {
            UE_LOG(LogTemp, Log, TEXT("Checking asset: %s (Type: %s)"),
                *Asset->GetName(), *Asset->GetClass()->GetName());

            if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
            {
                OutMergedMesh = StaticMesh;
                UE_LOG(LogTemp, Log, TEXT("Successfully merged %d meshes into: %s"),
                    ComponentsToMerge.Num(), *OutMergedMesh->GetName());

                // Verify the mesh has valid data
                if (OutMergedMesh->GetRenderData() != nullptr)
                {
                    UE_LOG(LogTemp, Log, TEXT("Merged mesh has valid render data"));
                    return true;
                }
                else if (OutMergedMesh->GetMeshDescription(0) != nullptr)
                {
                    UE_LOG(LogTemp, Log, TEXT("Merged mesh has mesh description but no render data, attempting build"));

                    // Try to build render data
                    OutMergedMesh->NeverStream = true;
                    OutMergedMesh->Build(false);
                    OutMergedMesh->PostEditChange();

                    if (OutMergedMesh->GetRenderData() != nullptr)
                    {
                        UE_LOG(LogTemp, Log, TEXT("Successfully built render data"));
                        return true;
                    }
                    else
                    {
                        UE_LOG(LogTemp, Warning, TEXT("Failed to build render data, but mesh description exists - will attempt export anyway"));
                        return true;
                    }
                }
                else
                {
                    UE_LOG(LogTemp, Error, TEXT("Merged mesh has no render data or mesh description"));
                    return false;
                }
            }
        }
    }

    UE_LOG(LogTemp, Error, TEXT("Failed to merge meshes - no static mesh in output assets (got %d assets)"), OutAssetsToSync.Num());
    return false;
}

FString AMeshMergerExporter::SanitizeFileName(const FString& FileName)
{
    FString Result = FileName;
    Result = Result.Replace(TEXT(" "), TEXT("_"));
    Result = Result.Replace(TEXT("/"), TEXT("_"));
    Result = Result.Replace(TEXT("\\"), TEXT("_"));
    Result = Result.Replace(TEXT(":"), TEXT("_"));
    Result = Result.Replace(TEXT("*"), TEXT("_"));
    Result = Result.Replace(TEXT("?"), TEXT("_"));
    Result = Result.Replace(TEXT("\""), TEXT("_"));
    Result = Result.Replace(TEXT("<"), TEXT("_"));
    Result = Result.Replace(TEXT(">"), TEXT("_"));
    Result = Result.Replace(TEXT("|"), TEXT("_"));
    return Result;
}

void AMeshMergerExporter::ExportMaterials(UStaticMesh* Mesh, const FString& BasePath, const FString& OBJFileName)
{
    if (!Mesh) return;

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

    // Create Textures folder at the same level as OBJ file
    FString TexturesPath = BasePath + TEXT("/Textures");
    if (!PlatformFile.DirectoryExists(*TexturesPath))
    {
        PlatformFile.CreateDirectoryTree(*TexturesPath);
        UE_LOG(LogTemp, Log, TEXT("Created directory: %s"), *TexturesPath);
    }

    TArray<FStaticMaterial> Materials = Mesh->GetStaticMaterials();
    FString MTLContent;

    UE_LOG(LogTemp, Log, TEXT("Exporting %d materials"), Materials.Num());
    UE_LOG(LogTemp, Log, TEXT("Base path: %s"), *BasePath);
    UE_LOG(LogTemp, Log, TEXT("Textures path: %s"), *TexturesPath);

    for (int32 i = 0; i < Materials.Num(); i++)
    {
        UMaterialInterface* Material = Materials[i].MaterialInterface;
        if (!Material)
        {
            UE_LOG(LogTemp, Warning, TEXT("Material %d is null"), i);
            continue;
        }

        FString MaterialName = SanitizeFileName(Material->GetName());
        UE_LOG(LogTemp, Log, TEXT("Processing material: %s"), *MaterialName);

        // Create MTL file content
        MTLContent += FString::Printf(TEXT("newmtl %s\n"), *MaterialName);
        MTLContent += TEXT("Ka 1.000 1.000 1.000\n");
        MTLContent += TEXT("Kd 1.000 1.000 1.000\n");
        MTLContent += TEXT("Ks 0.000 0.000 0.000\n");
        MTLContent += TEXT("Ns 10.0\n");
        MTLContent += TEXT("d 1.0\n");
        MTLContent += TEXT("illum 2\n");

        // Try multiple parameter names for base color texture
        UTexture* BaseColorTexture = nullptr;
        TArray<FName> ParamNames = {
            FName("BaseColor"),
            FName("Diffuse"),
            FName("DiffuseTexture"),
            FName("BaseColorTexture"),
            FName("Texture"),
            FName("Albedo")
        };

        for (const FName& ParamName : ParamNames)
        {
            if (Material->GetTextureParameterValue(ParamName, BaseColorTexture))
            {
                UE_LOG(LogTemp, Log, TEXT("Found texture with parameter name: %s"), *ParamName.ToString());
                break;
            }
        }

        // Also try to get textures from material instance
        if (!BaseColorTexture)
        {
            if (UMaterialInstanceConstant* MatInst = Cast<UMaterialInstanceConstant>(Material))
            {
                for (const FTextureParameterValue& TexParam : MatInst->TextureParameterValues)
                {
                    if (TexParam.ParameterValue)
                    {
                        BaseColorTexture = TexParam.ParameterValue;
                        UE_LOG(LogTemp, Log, TEXT("Found texture from material instance: %s"), *TexParam.ParameterInfo.Name.ToString());
                        break;
                    }
                }
            }
        }

        bool bTextureExported = false;

        if (BaseColorTexture)
        {
            UTexture2D* Texture2D = Cast<UTexture2D>(BaseColorTexture);
            if (Texture2D)
            {
                // Check if this is a virtual texture
                bool bIsVirtualTexture = Texture2D->VirtualTextureStreaming;
                if (bIsVirtualTexture)
                {
                    UE_LOG(LogTemp, Warning, TEXT("Texture %s uses Virtual Texture Streaming - this may cause export issues. Consider disabling VT for this texture."), *Texture2D->GetName());
                }

                // Try TGA format first (simpler, more compatible)
                FString TextureName = SanitizeFileName(Texture2D->GetName()) + TEXT(".tga");
                FString TexturePath = TexturesPath + TEXT("/") + TextureName;

                UE_LOG(LogTemp, Log, TEXT("Attempting to export texture: %s to %s (Virtual Texture: %s)"),
                    *Texture2D->GetName(), *TexturePath, bIsVirtualTexture ? TEXT("Yes") : TEXT("No"));

                // Get texture source data
                FTextureSource& TextureSource = Texture2D->Source;
                bool bUseAlternativeMethod = false;

                if (TextureSource.IsValid())
                {
                    TArray64<uint8> RawData;
                    TextureSource.GetMipData(RawData, 0);

                    if (RawData.Num() > 0)
                    {
                        int32 Width = TextureSource.GetSizeX();
                        int32 Height = TextureSource.GetSizeY();
                        ETextureSourceFormat Format = TextureSource.GetFormat();

                        UE_LOG(LogTemp, Log, TEXT("Texture size: %dx%d, Format: %d, Data size: %lld"),
                            Width, Height, (int32)Format, RawData.Num());

                        // Prepare BGRA data for TGA
                        TArray<FColor> ColorData;
                        ColorData.SetNum(Width * Height);

                        bool bConversionSuccess = false;

                        // Handle different source formats
                        switch (Format)
                        {
                        case TSF_BGRA8:
                        {
                            // Copy directly as FColor expects BGRA
                            for (int32 PixelIndex = 0; PixelIndex < Width * Height; PixelIndex++)
                            {
                                int32 ByteIndex = PixelIndex * 4;
                                if (ByteIndex + 3 < RawData.Num())
                                {
                                    ColorData[PixelIndex] = FColor(
                                        RawData[ByteIndex + 2], // R (from B position)
                                        RawData[ByteIndex + 1], // G
                                        RawData[ByteIndex + 0], // B (from R position)
                                        RawData[ByteIndex + 3]  // A
                                    );
                                }
                            }
                            bConversionSuccess = true;
                            break;
                        }
                        case TSF_G8:
                        {
                            // Grayscale to BGRA
                            for (int32 PixelIndex = 0; PixelIndex < Width * Height; PixelIndex++)
                            {
                                if (PixelIndex < RawData.Num())
                                {
                                    uint8 Gray = RawData[PixelIndex];
                                    ColorData[PixelIndex] = FColor(Gray, Gray, Gray, 255);
                                }
                            }
                            bConversionSuccess = true;
                            break;
                        }
                        default:
                        {
                            UE_LOG(LogTemp, Warning, TEXT("Unsupported texture format: %d, trying raw copy"), (int32)Format);
                            // Try to interpret as BGRA anyway
                            if (RawData.Num() >= Width * Height * 4)
                            {
                                for (int32 PixelIndex = 0; PixelIndex < Width * Height; PixelIndex++)
                                {
                                    int32 ByteIndex = PixelIndex * 4;
                                    ColorData[PixelIndex] = FColor(
                                        RawData[ByteIndex + 2],
                                        RawData[ByteIndex + 1],
                                        RawData[ByteIndex + 0],
                                        RawData[ByteIndex + 3]
                                    );
                                }
                                bConversionSuccess = true;
                            }
                            break;
                        }
                        }

                        if (bConversionSuccess && ColorData.Num() > 0)
                        {
                            // Try TGA first
                            IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
                            TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::TGA);

                            if (ImageWrapper.IsValid())
                            {
                                // TGA expects BGRA format which is what FColor provides
                                if (ImageWrapper->SetRaw(ColorData.GetData(), ColorData.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
                                {
                                    const TArray64<uint8>& CompressedData = ImageWrapper->GetCompressed(100);
                                    if (CompressedData.Num() > 0)
                                    {
                                        TArray<uint8> Data(CompressedData.GetData(), CompressedData.Num());
                                        if (FFileHelper::SaveArrayToFile(Data, *TexturePath))
                                        {
                                            UE_LOG(LogTemp, Log, TEXT("Successfully exported TGA texture: %s (%d bytes)"),
                                                *TexturePath, Data.Num());

                                            // Use relative path without ./
                                            MTLContent += FString::Printf(TEXT("map_Kd Textures/%s\n"), *TextureName);
                                            bTextureExported = true;
                                        }
                                        else
                                        {
                                            UE_LOG(LogTemp, Error, TEXT("Failed to save TGA file: %s"), *TexturePath);
                                        }
                                    }
                                }
                            }

                            // If TGA failed, try BMP (more widely supported)
                            if (!bTextureExported)
                            {
                                TextureName = SanitizeFileName(Texture2D->GetName()) + TEXT(".bmp");
                                TexturePath = TexturesPath + TEXT("/") + TextureName;

                                ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::BMP);
                                if (ImageWrapper.IsValid())
                                {
                                    if (ImageWrapper->SetRaw(ColorData.GetData(), ColorData.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
                                    {
                                        const TArray64<uint8>& CompressedData = ImageWrapper->GetCompressed(100);
                                        if (CompressedData.Num() > 0)
                                        {
                                            TArray<uint8> Data(CompressedData.GetData(), CompressedData.Num());
                                            if (FFileHelper::SaveArrayToFile(Data, *TexturePath))
                                            {
                                                UE_LOG(LogTemp, Log, TEXT("Successfully exported BMP texture: %s (%d bytes)"),
                                                    *TexturePath, Data.Num());

                                                MTLContent += FString::Printf(TEXT("map_Kd Textures/%s\n"), *TextureName);
                                                bTextureExported = true;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        else
                        {
                            UE_LOG(LogTemp, Error, TEXT("Texture conversion failed"));
                        }
                    }
                    else
                    {
                        UE_LOG(LogTemp, Warning, TEXT("Texture has no mip data, will try alternative method"));
                        bUseAlternativeMethod = true;
                    }
                }
                else
                {
                    UE_LOG(LogTemp, Warning, TEXT("Texture source is invalid, will try alternative method"));
                    bUseAlternativeMethod = true;
                }

                // Alternative method for Virtual Textures or when source is unavailable
                if (bUseAlternativeMethod && !bTextureExported)
                {
                    UE_LOG(LogTemp, Log, TEXT("Trying to export using alternative method..."));

                    // Try to read texture data from platform data
                    FTexturePlatformData* PlatformData = Texture2D->GetPlatformData();
                    if (PlatformData && PlatformData->Mips.Num() > 0)
                    {
                        FTexture2DMipMap& Mip = PlatformData->Mips[0];
                        void* MipData = Mip.BulkData.Lock(LOCK_READ_ONLY);

                        if (MipData)
                        {
                            int32 Width = Mip.SizeX;
                            int32 Height = Mip.SizeY;
                            int32 DataSize = Mip.BulkData.GetBulkDataSize();

                            UE_LOG(LogTemp, Log, TEXT("Got platform data: %dx%d, %d bytes"),
                                Width, Height, DataSize);

                            // Assume BGRA8 format for platform data
                            TArray<FColor> OutBMP;
                            OutBMP.SetNum(Width * Height);

                            if (DataSize >= Width * Height * 4)
                            {
                                uint8* SourceData = static_cast<uint8*>(MipData);
                                for (int32 PixIdx = 0; PixIdx < Width * Height; PixIdx++)
                                {
                                    int32 Idx = PixIdx * 4;
                                    OutBMP[PixIdx] = FColor(
                                        SourceData[Idx + 2], // R
                                        SourceData[Idx + 1], // G
                                        SourceData[Idx + 0], // B
                                        SourceData[Idx + 3]  // A
                                    );
                                }

                                Mip.BulkData.Unlock();

                                // Try to save as TGA
                                IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
                                TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::TGA);

                                if (ImageWrapper.IsValid())
                                {
                                    if (ImageWrapper->SetRaw(OutBMP.GetData(), OutBMP.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
                                    {
                                        const TArray64<uint8>& CompressedData = ImageWrapper->GetCompressed(100);
                                        if (CompressedData.Num() > 0)
                                        {
                                            FString AltTextureName = SanitizeFileName(Texture2D->GetName()) + TEXT(".tga");
                                            FString AltTexturePath = TexturesPath + TEXT("/") + AltTextureName;

                                            TArray<uint8> Data(CompressedData.GetData(), CompressedData.Num());
                                            if (FFileHelper::SaveArrayToFile(Data, *AltTexturePath))
                                            {
                                                UE_LOG(LogTemp, Log, TEXT("Successfully exported texture via platform data: %s"), *AltTexturePath);
                                                MTLContent += FString::Printf(TEXT("map_Kd Textures/%s\n"), *AltTextureName);
                                                bTextureExported = true;
                                            }
                                        }
                                    }
                                }

                                // If TGA failed, try BMP
                                if (!bTextureExported)
                                {
                                    FString AltTextureName = SanitizeFileName(Texture2D->GetName()) + TEXT(".bmp");
                                    FString AltTexturePath = TexturesPath + TEXT("/") + AltTextureName;

                                    ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::BMP);
                                    if (ImageWrapper.IsValid())
                                    {
                                        if (ImageWrapper->SetRaw(OutBMP.GetData(), OutBMP.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
                                        {
                                            const TArray64<uint8>& CompressedData = ImageWrapper->GetCompressed(100);
                                            if (CompressedData.Num() > 0)
                                            {
                                                TArray<uint8> Data(CompressedData.GetData(), CompressedData.Num());
                                                if (FFileHelper::SaveArrayToFile(Data, *AltTexturePath))
                                                {
                                                    UE_LOG(LogTemp, Log, TEXT("Successfully exported BMP via platform data: %s"), *AltTexturePath);
                                                    MTLContent += FString::Printf(TEXT("map_Kd Textures/%s\n"), *AltTextureName);
                                                    bTextureExported = true;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            else
                            {
                                Mip.BulkData.Unlock();
                                UE_LOG(LogTemp, Error, TEXT("Platform data size mismatch"));
                            }
                        }
                        else
                        {
                            UE_LOG(LogTemp, Error, TEXT("Failed to lock mip data"));
                        }
                    }
                    else
                    {
                        UE_LOG(LogTemp, Error, TEXT("No platform data available"));
                    }
                }
            }
            else
            {
                UE_LOG(LogTemp, Warning, TEXT("Failed to cast to UTexture2D"));
            }
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("No base color texture found for material: %s"), *MaterialName);
        }

        if (!bTextureExported)
        {
            UE_LOG(LogTemp, Warning, TEXT("No texture exported for material: %s"), *MaterialName);
        }

        MTLContent += TEXT("\n");
    }

    // Save single MTL file for all materials - use same base name as OBJ file
    FString MTLFileName = FPaths::GetBaseFilename(OBJFileName) + TEXT(".mtl");
    FString MTLPath = BasePath + TEXT("/") + MTLFileName;

    bool bSaved = FFileHelper::SaveStringToFile(MTLContent, *MTLPath);
    UE_LOG(LogTemp, Log, TEXT("MTL file saved to: %s (Success: %d)"), *MTLPath, bSaved);

    if (bSaved)
    {
        UE_LOG(LogTemp, Log, TEXT("=== MTL Content ===\n%s\n=== End MTL ==="), *MTLContent);

        // Verify texture files exist
        TArray<FString> FoundFiles;
        IFileManager::Get().FindFiles(FoundFiles, *TexturesPath, TEXT("*.*"));
        UE_LOG(LogTemp, Log, TEXT("Found %d files in Textures folder:"), FoundFiles.Num());
        for (const FString& File : FoundFiles)
        {
            FString FullPath = TexturesPath + TEXT("/") + File;
            int64 FileSize = IFileManager::Get().FileSize(*FullPath);
            UE_LOG(LogTemp, Log, TEXT("  - %s (%lld bytes)"), *File, FileSize);
        }
    }
}

bool AMeshMergerExporter::ExportToOBJ(UStaticMesh* Mesh, const FString& FilePath)
{
    if (!Mesh)
    {
        UE_LOG(LogTemp, Error, TEXT("Cannot export null mesh"));
        return false;
    }

    // Ensure mesh has valid render data
    if (!Mesh->GetRenderData())
    {
        UE_LOG(LogTemp, Error, TEXT("Mesh has no render data"));
        return false;
    }

    FString OBJContent;
    FString BaseFileName = FPaths::GetBaseFilename(FilePath);
    FString MTLFileName = BaseFileName + TEXT(".mtl");

    OBJContent += FString::Printf(TEXT("# Exported from Unreal Engine 5\n"));
    OBJContent += FString::Printf(TEXT("# Mesh: %s\n"), *Mesh->GetName());
    OBJContent += FString::Printf(TEXT("mtllib %s\n\n"), *MTLFileName);

    // Get mesh description for LOD 0
    FMeshDescription* MeshDescription = Mesh->GetMeshDescription(0);
    if (!MeshDescription)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to get mesh description"));
        return false;
    }

    FStaticMeshConstAttributes Attributes(*MeshDescription);
    TVertexAttributesConstRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
    TVertexInstanceAttributesConstRef<FVector3f> VertexNormals = Attributes.GetVertexInstanceNormals();
    TVertexInstanceAttributesConstRef<FVector2f> VertexUVs = Attributes.GetVertexInstanceUVs();

    UE_LOG(LogTemp, Log, TEXT("Exporting %d vertices, %d triangles"),
        MeshDescription->Vertices().Num(), MeshDescription->Triangles().Num());

    // Build vertex instance to index mapping
    TMap<FVertexInstanceID, int32> VertexInstanceToIndex;
    int32 CurrentIndex = 1; // OBJ indices start at 1

    // Export vertex positions
    for (const FVertexID VertexID : MeshDescription->Vertices().GetElementIDs())
    {
        FVector3f Pos = VertexPositions[VertexID];
        // Convert from UE coordinates (Z-up) to OBJ coordinates (Y-up)
        OBJContent += FString::Printf(TEXT("v %.6f %.6f %.6f\n"), Pos.X, Pos.Z, Pos.Y);
    }
    OBJContent += TEXT("\n");

    // Export texture coordinates
    for (const FVertexInstanceID InstanceID : MeshDescription->VertexInstances().GetElementIDs())
    {
        FVector2f UV = VertexUVs.Get(InstanceID, 0);
        OBJContent += FString::Printf(TEXT("vt %.6f %.6f\n"), UV.X, 1.0f - UV.Y);
        VertexInstanceToIndex.Add(InstanceID, CurrentIndex++);
    }
    OBJContent += TEXT("\n");

    // Export normals
    for (const FVertexInstanceID InstanceID : MeshDescription->VertexInstances().GetElementIDs())
    {
        FVector3f Normal = VertexNormals[InstanceID];
        // Convert from UE coordinates to OBJ coordinates
        OBJContent += FString::Printf(TEXT("vn %.6f %.6f %.6f\n"), Normal.X, Normal.Z, Normal.Y);
    }
    OBJContent += TEXT("\n");

    // Get polygon groups (sections) which correspond to materials
    TArray<FStaticMaterial> Materials = Mesh->GetStaticMaterials();
    TPolygonGroupAttributesConstRef<FName> PolygonGroupImportedMaterialSlotNames =
        Attributes.GetPolygonGroupMaterialSlotNames();

    UE_LOG(LogTemp, Log, TEXT("Mesh has %d materials/sections"), Materials.Num());

    // Export faces grouped by material
    for (int32 MatIndex = 0; MatIndex < Materials.Num(); MatIndex++)
    {
        if (!Materials[MatIndex].MaterialInterface)
        {
            UE_LOG(LogTemp, Warning, TEXT("Material %d is null"), MatIndex);
            continue;
        }

        FString MaterialName = SanitizeFileName(Materials[MatIndex].MaterialInterface->GetName());
        OBJContent += FString::Printf(TEXT("\n# Material: %s\n"), *MaterialName);
        OBJContent += FString::Printf(TEXT("usemtl %s\n"), *MaterialName);

        int32 FaceCount = 0;

        // Iterate through all polygons
        for (const FPolygonID PolygonID : MeshDescription->Polygons().GetElementIDs())
        {
            // Get the polygon group (material section) for this polygon
            FPolygonGroupID PolygonGroupID = MeshDescription->GetPolygonPolygonGroup(PolygonID);

            // Check if this polygon uses the current material
            if (PolygonGroupID.GetValue() != MatIndex)
            {
                continue;
            }

            // Get triangles for this polygon
            TArrayView<const FTriangleID> TriangleIDs = MeshDescription->GetPolygonTriangles(PolygonID);

            for (const FTriangleID TriangleID : TriangleIDs)
            {
                TArrayView<const FVertexInstanceID> VertexInstances = MeshDescription->GetTriangleVertexInstances(TriangleID);

                if (VertexInstances.Num() == 3)
                {
                    OBJContent += TEXT("f");
                    for (int32 i = 0; i < 3; i++)
                    {
                        FVertexInstanceID InstanceID = VertexInstances[i];
                        FVertexID VertexID = MeshDescription->GetVertexInstanceVertex(InstanceID);

                        int32 VertexIndex = VertexID.GetValue() + 1;
                        int32 UVNormalIndex = VertexInstanceToIndex[InstanceID];

                        OBJContent += FString::Printf(TEXT(" %d/%d/%d"), VertexIndex, UVNormalIndex, UVNormalIndex);
                    }
                    OBJContent += TEXT("\n");
                    FaceCount++;
                }
            }
        }

        UE_LOG(LogTemp, Log, TEXT("Exported %d faces for material: %s"), FaceCount, *MaterialName);
    }

    // Save OBJ file
    bool bSuccess = FFileHelper::SaveStringToFile(OBJContent, *FilePath);

    if (bSuccess)
    {
        UE_LOG(LogTemp, Log, TEXT("Successfully exported to OBJ: %s"), *FilePath);

        // Export materials and textures
        FString BasePath = FPaths::GetPath(FilePath);
        FString OBJFileName = FPaths::GetCleanFilename(FilePath);
        ExportMaterials(Mesh, BasePath, OBJFileName);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to save OBJ file: %s"), *FilePath);
    }

    return bSuccess;
}

bool AMeshMergerExporter::ExportToGLTF(UStaticMesh* Mesh, const FString& FilePath)
{
    // Note: GLTF export requires GLTFExporter plugin
    // This is a simplified version - for full GLTF support, use the plugin

    UE_LOG(LogTemp, Warning, TEXT("GLTF export requires GLTFExporter plugin. Falling back to OBJ export."));

    FString OBJPath = FilePath.Replace(TEXT(".gltf"), TEXT(".obj"));
    return ExportToOBJ(Mesh, OBJPath);
}

void AMeshMergerExporter::MergeAndExportMeshes(const FString& ExportPath, bool bExportAsGLTF)
{
    UE_LOG(LogTemp, Log, TEXT("Starting mesh merge and export process..."));

    // Collect all static mesh actors
    TArray<AStaticMeshActor*> StaticMeshActors;
    CollectStaticMeshActors(StaticMeshActors);

    if (StaticMeshActors.Num() == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("No static mesh actors found in level"));
        return;
    }

    // Merge meshes
    UStaticMesh* MergedMesh = nullptr;
    if (!MergeMeshes(StaticMeshActors, MergedMesh))
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to merge meshes"));
        return;
    }

    if (!MergedMesh)
    {
        UE_LOG(LogTemp, Error, TEXT("Merged mesh is null"));
        return;
    }

    // Ensure export directory exists
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    FString ExportDir = FPaths::GetPath(ExportPath);
    PlatformFile.CreateDirectoryTree(*ExportDir);

    // Export to file
    bool bSuccess = false;
    if (bExportAsGLTF)
    {
        bSuccess = ExportToGLTF(MergedMesh, ExportPath);
    }
    else
    {
        bSuccess = ExportToOBJ(MergedMesh, ExportPath);
    }

    if (bSuccess)
    {
        UE_LOG(LogTemp, Log, TEXT("Successfully exported merged mesh to: %s"), *ExportPath);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to export merged mesh"));
    }
}
