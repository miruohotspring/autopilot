# Phase 3 Spec: event モデルの拡張導入

## 1. 位置づけ

`docs/PLAN.md` の Phase 3 を、実装に着手できる粒度まで具体化した仕様書である。

前提として、Phase 2 で以下は導入済みとみなす。

- `runtime/state/project.json`
- `runtime/state/tasks/*.json`
- `runtime/events/events.jsonl`
- `task.selected` / `task.status_changed` / `run.started` / `run.finished` などの最小 event

Phase 3 の役割は、それらを捨てて作り直すことではない。役割は以下である。

- event を「最低限ある」状態から「履歴として十分に使える」状態へ拡張する
- run 中の出力、最終結果、blocker、alert を event として残せるようにする
- `failed` と `blocked` を運用上区別できるようにする
- state / run artifact / event の責務分担を明確にする

一方で、retry/lock/reviewer/task 自動生成/briefing 統合はまだこの Phase では扱わない。

## 2. ゴールと非ゴール

### 2.1 ゴール

- `runtime/events/events.jsonl` に、1 回の `ap start` の重要な流れを時系列で残せる
- Phase 2 の event type を維持したまま、以下を追加できる
  - `run.stdout`
  - `run.stderr`
  - `result.partial`
  - `result.final`
  - `task.blocked`
  - `alert.created`
- task state が `done` / `failed` / `blocked` を区別できる
- 人間判断待ちや外部要因待ちの停止を、`blocked` として記録できる
- 必要時に `runtime/alerts/*.json` を作り、将来の briefing から拾える形にできる
- 正常終了した run では、state と event が論理的に矛盾しない

### 2.2 非ゴール

- lock 導入
- retry policy 導入
- timeout 管理
- reviewer event の導入
- event から state を完全再構築する仕組み
- event log の rotation / compaction
- `ap alerts` や `ap runs` などの新規参照コマンド
- `dashboard.md` 自動更新
- child process の byte 単位完全再生

## 3. 基本方針

Phase 3 では、3 つの保存先の役割を以下のように固定する。

- state: 現在値
- event: append-only の履歴
- run artifact: 生ログと実行成果物

より具体的には以下である。

- `runtime/state/*` は「今どうなっているか」を持つ
- `runtime/events/events.jsonl` は「何が起きたか」を時系列で持つ
- `runtime/runs/*` は stdout/stderr/prompt/result などの原本を持つ

重要なのは、Phase 3 でも state を捨てて event sourcing に振り切らないことだ。

- 現在値の参照は引き続き state が主
- event は監査・調査・briefing 用の履歴
- run artifact は詳細確認用

## 4. runtime ディレクトリ構成

Phase 3 の推奨構成は以下。

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

`alerts/` は Phase 3 から導入するが、必要になるまで作成しなくてよい。

## 5. event 共通仕様

### 5.1 保存先

event log は引き続き以下に保存する。

```text
runtime/events/events.jsonl
```

1 event = 1 行の JSON とし、追記のみを行う。

### 5.2 共通フィールド

各 event は最低限以下を持つ。

```json
{
  "id": "evt-20260314-120001-0001",
  "timestamp": "2026-03-14T12:00:01+09:00",
  "project": "demo",
  "task_id": "task-0001",
  "run_id": "run-20260314-120000-L3",
  "type": "run.started",
  "actor": "ap.start",
  "payload": {
    "agent": "claude"
  }
}
```

### 5.3 各フィールドの意味

- `id`: project 内で実用上ユニークな event id
- `timestamp`: event が記録された時刻
- `project`: project 名
- `task_id`: 関連 task。task 非依存 event をまだ作らないので通常は必須
- `run_id`: 関連 run。run 開始前の event でも今回の実行に紐づくなら埋める
- `type`: event type
- `actor`: event を発生させた主体
- `payload`: type ごとの詳細情報

### 5.4 event id

推奨形式:

```text
evt-YYYYMMDD-HHMMSS-SSSS
```

末尾 `SSSS` は同一プロセス内の連番でよい。

### 5.5 event の順序

Phase 3 では以下を仕様とする。

- `events.jsonl` 上の行順が正とみなす
- `timestamp` は人間可読・外部参照用であり、完全な総順序は保証しない
- 同一 run 内の stream event は payload 内の連番で順序を補助する

## 6. actor 命名方針

actor は自由文字列ではなく、最低限以下の慣習に寄せる。

- `ap.start`
  - task 選択、state 遷移、run 開始/終了、alert 作成など orchestration 由来
- `agent.claude`
  - claude 実行由来の stdout/stderr/partial result
- `agent.codex`
  - codex 実行由来の stdout/stderr/partial result
- `runtime.classifier`
  - blocker 判定や final result 解釈のような内部分類処理

この命名で、将来 reviewer や manager を追加しても拡張しやすい。

## 7. event type 一覧

Phase 3 では Phase 2 の event type を残したまま、以下を扱う。

### 7.1 Phase 2 から継続する event

- `task.discovered`
- `task.selected`
- `task.status_changed`
- `run.started`
- `run.finished`
- `todo.sync_conflict`

### 7.2 Phase 3 で追加する event

- `run.stdout`
- `run.stderr`
- `result.partial`
- `result.final`
- `task.blocked`
- `alert.created`

`run.finished` を `result.final` で置き換えない点が重要である。

- `run.finished`: child process が終わった事実
- `result.final`: その run をどう解釈し、task をどう終結させたか

## 8. 各 event type の仕様

### 8.1 `task.selected`

task を今回の実行対象に選んだ event。

payload 例:

```json
{
  "reason": "first_runnable_task",
  "previous_status": "todo",
  "source_line": 3
}
```

### 8.2 `task.status_changed`

task state の `status` が変わった event。

payload 例:

```json
{
  "from": "in_progress",
  "to": "done",
  "reason": "result_finalized"
}
```

Phase 3 では少なくとも以下の遷移を記録する。

- `todo -> in_progress`
- `failed -> in_progress`
- `in_progress -> done`
- `in_progress -> failed`
- `in_progress -> blocked`
- `blocked -> in_progress`

### 8.3 `run.started`

child process を起動した event。

payload 例:

```json
{
  "agent": "claude",
  "path_name": "main",
  "working_directory": "/abs/path/to/repo",
  "attempt_number": 2
}
```

### 8.4 `run.finished`

child process が終了した event。

payload 例:

```json
{
  "agent": "claude",
  "exit_code": 0,
  "duration_ms": 252000
}
```

`run.finished` は必ず 1 run につき 1 回だけ出す。

### 8.5 `run.stdout` / `run.stderr`

run の出力断片を event に写したもの。

payload 例:

```json
{
  "stream": "stdout",
  "sequence": 1,
  "text": "implemented first task\n",
  "truncated": false
}
```

```json
{
  "stream": "stderr",
  "sequence": 2,
  "text": "warning: TODO update skipped\n",
  "truncated": false
}
```

仕様は以下。

- `stream` は `stdout` または `stderr`
- `sequence` は stream ごとの 1 始まり連番
- `text` はその chunk の文字列
- `truncated` は chunk 生成時に切り詰めたかどうか
- 空 chunk は event にしない

### 8.6 `result.partial`

run 途中または run 中の出力解釈から得られた中間結果。

payload 例:

```json
{
  "status_hint": "in_progress",
  "summary": "implemented parser, now adding tests",
  "source": "stdout_marker"
}
```

この event は 0 回以上でよい。Phase 3 の最小実装では以下を許容する。

- 実装が中間要約を取れない run では出さない
- 将来の structured output や reviewer 導入に備えて type だけ先に固定する

### 8.7 `result.final`

run の最終的な解釈結果。

payload 例:

```json
{
  "final_task_status": "done",
  "process_exit_code": 0,
  "summary": "implemented ap start skeleton",
  "todo_update_applied": true,
  "alert_id": null
}
```

`final_task_status` は少なくとも以下を取る。

- `done`
- `failed`
- `blocked`

`result.final` は 1 run につき 1 回だけ出す。

### 8.8 `task.blocked`

task が blocker により停止したことを示す event。

payload 例:

```json
{
  "reason": "needs production DB migration approval",
  "category": "approval_required",
  "approval_required": true,
  "alert_id": "alert-0001"
}
```

`task.blocked` は `status_changed` の代替ではない。

- blocker の中身は `task.blocked`
- task status の変化は `task.status_changed`

### 8.9 `alert.created`

人間が後で拾うべき alert artifact を作った event。

payload 例:

```json
{
  "alert_id": "alert-0001",
  "severity": "high",
  "type": "approval_required",
  "message": "Database migration may affect production schema"
}
```

## 9. stream event の運用方針

Phase 3 では、raw log と event の関係を以下のように定義する。

- `stdout.log` / `stderr.log` が完全な原本
- `run.stdout` / `run.stderr` は event log 側の検索・要約用ミラー

そのため、この Phase では以下を許容する。

- child process 中に逐次 event 化できなくてもよい
- 実装上難しければ、run 終了後に log を読み返して chunk event を追記してよい

この設計を採る理由は以下。

- 既存の file-based wrapper を大きく壊さず導入できる
- tmux 実行の有無に関係なく同じ保存モデルを取りやすい
- log 原本と event 要約を分離できる

### 9.1 chunk サイズ

1 event あたりの `text` は長すぎないよう制限してよい。

推奨:

- 1 chunk あたり最大 4 KiB 程度
- それを超える場合は複数 event に分割

### 9.2 interleaving

`stdout` と `stderr` の厳密な交互順序は保証しない。

仕様としては以下で十分とする。

- 各 stream 内順序は `sequence` で追える
- stream 間の時系列は `timestamp` と file の並びで概ね把握できる

## 10. blocker 判定と alert 作成

### 10.1 `failed` と `blocked` の違い

Phase 3 では `PLAN.md` の方針どおり、以下を分ける。

- `failed`: 実行は終わったが、task を完了扱いにできない
- `blocked`: 外部要因・人間判断・認証情報不足などで先へ進めない

### 10.2 最小 blocker 判定

この Phase では blocker 判定を過剰に賢くしない。

最低限の判定源は以下。

- agent 出力に明示 marker がある
- wrapper が明確な停止理由を知っている

推奨 marker 例:

```text
AUTOPILOT_BLOCKED: needs API key from general
AUTOPILOT_APPROVAL_REQUIRED: production migration approval needed
```

仕様としては以下。

- blocker 理由が明示的に取れた場合だけ `blocked` にする
- それ以外の非ゼロ終了は `failed` に倒す
- `TODO.md` 更新競合は blocker ではなく `todo.sync_conflict`

### 10.3 alert を作る条件

Phase 3 では alert を濫発しない。

最低限、以下のときだけ `runtime/alerts/*.json` を作る。

- `approval_required`
- `human_decision_required`
- `credential_required`
- `dangerous_action_requested`

単なるテスト失敗や lint failure は alert ではなく `failed` に留める。

## 11. alert artifact 仕様

Phase 3 では以下の最小 schema を推奨する。

```json
{
  "id": "alert-0001",
  "project": "demo",
  "task_id": "task-0001",
  "run_id": "run-20260314-120000-L3",
  "severity": "high",
  "type": "approval_required",
  "message": "Database migration may affect production schema",
  "created_at": "2026-03-14T12:05:30+09:00",
  "status": "open"
}
```

### 11.1 file 命名

推奨:

```text
alert-0001.json
alert-0002.json
```

task id と同様に、欠番は再利用しない。

### 11.2 Phase 3 でまだやらないこと

- alert を close / acknowledge する CLI
- alert の自動解消
- `project.json` への `open_alert_count` 集約
- briefing への反映

## 12. state と run artifact の更新

### 12.1 task state

Phase 3 では Phase 2 の task schema を大きく広げず、最低限以下を変える。

- `status` に `blocked` が実際に現れるようにする
- `last_error` に failed / blocked の短い理由を保存してよい
- `attempt_count` と `latest_run_id` は引き続き run 開始時に更新する

`approval_required` や詳細な blocker metadata は、Phase 4 以降の拡張候補に留める。

### 12.2 `project.json`

`task_counts` は `blocked` を通常の集計対象に含める。

例:

```json
{
  "task_counts": {
    "todo": 1,
    "in_progress": 0,
    "done": 2,
    "failed": 1,
    "blocked": 1
  }
}
```

### 12.3 `result.json`

Phase 3 では run artifact の最終結果にも、process 終了と task 終了の区別を入れる。

推奨フィールド:

```json
{
  "run_id": "run-20260314-120000-L3",
  "task_id": "task-0001",
  "attempt_number": 2,
  "process_exit_code": 0,
  "process_status": "succeeded",
  "final_task_status": "blocked",
  "todo_update_applied": false,
  "summary_excerpt": "approval required before production migration",
  "blocker_reason": "needs production approval",
  "alert_id": "alert-0001"
}
```

既存の `status` フィールドを維持する場合でも、少なくとも `final_task_status` は追加した方がよい。

## 13. `ap start` の実行フロー変更

Phase 3 の `ap start` は、Phase 2 の流れをベースに以下を推奨する。

1. project 解決
2. managed path 解決
3. `runtime/events`, `runtime/state/tasks`, `runtime/runs` を確認
4. `TODO.md` と task state を同期
5. runnable task を 1 件選択
6. task を `in_progress` に更新し、`task.selected` と `task.status_changed` を残す
7. run directory と `meta.json` / `prompt.txt` を作成
8. `run.started` を残す
9. agent を実行し、stdout/stderr を file へ保存する
10. `run.stdout` / `run.stderr` を event log へ追記する
11. 必要なら `result.partial` を追記する
12. `run.finished` を追記する
13. 結果を `done` / `failed` / `blocked` に分類する
14. `blocked` の場合は `task.blocked` を追記し、必要なら alert file を作って `alert.created` を追記する
15. task state と `project.json` を更新する
16. `done` の場合のみ `TODO.md` を `[x]` に更新する
17. `task.status_changed` と `result.final` を追記する
18. `result.json` と `last_run.json` を保存して終了する

### 13.1 event の推奨順序

1 run 内では、最低限以下の順序を推奨する。

1. `task.selected`
2. `task.status_changed` (`todo/failed/blocked -> in_progress`)
3. `run.started`
4. `run.stdout` / `run.stderr`
5. `result.partial` (0 回以上)
6. `run.finished`
7. `task.blocked` (該当時のみ)
8. `alert.created` (該当時のみ)
9. `task.status_changed` (`in_progress -> done/failed/blocked`)
10. `result.final`

厳密な crash consistency までは保証しないが、正常終了した run ではこの順序を保つ。

## 14. エラー仕様

Phase 3 では event log の重要度が上がるため、以下を考慮する。

- `events.jsonl` への追記失敗
- `alerts/*.json` の作成失敗
- stream replay 中の log 読み取り失敗
- blocker 判定に必要な marker 解析失敗

stderr メッセージ例:

```text
ap start failed: failed to append event log
ap start failed: failed to create alert file
ap start failed: failed to replay run stdout events
```

細かい終了コード分離はまだ不要で、終了コードは引き続き `0` / `1` でよい。

## 15. 互換性と移行

Phase 3 では Phase 2 の event log との互換を壊さない。

仕様:

- 既存の event type はそのまま有効
- consumer は未知の event type を無視できる前提で実装する
- 古い `result.json` に `final_task_status` がなくても読めるようにする
- 過去 run への backfill は必須にしない

これにより、Phase 2 完了直後の project に対しても段階的導入しやすい。

## 16. 推奨実装単位

既存実装へ無理なく載せるなら、責務は以下に分けるのがよい。

- `src/commands/cmd_start.cpp`
  - 全体 orchestration
- `src/runtime/event_log.cpp`
  - JSONL append と stream event 書き出し
- `src/runtime/run_result_classifier.cpp`
  - failed / blocked / done の分類
- `src/runtime/alert_store.cpp`
  - alert id 採番と JSON 保存

`cmd_start.cpp` に全ロジックを寄せても動くが、Phase 4/5 を見据えると分離した方が安全である。

## 17. テスト観点

既存の `tests/test_ap_start.sh` を拡張する形で、少なくとも以下をカバーする。

- 成功 run で `events.jsonl` に `task.selected`, `run.started`, `run.finished`, `result.final` が残る
- `stdout.log` の内容が `run.stdout` event に写る
- `stderr.log` の内容が `run.stderr` event に写る
- `result.final.final_task_status` が `done` / `failed` / `blocked` を区別する
- blocker marker を出した fake agent で task state が `blocked` になる
- `blocked` 時に `TODO.md` は `[x]` に更新されない
- approval marker を出した fake agent で `alerts/alert-0001.json` が作られる
- `alert.created` event に `alert_id` が入る
- 同じ project で run を重ねても `events.jsonl` が append-only で伸びる
- stream event の `sequence` が各 stream で単調増加する
- `task_counts.blocked` が更新される

fake agent は既存の shell script を流用しつつ、以下のような出力を返せばよい。

```text
AUTOPILOT_BLOCKED: needs API key from general
AUTOPILOT_APPROVAL_REQUIRED: production migration approval needed
```

## 18. Phase 4 以降への引き継ぎ

Phase 3 が完了すると、後続 Phase は以下のようにつなげやすくなる。

- Phase 4
  - task state に `approval_required` や `depends_on` を持たせやすい
- Phase 5
  - `run.finished` / `result.final` / `task.blocked` を使って retry policy を入れやすい
- Phase 6
  - reviewer の出力を同じ event モデルへ追加しやすい
- Phase 8
  - `alert.created` と `alerts/*.json` を briefing の入力にできる

重要なのは、この Phase で以下が揃うことだ。

- 履歴としての event
- 現在値としての state
- 原本としての run artifact
- 人間介入の入口としての alert artifact

## 19. 未解決事項

Phase 3 着手の blocker ではないが、実装前に意識しておくべき点がある。

- `run.stdout` / `run.stderr` を逐次出すか、終了後 replay にするか
- blocker 判定を marker ベースに限定するか、exit code でも増やすか
- `events.jsonl` に schema version を入れるか
- `result.partial` をどの時点で出すか

現時点の推奨は以下。

- まずは run 終了後 replay でよい
- blocker 判定は explicit marker を優先する
- schema version はまだ入れなくてよい
- `result.partial` は 0 回でもよいが type は先に固定する

この方針なら、Phase 2 の最小 event 基盤を壊さずに、`autopilot` を「調査可能な履歴を持つ runner」へ一段進められる。
