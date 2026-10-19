// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "LightSyncSubsystem.generated.h"

class ALightProbeActor;

/**
 * FLightSyncProbeInfo
 * プローブの状態情報 (UIやデバッグ用)
 */
USTRUCT(BlueprintType)
struct LIGHTSYNCDMX_API FLightSyncProbeInfo
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "LightSync")
    FString ProbeName;

    UPROPERTY(BlueprintReadOnly, Category = "LightSync")
    FVector Location = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category = "LightSync")
    FLinearColor CurrentColor = FLinearColor::Black;

    UPROPERTY(BlueprintReadOnly, Category = "LightSync")
    bool bIsActive = false;

    UPROPERTY(BlueprintReadOnly, Category = "LightSync")
    int32 Universe = 0;

    UPROPERTY(BlueprintReadOnly, Category = "LightSync")
    int32 StartChannel = 0;
};

/**
 * ULightSyncSubsystem
 *
 * ワールド内の全LightProbeActorを管理するサブシステム。
 * プローブの登録/解除、一括制御、デバッグ情報の提供を担当。
 */
UCLASS()
class LIGHTSYNCDMX_API ULightSyncSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    // === ライフサイクル ===
    virtual void Initialize(FSubsystemCollectionBase &Collection) override;
    virtual void Deinitialize() override;
    virtual bool ShouldCreateSubsystem(UObject *Outer) const override;

    // === プローブ管理 ===

    /** プローブを登録 */
    UFUNCTION(BlueprintCallable, Category = "LightSync")
    void RegisterProbe(ALightProbeActor *Probe);

    /** プローブを登録解除 */
    UFUNCTION(BlueprintCallable, Category = "LightSync")
    void UnregisterProbe(ALightProbeActor *Probe);

    /** 登録済みプローブ数を取得 */
    UFUNCTION(BlueprintPure, Category = "LightSync")
    int32 GetProbeCount() const { return RegisteredProbes.Num(); }

    /** 全プローブの情報を取得 */
    UFUNCTION(BlueprintCallable, Category = "LightSync")
    TArray<FLightSyncProbeInfo> GetAllProbeInfo() const;

    /** 名前でプローブを検索 */
    UFUNCTION(BlueprintCallable, Category = "LightSync")
    ALightProbeActor *FindProbeByName(const FString &Name) const;

    // === 一括制御 ===

    /** 全プローブを有効化/無効化 */
    UFUNCTION(BlueprintCallable, Category = "LightSync")
    void SetAllProbesActive(bool bActive);

    /** 全プローブをブラックアウト */
    UFUNCTION(BlueprintCallable, Category = "LightSync")
    void BlackoutAll();

    /** 全プローブの色を手動で一度サンプリング */
    UFUNCTION(BlueprintCallable, Category = "LightSync")
    void ForceSampleAll();

    /**
     * このフレームのキャプチャ枠を 1 つ確保する (Project Settings の Max Captures Per Frame)。
     * 枠が足りずに待たされたプローブは、次のフレームで先に枠を得る (Tick 順に関係なく公平に)。
     */
    bool TryReserveCapture(const ALightProbeActor *Probe);

    /** マスターディマーを全プローブの DMX / OSC 出力に設定 */
    UFUNCTION(BlueprintCallable, Category = "LightSync")
    void SetMasterDimmer(float Dimmer);

    // === デバッグ ===

    /** デバッグ描画を有効/無効 (各プローブ位置に出力色の球と名前を描く) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightSync|Debug")
    bool bDrawDebug = false;

    /** コンソールにプローブ状態をダンプ */
    UFUNCTION(BlueprintCallable, Category = "LightSync|Debug")
    void DumpProbeStatus() const;

private:
    /** 登録済みプローブの配列 */
    UPROPERTY()
    TArray<TWeakObjectPtr<ALightProbeActor>> RegisteredProbes;

    uint64 CaptureFrame = 0;
    int32 CapturesThisFrame = 0;

    /** キャプチャ枠を待っているプローブ (待たされた順) */
    struct FCaptureWaiter
    {
        TWeakObjectPtr<const ALightProbeActor> Probe;
        uint64 LastRequestFrame = 0;
    };
    TArray<FCaptureWaiter> CaptureWaiters;

    /** 無効な(GC済み)プローブを除去 */
    void CleanupInvalidProbes();
};
