// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncCalibrationPatch.h"
#include "LightSyncDMXModule.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
    // プラグイン同梱: Unlit / Two Sided / VectorParameter "Color" → Emissive
    const TCHAR *PatchMaterialPath = TEXT("/LightSyncDMX/Calibration/M_LightSyncPatch.M_LightSyncPatch");
    const FName ColorParameterName(TEXT("Color"));
    constexpr float EngineSphereRadiusCm = 50.0f; // /Engine/BasicShapes/Sphere の半径
}

ALightSyncCalibrationPatch::ALightSyncCalibrationPatch()
{
    PrimaryActorTick.bCanEverTick = false;

    Sphere = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Sphere"));
    SetRootComponent(Sphere);
    Sphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Sphere->SetCastShadow(false);
    Sphere->bAffectDistanceFieldLighting = false;
    Sphere->SetAffectDynamicIndirectLighting(false);

    static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    if (SphereMesh.Succeeded())
    {
        Sphere->SetStaticMesh(SphereMesh.Object);
    }

    static ConstructorHelpers::FObjectFinder<UMaterialInterface> PatchMaterial(PatchMaterialPath);
    if (PatchMaterial.Succeeded())
    {
        Sphere->SetMaterial(0, PatchMaterial.Object);
    }
}

void ALightSyncCalibrationPatch::OnConstruction(const FTransform &Transform)
{
    Super::OnConstruction(Transform);
    ApplyPatch();
}

bool ALightSyncCalibrationPatch::HasPatchMaterial() const
{
    if (PatchMID && PatchMID->Parent)
    {
        return PatchMID->Parent->GetPathName().StartsWith(TEXT("/LightSyncDMX/"));
    }
    const UMaterialInterface *Material = Sphere ? Sphere->GetMaterial(0) : nullptr;
    return Material && Material->GetPathName().StartsWith(TEXT("/LightSyncDMX/"));
}

void ALightSyncCalibrationPatch::ApplyPatch()
{
    if (!Sphere)
    {
        return;
    }

    const float Scale = FMath::Max(RadiusCm, 1.0f) / EngineSphereRadiusCm;
    Sphere->SetWorldScale3D(FVector(Scale));

    if (!PatchMID)
    {
        UMaterialInterface *Base = Sphere->GetMaterial(0);
        if (!Base || !Base->GetPathName().StartsWith(TEXT("/LightSyncDMX/")))
        {
            Base = LoadObject<UMaterialInterface>(nullptr, PatchMaterialPath);
        }
        if (!Base)
        {
            UE_LOG(LogLightSyncDMX, Error, TEXT("CalibrationPatch: 同梱マテリアル %s が見つかりません。"), PatchMaterialPath);
            return;
        }
        PatchMID = UMaterialInstanceDynamic::Create(Base, this);
        Sphere->SetMaterial(0, PatchMID);
    }

    PatchMID->SetVectorParameterValue(ColorParameterName, PatchColor);
}
