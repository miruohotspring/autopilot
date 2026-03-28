# Phase 6 Spec: reviewer を導入する

## 1. 位置づけ

`docs/PLAN.md` の Phase 6 を、実装に着手できる粒度まで具体化した仕様書である。

前提として、Phase 5 までに以下は導入済みとみなす。

- `runtime/lock/project.lock` および `runtime/lock/task-<id>.lock` によるファイルベース lock が機能している
- `runtime/state/tasks/*.json` が task schema v3 に準拠し、`attempt_count`, `max_retries`, `retry_on`, `last_run_exit_reason` を持つ
- `runtime/events/events.jsonl` に append-only で event が記録されている
- `runtime/runs/run-*/meta.json` に `exit_reason`, `attempt` が保存されている
- `ap start` が runnable 判定・lock 取得・agent spawn・timeout・lock 解放の全フローを実装している
- task status として `todo`, `ready`, `in_progress`, `review_pending`, `blocked`, `done`, `failed`, `cancelled` が定義されている

この Phase の役割は以下である。

- coder と reviewer を別 run として分離し、品質確認フローを明確化する
- reviewer が `approve`, `rework`, `blocked` を返せるようにする
- `approve` でタスクを `done` に遷移させる
- `rework` でタスクを `todo` に差し戻し、再修正ループを制御する
- `blocked` で alert を生成し、人間への通知を可能にする
- rework の無限ループを防ぐ最大 review サイクル数制御を導入する

## 2. ゴールと非ゴール

### 2.1 ゴール

- `ap start --review` フラグまたは project 設定で review フローを有効化できる
- coder 完了後に reviewer が自動で起動される
- reviewer が構造化された verdict（`approve` / `rework` / `blocked`）を返せる
- `approve` → task が `done` に遷移する
- `rework` → task が `todo` に戻り、reviewer のフィードバックが保存される
- `blocked` → alert が生成され、task が `blocked` に遷移する
- `review_cycle_count` が `max_review_cycles` を超えると強制 `blocked` になる
- reviewer の実行が別の run として記録される（`role: reviewer`）
- review 結果が task state の `review_result`, `reviewer_run_id` に反映される
- reviewer run が publish された後は、task state の `latest_run_id`, `last_run_exit_reason` が reviewer run を指す
- interrupted な reviewer run は coder を再実行せず安全側に倒して回復できる
- review フロー有効時は、`TODO.md` の完了反映を `review.approved` まで遅延できる
- review 関連 event（`review.started`, `review.comment`, `review.approved`, `review.rework_requested`, `review.blocked`）が記録される

### 2.2 非ゴール

- task 自動生成（Phase 7）
- 複数 reviewer の並列実行
- 人間レビュアーによる承認ワークフロー（human approval は別概念として管理）
- GitHub PR との連携
- reviewer の出力の自動適用（コード修正の自動マージ等）
- 常駐型 reviewer プロセス
- reviewer と coder の直接通信

Phase 6 の目的は「coder と reviewer の分離」と「review サイクルの制御」であり、完全自律修正ループの実現は後続 Phase の課題とする。

## 3. 基本方針

Phase 6 では review フローを以下の原則で設計する。

1. **review は基本 opt-in だが task 単位で強制できる**: `--review` フラグまたは `project.json` の `review_enabled: true` で有効化する。デフォルトでは無効だが、`task.review_required: true` はこれらより優先して reviewer を強制起動する。
2. **reviewer は単独 run として記録**: coder run と reviewer run は別の run id を持ち、独立して記録される。
3. **reviewer 出力は構造化 JSON**: reviewer の stdout から `{"verdict": "..."}` 形式の JSON を検出し、機械的に解析する。
4. **rework は task の差し戻し**: rework 時は task state を `todo` に戻し、reviewer のフィードバックを `review_feedback` として保存する。
5. **最大 review サイクル数で無限ループを防ぐ**: `review_cycle_count` は rework 回数を表し、`max_review_cycles`（デフォルト 2）を超えた場合は `blocked` に強制遷移する。
6. **review 有効時は TODO 完了反映を遅延する**: coder が `done` でも review に入る場合は `TODO.md` をまだ `[x]` にしない。`approve` 時に初めて task state と `TODO.md` の両方を完了へ揃える。
7. **lock は coder から reviewer へ継続保持する**: reviewer 実行中も coder run で取得した project/task lock を解放せず保持し、review 完了後にまとめて解放する。

## 4. runtime ディレクトリ構成の変更

Phase 6 では新規ディレクトリの追加はない。既存の `runs/` に reviewer run が追加されるだけである。

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
      run-YYYYMMDD-HHMMSS-LNN/        ← coder run（変更なし）
        meta.json
        prompt.txt
        stdout.log
        stderr.log
        result.json
      run-YYYYMMDD-HHMMSS-LNN/        ← reviewer run（新規）
        meta.json
        prompt.txt
        stdout.log
        stderr.log
        result.json
    alerts/
      alert-0001.json
```

reviewer run の `meta.json` には `role: "reviewer"` を追加する（後述）。

## 5. reviewer の起動条件

### 5.1 review フローの有効化

review フローの有効/無効は以下の優先順位で決定する。

1. task state の `skip_review: true` が設定されている場合は常に review をスキップする
2. task state の `review_required: true` が設定されている場合は review を強制する
3. `ap start --review` フラグが指定された場合は review を有効化する
4. `ap start --no-review` フラグが指定された場合は review を無効化する
5. `runtime/state/project.json` の `review_enabled: true` が設定されている場合は review を有効化する
6. 上記に該当しない場合は review 無効（デフォルト）

review フローが有効でも、以下の条件では reviewer を起動しない。

- coder の `exit_reason` が `done` でない（`failed`, `blocked`, `timeout` の場合は reviewer を起動しない）

### 5.2 coder 完了後の review 起動フロー概要

```
coder run 完了
  ↓
exit_reason == "done" かつ review フローが有効？
  ├─ No → Phase 5 と同じ（task を done に遷移）
  └─ Yes → task を review_pending に遷移
           → TODO.md は未完了のまま維持
           → reviewer run を起動
           → reviewer verdict を処理
```

## 6. reviewer run の仕様

### 6.1 run meta の拡張

reviewer run の `meta.json` に `role` フィールドを追加する。

coder run:
```json
{
  "id": "run-20260318-120000-L1",
  "task_id": "task-0003",
  "agent": "coder.claude",
  "role": "coder",
  "attempt": 1,
  "status": "finished",
  "started_at": "2026-03-18T12:00:00+09:00",
  "ended_at": "2026-03-18T12:05:30+09:00",
  "exit_reason": "done"
}
```

reviewer run:
```json
{
  "id": "run-20260318-120600-L2",
  "task_id": "task-0003",
  "agent": "reviewer.claude",
  "role": "reviewer",
  "coder_run_id": "run-20260318-120000-L1",
  "review_cycle": 1,
  "status": "finished",
  "started_at": "2026-03-18T12:06:00+09:00",
  "ended_at": "2026-03-18T12:08:30+09:00",
  "exit_reason": "done",
  "verdict": "approve"
}
```

reviewer run の id も coder run と同様に `project.run_counter` をインクリメントして採番する。Phase 6 以降、run id の `LNN` 部分は lock 取得回数ではなく、project 内で単調増加する run 連番として扱う。

`role` フィールドの値:

| 値 | 説明 |
|---|---|
| `coder` | 実装担当 run（Phase 5 までの標準 run） |
| `reviewer` | レビュー担当 run（Phase 6 新規） |

reviewer run の `exit_reason` は少なくとも以下を取りうる。

| 値 | 説明 |
|---|---|
| `done` | reviewer が正常終了し、verdict を返した |
| `parse_error` | reviewer は終了したが verdict を解析できなかった |
| `spawn_failed` | reviewer プロセス起動前後で失敗した |
| `timeout` | reviewer が timeout した |
| `non_zero_exit` | reviewer が非ゼロ exit で終了した |
| `signal` | reviewer が signal で終了した |
| `internal_error` | review 処理中に ap 内部エラーが発生した |

### 6.2 reviewer agent の種別

reviewer は `reviewer.claude` として識別する。

これは run meta / event 上の論理的な agent 識別子であり、実際の実行 CLI は既存の `claude` を使う。出力形式も coder と同様に `--output-format stream-json` を前提とする。

`agent_launcher` に以下の agent 種別を追加する。

| agent 識別子 | 説明 |
|---|---|
| `coder.claude` | 実装担当 claude（既存） |
| `coder.codex` | 実装担当 codex（既存） |
| `reviewer.claude` | レビュー担当 claude（Phase 6 新規） |

Phase 6 では `reviewer.codex` は対象外とする。reviewer は自然言語での判断が主なタスクであり、claude が適切である。

### 6.3 reviewer run の publish 境界

Phase 6 では reviewer run を interrupted recovery の対象にするため、spawn 前に最小限 publish する。

- `reviewer run id` の採番だけでは reviewer run は publish されない
- `prompt.txt` の準備後、spawn 前に reviewer 用 `meta.json` を `status: "starting"` で durable に保存した時点で publish とみなす
- `meta.json` 保存後、直ちに task / project state を reviewer run に切り替える
  - task `status` を `review_pending` に更新する
  - task `latest_run_id` を reviewer run id に更新する
  - project `active_run_id` を reviewer run id に設定する
  - 必要なら `reviewer_run_id` を reviewer run id に設定する
- `meta.json` 保存後から state 切り替え完了までの間にクラッシュした場合でも、その reviewer run は publish 済み interrupted review として回復対象に含める
- reviewer プロセス起動成功後に `meta.json` を `status: "running"` に更新し、`review.started` を記録する
- reviewer 完了時に `meta.json` を `status: "finished"` に更新し、`project.active_run_id` を `null` に戻す
- reviewer 完了時に `project.last_run_id` / `project.last_run_at` を reviewer run に更新する
- spawn 前失敗や `starting` 状態での中断でも、publish 済み reviewer run として interrupted recovery の対象にする
- spawn 失敗時も reviewer run は放置せず、`status: "finished"` と reviewer failure 用 `exit_reason` を保存して終端させる

この方針により、`latest_run_id` / `active_run_id` / `last_run_id` が指す run は常に実体を持つ。クラッシュにより `meta.json` だけが先に存在し state 更新が未完了だった場合も、interrupted recovery はその reviewer run を孤立した publish 済み run として検出し、task state をその reviewer run に寄せた上で安全に終端できる。

### 6.4 reviewer への入力（プロンプト構成）

reviewer には以下の情報をプロンプトとして渡す。

```
あなたは autopilot の reviewer エージェントです。
以下のタスクの実装結果をレビューし、verdict を JSON で返してください。

## タスク情報
- ID: {task_id}
- タイトル: {task_title}
- 説明: {task_description}

## coder の実行結果
- run ID: {coder_run_id}
- exit reason: {exit_reason}

## coder の出力（stdout）
{coder_stdout}

## verdict の返し方

必ず以下のいずれかの JSON を stdout の最後に出力してください。

approve の場合:
{"verdict": "approve", "summary": "承認理由の要約"}

rework の場合:
{"verdict": "rework", "issues": ["問題点1", "問題点2"], "suggestions": ["改善案1"]}

blocked の場合:
{"verdict": "blocked", "reason": "ブロック理由", "category": "spec_conflict|human_required|external_dependency|other"}
```

`prompt.txt` にこの内容を保存し、`reviewer.claude` へのプロンプトとして渡す。

### 6.5 reviewer の verdict 抽出

reviewer の stdout から verdict JSON を抽出するアルゴリズム。

1. stdout の生ログを保持したまま、判定用に正規化テキストを作る
2. stdout が Claude の `stream-json` 形式であれば、既存の result classifier と同様に最終テキストを抽出する
3. 正規化後のテキストを末尾から走査する
4. `{"verdict":` を含む行を探す
5. 見つかった場合、その行を JSON として解析する
6. 解析に失敗した場合、より広い JSON ブロック（`{` から `}` まで）を試みる
7. reviewer が `0` で終了したにもかかわらず有効な verdict を得られなかった場合、verdict を `"parse_error"` として扱い `blocked` 相当の処理を行う

reviewer が timeout / signal / 非ゼロ exit で終了した場合は verdict 抽出失敗ではなく `"reviewer_error"` として扱う。これは reviewer 自体の実行失敗であり、task は `blocked` に遷移させる。

verdict の値が上記 3 種以外の場合も `"parse_error"` として扱う。

### 6.6 verdict の処理

#### approve

1. task state の `status` を `done` に更新する
2. `latest_run_id` を reviewer run の id に設定する
3. `last_run_exit_reason` を reviewer run の `exit_reason` に設定する
4. `review_result` を `approve` に設定する
5. `reviewer_run_id` を reviewer run の id に設定する
6. `TODO.md` の対象行を `[x]` に更新する
7. `review.approved` event を記録する
8. reviewer run の task lock を解放する

#### rework

1. `review_cycle_count` をインクリメントする
2. `review_cycle_count > effective_max_review_cycles` の場合は後述の「最大 review サイクル超過」処理へ
3. `latest_run_id` を reviewer run の id に設定する
4. `last_run_exit_reason` を reviewer run の `exit_reason` に設定する
5. task state の `status` を `todo` に更新する
6. `review_result` を `rework` に設定する
7. `reviewer_run_id` を reviewer run の id に設定する
8. `review_feedback` に issues と suggestions を保存する
9. `TODO.md` は未完了のまま維持する（coder 完了時点では `[x]` にしない）
10. `review.rework_requested` event を記録する
11. reviewer run の task lock を解放する
12. 次回 `ap start` 時にこのタスクが runnable として再選択される

ここで `effective_max_review_cycles` は `task.max_review_cycles ?? project.max_review_cycles` で解決する。

#### blocked

1. task state の `status` を `blocked` に更新する
2. `latest_run_id` を reviewer run の id に設定する
3. `last_run_exit_reason` を reviewer run の `exit_reason` に設定する
4. `review_result` を `blocked` に設定する
5. `reviewer_run_id` を reviewer run の id に設定する
6. `blocker_reason` に reviewer の reason を設定する
7. `blocker_category` に reviewer の category を設定する
8. alert を生成する（後述）
9. `review.blocked` event を記録する
10. reviewer run の task lock を解放する

#### parse_error

1. task state の `status` を `blocked` に更新する
2. `latest_run_id` を reviewer run の id に設定する
3. `last_run_exit_reason` を reviewer run の `exit_reason` に設定する
4. `review_result` を `parse_error` に設定する
5. `reviewer_run_id` を reviewer run の id に設定する
6. `blocker_reason` を `"reviewer output could not be parsed"` に設定する
7. `blocker_category` を `"reviewer_error"` に設定する
8. alert を生成する
9. `review.blocked` event を記録する（type payload に `parse_error: true` を含める）

#### reviewer_error

1. task state の `status` を `blocked` に更新する
2. `review_result` を `reviewer_error` に設定する
3. reviewer run が publish 済みの場合は `latest_run_id` を reviewer run の id に設定する
4. reviewer run が publish 済みの場合は `last_run_exit_reason` を reviewer run の `exit_reason` に設定する
5. reviewer run が publish 済みの場合は `reviewer_run_id` を reviewer run の id に設定する
6. reviewer run が publish 済みの場合は `project.last_run_id` / `project.last_run_at` を reviewer run に更新する
7. `blocker_reason` を reviewer の実行失敗理由（timeout / non-zero exit / signal / pre-start failure）に設定する
8. `blocker_category` を `"reviewer_error"` に設定する
9. alert を生成する
10. reviewer run が publish 済みの場合は `review.blocked` event を記録する（type payload に `reviewer_error: true` を含める）
11. reviewer run が publish 済みの場合は reviewer run の `meta.json` を `status: "finished"` に更新し、`exit_reason` に失敗種別（`spawn_failed`, `timeout`, `non_zero_exit`, `signal`, `internal_error`）を保存する

## 7. task schema v4 の追加フィールド

Phase 6 では task schema に以下のフィールドを追加する。

```json
{
  "id": "task-0003",
  "title": "Add reviewer support",
  "description": "Implement review flow in ap start",
  "status": "done",
  "priority": 100,
  "depends_on": [],
  "approval_required": false,
  "related_paths": ["main"],
  "generated_by": "human.todo",
  "source_file": "TODO.md",
  "source_line": 12,
  "source_text": "- [ ] Add reviewer support",
  "present_in_todo": true,
  "attempt_count": 1,
  "max_retries": 3,
  "retry_on": ["failed"],
  "latest_run_id": "run-20260318-120600-L2",
  "last_run_exit_reason": "done",
  "last_error": null,
  "blocker_reason": null,
  "blocker_category": null,
  "review_required": false,
  "skip_review": false,
  "review_result": "approve",
  "reviewer_run_id": "run-20260318-120600-L2",
  "review_cycle_count": 0,
  "max_review_cycles": 2,
  "review_feedback": null,
  "created_at": "2026-03-18T10:00:00+09:00",
  "updated_at": "2026-03-18T12:08:30+09:00"
}
```

### 7.1 Phase 5 からの変更点（追加フィールド一覧）

| フィールド | 型 | 既定値 | 説明 |
|---|---|---|---|
| `review_required` | bool | `false` | このタスクに review を強制するかどうか |
| `skip_review` | bool | `false` | このタスクの review をスキップするかどうか |
| `review_result` | string\|null | `null` | 直近の reviewer verdict（`approve`, `rework`, `blocked`, `parse_error`, `reviewer_error`, `null`） |
| `reviewer_run_id` | string\|null | `null` | 直近の publish 済み reviewer run の id |
| `review_cycle_count` | int | `0` | reviewer が `rework` を返した回数 |
| `max_review_cycles` | int\|null | `null` | task 個別の最大 review サイクル数 override。`null` の場合は project の `max_review_cycles` を使う |
| `review_feedback` | object\|null | `null` | 直近の rework フィードバック（`issues`, `suggestions` を含む） |

### 7.2 `review_feedback` の構造

```json
{
  "review_cycle": 1,
  "reviewer_run_id": "run-20260318-120600-L2",
  "issues": ["テストが不足している", "エラーハンドリングがない"],
  "suggestions": ["unit test を追加する", "stderr に exit_reason を出力する"],
  "recorded_at": "2026-03-18T12:08:30+09:00"
}
```

`review_feedback` は直近の rework 情報のみを保持する。過去の全フィードバック履歴は event log から復元できる。

## 8. event の追加

### 8.1 Phase 6 で追加する event type

| type | タイミング |
|---|---|
| `review.started` | reviewer run 開始時 |
| `review.comment` | reviewer が中間コメントを出力した時（オプション） |
| `review.approved` | reviewer が approve を返した時 |
| `review.rework_requested` | reviewer が rework を返した時 |
| `review.blocked` | reviewer が blocked を返した時、または parse_error / reviewer_error 時 |
| `task.review_cycle_exceeded` | `max_review_cycles` 超過時 |

### 8.2 各 event の payload 例

`review.started`:
```json
{
  "id": "evt-015",
  "timestamp": "2026-03-18T12:06:00+09:00",
  "project": "autopilot",
  "task_id": "task-0003",
  "run_id": "run-20260318-120600-L2",
  "type": "review.started",
  "actor": "reviewer.claude",
  "payload": {
    "coder_run_id": "run-20260318-120000-L1",
    "review_cycle": 1,
    "agent": "reviewer.claude"
  }
}
```

`review.approved`:
```json
{
  "type": "review.approved",
  "payload": {
    "task_id": "task-0003",
    "reviewer_run_id": "run-20260318-120600-L2",
    "review_cycle": 1,
    "summary": "実装が仕様を満たしており、テストも適切"
  }
}
```

`review.rework_requested`:
```json
{
  "type": "review.rework_requested",
  "payload": {
    "task_id": "task-0003",
    "reviewer_run_id": "run-20260318-120600-L2",
    "review_cycle": 1,
    "issues": ["エラーハンドリングが不足", "テストがない"],
    "suggestions": ["try-catch を追加する"]
  }
}
```

`review.blocked`:
```json
{
  "type": "review.blocked",
  "payload": {
    "task_id": "task-0003",
    "reviewer_run_id": "run-20260318-120600-L2",
    "review_cycle": 1,
    "reason": "仕様が曖昧で判断できない",
    "category": "spec_conflict",
    "parse_error": false
  }
}
```

`task.review_cycle_exceeded`:
```json
{
  "type": "task.review_cycle_exceeded",
  "payload": {
    "task_id": "task-0003",
    "reviewer_run_id": "run-20260318-120600-L2",
    "review_cycle_count": 3,
    "max_review_cycles": 2
  }
}
```

## 9. 最大 review サイクル超過の処理

`review_cycle_count > effective_max_review_cycles` になった場合は以下の処理を行う。

ここで `effective_max_review_cycles` は `task.max_review_cycles ?? project.max_review_cycles` で解決する。

1. `latest_run_id` を reviewer run の id に設定する
2. `last_run_exit_reason` を reviewer run の `exit_reason` に設定する
3. `review_result` を `rework` に設定する
4. `reviewer_run_id` を reviewer run の id に設定する
5. `task.review_cycle_exceeded` event を記録する
6. task state を `blocked` に遷移する
7. `blocker_reason` を `"max review cycles exceeded ({N}/{M})"` に設定する
8. `blocker_category` を `"review_cycle_limit"` に設定する
9. alert を生成する

この状態から再開するには人間が手動で `status` を `todo` に戻し、`review_cycle_count` をリセットする必要がある。

stderr 例:
```text
ap start: task task-0003 exceeded max review cycles (3/2), marking blocked
```

## 10. alert 生成仕様

### 10.1 reviewer が blocked を返した場合の alert

```json
{
  "id": "alert-0003",
  "project": "autopilot",
  "task_id": "task-0003",
  "severity": "high",
  "type": "reviewer_blocked",
  "message": "reviewer blocked task-0003: 仕様が曖昧で判断できない",
  "reviewer_run_id": "run-20260318-120600-L2",
  "review_cycle": 1,
  "created_at": "2026-03-18T12:08:30+09:00",
  "status": "open"
}
```

### 10.2 最大 review サイクル超過の alert

```json
{
  "id": "alert-0004",
  "project": "autopilot",
  "task_id": "task-0003",
  "severity": "medium",
  "type": "review_cycle_exceeded",
  "message": "task task-0003 exceeded max review cycles (3/2)",
  "reviewer_run_id": "run-20260318-120600-L2",
  "review_cycle_count": 3,
  "max_review_cycles": 2,
  "created_at": "2026-03-18T12:10:00+09:00",
  "status": "open"
}
```

### 10.3 reviewer parse_error の alert

```json
{
  "id": "alert-0005",
  "project": "autopilot",
  "task_id": "task-0003",
  "severity": "medium",
  "type": "reviewer_parse_error",
  "message": "reviewer output could not be parsed for task task-0003",
  "reviewer_run_id": "run-20260318-120600-L2",
  "created_at": "2026-03-18T12:08:30+09:00",
  "status": "open"
}
```

### 10.4 reviewer_error の alert

```json
{
  "id": "alert-0006",
  "project": "autopilot",
  "task_id": "task-0003",
  "severity": "medium",
  "type": "reviewer_error",
  "message": "reviewer failed for task task-0003: timed out after 1800 seconds",
  "reviewer_run_id": "run-20260318-120600-L2",
  "created_at": "2026-03-18T12:08:30+09:00",
  "status": "open"
}
```

spawn 前失敗でも reviewer run は publish 済みなので、alert は `reviewer_run_id` を持ってよい。

## 11. `ap start` のフロー変更

Phase 6 での `ap start --review` の実行フローを示す。

```
1.  project 読み込み
2.  TODO.md と task state を同期（Phase 4 継続）
3.  dependency 検証（Phase 4 継続）
4.  interrupted run 検出と回復（Phase 5 継続）
5.  runnable task を選択（retry 上限チェックを含む、Phase 5 継続）
6.  project lock を取得（Phase 5 継続）
7.  task lock を取得（Phase 5 継続）
8.  run id を採番し、task state に記録（role: coder）
9.  task status を in_progress に更新
10. task.selected / run.started event を記録
11. coder エージェントを spawn
12. timeout タイマーを開始
13. coder 完了を待つ
14. coder の exit_reason を決定
15. coder run meta に結果を保存
16. exit_reason != "done" または review 無効の場合
    → task status を更新（done / failed / blocked）
    → done の場合のみ TODO.md を `[x]` に更新
    → result.final event を記録
    → task lock を解放
    → project lock を解放
    → last_run.json を更新
    → 終了
17. exit_reason == "done" かつ review が有効の場合
    → reviewer run id を採番
    → reviewer プロンプトを生成し prompt.txt を準備する
    → reviewer run meta を `status: "starting"` で保存する
    → task status を review_pending に更新
    → TODO.md は未完了のまま維持
    → task.latest_run_id / reviewer_run_id を reviewer run id に更新する
    → project.active_run_id を reviewer run id に更新する
    → task.status_changed event を記録
    → reviewer エージェントを spawn
    → spawn 成功時に reviewer run を `running` に更新する
       - meta.json を `status: "running"` で保存
       - review.started event を記録
    → spawn 前または spawn 中に失敗した場合は reviewer_error として処理
       - task status を `blocked` に更新
       - review_result を `reviewer_error` に設定
       - latest_run_id / reviewer_run_id は reviewer run を指してよい
       - reviewer run meta を `status: "finished"` / `exit_reason: "spawn_failed"` に更新する
       - project.active_run_id は `null` に戻す
       - project.last_run_id / last_run_at を reviewer run に更新する
       - alert を生成
       - lock を解放して終了
    → reviewer 完了を待つ
    → reviewer timeout / 非ゼロ exit の場合は reviewer_error として処理
    → それ以外は reviewer stdout を正規化して verdict を抽出
    → verdict に応じた処理（approve / rework / blocked / parse_error / reviewer_error）
    → reviewer run meta に結果を保存
    → task state を更新
    → project.active_run_id を `null` に戻し、project.last_run_id / last_run_at を reviewer run に更新
    → 対応 event を記録
    → 必要に応じて alert を生成
    → task lock を解放
    → project lock を解放
    → last_run.json を更新
```

### 11.1 reviewer 起動時の lock 継続

reviewer は coder の task lock が解放された後に新たな lock を取得するのではなく、**coder の task lock を継続保持した状態で起動する**。

これにより、coder → reviewer の間に別プロセスが task lock を取得することを防ぐ。

project lock も継続保持する。

reviewer 完了後に両方の lock を解放する。

publish 前失敗でも lock は必ず解放する。

### 11.2 interrupted review run の回復

Phase 5 の interrupted run 回復を reviewer にも拡張する。

次のいずれかを満たす場合、interrupted review とみなす。

- task state が `review_pending` であり、`latest_run_id` または `project.active_run_id` が reviewer run を指していて、対応する reviewer run の `meta.json` がない、または `status: "starting"` / `status: "running"` のまま残っている
- publish 済み reviewer run の `meta.json` が `status: "starting"` / `status: "running"` で存在するが、task/project state の reviewer run への切り替えが未完了である

上記に加えて、lock が存在しない、または reviewer / `ap start` 親プロセスが生きていないことを確認する。

この場合、次回 `ap start` では interrupted review とみなし、以下を行う。

1. `run.interrupted` event を記録する
2. task state の `status` を `blocked` に更新する
3. `review_result` を `reviewer_error` に設定する
4. `last_error` に `"previous reviewer run did not finish cleanly"` を記録する
5. `blocker_reason` を `"previous reviewer run did not finish cleanly"` に設定する
6. `blocker_category` を `"reviewer_error"` に設定する
7. `latest_run_id` を interrupted reviewer run の id に更新する
8. `last_run_exit_reason` を `"internal_error"` に更新する
9. `reviewer_run_id` を interrupted reviewer run の id に更新する
10. `project.active_run_id` を `null` に戻す
11. `project.last_run_id` / `project.last_run_at` を interrupted reviewer run に更新する
12. reviewer run が存在し、まだ `finished` でなければ `status: "finished"` / `exit_reason: "internal_error"` で終端させる
13. reviewer run が存在する場合はその run を interrupted として扱う
14. alert を生成して人間に通知できるようにする
15. 通常の runnable 判定へ進む（当該 task は `blocked` のため自動再選択されない）

これにより、`review_pending` のまま永久に詰まる状態を避けつつ、reviewer だけが壊れたケースで coder を同じ worktree に再投入して変更を二重適用する事故を防ぐ。

## 12. `project.json` の拡張

Phase 6 では `project.json` に以下を追加する。

```json
{
  "project": "autopilot",
  "status": "active",
  "active_task_id": null,
  "active_run_id": null,
  "last_run_id": "run-20260318-120600-L2",
  "last_run_at": "2026-03-18T12:08:30+09:00",
  "run_counter": 2,
  "default_timeout_seconds": 1800,
  "review_enabled": false,
  "max_review_cycles": 2,
  "task_counts": {
    "todo": 1,
    "in_progress": 0,
    "review_pending": 0,
    "blocked": 0,
    "done": 5,
    "failed": 0,
    "cancelled": 0
  },
  "updated_at": "2026-03-18T12:08:30+09:00"
}
```

追加フィールド:

| フィールド | 型 | 既定値 | 説明 |
|---|---|---|---|
| `review_enabled` | bool | `false` | project 全体で review フローを有効化するか |
| `max_review_cycles` | int | `2` | project 既定の最大 review サイクル数 |

task 個別の `max_review_cycles` が `null` または未設定の場合は `project.json` の値を使う。

## 13. `ap start` の CLI 変更

### 13.1 新規フラグ

```
ap start [project_name] [--review] [--no-review] [--timeout <seconds>]
```

| フラグ | 説明 |
|---|---|
| `--review` | review フローを有効化（project 設定を上書き） |
| `--no-review` | review フローを無効化（project 設定を上書き） |

`--review` と `--no-review` が同時に指定された場合は `--review` が優先される。

### 13.2 フラグの優先順位まとめ

```
task.skip_review > task.review_required > --review > --no-review > project.review_enabled > デフォルト（false）
```

## 14. 推奨実装単位

責務は以下に分けることを推奨する。

- `src/agents/reviewer_launcher.cpp`（または `agent_launcher.cpp` の拡張）
  - reviewer 用 agent 起動（`reviewer.claude` として識別）
  - reviewer プロンプト生成
  - reviewer stdout からの verdict 抽出
- `src/runtime/review_processor.cpp`
  - verdict に応じた task state 更新
  - `review_feedback` の保存
  - alert 生成（reviewer blocked / parse_error / reviewer_error / cycle exceeded）
- `src/commands/cmd_start.cpp`
  - `--review` / `--no-review` フラグ解析
  - coder 完了後の review フロー呼び出し
  - reviewer run id 採番・meta 保存
- `src/runtime/event_log.cpp`
  - `review.*` event の追記
  - `task.review_cycle_exceeded` event の追記

## 15. テスト観点

少なくとも以下をカバーする。

- `--review` フラグなし、かつ `project.review_enabled != true`、かつ `task.review_required != true` の場合は reviewer が起動しない
- `--review` フラグ指定時、coder が `done` で終わると reviewer が起動する
- coder が `failed` で終わると `--review` 指定でも reviewer が起動しない
- review 有効時、coder が `done` で終わっても `approve` までは `TODO.md` が `[x]` に更新されない
- reviewer が `approve` を返すと task が `done` になる
- reviewer が `approve` を返すと `TODO.md` が `[x]` に更新される
- reviewer が `rework` を返すと task が `todo` に戻る
- reviewer が `rework` を返すと `review_cycle_count` がインクリメントされる
- reviewer run が publish された後、task の `latest_run_id` が reviewer run を指す
- reviewer run が publish された後、task の `last_run_exit_reason` が reviewer run の `exit_reason` を持つ
- `review_cycle_count > effective_max_review_cycles` になると task が `blocked` になり alert が生成される
- reviewer が `blocked` を返すと task が `blocked` になり alert が生成される
- reviewer の stdout が verdict JSON を含まない場合、正規化後テキストに対して `parse_error` として `blocked` に遷移する
- reviewer が `starting` または `running` のまま中断した場合、または review setup 中に落ちた場合、次回 `ap start` で task が `blocked` になり coder は自動再実行されない
- reviewer `meta.json` 保存後、task/project state を reviewer run へ切り替える前に落ちた場合でも、次回 `ap start` でその orphaned reviewer run が検出され、task の `latest_run_id` / `last_run_exit_reason` / `reviewer_run_id` が reviewer run を指す形で `blocked` に回復される
- reviewer の spawn 前後で失敗した場合、task が `blocked` になり alert が生成され、`latest_run_id` は reviewer run を指してよい
- reviewer の spawn 前後で失敗した場合、reviewer run の meta.json が `finished` / `exit_reason: "spawn_failed"` で終端される
- reviewer run の meta.json に `role: "reviewer"` が記録される
- reviewer run の meta.json に `coder_run_id` が記録される
- reviewer run の meta.json が `starting` → `running` → `finished` と遷移する
- coder run の meta.json に `role: "coder"` が記録される
- `review.started` / `review.approved` / `review.rework_requested` / `review.blocked` event が正しく記録される
- `task.status_changed` で `review_pending` への遷移が記録される
- rework 後の task が次回 `ap start` で runnable として選択される
- `task.skip_review == true` の task では `--review` 指定でも reviewer が起動しない
- `review_enabled: true` の project では `--review` フラグなしでも reviewer が起動する
- reviewer 実行中に project lock と task lock が継続保持されている
- reviewer が timeout / 非ゼロ exit で失敗した場合、task が `blocked` になり alert が生成される

## 16. 互換性と移行

### 16.1 Phase 5 からの移行

Phase 5 の task state を Phase 6 で読む場合、以下の default を適用する。

- `review_required = false`
- `skip_review = false`
- `review_result = null`
- `reviewer_run_id = null`
- `review_cycle_count = 0`（rework 回数）
- `max_review_cycles = null`（project 既定値へフォールバック）
- `review_feedback = null`

### 16.2 互換性の原則

- review 関連の設定（`--review` / `project.review_enabled` / `task.review_required`）を使わない限り既存の動作は変わらない
- 既存の `events.jsonl`, `runs/`, `state/` の書き換えは不要
- `project.json` に `review_enabled` が存在しない場合は `false` として扱う
- 既存の run meta に `role` フィールドがない場合は `coder` として扱う
- Phase 5 文書中の `run_counter` を lock 番号と説明している箇所は、Phase 6 以降は project 全体の run 連番として読み替える

## 17. エラー仕様

Phase 6 では以下が新しい主要エラーになる。

```text
ap start: reviewer verdict could not be parsed for task task-0003, marking blocked
ap start: task task-0003 exceeded max review cycles (3/2), marking blocked
ap start: reviewer returned blocked for task task-0003: 仕様が曖昧で判断できない
ap start: reviewer returned rework for task task-0003 (cycle 1/2)
ap start: reviewer approved task task-0003 (cycle 1)
ap start: reviewer failed for task task-0003: timed out after 1800 seconds
```

これらはすべて `stderr` に出力する。

終了コード:

- `approve` で終わった場合: `0`
- `rework` で task が `todo` に戻った場合: `1`
- `blocked` / `parse_error` / `reviewer_error` で終わった場合: `1`

`0` は「task が完了した」場合に限定する。`rework` は自動化上は未完了であり、呼び出し側が成功扱いしないよう非 `0` とする。

## 18. Phase 7 以降への引き継ぎ

Phase 6 が完了すると、後続 Phase は以下のようにつなげやすくなる。

- **Phase 7 (task 自動生成)**
  - `review.rework_requested` の `issues` と `suggestions` を follow-up task 生成の根拠として使える
  - `review_cycle_count == effective_max_review_cycles` 到達時に「仕様整理タスク」を自動生成できる
  - reviewer blocked の `category` に応じて種別の異なる follow-up task を生成できる
- **Phase 8 (alert / briefing)**
  - `review_pending` タスクの一覧を briefing に組み込める
  - reviewer blocked / parse_error の alert を briefing で集約できる
  - `review_cycle_count` が高いタスクを「要注意」として briefing に表示できる

## 19. 未解決事項

Phase 6 着手の blocker ではないが、実装前に意識しておくべき点がある。

- reviewer プロンプトに git diff を含めるかどうか
  - 推奨: Phase 6 では coder stdout のみ渡す。git diff の取得は Phase 7 以降で対応する
- reviewer のタイムアウト値を coder と別に設定できるか
  - 推奨: `default_timeout_seconds` を共有し、`--timeout` フラグで上書き可能にする。reviewer 専用 timeout は後回し
- rework 時に coder への再指示プロンプトに `review_feedback` を自動的に含めるか
  - 推奨: Phase 6 では `review_feedback` を task state に保存するだけにとどめる。coder への自動フィードバック組み込みは Phase 7 以降で対応する
- reviewer が複数回 `review.comment` event を生成するか（中間出力）
  - 推奨: Phase 6 では `review.comment` は optional とし、未実装でよい。最終 verdict の記録を優先する
- `review_feedback` の履歴を全サイクル分保持するか
  - 推奨: `review_feedback` は直近のもののみ保持し、全履歴は `events.jsonl` から復元できる設計とする

この方針であれば、Phase 5 までの lock / retry / run 管理の土台を維持しつつ、`autopilot` を「coder と reviewer が分離した品質確認フロー付き runner」へ一段進められる。
