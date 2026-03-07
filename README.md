# LightSync DMX - Unreal Engine VP Light Synchronization Plugin

## 概要

**LightSync DMX** は、Unreal Engine のバーチャルプロダクション (VP) において、
UEシーン内の光の色味と実際のLED照明の色を一致させるためのプラグインです。

### 問題
VP撮影時、UEのバーチャル背景から放たれる光と、スタジオのLED照明の色味が一致しないため、
被写体への照明が不自然になる。

### 解決策
シーン内に「光を吸収するプローブ」を配置し、その位置での周囲光の平均色を
リアルタイムでサンプリング → DMX信号として送信 → 実際のLED照明に反映。

---

## アーキテクチャ

```
UEシーンの光源
      ↓
┌─────────────────────────┐
│    LightProbeActor      │   ← シーンに配置する光プローブ
│  ┌───────────────────┐  │
│  │ SceneCaptureEcube │  │   ← 360°キャプチャ
│  └─────────┬─────────┘  │
│            ↓            │
│  ┌───────────────────┐  │
│  │ LightColorSampler │  │   ← 平均色を算出
│  └─────────┬─────────┘  │
│            ↓            │
│  ┌───────────────────┐  │
│  │ DMXColorOutput    │  │   ← DMX信号に変換して送信 (同一PC)
│  └───────────────────┘  │
│            ↓            │
│  ┌───────────────────┐  │
│  │ OSCColorOutput    │  │   ← OSCで別PCに色データを送信
│  └───────────────────┘  │
└─────────────────────────┘
      ↓ (Art-Net / sACN)       ↓ (OSC over Network)
   LED照明器具           別PC (QLC+ / DasLight)
                              ↓ (DMX変換)
                           LED照明器具
```

### 2つの出力パス

| パス | ユースケース |
|------|-------------|
| **DMX直接** | UEのPCにDMXインターフェースが接続されている場合 |
| **OSC経由** | UEのPCとDMX制御PCが別マシンの場合 (QLC+, DasLight等) |

---

## コンポーネント一覧

### LightProbeActor
- シーンに配置するメインのActor
- `SceneCaptureComponentCube` で360°の環境光をキャプチャ
- 色補正機能 (ガンマ、色温度、彩度、明度)
- エディタ上で視覚的にプローブ位置を確認可能

### LightColorSamplerComponent
- CubeMapレンダーターゲットからピクセルデータを読み取り
- ダウンサンプリングで効率的に平均色を算出
- EMA (指数移動平均) によるスムージングでちらつきを抑制
- HDRクランプと暗部閾値による精度向上

### DMXColorOutputComponent
- UE DMXProtocol プラグインを使用してArt-Net / sACN で送信
- 複数のフィクスチャへの同時出力対応
- カラーモード: RGB, RGBW, RGBAW, Dimmer+RGB, CCT+Brightness
- フィクスチャ毎の個別ブライトネス調整

### OSCColorOutputComponent (★ 別PC連携)
- **ネットワーク経由** で別PCの照明ソフトに色データを送信
- QLC+ 互換モード: `/lightsync/{probe}/r`, `/lightsync/{probe}/g`, `/lightsync/{probe}/b` [int 0-255]
- DasLight 互換モード: `/dmx/{universe}/{channel}` [int 0-255]
- 汎用 Float RGB / Int RGB255 / RGBA / カスタムアドレス
- 複数ターゲットへの同時送信対応 (異なるPCに同時送信可)
- バンドルメッセージ対応 (R/G/Bを1パケットにまとめて送信)

### LightSyncSubsystem
- ワールド内の全プローブを一括管理
- マスターディマー、一括ON/OFF、ブラックアウト
- Blueprint/C++ から統一的にアクセス可能

### LightSyncDMXEditor (Editor Module)
- **LightSync Monitor** ウィンドウ: Window → Virtual Production → LightSync DMX Monitor
- リアルタイムでプローブ状態、色、DMXチャンネルを確認
- マスターコントロール (一括有効/無効、ブラックアウト、ディマー)

---

## セットアップ手順

### 1. プラグインの有効化
1. UEプロジェクトの `Plugins` フォルダに本プラグインをコピー
2. Edit → Plugins で以下を有効化:
   - **LightSync DMX**
   - **DMX Protocol** (依存プラグイン)
   - **DMX Engine** (依存プラグイン)
   - **OSC** (依存プラグイン - OSC出力使用時)
3. エディタを再起動

### 2. DMX出力の設定 (同一PC構成の場合)
1. Project Settings → Plugins → DMX Protocol で出力ポートを設定
   - Art-Net の場合: IPアドレスとサブネットを設定
   - sACN の場合: Universe範囲を設定
2. DMX Library アセットを作成 (任意)

### 2b. OSC出力の設定 (別PC構成の場合: QLC+ / DasLight)
1. LightProbeActor の Details パネルで:
   - **Use DMX Output** を `false` に (同一PCでDMXも送る場合はtrueのまま)
   - **Use OSC Output** を `true` に
2. OSC Output → Targets で送信先を設定:
   - **IP Address**: QLC+ / DasLight が動作するPCのIPアドレス
   - **Port**: QLC+ OSCプラグインのポート (デフォルト: 7700)
   - **Format**: 照明ソフトに合わせたフォーマットを選択

#### QLC+ の場合
- QLC+ の Input/Output Settings で OSC プラグインを有効化
- Input ポートを本プラグインの送信ポートに合わせる
- フォーマット: `QLC+ Compatible` を選択
- OSCアドレスをQLC+のチャンネルにマッピング

#### DasLight の場合
- DasLight の OSC 受信ポートを確認
- フォーマット: `DasLight Compatible` を選択
- Universe と Start Channel を DasLight の設定と一致させる

### 3. プローブの配置
1. Content Browser → LightSyncDMX → Place in Level
   または Place Actors パネルから "Light Probe (DMX)" を検索して配置
2. プローブの Details パネルで設定:
   - **Probe Name**: 識別名
   - **Capture Resolution**: キャプチャ解像度 (64推奨)
   - **Sampling Rate**: 更新レート (30Hz推奨)

### 4. DMXマッピングの設定
1. LightProbeActor → DMX Output → Fixture Mappings
2. 各フィクスチャに対して:
   - **Universe**: DMXユニバース番号
   - **Start Channel**: 開始チャンネル
   - **Color Mode**: 照明器具に合わせたモード
   - **Brightness Scale**: 個別の明度調整

### 5. 色補正
1. LightProbeActor の Color セクション:
   - **Gamma Correction**: 2.2 (sRGB標準)
   - **Color Temperature Offset**: LED照明との色温度差を補正
   - **Saturation Multiplier**: 彩度の調整
   - **Brightness Multiplier**: 全体の明度調整

---

## Blueprint からの使用

### ユーティリティ関数
```
// 全プローブを取得
LightSyncBPLibrary::GetAllLightProbes(WorldContext)

// 一括制御
LightSyncBPLibrary::SetAllProbesActive(WorldContext, true/false)
LightSyncBPLibrary::BlackoutAllProbes(WorldContext)
LightSyncBPLibrary::SetMasterDimmer(WorldContext, 0.8)

// 色変換ユーティリティ
LightSyncBPLibrary::LinearColorToDMXValues(Color, R, G, B)
LightSyncBPLibrary::DMXValuesToLinearColor(R, G, B)
```

---

## パフォーマンスノート

| 設定 | 推奨値 | 備考 |
|------|--------|------|
| Capture Resolution | 64 | 色の平均なので低解像度で十分 |
| Sampling Rate | 30 Hz | 映像と同期する場合はフレームレートに合わせる |
| Downsample Resolution | 8 | CPU読み取りピクセル数を制限 |
| Smoothing Alpha | 0.3 | 低い = よりスムーズ、高い = よりレスポンシブ |

1プローブあたりの GPU コスト: ~0.1ms (64x64 CubeMap)
1プローブあたりの CPU コスト: ~0.05ms (SmoothStep + DMX送信)

---

## 対応DMXカラーモード

| モード | チャンネル数 | 内容 |
|--------|-------------|------|
| RGB | 3 | R, G, B |
| RGBW | 4 | R, G, B, White |
| RGBAW | 5 | R, G, B, Amber, White |
| Dimmer+RGB | 4 | Dimmer, R, G, B |
| CCT+Brightness | 2 | 色温度, 明度 |

---

## ファイル構成

```
LightSyncDMX/
├── LightSyncDMX.uplugin
├── README.md
└── Source/
    ├── LightSyncDMX/                  (Runtime Module)
    │   ├── LightSyncDMX.Build.cs
    │   ├── Public/
    │   │   ├── LightSyncDMXModule.h
    │   │   ├── LightProbeActor.h
    │   │   ├── LightColorSamplerComponent.h
    │   │   ├── DMXColorOutputComponent.h
    │   │   ├── OSCColorOutputComponent.h
    │   │   ├── LightSyncSubsystem.h
    │   │   └── LightSyncBlueprintLibrary.h
    │   └── Private/
    │       ├── LightSyncDMXModule.cpp
    │       ├── LightProbeActor.cpp
    │       ├── LightColorSamplerComponent.cpp
    │       ├── DMXColorOutputComponent.cpp
    │       ├── OSCColorOutputComponent.cpp
    │       ├── LightSyncSubsystem.cpp
    │       └── LightSyncBlueprintLibrary.cpp
    └── LightSyncDMXEditor/            (Editor Module)
        ├── LightSyncDMXEditor.Build.cs
        ├── Public/
        │   ├── LightSyncDMXEditorModule.h
        │   └── SLightSyncMonitorWidget.h
        └── Private/
            ├── LightSyncDMXEditorModule.cpp
            └── SLightSyncMonitorWidget.cpp
```

---

## 動作要件

- Unreal Engine 5.4 (ビルド済みバイナリは 5.4 専用)
- DMX Protocol プラグイン (UE標準同梱)
- DMX Engine プラグイン (UE標準同梱)
- OSC プラグイン (UE標準同梱 - OSC出力使用時)
- Art-Net / sACN 対応DMXインターフェース (DMX直接出力時)
- ネットワーク接続 (OSC経由で別PCに送信する場合)

---

## インストール方法 (バイナリ配布版)

### 受け取った人向け

1. ZIP を展開して `LightSyncDMX` フォルダを取得
2. 自分のUEプロジェクトの `Plugins/` フォルダにコピー
   ```
   MyProject/
     Plugins/
       LightSyncDMX/         ← ここに配置
         LightSyncDMX.uplugin
         Binaries/
         Source/
   ```
3. UE Editor を起動（自動で有効化される）
4. **Window > Virtual Production > LightSync Monitor** からモニターウィンドウを開く

### ソースからビルドする場合 (開発者向け)

1. `LightSyncDMX` フォルダ (ソースのみ) をプロジェクトの `Plugins/` に配置
2. UE Editor を起動するとソースからビルドされる
3. MSVC 14.43+ を使う場合は `__has_feature` エラー対策が必要:
   - プロジェクトの `Source/` に `MSVCCompat.h` を作成:
     ```cpp
     #pragma once
     #ifndef __has_feature
     #define __has_feature(x) 0
     #endif
     #ifndef __has_extension
     #define __has_extension(x) 0
     #endif
     ```
   - `*.Target.cs` に追記:
     ```csharp
     AdditionalCompilerArguments = "/FI\"" + Path.Combine(ProjectDirectory, "Source", "MSVCCompat.h") + "\"";
     ```

---

## ライセンス

Copyright UE-Comp. All Rights Reserved.
