// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LightSyncCalibrationPatch.generated.h"

class UStaticMeshComponent;
class UMaterialInstanceDynamic;

/**
 * ALightSyncCalibrationPatch
 *
 * キャリブレーション用の「全方位パッチ」。プローブを囲む大きな Unlit 球で、
 * LEDウォールいっぱいに一様な色を表示させる (ICVFX の外側フラスタムに映る)。
 * プローブはこのパッチを自分の単位 (正規化照度) で測るので、トーンカーブを予測する必要がない。
 *
 * レベルに置かれたアクターなので Multi-User 経由で nDisplay ノードにも反映される。
 */
UCLASS(NotBlueprintable, meta = (DisplayName = "LightSync Calibration Patch"))
class LIGHTSYNCDMX_API ALightSyncCalibrationPatch : public AActor
{
    GENERATED_BODY()

public:
    ALightSyncCalibrationPatch();

    /** パッチの色 (エミッシブ、リニア) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration")
    FLinearColor PatchColor = FLinearColor::White;

    /** 球の半径 (cm)。ステージとプローブを十分に囲む大きさにする */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration", meta = (ClampMin = "100", ClampMax = "1000000"))
    float RadiusCm = 5000.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Calibration")
    TObjectPtr<UStaticMeshComponent> Sphere;

    /** 色と大きさを反映 */
    UFUNCTION(BlueprintCallable, Category = "Calibration")
    void ApplyPatch();

    /** 同梱マテリアルが見つかったか (見つからないと色が正しく出ない) */
    UFUNCTION(BlueprintPure, Category = "Calibration")
    bool HasPatchMaterial() const;

protected:
    virtual void OnConstruction(const FTransform &Transform) override;

private:
    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> PatchMID;
};
