# Phase 5 Spec: run / retry / lock を導入する

## 1. 位置づけ

`docs/PLAN.md` の Phase 5 を、実装に着手できる粒度まで具体化した仕様書である。

前提として、Phase 4 までに以下は導入済みとみなす。

- `runtime/state/tasks/*.json` が task schema v2 に準拠している
- `runtime/events/events.jsonl` が各操作の append-only ログを持つ
- `runtime/runs/*/meta.json` に run の基本情報が保存されている
- `priority`, `depends_on`, `approval_required`, `related_paths`, `generated_by` が task state に存在する
- `ready` は派生状態として扱われ、`ap start` が runnable 判定を行う

この Phase の役割は以下である。

- run に明示的な id と attempt 番号を付与し、実行試行を一意に識別できるようにする
- project / task 単位の lock を導入し、二重実行を防ぐ
- retry policy を導入し、failed task の再試行回数を制御する
- timeout を設け、無限実行を防ぐ
- idempotency の原則を確立し、実行記録の整合性を保つ

重要な注意点として、`PLAN.md` には以下の番号ずれがある。

- Milestone 3: run 管理と blocker 管理
- Phase 5: run / retry / lock を導入する

本書は後者、つまり「二重実行防止・再試行制御」の詳細仕様である。

## 2. ゴールと非ゴール

### 2.1 ゴール

- `ap start` が実行中に再度呼ばれても二重実行にならない
- task 単位でも lock が取れ、同一 task の並行実行を防ぐ
- failed task の自動 retry が最大 N 回で停止する
- blocked task は自動 retry しない
- `approval_required == true` の task は lock/retry の対象にならず停止する
- run の attempt 番号が task state と run meta の双方に記録される
- timeout が設定でき、エージェントが一定時間内に終わらない場合は failed/blocked 扱いにできる
- lock ファイルが残った場合の stale lock 検出と手動解除手順がある
- 実行結果に idempotency を持たせ、同じ run を再処理しても状態が壊れない

### 2.2 非ゴール

- reviewer の導入
- task 自動生成
- 複数 project の並列実行
- 複数 task の並列実行（parallel run）
- 高度な DAG スケジューラ
- 分散ロックサービス（ZooKeeper, etcd など）
- 分散メッセージキュー
- 複雑な backoff アルゴリズム（exponential backoff の実装は後回し）

Phase 5 の目的は「単一 project / 単一 task の安全な実行管理」の確立であり、並列化や高度な retry 戦略は後続 Phase で対応する。

## 3. 基本方針

Phase 5 では実行の安全性を以下の 3 層で確保する。

1. **lock**: 二重実行を物理的に防ぐ
2. **retry policy**: 失敗時の再試行を有限かつ制御可能にする
3. **timeout**: 無限実行を防ぎ、hung run を検出する

いずれも「シンプルに始める」原則を維持する。

- lock はファイルベース（`runtime/lock/`）
- retry は task state の `attempt_count` と `max_retries` で制御
- timeout はプロセス kill + event 記録

これにより、常駐プロセスや外部サービスなしで実現できる。

## 4. runtime ディレクトリ構成の変更

Phase 5 では `runtime/lock/` を追加する。

```text
$HOME/.autopilot/projects/<project_name>/
  TODO.md
  dashboard.md
  runtime/
    last_run.json
    lock/
      project.lock
      task-<task_id>.lock
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

lock ファイルはプロセスが存在する間だけ保持する。プロセス終了時（正常・異常問わず）に削除する。

## 5. lock 仕様

### 5.1 lock の種類

Phase 5 では以下の 2 種類の lock を導入する。

- **project lock**: `runtime/lock/project.lock`
  - project 全体の排他制御
  - `ap start` 起動時に取得し、終了時に解放する
- **task lock**: `runtime/lock/task-<task_id>.lock`
  - 個別 task の排他制御
  - task 実行開始時に取得し、完了時に解放する

Phase 5 では 1 project につき 1 task しか並列実行しないため、task lock の主な用途は stale 検出と将来の並列化への備えである。

### 5.2 lock ファイルの内容

lock ファイルには最低限以下を記録する。

```json
{
  "pid": 12345,
  "run_id": "run-20260316-120000-L1",
  "task_id": "task-0003",
  "started_at": "2026-03-16T12:00:00+09:00",
  "hostname": "dev-host"
}
```

- `pid`: lock を保持するプロセスの PID
- `run_id`: 現在の run id
- `task_id`: 実行中 task id（project lock では `null` でもよい）
- `started_at`: lock 取得時刻
- `hostname`: lock を取得したホスト名

### 5.3 lock 取得のフロー

1. lock ファイルが存在するか確認する
2. 存在しない → lock ファイルを作成し、実行を継続する
3. 存在する → stale 判定を行う（後述）
4. stale でない → 実行を中断し、エラーを出す
5. stale → lock ファイルを削除し、lock を再取得して実行する

### 5.4 stale lock の判定

lock ファイルが存在するとき、以下のいずれかが満たされれば stale とみなす。

- lock ファイル内の `pid` が存在しないプロセスである
- lock ファイルの `started_at` が `timeout` + 一定マージン（例: 5 分）を超えている

stale でないと判断した場合は以下のメッセージを出して終了する。

```text
ap start failed: project is already locked by pid 12345 (run: run-20260316-120000-L1)
```

### 5.5 lock の解放タイミング

- 正常完了時: プロセス終了直前に lock ファイルを削除
- エラー終了時: 同上
- シグナル受信時（SIGTERM, SIGINT）: signal handler でも lock ファイルを削除

lock ディレクトリ自体は作成したままでよい。

### 5.6 lock 取得失敗時の event

lock 取得に失敗した場合、event を記録せずに終了してよい。ただし lock が stale であった場合は以下の event を残す。

```json
{
  "type": "lock.stale_detected",
  "payload": {
    "stale_pid": 12345,
    "stale_run_id": "run-20260316-110000-L1"
  }
}
```

## 6. run id と attempt 番号

### 6.1 run id の形式

Phase 4 の形式を継続する。

```
run-YYYYMMDD-HHMMSS-LNN
```

例: `run-20260316-120000-L1`

`LNN` の `L` は lock 番号（project に対して単調増加）、`NN` は 0 埋め 2 桁以上の数字とする。Phase 5 では L 番号は lock 取得ごとに `runtime/state/project.json` の `run_counter` をインクリメントして生成する。

### 6.2 attempt 番号

`attempt` は task に対する試行回数であり、`task.json` の `attempt_count` と一致させる。

- 初回実行: `attempt = 1`
- retry 時: `attempt = attempt_count + 1`

run meta に `attempt` フィールドを追加する。

```json
{
  "id": "run-20260316-120000-L1",
  "task_id": "task-0003",
  "agent": "coder.claude",
  "attempt": 2,
  "status": "finished",
  "started_at": "2026-03-16T12:00:00+09:00",
  "ended_at": "2026-03-16T12:05:30+09:00",
  "exit_reason": "done"
}
```

### 6.3 `exit_reason` の値

run meta に `exit_reason` を追加する。取りうる値は以下。

| 値 | 意味 |
|---|---|
| `done` | 正常完了 |
| `failed` | 実行はできたが失敗 |
| `blocked` | 外部要因・人間判断待ちで停止 |
| `timeout` | タイムアウトで強制終了 |
| `lock_conflict` | lock 取得失敗（通常この場合は run 自体が作られない） |
| `internal_error` | ap 内部エラー |

## 7. retry policy

### 7.1 retry の基本方針

Phase 5 での retry 方針は以下の通りである。

- `failed` task は最大 `max_retries` 回まで自動 retry する
- `blocked` task は自動 retry しない（人間の介入が必要）
- `approval_required == true` の task は retry しない
- `done` / `cancelled` task は retry しない

### 7.2 task schema の追加フィールド

Phase 5 では task schema に以下を追加する。

```json
{
  "attempt_count": 2,
  "max_retries": 3,
  "retry_on": ["failed"],
  "last_run_id": "run-20260316-120000-L1",
  "last_run_exit_reason": "failed"
}
```

| フィールド | 型 | 説明 |
|---|---|---|
| `attempt_count` | int | これまでの実行試行回数 |
| `max_retries` | int | 最大自動 retry 回数。`0` は retry なし |
| `retry_on` | string[] | retry 対象の `exit_reason` リスト |
| `last_run_id` | string\|null | 直近 run id |
| `last_run_exit_reason` | string\|null | 直近 run の `exit_reason` |

`max_retries` の既定値は `3` とする。`retry_on` の既定値は `["failed"]` とする。

### 7.3 runnable 判定への組み込み

Phase 4 の runnable 条件に以下を追加する。

- `status == "failed"` のとき、`attempt_count < max_retries` であれば runnable
- `status == "failed"` のとき、`attempt_count >= max_retries` であれば runnable から除外
- `status == "blocked"` は引き続き runnable から除外（変更なし）

### 7.4 retry 上限超過時の扱い

`attempt_count >= max_retries` に達した failed task は `status` を `failed` のまま維持し、`runnable` から外す。

status を新たに `exhausted` などに変えない理由:

- status の種類を増やすと Phase 4 の投影モデルが複雑になる
- `attempt_count >= max_retries` の組み合わせで十分に識別できる
- `failed` のまま `runnable == false` にすることで briefing/dashboard でも扱いやすい

stderr 例:

```text
ap start: task task-0003 has reached max retries (3/3), skipping
```

event を残す。

```json
{
  "type": "task.retry_exhausted",
  "payload": {
    "task_id": "task-0003",
    "attempt_count": 3,
    "max_retries": 3
  }
}
```

### 7.5 retry 時の処理フロー

1. `ap start` が runnable task を選択する
2. 選択された task の `status == "failed"` かつ `attempt_count < max_retries` を確認する
3. project lock を取得する
4. task lock を取得する
5. `attempt_count` をインクリメントし task state を更新する
6. `task.status_changed` event を記録する（`in_progress` へ遷移）
7. run を開始する
8. 完了後、`exit_reason` に応じて task state を更新する
9. lock を解放する

## 8. timeout 仕様

### 8.1 timeout の目的

エージェント（codex / claude）がハングアップした場合に、ap プロセスが永続しないようにする。

### 8.2 timeout の設定場所

timeout は以下の優先順位で決定する。

1. `ap start --timeout <seconds>` で明示指定
2. `runtime/state/project.json` の `default_timeout_seconds`
3. 組み込み既定値: `1800`（30 分）

```json
{
  "project": "demo",
  "default_timeout_seconds": 1800
}
```

### 8.3 timeout 発生時の処理

1. エージェントプロセスを `SIGTERM` で停止する
2. 数秒後も応答しない場合は `SIGKILL` する
3. run meta の `exit_reason` を `timeout` にして保存する
4. task state を `failed` に更新する（`blocked` にしない）
5. `run.timeout` event を記録する
6. lock を解放する

理由: timeout は実行失敗の一形態であり、外部要因ではないため `failed` とする。ただし `last_error` に `timeout` の旨を記録し、区別できるようにする。

### 8.4 timeout 関連 event

```json
{
  "type": "run.timeout",
  "payload": {
    "task_id": "task-0003",
    "run_id": "run-20260316-120000-L1",
    "timeout_seconds": 1800,
    "pid": 12456
  }
}
```

## 9. idempotency の原則

### 9.1 なぜ idempotency が必要か

ap プロセスが途中でクラッシュした場合、次回起動時に状態が不整合になりうる。idempotency の原則を守ることで、再実行しても状態が壊れないようにする。

### 9.2 idempotency の確保方法

- run id は事前に採番し、run 開始前に lock ファイルと task state に記録する
- 同一 run id の run が `runs/` に存在する場合、それを重複実行とみなす
- task state が `in_progress` でも run の `meta.json` が存在しない場合は interrupted run とみなし、`failed` に戻す
- event log は append-only であり、書き込み済みの event を変更・削除しない

### 9.3 interrupted run の検出と回復

`ap start` 実行時に以下の状態を検出する。

- task state が `in_progress` である
- 対応する run の `meta.json` が存在しない、または `status` が `running` のままである
- lock ファイルが存在しない（クラッシュで削除されなかった場合は stale 判定）

この場合、以下を行う。

1. interrupted run として event を記録する
2. task state を `failed` に戻す
3. `last_error` に `interrupted` を記録する
4. 通常の runnable 判定へ進む

```json
{
  "type": "run.interrupted",
  "payload": {
    "task_id": "task-0003",
    "run_id": "run-20260316-115000-L1",
    "detected_at": "2026-03-16T12:00:05+09:00"
  }
}
```

## 10. task schema v3 の追加フィールド

Phase 5 では task schema に以下のフィールドを追加する。

```json
{
  "id": "task-0003",
  "title": "Add retry logic",
  "description": "Implement retry with max attempts",
  "status": "failed",
  "priority": 100,
  "depends_on": [],
  "approval_required": false,
  "related_paths": ["main"],
  "generated_by": "human.todo",
  "source_file": "TODO.md",
  "source_line": 12,
  "source_text": "- [ ] Add retry logic",
  "present_in_todo": true,
  "attempt_count": 2,
  "max_retries": 3,
  "retry_on": ["failed"],
  "latest_run_id": "run-20260316-120000-L1",
  "last_run_exit_reason": "failed",
  "last_error": "codex exited with status 1",
  "blocker_reason": null,
  "blocker_category": null,
  "created_at": "2026-03-15T10:00:00+09:00",
  "updated_at": "2026-03-16T12:05:30+09:00"
}
```

### 10.1 Phase 4 からの変更点

| フィールド | 変更 |
|---|---|
| `attempt_count` | Phase 4 から継続。意味は変わらない |
| `max_retries` | **新規追加**。既定値 `3` |
| `retry_on` | **新規追加**。既定値 `["failed"]` |
| `last_run_exit_reason` | **新規追加**。直近 run の `exit_reason` |
| `latest_run_id` | Phase 4 の `latest_run_id` と同一フィールド、継続 |

### 10.2 Phase 5 では入れないフィールド

- `retry_delay_seconds`（固定 0 でよい）
- `backoff_factor`（exponential backoff は後回し）
- `review_result`（Phase 6 の責務）
- `reviewer_run_id`（Phase 6 の責務）

## 11. event の追加

### 11.1 Phase 5 で追加する event type

| type | タイミング |
|---|---|
| `lock.acquired` | project/task lock 取得時 |
| `lock.released` | project/task lock 解放時 |
| `lock.stale_detected` | stale lock を検出した時 |
| `run.timeout` | タイムアウト発生時 |
| `run.interrupted` | interrupted run を検出した時 |
| `task.retry_exhausted` | retry 上限到達時 |

### 11.2 既存 event との整合

- `task.selected`, `run.started`, `result.final`, `task.status_changed` は Phase 3 からの継続で変更なし
- `run.started` に `attempt` フィールドを payload に追加する

```json
{
  "type": "run.started",
  "payload": {
    "agent": "coder.claude",
    "model": "claude-sonnet-4-6",
    "attempt": 2
  }
}
```

## 12. `ap start` のフロー変更

Phase 5 での `ap start` の実行フローを示す。

```
1. project 読み込み
2. TODO.md と task state を同期（Phase 4 継続）
3. dependency 検証（Phase 4 継続）
4. interrupted run 検出と回復（Phase 5 新規）
5. runnable task を選択（retry 上限チェックを追加）
6. project lock を取得
   - 失敗 → エラー終了
   - stale → stale event 記録 → 再取得
7. task lock を取得
8. run id を採番し、task state に記録
9. task status を in_progress に更新
10. task.selected / run.started event を記録
11. エージェントを spawn
12. timeout タイマーを開始
13. エージェント完了（または timeout/signal）を待つ
14. exit_reason を決定
15. run meta に結果を保存
16. task status を更新（done / failed / blocked）
17. result.final event を記録
18. task lock を解放
19. project lock を解放
20. last_run.json を更新
```

## 13. `project.json` の拡張

Phase 5 では `project.json` に以下を追加する。

```json
{
  "project": "demo",
  "status": "active",
  "active_task_id": "task-0003",
  "active_run_id": "run-20260316-120000-L1",
  "last_run_id": "run-20260316-115000-L0",
  "last_run_at": "2026-03-16T11:55:00+09:00",
  "run_counter": 2,
  "default_timeout_seconds": 1800,
  "task_counts": {
    "todo": 2,
    "in_progress": 1,
    "review_pending": 0,
    "blocked": 1,
    "done": 4,
    "failed": 1,
    "cancelled": 0
  },
  "updated_at": "2026-03-16T12:00:00+09:00"
}
```

追加フィールド:

| フィールド | 説明 |
|---|---|
| `active_run_id` | 現在実行中の run id。未実行時は `null` |
| `run_counter` | run id 採番用の単調増加カウンタ |
| `default_timeout_seconds` | project 既定の timeout 秒数 |

## 14. エラー仕様

Phase 5 では以下が新しい主要エラーになる。

```text
ap start failed: project is already locked by pid 12345 (run: run-20260316-110000-L0)
ap start failed: task task-0003 is already locked
ap start failed: no runnable task (3 tasks exhausted max retries)
ap start: stale lock detected for pid 99999, recovering
ap start: task task-0003 interrupted in previous run, recovering
ap start: task task-0003 timed out after 1800 seconds
```

終了コードは `0` / `1` を継続する。

lock 関連エラーは `1` で終了するが、stale 回復は `0` で継続する。

## 15. 互換性と移行

### 15.1 Phase 4 からの移行

Phase 4 の task state を Phase 5 で読む場合、以下の default を適用する。

- `max_retries = 3`
- `retry_on = ["failed"]`
- `last_run_exit_reason = null`

`attempt_count` は Phase 4 の値をそのまま使う。

### 15.2 互換性の原則

- 既存 `events.jsonl` はそのまま有効
- 既存 `runs/` ディレクトリと `meta.json` の書き換えは不要
- 既存 `TODO.md` の形式は変えない
- `lock/` ディレクトリが存在しない場合は自動作成する

## 16. 推奨実装単位

責務は以下に分けることを推奨する。

- `src/runtime/lock_manager.cpp`
  - lock ファイルの取得・解放・stale 判定
- `src/runtime/retry_policy.cpp`
  - runnable 判定への retry 上限チェック組み込み
  - retry 時の `attempt_count` 更新
- `src/runtime/run_manager.cpp`
  - run id 採番
  - interrupted run 検出と回復
- `src/commands/cmd_start.cpp`
  - timeout タイマー管理
  - signal handler での lock 解放
- `src/runtime/event_log.cpp`
  - lock/retry/timeout 関連 event の追記

## 17. テスト観点

少なくとも以下をカバーする。

- `ap start` 実行中に再度 `ap start` を呼ぶと lock conflict で失敗する
- プロセスが終了した後の lock ファイルは stale と判定される
- stale lock は自動で回復し、実行が継続される
- `attempt_count >= max_retries` の task は runnable から除外される
- `attempt_count < max_retries` の failed task は選択される
- retry 時に `attempt_count` が正しくインクリメントされる
- timeout が発生すると task が `failed` になる
- `exit_reason` が run meta に正しく記録される
- interrupted run（`in_progress` かつ lock なし）は自動回復される
- `blocked` task は `max_retries` に関係なく自動 retry されない
- `approval_required == true` の task は lock 取得前に除外される
- `run_counter` が run ごとに単調増加する
- `lock.acquired` / `lock.released` event が記録される

## 18. Phase 6 以降への引き継ぎ

Phase 5 が完了すると、後続 Phase は以下のようにつなげやすくなる。

- Phase 6 (reviewer)
  - task lock を保持したまま reviewer を起動できる
  - `exit_reason` と `attempt_count` を review 結果に組み合わせた retry 制御ができる
  - `review_pending` への遷移は lock フロー内で自然に組み込める
- Phase 7 (task 自動生成)
  - `task.retry_exhausted` event を根拠に follow-up task を生成しやすい
  - `attempt_count` と `last_run_exit_reason` を task 生成の入力として使いやすい
- Phase 8 (alert / briefing)
  - retry 上限到達 task や blocked task を briefing へ集約しやすい
  - `active_run_id` を使えば、実行中 run のリアルタイム情報も表示できる

## 19. 未解決事項

Phase 5 着手の blocker ではないが、実装前に意識しておくべき点がある。

- `max_retries` を project 全体の既定値として持つか、task ごとに持つか
  - 推奨: task ごとに持ち、未設定時に project 既定値を使う
- timeout 時に task を `blocked` ではなく `failed` にする判断の妥当性
  - 推奨: timeout は再試行可能な失敗として `failed` とし、`max_retries` で上限を設ける
- stale lock 判定のマージン値（timeout + N 分）を設定可能にするか
  - 推奨: `default_timeout_seconds + 300`（5 分）を固定マージンとして使い、設定化は後回し
- lock ディレクトリが OS 再起動で消える tmpfs 上にある場合の考慮
  - 推奨: `runtime/lock/` は `$HOME/.autopilot/` 以下であるため通常は tmpfs ではない。Phase 5 では特別対処不要

この方針であれば、Phase 4 までの task selection モデルを維持しつつ、`autopilot` を「二重実行せず、有限回だけ再試行し、無限実行しない安全な runner」へ一段進められる。
