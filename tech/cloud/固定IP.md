GCP 固定IP（Cloud NAT）で Cloud Functions(第2世代) の外向き通信を固定化する手順

（Nuxt3 フロント＋ Firebase Functions v2／LINE JPKI 向け）

このドキュメントは、外部APIへ“自分の固定IP”でアクセスするために必要な GCP 構成を、概念→手順→検証→トラブルシュートの順にまとめたものです。
前提：リージョンは例として asia-northeast1（東京） を用います。すべてのリソースは同一リージョンで揃えます。

1. 全体像（概念）
[Cloud Functions (2nd gen)] --(Serverless VPC Access Connector)--> [VPC]
                                                            \
                                                             \-> [Cloud Router] -> [Cloud NAT] -> [固定 外部IP] -> Internet


固定IP: 外向き通信の“発信番号”。ドメインではありません。

Cloud Router: NAT のコントロールプレーン（BGP 管理役）。

Cloud NAT: VPC（/コネクタ経由のサブネット）⇄ 固定IP を結びつけ、外向きを その固定IPでSNAT します。

Serverless VPC Access コネクタ: Functions v2 を VPC へ接続する“橋”。（Cloud Functions v2 は Cloud Run 基盤ですが、Functions v2 ではコネクタ方式が一般的）

すでに Function 側の vpcConnector/egress が設定済みなら、NAT の後付けに再デプロイは不要です（設定を変えた場合は再デプロイが必要）。

2. 作るもの（名前例）
リソース	推奨名	リージョン
外部静的IP（Regional）	nat-tokyo-ip-01	asia-northeast1
VPCネットワーク	prod-vpc（既存でも可）	グローバル
Cloud Router	nat-tokyo-router	asia-northeast1
Cloud NAT	nat-tokyo-gateway	asia-northeast1
Serverless VPC Access コネクタ	line-jpki-connector	asia-northeast1
3. 手順（GCP コンソール）
3.1 外部静的IP（Regional）の作成

[VPC ネットワーク] → [外部 IP アドレス]

「予約済みアドレスを予約」

タイプ: IPv4

スコープ: リージョン（asia-northeast1）

名前: nat-tokyo-ip-01

説明: “Cloud NAT outbound IP for Functions v2” など

※「グローバル IP」は対象外です。Regional IP を選びます。

3.2 Serverless VPC Access コネクタの作成

[VPC ネットワーク] → [Serverless VPC アクセス] → [コネクタを作成]

名前: line-jpki-connector

リージョン: asia-northeast1

ネットワーク: prod-vpc（既存VPC）

IP 範囲: 自動 or 手動（/28 以上の未使用プライベート範囲）

最小/最大スループット（コスト最小でOK、後で変更可）

コネクタは時間課金があります。必要最小限で。

3.3 Cloud Router の作成

[VPC ネットワーク] → [Cloud ルーター] → [作成]

名前: nat-tokyo-router

リージョン: asia-northeast1

ネットワーク: prod-vpc

BGP ASN は自動でOK

3.4 Cloud NAT の作成（ここが要点）

[VPC ネットワーク] → [NAT] → [NAT を作成]

名前: nat-tokyo-gateway

リージョン: asia-northeast1

ルーター: nat-tokyo-router

NAT マッピング

対象: 「マネージド プロキシ（GKE/Serverless）」にチェック

※Functions v2/Cloud Run などのサーバーレスを対象にするため

（必要なら VM インスタンスも追加可）

外部IPの割り当て

「手動（既存の外部 IP を選択）」を選び、nat-tokyo-ip-01 を指定

ログ（推奨）

フル または エラーのみ を有効化（後で Logs Explorer で追える）

以上で「Serverless（= Functions v2）→ コネクタ → VPC → Cloud NAT → 固定IP」の経路が完成します。

4. Functions v2 の設定（抜粋）
4.1 典型設定（Callable）
import { onCall } from "firebase-functions/v2/https";
import { defineSecret, defineString } from "firebase-functions/params";

const LINE_JPKI_BASE_URL = defineSecret("LINE_JPKI_BASE_URL");
const LINE_JPKI_CLIENT_ID = defineSecret("LINE_JPKI_CLIENT_ID");
const LINE_JPKI_CLIENT_SECRET = defineSecret("LINE_JPKI_CLIENT_SECRET");

// ★コネクタ名は Secret でなく "文字列パラメータ" の方が適切
const LINE_JPKI_VPC_CONNECTOR = defineString("LINE_JPKI_VPC_CONNECTOR"); // 例: "projects/<proj>/locations/asia-northeast1/connectors/line-jpki-connector"

export const lineJpkiStart = onCall(
  {
    region: "asia-northeast1",
    // VPC / NAT 経由にする
    vpcConnector: LINE_JPKI_VPC_CONNECTOR.value(),
    vpcConnectorEgressSettings: "ALL_TRAFFIC",
    // リモートAPIが少し重い前提で余裕を持つ
    timeoutSeconds: 30,
    memory: "256MiB",
    secrets: [LINE_JPKI_BASE_URL, LINE_JPKI_CLIENT_ID, LINE_JPKI_CLIENT_SECRET],
    // （必要に応じて）呼出元制限や CORS 設定はフロントのドメインに合わせる
  },
  async (req) => {
    // ここで外部APIへ fetch / axios（NAT経由で固定IPから出ていく）
    // ...
    return { ok: true };
  }
);


ポイント

vpcConnector は 完全リソース名（projects/…/locations/…/connectors/…）で指定するのが確実です。

egress=ALL_TRAFFIC により全外向きが VPC/NAT 経由になります。

パラメータは defineString を推奨（コネクタ名は秘密情報ではないため）。

5. 動作確認
5.1 外向きIPの確認（簡易）

関数から以下のようなエンドポイントにアクセスし、返るIPが nat-tokyo-ip-01 に一致するか確認します。

const ip = await fetch("https://ifconfig.me/ip", { method: "GET" }).then(r => r.text());
console.log("Egress IP:", ip.trim()); // 期待: 予約した固定IP

5.2 JPKI サンドボックス到達性

到達できていれば（パスが違っても）HTTP応答が返ります。

到達できなければ DEADLINE_EXCEEDED などのタイムアウト傾向。

まずは**“応答が返る”こと**（= NAT 経由で出口ができていること）を確認 → その後にAPI パス/ヘッダの正当性を詰める順がおすすめ。

5.3 ログでの追跡

Cloud NAT ログ: 「ログ エクスプローラ」で resource.type="nat_gateway" を参照

Functions 実行ログ: resource.type="cloud_run_revision"（第2世代基盤）／関数名で絞り込み

6. よくあるつまずき

リージョン不一致

外部IP（Regional）・Cloud Router・Cloud NAT・コネクタ・Functions は同一リージョンで。

NAT の対象にサーバーレスが入っていない

Cloud NAT 作成時の 「マネージド プロキシ（GKE/Serverless）」 にチェック必須。

vpcConnector が空 or 名前不正

完全リソース名で指定し、プロジェクト名/リージョンを間違えない。

Firewall/Egress 制御

デフォルトで外向きは許可ですが、厳しめの組織ポリシーだと塞がる場合あり。

タイムアウト

外部API応答が遅い／到達できていない場合は timeoutSeconds を 30–60 に拡大して切り分け。

コネクタ作成直後の収束待ち

反映に数分かかることがあります。少し間を置いて再試行。

7. gcloud での作成例（参考・一括化したい場合）
# 1) 外部静的IP（Regional）
gcloud compute addresses create nat-tokyo-ip-01 \
  --region=asia-northeast1

# 2) Serverless VPC Access コネクタ
gcloud compute networks vpc-access connectors create line-jpki-connector \
  --region=asia-northeast1 \
  --network=prod-vpc \
  --range=10.8.0.0/28

# 3) Cloud Router
gcloud compute routers create nat-tokyo-router \
  --network=prod-vpc \
  --region=asia-northeast1

# 4) Cloud NAT（サーバーレス対象 & 予約IPを手動割当）
gcloud compute routers nats create nat-tokyo-gateway \
  --router=nat-tokyo-router \
  --region=asia-northeast1 \
  --nat-all-subnet-ip-ranges \
  --nat-primary-subnet-ip-ranges ALL_SUBNETWORKS_ALL_IP_RANGES \
  --enable-logging \
  --nat-custom-subnet-ip-ranges="" \
  --auto-allocate-nat-external-ips=false \
  --nat-external-ip-pool=nat-tokyo-ip-01 \
  --enable-dynamic-port-allocation

# ※2025年時点のCLIは UI と表現差がある場合あり。
#   “マネージドプロキシ（Serverless）”の有効化はUIが楽です。UIで NAT の対象に Serverless を含めてください。

8. フロント／Callable の呼び出し時の注意

onCall（Callable） は**プリフライト（OPTIONS）**が 204 で返っていれば CORS 的にはOK。

実本体の POST が DEADLINE_EXCEEDED なら、外部API未到達 or 応答遅延の可能性が高い。

まずは 固定IPで外へ出ているか（5.1 の外向きIP確認）を優先チェック。

9. 最小チェックリスト

 すべて asia-northeast1 で揃えた

 Regional 外部IP を予約（nat-tokyo-ip-01）

 Serverless VPC Access コネクタ（line-jpki-connector）作成

 Cloud Router（nat-tokyo-router）作成

 Cloud NAT（nat-tokyo-gateway）作成：マネージドプロキシ ON、手動で外部IP割当

 Function の vpcConnector=projects/<proj>/locations/asia-northeast1/connectors/line-jpki-connector、vpcConnectorEgressSettings=ALL_TRAFFIC

 NAT ログ有効化、Logs Explorer で疎通を確認

 ifconfig.me 等で外向きIPが一致することを確認

10. 付録：切り分けクイックフロー

Callable 叩く → 204 の OPTIONS は返る？（CORS OK）

本体が 504/DEADLINE_EXCEEDED → まず ifconfig.me で固定IP確認

固定IP一致しない → NAT 対象/リージョン/コネクタ/egress 設定を再点検

固定IP一致＆到達するが 4xx → API パス/ヘッダ/メソッドの問題（仕様書再確認）

5xx/遅い → タイムアウト延長＋先方の稼働状況確認

この構成で、LINE JPKI サンドボックス／本番への到達元をあなたの固定IPに統一できます。必要があれば、この文書をさらにプロジェクト名や実リソースIDで具体化した“運用Runbook版”も作れます。
