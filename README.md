# LightSync DMX 2.0

バーチャルプロダクション (VP) で、**実際の照明を、LEDウォール (または仮想シーン) の光と同じ色・同じ強さの比率で光らせる** Unreal Engine 5.7 プラグインです。

被写体の位置にプローブを置くと、周囲 360° の光を方向ごとに測り、灯体ごとに DMX (Art-Net / sACN) または OSC で送ります。
2.0 では色の計算を作り直し、物理量と実測値で一貫するようにしました。

- 光の量は **立体角で正しく重み付けした照度** で測る (方向ごとの「ゾーン」)
- 色の基準を **LEDウォールの表示色** と **シーンの物理的な光** から選べる
- 灯体の色は **カメラ上で一致するように** ソルバで解く (RGB / RGBW / RGBAW / CCT など、16bit 対応)
- **カメラだけ** で灯体と壁をキャリブレーションできる
- **nDisplay** の外側ビューポートと同じ露出・カラーグレーディングを使える
- **照明を送る PC を選べる**。nDisplay のクラスタでは 1 台だけが送り、別の PC から送ることもできる
- 終了時は **直接ブラックアウト** を送るので、アプリを閉じても照明が点いたまま残らない

---

## 目次

1. [仕組み](#仕組み)
2. [クイックスタート](#クイックスタート)
3. [色の基準](#色の基準)
4. [ゾーン](#ゾーン)
5. [灯体と DMX 出力](#灯体と-dmx-出力)
6. [OSC 出力](#osc-出力)
7. [キャリブレーション (カメラ)](#キャリブレーション-カメラ)
8. [nDisplay 連携](#ndisplay-連携)
9. [照明を送る PC](#照明を送る-pc)
10. [性能と遅延](#性能と遅延)
11. [モニターウィンドウ](#モニターウィンドウ)
12. [Blueprint / Python / コンソール](#blueprint--python--コンソール)
13. [検証と診断](#検証と診断)
14. [制約と注意点](#制約と注意点)
15. [トラブルシューティング](#トラブルシューティング)
16. [インストールとビルド](#インストールとビルド)
17. [1.x からの移行](#1x-からの移行)

---

## 仕組み

```
 LightProbeActor (被写体の位置)
 ┌─────────────────────────────────────────────────────────────────────┐
 │ ① キューブキャプチャ                                                 │
 │    Wall : 露出・グレーディング・トーンカーブ後の値 (= 壁へ送る信号)  │
 │           → 画素ごとに壁の表示特性 (sRGB 等) で光に戻す             │
 │    Scene: トーンマップ前の放射輝度 × 露出                             │
 │                    ↓                                                 │
 │ ② ゾーンごとに積分 (GPU Compute、立体角で重み付け)                   │
 │    → 正規化照度 T (リニア RGB、1.0 = 基準白の一様な環境から受ける照度) │
 │                    ↓                                                 │
 │ ③ 時定数でスムージング (フレームレートに依存しない)                  │
 └────────────────────┬────────────────────────────────────────────────┘
                      ↓
 ④ 灯体ごとにソルバで解く (カメラ空間で一致させる)
      M_fix · d = M_ref · (ゲイン × T)
        M_ref : 基準 → カメラ (ステージキャリブレーション)
        M_fix : 灯体のエミッタ → カメラ (灯体プロファイル)
      → 色域外は色相を保って彩度を下げ、上限超えは等倍で下げる
      → 調光カーブの逆 → 8 / 16bit
                      ↓
 ⑤ DMX (Art-Net / sACN) / OSC で送信
```

キャリブレーションしなくても動きます。その場合は M_ref と M_fix を理想値 (カメラ = Rec.709、エミッタ = 純色) として扱います。

---

## クイックスタート

1. プラグインを有効にする (依存: DMX Protocol / DMX Engine / OSC。いずれも UE 同梱)
2. **Project Settings → Plugins → DMX → Output Ports** に Art-Net / sACN の出力ポートを追加
3. **Place Actors** で「Light Probe (DMX)」を検索し、被写体の位置に置く
4. プローブの Details:
   - **Reference Mode**: `LED Wall` (既定) か `Scene Light`
   - **Reference Camera**: 撮影カメラ (ICVFX カメラや CineCameraActor)。露出は **Manual** にしておく
   - **DMX Output → Fixture Mappings**: 灯体ごとに Universe / Start Channel / **Zone** / プリセット (または **Profile**)
5. **Window → Virtual Production → LightSync DMX Monitor** で色と送信値を確認
6. 色を本当に合わせたいときは [キャリブレーション](#キャリブレーション-カメラ) を行う

---

## 色の基準

### Reference Mode

| モード | 使う値 | 向いている場面 |
|--------|--------|---------------|
| **LED Wall** (既定) | 参照カメラと同じ露出・グレーディング・トーンカーブを通した値。壁へ送る信号そのもの (実測で LDR のコード値と一致) を、**画素ごとに** 壁の表示特性 (`Wall Display EOTF`: sRGB / ガンマ 2.2 / 2.4) で光に戻してから積分する | 壁と照明をカメラ上で揃えたい (通常はこちら) |
| **Scene Light** | トーンマップ前の放射輝度に、UE の手動露出と同じ倍率 `0.18 × 2^(Bias − EV100)` を掛けた値 | 仮想シーンの光をそのまま再現したい。グレーディングの影響を受けたくない |

どちらも「カメラで 1.0 になる明るさ」を基準白にしているので、モードを切り替えても強さの尺度は同じです。

### ポストプロセスの取得元 (Post Process Source)

| 取得元 | 内容 |
|--------|------|
| **Reference Camera / Manual** | 参照カメラの最終的なポストプロセス (CineCamera の絞り等を反映)。カメラが無ければ `Manual EV100` と `Manual Exposure Bias` |
| **nDisplay Outer Viewport** | 上をシーンとして、nDisplay RootActor のカスタム PP とカラーグレーディングを重ねる ([nDisplay 連携](#ndisplay-連携)) |

露出は常に **Manual** として扱います。キューブキャプチャが自分の中身に自動露出すると、壁と違う明るさになるためです。参照カメラが自動露出なら、ログに警告を一度出します。

色ではなく「像」にかかる効果 (ブルーム、フレア、ビネット、グレイン、DOF、モーションブラー、AA) は無効にしています。フォグや AO のように光の見え方に関わるものは有効のままです。

### Output Gain

全ゾーン共通のリニアなゲインです。灯体ごとの強さは、マッピングの **強さのゲイン** で調整します。

---

## ゾーン

プローブの **Color Sampler → Zones** で、「どの方向の光を、どの灯体で再現するか」を決めます (最大 8 個)。
DMX のフィクスチャと OSC のターゲットは、ゾーンを **名前** で参照します。

### 形状

どの形状も、**一様な放射輝度 1 の環境で出力が 1.0** になるよう正規化してあります。

| 形状 | 値 | 用途 |
|------|----|------|
| **Irradiance** | Direction を向いた面が受ける照度 (半球、コサイン重み) | 天井・正面・逆光など向きの決まった灯体 |
| **Ring** | Direction を軸とした水平リング。軸に垂直な面の照度を全方位で平均 | 側面の灯体をまとめて 1 色で |
| **Cone** | Direction 周りのコーンの照度 (境界はぼかし幅でなめらかに) | 狭い範囲だけを担当する灯体 |
| **Sphere** | 全方位の平均放射輝度 | 全体の色 (アンビエント) |
| **Key** | 最も明るい方向を自動で探し、その周りのコーンの照度 | スポット・太陽など動くキーライト |
| **Exclude** | 出力しない。そのコーンの方向を **全ゾーンから除外** | LED ウォール自体が照らす方向 (壁の光は壁が出すので灯体で足さない) |

- **Share Directions**: Irradiance / Ring / Cone は方向を分け合い、同じ光を二重に数えません。境界の鋭さは `Partition Sharpness` で調整します (大きいほど「一番近いゾーンが全部持つ」に近づく)
- 方向はプローブのローカル空間です。プローブを回すとゾーンも回ります
- Key は「ブロック単位で最も明るい方向 → そのコーン内の輝度重心」の 2 段で方向を決めます (テストでの誤差 0°)

### 既定のゾーン

| 名前 | 形状 | 役割 |
|------|------|------|
| `Top` | Irradiance (+Z、分割) | 天井 |
| `Side` | Ring (Z 軸、分割) | 側面 |
| `Average` | Sphere | 全体 |
| `Key` | Key (20°) | キーライト |

### 設計例: 正面に LED ウォール、天井と左右に灯体

```
Wall    Exclude    Direction = ウォールの方向 (例 +X)、Cone 70°    ← 壁の光は数えない
Top     Irradiance Direction = +Z (分割)                            → 天井の PavoTube
Left    Cone       Direction = +Y、Cone 60° (分割)                  → 左の灯体
Right   Cone       Direction = −Y、Cone 60° (分割)                  → 右の灯体
Back    Irradiance Direction = −X (分割)                            → 逆光
```

---

## 灯体と DMX 出力

### フィクスチャマッピング (DMX Output → Fixture Mappings)

| 項目 | 内容 |
|------|------|
| Zone | 再現するゾーン名 |
| Universe / Start Channel | 送信先。プロファイルのチャンネル数が 512 を超える場合は送らずに警告 (モニターで赤表示) |
| **Profile** | 灯体プロファイルアセット。未設定なら下の内蔵プリセット |
| Color Mode | 内蔵プリセット (Profile 未設定時) |
| 強さのゲイン (BrightnessScale) | 目標 1.0 をこの灯体の何倍で出すか。灯体の距離・大きさの違いを吸収する |

コンポーネント全体の設定: `DMX Library` (指定するとそのライブラリの出力ポートだけに送る)、`Stage Calibration`、`Master Dimmer`。

### 内蔵プリセット

| プリセット | チャンネル | 備考 |
|-----------|-----------|------|
| RGB (3ch) | R, G, B | |
| RGBW (4ch) | R, G, B, W | 白を優先して使う (色は厳密一致のまま) |
| RGBAW (5ch) | R, G, B, A, W | アンバーは 590nm 前後の LED の近似値。**要キャリブレーション** |
| Dimmer+RGB (4ch) | Dimmer, R, G, B | Dimmer = 明るさ、RGB = 混色比 |
| CCT+Brightness (2ch) | CCT, Dimmer | 2700–6500K |
| CCT & RGBW (10ch) | CCT, G/M, Dimmer, Crossfade, R, G, B, W, Strobe, Reserved | RGBW モード (Crossfade = 255) で使う。2700–7500K |

### 灯体プロファイル (ULightSyncFixtureProfile)

Content Browser で Data Asset として作るか、モニターのキャリブレーション欄で **「プリセットからアセットを作成して割り当て」** を押します (`/Game/LightSync/Profiles/`)。

| 項目 | 内容 |
|------|------|
| Color Model | `Emitter Mix` (R/G/B/W/A… の混色をソルバで解く) / `CCT + Tint` (白色灯) |
| Emitters | 各エミッタを 100% で点けたときの **カメラのリニア RGB** (キャリブレーションで埋まる)。`Is White` はソルバが優先して使う白 |
| White Preference | 0 = 最小エネルギー解、1 = 白を最大限使う。色の一致が常に優先 |
| Channels | 先頭から順の配置。機能は Emitter / Dimmer / CCT / Tint / Fixed、各チャンネル 8bit か **16bit (Coarse + Fine)** |
| Dimming Curve | DMX レベル → 光出力。Linear / Gamma / LUT。Dimmer があれば Dimmer に、なければ各エミッタに適用する |
| CCT / Tint レンジ | CCT チャンネル 0–最大 に対応する色温度、Tint (Duv) の範囲 |

**ソルバの動作**

1. 目標をカメラ空間へ変換し、エミッタのレベルを非負最小二乗で解く
2. 灯体の色域外なら、色相を保ったまま白の方向へ彩度を下げる (**色域外** フラグ)
3. レベルが 1 を超えたら全エミッタを等倍で下げる (**明るさ上限** フラグ)。チャンネルごとに切らないので色相は変わらない
4. 調光カーブの逆変換 → 8 / 16bit に量子化

CCT 灯体は、目標の色度から **CCT と Duv** (Ohno 2013) を求め、明るさはカメラ上の輝度比で決めます。レンジ外の色温度や Duv は **色域外** になります。

送信値と各フラグは、**DMX Output → Fixture Status** とモニターで確認できます。

---

## OSC 出力

別 PC の照明ソフト (QLC+ / DasLight 等) に送るときに使います。ターゲットごとの設定は次のとおりです。

| 項目 | 内容 |
|------|------|
| Zone | 送るゾーン |
| Format | Float RGB / Int RGB (0-255) / Float RGBA / QLC+ 互換 / DasLight 互換 / カスタムアドレス |
| **Value Mode** | `Fixture Levels` (既定): プロファイルとソルバで解いた DMX レベル。DasLight 形式ではプロファイルの全チャンネルを送る / `Target (Linear)` / `Target (sRGB Encoded)` |
| Profile | Fixture Levels で使うプロファイル (未設定なら RGB) |
| Intensity Gain | 強さのゲイン |

アドレスの例: `/lightsync/{probe}/rgb`、QLC+ 互換 `/lightsync/{probe}/r|g|b`、DasLight 互換 `/dmx/{universe}/{channel}`。
IP / Port を変えると自動で再接続します。

---

## キャリブレーション (カメラ)

色度計がなくても、**撮影カメラとグレーカード** でキャリブレーションできます。
モニターでプローブを選び、**キャリブレーション (カメラ)** 欄から行います。

### 準備

- カメラの **露出・ホワイトバランス・ISO・シャッター** は、全測定で固定する
- グレーカードの位置:
  - **壁の測定**: カードを壁に向けて壁の近く (視野の大半が壁になる位置)
  - **灯体の測定**: 被写体の位置で、その灯体に向ける
- **カメラの記録方式** を選ぶ: Linear / sRGB / Rec.709 / ARRI LogC3 (EI 800) / ARRI LogC4 / Sony S-Log3
- 値は次のどちらかで記録する:
  - **手入力**: DaVinci Resolve やカメラのスコープで読んだ、グレーカードのコード値 (0–1)
  - **映像から**: カメラ映像 (Media Texture など) とグレーカードの範囲 (ROI) を指定し、[映像から] を押す。sRGB テクスチャとしてデコードされる映像は、自動でコード値に戻す

### 灯体 → プロファイル

1. 対象フィクスチャを選ぶ。プロファイルが無ければ **作成して割り当て** る
2. 各ステップで [点灯] → 値を記録
   - 消灯 (黒レベル)
   - 各エミッタ 100%
   - 全エミッタ 25 / 50 / 75 / 100% (調光カーブ用)
3. **灯体をフィット → プロファイルに保存**
   - エミッタの応答 (黒を差し引いたカメラ値)
   - 調光カーブ: ガンマで最大誤差 2% 以内に収まればガンマ、収まらなければ測定点の LUT
   - **加法性の誤差** (単独点灯の和 と 全点灯 の差) も報告する。5% を超える灯体は全点灯で出力が落ちている可能性がある

### 壁 → ステージキャリブレーション (M_ref)

1. ステージキャリブレーションを **作成して割り当て** (DMX / OSC 出力の両方に設定される)
2. 各ステップで [表示] → 値を記録: 壁 黒 / 赤 / 緑 / 青 / 白 / 灰 6.25% / 12.5% / 25% / 50%
   - プローブを囲む大きなパッチ (`LightSync Calibration Patch`) が出て、壁いっぱいにその色が映る
   - 記録の瞬間に、**プローブ自身がパッチを測った値** も一緒に記録する。トーンカーブを予測する必要はない
   - パッチの明るさは `パッチの明るさ` で調整する (壁の信号が白飛びしない程度に)
   - 灰のステップは **壁の応答カーブ** を測るためのもの。実際の壁は `Wall Display EOTF` (sRGB 等) どおりに光るとは限らないので、ずれをガンマとして求める
3. **壁をフィット → ステージキャリブレーションに保存** (残差 RMS を表示)
   - `M_ref` (壁の原色 → カメラ) と、壁の応答ガンマ `Reference Gamma` を保存する
   - LED Wall モードのプローブは、EOTF でデコードした後に画素ごとに `Reference Gamma` を掛けてから積分する (非線形なので積分後には掛けられない)
   - 灰のステップを記録しなければガンマは 1.0 (EOTF どおり) のまま。Scene Light モードで記録した場合もガンマは使わない
4. 最後に **テスト表示を終了** (パッチを消してプローブの出力を再開。ウィンドウを閉じても自動で終了する)

テスト点灯中はプローブの出力を止めています (`Suspend Output`)。サンプリングは続いています。
フィット結果はアセットの未保存変更になるので、保存してください。

### 強さの合わせ方

壁の測定 (カードは壁の近く) と灯体の測定 (カードは被写体の位置) が同じカメラ尺度で記録されていれば、強さのゲイン 1.0 で物理的に正しい比率になります。
カメラ上でずれが残る場合は、フィクスチャの **強さのゲイン** で詰めてください。

---

## nDisplay 連携

**Post Process Source** を `nDisplay Outer Viewport` にすると、プローブのキャプチャが、nDisplay が外側ビューポート (壁に映る環境) に適用するのと同じポストプロセスを使います。

- `NDisplay Root Actor`: 未設定ならレベル内の最初の RootActor
- `NDisplay Viewport Id`: Per-Viewport グレーディングを引くビューポート (例 `VP_0`。モニターに候補が出る)。プローブが再現したい方向の壁を映しているビューポートを選ぶ。空ならクラスタ全体のグレーディングだけを使う

**合成の順序** (UE 5.7 の nDisplay と同じ)

```
Start (カスタム PP) → シーン (参照カメラ / 手動露出) → Override → Final → FinalPerViewport
FinalPerViewport = Entire Cluster グレーディング + Per-Viewport グレーディング
```

- グレーディング同士の合成: 露出補正・オフセット・WhiteTemp 等は **加算** (WhiteTemp は 1 枚増えるごとに −6500)、彩度・コントラスト・ガンマ・ゲインは **乗算**、TemperatureType は後勝ち、重みは積
- レイヤーの重ね方: 数値は重みで補間、それ以外は置き換え (`FSceneView::OverridePostProcessSettings` と同じ)
- 露出補正はシーンの Bias を **置き換える** (nDisplay と同じ)。EV100 はシーン側のまま残る

nDisplay モジュールにはリンクせず、リフレクションで設定を読みます。nDisplay を使わないプロジェクトで nDisplay が有効になることはありません。
適用されたレイヤーはモニターの **適用中の nDisplay レイヤー** に表示されます。

---

## 照明を送る PC

照明 (DMX / OSC) は、**ネットワークでつながっていれば別の PC から送れます**。Art-Net / sACN / OSC はどれも UDP なので、送る PC と灯体 (または DMX ノード・照明卓) が同じ LAN にあれば届きます。
同じ照明を複数の PC が同時に送ると、値がちらついたり (Art-Net は最後に届いた値、sACN は優先度で決まる) 二重に送られたりするので、**送る PC は 1 台に決めます**。

**Project Settings → Plugins → LightSync DMX → Output Policy**

| 設定 | エディタ / 単体実行 (-game, パッケージ版) | nDisplay クラスタノード |
|------|----------------------------------------|------------------------|
| **Auto** (既定) | 送る | **送信ノードだけ** が送る |
| **nDisplay Output Node Only** | 送らない | 送信ノードだけが送る |
| **Editor / Standalone Only** | 送る | 送らない |
| **Disabled** | 送らない | 送らない |

- **送信ノード**: `Output Cluster Node Id` (空なら nDisplay 設定のプライマリノード)
- **PC ごとの上書き** (最優先): 起動引数 `-LightSyncOutput=On` / `-LightSyncOutput=Off`。たとえば照明専用 PC だけ `On` にし、他を `Off` にする
- 送らない PC では、プローブは **キャプチャもしない** ので描画ノードの負荷は増えません (値を Blueprint で使うときは プローブの `Sample When Not Outputting` をオン)。エディタのプレビューは常に動きます
- どの PC が送るか・その理由は起動時にログに出ます (`LightSync 出力: この PC から送信する (...)`)。`LightSync.Stats` でも確認できます

**よくある構成**

1. **nDisplay の 1 ノードから送る** (Auto のまま): 照明の色はそのノードのレンダリング結果から計算されます。全ノードが同じレベルを開いているので、どのノードでも同じ値になります
2. **照明専用の PC から送る**: 同じプロジェクト・レベルをその PC で `-game` (またはパッケージ版) で起動し `-LightSyncOutput=On`。クラスタノードには `Output Policy = Editor / Standalone Only` にするか `-LightSyncOutput=Off` を付けます。描画ノードの負荷がゼロになるので、プローブが多いときはこれが一番軽い構成です
3. **Multi-User のエディタ (オペレーター PC) から送る**: クラスタ側を `-LightSyncOutput=Off` にします。Auto のままだとエディタとクラスタの両方が送ります

送る PC の **DMX の出力ポート** (Project Settings → DMX → Output Ports) の `Network Interface` は、その PC の NIC の IP にします。プロジェクト設定は全 PC で共通なので、PC ごとに IP が違う場合は次のどちらかを使います (終了時の直接ブラックアウトも同じアドレスから送ります)。

- ポートの **Auto Complete Device Address** をオンにし、`192.168.1.*` のようにサブネットで指定する
- 起動引数 `-dmxoutputportip=<ポート名>:<IP>` で上書きする

照明専用 PC は、壁と同じレベル・同じシーケンス (タイムコード同期など) で動かしてください。光の変化のタイミングがずれないようにするためです。

### 終了時のブラックアウト

UE の DMX 出力ポートは、エンジン終了が要求されると送信を止めます。そのためプラグインは、**終了が要求された直後** と **終了処理の最後** の 2 回、DMX ポートを通さず自前の UDP ソケットで Art-Net / sACN のブラックアウトを直接送ります (各 3 パケット)。
コンソールウィンドウを閉じて OS に数秒でプロセスを止められる場合でも届きます。送る値は各チャンネルの「消灯値」(プロファイルの Fixed チャンネルは固定値のまま) です。
不要なら **Direct Blackout On Exit** をオフにします。

---

## 性能と遅延

**処理の流れと遅延**

1. サンプリング間隔 (`Sampling Rate`、既定 30 Hz) ごとにキューブをキャプチャし、GPU で積分を発行する
2. GPU の結果は 1〜2 フレーム後に非同期で読み戻す (ゲームスレッドは待たない)
3. 結果が届いたフレームで **すぐ** DMX / OSC を送る (次のサンプリングを待たない)
4. 前の結果を待っている間は次のキャプチャをしない (使われない描画をしない)

キャプチャから送信までの遅延は、開発機の実測で 10〜33 ms (フレームレートに依存) です。

**負荷** (開発機、1280×720 の -game、64 px キューブ、30 Hz)

| | フレーム時間 | LightSync の GPU 積分 |
|--|-------------|----------------------|
| プローブなし | 2.85 ms | - |
| 1 個 | 3.24 ms | 0.003 ms / フレーム |
| 8 個 | 5.20 ms | 0.036 ms / フレーム |

- 負荷のほとんどは **キューブキャプチャの描画** (UE のレンダラーで 6 面を描く。開発機で 1 回あたり描画スレッド約 1.75 ms) です。積分そのものは無視できる程度です
- キャプチャ 1 回のゲームスレッド時間は 0.1〜0.2 ms です
- プローブが多いときは **Max Captures Per Frame** (Project Settings → LightSync DMX、既定 2) で 1 フレームのキャプチャ数を抑え、フレーム時間の山を平らにします。待たされたプローブは次のフレームで優先されるので、どのプローブも同じ頻度で更新されます
- 描画ノードの負荷を増やしたくないときは、[照明専用の PC から送る](#照明を送る-pc) 構成にします
- 軽くする順: `Sampling Rate` を下げる (DMX 灯体の多くは 30〜44 Hz で十分) → プローブ数を減らす (ゾーンで方向を分ければ 1 個で複数の灯体を駆動できる) → [変化がないときは更新を減らす](#変化がないときは更新を減らす) → `Capture Resolution` を下げる (照度の積分には 32〜64 px で十分)

### 変化がないときは更新を減らす

プローブの **Reduce When Static → Reduce Rate When Static** (既定オフ。モニターの「変化がないときは更新を減らす」)。

- 出力が `Idle Delay` (既定 1 秒) の間 `Idle Change Threshold` (既定 1%) より変わらず、スムージングも落ち着いたら、キャプチャを `Idle Sampling Rate` (既定 5 Hz) に減らす
- 変化を見つけたら、その場で `Sampling Rate` に戻して次のキャプチャをすぐ行う
- 変化は「最後に変化を見つけたときの値」と比べるので、ゆっくりした変化も積み重なれば検出する
- 出力する値そのものは変わらない。変わるのは **止まっている状態から光が変わったときの、最初の反応の速さ** だけ
- レンダリングのノイズ (Lumen など) で落ち着かないときは `Idle Change Threshold` を上げる。今減らしているかはモニターと `LightSync.Stats` (`idle=yes`) で分かる

**実測** (開発機。8 プローブ、30 Hz、静止したシーン、-game 1280×720)

| | 1 プローブのキャプチャ | フレーム時間 | うち LightSync の分 |
|--|----------------------|-------------|--------------------|
| オフ | 29.9 回/秒 | 4.72 ms | 2.56 ms |
| オン (5 Hz) | 5.9 回/秒 (96% の時間を減らした状態で過ごす) | 2.79 ms | 0.63 ms |

(プローブなしのフレーム時間は 2.16 ms)

**反応の遅れ** (エディタ 120 fps、1 プローブ、スムージング 0。ドームの色を変えてから新しい値の Art-Net が届くまで、各 16 回)

| | 平均 | 最小 〜 最大 |
|--|------|-------------|
| オフ (30 Hz) | 36 ms | 22 〜 49 ms |
| オン、Idle Sampling Rate 10 Hz | 73 ms | 25 〜 125 ms |
| オン、Idle Sampling Rate 5 Hz (既定) | 109 ms | 35 〜 218 ms |

増える遅れは平均でおよそ `1 / (2 × Idle Sampling Rate)`、最大で `1 / Idle Sampling Rate` です。変化を見つけた後は通常の速さで追います。
すばやい明転・暗転を合わせたいシーンでは、オフにするか `Idle Sampling Rate` を上げてください。

光がずっと変わり続けるシーンでは減らす時間がないので、効果はありません (負荷はオフと同じ)。キューの合間に止まっている時間が長いシーンほど効きます。

---

## モニターウィンドウ

**Window → Virtual Production → LightSync DMX Monitor**

- **ツールバー**: 全て有効 / 全て無効 / ブラックアウト / 今すぐサンプリング / マスターディマー / デバッグ表示 (ビューポートに各プローブの出力色を球で表示)
- **プローブ一覧**: 状態と先頭ゾーンの色
- 選択したプローブの設定 (編集はレベルの未保存変更になる。選択肢はドロップダウン):
  - 基本
  - 色の基準 (モード、取得元、nDisplay のビューポート候補と適用中のレイヤー、露出、ゲイン、スムージング)
  - ゾーンごとの結果 (正規化照度)
  - DMX: **フィクスチャを選んで** 編集 (追加 / 削除も可)。ゾーン、プリセット、プロファイル、使用チャンネル、ソルバの出力と警告 (色域外 / 明るさ上限)
  - OSC: **ターゲットを選んで** 編集。フォーマット、送る値 (ソルバ後の値 / 目標リニア / 目標 sRGB)
  - キャリブレーション

コンソールから `LightSync.OpenMonitor [プローブ名] [幅 高さ]` でも開けます。

---

## Blueprint / Python / コンソール

```
// プローブ
LightProbeActor.GetZoneColor(ZoneName) -> (bool, LinearColor)
LightProbeActor.ForceSampleOnce()
LightProbeActor.SetProbeActive(bool)

// 一括制御 (LightSyncBlueprintLibrary)
GetAllLightProbes / SetAllProbesActive / BlackoutAllProbes / SetMasterDimmer

// 出力
DMXColorOutputComponent.SendZoneColors / SendColor / SendRawToFixture / Blackout
OSCColorOutputComponent.SendZoneColors / SendCustomOSCFloat / SendCustomOSCInt

// キャリブレーション (ULightSyncCalibrator)
BuildSteps / ShowFixtureStep / ShowWallStep / CaptureCameraFeed
RecordFixtureMeasurement / RecordWallMeasurement / FitFixture / FitStage / EndTestOutput
CreateProfileAssetForFixture / CreateStageCalibrationAsset   (エディタのみ)

// 検証
LightColorSamplerComponent.ValidateAgainstCPU() -> (bool, Report, MaxRelError)
```

| コンソールコマンド | 内容 |
|-------------------|------|
| `LightSync.Stats` | 各プローブのキャプチャ数、結果待ちで省いた数、フレーム上限で持ち越した数、遅延、ゲームスレッド時間、更新を減らしているか、この PC が送るか |
| `LightSync.ValidateGPU` | GPU の積分を CPU 版と比べる ([GPU の検証](#gpu-の検証)) |
| `LightSync.OpenMonitor [プローブ名] [幅 高さ]` | モニターを開く (エディタ) |
| `LightSync.SnapshotMonitor <PNG パス> [スクロール位置]` | モニターの見た目を PNG に保存 (エディタ、記録や不具合報告用) |

| 起動引数 | 内容 |
|----------|------|
| `-LightSyncOutput=On` / `Off` | この PC から送るかを上書き ([照明を送る PC](#照明を送る-pc)) |

各プローブの統計は、プレイ終了時にもログに出ます (`LIGHTSYNC_STATS`)。

---

## 検証と診断

### 自動テスト

**Session Frontend → Automation** で `LightSync` を実行するか、コマンドラインで実行します。

```
UnrealEditor-Cmd <Project>.uproject -ExecCmds="Automation RunTests LightSync; Quit" -unattended -nullrhi
```

| グループ | 内容 |
|----------|------|
| `LightSync.Zones.*` | 立体角の合計 = 4π、一様環境 = 1.0、半球、分割で二重計上しない、除外、回転、Key の方向、EOTF のデコード、変化の検出 |
| `LightSync.ColorMath.*` | 色空間、CCT/Duv (D65 → 6504K、Planck 往復)、ソルバ、調光カーブ、量子化、Log の線形化、フィット |
| `LightSync.Fixture.*` | 各プリセットの DMX 値、Dimmer 分離、ガンマ逆変換、16bit、キャリブレーション適用、ブラックアウト |
| `LightSync.Calibration.*` | 合成した測定値から応答・ガンマ・LUT・M_ref・壁の応答カーブを復元。校正後に灯体の光がカメラ上で壁と一致する |
| `LightSync.NDisplay.*` | レイヤー合成の規則 |
| `LightSync.Output.*` | どの PC が送るか (方針・クラスタノード・起動引数)、直接ブラックアウトのパケット (Art-Net 4 / E1.31) |

### GPU の検証

静止したシーンで、コンソールコマンドを実行します。

```
LightSync.ValidateGPU
```

全プローブについて、GPU の積分結果と CPU の同じ式 (テストオラクル) を比べ、ゾーンごとの誤差をログに出します。開発時の実測では誤差 0.0007% 以下でした。

### 実レンダリングでの確認 (開発時に実施)

- 一様な Unlit ドーム (0.5, 1, 2): Scene モードで全ゾーン誤差 0.006%。EV +2 で正確に 1/4
- Wall モード: 壁へ送る信号 (LDR キャプチャ) と、8bit 量子化の範囲で一致
- DMX / OSC: 実パケットがソルバの理論値と一致 (例: RGB 64,128,32 / 10ch 0,0,96,255,85,255,0,85,0,0)
- キャリブレーション: パッチの測定、M_ref の復元、テスト点灯の送信、映像の読み取り
- nDisplay: ICVFX テンプレートの設定で、合成結果が理論どおり。同じ PP を持つ参照カメラと描画結果が一致
- 壁の応答ガンマ 1.2: プローブの出力が、ガンマなしの値の 1.2 乗と一致
- 送信の制御: `-LightSyncOutput=Off` でパケット 0・キャプチャ 0
- 終了時: エディタ / -game / パッケージ版を閉じた後、最後に届いた Art-Net と OSC がブラックアウト (コンソールウィンドウを閉じた場合も)

**実機の LED ウォール・灯体・カメラでの最終的な一致は、各ステージでキャリブレーションして確認してください。**

---

## 制約と注意点

- **終了時の消灯** は直接送信で届けます ([終了時のブラックアウト](#終了時のブラックアウト))。ただし、プロセスが強制終了 (クラッシュ、タスクマネージャー) した場合は送れません。照明卓や DMX ノード側でも、信号が途絶えたときの動作 (消灯 / 保持) を決めておいてください
- **ポストプロセスボリューム** はプローブの位置で評価されます。nDisplay の視点と同じボリュームの中に置いてください (Unbound なら問題ありません)
- 参照カメラの露出が **Auto** の場合は Manual として計算します。ICVFX では Manual 露出を推奨します
- nDisplay の **Start** カスタム PP は「シーンの下」に入るのが本来の順序です。プローブではボリュームの評価順を完全には再現できません (通常の運用では使われない内部項目です)
- **OCIO** を使う nDisplay 構成には未対応です (OCIO 変換後の信号を前提にした EOTF 選択が必要)
- 内蔵プリセットのアンバーは近似値です。RGBAW 灯体はキャリブレーションしてください
- **PIE 中** は、エディタワールド側のプローブは送信しません (二重送信を防ぐため)
- 最大ゾーン数は 8 です

---

## トラブルシューティング

| 症状 | 確認すること |
|------|-------------|
| 照明が出ない / ログに「この PC からは送信しない」 | [照明を送る PC](#照明を送る-pc) の設定と起動引数。理由がログの括弧内に出る |
| nDisplay で照明がちらつく (複数の PC が送っている) | 送る PC を 1 台に。Multi-User のエディタとクラスタを同時に動かすときは、どちらかを `-LightSyncOutput=Off` に |
| プローブが多いとフレームが重い | [性能と遅延](#性能と遅延)。`Max Captures Per Frame`、`Sampling Rate`、照明専用 PC |
| DMX が出ない / ログに「DMX出力ポートが設定されていません」 | Project Settings → Plugins → DMX → Output Ports にポートを追加。DMX Library 指定時はライブラリ側の Output Ports も確認 |
| ログに「512ch を超えます」 | Start Channel を下げる。モニターの「使用チャンネル」で範囲を確認 |
| フィクスチャが送られない / 「ゾーン '...' がプローブにありません」 | マッピングの Zone 名とプローブの Zones の Name を揃える |
| 色が白っぽい・彩度が足りない (**色域外**) | 灯体の色域外。白の方向へ彩度を下げて出している。キャリブレーション済みか確認、より広色域の灯体を使う |
| 明るさが頭打ち (**明るさ上限**) | 強さのゲイン / Output Gain を下げるか、灯体を近づける |
| 壁と照明の明るさが合わない | 参照カメラの露出が Manual か、Reference Mode と Post Process Source が正しいか。キャリブレーションの強さの合わせ方を参照 |
| OSC が届かない | IP / Port、受信側のファイアウォール (UDP)、`Use OSC Output` と各 Target の `Enabled` |
| パッチが壁に映らない | パッチは `LightSync Calibration Patch` アクター。Multi-User 経由で nDisplay ノードに反映されているか確認。マテリアル `/LightSyncDMX/Calibration/M_LightSyncPatch` があるか |
| PIE 中にエディタ側プローブの色が止まる | 仕様。PIE ワールドのプローブだけが送信する |
| プローブを止めると照明が消える | 仕様。最後の色を残すなら `Blackout When Inactive` をオフ |
| GPU の結果が怪しい | 静止したシーンで `LightSync.ValidateGPU` を実行し、CPU 版との誤差を見る |

---

## インストールとビルド

### 動作要件

- Unreal Engine 5.7 (ビルド済みバイナリは 5.7 専用)、Win64
- DMX Protocol / DMX Engine / OSC プラグイン (UE 同梱)
- Art-Net / sACN 対応の DMX インターフェース (DMX を直接出す場合)
- nDisplay (任意。連携を使う場合)

### バイナリ配布版

1. ZIP を展開し、`LightSyncDMX` フォルダをプロジェクトの `Plugins/` にコピー
2. エディタを起動 (自動で有効になる)
3. **Window → Virtual Production → LightSync DMX Monitor**

### ソースから (開発者向け)

`LightSyncDMX` フォルダをプロジェクトの `Plugins/` に置いてエディタを起動すると、ソースからビルドされます。

MSVC 14.43 以降で `__has_feature` のエラーが出る場合:

- プロジェクトの `Source/` に `MSVCCompat.h` を作る

  ```cpp
  #pragma once
  #ifndef __has_feature
  #define __has_feature(x) 0
  #endif
  #ifndef __has_extension
  #define __has_extension(x) 0
  #endif
  ```

- `*.Target.cs` に追記する

  ```csharp
  AdditionalCompilerArguments = "/FI\"" + Path.Combine(ProjectDirectory, "Source", "MSVCCompat.h") + "\"";
  ```

### パッケージビルド (配布者向け)

`BuildPlugin.bat [UE のインストール先]` を実行すると、`PackagedPlugin/` にバイナリ同梱のプラグインができます (Editor / Game Development / Shipping)。

### ファイル構成

```
LightSyncDMX/
├── LightSyncDMX.uplugin
├── BuildPlugin.bat
├── Config/FilterPlugin.ini
├── Content/Calibration/M_LightSyncPatch.uasset   キャリブレーション用パッチ
├── Resources/Icon128.png
├── Shaders/Private/LightSyncAverageCS.usf        ゾーン積分 (ピーク → 精密化 → 積分。6 面 × 多数のグループ + 合計)
└── Source/
    ├── LightSyncDMX/                               ランタイム
    │   ├── LightProbeActor                         プローブ、基準モード、露出
    │   ├── LightColorSamplerComponent              ゾーン、スムージング
    │   ├── LightSyncGPUSampler                     GPU 積分と非同期リードバック
    │   ├── LightSyncZones                          ゾーン定義、CPU 版積分器 (オラクル)
    │   ├── LightSyncColorMath                      色空間、CCT/Duv、ソルバ、調光カーブ、Log、フィット
    │   ├── LightSyncFixtureProfile                 灯体プロファイル、プリセット、DMX 値の計算
    │   ├── DMXColorOutputComponent / OSCColorOutputComponent
    │   ├── LightSyncCalibrator / LightSyncCalibrationPatch
    │   ├── LightSyncNDisplay                       nDisplay の PP 合成 (リフレクション)
    │   ├── LightSyncSettings                       Project Settings、どの PC が送るか
    │   ├── LightSyncDirectOutput                   終了時の直接ブラックアウト (Art-Net / sACN)
    │   ├── LightSyncSubsystem / LightSyncBlueprintLibrary
    │   └── Private/Tests/                          自動テスト
    └── LightSyncDMXEditor/                         モニターウィンドウ
```

---

## 1.x からの移行

2.0 は色の計算を作り直したため、**プローブの色関連の設定に互換性はありません**。
DMX の Universe / Start Channel / カラーモード (= 内蔵プリセット)、OSC の送信先は引き継がれます。

| 1.x | 2.0 |
|-----|-----|
| DMX / OSC の Color Source (Average / Top / Side / Dominant) | フィクスチャ / ターゲットごとの **Zone** (既定ゾーン `Average` / `Top` / `Side` / `Key`) |
| ガンマ補正・色温度オフセット・彩度・明度・色のみモード | 削除。基準モード + 露出 + Output Gain、灯体側はプロファイルと強さのゲイン |
| サンプラーの輝度べき乗・直接光フィルター・HDR クランプ・独自トーンマップ・彩度ブースト・面の重み | 削除 (立体角で正しい積分に置き換え) |
| Smoothing Alpha (サンプルごと) | Smoothing Time Constant (秒) |
| 10ch モードの固定値 (G/M・Crossfade・Strobe・Reserved) | プロファイルの Fixed チャンネル |
| CCT の B/R 比による推定 | Ohno 2013 の CCT + Duv |

## ライセンス

Copyright UE-Comp. All Rights Reserved.
