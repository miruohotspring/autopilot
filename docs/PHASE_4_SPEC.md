# Phase 4 Spec: task state を Markdown から分離する

## 1. 位置づけ

`docs/PLAN.md` の Phase 4 を、実装に着手できる粒度まで具体化した仕様書である。

前提として、Phase 3 までに以下は導入済みとみなす。

- `runtime/state/project.json`
- `runtime/state/tasks/*.json`
- `runtime/events/events.jsonl`
- `runtime/runs/*`
- `done` / `failed` / `blocked` を区別する task state と event

この Phase の役割は、Phase 2/3 で導入した最小 task state を捨てて作り直すことではない。役割は以下である。

- task state を「実行結果の記録」中心から「次の task を選ぶための内部モデル」へ拡張する
- `TODO.md` では持ちにくい task metadata を `runtime/state/tasks/*.json` に持たせる
- `TODO.md` を唯一の truth ではなく、人間向けの入口兼投影先として位置づけ直す
- priority / dependency / approval / path affinity / provenance を後続 Phase の土台として先に入れる

重要な注意点として、`PLAN.md` には以下の番号ずれがある。

- Milestone 4: reviewer 導入
- Phase 4: task state を Markdown から分離する

本書は後者、つまり「task state 拡張」の詳細仕様である。

## 2. ゴールと非ゴール

### 2.1 ゴール

- `runtime/state/tasks/*.json` が Phase 3 より豊かな task schema を持てる
- task ごとに `description`, `priority`, `depends_on`, `approval_required`, `related_paths`, `generated_by` を保持できる
- `TODO.md` の checkbox と title に依存しない task selection が可能になる
- `ready` を task metadata から導出できる
- dependency 未解決 task と approval 待ち task を `TODO.md` 以外の手段で区別できる
- 既存の Phase 3 task state を破壊せずに段階的移行できる
- `ap start` が state 上の metadata を使って runnable task を決定できる

### 2.2 非ゴール

- reviewer 導入
- retry policy 導入
- lock 導入
- task 自動生成の本格導入
- `dashboard.md` 自動更新
- `TODO.md` への metadata 埋め込み記法導入
- `ap tasks edit` のような専用編集 CLI 導入
- dependency DAG の最適化や並列実行

この Phase では「task を richer に表現できるようにする」ことが目的であり、review/retry/generation の制御本体は Phase 5 以降へ送る。

## 3. 基本方針

Phase 4 では task 情報の責務を以下のように整理する。

- `TODO.md`: 人間向けの task 一覧、簡易な完了/再開操作、入口
- `runtime/state/tasks/*.json`: task の現在値と metadata の正本
- `runtime/events/events.jsonl`: task metadata や status 変更の履歴
- `runtime/runs/*`: 実行ログと result の原本

重要なのは、`TODO.md` を捨てることではない。

- task の可視化と軽い手編集は引き続き `TODO.md`
- ただし priority/dependency/approval/path/provenance は Markdown に押し込まない
- `TODO.md` にない metadata は state 側だけで持つ

この方針により、Phase 2/3 の単純な checklist 同期を保ちつつ、後続 Phase に必要な内部表現を先に確保する。

## 4. runtime ディレクトリ構成

Phase 4 でも directory 構成自体は大きく変えない。

```text
$HOME/.autopilot/projects/<project_name>/
  TODO.md
  dashboard.md
  runtime/
    last_run.json
    events/
      events.jsonl
    state/
      project.json
      tasks/
        task-0001.json
        task-0002.json
    runs/
      run-YYYYMMDD-HHMMSS-LNN/
        meta.json
        prompt.txt
        stdout.log
        stderr.log
        result.json
    alerts/
      alert-0001.json
```

変わるのは主に `runtime/state/tasks/*.json` と `project.json` の schema である。

## 5. task schema v2

Phase 4 では各 task file を最低限以下の形に拡張する。

```json
{
  "id": "task-0001",
  "title": "Add ap start command",
  "description": "Implement the initial ap start command and single task execution flow",
  "status": "todo",
  "priority": 100,
  "depends_on": [],
  "approval_required": false,
  "related_paths": ["main"],
  "generated_by": "human.todo",
  "source_file": "TODO.md",
  "source_line": 7,
  "source_text": "- [ ] Add ap start command",
  "present_in_todo": true,
  "attempt_count": 0,
  "latest_run_id": null,
  "last_error": null,
  "blocker_reason": null,
  "blocker_category": null,
  "created_at": "2026-03-15T10:00:00+09:00",
  "updated_at": "2026-03-15T10:00:00+09:00"
}
```

JSON object のネストは最小限に留め、Phase 2/3 の実装方針を崩さない。

## 6. 各フィールドの仕様

### 6.1 必須フィールド

- `id`
  - 内部識別子
  - 既存の `task-0001` 形式を継続する
- `title`
  - 人間向け task 名
- `status`
  - 現在状態
- `priority`
  - 整数。小さいほど優先
  - 既定値は `100`
- `depends_on`
  - task id 配列
  - 依存先は同一 project 内 task に限る
- `approval_required`
  - `true` の task は自動選択しない
- `related_paths`
  - managed path 名の配列
  - 既定値は `["main"]` または解決済み working path 1 件
- `generated_by`
  - task 生成由来
  - Phase 4 では最低限 `human.todo` を使う
- `source_file`, `source_line`, `source_text`
  - `TODO.md` との対応情報
- `present_in_todo`
  - 今回の同期時点で `TODO.md` に見えているか
- `attempt_count`
  - run 開始回数
  - Phase 4 では `retry_count` を導入せず、既存 field を継続する
- `created_at`, `updated_at`
  - ISO 8601 timestamp

### 6.2 任意フィールド

- `description`
  - 詳細説明。未設定なら `null`
- `latest_run_id`
  - 直近 run id
- `last_error`
  - 直近 failed/blocked の短い説明
- `blocker_reason`
  - `blocked` の具体理由
- `blocker_category`
  - 例: `approval_required`, `credentials`, `external_dependency`, `spec_conflict`

### 6.3 Phase 4 では入れないフィールド

以下は将来候補だが、この Phase ではまだ入れない。

- `retry_count`
- `max_retries`
- `review_result`
- `reviewer_run_id`
- `generation_reason`
- `source_task_id`
- `constraints`
- `artifacts`

理由:

- retry は Phase 5 の責務
- reviewer は Phase 6 の責務
- provenance の詳細化は Phase 7 で十分

## 7. status モデル

### 7.1 永続化する status

Phase 4 で task file に保存する status は以下とする。

- `todo`
- `in_progress`
- `review_pending`
- `blocked`
- `done`
- `failed`
- `cancelled`

### 7.2 `ready` は派生状態とする

`PLAN.md` には `ready` が status 候補として挙がっているが、Phase 4 では永続化しない。

代わりに `ready` は以下を満たす task の派生状態として扱う。

- `status == "todo"` または `status == "failed"`
- `approval_required == false`
- `depends_on` の全 task が `done`
- `related_paths` が今回の selected path と両立する

理由:

- `ready` を保存値にすると、dependency や approval 変更のたびに再同期が必要になる
- Phase 4 の目的は selection の改善であり、派生値の重複保存ではない

### 7.3 Phase 4 で自動遷移させる status

この Phase で `ap start` が自動で扱う主な遷移は以下である。

- `todo -> in_progress`
- `failed -> in_progress`
- `in_progress -> done`
- `in_progress -> failed`
- `in_progress -> blocked`

以下は予約または将来導入とする。

- `in_progress -> review_pending`
- `review_pending -> done`
- `review_pending -> blocked`
- `* -> cancelled`

## 8. dependency モデル

### 8.1 依存先の表現

- `depends_on` は title ではなく task id で持つ
- dependency は同一 project 内のみ
- 自己依存は禁止する

例:

```json
"depends_on": ["task-0001", "task-0003"]
```

### 8.2 runnable 判定

task が dependency 的に runnable である条件は以下。

- `depends_on` の各 task が存在する
- その全 task の `status == "done"`

`done` 以外は未解決とみなす。

- `todo`
- `in_progress`
- `failed`
- `blocked`
- `review_pending`
- `cancelled`

### 8.3 不正 dependency

以下は Phase 4 では hard error とする。

- 存在しない task id 参照
- 自己依存
- 明らかな dependency cycle

stderr 例:

```text
ap start failed: invalid task dependency: task-0004 -> task-9999
ap start failed: dependency cycle detected at task-0003
```

Phase 4 では cycle 解消アルゴリズムは実装せず、検出した時点で停止する。

## 9. priority と path affinity

### 9.1 priority

- `priority` は整数
- 値が小さいほど優先
- 既定値は `100`
- 人間が明示した task を後方互換で扱いやすくするため、未設定 task を 100 に寄せる

### 9.2 related_paths

- `related_paths` は managed path 名の配列
- 空配列は「どの path でもよい」を意味しない
- Phase 4 では空配列を許可せず、少なくとも 1 path を持たせる

基本ルール:

- `TODO.md` 由来 task は、現在の working path を 1 件だけ入れる
- 将来 multi-path routing を高度化するまでは、`ap start` で選ばれた path と無関係な task は runnable から外す

### 9.3 tie-break

runnable task が複数ある場合、選択順は以下とする。

1. `priority` 昇順
2. `status` は `todo` を `failed` より先
3. `source_line` 昇順
4. `id` 昇順

Phase 4 でも deterministic selection を維持することを優先する。

## 10. approval と blocker metadata

### 10.1 `approval_required`

`approval_required` は「人間承認がない限り自動実行してはいけない」ことを示す boolean である。

仕様:

- `approval_required == true` の task は `ready` にならない
- `status` が `todo` でも自動選択しない
- `blocked` と併用してよい

### 10.2 blocker 情報

Phase 3 では blocker の詳細は主に event/result 側にあった。Phase 4 では task state 側にも最低限保持する。

- `blocker_reason`
  - 直近 blocker の人間可読な要約
- `blocker_category`
  - 分類名

例:

```json
{
  "status": "blocked",
  "approval_required": true,
  "blocker_reason": "production migration approval needed",
  "blocker_category": "approval_required"
}
```

### 10.3 `last_error` との使い分け

- `last_error`: failed/blocked を問わず直近停止理由の短文
- `blocker_reason`: `blocked` 専用の要約

`blocked` のときは両方同値でもよい。

## 11. `TODO.md` との同期モデル

### 11.1 `TODO.md` の役割

Phase 4 でも `TODO.md` は以下の役割を持つ。

- 人間向け task 一覧
- 新規 human task の入口
- checkbox による `done` / reopen の簡易操作

一方、以下は `TODO.md` から読まない。

- `description`
- `priority`
- `depends_on`
- `approval_required`
- `related_paths`
- `generated_by`

### 11.2 title と checkbox の同期

既存の Phase 2 同期ルールをベースに、以下を継続する。

- title は `TODO.md` 側変更を state へ反映してよい
- `[x]` は `done`
- `[ ]` は少なくとも `done` の解除を意味する

ただし checkbox のみで以下は変更しない。

- `priority`
- `depends_on`
- `approval_required`
- `related_paths`
- `generated_by`
- `attempt_count`

### 11.3 `TODO.md` にない task

state に存在し、今回の `TODO.md` に現れない task は削除しない。

扱い:

- `present_in_todo = false`
- runnable 候補から外す
- 履歴・dependency 参照先としては残す

これにより、将来 generated task や completed task の履歴を保持しやすくなる。

### 11.4 重複 title 制約

Phase 4 でも `TODO.md` 由来の未完了 task に同一 title が複数ある場合は hard error としてよい。

理由:

- dependency は task id で持つため、曖昧な title 再結合を避けたい
- `TODO.md` に task id を埋め込まない方針を維持するため

## 12. metadata の入力経路

Phase 4 の重要な割り切りとして、専用 task 編集 CLI はまだ導入しない。

そのため metadata の入力経路は以下とする。

- 新規 task 生成: `TODO.md`
- metadata 補完/編集: `runtime/state/tasks/<task_id>.json` の手編集

これは UX として理想ではないが、Phase 4 の目的は「内部表現の確立」であり、編集 UX は後続 Phase で改善する。

この判断により以下を避けられる。

- `TODO.md` 独自記法の拙速な導入
- YAML/JSON/Markdown 間の複雑な双方向同期
- reviewer/generator より前に task editor を作り込むこと

## 13. `ap start` の task selection 変更

### 13.1 選択前処理

`ap start` は task selection 前に少なくとも以下を行う。

1. `TODO.md` と task state を同期する
2. Phase 3 以前の task file を v2 schema として補完する
3. dependency の妥当性を検証する
4. working path と各 task の `related_paths` を照合する

### 13.2 runnable 条件

task が selection 候補に入る条件は以下。

- `present_in_todo == true`
- `status == "todo"` または `status == "failed"`
- `approval_required == false`
- dependency がすべて解決済み
- `related_paths` に selected path が含まれる

以下は候補から除外する。

- `in_progress`
- `review_pending`
- `blocked`
- `done`
- `cancelled`

`blocked` を自動再実行しない点は、Phase 5 の retry 方針と整合する。

### 13.3 候補が 0 件のとき

候補が 0 件なら、理由を短く出して失敗する。

例:

```text
ap start failed: no runnable task
```

必要なら内部診断用に以下を区別してよい。

- runnable task がない
- approval 待ちしかない
- dependency 未解決しかない

ただし public stderr は短く保つ。

## 14. event への反映

Phase 4 では event model 自体を大きく作り直さないが、task metadata 変更の履歴は最低限残せるようにする。

### 14.1 追加する event type

- `task.updated`

### 14.2 `task.updated` の用途

以下のような task metadata 変更時に使う。

- `description` 更新
- `priority` 更新
- `depends_on` 更新
- `approval_required` 更新
- `related_paths` 更新
- `generated_by` 更新

payload 例:

```json
{
  "changed_fields": ["priority", "depends_on"],
  "previous": {
    "priority": 100,
    "depends_on": []
  },
  "current": {
    "priority": 10,
    "depends_on": ["task-0001"]
  }
}
```

### 14.3 title / checkbox 同期との関係

Phase 2/3 で使っている `task.discovered` と `task.status_changed` はそのまま残す。

- title 変更のみなら `task.updated`
- checkbox による `done`/reopen は `task.status_changed`
- 新規 checklist 発見は `task.discovered`

## 15. `project.json` の拡張

Phase 4 では project 集計も task schema に合わせて少し拡張する。

推奨 schema:

```json
{
  "project": "demo",
  "status": "active",
  "active_task_id": null,
  "last_run_id": "run-20260315-100000-L7",
  "last_run_at": "2026-03-15T10:04:12+09:00",
  "task_counts": {
    "todo": 3,
    "in_progress": 0,
    "review_pending": 0,
    "blocked": 1,
    "done": 4,
    "failed": 1,
    "cancelled": 0
  },
  "updated_at": "2026-03-15T10:04:12+09:00"
}
```

`ready` は派生値であり、task_counts に保存しない。

## 16. 互換性と移行

### 16.1 既存 task file からの移行

Phase 3 以前の task file は以下の default で読み込めるようにする。

- `description = null`
- `priority = 100`
- `depends_on = []`
- `approval_required = false`
- `related_paths = ["main"]` または現在の resolved path
- `generated_by = "human.todo"`
- `blocker_reason = null`
- `blocker_category = null`

### 16.2 backfill 方針

- 過去 run/event の backfill は不要
- task file は `ap start` 実行時に lazy migration してよい
- 未知 field を reader が無視できる実装に寄せる

### 16.3 互換性の原則

- 既存 `events.jsonl` はそのまま有効
- 既存 `result.json` は書き換えない
- 既存 `TODO.md` 形式を壊さない

## 17. エラー仕様

Phase 4 では以下が新しい主要エラーになる。

- invalid dependency
- dependency cycle
- invalid related path
- invalid priority value
- malformed task state v2

stderr 例:

```text
ap start failed: invalid task dependency: task-0004 -> task-9999
ap start failed: dependency cycle detected at task-0003
ap start failed: invalid related path in task task-0002
ap start failed: failed to read task state: task-0005
```

終了コードは引き続き `0` / `1` でよい。

## 18. 推奨実装単位

責務は以下に分けるのがよい。

- `src/runtime/task_state_store.cpp`
  - schema v2 の read/write と default 補完
- `src/projects/todo_task_sync.cpp`
  - `TODO.md` 由来 field と state-only field の分離
- `src/commands/cmd_start.cpp`
  - runnable 判定と selection
- `src/runtime/event_log.cpp`
  - `task.updated` 追記

必要なら `task selection` を `cmd_start.cpp` から切り出してもよいが、Phase 4 時点では必須ではない。

## 19. テスト観点

少なくとも以下をカバーする。

- 既存 Phase 3 task file を読むと default 補完される
- `priority` の小さい task が先に選ばれる
- 同 priority なら `source_line` が早い task が選ばれる
- dependency 未解決 task は runnable から外れる
- dependency 解決後に task が選ばれる
- `approval_required == true` の task は自動選択されない
- `related_paths` が selected path と不一致の task は選ばれない
- `blocked` task は自動再実行されない
- `project.json.task_counts` に `review_pending` と `cancelled` が入る
- title 変更では metadata field が消えない
- `task.updated` event が metadata 変更時に残る
- duplicate open title は引き続き失敗する

## 20. Phase 5 以降への引き継ぎ

Phase 4 が完了すると、後続 Phase は以下のようにつなげやすくなる。

- Phase 5
  - `attempt_count` と `status` を使って retry policy を入れやすい
  - `approval_required` を停止条件へ組み込みやすい
  - dependency 解決済み task のみを lock 対象にしやすい
- Phase 6
  - `review_pending` を実際の状態として使える
- Phase 7
  - `generated_by` を起点に task provenance を拡張しやすい
  - `depends_on` を使って generated follow-up task を接続しやすい
- Phase 8
  - approval 待ち task と blocked task を briefing へ集約しやすい

## 21. 未解決事項

Phase 4 着手の blocker ではないが、実装前に意識しておくべき点がある。

- metadata 編集 UX をいつ CLI 化するか
- `TODO.md` に存在しない generated task をどの時点で人間へ可視化するか
- dependency cycle 検出をどこまで厳密にするか
- `related_paths` の default を `main` 固定にするか、resolved path から埋めるか

現時点の推奨は以下。

- まずは state JSON 手編集で十分と割り切る
- generated task の可視化は Phase 7 で決める
- cycle 検出は深さ優先探索の最小実装でよい
- `related_paths` は resolved path を 1 件埋める方が安全

この方針なら、Phase 3 までの最小 runner を壊さずに、`autopilot` を「task metadata を根拠に次を選べる orchestrator」へ一段進められる。
