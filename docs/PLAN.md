# autopilot 自律実行オーケストレーション設計方針

## 1. 目的

`autopilot` に `ap start` を追加し、管理中の project に対して以下を段階的に実現する。

- project の現状から次に行うべき task を決定する
- 必要に応じて task を自動生成する
- codex cli / claude code cli を都度起動して task を実行する
- 実行結果を構造化して記録する
- 必要なときだけ人間に alert / briefing を行う
- reviewer 的な後続処理も段階的に導入する

この文書は、実装を一気に完成させるためのものではなく、**段階的に Codex へ実装させるための全体設計書**である。

---

## 2. 全体思想

## 2.1 基本方針

`autopilot` は「常駐する複数エージェントが会話し続けるシステム」ではなく、以下の性質を持つシステムとして設計する。

- `ap start` を起点に必要な処理を進める
- エージェントは必要時にだけ起動する
- エージェント間の中心は会話ではなく **task の進行記録** である
- 状態は Markdown だけでなく、機械可読な state/event でも管理する
- briefing は最終的に event/state を集約して人間に提示する

## 2.2 中心概念

中心に置くべきものは **agent** ではなく **task** である。

- agent は実行者
- task は管理対象
- message は task に紐づくイベント
- state はイベントから再構成または更新される集約結果

つまり、設計の重心は「agent 同士の inbox」ではなく、**project/task/run/event** に置く。

## 2.3 なぜこの方針か

この方針を採る理由は以下。

- codex / claude は常駐会話よりも「都度実行」に向いている
- 実行ログ、成果物、失敗理由、blocker を追跡しやすい
- 人間への briefing と相性が良い
- 将来的に manager / coder / reviewer を導入しても破綻しにくい
- まずは単純な single-run orchestration として実装できる

---

## 3. システムの基本モデル

## 3.1 project

`project` は管理単位であり、複数の path / repository / managed file を持てる。

役割:

- 自律進行の対象単位
- TODO / dashboard / runtime/state/events の格納単位
- briefing の集約単位

## 3.2 task

`task` は実行単位であり、将来的には以下の情報を持つ。

- id
- title
- description
- status
- priority
- dependencies
- constraints
- related paths
- generated_by
- approval_required
- retry_count

## 3.3 run

`run` は、ある task に対する 1 回の実行試行である。

例:

- task-001 に対する attempt-1
- task-001 に対する attempt-2

run は以下を持つ。

- 実行 agent
- start/end timestamp
- stdout/stderr
- partial result
- final result
- exit reason
- artifacts

## 3.4 event

event は append-only の記録であり、履歴の真実である。

例:

- task.selected
- run.started
- result.partial
- result.final
- work.blocked
- approval.requested

## 3.5 state

state は、現在の task / project の集約結果である。

event が履歴であり、state は現在値である。

---

## 4. 目指す最終像

理想的には `ap start` で以下が動く。

1. project を読み込む
2. 現在の TODO / state / dashboard / managed paths を読む
3. 次に着手すべき task を選ぶ
4. task がなければ task 候補を生成する
5. coder agent を起動する
6. 実行結果を event/state に反映する
7. 必要なら reviewer agent を起動する
8. blocker があれば alert として残す
9. 必要なら dashboard/briefing に要約を反映する
10. 継続可能なら次 task へ進む
11. 人間判断が必要な場合は止まり、briefing で拾えるようにする

ただし、これを最初から全部実装しない。

---

## 5. 実装の原則

## 5.1 最初から複雑にしない

初期実装では以下を避ける。

- 常駐 manager / coder / reviewer
- agent 間の自由会話プロトコル
- 分散メッセージキュー
- 複雑な DAG 実行
- 過剰な抽象化

## 5.2 append-only event log を早めに入れる

最も重要なのは、「何が起きたか」を後から追えること。

そのため、早い段階で以下を導入する。

- `events/*.jsonl`
- `state/*.json`
- `runs/*`

## 5.3 Markdown は人間向け、state は機械向け

- `TODO.md` は人間が読む・編集する入口
- `dashboard.md` は人間向けの要約ビュー
- `runtime/state` は機械の truth
- `runtime/events` は履歴

## 5.4 まずは 1 project / 1 active task / 1 active run でもよい

初期段階では並列性を抑える。

---

## 6. マイルストーン

## Milestone 1: `ap start` の最小実装

目的:
- 1 task を選び、1回 agent を起動し、結果を記録する

成功条件:
- `ap start <project>` が動く
- project の TODO から task を 1 件選ぶ
- codex または claude を起動できる
- 実行記録を event として残せる
- task の state が更新される

## Milestone 2: task/state/event の導入

目的:
- TODO.md だけに依存しない内部管理を作る

成功条件:
- `runtime/state/tasks/*.json` を持てる
- `runtime/events/*.jsonl` を持てる
- TODO と state の対応が取れる
- task status が機械的に更新される

## Milestone 3: run 管理と blocker 管理

目的:
- 実行試行と停止理由を明確化する

成功条件:
- attempt/run が識別できる
- blocked / failed / done を区別できる
- retry 可能になる
- alert を残せる

## Milestone 4: reviewer 導入

目的:
- coder の後に review を分離する

成功条件:
- coder 完了後に reviewer を起動できる
- reviewer が approve / rework / blocked を返せる
- 再修正ループを制御できる

## Milestone 5: task 自動生成

目的:
- project 状況から task を増やせるようにする

成功条件:
- task 候補生成の仕組みがある
- 自動生成 task に provenance が残る
- 人間がレビュー可能

## Milestone 6: briefing の高度化

目的:
- dashboard 依存から event/state 集約ベースへ進化させる

成功条件:
- briefing が blocked / pending approval / recent results を集約できる
- dashboard は投影先として扱える

---

## 7. 各 Phase

## Phase 1: `ap start` の入口を作る

### 目的
まずは `ap start` を CLI に追加し、自律実行の起点を作る。

### スコープ
- `ap start [project_name]`
- project 読み込み
- 実行対象 task の選択
- 単一 agent 実行
- 実行結果の保存

### この phase でやること
- CLI コマンド追加
- project runtime ディレクトリ作成
- task 選択処理
- codex/claude 呼び出しラッパ追加
- 最小の result 保存

### まだやらないこと
- reviewer
- loop 実行
- task 自動生成
- 高度な dependency 解決
- human approval ワークフロー

### 完了条件
- 1 task を自動実行できる
- 結果が後で見返せる

---

## Phase 2: runtime 構造を導入する

### 目的
project の内部実行状態を保存する土台を作る。

### 推奨ディレクトリ構成

```text
.autopilot/projects/<project>/
  TODO.md
  dashboard.md
  runtime/
    events/
      events.jsonl
    state/
      project.json
      tasks/
        task-001.json
        task-002.json
    runs/
      run-20260314-120000-task-001/
        meta.json
        stdout.log
        stderr.log
        result.json
    alerts/
      alert-001.json
````

### 最低限の state 項目

* project status
* task status
* task attempt count
* latest run id
* blocked reason
* last updated

### 完了条件

* runtime 以下に状態を保存できる
* 次回起動時にその状態を読める

---

## Phase 3: event モデルを導入する

### 目的

何が起きたかを append-only で残す。

### event の基本方針

各 event は以下を持つ。

* id
* timestamp
* project
* task_id
* run_id
* type
* actor
* payload

### 最低限の event type

* `task.selected`
* `run.started`
* `run.stdout`
* `run.stderr`
* `result.partial`
* `result.final`
* `task.status_changed`
* `task.blocked`
* `alert.created`

### event 例

```json
{"id":"evt-001","timestamp":"2026-03-14T12:00:00+09:00","project":"demo","task_id":"task-001","run_id":"run-001","type":"task.selected","actor":"ap.start","payload":{"reason":"highest_priority_ready_task"}}
{"id":"evt-002","timestamp":"2026-03-14T12:00:01+09:00","project":"demo","task_id":"task-001","run_id":"run-001","type":"run.started","actor":"coder.codex","payload":{"agent":"codex","model":"gpt-5-codex"}}
{"id":"evt-003","timestamp":"2026-03-14T12:01:10+09:00","project":"demo","task_id":"task-001","run_id":"run-001","type":"result.final","actor":"coder.codex","payload":{"status":"done","summary":"implemented ap start skeleton"}}
```

### 完了条件

* すべての重要操作が event に残る
* state が event と矛盾しない

---

## Phase 4: task state を Markdown から分離する

### 目的

`TODO.md` だけでは持てない情報を扱えるようにする。

### 基本方針

* 人間向け: `TODO.md`
* 機械向け: `runtime/state/tasks/*.json`

### task state 例

```json
{
  "id": "task-001",
  "title": "Add ap start command",
  "description": "Implement the initial ap start command and single task execution flow",
  "status": "in_progress",
  "priority": 1,
  "depends_on": [],
  "approval_required": false,
  "retry_count": 0,
  "latest_run_id": "run-001",
  "generated_by": "human",
  "paths": ["main"]
}
```

### status の候補

* `todo`
* `ready`
* `in_progress`
* `review_pending`
* `blocked`
* `done`
* `failed`
* `cancelled`

### 完了条件

* task の内部状態が state で表現できる
* TODO.md は投影・補助として扱える

---

## Phase 5: run / retry / lock を導入する

### 目的

二重実行や無限再試行を防ぐ。

### 必要な概念

* run id
* attempt number
* lock
* retry policy
* timeout
* idempotency

### 最小 lock 方針

* project 単位 lock
* task 単位 lock

### 最小 retry 方針

* blocked は自動 retry しない
* failed は最大 N 回まで
* approval_required は停止

### 完了条件

* 同じ task が二重に走らない
* 再試行回数が管理される

---

## Phase 6: reviewer を導入する

### 目的

coder と reviewer を分離し、品質確認を明確化する。

### reviewer の役割

* 実装結果の確認
* 差分/テスト/仕様整合のレビュー
* approve / rework / blocked の判定

### 処理フロー

1. coder 完了
2. reviewer 起動
3. reviewer 判定
4. approve -> done
5. rework -> task を戻す or follow-up task 作成
6. blocked -> alert

### event の追加候補

* `review.started`
* `review.comment`
* `review.approved`
* `review.rework_requested`
* `review.blocked`

### 完了条件

* coder と reviewer が別実行として記録される
* review 結果が task state に反映される

---

## Phase 7: task 自動生成を導入する

### 目的

project の状態から次の task を提案・生成する。

### task 生成の入力

* TODO.md
* dashboard.md
* 過去の events
* 現在の state
* path/repo の状況
* blocked task の存在

### task 生成の出力

* title
* description
* priority
* dependencies
* rationale
* generated_by

### 重要な原則

自動生成された task には必ず provenance を持たせる。

例:

* `generated_by: manager.llm`
* `generation_reason: blocked_followup`
* `source_task_id: task-003`

### 完了条件

* 既存 task が尽きても次の候補を作れる
* 自動生成 task を後から追跡できる

---

## Phase 8: alert / briefing を統合する

### 目的

必要なときだけ人間が介入できるようにする。

### alert を出すべきケース

* human decision required
* secrets / credentials / external approval が必要
* spec conflict
* repeated failure
* unresolved blocker
* dangerous action request

### alert の最小例

```json
{
  "id": "alert-001",
  "project": "demo",
  "task_id": "task-004",
  "severity": "high",
  "type": "approval_required",
  "message": "Database migration may affect production schema",
  "created_at": "2026-03-14T13:00:00+09:00",
  "status": "open"
}
```

### briefing との関係

`ap briefing` は最終的に以下を集約する。

* open alerts
* blocked tasks
* review pending tasks
* recent completed tasks
* next suggested tasks

### 完了条件

* briefing が dashboard だけでなく state/event/alerts からも構築できる

---

## 8. 推奨データ構造 v0

## 8.1 project state

```json
{
  "project": "demo",
  "status": "active",
  "last_run_at": "2026-03-14T12:30:00+09:00",
  "active_task_id": null,
  "open_alert_count": 1
}
```

## 8.2 task state

```json
{
  "id": "task-001",
  "title": "Implement ap start",
  "status": "in_progress",
  "priority": 1,
  "depends_on": [],
  "approval_required": false,
  "retry_count": 0,
  "latest_run_id": "run-001",
  "generated_by": "human"
}
```

## 8.3 run meta

```json
{
  "id": "run-001",
  "task_id": "task-001",
  "agent": "coder.codex",
  "attempt": 1,
  "status": "finished",
  "started_at": "2026-03-14T12:00:01+09:00",
  "ended_at": "2026-03-14T12:05:30+09:00"
}
```

---

## 9. 実装順の提案

Codex に段階的に実装させる場合、以下の順が良い。

1. `ap start` コマンド追加
2. runtime ディレクトリ作成
3. task state JSON の導入
4. run meta 保存
5. event log 保存
6. task status 遷移
7. lock/retry
8. reviewer
9. task 自動生成
10. alert/briefing 統合

---

## 10. 注意点

## 10.1 TODO.md を唯一の truth にしない

Markdown は人間向けには便利だが、機械制御には不向き。

弱い点:

* lock を持ちにくい
* retry 状態を持ちにくい
* 正規化しづらい
* 競合しやすい

## 10.2 dashboard.md を通信路にしない

dashboard は人間向けの要約先としては有効だが、agent 間の実通信や内部状態の正本にしない。

## 10.3 最初から multi-agent 常駐設計にしない

今の `autopilot` に必要なのは「都度 spawn する runner」であり、常駐 agent mesh ではない。

## 10.4 自動生成 task には由来を残す

誰が、何を根拠に作った task か追跡できないと、後で破綻する。

## 10.5 blocked と failed を分ける

* failed: 実行はできたが失敗
* blocked: 外部要因や人間判断待ちで進めない

この 2 つは必ず分ける。

## 10.6 reviewer 導入前に event/state を固める

reviewer を急いで入れるより、先に履歴と状態の土台を固める方がよい。

## 10.7 人間介入点を消さない

完全自律を目指しても、以下は人間に残すべき。

* 曖昧な仕様決定
* 破壊的変更
* 認証情報や課金に関わる操作
* 本番影響が大きい判断

---

## 11. 将来拡張

将来的には以下へ拡張可能。

* `ap start --loop`
* `ap start --review`
* `ap resume`
* `ap alerts`
* `ap runs`
* `ap tasks`
* `ap state`
* `ap explain <task-id>`

また、将来 Web UI を付ける場合も、event/state ベースなら流用しやすい。

---

## 12. 現時点の実装優先度

最優先:

* `ap start`
* runtime
* task state
* run log
* event log

次点:

* lock / retry
* alert
* briefing 連携

後回し:

* reviewer
* task 自動生成
* 高度な planner
* 複数 agent の柔軟ルーティング

---

## 13. 最終まとめ

`autopilot` に必要なのは、常駐する会話型 multi-agent 基盤ではない。

必要なのは以下である。

* `ap start` を入口にした実行フロー
* task を中心とした進行管理
* append-only event log
* 機械可読 state
* 必要時だけ起動される codex / claude runner
* 必要時だけ人間を呼ぶ alert / briefing

よって、実装は以下の順で段階的に進める。

1. `ap start` 追加
2. runtime/state/events/runs 導入
3. task 状態遷移
4. lock/retry
5. reviewer
6. task 自動生成
7. alert/briefing 高度化

この方針であれば、現在の `autopilot` の project 管理・managed files・briefing 導線を活かしつつ、無理なく自律実行基盤へ発展させられる。
